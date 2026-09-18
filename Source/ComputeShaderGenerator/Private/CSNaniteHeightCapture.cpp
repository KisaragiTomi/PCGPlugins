#include "CSNaniteHeightCapture.h"

#include "ComputeShaderGenerateHelper.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "Math/OrthoMatrix.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "PrimitiveSceneProxy.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "Rendering/CustomRenderPass.h"
#include "SceneInterface.h"
#include "ShaderParameterStruct.h"
#include "TextureResource.h"
#include "UObject/ObjectKey.h"
#include "UObject/Package.h"

#include <atomic>

static TAutoConsoleVariable<int32> CVarCSNaniteHeightCaptureMaxTileSize(
	TEXT("r.CSNaniteHeightCapture.MaxTileSize"),
	1024,
	TEXT("Nanite 高度图捕获的单块边长上限（texel）。custom render pass 与渲染器共用 scene textures，")
	TEXT("编辑器里 scene texture 只长不缩：整张大图一次拍，之后整个会话的 GBuffer 都会按那个尺寸分配。"),
	ECVF_Default);

class FCSNaniteHeightMergeCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSNaniteHeightMergeCS);
	SHADER_USE_PARAMETER_STRUCT(FCSNaniteHeightMergeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, T_RenderedDepth)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_Heightmap)
		SHADER_PARAMETER(FIntPoint, TileOffset)
		SHADER_PARAMETER(FIntPoint, TileSize)
		SHADER_PARAMETER(FIntPoint, HeightmapSize)
		SHADER_PARAMETER(float, DepthOffset)
		SHADER_PARAMETER(float, EmptyDepth)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5()
};

IMPLEMENT_GLOBAL_SHADER(FCSNaniteHeightMergeCS, "/Plugin/PCGPlugins/Shaders/Private/CSNaniteHeightCapture.usf", "MergeRenderedDepthCS", SF_Compute);

namespace CSNaniteHeightCaptureDetail
{
	/** 近平面抬到最高点之上、远平面压到最低点之下的余量（cm）：贴着包围盒边界的面不被裁掉。 */
	constexpr double DepthMargin = 100.0;

	std::atomic<uint64> GMergedTileCount{0};

	/**
	 * 每个世界一个空拍的 SceneCapture：它的 CaptureScene() 立刻建出一个 renderer，顺带执行场景里
	 * 排队的 custom render pass。它自己的视图 4×4、ShowOnly 为空、深度源（渲染器走 DepthPrepassOnly），
	 * 几乎不花钱。故意不挂在任何 actor 上 —— 挂上去编辑器会给它生成摄像机模型和视锥线、出现在组件树里、
	 * 进撤销记录；无主组件由这里自己管生命周期：世界清理时注销并解除 root。
	 */
	TMap<TObjectKey<UWorld>, TWeakObjectPtr<USceneCaptureComponent2D>> GTriggers;
	FDelegateHandle GWorldCleanupHandle;
	FDelegateHandle GPreExitHandle;

	void ReleaseTrigger(USceneCaptureComponent2D* Trigger)
	{
		if (!Trigger) return;
		if (Trigger->IsRegistered()) Trigger->UnregisterComponent();
		Trigger->RemoveFromRoot();
	}

	void OnWorldCleanup(UWorld* World, bool /*bSessionEnded*/, bool /*bCleanupResources*/)
	{
		TWeakObjectPtr<USceneCaptureComponent2D> Trigger;
		if (GTriggers.RemoveAndCopyValue(World, Trigger)) ReleaseTrigger(Trigger.Get());
	}

	void OnEnginePreExit()
	{
		for (TPair<TObjectKey<UWorld>, TWeakObjectPtr<USceneCaptureComponent2D>>& Pair : GTriggers) ReleaseTrigger(Pair.Value.Get());
		GTriggers.Reset();
	}

	USceneCaptureComponent2D* GetTrigger(UWorld* World)
	{
		if (const TWeakObjectPtr<USceneCaptureComponent2D>* Found = GTriggers.Find(World))
		{
			USceneCaptureComponent2D* Existing = Found->Get();
			if (Existing && Existing->IsRegistered()) return Existing;
			ReleaseTrigger(Existing);
			GTriggers.Remove(World);
		}

		if (!GWorldCleanupHandle.IsValid()) GWorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddStatic(&OnWorldCleanup);
		if (!GPreExitHandle.IsValid()) GPreExitHandle = FCoreDelegates::OnEnginePreExit.AddStatic(&OnEnginePreExit);

		// CaptureScene 要求有 TextureTarget；它只接触发器自己那个什么都不画的视图。
		UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
		Target->RenderTargetFormat = RTF_R8;
		Target->ClearColor = FLinearColor::Black;
		Target->InitAutoFormat(4, 4);
		Target->UpdateResourceImmediate(true);

		USceneCaptureComponent2D* Trigger = NewObject<USceneCaptureComponent2D>(GetTransientPackage(), NAME_None, RF_Transient);
		Trigger->PrimaryComponentTick.bCanEverTick = false;
		Trigger->bCaptureEveryFrame = false;
		Trigger->bCaptureOnMovement = false;
		Trigger->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
		Trigger->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
		Trigger->TextureTarget = Target;
		Trigger->AddToRoot();
		Trigger->RegisterComponentWithWorld(World);
		if (!Trigger->IsRegistered())
		{
			ReleaseTrigger(Trigger);
			return nullptr;
		}

		GTriggers.Add(World, Trigger);
		return Trigger;
	}
}

