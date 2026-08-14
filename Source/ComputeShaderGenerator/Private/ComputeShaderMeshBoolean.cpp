#include "ComputeShaderMeshBoolean.h"

#include "CSBoxSceneCollection.h"
#include "CSGpuMeshComponent.h"
#include "CSMesh.h"
#include "CSMeshOps.h"
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
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, FinalKeptCounter)
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

// ---- GPU 输出路径：fragment → UCSMesh 常驻流（CPU 属性重建的移植） ----

/** 只数不写：CPU 用这个数分配常驻容量，判据与 emit 共用同一个 HLSL 函数。 */
class FMeshBooleanCountKeptCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMeshBooleanCountKeptCS);
	SHADER_USE_PARAMETER_STRUCT(FMeshBooleanCountKeptCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector3f>, MBOutFragmentSoup)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBOutFragmentSource)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBOutFragmentCounterSRV)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBOutSoupCounterSRV)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, MBOutSourceVertices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBOutKeptCounter)
		SHADER_PARAMETER(uint32, MBOutFragmentCapacity)
		SHADER_PARAMETER(uint32, MBOutSourceTriangleCapacity)
		SHADER_PARAMETER(uint32, MBOutStageB)
		RDG_BUFFER_ACCESS(MBOutIndirectArgsBuffer, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()
	CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
};

class FMeshBooleanEmitToMeshCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMeshBooleanEmitToMeshCS);
	SHADER_USE_PARAMETER_STRUCT(FMeshBooleanEmitToMeshCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector3f>, MBOutFragmentSoup)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBOutFragmentSource)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, MBOutSourceVertices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, MBOutSourceNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float2>, MBOutSourceUVs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, MBOutSourceColors)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, MBOutSourceTangents)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, MBOutSourceBiTangents)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBOutSourceMaterialIds)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RW_MBOutPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBOutTangents)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RW_MBOutTexCoords)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBOutColors)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBOutIndices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBOutMaterialIds)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBOutTriangleCounter)
		SHADER_PARAMETER(uint32, MBOutFragmentCount)
		SHADER_PARAMETER(uint32, MBOutSourceTriangleCount)
		SHADER_PARAMETER(uint32, MBOutSourceUVChannels)
		SHADER_PARAMETER(uint32, MBOutMaterialRegistryCount)
		SHADER_PARAMETER(uint32, MBOutNoMaterialSlot)
		SHADER_PARAMETER(uint32, MBOutVertexCapacity)
		SHADER_PARAMETER(uint32, MBOutIndexCapacity)
		SHADER_PARAMETER(uint32, MBOutStageB)
	END_SHADER_PARAMETER_STRUCT()
	CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
};

class FMeshBooleanFinalizeMeshCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMeshBooleanFinalizeMeshCS);
	SHADER_USE_PARAMETER_STRUCT(FMeshBooleanFinalizeMeshCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBOutTriangleCounterSRV)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBOutMeshCounters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBOutIndirectArgs)
		SHADER_PARAMETER(uint32, MBOutVertexCapacity)
		SHADER_PARAMETER(uint32, MBOutIndexCapacity)
	END_SHADER_PARAMETER_STRUCT()
	CSGEN_SHADER_PERM_SM5()
};

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
IMPLEMENT_GLOBAL_SHADER(FMeshBooleanCountKeptCS, "/Plugin/PCGPlugins/Shaders/Private/MeshBoolean.usf", "MeshBooleanCountKeptCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FMeshBooleanEmitToMeshCS, "/Plugin/PCGPlugins/Shaders/Private/MeshBoolean.usf", "MeshBooleanEmitToMeshCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FMeshBooleanFinalizeMeshCS, "/Plugin/PCGPlugins/Shaders/Private/MeshBoolean.usf", "MeshBooleanFinalizeMeshCS", SF_Compute);

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
	// GPU 输出路径的 accept 计数（= 输出三角数）。CPU 回读路径不跑计数核，读到 0。
	constexpr int32 FinalStatusKeptCount = FinalStatusBSPStatsBase + BSPStatCount;
	constexpr int32 FinalStatusCount = FinalStatusKeptCount + 1;

	// CPU winding BVH 已移除：最终 BSP emit 直接遍历同一 RDG 内的 GPU LBVH + 多极 refit。

	/**
	 * Boolean 管线的归一化参数。
	 *
	 * 两条消费路径（CPU 回读快照、GPU 直写常驻流）跑的是同一张图，参数换算也就只能有
	 * 一份：各写一份 clamp/单位换算，漂移的那一份不会报编译错，只会让两条路径悄悄算出
	 * 不同的布尔结果 —— 而那正是 parity 测试要防的东西，参数本身先分了岔，测试就白测了。
	 */
	struct FMeshBooleanPipelineConfig
	{
		int32 CutSegmentsPerTriangle = 4;
		int32 CutSegmentsHardCap = 200000000;
		float SideEps = 0.01f;
		float MinSegLenSq = 0.0025f;
		float SinCoplanarSq = 0.0f;
		float CoplanarOffsetEps = 0.1f;
		FVector3f LBVHAabbMin = FVector3f::ZeroVector;
		FVector3f LBVHInvExtent = FVector3f::ZeroVector;
		FVector3f SnapOrigin = FVector3f::ZeroVector;
		float SnapQuantum = 0.01f;
		bool bRunStageB = false;
		float WindingBetaSq = 6.25f;
		float WindingSampleOffset = 0.5f;
		float WindingThreshold = 0.5f;
		float ExpansionDistance = 0.0f;
		uint32 RayCount = 64;
		float CapMinCos = 0.0f;
		float ShellRadius = 1.0f;
		float RayBias = 0.05f;
		float SampleDensity = 0.0f;
		uint32 bKeepBack = 0;
		float WeldDistance = 0.0f;
		int32 OutputTrianglesPerSource = 8;
		/** GPU 输出路径专用：多跑一个 accept 计数核，结果搭 FinalStatus 的车回 CPU。 */
		bool bCountKeptFragments = false;
	};

	/** 管线产出的、跨 FRDGBuilder 存活的 buffer 与容量。 */
	struct FMeshBooleanPipelineBuffers
	{
		TRefCountPtr<FRDGPooledBuffer> SourceVertices;
		TRefCountPtr<FRDGPooledBuffer> SourceNormals;
		TRefCountPtr<FRDGPooledBuffer> SourceMaterialIds;
		TRefCountPtr<FRDGPooledBuffer> SourceUVs;
		TRefCountPtr<FRDGPooledBuffer> SourceColors;
		TRefCountPtr<FRDGPooledBuffer> SourceTangents;
		TRefCountPtr<FRDGPooledBuffer> SourceBiTangents;
		TRefCountPtr<FRDGPooledBuffer> FragmentSoup;
		TRefCountPtr<FRDGPooledBuffer> FragmentSource;
		TRefCountPtr<FRDGPooledBuffer> WeldRepresentatives;

		int32 SourceVertexCapacity = 0;
		int32 SourceTriangleCapacity = 0;
		int32 SourceUVChannels = 1;
		int32 CutCapacity = 0;
		int32 FragmentCapacity = 0;

		/** 图确实建起来了（soup 非空、容量在 RHI 支持范围内）。 */
		bool bGraphBuilt = false;
		/** 源容量或输出容量超出 RHI/工程上限；调用方据此报错而不是当成空结果。 */
		bool bCapacityUnsupported = false;
	};

	FMeshBooleanPipelineConfig MeshBoolean_BuildPipelineConfig(
		const FCSMeshBooleanOptions& Options, const FBox& QueryBox, bool bRunStageB)
	{
		FMeshBooleanPipelineConfig Config;
		Config.CutSegmentsPerTriangle = FMath::Max(1, Options.CutSegmentsPerTriangle);
		Config.CutSegmentsHardCap = FMath::Max(1024, Options.MaxCutSegmentsHardCap);
		Config.SideEps = FMath::Max(0.0f, Options.SideEpsilon);
		Config.MinSegLenSq = FMath::Max(1e-6f, Options.MinCutSegmentLength * Options.MinCutSegmentLength);
		// 共面判定阈值（归一化）：角度→sin²，偏移→真实 cm。
		const float CoplanarAngleRad = FMath::DegreesToRadians(FMath::Clamp(Options.CoplanarAngleDegrees, 0.0f, 45.0f));
		Config.SinCoplanarSq = FMath::Square(FMath::Sin(CoplanarAngleRad));
		Config.CoplanarOffsetEps = FMath::Max(0.0f, Options.CoplanarOffsetEpsilon);

		// tri-tri broad-phase：LBVH 的 Morton 量化用查询盒作 AABB（CPU 已知，免 GPU 归约往返）。
		Config.LBVHAabbMin = FVector3f((float)QueryBox.Min.X, (float)QueryBox.Min.Y, (float)QueryBox.Min.Z);
		const FVector BoxExtent = QueryBox.GetSize();
		Config.LBVHInvExtent = FVector3f(
			BoxExtent.X > 1e-3 ? 1.0f / float(BoxExtent.X) : 0.0f,
			BoxExtent.Y > 1e-3 ? 1.0f / float(BoxExtent.Y) : 0.0f,
			BoxExtent.Z > 1e-3 ? 1.0f / float(BoxExtent.Z) : 0.0f);

		// snap-round 平移帧（origin=QueryBox.Min；量化=max(SnapRoundQuantum, 最大边·2^-18)）。
		Config.SnapOrigin = Config.LBVHAabbMin;
		Config.SnapQuantum = FMath::Max(
			FMath::Max(Options.SnapRoundQuantum, float(BoxExtent.GetMax()) * FMath::Pow(2.0f, -18.0f)), 1e-6f);

		Config.bRunStageB = bRunStageB;
		Config.WindingBetaSq = FMath::Max(1.0f, Options.WindingBeta * Options.WindingBeta);
		Config.WindingSampleOffset = FMath::Max(0.001f, Options.WindingSampleOffset);
		Config.WindingThreshold = FMath::Max(0.0f, Options.WindingIsoThreshold);
		Config.ExpansionDistance = bRunStageB ? FMath::Max(0.0f, Options.RetainedTriangleExpansionDistance) : 0.0f;
		Config.RayCount = uint32(FMath::Max(1, Options.VisibilityRayCount));
		Config.CapMinCos = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(Options.VisibilityHalfAngleDegrees, 90.0f, 180.0f)));
		Config.ShellRadius = FMath::Max(
			Options.VisibilityShellRadius > 0.0f ? Options.VisibilityShellRadius : float(BoxExtent.Size()), 1.0f);
		Config.RayBias = FMath::Max(0.0f, Options.VisibilityRayBiasEpsilon);
		Config.SampleDensity = FMath::Max(0.0f, Options.VisibilitySampleDensity);
		Config.bKeepBack = Options.bKeepBackFacingVisible ? 1u : 0u;
		Config.WeldDistance = FMath::Max(0.0f, Options.VertexWeldDistance);
		Config.OutputTrianglesPerSource = FMath::Max(2, Options.ArrangementOutputTrianglesPerSource);
		return Config;
	}

}

