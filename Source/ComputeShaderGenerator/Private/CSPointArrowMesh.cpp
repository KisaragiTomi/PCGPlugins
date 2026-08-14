#include "CSPointArrowMesh.h"

#include "CSMesh.h"
#include "CSMeshOps.h"

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHI.h"
#include "RHICommandList.h"

// -----------------------------------------------------------------------------
// 箭头展开 / indirect-args compute shaders（Shaders/Private/CSPointArrowMesh.usf）
// -----------------------------------------------------------------------------

class FCSPointArrowBuildCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCSPointArrowBuildCS);
	SHADER_USE_PARAMETER_STRUCT(FCSPointArrowBuildCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, InPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, InNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RWPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWTangents)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RWTexCoords)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWColors)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWIndices)
		SHADER_PARAMETER(uint32, MaxArrows)
		SHADER_PARAMETER(float, ArrowLength)
		SHADER_PARAMETER(float, ShaftRadius)
		SHADER_PARAMETER(float, HeadRadius)
		SHADER_PARAMETER(float, HeadFraction)
		SHADER_PARAMETER(uint32, ArrowColor)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FCSPointArrowArgsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCSPointArrowArgsCS);
	SHADER_USE_PARAMETER_STRUCT(FCSPointArrowArgsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWMeshCounters)
		SHADER_PARAMETER(uint32, MaxArrows)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCSPointArrowBuildCS, "/Plugin/PCGPlugins/Shaders/Private/CSPointArrowMesh.usf", "BuildPointArrowsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSPointArrowArgsCS, "/Plugin/PCGPlugins/Shaders/Private/CSPointArrowMesh.usf", "BuildPointArrowArgsCS", SF_Compute);

// -----------------------------------------------------------------------------
// 箭头几何作为 UCSMesh 算子
// -----------------------------------------------------------------------------

namespace
{
	/** 一个箭头的顶点数 / 三角数 / 索引数，必须与 CSPointArrowMesh.usf 中的常量一致。 */
	constexpr uint32 GVertsPerArrow = 13;
	constexpr uint32 GTrisPerArrow = 14;
	constexpr uint32 GIndicesPerArrow = GTrisPerArrow * 3;

	// 形状比例：柱身半边长与锥底半径都按总长派生，锥头占总长的固定比例。调用方只给一个长度就能
	// 得到形状合理的箭头 —— 这三个系数是形状定义的一部分，和消费它们的 shader 待在同一个文件里，
	// 而不是摊在接口上假装可调。
	constexpr float GShaftRadiusRatio = 0.06f;
	constexpr float GHeadRadiusRatio = 0.16f;
	constexpr float GHeadFraction = 0.35f;
}

