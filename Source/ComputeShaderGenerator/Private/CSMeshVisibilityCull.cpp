#include "CSMeshVisibilityCull.h"

#include "CSMesh.h"
#include "CSMeshOps.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "HAL/PlatformTime.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGlobals.h"
#include "ShaderParameterStruct.h"

DEFINE_LOG_CATEGORY_STATIC(LogCSMeshVisibility, Log, All);

// 与 CSMeshVisibilityCull.usf 的 CSVC_GROUP_SIZE 一致。
static constexpr uint32 CSMeshVisibility_GroupSize = 64u;

// -----------------------------------------------------------------------------
// Shaders (Shaders/Private/CSMeshVisibilityCull.usf)
// -----------------------------------------------------------------------------

class FCSMeshVisibilityShader : public FGlobalShader
{
public:
	FCSMeshVisibilityShader() = default;
	FCSMeshVisibilityShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer) : FGlobalShader(Initializer) {}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FCSMeshVisibilityRasterCS : public FCSMeshVisibilityShader
{
	DECLARE_GLOBAL_SHADER(FCSMeshVisibilityRasterCS);
	SHADER_USE_PARAMETER_STRUCT(FCSMeshVisibilityRasterCS, FCSMeshVisibilityShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, CSVC_Positions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CSVC_Indices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, CSVC_ViewRows)
		SHADER_PARAMETER(FVector3f, CSVC_Center)
		SHADER_PARAMETER(uint32, CSVC_NumTriangles)
		SHADER_PARAMETER(uint32, CSVC_ViewBase)
		SHADER_PARAMETER(uint32, CSVC_NumViews)
		SHADER_PARAMETER(uint32, CSVC_Resolution)
		SHADER_PARAMETER(uint32, CSVC_Mode)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, CSVC_Depth)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, CSVC_Visible)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FCSMeshVisibilityRasterCS, "/Plugin/PCGPlugins/Shaders/Private/CSMeshVisibilityCull.usf", "RasterCS", SF_Compute);

class FCSMeshVisibilityMarkVerticesCS : public FCSMeshVisibilityShader
{
	DECLARE_GLOBAL_SHADER(FCSMeshVisibilityMarkVerticesCS);
	SHADER_USE_PARAMETER_STRUCT(FCSMeshVisibilityMarkVerticesCS, FCSMeshVisibilityShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CSVC_VisibleIn)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CSVC_TriIndices)
		SHADER_PARAMETER(uint32, CSVC_NumTris)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, CSVC_VertexMark)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FCSMeshVisibilityMarkVerticesCS, "/Plugin/PCGPlugins/Shaders/Private/CSMeshVisibilityCull.usf", "MarkVerticesCS", SF_Compute);

class FCSMeshVisibilityKeepCS : public FCSMeshVisibilityShader
{
	DECLARE_GLOBAL_SHADER(FCSMeshVisibilityKeepCS);
	SHADER_USE_PARAMETER_STRUCT(FCSMeshVisibilityKeepCS, FCSMeshVisibilityShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CSVC_VisibleIn)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CSVC_TriIndices)
		SHADER_PARAMETER(uint32, CSVC_NumTris)
		SHADER_PARAMETER(uint32, CSVC_KeepNeighbors)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, CSVC_VertexMark)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, CSVC_Keep)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FCSMeshVisibilityKeepCS, "/Plugin/PCGPlugins/Shaders/Private/CSMeshVisibilityCull.usf", "KeepCS", SF_Compute);

BEGIN_SHADER_PARAMETER_STRUCT(FCSMeshVisibilityCompactParameters, )
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CSVC_KeptTriangles)
	SHADER_PARAMETER(uint32, CSVC_NumKept)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CSVC_SrcIndices)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CSVC_SrcMaterialIds)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, CSVC_DstIndices)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, CSVC_DstMaterialIds)
END_SHADER_PARAMETER_STRUCT()

class FCSMeshVisibilityGatherCS : public FCSMeshVisibilityShader
{
	DECLARE_GLOBAL_SHADER(FCSMeshVisibilityGatherCS);
	SHADER_USE_PARAMETER_STRUCT(FCSMeshVisibilityGatherCS, FCSMeshVisibilityShader);
	using FParameters = FCSMeshVisibilityCompactParameters;
};
IMPLEMENT_GLOBAL_SHADER(FCSMeshVisibilityGatherCS, "/Plugin/PCGPlugins/Shaders/Private/CSMeshVisibilityCull.usf", "GatherCS", SF_Compute);