/**
 * [render thread] 在给定的 FRDGBuilder 上建出整条 Boolean 管线：源 soup 上传、LBVH、
 * 可选的 fast-winding 场、tri-tri 求交、CSR 分组 + BSP 重三角化、Stage B 分类与射线救回、
 * 可选的焊接代表元，最后把要跨图存活的 buffer 提取出来。
 *
 * 故意不 Execute：提交时机和提交后要读什么由调用方决定。这条缝就是「布尔管线」与
 * 「结果送去哪」的分界 —— 前者两条消费路径（CPU 快照、GPU 直写常驻流）必须共用一份，
 * 否则 parity 测试比的是两条已经先分了岔的管线。
 */
static void MeshBoolean_AddPipelineToRDG(
	FRDGBuilder& GraphBuilder,
	FRHICommandListImmediate& RHICmdList,
	const FCSBoxScenePreparedData& Prepared,
	const FMeshBooleanPipelineConfig& Config,
	const FString& DebugName,
	FRHIGPUBufferReadback* FinalStatusReadback,
	uint32 FinalStatusBytes,
	FMeshBooleanPipelineBuffers& Out)
{
	FCSStaticMeshTriangleRDGOutput Soup;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MeshBoolean_SceneSoupUpload);
		Soup = AComputeShaderMeshGenerator::AddPreparedBoxSceneTrianglesToRDG(
			GraphBuilder, RHICmdList, Prepared, TEXT("CS.MeshBoolean.Soup"));
	}
	if (!Soup.TriangleVertices || !Soup.TriangleCounter || Soup.MaxTriangles == 0) return;

	const uint32 TriangleCapacity = Soup.MaxTriangles;
	if (TriangleCapacity > 8000000u)
	{
		Out.bCapacityUnsupported = true;
		return;
	}
	// 按实际要分配的容量校验 RHI 上限，而不是按 1536 MiB 硬预算——缩容后本就更容易通过。
	const int64 PlannedOutputTriangleCapacity = FMath::Clamp<int64>(
		int64(TriangleCapacity) * int64(Config.OutputTrianglesPerSource),
		1024, int64(ArrangementOutputTriangleCapacity));
	const uint64 ArrangementOutSoupCapacityBytes = uint64(PlannedOutputTriangleCapacity) * 3ull * sizeof(FVector3f);
	if (ArrangementOutSoupCapacityBytes > GRHIGlobals.MaxViewSizeBytesForNonTypedBuffer ||
		uint64(PlannedOutputTriangleCapacity) > GRHIGlobals.MaxViewDimensionForTypedBuffer)
	{
		Out.bCapacityUnsupported = true;
		return;
	}
	const uint32 MaxCutSegments = uint32(FMath::Clamp<int64>(
		int64(TriangleCapacity) * Config.CutSegmentsPerTriangle, 1024, Config.CutSegmentsHardCap));

	Out.SourceVertexCapacity = int32(Soup.MaxVertices);
	Out.SourceTriangleCapacity = int32(Soup.MaxTriangles);
	Out.SourceUVChannels = FMath::Max(1, Soup.NumUVChannels);
	Out.CutCapacity = int32(MaxCutSegments);

	// 源法线/切线恒需保留：输出沿用源属性，不再重算法线。
	GraphBuilder.QueueBufferExtraction(Soup.TriangleVertices, &Out.SourceVertices, ERHIAccess::CopySrc);
	if (Soup.TriangleNormals) GraphBuilder.QueueBufferExtraction(Soup.TriangleNormals, &Out.SourceNormals, ERHIAccess::CopySrc);
	if (Soup.TriangleMaterialIds) GraphBuilder.QueueBufferExtraction(Soup.TriangleMaterialIds, &Out.SourceMaterialIds, ERHIAccess::CopySrc);
	if (Soup.TriangleUVs) GraphBuilder.QueueBufferExtraction(Soup.TriangleUVs, &Out.SourceUVs, ERHIAccess::CopySrc);
	if (Soup.TriangleColors) GraphBuilder.QueueBufferExtraction(Soup.TriangleColors, &Out.SourceColors, ERHIAccess::CopySrc);
	if (Soup.TriangleTangents) GraphBuilder.QueueBufferExtraction(Soup.TriangleTangents, &Out.SourceTangents, ERHIAccess::CopySrc);
	if (Soup.TriangleBiTangents) GraphBuilder.QueueBufferExtraction(Soup.TriangleBiTangents, &Out.SourceBiTangents, ERHIAccess::CopySrc);

	// ---- 交线 buffer ----
	FRDGBufferRef CutP0Buf; FRDGBufferUAVRef CutP0UAV;
	CSHelper::CreateClearedTypedBuffer(GraphBuilder, CutP0Buf, CutP0UAV, sizeof(FVector4f), MaxCutSegments, PF_A32B32G32R32F, TEXT("CS.MeshBoolean.CutP0"), 0.0f);

	FRDGBufferRef CutP1Buf; FRDGBufferUAVRef CutP1UAV;
	CSHelper::CreateClearedTypedBuffer(GraphBuilder, CutP1Buf, CutP1UAV, sizeof(FVector4f), MaxCutSegments, PF_A32B32G32R32F, TEXT("CS.MeshBoolean.CutP1"), 0.0f);

	FRDGBufferRef CutCounterBuf; FRDGBufferUAVRef CutCounterUAV;
	CSHelper::CreateClearedTypedBuffer(GraphBuilder, CutCounterBuf, CutCounterUAV, sizeof(uint32), 2, PF_R32_UINT, TEXT("CS.MeshBoolean.CutCounter"), 0u);

	FRDGBufferRef StatsBuf; FRDGBufferUAVRef StatsUAV;
	CSHelper::CreateClearedTypedBuffer(GraphBuilder, StatsBuf, StatsUAV, sizeof(uint32), 8, PF_R32_UINT, TEXT("CS.MeshBoolean.Stats"), 0u);

	// ---- tri-tri broad-phase：固定构建并遍历 LBVH；需要分类时同时建多极矩场 ----
	int32 SortM = 1; while (SortM < int32(TriangleCapacity)) SortM <<= 1;
	const CSGpuTriangleUtilities::FTriangleLBVH TriangleLBVH = AComputeShaderMeshGenerator::AddTriangleLBVHToRDG(
		GraphBuilder, Soup.TriangleVerticesSRV, int32(TriangleCapacity), SortM,
		Config.LBVHAabbMin, Config.LBVHInvExtent);
	FRDGBufferSRVRef TriBVHNodesSRV = GraphBuilder.CreateSRV(
		FRDGBufferSRVDesc(TriangleLBVH.Nodes, PF_A32B32G32R32F));
	FMeshBooleanStageBRDGContext StageBContext;
	if (Config.bRunStageB)
	{
		// The base facility produces only the winding field. Boolean retains the
		// iso threshold and sample offset below because those define classification.
		FRDGBufferRef WindingMultipoles = AComputeShaderMeshGenerator::AddFastWindingToRDG(
			GraphBuilder, Soup.TriangleVerticesSRV, TriangleLBVH, int32(TriangleCapacity));
		StageBContext.bEnabled = true;
		StageBContext.TopologySRV = TriBVHNodesSRV;
		StageBContext.MultipoleSRV = GraphBuilder.CreateSRV(
			FRDGBufferSRVDesc(WindingMultipoles, PF_A32B32G32R32F));
		StageBContext.SoupSRV = Soup.TriangleVerticesSRV;
		StageBContext.TriangleCount = TriangleCapacity;
		StageBContext.WindingBetaSq = Config.WindingBetaSq;
		StageBContext.WindingSampleOffset = Config.WindingSampleOffset;
		StageBContext.WindingThreshold = Config.WindingThreshold;
		StageBContext.ExpansionDistance = Config.ExpansionDistance;
		StageBContext.RayCount = Config.RayCount;
		StageBContext.CapMinCos = Config.CapMinCos;
		StageBContext.ShellRadius = Config.ShellRadius;
		StageBContext.RayBias = Config.RayBias;
		StageBContext.SampleDensity = Config.SampleDensity;
		StageBContext.bKeepBack = Config.bKeepBack;
	}

	// ---- 三角形对求交（LBVH broad-phase），包裹 dispatch 支持 >4.19M 三角 ----
	{
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
		P->SideEps = Config.SideEps;
		P->ParallelEps = 1e-8f;
		P->MinSegLenSq = Config.MinSegLenSq;
		P->SinCoplanarSq = Config.SinCoplanarSq;
		P->CoplanarOffsetEps = Config.CoplanarOffsetEps;

		TShaderMapRef<FTriTriIntersectCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CS.MeshBoolean.TriTriIntersect"), Shader, P,
			FComputeShaderUtils::GetGroupCountWrapped(int32(TriangleCapacity), 64));
	}

	// ---- GPU arrangement：CSR 分组 + N 段 BSP 重三角化，Stage B 在其中完成分类 ----
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MeshBoolean_ArrangementSubmission);
		const int32 ScratchBatchSize = FMath::Min(int32(TriangleCapacity), GPUArrangementTriangleBatchSize);
		// 输出容量按源三角数缩放，不再无条件买断 1536 MiB：实测子三角数约为源三角的 2.5 倍，
		// 固定容量的利用率只有 ~7%，其余全是每次调用都要分配并清零的死重。溢出仍由
		// OutCounter[1] 拦下并报错（提示调大倍率），语义与固定容量时一致。
		const int32 OutCapLocal = int32(FMath::Clamp<int64>(
			int64(TriangleCapacity) * int64(Config.OutputTrianglesPerSource),
			1024, int64(ArrangementOutputTriangleCapacity)));
		FRDGBufferRef ArrOutSoup = nullptr, ArrOutSrc = nullptr, ArrOutCnt = nullptr, ArrOutStat = nullptr;
		AddArrangementToRDG(GraphBuilder, Soup.TriangleVerticesSRV, Soup.TriangleReferenceFlagsSRV,
			GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CutP0Buf, PF_A32B32G32R32F)),
			GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CutP1Buf, PF_A32B32G32R32F)),
			GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CutCounterBuf, PF_R32_UINT)),
			int32(TriangleCapacity), int32(MaxCutSegments),
			Config.SnapOrigin, Config.SnapQuantum, ScratchBatchSize, OutCapLocal, StageBContext,
			ArrOutSoup, ArrOutSrc, ArrOutCnt, ArrOutStat);
		if (Config.WeldDistance > UE_SMALL_NUMBER)
		{
			// Shared welding stops at corner representatives. Source attributes,
			// duplicate removal, and winding restoration are Boolean output policy.
			// Stage B 只给 fragment 打标记而不从 soup 里移除，故必须让 weld 跳过被剔除的
			// 角点：桶里只保留最小角点序号，混入的死角点会遮蔽真正该配对的活角点。
			// 过滤条件与消费端一致：ArrangementOnly 不跑分类，此时不过滤。
			FRDGBufferSRVRef WeldFilterSRV = Config.bRunStageB
				? GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ArrOutSrc, PF_R32_UINT))
				: nullptr;
			FRDGBufferRef WeldRepresentatives = AComputeShaderMeshGenerator::AddVertexWeldToRDG(
				GraphBuilder, ArrOutSoup, ArrOutCnt, OutCapLocal, int32(TriangleCapacity),
				Config.SnapOrigin, Config.WeldDistance,
				WeldFilterSRV, Config.bRunStageB ? MeshBooleanSourceKeep : 0u);
			GraphBuilder.QueueBufferExtraction(
				WeldRepresentatives, &Out.WeldRepresentatives, ERHIAccess::CopySrc);
		}

		// GPU 输出路径的 accept 计数。CPU 分配常驻容量要的就是这个数，而它只有 GPU 知道；
		// 判据与 emit 核共用同一个 HLSL 函数，两者不可能算出不同的三角数。
		FRDGBufferRef KeptCounterBuf; FRDGBufferUAVRef KeptCounterUAV; FRDGBufferSRVRef KeptCounterSRV;
		CSHelper::CreateClearedTypedBuffer(GraphBuilder, KeptCounterBuf, KeptCounterUAV, KeptCounterSRV,
			sizeof(uint32), 2, PF_R32_UINT, TEXT("MB.Out.KeptCounter"), 0u);
		if (Config.bCountKeptFragments)
		{
			FRDGBufferSRVRef FragmentCounterSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ArrOutCnt, PF_R32_UINT));
			FRDGBufferRef CountArgsBuf = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(), TEXT("MB.Out.CountArgs"));
			FRDGBufferUAVRef CountArgsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CountArgsBuf, PF_R32_UINT));
			{
				// fragment 数只有 GPU 知道，所以线程数也只能由 GPU 写。复用 Stage B 的同一个
				// args 核，规则（含 WRAPPED_GROUP_STRIDE 截断）自然与 classify 一致。
				FClassifyIndirectArgsCS::FParameters* P = GraphBuilder.AllocParameters<FClassifyIndirectArgsCS::FParameters>();
				P->MBFragmentCounter = FragmentCounterSRV;
				P->RW_MBIndirectArgs = CountArgsUAV;
				P->MBFragmentCapacity = uint32(OutCapLocal);
				TShaderMapRef<FClassifyIndirectArgsCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.Out.CountArgs"), S, P, FIntVector(1, 1, 1));
			}
			{
				FMeshBooleanCountKeptCS::FParameters* P = GraphBuilder.AllocParameters<FMeshBooleanCountKeptCS::FParameters>();
				P->MBOutFragmentSoup = GraphBuilder.CreateSRV(ArrOutSoup);
				P->MBOutFragmentSource = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ArrOutSrc, PF_R32_UINT));
				P->MBOutFragmentCounterSRV = FragmentCounterSRV;
				P->MBOutSoupCounterSRV = Soup.TriangleCounterSRV;
				P->MBOutSourceVertices = Soup.TriangleVerticesSRV;
				P->RW_MBOutKeptCounter = KeptCounterUAV;
				P->MBOutFragmentCapacity = uint32(OutCapLocal);
				P->MBOutSourceTriangleCapacity = TriangleCapacity;
				P->MBOutStageB = Config.bRunStageB ? 1u : 0u;
				P->MBOutIndirectArgsBuffer = CountArgsBuf;
				TShaderMapRef<FMeshBooleanCountKeptCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.Out.CountKept"), S, P, CountArgsBuf, 0u);
			}
		}

		// 先回读小型状态块，调用方据此确定精确的输出规模。
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
		FinalStatusParameters->FinalKeptCounter = KeptCounterSRV;
		FinalStatusParameters->RW_FinalStatus = GraphBuilder.CreateUAV(
			FRDGBufferUAVDesc(FinalStatusBuf, PF_R32_UINT));
		FinalStatusParameters->FinalBSPStatCount = BSPStatCount;
		TShaderMapRef<FFinalStatusCS> FinalStatusShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.FinalStatus"),
			FinalStatusShader, FinalStatusParameters, FIntVector(1, 1, 1));
		if (FinalStatusReadback) AddEnqueueCopyPass(GraphBuilder, FinalStatusReadback, FinalStatusBuf, FinalStatusBytes);
		UE_LOG(LogTemp, Log, TEXT("[MeshBoolean:%s] [GPU-arr] scratchBatch=%d batches=%d limits=%d cuts/%d verts/%d cells/%d outputsPerSource outCap=%d (%.0fx源三角, %.0fMiB, 硬上限 %d/1536MiB)"),
			*DebugName, ScratchBatchSize, FMath::DivideAndRoundUp(int32(TriangleCapacity), ScratchBatchSize),
			BSPMaxCuts, BSPMaxVerts, BSPMaxCells, BSPMaxOutputTrianglesPerSource, OutCapLocal,
			double(OutCapLocal) / FMath::Max(1.0, double(TriangleCapacity)),
			double(uint64(OutCapLocal) * 40ull) / (1024.0 * 1024.0),
			ArrangementOutputTriangleCapacity);
		Out.FragmentCapacity = OutCapLocal;
		GraphBuilder.QueueBufferExtraction(ArrOutSoup, &Out.FragmentSoup, ERHIAccess::CopySrc);
		GraphBuilder.QueueBufferExtraction(ArrOutSrc, &Out.FragmentSource, ERHIAccess::CopySrc);
	}

	Out.bGraphBuilt = true;
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

