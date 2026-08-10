#include "ComputeShaderMeshBoolean.h"

#include "CSGpuMeshConvert.h"
#include "ComputeShaderGenerateHelper.h"

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderGraphResources.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "RHIGlobals.h"
#include "Engine/World.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

#include "Materials/MaterialInterface.h"
#include "Engine/StaticMesh.h"
#include "VectorTypes.h"
#include "IndexTypes.h"
#include "Algo/Sort.h"
#include "Async/ParallelFor.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

using namespace UE::Geometry;

namespace
{
	struct FMeshBooleanOrientationStats
	{
		int32 Corrections = 0;
		int32 CorrectionFailures = 0;
		int32 ResidualMismatches = 0;
		int32 MissingSources = 0;
	};

}

// =============================================================================
// Compute shaders（声明配方照抄基类 FExtractStaticMeshTrianglesCS）
// =============================================================================

class FTriTriIntersectCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FTriTriIntersectCS);
	SHADER_USE_PARAMETER_STRUCT(FTriTriIntersectCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, TriangleVertices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, TriangleCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_CutP0)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_CutP1)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_CutCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_Stats)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, TriTriBVHNodes)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, TriangleReferenceFlags)
		SHADER_PARAMETER(uint32, TriangleCapacity)
		SHADER_PARAMETER(uint32, MaxCutSegments)
		SHADER_PARAMETER(float, SideEps)
		SHADER_PARAMETER(float, ParallelEps)
		SHADER_PARAMETER(float, MinSegLenSq)
		SHADER_PARAMETER(float, SinCoplanarSq)
		SHADER_PARAMETER(float, CoplanarOffsetEps)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
};


class FFinalStatusCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFinalStatusCS);
	SHADER_USE_PARAMETER_STRUCT(FFinalStatusCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, FinalSoupCounter)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, FinalCutCounter)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, FinalTriStats)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, FinalOutputCounter)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, FinalBSPStats)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_FinalStatus)
		SHADER_PARAMETER(uint32, FinalBSPStatCount)
	END_SHADER_PARAMETER_STRUCT()
	CSGEN_SHADER_PERM_SM5()
};

// ---- GPU arrangement grouping（Milestone 1）：交线段按 owner 三角计数-排序成 per-tri CSR ----
#define MB_GROUP_PERM() \
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) \
	{ return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5); } \
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment) \
	{ FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment); OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64); }

class FCountCutsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCountCutsCS);
	SHADER_USE_PARAMETER_STRUCT(FCountCutsCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, GCutP0)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, GCutP1)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, GCutTotal)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_CutCount)
		SHADER_PARAMETER(uint32, GTriCap)
		SHADER_PARAMETER(uint32, GCutCap)
	END_SHADER_PARAMETER_STRUCT()
	MB_GROUP_PERM()
};

class FScanBlocksCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FScanBlocksCS);
	SHADER_USE_PARAMETER_STRUCT(FScanBlocksCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, GCutCountSRV)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_CutOffset)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_BlockSums)
		SHADER_PARAMETER(uint32, GScanCount)
	END_SHADER_PARAMETER_STRUCT()
	MB_GROUP_PERM()
};

class FScanBlockSumsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FScanBlockSumsCS);
	SHADER_USE_PARAMETER_STRUCT(FScanBlockSumsCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_BlockSums)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_ScanTotal)
		SHADER_PARAMETER(uint32, GNumBlocks)
	END_SHADER_PARAMETER_STRUCT()
	MB_GROUP_PERM()
};

class FAddOffsetsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FAddOffsetsCS);
	SHADER_USE_PARAMETER_STRUCT(FAddOffsetsCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, GBlockSumsSRV)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_CutOffset)
		SHADER_PARAMETER(uint32, GScanCount)
	END_SHADER_PARAMETER_STRUCT()
	MB_GROUP_PERM()
};

class FScatterCutsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FScatterCutsCS);
	SHADER_USE_PARAMETER_STRUCT(FScatterCutsCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, GCutP0)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, GCutP1)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, GCutTotal)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, GCutOffsetSRV)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_CutFill)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_CutCSR)
		SHADER_PARAMETER(uint32, GTriCap)
		SHADER_PARAMETER(uint32, GCutCap)
	END_SHADER_PARAMETER_STRUCT()
	MB_GROUP_PERM()
};

class FRetriangulateBSPNCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRetriangulateBSPNCS);
	SHADER_USE_PARAMETER_STRUCT(FRetriangulateBSPNCS, FGlobalShader);
public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, GBSPSoup)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, TriangleReferenceFlags)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, GCutP0)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, GCutP1)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, GCutCountSRV)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, GCutOffsetSRV)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, GCutCSRSRV)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector3f>, RW_OutSoup)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_OutSource)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_OutCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_BSPStats)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_LimitedReasons)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float2>, RW_CellVerts)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_CellVN)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_ScratchCounter)
		SHADER_PARAMETER(FVector3f, GSnapOrigin)
		SHADER_PARAMETER(float, GSnapQuantum)
		SHADER_PARAMETER(uint32, GBSPTriBase)
		SHADER_PARAMETER(uint32, GBSPTriCount)
		SHADER_PARAMETER(uint32, GOutCap)
		SHADER_PARAMETER(uint32, GTriCap)
	END_SHADER_PARAMETER_STRUCT()
	CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
};

// ---- Stage B：从 BSP 核里拆出的独立分类 pass（每 fragment 一线程，indirect 全宽） ----
class FClassifyFragmentsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FClassifyFragmentsCS);
	SHADER_USE_PARAMETER_STRUCT(FClassifyFragmentsCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector3f>, MBFragmentSoup)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBFragmentCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBFragmentSource)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBAmbiguousList)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBAmbiguousCounter)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, GBSPSoup)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, WMTopo)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, WMMultipole)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, WSoup)
		SHADER_PARAMETER(uint32, WFastTriCount)
		SHADER_PARAMETER(float, WBetaSq)
		SHADER_PARAMETER(float, GBooleanWindingSampleOffset)
		SHADER_PARAMETER(float, GBooleanWindingThreshold)
		SHADER_PARAMETER(float, GBooleanExpansionDistance)
		SHADER_PARAMETER(uint32, MBFragmentCapacity)
		SHADER_PARAMETER(uint32, MBAmbiguousCapacity)
		SHADER_PARAMETER(uint32, GTriCap)
		RDG_BUFFER_ACCESS(MBIndirectArgsBuffer, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()
	MB_GROUP_PERM()
};

class FRayRescueCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRayRescueCS);
	SHADER_USE_PARAMETER_STRUCT(FRayRescueCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector3f>, MBFragmentSoup)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBFragmentSource)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBAmbiguousListSRV)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBAmbiguousCounterSRV)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, GBSPSoup)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, WMTopo)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, WMMultipole)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, WSoup)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, OccluderVerts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, LBVHNodesSRV)
		SHADER_PARAMETER(uint32, WFastTriCount)
		SHADER_PARAMETER(float, WBetaSq)
		SHADER_PARAMETER(uint32, OccluderTriCount)
		SHADER_PARAMETER(uint32, RayCount)
		SHADER_PARAMETER(uint32, MaxSamples)
		SHADER_PARAMETER(float, CapMinCos)
		SHADER_PARAMETER(float, ShellRadius)
		SHADER_PARAMETER(float, RayBias)
		SHADER_PARAMETER(float, SampleDensity)
		SHADER_PARAMETER(uint32, bKeepBack)
		SHADER_PARAMETER(float, GBooleanWindingSampleOffset)
		SHADER_PARAMETER(float, GBooleanWindingThreshold)
		SHADER_PARAMETER(float, GBooleanExpansionDistance)
		SHADER_PARAMETER(uint32, MBFragmentCapacity)
		SHADER_PARAMETER(uint32, MBAmbiguousCapacity)
		SHADER_PARAMETER(uint32, GTriCap)
		RDG_BUFFER_ACCESS(MBIndirectArgsBuffer, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()
	MB_GROUP_PERM()
};

class FClassifyIndirectArgsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FClassifyIndirectArgsCS);
	SHADER_USE_PARAMETER_STRUCT(FClassifyIndirectArgsCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBFragmentCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBIndirectArgs)
		SHADER_PARAMETER(uint32, MBFragmentCapacity)
	END_SHADER_PARAMETER_STRUCT()
	MB_GROUP_PERM()
};

class FRescueIndirectArgsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRescueIndirectArgsCS);
	SHADER_USE_PARAMETER_STRUCT(FRescueIndirectArgsCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBAmbiguousCounterSRV)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBIndirectArgs)
		SHADER_PARAMETER(uint32, MBAmbiguousCapacity)
	END_SHADER_PARAMETER_STRUCT()
	MB_GROUP_PERM()
};

#undef MB_GROUP_PERM

