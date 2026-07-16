#include "RoadBuilderShaders.h"

#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHICommandList.h"

#define ROAD_SHADER TEXT("/Plugin/PCGPlugins/Shaders/Private/RoadBuilder.usf")

IMPLEMENT_GLOBAL_SHADER(FRoadClearCS, "/Plugin/PCGPlugins/Shaders/Private/RoadBuilder.usf", "ClearCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRoadIntersectCS, "/Plugin/PCGPlugins/Shaders/Private/RoadBuilder.usf", "IntersectCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRoadClusterCS, "/Plugin/PCGPlugins/Shaders/Private/RoadBuilder.usf", "ClusterCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRoadCornerSolveCS, "/Plugin/PCGPlugins/Shaders/Private/RoadBuilder.usf", "CornerSolveCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRoadEmitRoadCS, "/Plugin/PCGPlugins/Shaders/Private/RoadBuilder.usf", "EmitRoadCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRoadEmitCornerCS, "/Plugin/PCGPlugins/Shaders/Private/RoadBuilder.usf", "EmitCornerCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRoadFinalizeCS, "/Plugin/PCGPlugins/Shaders/Private/RoadBuilder.usf", "FinalizeCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRoadDepthToResultCS, "/Plugin/PCGPlugins/Shaders/Private/RoadBuilder.usf", "RoadDepthToResultCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRoadDiffuseCS, "/Plugin/PCGPlugins/Shaders/Private/RoadBuilder.usf", "RoadDiffuseCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRoadUnpremultCS, "/Plugin/PCGPlugins/Shaders/Private/RoadBuilder.usf", "RoadUnpremultCS", SF_Compute);

#undef ROAD_SHADER

void AddRoadDepthToResultPass(
	FRDGBuilder& GraphBuilder,
	ERHIFeatureLevel::Type FeatureLevel,
	FRDGTextureRef DepthTexture,
	FRDGTextureRef ResultTexture,
	float CameraHeight,
	float Influence,
	float HeightOffset,
	int32 DiffuseIterations)
{
	if (!DepthTexture || !ResultTexture) return;

	// Must match RoadBuilder.usf.
	constexpr int32 kDiffTile = 16;   // ROAD_DIFF_TILE
	constexpr int32 kDiffLocal = 4;   // ROAD_DIFF_LOCAL

	const FIntPoint Size(ResultTexture->Desc.Extent.X, ResultTexture->Desc.Extent.Y);
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(FeatureLevel);
	const FIntVector Groups8 = FComputeShaderUtils::GetGroupCount(FIntVector(Size.X, Size.Y, 1), FIntVector(8, 8, 1));

	auto MakeStateTex = [&](const TCHAR* Name)
	{
		const FRDGTextureDesc D = FRDGTextureDesc::Create2D(Size, PF_FloatRGBA,
			FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
		return GraphBuilder.CreateTexture(D, Name);
	};
	FRDGTextureRef StateA = MakeStateTex(TEXT("Road.DiffuseA"));
	FRDGTextureRef StateB = MakeStateTex(TEXT("Road.DiffuseB"));

	// Pass 1: depth -> initial diffusion state (WorldZ*Weight, Weight, isSource) in StateA.
	{
		auto* P = GraphBuilder.AllocParameters<FRoadDepthToResultCS::FParameters>();
		P->T_RoadDepth = DepthTexture;
		P->RW_RoadResult = GraphBuilder.CreateUAV(StateA);
		P->RoadCameraHeight = CameraHeight;
		P->RoadInfluence = Influence;
		P->RoadHeightOffset = HeightOffset;
		P->RoadResultSize = Size;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Road.DepthToState"), ERDGPassFlags::Compute,
			TShaderMapRef<FRoadDepthToResultCS>(ShaderMap), P, Groups8);
	}

	// Pass 2: temporal-blocked height diffusion. Each dispatch runs up to kDiffLocal
	// iterations in groupshared, so global ping-pong happens ceil(N/kDiffLocal) times.
	const int32 TotalIters = FMath::Clamp(DiffuseIterations, 0, 256);
	const FIntVector DiffGroups(FMath::DivideAndRoundUp(Size.X, kDiffTile),
		FMath::DivideAndRoundUp(Size.Y, kDiffTile), 1);
	FRDGTextureRef Cur = StateA;
	FRDGTextureRef Nxt = StateB;
	for (int32 Done = 0; Done < TotalIters; Done += kDiffLocal)
	{
		auto* P = GraphBuilder.AllocParameters<FRoadDiffuseCS::FParameters>();
		P->T_RoadDiffuseIn = Cur;
		P->RW_RoadDiffuseOut = GraphBuilder.CreateUAV(Nxt);
		P->RoadDiffuseIterations = FMath::Min(kDiffLocal, TotalIters - Done);
		P->RoadDiffuseSize = Size;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Road.Diffuse"), ERDGPassFlags::Compute,
			TShaderMapRef<FRoadDiffuseCS>(ShaderMap), P, DiffGroups);
		Swap(Cur, Nxt);
	}

	// Pass 3: un-premultiply the final diffusion state -> ResultTexture (WorldZ, 0, 0, coverage).
	{
		auto* P = GraphBuilder.AllocParameters<FRoadUnpremultCS::FParameters>();
		P->T_RoadUnpremult = Cur;
		P->RW_RoadUnpremult = GraphBuilder.CreateUAV(ResultTexture);
		P->RoadUnpremultSize = Size;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Road.Unpremult"), ERDGPassFlags::Compute,
			TShaderMapRef<FRoadUnpremultCS>(ShaderMap), P, Groups8);
	}
}