FCSMeshBooleanOptions AComputeShaderMeshBoolean::MakeBooleanOptions() const
{
	// Every value still lives on the actor as a UPROPERTY (serialized Blueprint defaults and
	// existing call sites depend on that); this is only the hand-off into the pipeline.
	FCSMeshBooleanOptions Options;
	Options.MaxSourceTriangles = MaxTriangles;
	Options.bReadLandscape = bReadLandscape;
	Options.CutSegmentsPerTriangle = CutSegmentsPerTriangle;
	Options.MaxCutSegmentsHardCap = MaxCutSegmentsHardCap;
	Options.SideEpsilon = SideEpsilon;
	Options.MinCutSegmentLength = MinCutSegmentLength;
	Options.CoplanarAngleDegrees = CoplanarAngleDegrees;
	Options.CoplanarOffsetEpsilon = CoplanarOffsetEpsilon;
	Options.WindingIsoThreshold = WindingIsoThreshold;
	Options.WindingSampleOffset = WindingSampleOffset;
	Options.WindingBeta = WindingBeta;
	Options.VisibilityRayCount = VisibilityRayCount;
	Options.VisibilityHalfAngleDegrees = VisibilityHalfAngleDegrees;
	Options.VisibilityShellRadius = VisibilityShellRadius;
	Options.VisibilitySampleDensity = VisibilitySampleDensity;
	Options.VisibilityRayBiasEpsilon = VisibilityRayBiasEpsilon;
	Options.bKeepBackFacingVisible = bKeepBackFacingVisible;
	Options.RetainedTriangleExpansionDistance = RetainedTriangleExpansionDistance;
	Options.VertexWeldDistance = VertexWeldDistance;
	Options.bPreserveSourceMaterialSlots = bPreserveSourceMaterialSlots;
	Options.SnapRoundQuantum = SnapRoundQuantum;
	Options.ArrangementOutputTrianglesPerSource = ArrangementOutputTrianglesPerSource;
	return Options;
}