IMPLEMENT_GLOBAL_SHADER(FTriTriIntersectCS, "/Plugin/PCGPlugins/Shaders/Private/MeshBoolean.usf", "TriTriIntersectCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FFinalStatusCS, "/Plugin/PCGPlugins/Shaders/Private/MeshBoolean.usf", "FinalStatusCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCountCutsCS, "/Plugin/PCGPlugins/Shaders/Private/MeshBoolean.usf", "CountCutsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FScanBlocksCS, "/Plugin/PCGPlugins/Shaders/Private/MeshBoolean.usf", "ScanBlocksCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FScanBlockSumsCS, "/Plugin/PCGPlugins/Shaders/Private/MeshBoolean.usf", "ScanBlockSumsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FAddOffsetsCS, "/Plugin/PCGPlugins/Shaders/Private/MeshBoolean.usf", "AddOffsetsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FScatterCutsCS, "/Plugin/PCGPlugins/Shaders/Private/MeshBoolean.usf", "ScatterCutsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRetriangulateBSPNCS, "/Plugin/PCGPlugins/Shaders/Private/MeshBoolean.usf", "RetriangulateBSPNCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FClassifyFragmentsCS, "/Plugin/PCGPlugins/Shaders/Private/MeshBoolean.usf", "ClassifyFragmentsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRayRescueCS, "/Plugin/PCGPlugins/Shaders/Private/MeshBoolean.usf", "RayRescueCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FClassifyIndirectArgsCS, "/Plugin/PCGPlugins/Shaders/Private/MeshBoolean.usf", "ClassifyIndirectArgsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRescueIndirectArgsCS, "/Plugin/PCGPlugins/Shaders/Private/MeshBoolean.usf", "RescueIndirectArgsCS", SF_Compute);

struct FMeshBooleanStageBRDGContext
{
	bool bEnabled = false;
	FRDGBufferSRVRef TopologySRV = nullptr;
	FRDGBufferSRVRef MultipoleSRV = nullptr;
	FRDGBufferSRVRef SoupSRV = nullptr;
	uint32 TriangleCount = 0;
	float WindingBetaSq = 1.0f;
	float WindingSampleOffset = 0.001f;
	float WindingThreshold = 0.5f;
	float ExpansionDistance = 0.0f;
	uint32 RayCount = 1;
	uint32 MaxSamples = 200;
	float CapMinCos = 0.0f;
	float ShellRadius = 1.0f;
	float RayBias = 0.001f;
	float SampleDensity = 0.0f;
	uint32 bKeepBack = 0;
};

// Forward declaration: Boolean-specific arrangement remains local. Shared LBVH, winding,
// and weld orchestration is inherited from AComputeShaderMeshGenerator and implemented by
// CSGpuTriangleUtilities, so this file owns only Boolean policy.
static void AddArrangementToRDG(FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef SoupSRV, FRDGBufferSRVRef ReferenceFlagsSRV,
	FRDGBufferSRVRef CutP0SRV, FRDGBufferSRVRef CutP1SRV, FRDGBufferSRVRef CutCounterSRV,
	int32 TriCapacity, int32 CutCapacity,
	const FVector3f& SnapOrigin, float SnapQuantum,
	int32 ScratchBatchSize, int32 OutCap, const FMeshBooleanStageBRDGContext& StageB,
	FRDGBufferRef& OutSoupBuf, FRDGBufferRef& OutSourceBuf, FRDGBufferRef& OutCounterBuf, FRDGBufferRef& OutBSPStatsBuf);

// =============================================================================
// CPU 端辅助
// =============================================================================

namespace
{
	constexpr int32 GPUArrangementTriangleBatchSize = 16384;
	constexpr int32 BSPMaxCuts = 64;
	constexpr int32 BSPMaxVerts = 16;
	constexpr int32 BSPMaxCells = 64;
	constexpr int32 BSPMaxOutputTrianglesPerSource = 64;
	constexpr uint64 ArrangementOutputBudgetBytes = 1536ull * 1024ull * 1024ull;
	constexpr int32 ArrangementOutputTriangleCapacity = int32(ArrangementOutputBudgetBytes /
		(uint64(sizeof(FVector3f)) * 3ull + uint64(sizeof(uint32))));
	static_assert(sizeof(FVector3f) == 12, "GPU arrangement output budget assumes tightly packed FVector3f");
	static_assert(ArrangementOutputTriangleCapacity == 40265318, "Unexpected fixed GPU arrangement output capacity");

	// OutSource 的位编码，必须与 MeshBoolean.usf 的 MB_SRC_* 保持一致：
	// 低 29 位是源三角序号，高 3 位是 Stage B 分类结果。
	constexpr uint32 MeshBooleanSourceMask = 0x1fffffffu;
	constexpr uint32 MeshBooleanSourceAmbiguous = 0x20000000u;
	constexpr uint32 MeshBooleanSourceInterior = 0x40000000u;
	constexpr uint32 MeshBooleanSourceKeep = 0x80000000u;

	enum EMeshBooleanBSPStat : uint32
	{
		BSPStatLimitedTriangles = 0,
		BSPStatDegenerate,
		BSPStatCutLimit,
		BSPStatVertLimit,
		BSPStatCellLimit,
		BSPStatOutputPerSourceLimit,
		BSPStatMaxSegments,
		BSPStatMaxVertices,
		BSPStatMaxCells,
		BSPStatScratchOverflow,
		BSPStatPartialSplitCommit,
		BSPStatMaxOutputTrianglesPerSource,
		BSPStatAreaMismatch,
		BSPStatCount
	};
	constexpr int32 FinalStatusSoupCount = 0;
	constexpr int32 FinalStatusCutCount = 1;
	constexpr int32 FinalStatusCutOverflow = 2;
	constexpr int32 FinalStatusTriStatsBase = 3;
	constexpr int32 FinalStatusOutputCount = 11;
	constexpr int32 FinalStatusOutputOverflow = 12;
	constexpr int32 FinalStatusBSPStatsBase = 13;
	constexpr int32 FinalStatusCount = FinalStatusBSPStatsBase + BSPStatCount;

	// CPU winding BVH 已移除：最终 BSP emit 直接遍历同一 RDG 内的 GPU LBVH + 多极 refit。

}

// =============================================================================
// AComputeShaderMeshBoolean
// =============================================================================

UStaticMesh* AComputeShaderMeshBoolean::SplitInterpenetratingBoxScene()
{
	return RunBooleanInternal(ECSMeshBooleanOp::ArrangementOnly);
}

UStaticMesh* AComputeShaderMeshBoolean::BooleanBoxScene(ECSMeshBooleanOp Op)
{
	return RunBooleanInternal(Op);
}

UStaticMesh* AComputeShaderMeshBoolean::RunBooleanInternal(ECSMeshBooleanOp Op)
{
	// Stage 0：校验运行环境与查询范围；任一条件无效时不启动 Boolean 管线。
	TRACE_CPUPROFILER_EVENT_SCOPE(MeshBoolean_RunBooleanInternal);
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	const FBox QueryBox = GetGeneratorBoundsWorldBox();
	if (!QueryBox.IsValid)
	{
		return nullptr;
	}

	// tri-tri 固定使用 GPU LBVH broad-phase。
	// Stage 1：规范化 Boolean 参数，并计算 LBVH、吸附取整及 Stage B 分类所需常量。
	const int32 CutPerTri = FMath::Max(1, CutSegmentsPerTriangle);
	const int32 CutHardCap = FMath::Max(1024, MaxCutSegmentsHardCap);
	const float SideEpsV = FMath::Max(0.0f, SideEpsilon);
	const float MinSegLenSqV = FMath::Max(1e-6f, MinCutSegmentLength * MinCutSegmentLength);
	// 共面判定阈值（归一化）：角度→sin²，偏移→真实 cm。
	const float CoplanarAngleRadV = FMath::DegreesToRadians(FMath::Clamp(CoplanarAngleDegrees, 0.0f, 45.0f));
	const float SinCoplanarSqV = FMath::Square(FMath::Sin(CoplanarAngleRadV));
	const float CoplanarOffsetEpsV = FMath::Max(0.0f, CoplanarOffsetEpsilon);

	// tri-tri broad-phase：LBVH 的 Morton 量化用 GeneratorBounds 盒作 AABB（CPU 已知，免 GPU 归约往返）。
	const FVector3f LBVHAabbMinV((float)QueryBox.Min.X, (float)QueryBox.Min.Y, (float)QueryBox.Min.Z);
	const FVector QBExt = QueryBox.GetSize();
	const FVector3f LBVHInvExtV(
		QBExt.X > 1e-3 ? 1.0f / float(QBExt.X) : 0.0f,
		QBExt.Y > 1e-3 ? 1.0f / float(QBExt.Y) : 0.0f,
		QBExt.Z > 1e-3 ? 1.0f / float(QBExt.Z) : 0.0f);

	// M4 GPU arrangement：snap-round 平移帧（origin=QueryBox.Min=LBVHAabbMinV；量化=max(SnapRoundQuantum, 最大边·2^-18)）。
	const FVector3f SnapOriginV = LBVHAabbMinV;
	const float SnapQV = FMath::Max(FMath::Max(SnapRoundQuantum, float(QBExt.GetMax()) * FMath::Pow(2.0f, -18.0f)), 1e-6f);
	const bool bRunStageB = (Op != ECSMeshBooleanOp::ArrangementOnly);
	const float StageBWindingBetaSqV = FMath::Max(1.0f, WindingBeta * WindingBeta);
	const float StageBWindingSampleOffsetV = FMath::Max(0.001f, WindingSampleOffset);
	const float StageBWindingThresholdV = FMath::Max(0.0f, WindingIsoThreshold);
	const float StageBExpansionDistanceV = bRunStageB ? FMath::Max(0.0f, RetainedTriangleExpansionDistance) : 0.0f;
	const uint32 StageBRayCountV = uint32(FMath::Max(1, VisibilityRayCount));
	const float StageBCapMinCosV = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(VisibilityHalfAngleDegrees, 90.0f, 180.0f)));
	const float StageBShellRadiusV = FMath::Max(
		VisibilityShellRadius > 0.0f ? VisibilityShellRadius : float(QBExt.Size()), 1.0f);
	const float StageBRayBiasV = FMath::Max(0.0f, VisibilityRayBiasEpsilon);
	const float StageBSampleDensityV = FMath::Max(0.0f, VisibilitySampleDensity);
	const uint32 StageBKeepBackV = bKeepBackFacingVisible ? 1u : 0u;
	const float OutputWeldDistanceV = FMath::Max(0.0f, VertexWeldDistance);
	const int32 OutputTrianglesPerSourceV = FMath::Max(2, ArrangementOutputTrianglesPerSource);

	// Stage 1.5: VRAM pre-flight. Every buffer below scales linearly with the source triangle
	// count, so the machine-dependent ceiling is knowable before any work starts - unlike the
	// fixed 8M guard on the render thread, which fires only after the soup was already built and
	// uploaded. The cost model mirrors what this run will actually allocate, so toggling Stage B
	// or welding moves the limit accordingly.
	{
		CSGpuMemoryBudget::FTriangleSoupCostModel Cost;
		Cost.CutSegmentsPerTriangle = CutPerTri;
		Cost.OutputTrianglesPerSource = OutputTrianglesPerSourceV;
		Cost.bBuildLBVH = true;
		Cost.bBuildWindingField = bRunStageB;
		Cost.bWeldOutput = OutputWeldDistanceV > UE_SMALL_NUMBER;
		// 源法线/切线始终回读：输出沿用源属性，不再重算法线。
		Cost.bSourceNormals = true;
		Cost.bSourceTangents = true;
		if (!ConfirmGpuMemoryBudgetForBoxScene(TEXT("Mesh Boolean"), QueryBox, Cost, bReadLandscape)) return nullptr;
	}

	// ---- game thread：解析场景三角形（bReadLandscape 控制是否纳入地形）----
	// Stage 2（Game Thread）：收集查询框内的场景三角形及其顶点属性，形成源 triangle soup。
	constexpr bool bUseMeshDescriptionSourceTriangles = true;
	FCSBoxScenePreparedData Prepared;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MeshBoolean_MeshDescriptionExtraction);
		Prepared = PrepareBoxSceneTriangles(
			World, QueryBox, MaxTriangles, TArray<FVector>(), 0.0f, NAME_None, bReadLandscape,
			bUseMeshDescriptionSourceTriangles, bPreserveSourceMaterialSlots);
	}
	if (!Prepared.IsValid() || !Prepared.HasAnyTriangles()) return nullptr;

	// 输出沿用源法线/切线，故两者恒需回读。
	constexpr bool bNeedSourceNormals = true;
	constexpr bool bNeedSourceTangents = true;

	// ---- readback 对象 ----
	// Stage 3：按输出策略创建 GPU 回读对象；未使用的属性不分配回读资源。
	FRHIGPUBufferReadback* VertexReadback = new FRHIGPUBufferReadback(TEXT("MeshBoolean_VertexReadback"));
	FRHIGPUBufferReadback* NormalReadback = bNeedSourceNormals ? new FRHIGPUBufferReadback(TEXT("MeshBoolean_NormalReadback")) : nullptr;
	FRHIGPUBufferReadback* FinalStatusReadback = new FRHIGPUBufferReadback(TEXT("MeshBoolean_FinalStatusReadback"));
	// Phase 2 材质追踪：与 soup 顶点平行的 per-triangle 材质 id（同一 atomic slot 写入）。
	FRHIGPUBufferReadback* MaterialReadback = new FRHIGPUBufferReadback(TEXT("MeshBoolean_MaterialReadback"));
	// UV 追踪：与 soup 顶点平行的 per-vertex UV0（同一 atomic slot 写入，3 per triangle）。
	FRHIGPUBufferReadback* UVReadback = new FRHIGPUBufferReadback(TEXT("MeshBoolean_UVReadback"));
	FRHIGPUBufferReadback* ColorReadback = new FRHIGPUBufferReadback(TEXT("MeshBoolean_ColorReadback"));
	FRHIGPUBufferReadback* TangentReadback = bNeedSourceTangents ? new FRHIGPUBufferReadback(TEXT("MeshBoolean_TangentReadback")) : nullptr;
	FRHIGPUBufferReadback* BiTangentReadback = bNeedSourceTangents ? new FRHIGPUBufferReadback(TEXT("MeshBoolean_BiTangentReadback")) : nullptr;
	// GPU arrangement 子三角 soup 回读。
	FRHIGPUBufferReadback* OutSoupReadback = new FRHIGPUBufferReadback(TEXT("MeshBoolean_OutSoupReadback"));
	FRHIGPUBufferReadback* OutSourceReadback = new FRHIGPUBufferReadback(TEXT("MeshBoolean_OutSourceReadback"));
	FRHIGPUBufferReadback* WeldRepresentativeReadback = OutputWeldDistanceV > UE_SMALL_NUMBER
		? new FRHIGPUBufferReadback(TEXT("MeshBoolean_WeldRepresentativeReadback"))
		: nullptr;

	const uint32 FinalStatusBytes = sizeof(uint32) * uint32(FinalStatusCount);
	TArray<uint32> FinalStatusData;
	FinalStatusData.SetNumZeroed(FinalStatusCount);

	bool bRenderWorkQueued = false;
	bool bHasGPUOutput = false;
	bool bArrangementCapacityUnsupported = false;
	int32 OutSubTriCap = 0;           // OutSoup 容量（子三角数）
	uint32 OutSoupBytes = 0;
	uint32 OutSrcBytes = 0;
	uint32 GPUArrOutCount = 0;
	uint32 GPUArrOutOverflow = 0;
	uint32 SoupTriangleCount = 0;
	int32 VertexCapacity = 0;
	uint32 VertexBytes = 0;
	uint32 NormalBytes = 0;
	int32 CutCapacity = 0;
	int32 MaterialCapacity = 0;
	uint32 MaterialBytes = 0;
	int32 UVCapacity = 0;
	int32 SoupUVChannels = 1;
	uint32 UVBytes = 0;
	uint32 CornerAttributeBytes = 0;
	uint32 WeldRepresentativeBytes = 0;

	ENQUEUE_RENDER_COMMAND(MeshBooleanSplitGPU)(
		[this, Prepared,
		 CutPerTri, CutHardCap, SideEpsV, MinSegLenSqV, SinCoplanarSqV, CoplanarOffsetEpsV,
		 LBVHAabbMinV, LBVHInvExtV, SnapOriginV, SnapQV,
		 bRunStageB, StageBWindingBetaSqV, StageBWindingSampleOffsetV, StageBWindingThresholdV,
		 StageBExpansionDistanceV, StageBRayCountV, StageBCapMinCosV, StageBShellRadiusV,
		 StageBRayBiasV, StageBSampleDensityV, StageBKeepBackV, OutputWeldDistanceV,
		 OutputTrianglesPerSourceV,
		 VertexReadback, NormalReadback, FinalStatusReadback, MaterialReadback, UVReadback, ColorReadback, TangentReadback, BiTangentReadback,
		 OutSoupReadback, OutSourceReadback, WeldRepresentativeReadback,
		 bNeedSourceNormals, bNeedSourceTangents,
		 FinalStatusBytes, &FinalStatusData,
		 &bRenderWorkQueued, &bHasGPUOutput, &bArrangementCapacityUnsupported, &OutSubTriCap, &OutSoupBytes, &OutSrcBytes,
		 &GPUArrOutCount, &GPUArrOutOverflow, &SoupTriangleCount, &WeldRepresentativeBytes,
		 &VertexCapacity, &VertexBytes, &NormalBytes, &CutCapacity, &MaterialCapacity, &MaterialBytes, &UVCapacity, &UVBytes, &SoupUVChannels, &CornerAttributeBytes]
		(FRHICommandListImmediate& RHICmdList)
		{
			// Stage 4（Render Thread）：上传源 triangle soup，并检查输入及固定输出容量是否受 RHI 支持。
			FRDGBuilder GraphBuilder(RHICmdList);
			auto ExecuteSingleGraph = [&GraphBuilder]() { GraphBuilder.Execute(); };

			FCSStaticMeshTriangleRDGOutput Soup;
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(MeshBoolean_SceneSoupUpload);
				Soup = AddPreparedBoxSceneTrianglesToRDG(
					GraphBuilder, RHICmdList, Prepared, TEXT("CS.MeshBoolean.Soup"));
			}

			if (!Soup.TriangleVertices || !Soup.TriangleCounter || Soup.MaxTriangles == 0)
			{
				ExecuteSingleGraph();
				return;
			}

			const uint32 TriangleCapacity = Soup.MaxTriangles;
			if (TriangleCapacity > 8000000u)
			{
				bArrangementCapacityUnsupported = true;
				ExecuteSingleGraph();
				bRenderWorkQueued = true;
				return;
			}
			// 按实际要分配的容量校验 RHI 上限，而不是按 1536 MiB 硬预算——缩容后本就更容易通过。
			const int64 PlannedOutputTriangleCapacity = FMath::Clamp<int64>(
				int64(TriangleCapacity) * int64(OutputTrianglesPerSourceV),
				1024, int64(ArrangementOutputTriangleCapacity));
			const uint64 ArrangementOutSoupCapacityBytes = uint64(PlannedOutputTriangleCapacity) * 3ull * sizeof(FVector3f);
			if (ArrangementOutSoupCapacityBytes > GRHIGlobals.MaxViewSizeBytesForNonTypedBuffer ||
				uint64(PlannedOutputTriangleCapacity) > GRHIGlobals.MaxViewDimensionForTypedBuffer)
			{
				bArrangementCapacityUnsupported = true;
				ExecuteSingleGraph();
				bRenderWorkQueued = true;
				return;
			}
			const uint32 MaxCutSegments = uint32(FMath::Clamp<int64>(
				int64(TriangleCapacity) * CutPerTri, 1024, CutHardCap));

			VertexCapacity = int32(Soup.MaxVertices);
			VertexBytes = uint32(uint64(Soup.MaxVertices) * sizeof(FVector4f));
			NormalBytes = VertexBytes;
			CutCapacity = int32(MaxCutSegments);
			MaterialCapacity = int32(Soup.MaxTriangles);
			MaterialBytes = uint32(uint64(Soup.MaxTriangles) * sizeof(uint32));
			SoupUVChannels = FMath::Max(1, Soup.NumUVChannels);
			UVCapacity = int32(Soup.MaxVertices) * SoupUVChannels;
			UVBytes = uint32(uint64(Soup.MaxVertices) * uint64(SoupUVChannels) * sizeof(FVector2f));
			CornerAttributeBytes = uint32(uint64(Soup.MaxVertices) * sizeof(FVector4f));

			TRefCountPtr<FRDGPooledBuffer> SourceVerticesExtracted;
			TRefCountPtr<FRDGPooledBuffer> SourceNormalsExtracted;
			TRefCountPtr<FRDGPooledBuffer> SourceMaterialsExtracted;
			TRefCountPtr<FRDGPooledBuffer> SourceUVsExtracted;
			TRefCountPtr<FRDGPooledBuffer> SourceColorsExtracted;
			TRefCountPtr<FRDGPooledBuffer> SourceTangentsExtracted;
			TRefCountPtr<FRDGPooledBuffer> SourceBiTangentsExtracted;
			GraphBuilder.QueueBufferExtraction(Soup.TriangleVertices, &SourceVerticesExtracted, ERHIAccess::CopySrc);
			if (bNeedSourceNormals && Soup.TriangleNormals) GraphBuilder.QueueBufferExtraction(Soup.TriangleNormals, &SourceNormalsExtracted, ERHIAccess::CopySrc);
			if (Soup.TriangleMaterialIds) GraphBuilder.QueueBufferExtraction(Soup.TriangleMaterialIds, &SourceMaterialsExtracted, ERHIAccess::CopySrc);
			if (Soup.TriangleUVs) GraphBuilder.QueueBufferExtraction(Soup.TriangleUVs, &SourceUVsExtracted, ERHIAccess::CopySrc);
			if (Soup.TriangleColors) GraphBuilder.QueueBufferExtraction(Soup.TriangleColors, &SourceColorsExtracted, ERHIAccess::CopySrc);
			if (bNeedSourceTangents && Soup.TriangleTangents) GraphBuilder.QueueBufferExtraction(Soup.TriangleTangents, &SourceTangentsExtracted, ERHIAccess::CopySrc);
			if (bNeedSourceTangents && Soup.TriangleBiTangents) GraphBuilder.QueueBufferExtraction(Soup.TriangleBiTangents, &SourceBiTangentsExtracted, ERHIAccess::CopySrc);

			// ---- 交线 buffer ----
			// Stage 5：创建相交线段、计数器及诊断缓冲区，供窄相位求交和 arrangement 共用。
			FRDGBufferRef CutP0Buf; FRDGBufferUAVRef CutP0UAV;
			CSHelper::CreateClearedTypedBuffer(GraphBuilder, CutP0Buf, CutP0UAV, sizeof(FVector4f), MaxCutSegments, PF_A32B32G32R32F, TEXT("CS.MeshBoolean.CutP0"), 0.0f);

			FRDGBufferRef CutP1Buf; FRDGBufferUAVRef CutP1UAV;
			CSHelper::CreateClearedTypedBuffer(GraphBuilder, CutP1Buf, CutP1UAV, sizeof(FVector4f), MaxCutSegments, PF_A32B32G32R32F, TEXT("CS.MeshBoolean.CutP1"), 0.0f);

			FRDGBufferRef CutCounterBuf; FRDGBufferUAVRef CutCounterUAV;
			CSHelper::CreateClearedTypedBuffer(GraphBuilder, CutCounterBuf, CutCounterUAV, sizeof(uint32), 2, PF_R32_UINT, TEXT("CS.MeshBoolean.CutCounter"), 0u);

			FRDGBufferRef StatsBuf; FRDGBufferUAVRef StatsUAV;
			CSHelper::CreateClearedTypedBuffer(GraphBuilder, StatsBuf, StatsUAV, sizeof(uint32), 8, PF_R32_UINT, TEXT("CS.MeshBoolean.Stats"), 0u);

			// ---- tri-tri broad-phase：固定构建并遍历 LBVH。 ----
			// Stage 6：构建三角形 LBVH；需要布尔分类时同时构建快速缠绕数多极矩场。
			int32 SortM = 1; while (SortM < int32(TriangleCapacity)) SortM <<= 1;
			const CSGpuTriangleUtilities::FTriangleLBVH TriangleLBVH = AddTriangleLBVHToRDG(
				GraphBuilder, Soup.TriangleVerticesSRV, int32(TriangleCapacity), SortM,
				LBVHAabbMinV, LBVHInvExtV);
			FRDGBufferSRVRef TriBVHNodesSRV = GraphBuilder.CreateSRV(
				FRDGBufferSRVDesc(TriangleLBVH.Nodes, PF_A32B32G32R32F));
			FMeshBooleanStageBRDGContext StageBContext;
			if (bRunStageB)
			{
				// The base facility produces only the winding field. Boolean retains the
				// iso threshold and sample offset below because those define classification.
				FRDGBufferRef WindingMultipoles = AddFastWindingToRDG(
					GraphBuilder, Soup.TriangleVerticesSRV, TriangleLBVH, int32(TriangleCapacity));
				StageBContext.bEnabled = true;
				StageBContext.TopologySRV = TriBVHNodesSRV;
				StageBContext.MultipoleSRV = GraphBuilder.CreateSRV(
					FRDGBufferSRVDesc(WindingMultipoles, PF_A32B32G32R32F));
				StageBContext.SoupSRV = Soup.TriangleVerticesSRV;
				StageBContext.TriangleCount = TriangleCapacity;
				StageBContext.WindingBetaSq = StageBWindingBetaSqV;
				StageBContext.WindingSampleOffset = StageBWindingSampleOffsetV;
				StageBContext.WindingThreshold = StageBWindingThresholdV;
				StageBContext.ExpansionDistance = StageBExpansionDistanceV;
				StageBContext.RayCount = StageBRayCountV;
				StageBContext.CapMinCos = StageBCapMinCosV;
				StageBContext.ShellRadius = StageBShellRadiusV;
				StageBContext.RayBias = StageBRayBiasV;
				StageBContext.SampleDensity = StageBSampleDensityV;
				StageBContext.bKeepBack = StageBKeepBackV;
			}

			// ---- 三角形对求交（LBVH broad-phase），包裹 dispatch 支持 >4.19M 三角 ----
			{
				// Stage 7：遍历 LBVH 执行 tri-tri 窄相位求交，输出每个源三角形上的切割线段。
				TRACE_CPUPROFILER_EVENT_SCOPE(MeshBoolean_TriTriSubmission);
				FTriTriIntersectCS::FParameters* P = GraphBuilder.AllocParameters<FTriTriIntersectCS::FParameters>();
				P->TriangleVertices = Soup.TriangleVerticesSRV;
				P->TriangleCounter = Soup.TriangleCounterSRV;
				P->RW_CutP0 = CutP0UAV;
				P->RW_CutP1 = CutP1UAV;
				P->RW_CutCounter = CutCounterUAV;
				P->RW_Stats = StatsUAV;
				P->TriTriBVHNodes = TriBVHNodesSRV;
				P->TriangleReferenceFlags = Soup.TriangleReferenceFlagsSRV;
				P->TriangleCapacity = TriangleCapacity;
				P->MaxCutSegments = MaxCutSegments;
				P->SideEps = SideEpsV;
				P->ParallelEps = 1e-8f;
				P->MinSegLenSq = MinSegLenSqV;
				P->SinCoplanarSq = SinCoplanarSqV;
				P->CoplanarOffsetEps = CoplanarOffsetEpsV;

				TShaderMapRef<FTriTriIntersectCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CS.MeshBoolean.TriTriIntersect"), Shader, P,
					FComputeShaderUtils::GetGroupCountWrapped(int32(TriangleCapacity), 64));
			}

			// ---- GPU arrangement：固定执行 CSR 分组 + N 段 BSP 重三角化。 ----
			// Stage 8：将切割线段整理为 CSR，分批进行 BSP 切分与重三角化；Stage B 在此融合完成保留/剔除分类。
			TRefCountPtr<FRDGPooledBuffer> ArrOutSoupExtracted;
			TRefCountPtr<FRDGPooledBuffer> ArrOutSourceExtracted;
			TRefCountPtr<FRDGPooledBuffer> WeldRepresentativeExtracted;
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(MeshBoolean_ArrangementSubmission);
				const int32 ScratchBatchSize = FMath::Min(int32(TriangleCapacity), GPUArrangementTriangleBatchSize);
				// 输出容量按源三角数缩放，不再无条件买断 1536 MiB：实测子三角数约为源三角的 2.5 倍，
				// 固定容量的利用率只有 ~7%，其余全是每次调用都要分配并清零的死重。溢出仍由
				// GPUArrOutOverflow 拦下并报错（提示调大倍率），语义与固定容量时一致。
				const int32 OutCapLocal = int32(FMath::Clamp<int64>(
					int64(TriangleCapacity) * int64(OutputTrianglesPerSourceV),
					1024, int64(ArrangementOutputTriangleCapacity)));
				FRDGBufferRef ArrOutSoup = nullptr, ArrOutSrc = nullptr, ArrOutCnt = nullptr, ArrOutStat = nullptr;
				AddArrangementToRDG(GraphBuilder, Soup.TriangleVerticesSRV, Soup.TriangleReferenceFlagsSRV,
					GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CutP0Buf, PF_A32B32G32R32F)),
					GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CutP1Buf, PF_A32B32G32R32F)),
					GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CutCounterBuf, PF_R32_UINT)),
					int32(TriangleCapacity), int32(MaxCutSegments),
					SnapOriginV, SnapQV, ScratchBatchSize, OutCapLocal, StageBContext,
					ArrOutSoup, ArrOutSrc, ArrOutCnt, ArrOutStat);
				if (OutputWeldDistanceV > UE_SMALL_NUMBER)
				{
					// Stage 9（可选 GPU 后处理）：为近邻角点生成焊接代表元，实际拓扑重建留在 CPU 完成。
					// Shared welding stops at corner representatives. Source attributes,
					// duplicate removal, and winding restoration are Boolean output policy.
					// Stage B 只给 fragment 打标记而不从 soup 里移除，故必须让 weld 跳过被剔除的
					// 角点：桶里只保留最小角点序号，混入的死角点会遮蔽真正该配对的活角点。
					// 过滤条件与 CPU 消费端一致：ArrangementOnly 不跑分类，此时不过滤。
					const bool bFilterWeldByKeep = bRunStageB;
					FRDGBufferSRVRef WeldFilterSRV = bFilterWeldByKeep
						? GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ArrOutSrc, PF_R32_UINT))
						: nullptr;
					FRDGBufferRef WeldRepresentatives = AddVertexWeldToRDG(
						GraphBuilder, ArrOutSoup, ArrOutCnt, OutCapLocal, int32(TriangleCapacity),
						SnapOriginV, OutputWeldDistanceV,
						WeldFilterSRV, bFilterWeldByKeep ? MeshBooleanSourceKeep : 0u);
					GraphBuilder.QueueBufferExtraction(
						WeldRepresentatives, &WeldRepresentativeExtracted, ERHIAccess::CopySrc);
				}
				// Stage 10：汇总 soup、切线、输出及 BSP 状态，先回读小型状态块以确定精确回读字节数。
				FRDGBufferRef FinalStatusBuf = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), FinalStatusCount), TEXT("MB.FinalStatus"));
				FFinalStatusCS::FParameters* FinalStatusParameters =
					GraphBuilder.AllocParameters<FFinalStatusCS::FParameters>();
				FinalStatusParameters->FinalSoupCounter = Soup.TriangleCounterSRV;
				FinalStatusParameters->FinalCutCounter = GraphBuilder.CreateSRV(
					FRDGBufferSRVDesc(CutCounterBuf, PF_R32_UINT));
				FinalStatusParameters->FinalTriStats = GraphBuilder.CreateSRV(
					FRDGBufferSRVDesc(StatsBuf, PF_R32_UINT));
				FinalStatusParameters->FinalOutputCounter = GraphBuilder.CreateSRV(
					FRDGBufferSRVDesc(ArrOutCnt, PF_R32_UINT));
				FinalStatusParameters->FinalBSPStats = GraphBuilder.CreateSRV(
					FRDGBufferSRVDesc(ArrOutStat, PF_R32_UINT));
				FinalStatusParameters->RW_FinalStatus = GraphBuilder.CreateUAV(
					FRDGBufferUAVDesc(FinalStatusBuf, PF_R32_UINT));
				FinalStatusParameters->FinalBSPStatCount = BSPStatCount;
				TShaderMapRef<FFinalStatusCS> FinalStatusShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.FinalStatus"),
					FinalStatusShader, FinalStatusParameters, FIntVector(1, 1, 1));
				AddEnqueueCopyPass(GraphBuilder, FinalStatusReadback, FinalStatusBuf, FinalStatusBytes);
				UE_LOG(LogTemp, Log, TEXT("[MeshBoolean:%s] [GPU-arr] scratchBatch=%d batches=%d limits=%d cuts/%d verts/%d cells/%d outputsPerSource outCap=%d (%.0fx源三角, %.0fMiB, 硬上限 %d/1536MiB)"),
					*GetName(), ScratchBatchSize, FMath::DivideAndRoundUp(int32(TriangleCapacity), ScratchBatchSize),
					BSPMaxCuts, BSPMaxVerts, BSPMaxCells, BSPMaxOutputTrianglesPerSource, OutCapLocal,
					double(OutCapLocal) / FMath::Max(1.0, double(TriangleCapacity)),
					double(uint64(OutCapLocal) * 40ull) / (1024.0 * 1024.0),
					ArrangementOutputTriangleCapacity);
				OutSubTriCap = OutCapLocal;
				GraphBuilder.QueueBufferExtraction(ArrOutSoup, &ArrOutSoupExtracted, ERHIAccess::CopySrc);
				GraphBuilder.QueueBufferExtraction(ArrOutSrc, &ArrOutSourceExtracted, ERHIAccess::CopySrc);
			}

			ExecuteSingleGraph();
			bHasGPUOutput = true;
			{
				// Stage 11：读取状态前缀，并仅为有效输出范围提交几何及属性回读，避免整容量复制。
				TRACE_CPUPROFILER_EVENT_SCOPE(MeshBoolean_ArrangementPrefixReadbackSubmission);
				if (!FinalStatusReadback->IsReady()) RHICmdList.SubmitAndBlockUntilGPUIdle();
				if (const uint32* Ptr = static_cast<const uint32*>(FinalStatusReadback->Lock(FinalStatusBytes)))
				{
					FMemory::Memcpy(FinalStatusData.GetData(), Ptr, FinalStatusBytes);
					FinalStatusReadback->Unlock();
					SoupTriangleCount = FMath::Min(
						FinalStatusData[FinalStatusSoupCount], TriangleCapacity);
					GPUArrOutCount = FinalStatusData[FinalStatusOutputCount];
					GPUArrOutOverflow = FinalStatusData[FinalStatusOutputOverflow];
				}
				else bHasGPUOutput = false;

				const uint32 SourceVertexCount = SoupTriangleCount * 3u;
				VertexCapacity = int32(SourceVertexCount);
				VertexBytes = SourceVertexCount * sizeof(FVector4f);
				NormalBytes = bNeedSourceNormals ? VertexBytes : 0u;
				MaterialCapacity = int32(SoupTriangleCount);
				MaterialBytes = SoupTriangleCount * sizeof(uint32);
				UVCapacity = int32(SourceVertexCount) * SoupUVChannels;
				UVBytes = SourceVertexCount * uint32(SoupUVChannels) * sizeof(FVector2f);
				CornerAttributeBytes = SourceVertexCount * sizeof(FVector4f);
				if (bHasGPUOutput && VertexBytes > 0u && SourceVerticesExtracted) VertexReadback->EnqueueCopy(RHICmdList, SourceVerticesExtracted->GetRHI(), VertexBytes);
				else bHasGPUOutput = false;
				if (bHasGPUOutput && NormalBytes > 0u && SourceNormalsExtracted) NormalReadback->EnqueueCopy(RHICmdList, SourceNormalsExtracted->GetRHI(), NormalBytes);
				if (bHasGPUOutput && MaterialBytes > 0u && SourceMaterialsExtracted) MaterialReadback->EnqueueCopy(RHICmdList, SourceMaterialsExtracted->GetRHI(), MaterialBytes);
				if (bHasGPUOutput && UVBytes > 0u && SourceUVsExtracted) UVReadback->EnqueueCopy(RHICmdList, SourceUVsExtracted->GetRHI(), UVBytes);
				if (bHasGPUOutput && CornerAttributeBytes > 0u && SourceColorsExtracted) ColorReadback->EnqueueCopy(RHICmdList, SourceColorsExtracted->GetRHI(), CornerAttributeBytes);
				if (bHasGPUOutput && bNeedSourceTangents && CornerAttributeBytes > 0u && SourceTangentsExtracted) TangentReadback->EnqueueCopy(RHICmdList, SourceTangentsExtracted->GetRHI(), CornerAttributeBytes);
				if (bHasGPUOutput && bNeedSourceTangents && CornerAttributeBytes > 0u && SourceBiTangentsExtracted) BiTangentReadback->EnqueueCopy(RHICmdList, SourceBiTangentsExtracted->GetRHI(), CornerAttributeBytes);

				const uint32 PrefixCount = GPUArrOutOverflow == 0u
					? FMath::Min(GPUArrOutCount, uint32(FMath::Max(0, OutSubTriCap)))
					: 0u;
				OutSoupBytes = PrefixCount * 3u * sizeof(FVector3f);
				OutSrcBytes = PrefixCount * sizeof(uint32);
				WeldRepresentativeBytes = OutputWeldDistanceV > UE_SMALL_NUMBER
					? PrefixCount * 3u * sizeof(uint32)
					: 0u;
				if (bHasGPUOutput && OutSoupBytes > 0u && ArrOutSoupExtracted && ArrOutSourceExtracted)
				{
					OutSoupReadback->EnqueueCopy(RHICmdList, ArrOutSoupExtracted->GetRHI(), OutSoupBytes);
					OutSourceReadback->EnqueueCopy(RHICmdList, ArrOutSourceExtracted->GetRHI(), OutSrcBytes);
				}
				else if (bHasGPUOutput && OutSoupBytes > 0u) bHasGPUOutput = false;
				if (bHasGPUOutput && WeldRepresentativeBytes > 0u
					&& WeldRepresentativeReadback && WeldRepresentativeExtracted)
				{
					WeldRepresentativeReadback->EnqueueCopy(
						RHICmdList, WeldRepresentativeExtracted->GetRHI(), WeldRepresentativeBytes);
				}
				else if (bHasGPUOutput && WeldRepresentativeBytes > 0u) bHasGPUOutput = false;
			}
			bRenderWorkQueued = true;
		});

	FlushRenderingCommands();

	if (!bRenderWorkQueued || !bHasGPUOutput || VertexCapacity <= 0)
	{
		if (bArrangementCapacityUnsupported)
			UE_LOG(LogTemp, Error, TEXT("[MeshBoolean:%s] GPU arrangement aborted: source or fixed 1536 MiB output buffers exceed the supported project/RHI limits"), *GetName());
		delete VertexReadback; delete NormalReadback; delete FinalStatusReadback;
		delete MaterialReadback; delete UVReadback;
		delete ColorReadback; delete TangentReadback; delete BiTangentReadback;
		delete OutSoupReadback; delete OutSourceReadback;
		delete WeldRepresentativeReadback;
		return nullptr;
	}
	delete FinalStatusReadback;
	FinalStatusReadback = nullptr;

	// ---- 读回 ----
	// Stage 12（Game Thread）：等待并解码 GPU 输出、源属性及诊断数据。
	TArray<FVector4f> VertexData;
	TArray<FVector4f> NormalData;
	VertexData.SetNumZeroed(VertexCapacity);
	if (bNeedSourceNormals) NormalData.SetNumZeroed(VertexCapacity);
	// GPU arrangement 子三角 soup。
	TArray<FVector3f> GOutSoup;
	TArray<uint32> GOutSrc;
	TArray<uint32> GOutWeldRepresentatives;
	TArray<uint32> GOutCnt; GOutCnt.SetNumZeroed(2);
	TArray<uint32> GStat;   GStat.SetNumZeroed(BSPStatCount);
	GOutCnt[0] = FinalStatusData[FinalStatusOutputCount];
	GOutCnt[1] = FinalStatusData[FinalStatusOutputOverflow];
	for (int32 Index = 0; Index < BSPStatCount; ++Index) GStat[Index] = FinalStatusData[FinalStatusBSPStatsBase + Index];
	// CPU storage and GPU staging are both count-sized; the arrangement compute buffers remain capacity-sized.
	GOutSoup.SetNumZeroed(1);
	GOutSrc.SetNumZeroed(1);
	uint32 CutCount = FinalStatusData[FinalStatusCutCount];
	uint32 CutOverflow = FinalStatusData[FinalStatusCutOverflow];
	TArray<uint32> StatsData;
	StatsData.SetNumZeroed(8);
	for (int32 Index = 0; Index < StatsData.Num(); ++Index) StatsData[Index] = FinalStatusData[FinalStatusTriStatsBase + Index];
	// 每 soup 三角一个材质 registry id（CS_NO_MATERIAL_ID = 无材质）。
	TArray<uint32> MaterialIds;
	MaterialIds.Init(CS_NO_MATERIAL_ID, FMath::Max(1, MaterialCapacity));
	// 每 soup 顶点一个 UV0（3 per triangle，与 VertexData 平行）。无 UV 的源保持 (0,0)。
	TArray<FVector2f> UVData;
	UVData.SetNumZeroed(FMath::Max(1, UVCapacity));
	TArray<FVector4f> ColorData, TangentData, BiTangentData;
	ColorData.SetNumZeroed(FMath::Max(1, UVCapacity));
	if (bNeedSourceTangents)
	{
		TangentData.SetNumZeroed(FMath::Max(1, UVCapacity));
		BiTangentData.SetNumZeroed(FMath::Max(1, UVCapacity));
	}
	bool bReadbackOk = false;

	ENQUEUE_RENDER_COMMAND(MeshBooleanSplitReadback)(
		[VertexReadback, NormalReadback, MaterialReadback, UVReadback, ColorReadback, TangentReadback, BiTangentReadback,
		 OutSoupReadback, OutSourceReadback, WeldRepresentativeReadback,
		 VertexBytes, NormalBytes, MaterialBytes, UVBytes, CornerAttributeBytes, OutSoupBytes, OutSrcBytes,
		 WeldRepresentativeBytes,
		 bNeedSourceTangents,
		 OutSubTriCap, GPUArrOutCount, GPUArrOutOverflow,
		 &VertexData, &NormalData, &MaterialIds, &UVData, &ColorData, &TangentData, &BiTangentData,
		 &GOutSoup, &GOutSrc, &GOutWeldRepresentatives, &GOutCnt, &bReadbackOk]
		(FRHICommandListImmediate& RHICmdList)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(MeshBoolean_GPUReadback);
			// 等待固定 GPU arrangement 路径实际提交的回读。
			const bool bBusy = !VertexReadback->IsReady()
				|| (NormalBytes > 0u && !NormalReadback->IsReady())
				|| (MaterialBytes > 0u && !MaterialReadback->IsReady())
				|| (UVBytes > 0u && !UVReadback->IsReady())
				|| (CornerAttributeBytes > 0u && !ColorReadback->IsReady())
				|| (bNeedSourceTangents && CornerAttributeBytes > 0u
					&& (!TangentReadback->IsReady() || !BiTangentReadback->IsReady()))
				|| (OutSoupBytes > 0u && !OutSoupReadback->IsReady())
				|| (OutSrcBytes > 0u && !OutSourceReadback->IsReady())
				|| (WeldRepresentativeBytes > 0u
					&& (!WeldRepresentativeReadback || !WeldRepresentativeReadback->IsReady()));
			if (bBusy) RHICmdList.SubmitAndBlockUntilGPUIdle();

			bool bOk = true;
			if (const FVector4f* Ptr = static_cast<const FVector4f*>(VertexReadback->Lock(VertexBytes)))
			{ FMemory::Memcpy(VertexData.GetData(), Ptr, VertexBytes); VertexReadback->Unlock(); }
			else { bOk = false; }
			if (NormalBytes > 0u)
			{
				if (const FVector4f* Ptr = static_cast<const FVector4f*>(NormalReadback->Lock(NormalBytes)))
				{ FMemory::Memcpy(NormalData.GetData(), Ptr, NormalBytes); NormalReadback->Unlock(); }
				else bOk = false;
			}

			// Counter was consumed after the first RDG submission so only the valid output prefix was copied.
			{
				GOutCnt[0] = GPUArrOutCount;
				GOutCnt[1] = GPUArrOutOverflow;
				const uint32 OutCount = GOutCnt[1] == 0u
					? FMath::Min(GOutCnt[0], uint32(FMath::Max(0, OutSubTriCap)))
					: 0u;
				const uint32 OutSoupReadBytes = OutSoupBytes;
				const uint32 OutSrcReadBytes = OutSrcBytes;
				GOutSoup.SetNumZeroed(FMath::Max(1, int32(OutCount * 3u)));
				GOutSrc.SetNumZeroed(FMath::Max(1, int32(OutCount)));
				if (OutSoupReadBytes > 0u)
				{
					if (const FVector3f* Ptr = static_cast<const FVector3f*>(OutSoupReadback->Lock(OutSoupReadBytes)))
					{ FMemory::Memcpy(GOutSoup.GetData(), Ptr, OutSoupReadBytes); OutSoupReadback->Unlock(); }
					else { bOk = false; }
				}
				if (OutSrcReadBytes > 0u)
				{
					if (const uint32* Ptr = static_cast<const uint32*>(OutSourceReadback->Lock(OutSrcReadBytes)))
					{ FMemory::Memcpy(GOutSrc.GetData(), Ptr, OutSrcReadBytes); OutSourceReadback->Unlock(); }
					else { bOk = false; }
				}
				if (WeldRepresentativeBytes > 0u)
				{
					GOutWeldRepresentatives.SetNumZeroed(int32(WeldRepresentativeBytes / sizeof(uint32)));
					if (const uint32* Ptr = static_cast<const uint32*>(
						WeldRepresentativeReadback->Lock(WeldRepresentativeBytes)))
					{
						FMemory::Memcpy(
							GOutWeldRepresentatives.GetData(), Ptr, WeldRepresentativeBytes);
						WeldRepresentativeReadback->Unlock();
					}
					else bOk = false;
				}
			}

			if (MaterialBytes > 0)
			{
				if (const uint32* Ptr = static_cast<const uint32*>(MaterialReadback->Lock(MaterialBytes)))
				{ FMemory::Memcpy(MaterialIds.GetData(), Ptr, MaterialBytes); MaterialReadback->Unlock(); }
				else bOk = false;
			}
			if (UVBytes > 0)
			{
				if (const FVector2f* Ptr = static_cast<const FVector2f*>(UVReadback->Lock(UVBytes)))
				{ FMemory::Memcpy(UVData.GetData(), Ptr, UVBytes); UVReadback->Unlock(); }
			}
			if (CornerAttributeBytes > 0)
			{
				if (const FVector4f* Ptr = static_cast<const FVector4f*>(ColorReadback->Lock(CornerAttributeBytes)))
				{ FMemory::Memcpy(ColorData.GetData(), Ptr, CornerAttributeBytes); ColorReadback->Unlock(); }
			}
			if (bNeedSourceTangents && CornerAttributeBytes > 0)
			{
				if (const FVector4f* Ptr = static_cast<const FVector4f*>(TangentReadback->Lock(CornerAttributeBytes)))
				{ FMemory::Memcpy(TangentData.GetData(), Ptr, CornerAttributeBytes); TangentReadback->Unlock(); }
				if (const FVector4f* Ptr = static_cast<const FVector4f*>(BiTangentReadback->Lock(CornerAttributeBytes)))
				{ FMemory::Memcpy(BiTangentData.GetData(), Ptr, CornerAttributeBytes); BiTangentReadback->Unlock(); }
			}

			delete VertexReadback; delete NormalReadback;
			delete MaterialReadback; delete UVReadback;
			delete ColorReadback; delete TangentReadback; delete BiTangentReadback;
			delete OutSoupReadback; delete OutSourceReadback;
			delete WeldRepresentativeReadback;
			bReadbackOk = bOk;
		});

	FlushRenderingCommands();

	if (!bReadbackOk)
	{
		return nullptr;
	}

	const int32 MaxTriCapacity = VertexCapacity / 3;
	const int32 TriCount = FMath::Clamp<int32>(int32(SoupTriangleCount), 0, MaxTriCapacity);
	const int32 SafeCutCount = FMath::Clamp<int32>(int32(CutCount), 0, CutCapacity);
	const bool bInputOverflow = (CutOverflow > 0u) || (StatsData[3] > 0u);
	const bool bArrangementOverflow = (GOutCnt[1] > 0u) ||
		(GOutCnt[0] > uint32(FMath::Max(0, OutSubTriCap))) ||
		(GStat[BSPStatScratchOverflow] > 0u) ||
		(GStat[BSPStatPartialSplitCommit] > 0u) ||
		(GStat[BSPStatAreaMismatch] > 0u);
	if (bInputOverflow)
	{
		UE_LOG(LogTemp, Error, TEXT("[MeshBoolean:%s] Arrangement aborted: incomplete tri-tri cut input (cutOverflow=%u segOverflow=%u)"),
			*GetName(), CutOverflow, StatsData[3]);
		return nullptr;
	}
	if (bArrangementOverflow)
	{
		if (GOutCnt[1] > 0u || GOutCnt[0] > uint32(FMath::Max(0, OutSubTriCap)))
			UE_LOG(LogTemp, Error, TEXT("[MeshBoolean:%s] 子三角输出超出容量（需要 %u > 容量 %d）。把 ArrangementOutputTrianglesPerSource 调大（当前 %d）后重试。"),
				*GetName(), GOutCnt[0], OutSubTriCap, OutputTrianglesPerSourceV);
		UE_LOG(LogTemp, Error, TEXT("[MeshBoolean:%s] GPU arrangement aborted: structural/global failure (outOverflow=%u scratchOverflow=%u partialSplitCommit=%u areaMismatch=%u maxSeg=%u maxVerts=%u maxCells=%u maxOutputPerSource=%u)"),
			*GetName(), GOutCnt[1], GStat[BSPStatScratchOverflow], GStat[BSPStatPartialSplitCommit],
			GStat[BSPStatAreaMismatch], GStat[BSPStatMaxSegments], GStat[BSPStatMaxVertices],
			GStat[BSPStatMaxCells], GStat[BSPStatMaxOutputTrianglesPerSource]);
		return nullptr;
	}
	{
		const double GPUCopyMiB = double(uint64(OutSoupBytes) + uint64(OutSrcBytes)) / (1024.0 * 1024.0);
		const double CPUCopyMiB = double(uint64(GOutSoup.Num()) * sizeof(FVector3f) + uint64(GOutSrc.Num()) * sizeof(uint32)) / (1024.0 * 1024.0);
		UE_LOG(LogTemp, Log, TEXT("[MeshBoolean:%s] arrangement readback cap=%d actual=%u gpuCopy=%.1fMiB cpuPrefix=%.1fMiB"),
			*GetName(), OutSubTriCap, GOutCnt[0], GPUCopyMiB, CPUCopyMiB);
	}

	// [丢面诊断] 提取阶段：QueryBox 是否框住网格 + 实际写入 soup 的三角数 vs 容量。
	// box 不含网格 → 边界剔除丢面；box 含网格但 SoupTriCount << MaxTriCap → 源网格退化三角被 extraction(dot(N,N)<=1e-8) 跳过。
	{
		const FVector QBSize = QueryBox.GetSize();
		UE_LOG(LogTemp, Warning, TEXT("[MeshBoolean:%s] [丢面诊断] QueryBox valid=%d min=(%.0f,%.0f,%.0f) max=(%.0f,%.0f,%.0f) size=(%.0f,%.0f,%.0f) | SoupTriCount(写入)=%u MaxTriCap(容量)=%d"),
			*GetName(), QueryBox.IsValid ? 1 : 0,
			QueryBox.Min.X, QueryBox.Min.Y, QueryBox.Min.Z, QueryBox.Max.X, QueryBox.Max.Y, QueryBox.Max.Z,
			QBSize.X, QBSize.Y, QBSize.Z, SoupTriangleCount, MaxTriCapacity);
	}

	UE_LOG(LogTemp, Log, TEXT("[MeshBoolean:%s] LBVH tris=%d cuts=%d (coplanar=%u degenerate=%u segOverflow=%u)%s"),
		*GetName(), TriCount, SafeCutCount, StatsData[0], StatsData[1], StatsData[3],
		StatsData[3] > 0 ? TEXT("  <<< SEG OVERFLOW: 切段被丢，共面切割不完整") : TEXT(""));

	// ---- 逐三角输出：直接消费 GPU arrangement 子三角 soup。 ----
	auto SoupVert = [&](int32 Tri, int32 K) -> FVector3d
	{
		const FVector4f& P = VertexData[Tri * 3 + K];
		return FVector3d(P.X, P.Y, P.Z);
	};
	// 源顶点 UV（3 per triangle，与 SoupVert 平行），按通道交错：[(Tri*3+K) * 通道数 + 通道]。
	// 源没有该通道时读到清零的 (0,0)。
	auto SoupUV = [&](int32 Tri, int32 K, int32 Channel) -> FVector2f
	{
		const int32 Idx = (Tri * 3 + K) * SoupUVChannels + Channel;
		return UVData.IsValidIndex(Idx) ? UVData[Idx] : FVector2f(0.0f, 0.0f);
	};
	auto SoupNormal = [&](int32 Tri, int32 K) -> FVector3f
	{
		const int32 Idx = Tri * 3 + K;
		if (!NormalData.IsValidIndex(Idx)) return FVector3f::UnitZ();
		const FVector4f& N = NormalData[Idx];
		return FVector3f(N.X, N.Y, N.Z).GetSafeNormal(UE_SMALL_NUMBER, FVector3f::UnitZ());
	};
	// 把世界点 P（位于源三角 Tri 平面内/近旁）用重心坐标插值出 UV。重心 clamp[0,1]+归一化保数值稳。
	// arrangement 生成的每个输出顶点都落在其源三角内 → 直通顶点自然得到 (1,0,0)/(0,1,0)/(0,0,1) 即直接拷贝源 UV。
	// 同一个 (源三角, 点) 的重心权重只解一次，供 UV/法线/切线/副切线/颜色五个属性复用。
	// 此前每个 corner 把这段 2x2 解重复算 5 遍，占属性重建的大头。
	auto SolveBary = [&](int32 Tri, const FVector3d& P) -> FVector3d
	{
		const FVector3d A = SoupVert(Tri, 0), B = SoupVert(Tri, 1), C = SoupVert(Tri, 2);
		const FVector3d V0 = B - A, V1 = C - A, V2 = P - A;
		const double D00 = V0.Dot(V0), D01 = V0.Dot(V1), D11 = V1.Dot(V1);
		const double D20 = V2.Dot(V0), D21 = V2.Dot(V1);
		const double Denom = D00 * D11 - D01 * D01;
		double Wb = 0.0, Wc = 0.0, Wa = 1.0;
		if (FMath::Abs(Denom) > 1e-20)
		{
			Wb = (D11 * D20 - D01 * D21) / Denom;
			Wc = (D00 * D21 - D01 * D20) / Denom;
			Wa = 1.0 - Wb - Wc;
		}
		Wa = FMath::Clamp(Wa, 0.0, 1.0); Wb = FMath::Clamp(Wb, 0.0, 1.0); Wc = FMath::Clamp(Wc, 0.0, 1.0);
		const double Sum = Wa + Wb + Wc;
		if (Sum > 1e-12) { Wa /= Sum; Wb /= Sum; Wc /= Sum; }
		else { Wa = 1.0; Wb = 0.0; Wc = 0.0; }
		return FVector3d(Wa, Wb, Wc);
	};
	auto BaryUV = [&](int32 Tri, const FVector3d& W, int32 Channel) -> FVector2f
	{
		const FVector2f Ua = SoupUV(Tri, 0, Channel), Ub = SoupUV(Tri, 1, Channel), Uc = SoupUV(Tri, 2, Channel);
		return FVector2f(
			float(W.X) * Ua.X + float(W.Y) * Ub.X + float(W.Z) * Uc.X,
			float(W.X) * Ua.Y + float(W.Y) * Ub.Y + float(W.Z) * Uc.Y);
	};
	auto BaryNormal = [&](int32 Tri, const FVector3d& W) -> FVector3f
	{
		const FVector3f Na = SoupNormal(Tri, 0), Nb = SoupNormal(Tri, 1), Nc = SoupNormal(Tri, 2);
		return (float(W.X) * Na + float(W.Y) * Nb + float(W.Z) * Nc).GetSafeNormal(UE_SMALL_NUMBER, Na);
	};
	auto BaryFloat4 = [&](const TArray<FVector4f>& Data, int32 Tri, const FVector3d& W, const FVector4f& DefaultValue) -> FVector4f
	{
		const int32 Base = Tri * 3;
		if (!Data.IsValidIndex(Base + 2)) return DefaultValue;
		return float(W.X) * Data[Base] + float(W.Y) * Data[Base + 1] + float(W.Z) * Data[Base + 2];
	};

	// 源三角平面。
	TArray<FVector3d> SrcN;  SrcN.SetNumUninitialized(TriCount);
	for (int32 t = 0; t < TriCount; ++t)
	{
		const FVector3d n = (SoupVert(t, 1) - SoupVert(t, 0)).Cross(SoupVert(t, 2) - SoupVert(t, 0));
		const double len = n.Length();
		SrcN[t] = (len > 1e-12) ? (n / len) : FVector3d(0.0, 0.0, 1.0);
	}
	const double SinSq = FMath::Square(FMath::Sin(FMath::DegreesToRadians(FMath::Clamp(CoplanarAngleDegrees, 0.0f, 45.0f))));
	const double OffEps = FMath::Max(0.0f, CoplanarOffsetEpsilon);

	TArray<FVector3f> OutputCorners;
	TArray<int32> OutputSources;
	TArray<uint32> OutputWeldRepresentatives;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MeshBoolean_GPUOutputArrayBuild);
		const int32 OutCount = FMath::Clamp<int32>(int32(GOutCnt[0]), 0, OutSubTriCap);
		OutputCorners.Reserve(OutCount * 3);
		OutputSources.Reserve(OutCount);
		if (OutputWeldDistanceV > UE_SMALL_NUMBER) OutputWeldRepresentatives.Reserve(OutCount * 3);
		for (int32 i = 0; i < OutCount; ++i)
		{
			const uint32 EncodedSource = GOutSrc[i];
			const int32 Source = int32(EncodedSource & MeshBooleanSourceMask);
			if (Source < 0 || Source >= TriCount) continue;
			// Stage B 现在只在 OutSource 上打标记而不再于 emit 时丢弃，故过滤移到这里。
			// ArrangementOnly 不跑分类 pass，全部 fragment 保留。
			if (bRunStageB && (EncodedSource & MeshBooleanSourceKeep) == 0u) continue;
			FVector3f P0 = GOutSoup[i * 3 + 0];
			FVector3f P1 = GOutSoup[i * 3 + 1];
			FVector3f P2 = GOutSoup[i * 3 + 2];
			uint32 Representatives[3] =
			{
				GOutWeldRepresentatives.IsValidIndex(i * 3 + 0)
					? GOutWeldRepresentatives[i * 3 + 0] : uint32(i * 3 + 0),
				GOutWeldRepresentatives.IsValidIndex(i * 3 + 1)
					? GOutWeldRepresentatives[i * 3 + 1] : uint32(i * 3 + 1),
				GOutWeldRepresentatives.IsValidIndex(i * 3 + 2)
					? GOutWeldRepresentatives[i * 3 + 2] : uint32(i * 3 + 2)
			};
			// 绕序约定（务必与 UE 一致，否则整个输出网格法线朝内）：
			// UE 是左手坐标系 + 逆时针绕序，StaticMesh 对角点序 (0,1,2) 的正面法线取的是
			// **反向**叉积 cross(P2-P0, P1-P0)（见引擎 StaticMeshOperations.cpp
			// ComputeTriangleTangentsAndNormals：`CrossProduct(DPosition2, DPosition1)`）。
			// 而 compute shader 侧的 soup 用的是常规右手叉积 cross(P1-P0, P2-P0) 对齐源法线，
			// 两者刚好差一个负号。所以这里要让 UE 口径的法线（= -Facing）与源法线同向，
			// 即 Facing 与源法线**反**向时才是正确绕序，与 GeometricNormal 的算法保持一致。
			const FVector3d Facing = FVector3d(P1 - P0).Cross(FVector3d(P2 - P0));
			if (!FMath::IsFinite(Facing.SquaredLength()) || Facing.SquaredLength() < 1e-24) continue;
			if (Facing.Dot(SrcN[Source]) > 0.0)
			{
				Swap(P1, P2);
				Swap(Representatives[1], Representatives[2]);
			}
			OutputCorners.Add(P0);
			OutputCorners.Add(P1);
			OutputCorners.Add(P2);
			OutputSources.Add(Source);
			if (OutputWeldDistanceV > UE_SMALL_NUMBER)
			{
				OutputWeldRepresentatives.Add(Representatives[0]);
				OutputWeldRepresentatives.Add(Representatives[1]);
				OutputWeldRepresentatives.Add(Representatives[2]);
			}
		}
		UE_LOG(LogTemp, Log, TEXT("[MeshBoolean:%s] [GPU-arr] outSubTris=%d tris=%d cuts=%d degen=%u limited=%u cutLimit=%u vertLimit=%u cellLimit=%u outputLimit=%u scratchOverflow=%u partialSplitCommit=%u areaMismatch=%u maxSeg=%u maxVerts=%u maxCells=%u maxOutputPerSource=%u outOverflow=%u"),
			*GetName(), OutputSources.Num(), TriCount, SafeCutCount, GStat[BSPStatDegenerate],
			GStat[BSPStatLimitedTriangles], GStat[BSPStatCutLimit], GStat[BSPStatVertLimit], GStat[BSPStatCellLimit],
			GStat[BSPStatOutputPerSourceLimit], GStat[BSPStatScratchOverflow], GStat[BSPStatPartialSplitCommit],
			GStat[BSPStatAreaMismatch], GStat[BSPStatMaxSegments], GStat[BSPStatMaxVertices], GStat[BSPStatMaxCells],
			GStat[BSPStatMaxOutputTrianglesPerSource], GOutCnt[1]);
	}

	if (OutputSources.IsEmpty()) return nullptr;

	// 诊断：统计 GPU arrangement 后仍共面重叠的输出三角对。
	int32 OverlapSame = 0, OverlapCross = 0;
	{
			TArray<FVector3d> P0, P1, P2, Cen, Nrm;
			TArray<int32> Src;
			for (int32 TriangleIndex = 0; TriangleIndex < OutputSources.Num(); ++TriangleIndex)
			{
				const FVector3d a(OutputCorners[TriangleIndex * 3 + 0]);
				const FVector3d b(OutputCorners[TriangleIndex * 3 + 1]);
				const FVector3d cc(OutputCorners[TriangleIndex * 3 + 2]);
				FVector3d n = (b - a).Cross(cc - a); const double l = n.Length();
				if (l < 1e-12) continue;
				P0.Add(a); P1.Add(b); P2.Add(cc); Cen.Add((a + b + cc) / 3.0); Nrm.Add(n / l);
				Src.Add(OutputSources[TriangleIndex]);
			}
			const int32 M = Cen.Num();
			auto Inside = [](const FVector3d& p, const FVector3d& a, const FVector3d& b, const FVector3d& cc, const FVector3d& n) -> bool
			{
				const double d0 = n.Dot((b - a).Cross(p - a));
				const double d1 = n.Dot((cc - b).Cross(p - b));
				const double d2 = n.Dot((a - cc).Cross(p - cc));
				return (d0 >= -1e-4 && d1 >= -1e-4 && d2 >= -1e-4) || (d0 <= 1e-4 && d1 <= 1e-4 && d2 <= 1e-4);
			};
			if (M <= 6000)
			{
				for (int32 i = 0; i < M; ++i)
					for (int32 j = i + 1; j < M; ++j)
					{
						if (Nrm[i].Cross(Nrm[j]).SquaredLength() > SinSq) continue;
						if (FMath::Abs((Cen[j] - Cen[i]).Dot(Nrm[i])) > OffEps) continue;
						if (Inside(Cen[i], P0[j], P1[j], P2[j], Nrm[j]) || Inside(Cen[j], P0[i], P1[i], P2[i], Nrm[i]))
						{
							if (Src[i] == Src[j]) ++OverlapSame; else ++OverlapCross;
						}
					}
			}
			else { OverlapSame = -1; OverlapCross = -1; } // 输出过多，跳过 O(m²)
		}

	UE_LOG(LogTemp, Log, TEXT("[MeshBoolean:%s] GPU arrangement kept=%d, remaining-overlap same-src=%d cross-src=%d"),
		*GetName(), OutputSources.Num(), OverlapSame, OverlapCross);


	UE_LOG(LogTemp, Log, TEXT("[MeshBoolean:%s] StageB standalone classify/rescue passes: enabled=%d threshold=%.3f expansion=%.3f emittedFragments=%u keptTriangles=%d"),
		*GetName(), bRunStageB ? 1 : 0, StageBWindingThresholdV,
		StageBExpansionDistanceV, GOutCnt[0], OutputSources.Num());

	const float EffectiveWeldDistance = FMath::Max(0.0f, VertexWeldDistance);
	const FVector4f DefaultColor(1.0f, 1.0f, 1.0f, 1.0f);
	TArray<FVector3f> FinalPositions = OutputCorners;
	TArray<uint32> FinalIndices;
	TArray<int32> FinalSources = OutputSources;
	FinalIndices.SetNumUninitialized(FinalPositions.Num());
	for (int32 Corner = 0; Corner < FinalIndices.Num(); ++Corner) FinalIndices[Corner] = uint32(Corner);

	if (EffectiveWeldDistance > UE_SMALL_NUMBER && !OutputSources.IsEmpty())
	{
		// Stage 13（可选 CPU 后处理）：按 GPU 代表元重建索引，并移除焊接产生的退化面和重复面。
		TRACE_CPUPROFILER_EVENT_SCOPE(MeshBoolean_VertexWeldRebuild);
		const TArray<uint32>& CornerRepresentatives = OutputWeldRepresentatives;
		if (CornerRepresentatives.Num() == OutputCorners.Num())
		{
			FinalPositions.Reset();
			FinalIndices.Reset();
			FinalSources.Reset();
			FinalIndices.Reserve(OutputCorners.Num());
			FinalSources.Reserve(OutputSources.Num());

			TMap<uint32, int32> RepresentativeToVertex;
			TSet<FIntVector> SeenTriangles;
			int32 RemovedDegenerate = 0;
			int32 RemovedDuplicate = 0;
			auto RepresentativePosition = [&](uint32 Representative, const FVector3f& Fallback) -> FVector3f
			{
				return GOutSoup.IsValidIndex(int32(Representative))
					? GOutSoup[int32(Representative)]
					: Fallback;
			};
			auto FindOrAppendVertex = [&](uint32 Representative, const FVector3f& Position) -> uint32
			{
				if (const int32* ExistingVertex = RepresentativeToVertex.Find(Representative)) return uint32(*ExistingVertex);
				const int32 NewVertex = FinalPositions.Add(Position);
				RepresentativeToVertex.Add(Representative, NewVertex);
				return uint32(NewVertex);
			};

			for (int32 TriangleIndex = 0; TriangleIndex < OutputSources.Num(); ++TriangleIndex)
			{
				const int32 CornerBase = TriangleIndex * 3;
				uint32 Representatives[3] =
				{
					CornerRepresentatives[CornerBase + 0],
					CornerRepresentatives[CornerBase + 1],
					CornerRepresentatives[CornerBase + 2]
				};
				for (int32 Corner = 0; Corner < 3; ++Corner) if (!GOutSoup.IsValidIndex(int32(Representatives[Corner]))) Representatives[Corner] = uint32(GOutSoup.Num() + CornerBase + Corner);
				if (Representatives[0] == Representatives[1] || Representatives[1] == Representatives[2]
					|| Representatives[0] == Representatives[2])
				{
					++RemovedDegenerate;
					continue;
				}

				const FVector3f WeldedPositions[3] =
				{
					RepresentativePosition(Representatives[0], OutputCorners[CornerBase + 0]),
					RepresentativePosition(Representatives[1], OutputCorners[CornerBase + 1]),
					RepresentativePosition(Representatives[2], OutputCorners[CornerBase + 2])
				};
				const FVector3d P0(WeldedPositions[0]);
				const FVector3d P1(WeldedPositions[1]);
				const FVector3d P2(WeldedPositions[2]);
				const double AreaVectorSquared = (P1 - P0).Cross(P2 - P0).SquaredLength();
				if (!FMath::IsFinite(AreaVectorSquared) || AreaVectorSquared <= 1e-24)
				{
					++RemovedDegenerate;
					continue;
				}

				int32 Sorted[3] = { int32(Representatives[0]), int32(Representatives[1]), int32(Representatives[2]) };
				if (Sorted[0] > Sorted[1]) Swap(Sorted[0], Sorted[1]);
				if (Sorted[1] > Sorted[2]) Swap(Sorted[1], Sorted[2]);
				if (Sorted[0] > Sorted[1]) Swap(Sorted[0], Sorted[1]);
				if (SeenTriangles.Contains(FIntVector(Sorted[0], Sorted[1], Sorted[2])))
				{
					++RemovedDuplicate;
					continue;
				}
				SeenTriangles.Add(FIntVector(Sorted[0], Sorted[1], Sorted[2]));

				for (int32 Corner = 0; Corner < 3; ++Corner) FinalIndices.Add(FindOrAppendVertex(Representatives[Corner], WeldedPositions[Corner]));
				FinalSources.Add(OutputSources[TriangleIndex]);
			}

			const double ReductionPercent = OutputCorners.IsEmpty()
				? 0.0
				: 100.0 * (1.0 - double(FinalPositions.Num()) / double(OutputCorners.Num()));
			UE_LOG(LogTemp, Log, TEXT("[MeshBoolean:%s] GPU vertex weld distance=%.6fcm corners=%d vertices=%d reduction=%.2f%% tris=%d degenerateRemoved=%d duplicateRemoved=%d"),
				*GetName(), EffectiveWeldDistance, OutputCorners.Num(), FinalPositions.Num(), ReductionPercent,
				FinalSources.Num(), RemovedDegenerate, RemovedDuplicate);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[MeshBoolean:%s] GPU vertex weld failed; preserving the unwelded final mesh"), *GetName());
		}
	}

	if (FinalSources.IsEmpty()) return nullptr;

	// Stage 14：焊接可能改变角点位置；按源面法线恢复每个保留碎片的绕序，确保属性与索引一致。
	FMeshBooleanOrientationStats WorldOrientationStats;
	// 焊接会挪动角点，故焊接后必须重验绕序；未焊接时 FinalPositions/Indices 与 OutputCorners
	// 逐一对应，而那批数据在输出重建时已按 SrcN 翻转过，这里再扫一遍必然零修正。
	const bool bNeedOrientationValidation = EffectiveWeldDistance > UE_SMALL_NUMBER;
	if (bNeedOrientationValidation)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MeshBoolean_SourceOrientationValidation);
		for (int32 TriangleIndex = 0; TriangleIndex < FinalSources.Num(); ++TriangleIndex)
		{
			const int32 Source = FinalSources[TriangleIndex];
			if (!SrcN.IsValidIndex(Source) || SrcN[Source].SquaredLength() <= UE_DOUBLE_SMALL_NUMBER)
			{
				++WorldOrientationStats.MissingSources;
				continue;
			}
			const int32 CornerBase = TriangleIndex * 3;
			const FVector3d P0(FinalPositions[FinalIndices[CornerBase + 0]]);
			const FVector3d P1(FinalPositions[FinalIndices[CornerBase + 1]]);
			const FVector3d P2(FinalPositions[FinalIndices[CornerBase + 2]]);
			// 同上：UE 正面法线 = cross(P2-P0, P1-P0) = -cross(P1-P0, P2-P0)。这里用常规右手
			// 叉积算 Alignment，因此「与源法线同向」反而说明绕序反了，需要交换后两个角点。
			const double Alignment = (P1 - P0).Cross(P2 - P0).Dot(SrcN[Source]);
			if (FMath::IsFinite(Alignment) && Alignment > 0.0)
			{
				Swap(FinalIndices[CornerBase + 1], FinalIndices[CornerBase + 2]);
				++WorldOrientationStats.Corrections;
			}
			const FVector3d Q1(FinalPositions[FinalIndices[CornerBase + 1]]);
			const FVector3d Q2(FinalPositions[FinalIndices[CornerBase + 2]]);
			const double FinalAlignment = (Q1 - P0).Cross(Q2 - P0).Dot(SrcN[Source]);
			if (!FMath::IsFinite(FinalAlignment) || FinalAlignment > 0.0) ++WorldOrientationStats.ResidualMismatches;
		}
	}
	UE_LOG(LogTemp, Log, TEXT("[MeshBoolean:%s] source orientation world: corrected=%d residualMismatch=%d missingSource=%d tris=%d"),
		*GetName(), WorldOrientationStats.Corrections, WorldOrientationStats.ResidualMismatches,
		WorldOrientationStats.MissingSources, FinalSources.Num());

	// Stage 15：插值或重建法线、切线、UV、颜色和材质，组装最终 Static Mesh 数据。
	FCSGpuMeshCPUData StaticMeshData;
	StaticMeshData.Positions = MoveTemp(FinalPositions);
	StaticMeshData.Indices = MoveTemp(FinalIndices);
	StaticMeshData.Normals.SetNumUninitialized(StaticMeshData.Indices.Num());
	StaticMeshData.Tangents.SetNumUninitialized(StaticMeshData.Indices.Num());
	// 源模型有几条 UV 就输出几条：通道数直接继承 soup，源只有 1 条时退化成单通道。
	const int32 OutputUVChannels = FMath::Clamp(SoupUVChannels, 1, FCSGpuMeshCPUData::MaxTexCoordChannels);
	StaticMeshData.NumTexCoordChannels = OutputUVChannels;
	for (int32 Channel = 0; Channel < OutputUVChannels; ++Channel)
		StaticMeshData.TexCoordChannels[Channel].SetNumUninitialized(StaticMeshData.Indices.Num());
	StaticMeshData.Colors.SetNumUninitialized(StaticMeshData.Indices.Num());
	StaticMeshData.BinormalSigns.SetNumUninitialized(StaticMeshData.Indices.Num());
	// 布尔输出是世界空间、逐角点属性（焊接后的顶点仍要保留各自的 UV/法线/颜色接缝）。
	// 显式标注，转换段据此决定是否烘到局部空间，不再靠数组长度猜。
	StaticMeshData.SourceSpace = FCSGpuMeshCPUData::ESpace::World;
	StaticMeshData.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerCorner;

	// 每个三角只写自己那 3 个 corner 槽位，彼此不重叠，可直接并行。
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MeshBoolean_StaticMeshAttributeBuild);
		ParallelFor(FinalSources.Num(), [&](int32 TriangleIndex)
		{
			const int32 Source = FinalSources[TriangleIndex];
			const int32 CornerBase = TriangleIndex * 3;
			const FVector3f P[3] =
			{
				StaticMeshData.Positions[StaticMeshData.Indices[CornerBase + 0]],
				StaticMeshData.Positions[StaticMeshData.Indices[CornerBase + 1]],
				StaticMeshData.Positions[StaticMeshData.Indices[CornerBase + 2]]
			};
			// UE uses counter-clockwise winding in a left-handed coordinate system, so the
			// geometric normal for StaticMesh attributes requires the reversed cross product.
			// 与引擎 StaticMeshOperations.cpp 的 CrossProduct(DPosition2, DPosition1) 同一口径；
			// 上面输出重建的绕序判定也按这个口径取反，两处必须同时改，否则网格整体翻面。
			const bool bHasSource = Source >= 0 && Source < TriCount;
			// 半球判据必须稳：布尔切出的窄条/尖角碎片上，这个 float32 世界坐标叉积会因灾难性
			// 抵消而方向失真，模平方过小时更会 fallback 成 UnitZ()。此时逐角点的
			// dot(Normal, ref) < 0 就成了近似随机的判据，三个角点里只有个别被误判取反 ——
			// 表现为少数三角的某一个顶点法线朝里、该角出现异常暗部。
			// SrcN 是源三角的 double 精度法线，每个源三角只算一次且不受碎片退化影响；
			// 绕序修复后 GeometricNormal 与 SrcN 同向，换成它不改变语义，只是更稳。
			const FVector3f FragmentNormal = FVector3f::CrossProduct(P[2] - P[0], P[1] - P[0])
				.GetSafeNormal(UE_SMALL_NUMBER, FVector3f::UnitZ());
			const bool bStableSourceNormal = bHasSource
				&& SrcN.IsValidIndex(Source)
				&& SrcN[Source].SquaredLength() > UE_DOUBLE_SMALL_NUMBER;
			const FVector3f GeometricNormal = bStableSourceNormal
				? FVector3f(SrcN[Source]).GetSafeNormal(UE_SMALL_NUMBER, FragmentNormal)
				: FragmentNormal;
			// 每个 corner 的重心权重解一次，下面 UV/法线/切线/副切线/颜色全部复用。
			FVector3d BaryWeights[3];
			for (int32 Corner = 0; Corner < 3; ++Corner)
				BaryWeights[Corner] = bHasSource ? SolveBary(Source, FVector3d(P[Corner])) : FVector3d(1.0, 0.0, 0.0);
			// 通道 0 另外留一份给切线重算用；其余通道只写进各自的输出数组。
			FVector2f UVs[3];
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				UVs[Corner] = bHasSource ? BaryUV(Source, BaryWeights[Corner], 0) : FVector2f::ZeroVector;
				StaticMeshData.TexCoords()[CornerBase + Corner] = UVs[Corner];
			}
			for (int32 Channel = 1; Channel < OutputUVChannels; ++Channel)
			{
				TArray<FVector2f>& ChannelUVs = StaticMeshData.TexCoordChannels[Channel];
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					ChannelUVs[CornerBase + Corner] = bHasSource
						? BaryUV(Source, BaryWeights[Corner], Channel)
						: FVector2f::ZeroVector;
				}
			}

			FVector3f RecomputedTangent = P[1] - P[0];
			FVector3f RecomputedBiTangent = FVector3f::CrossProduct(GeometricNormal, RecomputedTangent);
			const FVector2f DeltaUV1 = UVs[1] - UVs[0];
			const FVector2f DeltaUV2 = UVs[2] - UVs[0];
			const float UVDenominator = DeltaUV1.X * DeltaUV2.Y - DeltaUV1.Y * DeltaUV2.X;
			if (FMath::Abs(UVDenominator) > UE_SMALL_NUMBER)
			{
				const float InvUV = 1.0f / UVDenominator;
				RecomputedTangent = ((P[1] - P[0]) * DeltaUV2.Y - (P[2] - P[0]) * DeltaUV1.Y) * InvUV;
				RecomputedBiTangent = ((P[2] - P[0]) * DeltaUV1.X - (P[1] - P[0]) * DeltaUV2.X) * InvUV;
			}

			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const FVector3d& W = BaryWeights[Corner];
				const bool bUseGeometric = !bHasSource;
				FVector3f Normal = bUseGeometric ? GeometricNormal : BaryNormal(Source, W);
				// 源法线原样保留，不做半球校正。那段校正是为了掩盖输出绕序的 bug 而加的：
				// 绕序修好前它每次触发约 450 万次，修好后只剩 7 千次，剩下的多半是作者有意的
				// 背面法线（双面卡片、硬边烘焙、外扩 shell）或近切向法线 —— 翻转它们是破坏而非修复，
				// 而且是逐角点翻转，会在三角内部造成着色断裂。零长度法线仍由下面的兜底处理。
				Normal = Normal.GetSafeNormal(UE_SMALL_NUMBER, GeometricNormal);
				FVector3f Tangent;
				FVector3f BiTangent;
				if (bUseGeometric)
				{
					Tangent = RecomputedTangent;
					BiTangent = RecomputedBiTangent;
				}
				else
				{
					const FVector4f Tangent4 = BaryFloat4(TangentData, Source, W, FVector4f(1, 0, 0, 0));
					const FVector4f BiTangent4 = BaryFloat4(BiTangentData, Source, W, FVector4f(0, 1, 0, 0));
					Tangent = FVector3f(Tangent4.X, Tangent4.Y, Tangent4.Z);
					BiTangent = FVector3f(BiTangent4.X, BiTangent4.Y, BiTangent4.Z);
				}

				Tangent = (Tangent - FVector3f::DotProduct(Tangent, Normal) * Normal).GetSafeNormal();
				if (Tangent.IsNearlyZero())
				{
					const FVector3f Axis = FMath::Abs(Normal.Z) < 0.9f ? FVector3f::UnitZ() : FVector3f::UnitX();
					Tangent = FVector3f::CrossProduct(Axis, Normal).GetSafeNormal();
				}
				if (BiTangent.IsNearlyZero()) BiTangent = FVector3f::CrossProduct(Normal, Tangent);

				StaticMeshData.Normals[CornerBase + Corner] = Normal;
				StaticMeshData.Tangents[CornerBase + Corner] = Tangent;
				StaticMeshData.BinormalSigns[CornerBase + Corner] =
					FVector3f::DotProduct(FVector3f::CrossProduct(Normal, Tangent), BiTangent) < 0.0f ? -1.0f : 1.0f;
				StaticMeshData.Colors[CornerBase + Corner] =
					bHasSource ? BaryFloat4(ColorData, Source, W, DefaultColor) : DefaultColor;
			}
		});
	}
	TArray<UMaterialInterface*> OutputMaterialSlots;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MeshBoolean_MaterialAttributeBuild);
		TMap<int32, int32> RegistryToSlot;
		TSet<int32> UsedMaterialSlots;
		int32 MissingMaterialSources = 0;
		int32 InvalidMaterialRegistryIds = 0;
		StaticMeshData.TriangleMaterialSlots.Reserve(FinalSources.Num());
		for (const int32 Source : FinalSources)
		{
			if (Source < 0 || Source >= TriCount) ++MissingMaterialSources;
			uint32 RegistryId = MaterialIds.IsValidIndex(Source) ? MaterialIds[Source] : CS_NO_MATERIAL_ID;
			if (RegistryId != CS_NO_MATERIAL_ID && RegistryId >= uint32(Prepared.GetMaterialRegistryNum()))
			{
				++InvalidMaterialRegistryIds;
				RegistryId = CS_NO_MATERIAL_ID;
			}
			const int32 RegistryKey = RegistryId == CS_NO_MATERIAL_ID ? INDEX_NONE : int32(RegistryId);
			int32 Slot = 0;
			if (const int32* ExistingSlot = RegistryToSlot.Find(RegistryKey)) Slot = *ExistingSlot;
			else
			{
				UMaterialInterface* Material = RegistryId == CS_NO_MATERIAL_ID
					? nullptr
					: Prepared.GetMaterialByRegistryIndex(int32(RegistryId));
				Slot = OutputMaterialSlots.Add(Material);
				RegistryToSlot.Add(RegistryKey, Slot);
			}
			StaticMeshData.TriangleMaterialSlots.Add(Slot);
			UsedMaterialSlots.Add(Slot);
		}
		if (OutputMaterialSlots.IsEmpty()) OutputMaterialSlots.Add(nullptr);
		auto LogMaterials = [&]()
		{
			UE_LOG(LogTemp, Log, TEXT("[MeshBoolean:%s] materials: slots=%d usedSlots=%d assigned=%d tris=%d registry=%d missingSource=%d invalidRegistry=%d"),
				*GetName(), OutputMaterialSlots.Num(), UsedMaterialSlots.Num(), StaticMeshData.TriangleMaterialSlots.Num(),
				FinalSources.Num(), Prepared.GetMaterialRegistryNum(), MissingMaterialSources, InvalidMaterialRegistryIds);
		};
		auto LogMaterialWarnings = [&]()
		{
			UE_LOG(LogTemp, Warning, TEXT("[MeshBoolean:%s] materials: slots=%d usedSlots=%d assigned=%d tris=%d registry=%d missingSource=%d invalidRegistry=%d"),
				*GetName(), OutputMaterialSlots.Num(), UsedMaterialSlots.Num(), StaticMeshData.TriangleMaterialSlots.Num(),
				FinalSources.Num(), Prepared.GetMaterialRegistryNum(), MissingMaterialSources, InvalidMaterialRegistryIds);
		};
		if (MissingMaterialSources == 0 && InvalidMaterialRegistryIds == 0) LogMaterials();
		else LogMaterialWarnings();
	}

	// 输出是 StaticMesh 资产，与 DynamicMesh 组件无关，直接用 actor 变换把世界空间结果烘到局部空间。
	const FTransform OutputTransform = GetActorTransform();
	// 统一走公用转换入口：属性装配与落盘的策略（绕序、退化面阈值、空槽兜底默认材质）都在
	// CSGpuMeshConvert 里，不再由各产出路径各写一份。
	// 结果落盘为 level 同级 AutoResult 文件夹（/<level 目录>/AutoResult/SM_<actor>_<稳定编号>），建完标脏，
	// 由用户自行 Save All 决定是否写盘。名字里不带每次运行的时间戳（命名规则与 CSSW 烘焙一致），
	// 同一个 actor 反复运行始终写同一个资产，直接覆盖旧模型，引用它的组件仍指向同一份资产。
	// 非编辑器构建没有资产系统，公用入口内部退回 transient。
	CSGpuMeshConvert::FConvertOptions ConvertOptions;
	ConvertOptions.TargetTransform = OutputTransform;
	ConvertOptions.bBakeToLocalSpace = true;

	CSGpuMeshConvert::FAssetOptions AssetOptions;
	// 布尔结果动辄百万级三角，正是 Nanite 的适用场景：交给它做 LOD 与剔除，
	// 省掉手工 LOD，渲染开销与三角数基本脱钩。
	AssetOptions.bEnableNanite = bOutputNanite;