bool BuildPointArrowGeometryIntoMesh(UCSMesh* Target, const FCSPointArrowBuildParams& Params)
{
	if (!Target || !Params.Source.IsValid()) return false;
	// 代理时代由 UCSDisplayComponent::CreateSceneProxy 做的兜底。两个 kernel 在 SM5 以下都不编译，
	// 照建只会留下一个已分配的空网格 —— 那比一次明确的失败更难查。
	if (GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5) return false;

	// 上限总是夹在源容量内：源 buffer 之外的槽位没有点可读，多分配出来的只会是退化几何。
	const uint32 SourceCapacity = uint32(FMath::Max(Params.Source.Capacity, 1));
	const uint32 RequestedArrows = Params.MaxArrows > 0
		? FMath::Min(uint32(Params.MaxArrows), SourceCapacity)
		: SourceCapacity;

	// 容量还要过 UCSMesh 的 int32 接口。索引数溢出成负数不会被显存预检拦下——它会被当成一个
	// "3 索引"的迷你分配放行，而展开 kernel 仍按 MaxArrows 写：一次静默的越界写，而不是一次失败。
	constexpr uint32 MaxArrowsPerAllocation = uint32(MAX_int32) / GIndicesPerArrow;
	const uint32 ArrowCapacity = FMath::Min(RequestedArrows, MaxArrowsPerAllocation);
	const uint32 VertexCapacity = ArrowCapacity * GVertsPerArrow;
	const uint32 IndexCapacity = ArrowCapacity * GIndicesPerArrow;

	// 与 road 同样是承重而非防御：展开 kernel 按传进去的 MaxArrows 夹自己的写入，而不是按网格
	// 实际拿到的容量。分配被显存预检拒掉时留下的是上一次（可能更小的）buffer，往里写就是越界。
	if (!Target->EnsureCapacitySync(int32(VertexCapacity), int32(IndexCapacity)))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[CSPointArrow] Arrow build skipped: capacity for %u arrows (%u vertices / %u indices) was refused."),
			ArrowCapacity, VertexCapacity, IndexCapacity);
		return false;
	}

	const float ArrowLength = FMath::Max(Params.ArrowLength, UE_KINDA_SMALL_NUMBER);
	const float ShaftRadius = ArrowLength * GShaftRadiusRatio;
	const float HeadRadius = ArrowLength * GHeadRadiusRatio;

	// 箭头沿法线伸出一整个总长，包围盒必须按此外扩，否则会被过早剔除。源点集没有有效包围盒时
	// （画的全是 GPU 侧追加的点，CPU 一个都不知道）退回一个固定小盒，与代理路径一致。
	const FBox WorldBounds = Params.Source.WorldBounds.IsValid
		? Params.Source.WorldBounds.ExpandBy(ArrowLength)
		: FBox(FVector(-100.0), FVector(100.0));

	const uint32 PackedColor = Params.ArrowColor.ToFColor(/*bSRGB*/ true).ToPackedABGR();

	// 在 edit 内部置位，报告的是"pass 有没有真的加进去"，而不是"edit 有没有跑过"。
	// EditMeshSync 的 flush 就是把这个调用栈局部变量交给渲染线程写的那道 fence。
	bool bBuilt = false;
	Target->EditMeshSync([&Params, &WorldBounds, &bBuilt, ArrowCapacity, ArrowLength, ShaftRadius, HeadRadius, PackedColor]
		(FCSMeshEditContext& Context)
	{
		FRDGBufferRef Positions = Context.Positions();
		FRDGBufferRef Tangents = Context.Tangents();
		FRDGBufferRef TexCoords = Context.TexCoords();
		FRDGBufferRef Colors = Context.Colors();
		FRDGBufferRef Indices = Context.Indices();
		FRDGBufferRef IndirectArgs = Context.IndirectArgs();
		FRDGBufferRef MeshCounters = Context.Counters();
		if (!Positions || !Tangents || !TexCoords || !Colors) return;
		if (!Indices || !IndirectArgs || !MeshCounters) return;

		FRDGBuilder& GraphBuilder = Context.GraphBuilder;

		// 源三件套是本次 edit 的外来 buffer，注册进这张图归本算子管；常驻流已经由 Context 注册好，
		// 谁都不该再自己 RegisterExternalBuffer 一遍。
		FRDGBufferRef SrcPositions = GraphBuilder.RegisterExternalBuffer(Params.Source.Positions, TEXT("CSPointArrow.SrcPositions"));
		FRDGBufferRef SrcNormals = GraphBuilder.RegisterExternalBuffer(Params.Source.Normals, TEXT("CSPointArrow.SrcNormals"));
		FRDGBufferRef SrcCounter = GraphBuilder.RegisterExternalBuffer(Params.Source.Counter, TEXT("CSPointArrow.SrcCounter"));

		FRDGBufferSRVRef PositionsSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SrcPositions, PF_A32B32G32R32F));
		FRDGBufferSRVRef NormalsSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SrcNormals, PF_A32B32G32R32F));
		FRDGBufferSRVRef CounterSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SrcCounter, PF_R32_UINT));

		FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

		{
			FCSPointArrowBuildCS::FParameters* BuildParams = GraphBuilder.AllocParameters<FCSPointArrowBuildCS::FParameters>();
			BuildParams->InPositions = PositionsSRV;
			BuildParams->InNormals = NormalsSRV;
			BuildParams->InCounter = CounterSRV;
			BuildParams->RWPositions = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Positions, PF_R32_FLOAT));
			BuildParams->RWTangents = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Tangents, PF_R32_UINT));
			BuildParams->RWTexCoords = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TexCoords, PF_R32_FLOAT));
			BuildParams->RWColors = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Colors, PF_R32_UINT));
			BuildParams->RWIndices = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Indices, PF_R32_UINT));
			BuildParams->MaxArrows = ArrowCapacity;
			BuildParams->ArrowLength = ArrowLength;
			BuildParams->ShaftRadius = ShaftRadius;
			BuildParams->HeadRadius = HeadRadius;
			BuildParams->HeadFraction = GHeadFraction;
			BuildParams->ArrowColor = PackedColor;

			TShaderMapRef<FCSPointArrowBuildCS> BuildCS(ShaderMap);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSPointArrow.Expand"), BuildCS, BuildParams,
				FComputeShaderUtils::GetGroupCount(ArrowCapacity, 64));
		}
		{
			FCSPointArrowArgsCS::FParameters* ArgsParams = GraphBuilder.AllocParameters<FCSPointArrowArgsCS::FParameters>();
			ArgsParams->InCounter = CounterSRV;
			ArgsParams->RWIndirectArgs = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgs, PF_R32_UINT));
			ArgsParams->RWMeshCounters = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(MeshCounters, PF_R32_UINT));
			ArgsParams->MaxArrows = ArrowCapacity;

			TShaderMapRef<FCSPointArrowArgsCS> ArgsCS(ShaderMap);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSPointArrow.IndirectArgs"), ArgsCS, ArgsParams,
				FIntVector(1, 1, 1));
		}

		// 箭头只有一种材质，不写材质 id，但每个 UCSMesh 都带这条流，存盘时它被当成逐三角材质槽
		// 读回去。留着 buffer 池上一位租客写的东西，一个单材质的箭头网格会被存成几十个没有材质
		// 撑着的槽 —— 只有池子命中脏 buffer 时才复现，测试抓不到。0 是这里唯一的槽。
		FRDGBufferRef MaterialIds = Context.MaterialIds();
		if (MaterialIds) AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(MaterialIds, PF_R32_UINT)), 0u);

		// 上面那个 args kernel 只写 arg set 0，和共享计数 kernel 是一回事：这个网格上一辈子留下的
		// section 表会继续指向一套描述已经被替换掉的三角布局的 arg set。
		UCSMeshOps::InvalidateSections(Context);
		// 画几个箭头由 GPU 计数决定（InCounter[0]），游戏线程不能声称自己知道输出大小。
		Context.InvalidateKnownCounts();
		Context.Resident.WorldBounds = WorldBounds;
		bBuilt = true;

		// 这里没有 SetStandardStreamAccessFinal，也没有 GraphBuilder.Execute()：两件事都归
		// EditMeshSync。自己动手是那种没有症状、只表现为"某次改动之后它不画了"的错误。
	});
	if (!bBuilt) return false;

	// 常驻带来了代理路径没有的容量棘轮：以前每次重建都重新分配，现在只有 EnsureCapacitySync 会
	// 动容量，而它只涨不落。降低 MaxPointsToDraw、或换一批更少的点重画，都会让 buffer 停在这个
	// session 里出现过的最大尺寸上。按 CPU 侧的请求闸一下再调用，是因为箭头的实际条数由 GPU 计数
	// 决定，ShrinkCapacitySync 会先为此付一次计数回读（整条 GPU stall）才轮到它自己的迟滞判断。
	const bool bNeedsLessThanItHolds = int32(VertexCapacity) < Target->GetVertexCapacity()
		|| int32(IndexCapacity) < Target->GetIndexCapacity();
	if (bNeedsLessThanItHolds) Target->ShrinkCapacitySync(int32(VertexCapacity), int32(IndexCapacity));

	return true;
}