UStaticMesh* AComputeShaderMeshBoolean::RunBooleanInternal(ECSMeshBooleanOp Op)
{
	const FCSMeshBooleanOptions BooleanOptions = MakeBooleanOptions();

	// GPU 直写路径：结果先落进一个 transient UCSMesh，再由公用 sink 转成 StaticMesh。这个
	// 一次性入口和 operator 库因此走同一条管线、同一份属性重建 —— 两条产出各自维护一份
	// 重建代码，正是「资产里的 UV 和运行时画出来的不一样」这类问题的来源。
	// 焊接留在 CPU 快照路径（重复三角剔除未移植，见 RunBooleanToGpuMesh）。
	if (BooleanOptions.VertexWeldDistance <= UE_SMALL_NUMBER)
	{
		UCSMesh* GpuMesh = NewObject<UCSMesh>(this);
		if (!RunBooleanToGpuMesh(Op, BooleanOptions, GpuMesh)) return nullptr;

		FCSMeshToStaticMeshOptions SinkOptions;
		// 输出是 StaticMesh 资产，与组件无关，直接用 actor 变换把世界空间结果烘到局部空间。
		SinkOptions.TargetTransform = GetActorTransform();
		SinkOptions.bBakeToLocalSpace = true;
		// 布尔结果动辄百万级三角，正是 Nanite 的适用场景。
		SinkOptions.bEnableNanite = bOutputNanite;
#if WITH_EDITOR
		// 结果落盘为 level 同级 AutoResult 文件夹，建完标脏，由用户自行 Save All 决定是否写盘。
		SinkOptions.AssetPath = BuildResultAssetPath();
		SinkOptions.bTransient = SinkOptions.AssetPath.IsEmpty();
#else
		SinkOptions.bTransient = true;
#endif
		OutputStaticMesh = UCSMeshOps::CopyToStaticMesh(GpuMesh, this, this, SinkOptions);
		if (!OutputStaticMesh && !SinkOptions.bTransient)
		{
			UE_LOG(LogTemp, Warning, TEXT("[MeshBoolean:%s] result 资产保存失败，回退为 transient StaticMesh。"), *GetName());
			SinkOptions.bTransient = true;
			SinkOptions.AssetPath.Empty();
			OutputStaticMesh = UCSMeshOps::CopyToStaticMesh(GpuMesh, this, this, SinkOptions);
		}
		if (!OutputStaticMesh)
			UE_LOG(LogTemp, Error, TEXT("[MeshBoolean:%s] shared GPU-mesh StaticMesh conversion failed."), *GetName());
		return OutputStaticMesh;
	}

	FCSGpuMeshCPUData StaticMeshData;
	TArray<UMaterialInterface*> OutputMaterialSlots;
	if (!RunBooleanToSnapshot(Op, BooleanOptions, StaticMeshData, OutputMaterialSlots)) return nullptr;

	// 输出是 StaticMesh 资产，与 DynamicMesh 组件无关，直接用 actor 变换把世界空间结果烘到局部空间。
	const FTransform OutputTransform = GetActorTransform();
	// 统一走公用转换入口：属性装配与落盘的策略（绕序、退化面阈值、空槽兜底默认材质）都在
	// CSGpuMeshConvert 里，不再由各产出路径各写一份。
	// 结果落盘为 level 同级 AutoResult 文件夹（/<level 目录>/AutoResult/SM_<actor>_<稳定编号>），建完标脏，
	// 由用户自行 Save All 决定是否写盘。名字里不带每次运行的时间戳（命名规则与 CSSW 烘焙一致），
	// 同一个 actor 反复运行始终写同一个资产，直接覆盖旧模型，引用它的组件仍指向同一份资产。
	// 非编辑器构建没有资产系统，公用入口内部退回 transient。
	FCSGpuMeshConvertOptions ConvertOptions;
	ConvertOptions.TargetTransform = OutputTransform;
	ConvertOptions.bBakeToLocalSpace = true;

	FCSGpuMeshAssetOptions AssetOptions;
	// 布尔结果动辄百万级三角，正是 Nanite 的适用场景：交给它做 LOD 与剔除，
	// 省掉手工 LOD，渲染开销与三角数基本脱钩。
	AssetOptions.bEnableNanite = bOutputNanite;
#if WITH_EDITOR
	AssetOptions.AssetPath = BuildResultAssetPath();
#else
	AssetOptions.bTransient = true;
#endif
	OutputStaticMesh = UCSGpuMeshComponent::BuildStaticMesh(
		this, this, StaticMeshData, OutputMaterialSlots, ConvertOptions, AssetOptions);
	if (!OutputStaticMesh && !AssetOptions.bTransient)
	{
		UE_LOG(LogTemp, Warning, TEXT("[MeshBoolean:%s] result 资产保存失败，回退为 transient StaticMesh。"), *GetName());
		AssetOptions.bTransient = true;
		OutputStaticMesh = UCSGpuMeshComponent::BuildStaticMesh(
			this, this, StaticMeshData, OutputMaterialSlots, ConvertOptions, AssetOptions);
	}
	if (!OutputStaticMesh)
	{
		UE_LOG(LogTemp, Error, TEXT("[MeshBoolean:%s] shared GPU-mesh StaticMesh conversion failed (vertices=%d indices=%d)."),
			*GetName(), StaticMeshData.Positions.Num(), StaticMeshData.Indices.Num());
	}

	return OutputStaticMesh;
}