// -----------------------------------------------------------------------------
// Reusable road-build producer (extracted from FRoadMeshSceneProxy::BuildRoadNetwork)
// -----------------------------------------------------------------------------

namespace
{
	constexpr uint32 GRoadMaxLegs = 12; // must match MAX_LEGS in RoadBuilder.usf

	// GPU mirror struct -- layout must match RoadBuilder.usf.
	struct FSplineInfoGPU
	{
		uint32 FirstPoint;
		uint32 NumPoints;
		float Length;
		float HalfWidth;
	};
}

void BuildRoadGeometryRDG(
	FRDGBuilder& GraphBuilder,
	ERHIFeatureLevel::Type FeatureLevel,
	const FRoadBuildInput& Input,
	const FRoadGeometryBuffers& Out)
{
	const uint32 NumSplines = Input.Splines.Num();
	if (NumSplines == 0) return;
	if (!Out.Positions || !Out.Tangents || !Out.TexCoords || !Out.Colors || !Out.Indices || !Out.IndirectArgs) return;

	// ---- upload spline samples (the only CPU -> GPU data transfer)
	TArray<FVector4f> PointData;
	PointData.Reserve(Input.Points.Num());
	for (const FRoadSplinePoint& P : Input.Points)
		PointData.Add(FVector4f(P.Position.X, P.Position.Y, P.Position.Z, P.DistanceAlongSpline));

	TArray<FSplineInfoGPU> SplineData;
	TArray<uint32> SegPrefixData;
	uint32 NumSegments = 0;
	for (const FRoadSplineRange& R : Input.Splines)
	{
		FSplineInfoGPU S;
		S.FirstPoint = R.FirstPoint;
		S.NumPoints = R.NumPoints;
		S.Length = R.Length;
		S.HalfWidth = R.HalfWidth;
		SplineData.Add(S);
		SegPrefixData.Add(NumSegments);
		NumSegments += R.NumPoints - 1;
	}
	SegPrefixData.Add(NumSegments);

	FRDGBufferRef SplinePointsBuf = CreateStructuredBuffer(GraphBuilder, TEXT("Road.SplinePoints"),
		sizeof(FVector4f), PointData.Num(), PointData.GetData(), sizeof(FVector4f) * PointData.Num());
	FRDGBufferRef SplinesBuf = CreateStructuredBuffer(GraphBuilder, TEXT("Road.Splines"),
		sizeof(FSplineInfoGPU), SplineData.Num(), SplineData.GetData(), sizeof(FSplineInfoGPU) * SplineData.Num());
	FRDGBufferRef SegPrefixBuf = CreateStructuredBuffer(GraphBuilder, TEXT("Road.SegPrefix"),
		sizeof(uint32), SegPrefixData.Num(), SegPrefixData.GetData(), sizeof(uint32) * SegPrefixData.Num());

	// ---- transient working buffers
	const uint32 MaxCrossRecs = FMath::Max(Input.MaxIntersections * 4, 64u);
	const uint32 MaxJunctions = FMath::Max(Input.MaxIntersections, 16u);
	const uint32 MaxLegsTotal = MaxJunctions * GRoadMaxLegs;

	FRDGBufferRef CrossRecsBuf = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(32, MaxCrossRecs), TEXT("Road.CrossRecs"));
	FRDGBufferRef JunctionsBuf = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(32, MaxJunctions), TEXT("Road.Junctions"));
	FRDGBufferRef LegsBuf = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(32, MaxLegsTotal), TEXT("Road.Legs"));
	FRDGBufferRef CornersBuf = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(48, MaxLegsTotal), TEXT("Road.Corners"));
	FRDGBufferRef CountersBuf = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 8), TEXT("Road.Counters"));
	FRDGBufferRef SplineFlagsBuf = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSplines), TEXT("Road.SplineFlags"));

	FRDGBufferRef PositionsRDG = Out.Positions;
	FRDGBufferRef TangentsRDG = Out.Tangents;
	FRDGBufferRef TexCoordsRDG = Out.TexCoords;
	FRDGBufferRef ColorsRDG = Out.Colors;
	FRDGBufferRef IndicesRDG = Out.Indices;
	FRDGBufferRef IndirectArgsRDG = Out.IndirectArgs;

	const auto CountersUAV = [&] { return GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CountersBuf, PF_R32_UINT)); };
	const auto SplineFlagsUAV = [&] { return GraphBuilder.CreateUAV(FRDGBufferUAVDesc(SplineFlagsBuf, PF_R32_UINT)); };

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(FeatureLevel);

	// ---- pass 1: clear
	{
		auto* Params = GraphBuilder.AllocParameters<FRoadClearCS::FParameters>();
		Params->RWCounters = CountersUAV();
		Params->RWIndirectArgs = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgsRDG, PF_R32_UINT));
		Params->RWSplineFlags = SplineFlagsUAV();
		Params->RWJunctions = GraphBuilder.CreateUAV(JunctionsBuf);
		Params->RWIndices = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndicesRDG, PF_R32_UINT));
		Params->NumSplines = NumSplines;
		Params->MaxJunctions = MaxJunctions;
		Params->MaxIndices = FMath::Max(Input.MaxIndices, 192u);
		const uint32 MaxItems = FMath::Max(FMath::Max(FMath::Max(NumSplines, MaxJunctions), 8u), Params->MaxIndices);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Road.Clear"), ERDGPassFlags::Compute,
			TShaderMapRef<FRoadClearCS>(ShaderMap), Params,
			FComputeShaderUtils::GetGroupCount(int32(MaxItems), 64));
	}

	// ---- pass 2: curve-curve crossings + endpoint T snapping
	{
		auto* Params = GraphBuilder.AllocParameters<FRoadIntersectCS::FParameters>();
		Params->SplinePoints = GraphBuilder.CreateSRV(SplinePointsBuf);
		Params->Splines = GraphBuilder.CreateSRV(SplinesBuf);
		Params->SegPrefix = GraphBuilder.CreateSRV(SegPrefixBuf);
		Params->RWCrossRecs = GraphBuilder.CreateUAV(CrossRecsBuf);
		Params->RWCounters = CountersUAV();
		Params->NumSplines = NumSplines;
		Params->NumSegments = NumSegments;
		Params->EndSnapRadius = Input.RoadHalfWidth * 2.0f;
		Params->MaxCrossRecs = MaxCrossRecs;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Road.Intersect"), ERDGPassFlags::Compute,
			TShaderMapRef<FRoadIntersectCS>(ShaderMap), Params,
			FComputeShaderUtils::GetGroupCount(int32(NumSegments + NumSplines * 2), 64));
	}

	// ---- pass 3: cluster crossings into junctions, build + sort legs
	{
		auto* Params = GraphBuilder.AllocParameters<FRoadClusterCS::FParameters>();
		Params->SplinePoints = GraphBuilder.CreateSRV(SplinePointsBuf);
		Params->Splines = GraphBuilder.CreateSRV(SplinesBuf);
		Params->RWCrossRecs = GraphBuilder.CreateUAV(CrossRecsBuf);
		Params->RWJunctions = GraphBuilder.CreateUAV(JunctionsBuf);
		Params->RWLegs = GraphBuilder.CreateUAV(LegsBuf);
		Params->RWCounters = CountersUAV();
		Params->RWSplineFlags = SplineFlagsUAV();
		Params->SampleStep = Input.SampleStep;
		Params->HalfWidth = Input.RoadHalfWidth;
		Params->MergeRadius = Input.RoadHalfWidth * 2.0f * Input.IntersectionMergeRadius;
		Params->MaxCrossRecs = MaxCrossRecs;
		Params->MaxJunctions = MaxJunctions;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Road.Cluster"), ERDGPassFlags::Compute,
			TShaderMapRef<FRoadClusterCS>(ShaderMap), Params, FIntVector(1, 1, 1));
	}

	// ---- pass 4: tangent-arc corner solve
	{
		auto* Params = GraphBuilder.AllocParameters<FRoadCornerSolveCS::FParameters>();
		Params->SplinePoints = GraphBuilder.CreateSRV(SplinePointsBuf);
		Params->Splines = GraphBuilder.CreateSRV(SplinesBuf);
		Params->Junctions = GraphBuilder.CreateSRV(JunctionsBuf);
		Params->Legs = GraphBuilder.CreateSRV(LegsBuf);
		Params->Counters = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CountersBuf, PF_R32_UINT));
		Params->RWCorners = GraphBuilder.CreateUAV(CornersBuf);
		Params->SampleStep = Input.SampleStep;
		Params->HalfWidth = Input.RoadHalfWidth;
		Params->CornerDist = FMath::Max(Input.RoadHalfWidth * 3.0f, Input.SampleStep);
		Params->MiterDotThreshold = 0.75f;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Road.CornerSolve"), ERDGPassFlags::Compute,
			TShaderMapRef<FRoadCornerSolveCS>(ShaderMap), Params,
			FComputeShaderUtils::GetGroupCount(int32(MaxLegsTotal), 64));
	}

	const auto MakeVertexUAVs = [&](auto* Params)
	{
		Params->RWCounters = CountersUAV();
		Params->RWPositions = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(PositionsRDG, PF_R32_FLOAT));
		Params->RWTangents = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TangentsRDG, PF_R32_UINT));
		Params->RWTexCoords = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TexCoordsRDG, PF_R32_FLOAT));
		Params->RWColors = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(ColorsRDG, PF_R32_UINT));
		Params->RWIndices = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndicesRDG, PF_R32_UINT));
	};

	// ---- pass 5: plain ribbons for splines without junctions
	{
		auto* Params = GraphBuilder.AllocParameters<FRoadEmitRoadCS::FParameters>();
		Params->SplinePoints = GraphBuilder.CreateSRV(SplinePointsBuf);
		Params->Splines = GraphBuilder.CreateSRV(SplinesBuf);
		Params->SplineFlags = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SplineFlagsBuf, PF_R32_UINT));
		MakeVertexUAVs(Params);
		Params->NumSplines = NumSplines;
		Params->SampleStep = Input.SampleStep;
		Params->HalfWidth = Input.RoadHalfWidth;
		Params->UVInvTile = Input.UVLengthScale;
		Params->MaxVertices = FMath::Max(Input.MaxVertices, 64u);
		Params->MaxIndices = FMath::Max(Input.MaxIndices, 192u);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Road.EmitRoad"), ERDGPassFlags::Compute,
			TShaderMapRef<FRoadEmitRoadCS>(ShaderMap), Params,
			FComputeShaderUtils::GetGroupCount(int32(NumSplines), 64));
	}

	// ---- pass 6: corner strips (junction surface)
	{
		auto* Params = GraphBuilder.AllocParameters<FRoadEmitCornerCS::FParameters>();
		Params->SplinePoints = GraphBuilder.CreateSRV(SplinePointsBuf);
		Params->Splines = GraphBuilder.CreateSRV(SplinesBuf);
		Params->Junctions = GraphBuilder.CreateSRV(JunctionsBuf);
		Params->Legs = GraphBuilder.CreateSRV(LegsBuf);
		Params->Corners = GraphBuilder.CreateSRV(CornersBuf);
		MakeVertexUAVs(Params);
		Params->SampleStep = Input.SampleStep;
		Params->HalfWidth = Input.RoadHalfWidth;
		Params->UVInvTile = Input.UVLengthScale;
		Params->MaxVertices = FMath::Max(Input.MaxVertices, 64u);
		Params->MaxIndices = FMath::Max(Input.MaxIndices, 192u);
		Params->MaxCornerSamples = 256;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Road.EmitCorner"), ERDGPassFlags::Compute,
			TShaderMapRef<FRoadEmitCornerCS>(ShaderMap), Params,
			FComputeShaderUtils::GetGroupCount(int32(MaxLegsTotal), 64));
	}

	// ---- pass 7: fill DrawIndexedIndirect arguments
	{
		auto* Params = GraphBuilder.AllocParameters<FRoadFinalizeCS::FParameters>();
		Params->RWCounters = CountersUAV();
		Params->RWIndirectArgs = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgsRDG, PF_R32_UINT));
		Params->MaxIndices = FMath::Max(Input.MaxIndices, 192u);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Road.Finalize"), ERDGPassFlags::Compute,
			TShaderMapRef<FRoadFinalizeCS>(ShaderMap), Params, FIntVector(1, 1, 1));
	}
}