namespace
{
/** 高度图的一块：只画深度。渲染器在 PreRender 与 PostRender 之间替我们把 SceneDepth 拷进
 *  RenderTargetTexture，PostRender 时在同一张 RDG 图里把这块并进高度图。 */
class FCSNaniteHeightCapturePass final : public FCustomRenderPassBase
{
public:
	IMPLEMENT_CUSTOM_RENDER_PASS(FCSNaniteHeightCapturePass);

	FCSNaniteHeightCapturePass(FTextureRenderTargetResource* InHeightmap, const FIntPoint& InHeightmapSize,
		const FIntPoint& InTileOffset, const FIntPoint& InTileSize, float InDepthOffset, float InEmptyDepth)
		: FCustomRenderPassBase(TEXT("CSNaniteHeightCapture"), ERenderMode::DepthPass, ERenderOutput::SceneDepth, InTileSize)
		, Heightmap(InHeightmap)
		, HeightmapSize(InHeightmapSize)
		, TileOffset(InTileOffset)
		, DepthOffset(InDepthOffset)
		, EmptyDepth(InEmptyDepth)
	{
	}

	virtual void OnPreRender(FRDGBuilder& GraphBuilder) override
	{
		const FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(RenderTargetSize, PF_R32_FLOAT, FClearValueBinding::Black,
			TexCreate_RenderTargetable | TexCreate_ShaderResource);
		RenderTargetTexture = GraphBuilder.CreateTexture(Desc, TEXT("CSNaniteHeightCapture.SceneDepth"));
	}

	virtual void OnPostRender(FRDGBuilder& GraphBuilder) override
	{
		FRHITexture* HeightmapRHI = Heightmap->GetRenderTargetTexture();
		if (!HeightmapRHI || !RenderTargetTexture) return;

		FRDGTextureRef HeightmapTexture = RegisterExternalTexture(GraphBuilder, HeightmapRHI, TEXT("CSNaniteHeightCapture.Heightmap"));

		FCSNaniteHeightMergeCS::FParameters* Parameters = GraphBuilder.AllocParameters<FCSNaniteHeightMergeCS::FParameters>();
		Parameters->T_RenderedDepth = RenderTargetTexture;
		Parameters->RW_Heightmap = GraphBuilder.CreateUAV(HeightmapTexture);
		Parameters->TileOffset = TileOffset;
		Parameters->TileSize = RenderTargetSize;
		Parameters->HeightmapSize = HeightmapSize;
		Parameters->DepthOffset = DepthOffset;
		Parameters->EmptyDepth = EmptyDepth;

		TShaderMapRef<FCSNaniteHeightMergeCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSNaniteHeightCapture.Merge"), ERDGPassFlags::Compute,
			ComputeShader, Parameters, FComputeShaderUtils::GetGroupCount(RenderTargetSize, 8));

		++CSNaniteHeightCaptureDetail::GMergedTileCount;
	}

private:
	FTextureRenderTargetResource* Heightmap;
	FIntPoint HeightmapSize;
	FIntPoint TileOffset;
	float DepthOffset;
	float EmptyDepth;
};
}

bool CSNaniteHeightCapture::IsAvailable(const UWorld* World)
{
	return World && World->Scene && FApp::CanEverRender() && !World->IsNetMode(NM_DedicatedServer);
}

bool CSNaniteHeightCapture::IsCapturableNaniteComponent(const UStaticMeshComponent* Component)
{
	if (!Component || !Component->SceneProxy || Component->bHiddenInGame || Component->bHiddenInSceneCapture) return false;

	// 渲染器在捕获视图里按 Game 规则判可见：actor 级的隐藏同样画不出来。
	const AActor* Owner = Component->GetOwner();
	if (Owner && Owner->IsHidden()) return false;

	// 以实际建出来的代理为准：材质不兼容、强制关 Nanite、平台不支持时代理是 fallback 网格，三角形路径读到的就是它。
	// 代理在帧末更新里构造（调用方先推过那一次），标志构造后不再变；SceneProxy 非空时代理一定还活着。
	return Component->SceneProxy->IsNaniteMesh();
}