bool AComputeShaderMeshBoolean::RunBooleanToSnapshot(
	ECSMeshBooleanOp Op,
	const FCSMeshBooleanOptions& Options,
	FCSGpuMeshCPUData& StaticMeshData,
	TArray<UMaterialInterface*>& OutputMaterialSlots,
	FCSMeshBooleanCapture* OutCapture)
{
	// Stage 0：校验运行环境与查询范围；任一条件无效时不启动 Boolean 管线。
	TRACE_CPUPROFILER_EVENT_SCOPE(MeshBoolean_RunBooleanInternal);
	StaticMeshData.Reset();
	OutputMaterialSlots.Reset();
	if (OutCapture) *OutCapture = FCSMeshBooleanCapture();

	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	const FBox QueryBox = GetGeneratorBoundsWorldBox();
	if (!QueryBox.IsValid)
	{
		return false;
	}

	// Shadow-free local names: the actor still owns UPROPERTYs with these names, and the
	// pipeline must read the options struct, never the members.
	const int32 SourceTriangleLimit = FMath::Max(1, Options.MaxSourceTriangles);
	const bool bIncludeLandscape = Options.bReadLandscape;

	// tri-tri 固定使用 GPU LBVH broad-phase。
	// Stage 1：规范化 Boolean 参数（与 GPU 直写路径共用同一份换算，见 MeshBoolean_BuildPipelineConfig）。
	const bool bRunStageB = (Op != ECSMeshBooleanOp::ArrangementOnly);
	FMeshBooleanPipelineConfig PipelineConfig = MeshBoolean_BuildPipelineConfig(Options, QueryBox, bRunStageB);
	// 只有要交出 capture 时才多跑那个计数核：拿到 capture 的调用方要用它给 GPU 重建定容量，
	// 而这条路径自己不需要。
	PipelineConfig.bCountKeptFragments = (OutCapture != nullptr);
	// 管线之外仍要用到的几个值，从同一份 config 里取，避免第二次换算。
	const float StageBWindingThresholdV = PipelineConfig.WindingThreshold;
	const float StageBExpansionDistanceV = PipelineConfig.ExpansionDistance;
	const float OutputWeldDistanceV = PipelineConfig.WeldDistance;
	const int32 OutputTrianglesPerSourceV = PipelineConfig.OutputTrianglesPerSource;

	// Stage 1.5: VRAM pre-flight. Every buffer below scales linearly with the source triangle
	// count, so the machine-dependent ceiling is knowable before any work starts - unlike the
	// fixed 8M guard on the render thread, which fires only after the soup was already built and
	// uploaded. The cost model mirrors what this run will actually allocate, so toggling Stage B
	// or welding moves the limit accordingly.
	{
		CSGpuMemoryBudget::FTriangleSoupCostModel Cost;
		Cost.CutSegmentsPerTriangle = PipelineConfig.CutSegmentsPerTriangle;
		Cost.OutputTrianglesPerSource = OutputTrianglesPerSourceV;
		Cost.bBuildLBVH = true;
		Cost.bBuildWindingField = bRunStageB;
		Cost.bWeldOutput = OutputWeldDistanceV > UE_SMALL_NUMBER;
		// 源法线/切线始终回读：输出沿用源属性，不再重算法线。
		Cost.bSourceNormals = true;
		Cost.bSourceTangents = true;
		if (!ConfirmGpuMemoryBudgetForBoxScene(TEXT("Mesh Boolean"), QueryBox, Cost, bIncludeLandscape)) return false;
	}

	// ---- game thread：解析场景三角形（bIncludeLandscape 控制是否纳入地形）----
	// Stage 2（Game Thread）：收集查询框内的场景三角形及其顶点属性，形成源 triangle soup。
	// 走公用的无状态收集器：盒内有哪些三角是对世界的提问，与本 actor 无关；actor 只提供
	// 自身的排除策略与 LOD（MakeBoxSceneCollectOptions），其余由 Boolean 的 options 决定。
	// Boolean 不做参照点距离过滤：源 soup 必须完整，否则被裁掉的邻接三角会让 winding 场判错。
	FCSBoxScenePreparedData Prepared;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MeshBoolean_MeshDescriptionExtraction);
		FCSBoxSceneCollectOptions CollectOptions = MakeBoxSceneCollectOptions(QueryBox);
		CollectOptions.MaxTriangles = SourceTriangleLimit;
		CollectOptions.bIncludeLandscape = bIncludeLandscape;
		CollectOptions.bPreserveSourceMaterialSlots = Options.bPreserveSourceMaterialSlots;
		Prepared = CSBoxSceneCollection::CollectBoxSceneTriangles(World, CollectOptions);
	}
	if (!Prepared.IsValid() || !Prepared.HasAnyTriangles()) return false;

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

	FMeshBooleanPipelineBuffers Pipeline;
	const FString PipelineDebugName = GetName();

	ENQUEUE_RENDER_COMMAND(MeshBooleanSplitGPU)(
		[Prepared, PipelineConfig, PipelineDebugName,
		 VertexReadback, NormalReadback, FinalStatusReadback, MaterialReadback, UVReadback, ColorReadback, TangentReadback, BiTangentReadback,
		 OutSoupReadback, OutSourceReadback, WeldRepresentativeReadback,
		 bNeedSourceNormals, bNeedSourceTangents,
		 FinalStatusBytes, &FinalStatusData, &Pipeline,
		 &bRenderWorkQueued, &bHasGPUOutput, &bArrangementCapacityUnsupported, &OutSubTriCap, &OutSoupBytes, &OutSrcBytes,
		 &GPUArrOutCount, &GPUArrOutOverflow, &SoupTriangleCount, &WeldRepresentativeBytes,
		 &VertexCapacity, &VertexBytes, &NormalBytes, &CutCapacity, &MaterialCapacity, &MaterialBytes, &UVCapacity, &UVBytes, &SoupUVChannels, &CornerAttributeBytes]
		(FRHICommandListImmediate& RHICmdList)
		{
			// Stage 4-10（Render Thread）：整条 Boolean 管线，与 GPU 直写路径共用同一个建图函数。
			FRDGBuilder GraphBuilder(RHICmdList);
			auto ExecuteSingleGraph = [&GraphBuilder]() { GraphBuilder.Execute(); };

			MeshBoolean_AddPipelineToRDG(GraphBuilder, RHICmdList, Prepared, PipelineConfig,
				PipelineDebugName, FinalStatusReadback, FinalStatusBytes, Pipeline);
			if (!Pipeline.bGraphBuilt)
			{
				// 容量不受支持要报错，空 soup 只是没东西可算：前者置位让调用方打日志，后者连
				// bRenderWorkQueued 都不置 —— 与把建图拆出去之前的两条 early-out 逐字等价。
				bArrangementCapacityUnsupported = Pipeline.bCapacityUnsupported;
				ExecuteSingleGraph();
				if (Pipeline.bCapacityUnsupported) bRenderWorkQueued = true;
				return;
			}

			const uint32 TriangleCapacity = uint32(Pipeline.SourceTriangleCapacity);
			VertexCapacity = Pipeline.SourceVertexCapacity;
			CutCapacity = Pipeline.CutCapacity;
			SoupUVChannels = Pipeline.SourceUVChannels;
			OutSubTriCap = Pipeline.FragmentCapacity;

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
				if (bHasGPUOutput && VertexBytes > 0u && Pipeline.SourceVertices) VertexReadback->EnqueueCopy(RHICmdList, Pipeline.SourceVertices->GetRHI(), VertexBytes);
				else bHasGPUOutput = false;
				if (bHasGPUOutput && NormalBytes > 0u && Pipeline.SourceNormals) NormalReadback->EnqueueCopy(RHICmdList, Pipeline.SourceNormals->GetRHI(), NormalBytes);
				if (bHasGPUOutput && MaterialBytes > 0u && Pipeline.SourceMaterialIds) MaterialReadback->EnqueueCopy(RHICmdList, Pipeline.SourceMaterialIds->GetRHI(), MaterialBytes);
				if (bHasGPUOutput && UVBytes > 0u && Pipeline.SourceUVs) UVReadback->EnqueueCopy(RHICmdList, Pipeline.SourceUVs->GetRHI(), UVBytes);
				if (bHasGPUOutput && CornerAttributeBytes > 0u && Pipeline.SourceColors) ColorReadback->EnqueueCopy(RHICmdList, Pipeline.SourceColors->GetRHI(), CornerAttributeBytes);
				if (bHasGPUOutput && bNeedSourceTangents && CornerAttributeBytes > 0u && Pipeline.SourceTangents) TangentReadback->EnqueueCopy(RHICmdList, Pipeline.SourceTangents->GetRHI(), CornerAttributeBytes);
				if (bHasGPUOutput && bNeedSourceTangents && CornerAttributeBytes > 0u && Pipeline.SourceBiTangents) BiTangentReadback->EnqueueCopy(RHICmdList, Pipeline.SourceBiTangents->GetRHI(), CornerAttributeBytes);

				const uint32 PrefixCount = GPUArrOutOverflow == 0u
					? FMath::Min(GPUArrOutCount, uint32(FMath::Max(0, OutSubTriCap)))
					: 0u;
				OutSoupBytes = PrefixCount * 3u * sizeof(FVector3f);
				OutSrcBytes = PrefixCount * sizeof(uint32);
				WeldRepresentativeBytes = PipelineConfig.WeldDistance > UE_SMALL_NUMBER
					? PrefixCount * 3u * sizeof(uint32)
					: 0u;
				if (bHasGPUOutput && OutSoupBytes > 0u && Pipeline.FragmentSoup && Pipeline.FragmentSource)
				{
					OutSoupReadback->EnqueueCopy(RHICmdList, Pipeline.FragmentSoup->GetRHI(), OutSoupBytes);
					OutSourceReadback->EnqueueCopy(RHICmdList, Pipeline.FragmentSource->GetRHI(), OutSrcBytes);
				}
				else if (bHasGPUOutput && OutSoupBytes > 0u) bHasGPUOutput = false;
				if (bHasGPUOutput && WeldRepresentativeBytes > 0u
					&& WeldRepresentativeReadback && Pipeline.WeldRepresentatives)
				{
					WeldRepresentativeReadback->EnqueueCopy(
						RHICmdList, Pipeline.WeldRepresentatives->GetRHI(), WeldRepresentativeBytes);
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
		return false;
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
		return false;
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
		return false;
	}
	if (bArrangementOverflow)
	{
		if (GOutCnt[1] > 0u || GOutCnt[0] > uint32(FMath::Max(0, OutSubTriCap)))
			UE_LOG(LogTemp, Error, TEXT("[MeshBoolean:%s] 子三角输出超出容量（需要 %u > 容量 %d）。把 Options.ArrangementOutputTrianglesPerSource 调大（当前 %d）后重试。"),
				*GetName(), GOutCnt[0], OutSubTriCap, OutputTrianglesPerSourceV);
		UE_LOG(LogTemp, Error, TEXT("[MeshBoolean:%s] GPU arrangement aborted: structural/global failure (outOverflow=%u scratchOverflow=%u partialSplitCommit=%u areaMismatch=%u maxSeg=%u maxVerts=%u maxCells=%u maxOutputPerSource=%u)"),
			*GetName(), GOutCnt[1], GStat[BSPStatScratchOverflow], GStat[BSPStatPartialSplitCommit],
			GStat[BSPStatAreaMismatch], GStat[BSPStatMaxSegments], GStat[BSPStatMaxVertices],
			GStat[BSPStatMaxCells], GStat[BSPStatMaxOutputTrianglesPerSource]);
		return false;
	}
	{
		const double GPUCopyMiB = double(uint64(OutSoupBytes) + uint64(OutSrcBytes)) / (1024.0 * 1024.0);
		const double CPUCopyMiB = double(uint64(GOutSoup.Num()) * sizeof(FVector3f) + uint64(GOutSrc.Num()) * sizeof(uint32)) / (1024.0 * 1024.0);
		UE_LOG(LogTemp, Log, TEXT("[MeshBoolean:%s] arrangement readback cap=%d actual=%u gpuCopy=%.1fMiB cpuPrefix=%.1fMiB"),
			*GetName(), OutSubTriCap, GOutCnt[0], GPUCopyMiB, CPUCopyMiB);
	}

	// 这一步之后的东西全部是「输出重建」，输入已经定死。要 capture 的调用方在这里拿走那份
	// 输入：下面那些 CPU 数组是这些 buffer 的回读，同一份字节的两种表示。管线本身不是可
	// 复现的（源 soup 用原子 bump 分配槽位，见 FCSMeshBooleanCapture 的注释），所以「跑两遍
	// 各自重建再比」比的是两套不同的碎片；要比重建，两边必须吃同一份 capture。
	if (OutCapture)
	{
		OutCapture->FragmentSoup = Pipeline.FragmentSoup;
		OutCapture->FragmentSource = Pipeline.FragmentSource;
		OutCapture->SourceVertices = Pipeline.SourceVertices;
		OutCapture->SourceNormals = Pipeline.SourceNormals;
		OutCapture->SourceUVs = Pipeline.SourceUVs;
		OutCapture->SourceColors = Pipeline.SourceColors;
		OutCapture->SourceTangents = Pipeline.SourceTangents;
		OutCapture->SourceBiTangents = Pipeline.SourceBiTangents;
		OutCapture->SourceMaterialIds = Pipeline.SourceMaterialIds;
		OutCapture->MaterialRegistry.Reset(Prepared.GetMaterialRegistryNum());
		for (int32 Index = 0; Index < Prepared.GetMaterialRegistryNum(); ++Index)
			OutCapture->MaterialRegistry.Add(Prepared.GetMaterialByRegistryIndex(Index));
		OutCapture->SourceTriangleCount = uint32(FMath::Max(0, TriCount));
		// 与下面 emit 循环遍历的范围完全一致（OutCount = clamp(GOutCnt[0], 0, OutSubTriCap)）。
		OutCapture->FragmentCount = uint32(FMath::Clamp<int32>(int32(GOutCnt[0]), 0, OutSubTriCap));
		OutCapture->OutputTriangleCount = FinalStatusData[FinalStatusKeptCount];
		OutCapture->SourceUVChannels = SoupUVChannels;
		OutCapture->bStageB = bRunStageB;
		OutCapture->QueryBox = QueryBox;
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
		// 判据写成 !(SquareSum > tol) 而不是直接调 GetSafeNormal：UE 的实现里 NaN 让
		// 「SquareSum < Tolerance」为假，于是 NaN 被乘进结果继续传播；而 GPU 侧的
		// MBOutSafeNormal 是刻意写成 !(x > t) 好让 NaN 也走 fallback。Nanite 提取
		// （AppendSourceTrianglesCS）对源法线只做裸 normalize()，零长度的授权法线会变成
		// NaN 写进 soup —— 两侧就在这里分叉，而 parity 用的干净数据永远照不到。
		const FVector3f V(N.X, N.Y, N.Z);
		const float SquareSum = V.SizeSquared();
		if (!(SquareSum > UE_SMALL_NUMBER)) return FVector3f::UnitZ();
		return V * FMath::InvSqrt(SquareSum);
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
	const double SinSq = FMath::Square(FMath::Sin(FMath::DegreesToRadians(FMath::Clamp(Options.CoplanarAngleDegrees, 0.0f, 45.0f))));
	const double OffEps = FMath::Max(0.0f, Options.CoplanarOffsetEpsilon);

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

	if (OutputSources.IsEmpty()) return false;

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

	const float EffectiveWeldDistance = FMath::Max(0.0f, Options.VertexWeldDistance);
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

	if (FinalSources.IsEmpty()) return false;

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

	return true;
}

bool AComputeShaderMeshBoolean::RunBooleanToGpuMesh(
	ECSMeshBooleanOp Op,
	const FCSMeshBooleanOptions& Options,
	UCSMesh* Target)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(MeshBoolean_RunBooleanToGpuMesh);
	if (!Target) return false;

	UWorld* World = GetWorld();
	if (!World) return false;

	const FBox QueryBox = GetGeneratorBoundsWorldBox();
	if (!QueryBox.IsValid) return false;

	const bool bRunStageB = (Op != ECSMeshBooleanOp::ArrangementOnly);
	FMeshBooleanPipelineConfig PipelineConfig = MeshBoolean_BuildPipelineConfig(Options, QueryBox, bRunStageB);
	// 常驻容量是 CPU 侧的分配，而输出三角数只有 GPU 知道，所以要多跑一个 accept 计数核。
	PipelineConfig.bCountKeptFragments = true;

	// 焊接没有移植。CPU 版的焊接后处理除了按代表元压缩顶点，还要剔退化面和**重复三角**，
	// 而后者的判据是「同一组代表元里保留 fragment 序号最小的那片」——在 GPU 上要一张全局
	// 哈希表才能复现。给出一条结果与 CPU 版不同的 GPU 路径，比慢一点糟得多：不一致是永久的，
	// 而且没有任何症状会指向这里。所以显式拒绝，由调用方回退。
	if (PipelineConfig.WeldDistance > UE_SMALL_NUMBER)
	{
		UE_LOG(LogTemp, Log, TEXT("[MeshBoolean:%s] GPU 直写路径不支持 VertexWeldDistance=%.4f（重复三角剔除未移植），回退 CPU 快照路径。"),
			*GetName(), PipelineConfig.WeldDistance);
		return false;
	}

	// VRAM pre-flight：与 CPU 路径同一套成本模型。常驻流之外的临时 buffer 两条路径完全一样，
	// 差别只在结果去哪，所以这里的上限也必须一样，否则同一个场景一条能跑一条不能。
	{
		CSGpuMemoryBudget::FTriangleSoupCostModel Cost;
		Cost.CutSegmentsPerTriangle = PipelineConfig.CutSegmentsPerTriangle;
		Cost.OutputTrianglesPerSource = PipelineConfig.OutputTrianglesPerSource;
		Cost.bBuildLBVH = true;
		Cost.bBuildWindingField = bRunStageB;
		Cost.bWeldOutput = false;
		Cost.bSourceNormals = true;
		Cost.bSourceTangents = true;
		if (!ConfirmGpuMemoryBudgetForBoxScene(TEXT("Mesh Boolean (GPU mesh)"), QueryBox, Cost, Options.bReadLandscape)) return false;
	}

	FCSBoxScenePreparedData Prepared;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MeshBoolean_MeshDescriptionExtraction);
		FCSBoxSceneCollectOptions CollectOptions = MakeBoxSceneCollectOptions(QueryBox);
		CollectOptions.MaxTriangles = FMath::Max(1, Options.MaxSourceTriangles);
		CollectOptions.bIncludeLandscape = Options.bReadLandscape;
		CollectOptions.bPreserveSourceMaterialSlots = Options.bPreserveSourceMaterialSlots;
		// Boolean 不做参照点距离过滤：源 soup 必须完整，否则被裁掉的邻接三角会让 winding 场判错。
		Prepared = CSBoxSceneCollection::CollectBoxSceneTriangles(World, CollectOptions);
	}
	if (!Prepared.IsValid() || !Prepared.HasAnyTriangles()) return false;

	// ---- 第一张图：整条布尔管线；回来的只有状态块 ----
	FRHIGPUBufferReadback* FinalStatusReadback = new FRHIGPUBufferReadback(TEXT("MeshBoolean_GpuMeshStatusReadback"));
	const uint32 FinalStatusBytes = sizeof(uint32) * uint32(FinalStatusCount);
	TArray<uint32> FinalStatusData;
	FinalStatusData.SetNumZeroed(FinalStatusCount);

	FMeshBooleanPipelineBuffers Pipeline;
	const FString PipelineDebugName = GetName();
	bool bStatusRead = false;

	ENQUEUE_RENDER_COMMAND(MeshBooleanToGpuMesh)(
		[Prepared, PipelineConfig, PipelineDebugName, FinalStatusReadback, FinalStatusBytes,
		 &FinalStatusData, &Pipeline, &bStatusRead](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			MeshBoolean_AddPipelineToRDG(GraphBuilder, RHICmdList, Prepared, PipelineConfig,
				PipelineDebugName, FinalStatusReadback, FinalStatusBytes, Pipeline);
			GraphBuilder.Execute();
			if (!Pipeline.bGraphBuilt) return;

			// 本管线唯一一次 CPU 等 GPU。读回来的是 27 个 uint，不是网格：常驻容量是 CPU 侧的
			// 分配，输出三角数又只有 GPU 知道，这一步换不掉。属性一个字节都没下来。
			if (!FinalStatusReadback->IsReady()) RHICmdList.SubmitAndBlockUntilGPUIdle();
			if (const uint32* Ptr = static_cast<const uint32*>(FinalStatusReadback->Lock(FinalStatusBytes)))
			{
				FMemory::Memcpy(FinalStatusData.GetData(), Ptr, FinalStatusBytes);
				FinalStatusReadback->Unlock();
				bStatusRead = true;
			}
		});

	FlushRenderingCommands();
	delete FinalStatusReadback;
	FinalStatusReadback = nullptr;

	if (!Pipeline.bGraphBuilt)
	{
		if (Pipeline.bCapacityUnsupported)
			UE_LOG(LogTemp, Error, TEXT("[MeshBoolean:%s] GPU arrangement aborted: source or output buffers exceed the supported project/RHI limits"), *GetName());
		return false;
	}
	if (!bStatusRead) return false;

	const uint32 SoupTriangleCount = FMath::Min(
		FinalStatusData[FinalStatusSoupCount], uint32(FMath::Max(0, Pipeline.SourceTriangleCapacity)));
	const uint32 FragmentCount = FMath::Min(
		FinalStatusData[FinalStatusOutputCount], uint32(FMath::Max(0, Pipeline.FragmentCapacity)));
	const uint32 KeptTriangleCount = FinalStatusData[FinalStatusKeptCount];

	// 中止判据与 CPU 路径逐条一致：切段不全或结构性溢出时结果就是错的，宁可不产出。
	{
		const uint32 CutOverflow = FinalStatusData[FinalStatusCutOverflow];
		const uint32 SegOverflow = FinalStatusData[FinalStatusTriStatsBase + 3];
		if (CutOverflow > 0u || SegOverflow > 0u)
		{
			UE_LOG(LogTemp, Error, TEXT("[MeshBoolean:%s] Arrangement aborted: incomplete tri-tri cut input (cutOverflow=%u segOverflow=%u)"),
				*GetName(), CutOverflow, SegOverflow);
			return false;
		}
		const uint32 OutputOverflow = FinalStatusData[FinalStatusOutputOverflow];
		const uint32 ScratchOverflow = FinalStatusData[FinalStatusBSPStatsBase + BSPStatScratchOverflow];
		const uint32 PartialSplitCommit = FinalStatusData[FinalStatusBSPStatsBase + BSPStatPartialSplitCommit];
		const uint32 AreaMismatch = FinalStatusData[FinalStatusBSPStatsBase + BSPStatAreaMismatch];
		if (OutputOverflow > 0u || FinalStatusData[FinalStatusOutputCount] > uint32(FMath::Max(0, Pipeline.FragmentCapacity))
			|| ScratchOverflow > 0u || PartialSplitCommit > 0u || AreaMismatch > 0u)
		{
			if (OutputOverflow > 0u || FinalStatusData[FinalStatusOutputCount] > uint32(FMath::Max(0, Pipeline.FragmentCapacity)))
				UE_LOG(LogTemp, Error, TEXT("[MeshBoolean:%s] 子三角输出超出容量（需要 %u > 容量 %d）。把 Options.ArrangementOutputTrianglesPerSource 调大（当前 %d）后重试。"),
					*GetName(), FinalStatusData[FinalStatusOutputCount], Pipeline.FragmentCapacity,
					PipelineConfig.OutputTrianglesPerSource);
			UE_LOG(LogTemp, Error, TEXT("[MeshBoolean:%s] GPU arrangement aborted: structural/global failure (outOverflow=%u scratchOverflow=%u partialSplitCommit=%u areaMismatch=%u)"),
				*GetName(), OutputOverflow, ScratchOverflow, PartialSplitCommit, AreaMismatch);
			return false;
		}
	}

	if (SoupTriangleCount == 0u || KeptTriangleCount == 0u)
	{
		UE_LOG(LogTemp, Warning, TEXT("[MeshBoolean:%s] GPU 直写路径没有输出三角（源三角=%u fragment=%u）。"),
			*GetName(), SoupTriangleCount, FragmentCount);
		return false;
	}

	FCSMeshBooleanCapture Capture;
	Capture.FragmentSoup = Pipeline.FragmentSoup;
	Capture.FragmentSource = Pipeline.FragmentSource;
	Capture.SourceVertices = Pipeline.SourceVertices;
	Capture.SourceNormals = Pipeline.SourceNormals;
	Capture.SourceUVs = Pipeline.SourceUVs;
	Capture.SourceColors = Pipeline.SourceColors;
	Capture.SourceTangents = Pipeline.SourceTangents;
	Capture.SourceBiTangents = Pipeline.SourceBiTangents;
	Capture.SourceMaterialIds = Pipeline.SourceMaterialIds;
	for (int32 Index = 0; Index < Prepared.GetMaterialRegistryNum(); ++Index)
		Capture.MaterialRegistry.Add(Prepared.GetMaterialByRegistryIndex(Index));
	Capture.SourceTriangleCount = SoupTriangleCount;
	Capture.FragmentCount = FragmentCount;
	Capture.OutputTriangleCount = KeptTriangleCount;
	Capture.SourceUVChannels = FMath::Max(1, Pipeline.SourceUVChannels);
	Capture.bStageB = bRunStageB;
	Capture.QueryBox = QueryBox;

	// 重建自己会打日志（它也服务于外部传进来的 capture），这里不重复。
	return RebuildGpuMeshFromCapture(Capture, Target);
}