class FCSMeshVisibilityCopyBackCS : public FCSMeshVisibilityShader
{
	DECLARE_GLOBAL_SHADER(FCSMeshVisibilityCopyBackCS);
	SHADER_USE_PARAMETER_STRUCT(FCSMeshVisibilityCopyBackCS, FCSMeshVisibilityShader);
	using FParameters = FCSMeshVisibilityCompactParameters;
};
IMPLEMENT_GLOBAL_SHADER(FCSMeshVisibilityCopyBackCS, "/Plugin/PCGPlugins/Shaders/Private/CSMeshVisibilityCull.usf", "CopyBackCS", SF_Compute);

// -----------------------------------------------------------------------------
// Internals
// -----------------------------------------------------------------------------

namespace
{
/**
 * 一张视图的 4 行矩阵：(P - Center, 1) → clip。视点在 Dir × EyeDistance（相对包围球心），看向球心。
 * 透视的视角恰好框住包围球；正交框住球的横截面。深度都映射到 [0, 1]、随距离增大（近平面 = 距离 - 半径）。
 */
void CSMeshVisibility_AddView(TArray<FVector4f>& Rows, const FVector& Dir, double EyeDistance, double Radius, bool bOrthographic)
{
	const FVector Forward = -Dir;
	const FVector UpHint = FMath::Abs(Forward.Z) > 0.99 ? FVector::XAxisVector : FVector::ZAxisVector;
	const FVector Right = FVector::CrossProduct(UpHint, Forward).GetSafeNormal();
	const FVector Up = FVector::CrossProduct(Forward, Right);
	const FVector Eye = Dir * EyeDistance;
	const double Near = FMath::Max(EyeDistance - Radius, 1.0);
	const double Far = EyeDistance + Radius;
	const double EyeRight = FVector::DotProduct(Eye, Right);
	const double EyeUp = FVector::DotProduct(Eye, Up);
	const double EyeForward = FVector::DotProduct(Eye, Forward);
	auto Row = [](const FVector& V, double W) { return FVector4f(float(V.X), float(V.Y), float(V.Z), float(W)); };

	if (bOrthographic)
	{
		const double Scale = 1.0 / Radius;
		const double InvDepth = 1.0 / (Far - Near);
		Rows.Add(Row(Right * Scale, -EyeRight * Scale));
		Rows.Add(Row(Up * Scale, -EyeUp * Scale));
		Rows.Add(Row(Forward * InvDepth, (-EyeForward - Near) * InvDepth));
		Rows.Add(FVector4f(0.0f, 0.0f, 0.0f, 1.0f));
		return;
	}

	// 1 / tan(半视角)：半径 R 的球在距离 D 上的切线张角。
	const double Scale = FMath::Sqrt(FMath::Max(EyeDistance * EyeDistance - Radius * Radius, 1.0)) / Radius;
	const double A = Far / (Far - Near);
	const double B = -Far * Near / (Far - Near);
	Rows.Add(Row(Right * Scale, -EyeRight * Scale));
	Rows.Add(Row(Up * Scale, -EyeUp * Scale));
	Rows.Add(Row(Forward * A, -EyeForward * A + B));
	Rows.Add(Row(Forward, -EyeForward));
}

/** 仰角带内的斐波那契球面方向（按 sin(仰角) 均分 ⇒ 面积均匀），每个方向一张透视 + 可选一张正交。 */
TArray<FVector4f> CSMeshVisibility_MakeViews(const FCSMeshVisibilityCullOptions& Options, double EyeDistance, double Radius)
{
	const int32 NumDirections = FMath::Clamp(Options.NumDirections, 1, 4096);
	double ZMin = FMath::Sin(FMath::DegreesToRadians(FMath::Clamp<double>(Options.MinElevationDegrees, -90.0, 90.0)));
	double ZMax = FMath::Sin(FMath::DegreesToRadians(FMath::Clamp<double>(Options.MaxElevationDegrees, -90.0, 90.0)));
	if (ZMax < ZMin) Swap(ZMin, ZMax);
	const double GoldenAngle = UE_DOUBLE_PI * (3.0 - FMath::Sqrt(5.0));

	TArray<FVector4f> Rows;
	Rows.Reserve(NumDirections * (Options.bOrthographicViews ? 8 : 4));
	for (int32 Index = 0; Index < NumDirections; ++Index)
	{
		const double Z = ZMin + (ZMax - ZMin) * (double(Index) + 0.5) / double(NumDirections);
		const double Ring = FMath::Sqrt(FMath::Max(1.0 - Z * Z, 0.0));
		const double Phi = GoldenAngle * double(Index);
		const FVector Dir(Ring * FMath::Cos(Phi), Ring * FMath::Sin(Phi), Z);
		CSMeshVisibility_AddView(Rows, Dir, EyeDistance, Radius, false);
		if (Options.bOrthographicViews) CSMeshVisibility_AddView(Rows, Dir, Radius * 1.1 + 1.0, Radius, true);
	}
	return Rows;
}
}