#if WITH_EDITOR
	AssetOptions.AssetPath = BuildResultAssetPath();
#else
	AssetOptions.bTransient = true;
#endif
	OutputStaticMesh = CSGpuMeshConvert::BuildStaticMesh(
		this, this, StaticMeshData, OutputMaterialSlots, ConvertOptions, AssetOptions);
	if (!OutputStaticMesh && !AssetOptions.bTransient)
	{
		UE_LOG(LogTemp, Warning, TEXT("[MeshBoolean:%s] result 资产保存失败，回退为 transient StaticMesh。"), *GetName());
		AssetOptions.bTransient = true;
		OutputStaticMesh = CSGpuMeshConvert::BuildStaticMesh(
			this, this, StaticMeshData, OutputMaterialSlots, ConvertOptions, AssetOptions);
	}
	if (!OutputStaticMesh)
	{
		UE_LOG(LogTemp, Error, TEXT("[MeshBoolean:%s] shared GPU-mesh StaticMesh conversion failed (vertices=%d indices=%d triangles=%d)."),
			*GetName(), StaticMeshData.Positions.Num(), StaticMeshData.Indices.Num(), FinalSources.Num());
	}

	return OutputStaticMesh;
}


// [M4c] 多级 exclusive 前缀和：InSRV(N uint) → OutUAV(N uint)。块数>512 时递归扫「块总和」，支持任意 N
// （单级 ScanBlockSums 只能扫 <=512 块 = 262144 元素；C1 的 5.67M 需 2 级）。ScanBlocks/AddOffsets 的组数
// = ceil(N/512) 对 5.67M 是 11079 < 65535，不用 wrap；只有 ScanBlockSums 是单组、须靠递归把块数降到 <=512。
static void AddExclusiveScan(FRDGBuilder& GraphBuilder, FRDGBufferSRVRef InSRV, FRDGBufferUAVRef OutUAV, int32 N)
{
	if (N <= 0) return;
	const int32 Blocks = FMath::DivideAndRoundUp(N, 512);
	FRDGBufferRef BlockSumsBuf; FRDGBufferUAVRef BlockSumsUAV; FRDGBufferSRVRef BlockSumsSRV;
	CSHelper::CreateClearedTypedBuffer(GraphBuilder, BlockSumsBuf, BlockSumsUAV, BlockSumsSRV, sizeof(uint32), Blocks, PF_R32_UINT, TEXT("MB.Scan.BlockSums"), 0u);
	{
		FScanBlocksCS::FParameters* P = GraphBuilder.AllocParameters<FScanBlocksCS::FParameters>();
		P->GCutCountSRV = InSRV; P->RW_CutOffset = OutUAV; P->RW_BlockSums = BlockSumsUAV; P->GScanCount = uint32(N);
		TShaderMapRef<FScanBlocksCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.Scan.Blocks"), S, P, FIntVector(Blocks, 1, 1));
	}
	FRDGBufferSRVRef BlockOffsetsSRV = BlockSumsSRV;  // 默认：块偏移就地存回 BlockSums（<=512 分支）
	if (Blocks <= 512)
	{
		FRDGBufferRef TotalBuf; FRDGBufferUAVRef TotalUAV;
		CSHelper::CreateClearedTypedBuffer(GraphBuilder, TotalBuf, TotalUAV, sizeof(uint32), 1, PF_R32_UINT, TEXT("MB.Scan.Total"), 0u);
		FScanBlockSumsCS::FParameters* P = GraphBuilder.AllocParameters<FScanBlockSumsCS::FParameters>();
		P->RW_BlockSums = BlockSumsUAV; P->RW_ScanTotal = TotalUAV; P->GNumBlocks = uint32(Blocks);  // 就地 RMW（单 UAV，无 SRV/UAV 别名）
		TShaderMapRef<FScanBlockSumsCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.Scan.BlockSums"), S, P, FIntVector(1, 1, 1));
	}
	else
	{
		// 递归：把块总和做 exclusive 前缀和，写到**独立** buffer。不能就地(In==Out)：递归里 ScanBlocksCS
		// 会把同一 buffer 既绑 SRV 又绑 UAV，RDG/D3D12 同 pass 资源状态冲突 → ensure/device removed。
		FRDGBufferRef BlockScanBuf; FRDGBufferUAVRef BlockScanUAV;
		CSHelper::CreateClearedTypedBuffer(GraphBuilder, BlockScanBuf, BlockScanUAV, sizeof(uint32), Blocks, PF_R32_UINT, TEXT("MB.Scan.BlockScan"), 0u);
		AddExclusiveScan(GraphBuilder, BlockSumsSRV, BlockScanUAV, Blocks);
		BlockOffsetsSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(BlockScanBuf, PF_R32_UINT));
	}
	{
		FAddOffsetsCS::FParameters* P = GraphBuilder.AllocParameters<FAddOffsetsCS::FParameters>();
		P->GBlockSumsSRV = BlockOffsetsSRV; P->RW_CutOffset = OutUAV; P->GScanCount = uint32(N);
		TShaderMapRef<FAddOffsetsCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.Scan.AddOffsets"), S, P, FIntVector(Blocks, 1, 1));
	}
}