bool FCSMeshBooleanCapture::IsValid() const
{
	return FragmentSoup.IsValid() && FragmentSource.IsValid() && SourceVertices.IsValid()
		&& SourceNormals.IsValid() && SourceUVs.IsValid() && SourceColors.IsValid()
		&& SourceTangents.IsValid() && SourceBiTangents.IsValid() && SourceMaterialIds.IsValid()
		&& SourceTriangleCount > 0u && FragmentCount > 0u && OutputTriangleCount > 0u;
}

bool AComputeShaderMeshBoolean::RebuildGpuMeshFromCapture(const FCSMeshBooleanCapture& Capture, UCSMesh* Target)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(MeshBoolean_RebuildGpuMeshFromCapture);
	if (!Target || !Capture.IsValid()) return false;
	if (!IsInGameThread()) return false;

	// 材质表：registry 原样铺开，末尾追加一个空槽。
	// 常驻流里的 id 是 UCSMesh::Materials 的槽位下标，所以「无材质」必须有自己的槽 —— 靠
	// 「越界即槽 0」的兜底会把无材质的三角和 registry 0 的三角画成同一个材质，而 CPU 路径
	// 给它们各自的槽。槽位编号与 CPU 路径不同（那边按首次使用顺序去重），每个三角解析到的
	// 材质相同，parity 测试比的也是后者。
	const int32 MaterialRegistryNum = Capture.MaterialRegistry.Num();
	Target->Materials.Reset(MaterialRegistryNum + 1);
	for (UMaterialInterface* Material : Capture.MaterialRegistry) Target->Materials.Add(Material);
	Target->Materials.Add(nullptr);
	const uint32 NoMaterialSlot = uint32(MaterialRegistryNum);

	const uint32 SoupTriangleCount = Capture.SourceTriangleCount;
	const uint32 FragmentCount = Capture.FragmentCount;
	const uint32 KeptTriangleCount = Capture.OutputTriangleCount;
	const FBox QueryBox = Capture.QueryBox;
	const bool bRunStageB = Capture.bStageB;

	// 输出是逐角点 soup（索引恒等），容量正好是 accept 计数核数出来的三角数 ×3。
	const int32 OutputCornerCount = int32(KeptTriangleCount) * 3;
	if (!Target->EnsureCapacitySync(OutputCornerCount, OutputCornerCount))
	{
		UE_LOG(LogTemp, Error, TEXT("[MeshBoolean:%s] GPU 直写路径无法为 %d 个角点分配常驻容量。"),
			*GetName(), OutputCornerCount);
		return false;
	}

	const int32 SourceUVChannels = FMath::Max(1, Capture.SourceUVChannels);
	bool bEmitted = false;
	const bool bEdited = Target->EditMeshSync(
		[&Capture, &bEmitted, FragmentCount, SoupTriangleCount, SourceUVChannels,
		 MaterialRegistryNum, NoMaterialSlot, bRunStageB, QueryBox](FCSMeshEditContext& Context)
	{
		FRDGBuilder& GraphBuilder = Context.GraphBuilder;
		if (!Context.Positions() || !Context.Tangents() || !Context.TexCoords() || !Context.Colors()
			|| !Context.Indices() || !Context.MaterialIds() || !Context.Counters() || !Context.IndirectArgs()) return;
		if (!Capture.FragmentSoup || !Capture.FragmentSource || !Capture.SourceVertices
			|| !Capture.SourceNormals || !Capture.SourceUVs || !Capture.SourceColors
			|| !Capture.SourceTangents || !Capture.SourceBiTangents || !Capture.SourceMaterialIds) return;

		// 上一张图产出的常驻 buffer 在这张图里重新注册。常驻流本身由 EditMeshSync 注册并在
		// 结束时恢复访问状态；这些是本次编辑自带的输入，归本函数管。
		FRDGBufferRef FragmentSoup = GraphBuilder.RegisterExternalBuffer(Capture.FragmentSoup);
		FRDGBufferRef FragmentSource = GraphBuilder.RegisterExternalBuffer(Capture.FragmentSource);
		FRDGBufferRef SourceVertices = GraphBuilder.RegisterExternalBuffer(Capture.SourceVertices);
		FRDGBufferRef SourceNormals = GraphBuilder.RegisterExternalBuffer(Capture.SourceNormals);
		FRDGBufferRef SourceUVs = GraphBuilder.RegisterExternalBuffer(Capture.SourceUVs);
		FRDGBufferRef SourceColors = GraphBuilder.RegisterExternalBuffer(Capture.SourceColors);
		FRDGBufferRef SourceTangents = GraphBuilder.RegisterExternalBuffer(Capture.SourceTangents);
		FRDGBufferRef SourceBiTangents = GraphBuilder.RegisterExternalBuffer(Capture.SourceBiTangents);
		FRDGBufferRef SourceMaterialIds = GraphBuilder.RegisterExternalBuffer(Capture.SourceMaterialIds);

		FRDGBufferRef TriangleCounterBuf; FRDGBufferUAVRef TriangleCounterUAV; FRDGBufferSRVRef TriangleCounterSRV;
		CSHelper::CreateClearedTypedBuffer(GraphBuilder, TriangleCounterBuf, TriangleCounterUAV, TriangleCounterSRV,
			sizeof(uint32), 2, PF_R32_UINT, TEXT("MB.Out.TriangleCounter"), 0u);

		FRDGBufferUAVRef MaterialIdUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Context.MaterialIds(), PF_R32_UINT));
		// 材质 id 流整条先清成「无材质」槽。发射核会覆写每个活三角自己那一格，可常驻 buffer
		// 来自池子，容量尾部留着上一位租客的字节 —— 计数一变大（下一次布尔、或别的算子），
		// 那些字节就成了「材质槽」，而没有任何症状会指向这里。
		AddClearUAVPass(GraphBuilder, MaterialIdUAV, NoMaterialSlot);

		{
			FMeshBooleanEmitToMeshCS::FParameters* P = GraphBuilder.AllocParameters<FMeshBooleanEmitToMeshCS::FParameters>();
			P->MBOutFragmentSoup = GraphBuilder.CreateSRV(FragmentSoup);
			P->MBOutFragmentSource = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(FragmentSource, PF_R32_UINT));
			P->MBOutSourceVertices = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SourceVertices, PF_A32B32G32R32F));
			P->MBOutSourceNormals = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SourceNormals, PF_A32B32G32R32F));
			P->MBOutSourceUVs = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SourceUVs, PF_G32R32F));
			P->MBOutSourceColors = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SourceColors, PF_A32B32G32R32F));
			P->MBOutSourceTangents = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SourceTangents, PF_A32B32G32R32F));
			P->MBOutSourceBiTangents = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SourceBiTangents, PF_A32B32G32R32F));
			P->MBOutSourceMaterialIds = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SourceMaterialIds, PF_R32_UINT));
			P->RW_MBOutPositions = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Context.Positions(), PF_R32_FLOAT));
			P->RW_MBOutTangents = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Context.Tangents(), PF_R32_UINT));
			P->RW_MBOutTexCoords = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Context.TexCoords(), PF_R32_FLOAT));
			P->RW_MBOutColors = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Context.Colors(), PF_R32_UINT));
			P->RW_MBOutIndices = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Context.Indices(), PF_R32_UINT));
			P->RW_MBOutMaterialIds = MaterialIdUAV;
			P->RW_MBOutTriangleCounter = TriangleCounterUAV;
			P->MBOutFragmentCount = FragmentCount;
			P->MBOutSourceTriangleCount = SoupTriangleCount;
			P->MBOutSourceUVChannels = uint32(SourceUVChannels);
			P->MBOutMaterialRegistryCount = uint32(MaterialRegistryNum);
			P->MBOutNoMaterialSlot = NoMaterialSlot;
			P->MBOutVertexCapacity = Context.Resident.VertexCapacity;
			P->MBOutIndexCapacity = Context.Resident.IndexCapacity;
			P->MBOutStageB = bRunStageB ? 1u : 0u;

			TShaderMapRef<FMeshBooleanEmitToMeshCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.Out.EmitToMesh"), Shader, P,
				FComputeShaderUtils::GetGroupCountWrapped(int32(FragmentCount), 64));
		}
		{
			// 三角数变了、顺序也变了：section 表描述的是已经不存在的那套排布，必须一起作废。
			UCSMeshOps::InvalidateSections(Context);

			FMeshBooleanFinalizeMeshCS::FParameters* P = GraphBuilder.AllocParameters<FMeshBooleanFinalizeMeshCS::FParameters>();
			P->MBOutTriangleCounterSRV = TriangleCounterSRV;
			P->RW_MBOutMeshCounters = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Context.Counters(), PF_R32_UINT));
			P->RW_MBOutIndirectArgs = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Context.IndirectArgs(), PF_R32_UINT));
			P->MBOutVertexCapacity = Context.Resident.VertexCapacity;
			P->MBOutIndexCapacity = Context.Resident.IndexCapacity;

			TShaderMapRef<FMeshBooleanFinalizeMeshCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.Out.FinalizeMesh"), Shader, P,
				FIntVector(1, 1, 1));
		}

		// 输出大小是 GPU 定的，游戏线程不能声称自己知道。
		Context.InvalidateKnownCounts();
		// 查询盒是不问 GPU 就能拿到的唯一界；下面的精确归约算不出来时它就是兜底。
		Context.Resident.WorldBounds = QueryBox;
		bEmitted = true;
	});

	if (!bEdited || !bEmitted)
	{
		UE_LOG(LogTemp, Error, TEXT("[MeshBoolean:%s] GPU 直写路径的常驻流写入失败。"), *GetName());
		return false;
	}

	// 结果的包围盒只有 GPU 知道。CPU 路径是顺手从回读的位置里算的；这里没有回读，所以显式
	// 归约一次 —— 否则包围盒就是整个查询盒，稀疏结果的剔除和阴影都要为那片空气买单。
	UCSMeshOps::ComputeWorldBoundsSync(Target);

	UE_LOG(LogTemp, Log, TEXT("[MeshBoolean:%s] GPU direct write: sourceTris=%u fragments=%u outTris=%u stageB=%d materialSlots=%d"),
		*GetName(), SoupTriangleCount, FragmentCount, KeptTriangleCount, bRunStageB ? 1 : 0, Target->Materials.Num());
	return true;
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