// -----------------------------------------------------------------------------
// UCSMeshVisibilityOps
// -----------------------------------------------------------------------------

UCSMesh* UCSMeshVisibilityOps::CullHiddenTriangles(
	UCSMesh* Target, float ViewDistance, const FCSMeshVisibilityCullOptions& Options, FCSMeshVisibilityCullResult& OutResult)
{
	OutResult = FCSMeshVisibilityCullResult();
	if (!Target || !IsInGameThread()) return Target;
	if (GUsingNullRHI)
	{
		UE_LOG(LogCSMeshVisibility, Warning, TEXT("[CSMeshVisibility] Needs a real RHI; mesh left untouched."));
		return Target;
	}

	uint32 NumVertices = 0;
	uint32 NumIndices = 0;
	if (!Target->GetCountsSync(NumVertices, NumIndices) || NumIndices < 3u || NumVertices == 0u) return Target;
	const uint32 NumTriangles = NumIndices / 3u;
	OutResult.TrianglesBefore = OutResult.TrianglesVisible = OutResult.TrianglesAfter = int32(NumTriangles);

	const FBox Bounds = Target->GetWorldBoundsApprox();
	if (!Bounds.IsValid)
	{
		UE_LOG(LogCSMeshVisibility, Warning, TEXT("[CSMeshVisibility] Mesh has no world bounds; left untouched."));
		return Target;
	}
	const FVector Center = Bounds.GetCenter();
	const double Radius = FMath::Max(Bounds.GetExtent().Size(), 1.0);
	const double EyeDistance = FMath::Max<double>(ViewDistance, Radius * 1.5);

	const TArray<FVector4f> ViewRows = CSMeshVisibility_MakeViews(Options, EyeDistance, Radius);
	const uint32 NumViews = uint32(ViewRows.Num() / 4);
	OutResult.NumViews = int32(NumViews);
	if (NumViews == 0u) return Target;

	// 一批视图共用一块深度 buffer（上限 16M 个 uint = 64 MB）；每批的线程数 = 三角 × 视图，要装进 int32
	// （GetGroupCountWrapped 收的是 int32）。
	const uint32 Resolution = uint32(FMath::Clamp(Options.Resolution, 64, 4096));
	const uint64 PixelsPerView = uint64(Resolution) * uint64(Resolution);
	uint32 ViewsPerBatch = uint32(FMath::Clamp<uint64>((16ull << 20) / PixelsPerView, 1ull, uint64(NumViews)));
	ViewsPerBatch = FMath::Clamp(uint32(FMath::Min<uint64>(uint64(MAX_int32) / uint64(NumTriangles), uint64(ViewsPerBatch))), 1u, ViewsPerBatch);

	const double StartSeconds = FPlatformTime::Seconds();
	TRefCountPtr<FRDGPooledBuffer> PooledKeep;
	Target->EditMeshSync([&](FCSMeshEditContext& Context)
	{
		FRDGBufferRef Positions = Context.Positions();
		FRDGBufferRef Indices = Context.Indices();
		if (!Positions || !Indices) return;

		FRDGBuilder& GraphBuilder = Context.GraphBuilder;
		FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

		FRDGBufferRef Views = CreateStructuredBuffer(GraphBuilder, TEXT("CSMeshVisibility.Views"), ViewRows);
		FRDGBufferRef Depth = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), uint32(PixelsPerView * ViewsPerBatch)), TEXT("CSMeshVisibility.Depth"));
		FRDGBufferRef Visible = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumTriangles), TEXT("CSMeshVisibility.Visible"));
		FRDGBufferRef VertexMark = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumVertices), TEXT("CSMeshVisibility.VertexMark"));
		FRDGBufferRef Keep = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumTriangles), TEXT("CSMeshVisibility.Keep"));

		FRDGBufferUAVRef DepthUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Depth, PF_R32_UINT));
		FRDGBufferUAVRef VisibleUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Visible, PF_R32_UINT));
		FRDGBufferUAVRef VertexMarkUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(VertexMark, PF_R32_UINT));
		AddClearUAVPass(GraphBuilder, VisibleUAV, 0u);
		AddClearUAVPass(GraphBuilder, VertexMarkUAV, 0u);

		FRDGBufferSRVRef PositionsSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Positions, PF_R32_FLOAT));
		FRDGBufferSRVRef IndicesSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Indices, PF_R32_UINT));
		FRDGBufferSRVRef ViewsSRV = GraphBuilder.CreateSRV(Views);
		TShaderMapRef<FCSMeshVisibilityRasterCS> RasterShader(ShaderMap);

		for (uint32 ViewBase = 0; ViewBase < NumViews; ViewBase += ViewsPerBatch)
		{
			const uint32 BatchViews = FMath::Min(ViewsPerBatch, NumViews - ViewBase);
			AddClearUAVPass(GraphBuilder, DepthUAV, 0xFFFFFFFFu);
			for (uint32 Mode = 0; Mode < 2u; ++Mode)
			{
				FCSMeshVisibilityRasterCS::FParameters* Params = GraphBuilder.AllocParameters<FCSMeshVisibilityRasterCS::FParameters>();
				Params->CSVC_Positions = PositionsSRV;
				Params->CSVC_Indices = IndicesSRV;
				Params->CSVC_ViewRows = ViewsSRV;
				Params->CSVC_Center = FVector3f(Center);
				Params->CSVC_NumTriangles = NumTriangles;
				Params->CSVC_ViewBase = ViewBase;
				Params->CSVC_NumViews = BatchViews;
				Params->CSVC_Resolution = Resolution;
				Params->CSVC_Mode = Mode;
				Params->CSVC_Depth = DepthUAV;
				Params->CSVC_Visible = VisibleUAV;
				FComputeShaderUtils::AddPass(GraphBuilder,
					Mode == 0u ? RDG_EVENT_NAME("CSMeshVisibility.Depth") : RDG_EVENT_NAME("CSMeshVisibility.Mark"),
					RasterShader, Params, FComputeShaderUtils::GetGroupCountWrapped(NumTriangles * BatchViews, CSMeshVisibility_GroupSize));
			}
		}

		FRDGBufferSRVRef VisibleSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Visible, PF_R32_UINT));
		if (Options.bKeepNeighbors)
		{
			FCSMeshVisibilityMarkVerticesCS::FParameters* Params = GraphBuilder.AllocParameters<FCSMeshVisibilityMarkVerticesCS::FParameters>();
			Params->CSVC_VisibleIn = VisibleSRV;
			Params->CSVC_TriIndices = IndicesSRV;
			Params->CSVC_NumTris = NumTriangles;
			Params->CSVC_VertexMark = VertexMarkUAV;
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSMeshVisibility.MarkVertices"),
				TShaderMapRef<FCSMeshVisibilityMarkVerticesCS>(ShaderMap), Params,
				FComputeShaderUtils::GetGroupCountWrapped(NumTriangles, CSMeshVisibility_GroupSize));
		}
		{
			FCSMeshVisibilityKeepCS::FParameters* Params = GraphBuilder.AllocParameters<FCSMeshVisibilityKeepCS::FParameters>();
			Params->CSVC_VisibleIn = VisibleSRV;
			Params->CSVC_TriIndices = IndicesSRV;
			Params->CSVC_NumTris = NumTriangles;
			Params->CSVC_KeepNeighbors = Options.bKeepNeighbors ? 1u : 0u;
			Params->CSVC_VertexMark = VertexMarkUAV;
			Params->CSVC_Keep = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Keep, PF_R32_UINT));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSMeshVisibility.Keep"),
				TShaderMapRef<FCSMeshVisibilityKeepCS>(ShaderMap), Params,
				FComputeShaderUtils::GetGroupCountWrapped(NumTriangles, CSMeshVisibility_GroupSize));
		}
		GraphBuilder.QueueBufferExtraction(Keep, &PooledKeep);
	});

	TArray<uint32> KeepFlags;
	if (!PooledKeep.IsValid() || !CSMeshReadback::ReadUintBufferSync(PooledKeep, NumTriangles, ECSGpuStreamRole::AuxVertex, KeepFlags)
		|| KeepFlags.Num() < int32(NumTriangles))
	{
		UE_LOG(LogCSMeshVisibility, Warning, TEXT("[CSMeshVisibility] Visibility readback failed; mesh left untouched."));
		return Target;
	}

	TArray<uint32> Kept;
	Kept.Reserve(int32(NumTriangles));
	int32 NumVisible = 0;
	for (uint32 Tri = 0; Tri < NumTriangles; ++Tri)
	{
		if (KeepFlags[Tri] == 1u) ++NumVisible;
		if (KeepFlags[Tri] != 0u) Kept.Add(Tri);
	}
	OutResult.TrianglesVisible = NumVisible;

	// 一个三角都没看见只能是视图没对准（包围盒不对之类），不可能是真的：外表面总有人看得见。
	if (Kept.IsEmpty())
	{
		UE_LOG(LogCSMeshVisibility, Warning, TEXT("[CSMeshVisibility] No triangle seen by any of %u views; mesh left untouched."), NumViews);
		return Target;
	}

	bool bCompacted = Kept.Num() == int32(NumTriangles);
	if (!bCompacted)
	{
		const uint32 NumKept = uint32(Kept.Num());
		Target->EditMeshSync([&](FCSMeshEditContext& Context)
		{
			FRDGBufferRef Indices = Context.Indices();
			FRDGBufferRef MaterialIds = Context.MaterialIds();
			if (!Indices || !MaterialIds) return;

			FRDGBuilder& GraphBuilder = Context.GraphBuilder;
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
			FRDGBufferRef KeptBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("CSMeshVisibility.Kept"), Kept);
			FRDGBufferRef TempIndices = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumKept * 3u), TEXT("CSMeshVisibility.TempIndices"));
			FRDGBufferRef TempMaterialIds = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumKept), TEXT("CSMeshVisibility.TempMaterialIds"));
			const FIntVector Groups = FComputeShaderUtils::GetGroupCountWrapped(NumKept, CSMeshVisibility_GroupSize);

			FCSMeshVisibilityCompactParameters* Gather = GraphBuilder.AllocParameters<FCSMeshVisibilityCompactParameters>();
			Gather->CSVC_KeptTriangles = GraphBuilder.CreateSRV(KeptBuffer);
			Gather->CSVC_NumKept = NumKept;
			Gather->CSVC_SrcIndices = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Indices, PF_R32_UINT));
			Gather->CSVC_SrcMaterialIds = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(MaterialIds, PF_R32_UINT));
			Gather->CSVC_DstIndices = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TempIndices, PF_R32_UINT));
			Gather->CSVC_DstMaterialIds = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TempMaterialIds, PF_R32_UINT));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSMeshVisibility.Gather"),
				TShaderMapRef<FCSMeshVisibilityGatherCS>(ShaderMap), Gather, Groups);

			FCSMeshVisibilityCompactParameters* CopyBack = GraphBuilder.AllocParameters<FCSMeshVisibilityCompactParameters>();
			CopyBack->CSVC_KeptTriangles = Gather->CSVC_KeptTriangles;
			CopyBack->CSVC_NumKept = NumKept;
			CopyBack->CSVC_SrcIndices = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(TempIndices, PF_R32_UINT));
			CopyBack->CSVC_SrcMaterialIds = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(TempMaterialIds, PF_R32_UINT));
			CopyBack->CSVC_DstIndices = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Indices, PF_R32_UINT));
			CopyBack->CSVC_DstMaterialIds = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(MaterialIds, PF_R32_UINT));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSMeshVisibility.CopyBack"),
				TShaderMapRef<FCSMeshVisibilityCopyBackCS>(ShaderMap), CopyBack, Groups);

			// 顶点不动，三角数变了：计数与间接参数一起写，section 表随之作废。
			UCSMeshOps::AddSetCountersPass(Context, NumVertices, NumKept * 3u);
			bCompacted = true;
		});
	}

	OutResult.bApplied = bCompacted;
	if (bCompacted) OutResult.TrianglesAfter = Kept.Num();
	UE_LOG(LogCSMeshVisibility, Log,
		TEXT("[CSMeshVisibility] %u view(s) (%d direction(s), %u px, eye %.0f cm, radius %.0f cm): %u -> %d triangles (%d visible, %d kept as neighbours) in %.2f s."),
		NumViews, FMath::Clamp(Options.NumDirections, 1, 4096), Resolution, EyeDistance, Radius, NumTriangles, OutResult.TrianglesAfter,
		NumVisible, Kept.Num() - NumVisible, FPlatformTime::Seconds() - StartSeconds);
	if (!bCompacted) UE_LOG(LogCSMeshVisibility, Warning, TEXT("[CSMeshVisibility] Mesh has no index / material stream to compact; left untouched."));
	return Target;
}