bool CSNaniteHeightCapture::CaptureIntoHeightmap(UWorld* World, const FHeightmapRequest& Request)
{
	using namespace CSNaniteHeightCaptureDetail;

	if (!IsAvailable(World) || !Request.Heightmap || !Request.WorldBounds.IsValid) return false;

	TSet<FPrimitiveComponentId> ShowOnly;
	for (const TWeakObjectPtr<UPrimitiveComponent>& Component : Request.Components) if (Component.IsValid()) ShowOnly.Add(Component->GetPrimitiveSceneId());
	if (ShowOnly.IsEmpty()) return false;

	const FIntPoint HeightmapSize(Request.Heightmap->SizeX, Request.Heightmap->SizeY);
	const FVector BoundsSize = Request.WorldBounds.GetSize();
	FTextureRenderTargetResource* HeightmapResource = Request.Heightmap->GameThread_GetRenderTargetResource();
	if (!HeightmapResource || HeightmapSize.X <= 0 || HeightmapSize.Y <= 0 || BoundsSize.X <= UE_KINDA_SMALL_NUMBER || BoundsSize.Y <= UE_KINDA_SMALL_NUMBER) return false;

	USceneCaptureComponent2D* Trigger = GetTrigger(World);
	if (!Trigger || !Trigger->IsVisible()) return false;

	// 渲染相机抬到最高的东西之上：比 CameraHeight 还高的几何先照样拍到，合并时再按三角形路径的约定钳成 0。
	const double RenderCameraZ = FMath::Max<double>(Request.CameraHeight, Request.WorldBounds.Max.Z) + DepthMargin;
	const double FarDepth = RenderCameraZ - Request.WorldBounds.Min.Z + DepthMargin;
	const float DepthOffset = float(RenderCameraZ - Request.CameraHeight);
	// 没画到的像素反算出来就是远平面深度；门槛留半个余量，避开反算的浮点误差。
	const float EmptyDepth = float(FarDepth - 0.5 * DepthMargin);

	// 朝下看：画面右 = +X、画面下 = +Y，与高度图 texel (x, y) ↔ 世界 (X, Y) 同向。行列式为 +1，不翻绕序。
	const FMatrix ViewRotation(
		FPlane(1, 0, 0, 0),
		FPlane(0, -1, 0, 0),
		FPlane(0, 0, -1, 0),
		FPlane(0, 0, 0, 1));
	const FVector2D TexelSize(BoundsSize.X / HeightmapSize.X, BoundsSize.Y / HeightmapSize.Y);
	const FVector2D BoundsMin(Request.WorldBounds.Min.X, Request.WorldBounds.Min.Y);
	const int32 MaxTileSize = FMath::Max(16, CVarCSNaniteHeightCaptureMaxTileSize.GetValueOnGameThread());

	for (int32 TileY = 0; TileY < HeightmapSize.Y; TileY += MaxTileSize)
	{
		for (int32 TileX = 0; TileX < HeightmapSize.X; TileX += MaxTileSize)
		{
			const FIntPoint TileOffset(TileX, TileY);
			const FIntPoint TileSize(FMath::Min(MaxTileSize, HeightmapSize.X - TileX), FMath::Min(MaxTileSize, HeightmapSize.Y - TileY));

			// 正交投影平移不变：相机挪到块中心、视锥对称，块内像素中心逐一落在整张图的 texel 中心上。
			const FVector2D HalfExtent = FVector2D(TileSize) * TexelSize * 0.5;
			const FVector2D Centre = BoundsMin + FVector2D(TileOffset) * TexelSize + HalfExtent;

			FSceneInterface::FCustomRenderPassRendererInput PassInput;
			PassInput.ViewLocation = FVector(Centre.X, Centre.Y, RenderCameraZ);
			PassInput.ViewRotationMatrix = ViewRotation;
			PassInput.ProjectionMatrix = FReversedZOrthoMatrix(HalfExtent.X, HalfExtent.Y, 1.0 / FarDepth, 0.0);
			PassInput.ShowOnlyPrimitives = ShowOnly;
			PassInput.bIsSceneCapture = true;
			PassInput.CustomRenderPass = new FCSNaniteHeightCapturePass(HeightmapResource, HeightmapSize, TileOffset, TileSize, DepthOffset, EmptyDepth);
			World->Scene->AddCustomRenderPass(nullptr, PassInput);
		}
	}

	// 排队的 pass 由下一个渲染本场景的 renderer 执行：立刻建一个，别等帧末的视口。
	Trigger->CaptureScene();
	return true;
}

uint64 CSNaniteHeightCapture::DebugGetMergedTileCount()
{
	return CSNaniteHeightCaptureDetail::GMergedTileCount.load();
}