// [M4] 常驻缓冲 arrangement：CSR 分组（CountCuts→多级块扫描→ScatterCuts）+ N 段 BSP 重三角化，
// 全在同一 FRDGBuilder 上、读常驻 soup/cut buffer（无上传）。核内靠 GCutTotal[0]（常驻 cut 数）+ GBSPTriBase/Count +
// 退化 guard 自限。**任意规模**：CountCuts/Scatter/BSPN dispatch 用 GetGroupCountWrapped（>65535 组 2D 包裹，
// shader 端 GetUnWrappedDispatchThreadId）；前缀和用 AddExclusiveScan 多级（去 262144 上限）；BSP 凸胞 scratch
// 按源三角区间分块复用，每批清零 slot counter，OutCounter/BSPStats 跨批累计。产出 Out* 由出参返回。
static void AddArrangementToRDG(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef SoupSRV, FRDGBufferSRVRef ReferenceFlagsSRV,
	FRDGBufferSRVRef CutP0SRV, FRDGBufferSRVRef CutP1SRV, FRDGBufferSRVRef CutCounterSRV,
	int32 TriCapacity, int32 CutCapacity,
	const FVector3f& SnapOrigin, float SnapQuantum,
	int32 ScratchBatchSize, int32 OutCap, const FMeshBooleanStageBRDGContext& StageB,
	FRDGBufferRef& OutSoupBuf, FRDGBufferRef& OutSourceBuf, FRDGBufferRef& OutCounterBuf, FRDGBufferRef& OutBSPStatsBuf)
{
	const int32 CsrCap = FMath::Max(1, 2 * CutCapacity);
	const int32 ScrCap = FMath::Clamp(ScratchBatchSize, 1, FMath::Max(1, TriCapacity));
	const int32 CellVertCount = FMath::Max(1, ScrCap * BSPMaxCells * BSPMaxVerts);
	const int32 CellVNCount = FMath::Max(1, ScrCap * BSPMaxCells);

	FRDGBufferRef CutCountBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), TriCapacity), TEXT("MB.Arr.CutCount"));
	FRDGBufferRef CutOffsetBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), TriCapacity), TEXT("MB.Arr.CutOffset"));
	FRDGBufferRef CutFillBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), TriCapacity), TEXT("MB.Arr.CutFill"));
	FRDGBufferRef CutCSRBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), CsrCap), TEXT("MB.Arr.CutCSR"));
	// CellVerts 用 structured（非 typed），避免 typed-buffer 元素数限制；容量只随单批源三角数增长。
	FRDGBufferRef CellVertsBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector2f), CellVertCount), TEXT("MB.Arr.CellVerts"));
	FRDGBufferRef CellVNBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), CellVNCount), TEXT("MB.Arr.CellVN"));
	FRDGBufferRef ScratchCntBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("MB.Arr.ScratchCounter"));
	FRDGBufferRef LimitedReasonsBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), TriCapacity), TEXT("MB.Arr.LimitedReasons"));
	OutSoupBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), OutCap * 3), TEXT("MB.Arr.OutSoup"));
	OutSourceBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), OutCap), TEXT("MB.Arr.OutSource"));
	OutCounterBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 2), TEXT("MB.Arr.OutCounter"));
	OutBSPStatsBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), BSPStatCount), TEXT("MB.Arr.BSPStats"));

	FRDGBufferUAVRef CutCountUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CutCountBuf, PF_R32_UINT));
	FRDGBufferUAVRef CutOffsetUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CutOffsetBuf, PF_R32_UINT));
	FRDGBufferUAVRef CutFillUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CutFillBuf, PF_R32_UINT));
	FRDGBufferUAVRef CutCSRUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CutCSRBuf, PF_R32_UINT));
	FRDGBufferUAVRef CellVertsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CellVertsBuf));  // structured UAV（无 format）
	FRDGBufferUAVRef CellVNUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CellVNBuf, PF_R32_UINT));
	FRDGBufferUAVRef ScratchCntUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(ScratchCntBuf, PF_R32_UINT));
	FRDGBufferUAVRef LimitedReasonsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(LimitedReasonsBuf, PF_R32_UINT));
	FRDGBufferUAVRef OutSoupUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutSoupBuf));
	FRDGBufferUAVRef OutSrcUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutSourceBuf, PF_R32_UINT));
	FRDGBufferUAVRef OutCntUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutCounterBuf, PF_R32_UINT));
	FRDGBufferUAVRef BSPStatUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutBSPStatsBuf, PF_R32_UINT));
	AddClearUAVPass(GraphBuilder, CutCountUAV, 0u);
	AddClearUAVPass(GraphBuilder, CutOffsetUAV, 0u);
	AddClearUAVPass(GraphBuilder, CutFillUAV, 0u);
	AddClearUAVPass(GraphBuilder, CutCSRUAV, 0xFFFFFFFFu);
	AddClearUAVPass(GraphBuilder, LimitedReasonsUAV, 0u);
	AddClearUAVPass(GraphBuilder, OutSrcUAV, 0xFFFFFFFFu);
	AddClearUAVPass(GraphBuilder, OutCntUAV, 0u);
	AddClearUAVPass(GraphBuilder, BSPStatUAV, 0u);

	FRDGBufferSRVRef CutCountSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CutCountBuf, PF_R32_UINT));
	FRDGBufferSRVRef CutOffsetSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CutOffsetBuf, PF_R32_UINT));
	FRDGBufferSRVRef CutCSRSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CutCSRBuf, PF_R32_UINT));

	// dispatch 组数用 wrapped（>65535 组时 2D 包裹；<=65535 时与 1D 等价）
	const FIntVector CutGroups = FComputeShaderUtils::GetGroupCountWrapped(FMath::Max(1, CutCapacity), 64);

	{
		FCountCutsCS::FParameters* P = GraphBuilder.AllocParameters<FCountCutsCS::FParameters>();
		P->GCutP0 = CutP0SRV; P->GCutP1 = CutP1SRV; P->GCutTotal = CutCounterSRV;
		P->RW_CutCount = CutCountUAV; P->GTriCap = uint32(TriCapacity); P->GCutCap = uint32(CutCapacity);
		TShaderMapRef<FCountCutsCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.Arr.Count"), S, P, CutGroups);
	}
	// per-tri 段计数 → CSR 偏移（多级 exclusive 前缀和，任意 TriCapacity）
	AddExclusiveScan(GraphBuilder, CutCountSRV, CutOffsetUAV, TriCapacity);
	{
		FScatterCutsCS::FParameters* P = GraphBuilder.AllocParameters<FScatterCutsCS::FParameters>();
		P->GCutP0 = CutP0SRV; P->GCutP1 = CutP1SRV; P->GCutTotal = CutCounterSRV; P->GCutOffsetSRV = CutOffsetSRV;
		P->RW_CutFill = CutFillUAV; P->RW_CutCSR = CutCSRUAV; P->GTriCap = uint32(TriCapacity); P->GCutCap = uint32(CutCapacity);
		TShaderMapRef<FScatterCutsCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.Arr.Scatter"), S, P, CutGroups);
	}
	for (int32 TriBase = 0; TriBase < TriCapacity; TriBase += ScrCap)
	{
		const int32 BatchCount = FMath::Min(ScrCap, TriCapacity - TriBase);
		AddClearUAVPass(GraphBuilder, ScratchCntUAV, 0u);
		FRetriangulateBSPNCS::FParameters* P = GraphBuilder.AllocParameters<FRetriangulateBSPNCS::FParameters>();
		P->GBSPSoup = SoupSRV; P->TriangleReferenceFlags = ReferenceFlagsSRV;
		P->GCutP0 = CutP0SRV; P->GCutP1 = CutP1SRV;
		P->GCutCountSRV = CutCountSRV; P->GCutOffsetSRV = CutOffsetSRV; P->GCutCSRSRV = CutCSRSRV;
		P->RW_OutSoup = OutSoupUAV; P->RW_OutSource = OutSrcUAV; P->RW_OutCounter = OutCntUAV; P->RW_BSPStats = BSPStatUAV;
		P->RW_LimitedReasons = LimitedReasonsUAV;
		P->RW_CellVerts = CellVertsUAV; P->RW_CellVN = CellVNUAV; P->RW_ScratchCounter = ScratchCntUAV;
		P->GSnapOrigin = SnapOrigin; P->GSnapQuantum = SnapQuantum;
		P->GBSPTriBase = uint32(TriBase); P->GBSPTriCount = uint32(BatchCount);
		P->GOutCap = uint32(OutCap); P->GTriCap = uint32(TriCapacity);
		TShaderMapRef<FRetriangulateBSPNCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.Arr.RetriangulateN Base=%d Count=%d", TriBase, BatchCount), S, P,
			FComputeShaderUtils::GetGroupCountWrapped(BatchCount, 64));
	}

	if (!StageB.bEnabled) return;

	// ---- Stage B：fragment 粒度的分类，与 BSP 批次解耦 ----
	// 按 fragment 而非源三角分配线程，一次全宽 indirect dispatch 覆盖全部输出；含糊的
	// fragment 经 compaction 单独发射线，避免 warp 里少数重线程拖住其余 63 条。
	// OutSource 已在上面清成 0xFFFFFFFF；未被 BSP 写过的槽位掩码后 tri >= TriCapacity，
	// classify 与 CPU 侧都会跳过，无需额外清零。
	// 含糊 fragment 只占输出的小部分，按 OutCap 分配是纯浪费；实测 fragment 数约为源三角的
	// 2.5 倍，这里留到 4 倍再夹到 OutCap。万一仍装不下，classify 会把装不下的那些直接判 keep
	//（保守：宁可多留面也不破洞），并计数上报。
	const int32 AmbiguousCap = FMath::Clamp(TriCapacity * 4, 1024, OutCap);
	FRDGBufferRef AmbiguousListBuf = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), AmbiguousCap), TEXT("MB.StageB.AmbiguousList"));
	FRDGBufferRef AmbiguousCounterBuf = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 2), TEXT("MB.StageB.AmbiguousCounter"));
	FRDGBufferRef IndirectArgsBuf = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(), TEXT("MB.StageB.IndirectArgs"));
	FRDGBufferUAVRef AmbiguousListUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(AmbiguousListBuf, PF_R32_UINT));
	FRDGBufferUAVRef AmbiguousCounterUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(AmbiguousCounterBuf, PF_R32_UINT));
	FRDGBufferUAVRef IndirectArgsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgsBuf, PF_R32_UINT));
	FRDGBufferSRVRef AmbiguousListSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(AmbiguousListBuf, PF_R32_UINT));
	FRDGBufferSRVRef AmbiguousCounterSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(AmbiguousCounterBuf, PF_R32_UINT));
	FRDGBufferSRVRef FragmentSoupSRV = GraphBuilder.CreateSRV(OutSoupBuf);
	FRDGBufferSRVRef FragmentCounterSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(OutCounterBuf, PF_R32_UINT));
	AddClearUAVPass(GraphBuilder, AmbiguousCounterUAV, 0u);

	{
		FClassifyIndirectArgsCS::FParameters* P = GraphBuilder.AllocParameters<FClassifyIndirectArgsCS::FParameters>();
		P->MBFragmentCounter = FragmentCounterSRV;
		P->RW_MBIndirectArgs = IndirectArgsUAV;
		P->MBFragmentCapacity = uint32(OutCap);
		TShaderMapRef<FClassifyIndirectArgsCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.StageB.ClassifyArgs"), S, P, FIntVector(1, 1, 1));
	}
	{
		FClassifyFragmentsCS::FParameters* P = GraphBuilder.AllocParameters<FClassifyFragmentsCS::FParameters>();
		P->MBFragmentSoup = FragmentSoupSRV;
		P->MBFragmentCounter = FragmentCounterSRV;
		P->RW_MBFragmentSource = OutSrcUAV;
		P->RW_MBAmbiguousList = AmbiguousListUAV;
		P->RW_MBAmbiguousCounter = AmbiguousCounterUAV;
		P->GBSPSoup = SoupSRV;
		P->WMTopo = StageB.TopologySRV;
		P->WMMultipole = StageB.MultipoleSRV;
		P->WSoup = StageB.SoupSRV;
		P->WFastTriCount = StageB.TriangleCount;
		P->WBetaSq = StageB.WindingBetaSq;
		P->GBooleanWindingSampleOffset = StageB.WindingSampleOffset;
		P->GBooleanWindingThreshold = StageB.WindingThreshold;
		P->GBooleanExpansionDistance = StageB.ExpansionDistance;
		P->MBFragmentCapacity = uint32(OutCap);
		P->MBAmbiguousCapacity = uint32(AmbiguousCap);
		P->GTriCap = uint32(TriCapacity);
		P->MBIndirectArgsBuffer = IndirectArgsBuf;
		TShaderMapRef<FClassifyFragmentsCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.StageB.Classify"), S, P, IndirectArgsBuf, 0u);
	}
	{
		FRescueIndirectArgsCS::FParameters* P = GraphBuilder.AllocParameters<FRescueIndirectArgsCS::FParameters>();
		P->MBAmbiguousCounterSRV = AmbiguousCounterSRV;
		P->RW_MBIndirectArgs = IndirectArgsUAV;
		P->MBAmbiguousCapacity = uint32(AmbiguousCap);
		TShaderMapRef<FRescueIndirectArgsCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.StageB.RescueArgs"), S, P, FIntVector(1, 1, 1));
	}
	{
		FRayRescueCS::FParameters* P = GraphBuilder.AllocParameters<FRayRescueCS::FParameters>();
		P->MBFragmentSoup = FragmentSoupSRV;
		P->RW_MBFragmentSource = OutSrcUAV;
		P->MBAmbiguousListSRV = AmbiguousListSRV;
		P->MBAmbiguousCounterSRV = AmbiguousCounterSRV;
		P->GBSPSoup = SoupSRV;
		P->WMTopo = StageB.TopologySRV;
		P->WMMultipole = StageB.MultipoleSRV;
		P->WSoup = StageB.SoupSRV;
		P->OccluderVerts = StageB.SoupSRV;
		P->LBVHNodesSRV = StageB.TopologySRV;
		P->WFastTriCount = StageB.TriangleCount;
		P->WBetaSq = StageB.WindingBetaSq;
		P->OccluderTriCount = StageB.TriangleCount;
		P->RayCount = StageB.RayCount;
		P->MaxSamples = StageB.MaxSamples;
		P->CapMinCos = StageB.CapMinCos;
		P->ShellRadius = StageB.ShellRadius;
		P->RayBias = StageB.RayBias;
		P->SampleDensity = StageB.SampleDensity;
		P->bKeepBack = StageB.bKeepBack;
		P->GBooleanWindingSampleOffset = StageB.WindingSampleOffset;
		P->GBooleanWindingThreshold = StageB.WindingThreshold;
		P->GBooleanExpansionDistance = StageB.ExpansionDistance;
		P->MBFragmentCapacity = uint32(OutCap);
		P->MBAmbiguousCapacity = uint32(AmbiguousCap);
		P->GTriCap = uint32(TriCapacity);
		P->MBIndirectArgsBuffer = IndirectArgsBuf;
		TShaderMapRef<FRayRescueCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.StageB.RayRescue"), S, P, IndirectArgsBuf, 0u);
	}
}
