#include "CSDepthBrushSampleService.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "Engine/Engine.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "SceneTexturesConfig.h"
#include "SceneView.h"
#include "SceneViewExtension.h"
#include "ShaderParameterStruct.h"
#include "Async/Async.h"

namespace
{
	// 与 CSDepthPointBrush.usf 的 numthreads 一致；也是一次 stroke update 的候选数上限，
	// 因为间距剔除要求所有候选在同一个组里。
	constexpr int32 CSDepthBrushGroupSize = 256;

	// 一个请求最多被"这一帧渲染的不是它那个视口"跳过多少次。超过就丢弃：那时它依据的深度
	// 早就过期了，硬塞进去反而会把点摆错地方。
	constexpr int32 CSDepthBrushMaxSkips = 32;
}

// ─────────────────────────────────────────────────────────────────────────────
// 采样 compute shader
// ─────────────────────────────────────────────────────────────────────────────
class FCSDepthBrushSampleCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCSDepthBrushSampleCS);
	SHADER_USE_PARAMETER_STRUCT(FCSDepthBrushSampleCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RWPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RWNormals)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWCounter)
		SHADER_PARAMETER(FVector3f, TranslatedBrushCentre)
		SHADER_PARAMETER(FVector3f, BrushCentreWorld)
		SHADER_PARAMETER(FVector3f, PaintBoundsMin)
		SHADER_PARAMETER(FVector3f, PaintBoundsMax)
		SHADER_PARAMETER(float, BrushRadius)
		SHADER_PARAMETER(float, MinSpacingSq)
		SHADER_PARAMETER(uint32, SampleCount)
		SHADER_PARAMETER(uint32, PointCapacity)
		SHADER_PARAMETER(uint32, RandomSeed)
		SHADER_PARAMETER(uint32, bUsePaintBounds)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("CS_DEPTH_BRUSH_GROUP_SIZE"), CSDepthBrushGroupSize);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCSDepthBrushSampleCS, "/Plugin/PCGPlugins/Shaders/Private/CSDepthPointBrush.usf", "DepthBrushSampleCS", SF_Compute);

// ─────────────────────────────────────────────────────────────────────────────
// View extension：唯一能拿到写满的场景深度的地方。
// 选 PostRenderBasePassDeferred_RenderThread 是因为此刻深度预通道与 base pass 都已结束，
// SceneTextures 里的 SceneDepthTexture 就是这一帧可见表面的完整深度。
// ─────────────────────────────────────────────────────────────────────────────
class FCSDepthBrushViewExtension : public FSceneViewExtensionBase
{
public:
	FCSDepthBrushViewExtension(const FAutoRegister& AutoReg, FCSDepthBrushSampleService& InOwner)
		: FSceneViewExtensionBase(AutoReg)
		, Owner(InOwner)
	{
	}

	virtual void PostRenderBasePassDeferred_RenderThread(
		FRDGBuilder& GraphBuilder,
		FSceneView& InView,
		const FRenderTargetBindingSlots& RenderTargets,
		TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures) override;

private:
	// 请求是给某个视口的，而当帧先渲染的可能是别的视口，所以队列排空后要在渲染线程侧留一份
	// 待认领的清单，等它那个视口渲染时再消费。
	struct FPendingRequest
	{
		FCSDepthBrushSampleRequest Request;
		int32 SkippedCount = 0;
	};

	void ProcessSample(FRDGBuilder& GraphBuilder, const FSceneView& View, FRDGTextureRef SceneDepth,
		FCSDepthBrushSampleRequest&& Request);

	FCSDepthBrushSampleService& Owner;
	TArray<FPendingRequest> Pending; // 仅渲染线程访问
};

// ─────────────────────────────────────────────────────────────────────────────
// 单例 + 生命周期
// ─────────────────────────────────────────────────────────────────────────────
FCSDepthBrushSampleService& FCSDepthBrushSampleService::Get()
{
	static FCSDepthBrushSampleService Singleton;
	return Singleton;
}

void FCSDepthBrushSampleService::Startup()
{
	if (!ViewExtension.IsValid() && GEngine) ViewExtension = FSceneViewExtensions::NewExtension<FCSDepthBrushViewExtension>(*this);
}

void FCSDepthBrushSampleService::Shutdown()
{
	ViewExtension.Reset();
	// 清空残留请求，避免回调悬空。
	FCSDepthBrushSampleRequest Request;
	while (SampleQueue.Dequeue(Request)) {}
}

void FCSDepthBrushSampleService::EnqueueSample(FCSDepthBrushSampleRequest&& Request)
{
	if (Request.SampleCount <= 0 || !Request.Output.IsValid()) return;

	Startup();
	SampleQueue.Enqueue(MoveTemp(Request));
}

// ─────────────────────────────────────────────────────────────────────────────
// 渲染线程回调
// ─────────────────────────────────────────────────────────────────────────────
void FCSDepthBrushViewExtension::PostRenderBasePassDeferred_RenderThread(
	FRDGBuilder& GraphBuilder,
	FSceneView& InView,
	const FRenderTargetBindingSlots& /*RenderTargets*/,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures)
{
	// 只处理真 FViewInfo：手工构造的视图没有可用的场景纹理。
	if (!InView.bIsViewInfo) return;

	FCSDepthBrushSampleRequest Incoming;
	while (Owner.SampleQueue.Dequeue(Incoming)) Pending.AddDefaulted_GetRef().Request = MoveTemp(Incoming);
	if (Pending.IsEmpty()) return;

	FRDGTextureRef SceneDepth = SceneTextures ? SceneTextures->GetContents()->SceneDepthTexture : nullptr;

	// 保序处理：同一个视口的多次 stroke update 必须按顺序落图，间距剔除才看得到前一批的点。
	TArray<FPendingRequest> Remaining;
	Remaining.Reserve(Pending.Num());
	for (FPendingRequest& Entry : Pending)
	{
		const bool bMatchesView = (Entry.Request.ViewState == nullptr) || (Entry.Request.ViewState == InView.State);
		if (bMatchesView)
		{
			ProcessSample(GraphBuilder, InView, SceneDepth, MoveTemp(Entry.Request));
			continue;
		}

		if (++Entry.SkippedCount <= CSDepthBrushMaxSkips) Remaining.Add(MoveTemp(Entry));
	}
	Pending = MoveTemp(Remaining);
}

void FCSDepthBrushViewExtension::ProcessSample(FRDGBuilder& GraphBuilder, const FSceneView& View,
	FRDGTextureRef SceneDepth, FCSDepthBrushSampleRequest&& Request)
{
	// 深度可能还没就绪（例如视图刚建起来那一帧），此时跳过采样但仍然回调，让调用方别卡住。
	if (SceneDepth)
	{
		FRDGBufferRef Positions = GraphBuilder.RegisterExternalBuffer(Request.Output.Positions, TEXT("CSDepthBrush.Positions"));
		FRDGBufferRef Normals = GraphBuilder.RegisterExternalBuffer(Request.Output.Normals, TEXT("CSDepthBrush.Normals"));
		FRDGBufferRef Counter = GraphBuilder.RegisterExternalBuffer(Request.Output.Counter, TEXT("CSDepthBrush.Counter"));

		// 整条重建链都在 translated world 里做：位置由深度还原出来就是这个空间的，笔刷中心
		// 也换算过去，两者相减得到的 brush-local 偏移只有笔刷尺度，float 精度绰绰有余。
		const FVector TranslatedBrushCentre = Request.BrushCentre + View.ViewMatrices.GetPreViewTranslation();
		const FBox& PaintBounds = Request.PaintBounds;

		FCSDepthBrushSampleCS::FParameters* Params = GraphBuilder.AllocParameters<FCSDepthBrushSampleCS::FParameters>();
		Params->SceneDepthTexture = SceneDepth;
		Params->RWPositions = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Positions, PF_A32B32G32R32F));
		Params->RWNormals = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Normals, PF_A32B32G32R32F));
		Params->RWCounter = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Counter, PF_R32_UINT));
		Params->TranslatedBrushCentre = (FVector3f)TranslatedBrushCentre;
		Params->BrushCentreWorld = (FVector3f)Request.BrushCentre;
		Params->PaintBoundsMin = PaintBounds.IsValid ? (FVector3f)PaintBounds.Min : FVector3f::ZeroVector;
		Params->PaintBoundsMax = PaintBounds.IsValid ? (FVector3f)PaintBounds.Max : FVector3f::ZeroVector;
		Params->BrushRadius = FMath::Max(1.0f, Request.BrushRadius);
		Params->MinSpacingSq = FMath::Square(FMath::Max(0.0f, Request.MinSpacing));
		Params->SampleCount = (uint32)FMath::Clamp(Request.SampleCount, 1, CSDepthBrushGroupSize);
		Params->PointCapacity = (uint32)FMath::Max(1, Request.Output.Capacity);
		Params->RandomSeed = Request.RandomSeed;
		Params->bUsePaintBounds = PaintBounds.IsValid ? 1u : 0u;
		Params->View = View.ViewUniformBuffer;

		TShaderMapRef<FCSDepthBrushSampleCS> Shader(GetGlobalShaderMap(View.GetFeatureLevel()));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSDepthBrush.Sample"), Shader, Params, FIntVector(1, 1, 1));

		// 显示路径把同一组 buffer 注册进自己的图并当 SRV 读，尾态必须回到 SRV。
		GraphBuilder.SetBufferAccessFinal(Positions, ERHIAccess::SRVMask);
		GraphBuilder.SetBufferAccessFinal(Normals, ERHIAccess::SRVMask);
		GraphBuilder.SetBufferAccessFinal(Counter, ERHIAccess::SRVMask);
	}

	if (!Request.OnDispatched) return;

	// pass 已经录进这一帧的图，游戏线程此刻再去重建显示代理，命令一定排在它后面。
	AsyncTask(ENamedThreads::GameThread, [OnDispatched = MoveTemp(Request.OnDispatched)]()
	{
		OnDispatched();
	});
}
