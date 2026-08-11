// Fill out your copyright notice in the Description page of Project Settings.
#include "GeometryEditorActor.h"

#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Landscape.h"
#include "ObjectTools.h"
#include "GeometryMathUtils.h"
#include "PCGPluginDebug.h"
#include "PackageTools.h"

// SpaceColonization GPU shader includes (moved from GenerateVines.cpp)
#include "GlobalShader.h"
#include "GeometryAsync.h"
#include "GeometryGenerate.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "ShaderParameterStruct.h"
#include "ComputeShaderBasicFunction.h"
#include "ComputeShaderGenerateHelper.h"
#include "Engine/Level.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SplineComponent.h"
#include "Materials/MaterialInterface.h"
#include "UObject/UObjectHash.h"
#include "DrawDebugHelpers.h"
#include "GeometryScript/MeshBasicEditFunctions.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryGeneral.h"
#include "DynamicMesh/MeshTransforms.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "ShaderParameterStruct.h"
#include "Misc/PackageName.h"
#include "VineMeshComponent.h"
#include "CSGpuDebugDraw.h"
#include "CSGpuMeshSceneProxy.h"
#include "CSGpuMeshComponent.h"
#include "SceneInterface.h"
#include "Engine/Engine.h"
#include "Materials/MaterialRenderProxy.h"

#define GV_ACTOR_ENABLE_PERF_LOGS 1
#if GV_ACTOR_ENABLE_PERF_LOGS
#define GV_ACTOR_TIME_SCOPE(Label) PCG_DEBUG_TIME_SCOPE_WITH_PREFIX(TEXT("[GenerateVinesTiming]"), Label)
#else
#define GV_ACTOR_TIME_SCOPE(Label)
#endif

#define GV_ENABLE_PERF_LOGS GV_ACTOR_ENABLE_PERF_LOGS
#define GV_TIME_SCOPE(Label) GV_ACTOR_TIME_SCOPE(Label)

// Voxel-based surface projection shader for GPU-only vine visualization.
class FVVVoxelCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVVVoxelCS);
	SHADER_USE_PARAMETER_STRUCT(FVVVoxelCS, FGlobalShader);

	// When set, emit the vine mesh into the GPU-resident base streams (positions/tangents/
	// texcoords/colors/indices + indirect args + counters) instead of the legacy transient
	// StructuredBuffers. The legacy path (false) stays byte-identical.
	class FBaseStreams : SHADER_PERMUTATION_BOOL("VINE_OUTPUT_BASESTREAMS");
	using FPermutationDomain = TShaderPermutationDomain<FBaseStreams>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPoints)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, PathPointMeta)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPointTangents)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPointNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPointFrameNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPointSurfaceTargets)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPointSurfaceNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, SegmentMeta)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, VoxelCells)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VoxelHashSlots)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, VoxelNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, VoxelTargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, TargetBucketRanges)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TargetBucketRangeCounts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TargetBucketVoxelIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TargetBucketHashSlots)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_OutVertices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float2>, RW_OutUVs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_OutIndices)
		// Base-stream outputs (only bound/referenced when the FBaseStreams permutation is set).
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RWPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>,  RWTangents)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RWTexCoords)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>,  RWColors)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>,  RWBaseIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VineMeshCounts)
		SHADER_PARAMETER(FMatrix44f, VineWorldToLocal)
		SHADER_PARAMETER(uint32, PathPointCount)
		SHADER_PARAMETER(uint32, SegmentCount)
		SHADER_PARAMETER(uint32, OutputVertexCount)
		SHADER_PARAMETER(uint32, OutputIndexCount)
		SHADER_PARAMETER(uint32, ProfileCount)
		SHADER_PARAMETER(uint32, bTube)
		SHADER_PARAMETER(float, CircleScale)
		SHADER_PARAMETER(float, LineScale)
		SHADER_PARAMETER(FVector3f, VoxelOrigin)
		SHADER_PARAMETER(float, VoxelSize)
		SHADER_PARAMETER(uint32, VoxelCount)
		SHADER_PARAMETER(uint32, VoxelHashSlotCount)
		SHADER_PARAMETER(FVector3f, TargetBucketOrigin)
		SHADER_PARAMETER(float, TargetBucketSize)
		SHADER_PARAMETER(uint32, TargetBucketCount)
		SHADER_PARAMETER(uint32, TargetBucketHashSlotCount)
		SHADER_PARAMETER(uint32, TargetBucketSearchRadius)
		SHADER_PARAMETER(float, VinesOffset)
		SHADER_PARAMETER(float, TinyZJitterStrength)
		RDG_BUFFER_ACCESS(VineDispatchArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

IMPLEMENT_GLOBAL_SHADER(FVVVoxelCS, "/Plugin/PCGPlugins/Shaders/Private/VVVoxel.usf", "BuildVVVoxelCS", SF_Compute);

// Stage E: publishes the four DispatchIndirect arg sets, the DrawIndexedIndirect args and the
// mesh counters straight from the GPU-decided counts. Runs once, as a direct 1x1x1 dispatch,
// before every other vine mesh pass (which are all DispatchIndirect'd from its output).
class FVineDispatchArgsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVineDispatchArgsCS);
	SHADER_USE_PARAMETER_STRUCT(FVineDispatchArgsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VineArgs_Counts)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_VineArgs_Dispatch)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_VineArgs_DrawIndirect)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_VineArgs_MeshCounters)
		SHADER_PARAMETER(uint32, VineArgs_PointCapacity)
		SHADER_PARAMETER(uint32, VineArgs_SegmentCapacity)
		SHADER_PARAMETER(uint32, VineArgs_ProfileCount)
		SHADER_PARAMETER(uint32, VineArgs_bTube)
		SHADER_PARAMETER(uint32, VineArgs_GroupSize)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FVineDispatchArgsCS, "/Plugin/PCGPlugins/Shaders/Private/VVVoxel.usf", "VineDispatchArgsCS", SF_Compute);

// Thread-group size shared by every vine mesh kernel (their ModifyCompilationEnvironment all set
// THREADGROUPSIZE_X to this); VineDispatchArgsCS divides the GPU counts by it. The axial-V
// segmented scan below also partitions points into blocks of exactly this size, which is what
// lets it reuse the per-point indirect arg slot instead of publishing its own.
static constexpr uint32 VineMeshGroupSize = 64u;

// ============================================================================
// Base-stream axial-V (mesh UV.y) GPU passes. Six kernels over scratch structured buffers,
// dispatched from AddVineMeshPasses. These replaced an equivalent CPU pass that ran after a
// vertex readback; the vine no longer reads vertices back, so this is the only axial-V path.
// VineUV_-prefixed names keep them unique in the unity build.
// ============================================================================
class FVineUVCentersCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVineUVCentersCS);
	SHADER_USE_PARAMETER_STRUCT(FVineUVCentersCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, VineUV_Positions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_VineUV_Centers)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, RW_VineUV_RingCirc)
		SHADER_PARAMETER(uint32, VineUV_PointCount)
		SHADER_PARAMETER(uint32, VineUV_ProfileCount)
		RDG_BUFFER_ACCESS(VineDispatchArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

IMPLEMENT_GLOBAL_SHADER(FVineUVCentersCS, "/Plugin/PCGPlugins/Shaders/Private/VVVoxel.usf", "VineUVCentersCS", SF_Compute);

class FVineUVSegLenCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVineUVSegLenCS);
	SHADER_USE_PARAMETER_STRUCT(FVineUVSegLenCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, VineUV_SegmentMeta)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, VineUV_Centers)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, RW_VineUV_SegLen)
		SHADER_PARAMETER(uint32, VineUV_PointCount)
		SHADER_PARAMETER(uint32, VineUV_SegmentCount)
		RDG_BUFFER_ACCESS(VineDispatchArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

IMPLEMENT_GLOBAL_SHADER(FVineUVSegLenCS, "/Plugin/PCGPlugins/Shaders/Private/VVVoxel.usf", "VineUVSegLenCS", SF_Compute);

// V3a/V3b/V3c: the axial-V segmented prefix sum. CurveV is an inclusive scan of the per-point
// normalized axial step, restarted at every line head (SegLen[P-1] < 0), so it parallelizes as
// the textbook two-level scan: per-block scan + block aggregate, scan the aggregates, add back.
// It replaced a [numthreads(1,1,1)] serial loop that walked every point one at a time.
// All three set VINE_UV_SCAN_BLOCK from VineMeshGroupSize so V3a's block partition matches the
// per-point indirect arg slot it is dispatched from.
static void VineUVScanModifyEnvironment(FShaderCompilerEnvironment& OutEnvironment)
{
	OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), VineMeshGroupSize);
	OutEnvironment.SetDefine(TEXT("VINE_UV_SCAN_BLOCK"), VineMeshGroupSize);
}

class FVineUVScanBlockCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVineUVScanBlockCS);
	SHADER_USE_PARAMETER_STRUCT(FVineUVScanBlockCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, VineUV_SegLen)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, VineUV_RingCirc)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VineUV_Counts)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, RW_VineUV_CurveV)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_VineUV_ScanFlags)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, RW_VineUV_ScanBlockSum)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_VineUV_ScanBlockFlag)
		SHADER_PARAMETER(uint32, VineUV_PointCount)
		SHADER_PARAMETER(float, VineUV_LengthScale)
		RDG_BUFFER_ACCESS(VineDispatchArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		VineUVScanModifyEnvironment(OutEnvironment);
	}
};

IMPLEMENT_GLOBAL_SHADER(FVineUVScanBlockCS, "/Plugin/PCGPlugins/Shaders/Private/VVVoxel.usf", "VineUVScanBlockCS", SF_Compute);

class FVineUVScanBlockOffsetsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVineUVScanBlockOffsetsCS);
	SHADER_USE_PARAMETER_STRUCT(FVineUVScanBlockOffsetsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, VineUV_ScanBlockSum)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VineUV_ScanBlockFlag)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VineUV_Counts)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, RW_VineUV_ScanBlockCarry)
		SHADER_PARAMETER(uint32, VineUV_PointCount)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		VineUVScanModifyEnvironment(OutEnvironment);
	}
};

IMPLEMENT_GLOBAL_SHADER(FVineUVScanBlockOffsetsCS, "/Plugin/PCGPlugins/Shaders/Private/VVVoxel.usf", "VineUVScanBlockOffsetsCS", SF_Compute);

class FVineUVScanApplyCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVineUVScanApplyCS);
	SHADER_USE_PARAMETER_STRUCT(FVineUVScanApplyCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VineUV_ScanFlags)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, VineUV_ScanBlockCarry)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VineUV_Counts)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, RW_VineUV_CurveV)
		SHADER_PARAMETER(uint32, VineUV_PointCount)
		RDG_BUFFER_ACCESS(VineDispatchArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		VineUVScanModifyEnvironment(OutEnvironment);
	}
};

IMPLEMENT_GLOBAL_SHADER(FVineUVScanApplyCS, "/Plugin/PCGPlugins/Shaders/Private/VVVoxel.usf", "VineUVScanApplyCS", SF_Compute);

class FVineUVWriteCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVineUVWriteCS);
	SHADER_USE_PARAMETER_STRUCT(FVineUVWriteCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, VineUV_CurveV)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RW_VineUV_TexCoords)
		SHADER_PARAMETER(uint32, VineUV_OutputVertexCount)
		SHADER_PARAMETER(uint32, VineUV_ProfileCount)
		SHADER_PARAMETER(uint32, VineUV_PointCount)
		RDG_BUFFER_ACCESS(VineDispatchArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

IMPLEMENT_GLOBAL_SHADER(FVineUVWriteCS, "/Plugin/PCGPlugins/Shaders/Private/VVVoxel.usf", "VineUVWriteCS", SF_Compute);


// Trip B: rebuild the vine's one-buffer voxel-cell hash on the GPU from the producer's
// retained cells (writes the exact layout FindVoxelIndexForCell decodes). Lets the vine
// consume the GPU-resident surface voxels without the CPU readback + BuildVineVoxelHashSlots.
class FVVBuildVoxelHashCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVVBuildVoxelHashCS);
	SHADER_USE_PARAMETER_STRUCT(FVVBuildVoxelHashCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, HashBuildCells)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, HashBuildCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_HashBuildSlots)
		SHADER_PARAMETER(uint32, HashBuildVoxelCount)
		SHADER_PARAMETER(uint32, HashBuildSlotCount)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

IMPLEMENT_GLOBAL_SHADER(FVVBuildVoxelHashCS, "/Plugin/PCGPlugins/Shaders/Private/VVVoxel.usf", "BuildVineVoxelHashCS", SF_Compute);

class FVVVoxelBuildAxesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVVVoxelBuildAxesCS);
	SHADER_USE_PARAMETER_STRUCT(FVVVoxelBuildAxesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPoints)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPointAxes)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, PathPointMeta)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, VoxelCells)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VoxelHashSlots)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, VoxelNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, VoxelTargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, TargetBucketRanges)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TargetBucketRangeCounts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TargetBucketVoxelIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TargetBucketHashSlots)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_PathPointTangents)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_PathPointNormals)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_PathPointSurfaceTargets)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_PathPointSurfaceNormals)
		SHADER_PARAMETER(uint32, PathPointCount)
		SHADER_PARAMETER(FVector3f, VoxelOrigin)
		SHADER_PARAMETER(float, VoxelSize)
		SHADER_PARAMETER(uint32, VoxelCount)
		SHADER_PARAMETER(uint32, VoxelHashSlotCount)
		SHADER_PARAMETER(FVector3f, TargetBucketOrigin)
		SHADER_PARAMETER(float, TargetBucketSize)
		SHADER_PARAMETER(uint32, TargetBucketCount)
		SHADER_PARAMETER(uint32, TargetBucketHashSlotCount)
		SHADER_PARAMETER(uint32, TargetBucketSearchRadius)
		RDG_BUFFER_ACCESS(VineDispatchArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

IMPLEMENT_GLOBAL_SHADER(FVVVoxelBuildAxesCS, "/Plugin/PCGPlugins/Shaders/Private/VVVoxel.usf", "BuildVVVoxelAxesCS", SF_Compute);

// Reparameterizes each vine's SurfaceTarget points to uniform arc-length spacing along the
// post-projection surface polyline, keeping the point count fixed. Runs after FinalProject and
// before the tangents are rebuilt, controlled by VisVineGPUResampleSurfaceEnabled.
class FVVVoxelResampleSurfaceCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVVVoxelResampleSurfaceCS);
	SHADER_USE_PARAMETER_STRUCT(FVVVoxelResampleSurfaceCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPoints)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPointSurfaceTargets)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPointSurfaceNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, PathPointMeta)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_PathPointSurfaceTargets)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_PathPointSurfaceNormals)
		SHADER_PARAMETER(uint32, PathPointCount)
		SHADER_PARAMETER(float, ResampleTargetDistance)
		RDG_BUFFER_ACCESS(VineDispatchArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

IMPLEMENT_GLOBAL_SHADER(FVVVoxelResampleSurfaceCS, "/Plugin/PCGPlugins/Shaders/Private/VVVoxel.usf", "ResampleVVSurfaceCS", SF_Compute);

// Rebuilds tangents from the final (post line-smoothing) surface targets, then parallel-transports
// the roll axis along each line to minimize twist. This is the final axis frame for mesh build.
class FVVVoxelBuildParallelTransportFrameCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVVVoxelBuildParallelTransportFrameCS);
	SHADER_USE_PARAMETER_STRUCT(FVVVoxelBuildParallelTransportFrameCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPoints)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPointSurfaceTargets)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPointSurfaceNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, PathPointMeta)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_PathPointTangents)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_PathPointFrameNormals)
		SHADER_PARAMETER(uint32, PathPointCount)
		RDG_BUFFER_ACCESS(VineDispatchArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

IMPLEMENT_GLOBAL_SHADER(FVVVoxelBuildParallelTransportFrameCS, "/Plugin/PCGPlugins/Shaders/Private/VVVoxel.usf", "BuildVVParallelTransportFrameCS", SF_Compute);

// Applies PerlinNoise displacement to SurfaceTarget after BuildAxes and before Smooth.
class FVVVoxelPerlinNoiseCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVVVoxelPerlinNoiseCS);
	SHADER_USE_PARAMETER_STRUCT(FVVVoxelPerlinNoiseCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPointSurfaceTargets)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPointSurfaceNormals)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_PathPointSurfaceTargets)
		SHADER_PARAMETER(float, PerlinNoiseStrength)
		SHADER_PARAMETER(float, PerlinNoiseFrequency)
		SHADER_PARAMETER(uint32, PathPointCount)
		RDG_BUFFER_ACCESS(VineDispatchArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

IMPLEMENT_GLOBAL_SHADER(FVVVoxelPerlinNoiseCS, "/Plugin/PCGPlugins/Shaders/Private/VVVoxel.usf", "ApplyVVPerlinNoiseCS", SF_Compute);

// Applies the CPU-equivalent noise loop (voxel projection + CurlNoise) on the GPU.
class FVVVoxelNoiseCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVVVoxelNoiseCS);
	SHADER_USE_PARAMETER_STRUCT(FVVVoxelNoiseCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPoints)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, VoxelCells)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VoxelHashSlots)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, VoxelNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, VoxelTargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, TargetBucketRanges)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TargetBucketRangeCounts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TargetBucketVoxelIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TargetBucketHashSlots)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_PathPointsNoised)
		SHADER_PARAMETER(uint32, PathPointCount)
		SHADER_PARAMETER(FVector3f, VoxelOrigin)
		SHADER_PARAMETER(float, VoxelSize)
		SHADER_PARAMETER(uint32, VoxelCount)
		SHADER_PARAMETER(uint32, VoxelHashSlotCount)
		SHADER_PARAMETER(FVector3f, TargetBucketOrigin)
		SHADER_PARAMETER(float, TargetBucketSize)
		SHADER_PARAMETER(uint32, TargetBucketCount)
		SHADER_PARAMETER(uint32, TargetBucketHashSlotCount)
		SHADER_PARAMETER(uint32, TargetBucketSearchRadius)
		SHADER_PARAMETER(float, CurlNoiseStrength)
		SHADER_PARAMETER(float, CurlNoiseFrequency)
		SHADER_PARAMETER(uint32, NoiseIterations)
		RDG_BUFFER_ACCESS(VineDispatchArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

IMPLEMENT_GLOBAL_SHADER(FVVVoxelNoiseCS, "/Plugin/PCGPlugins/Shaders/Private/VVVoxel.usf", "ApplyVVNoiseCS", SF_Compute);

class FVVVoxelSmoothPathCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVVVoxelSmoothPathCS);
	SHADER_USE_PARAMETER_STRUCT(FVVVoxelSmoothPathCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPointSurfaceTargets)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPointSurfaceNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, PathPointMeta)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_PathPointSurfaceTargets)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_PathPointSurfaceNormals)
		SHADER_PARAMETER(uint32, PathPointCount)
		SHADER_PARAMETER(int32, SmoothPathKernelRadius)
		SHADER_PARAMETER(float, SmoothPathAngleStrength)
		RDG_BUFFER_ACCESS(VineDispatchArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

IMPLEMENT_GLOBAL_SHADER(FVVVoxelSmoothPathCS, "/Plugin/PCGPlugins/Shaders/Private/VVVoxel.usf", "SmoothVVVoxelPathCS", SF_Compute);

class FVVVoxelFinalProjectCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVVVoxelFinalProjectCS);
	SHADER_USE_PARAMETER_STRUCT(FVVVoxelFinalProjectCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPointSurfaceTargets)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPointSurfaceNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, VoxelCells)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VoxelHashSlots)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, VoxelNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, VoxelTargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, TargetBucketRanges)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TargetBucketRangeCounts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TargetBucketVoxelIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TargetBucketHashSlots)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_PathPointSurfaceTargets)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_PathPointSurfaceNormals)
		SHADER_PARAMETER(uint32, PathPointCount)
		SHADER_PARAMETER(FVector3f, VoxelOrigin)
		SHADER_PARAMETER(float, VoxelSize)
		SHADER_PARAMETER(uint32, VoxelCount)
		SHADER_PARAMETER(uint32, VoxelHashSlotCount)
		SHADER_PARAMETER(FVector3f, TargetBucketOrigin)
		SHADER_PARAMETER(float, TargetBucketSize)
		SHADER_PARAMETER(uint32, TargetBucketCount)
		SHADER_PARAMETER(uint32, TargetBucketHashSlotCount)
		SHADER_PARAMETER(uint32, TargetBucketSearchRadius)
		SHADER_PARAMETER(float, VinesOffset)
		RDG_BUFFER_ACCESS(VineDispatchArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

IMPLEMENT_GLOBAL_SHADER(FVVVoxelFinalProjectCS, "/Plugin/PCGPlugins/Shaders/Private/VVVoxel.usf", "FinalProjectVVVoxelCS", SF_Compute);

// SpaceColonization GPU queue shaders (moved from GenerateVines.cpp)

class FSpaceColonizationQueueInitCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSpaceColonizationQueueInitCS);
	SHADER_USE_PARAMETER_STRUCT(FSpaceColonizationQueueInitCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, InitialTargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_TargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int4>, RW_State0)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int4>, RW_State1)
		SHADER_PARAMETER(uint32, TargetCount)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

class FSpaceColonizationQueueMarkSourcesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSpaceColonizationQueueMarkSourcesCS);
	SHADER_USE_PARAMETER_STRUCT(FSpaceColonizationQueueMarkSourcesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, SourcePositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, InitialTargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_TargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int4>, RW_State0)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int4>, RW_State1)
		SHADER_PARAMETER(uint32, SourceCount)
		SHADER_PARAMETER(uint32, TargetCount)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

class FSpaceColonizationQueueBuildNeighborsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSpaceColonizationQueueBuildNeighborsCS);
	SHADER_USE_PARAMETER_STRUCT(FSpaceColonizationQueueBuildNeighborsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, InitialTargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_NeighborCounts)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_NeighborIndices)
		SHADER_PARAMETER(uint32, TargetCount)
		SHADER_PARAMETER(uint32, MaxNeighbors)
		SHADER_PARAMETER(float, InfluenceRadius)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

class FSpaceColonizationQueueResetProposalsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSpaceColonizationQueueResetProposalsCS);
	SHADER_USE_PARAMETER_STRUCT(FSpaceColonizationQueueResetProposalsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_ProposalOwners)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_ProposalPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_Claims)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_ClaimCounts)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_ProposalTargets)
		SHADER_PARAMETER(uint32, TargetCount)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

class FSpaceColonizationQueueClaimCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSpaceColonizationQueueClaimCS);
	SHADER_USE_PARAMETER_STRUCT(FSpaceColonizationQueueClaimCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, TargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, State0)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborCounts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborIndices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_Claims)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_ClaimCounts)
		SHADER_PARAMETER(uint32, TargetCount)
		SHADER_PARAMETER(uint32, MaxNeighbors)
		SHADER_PARAMETER(float, InfluenceRadius)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

class FSpaceColonizationQueueProposeCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSpaceColonizationQueueProposeCS);
	SHADER_USE_PARAMETER_STRUCT(FSpaceColonizationQueueProposeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, InitialTargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, TargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, State0)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, State1)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborCounts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, Claims)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ClaimCounts)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_ProposalOwners)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_ProposalPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_ProposalTargets)
		SHADER_PARAMETER(uint32, TargetCount)
		SHADER_PARAMETER(uint32, MaxNeighbors)
		SHADER_PARAMETER(uint32, Iteration)
		SHADER_PARAMETER(uint32, Activetime)
		SHADER_PARAMETER(float, RandGrow)
		SHADER_PARAMETER(float, Seed)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

class FSpaceColonizationQueueCommitParentsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSpaceColonizationQueueCommitParentsCS);
	SHADER_USE_PARAMETER_STRUCT(FSpaceColonizationQueueCommitParentsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ProposalTargets)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int4>, RW_State0)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int4>, RW_State1)
		SHADER_PARAMETER(uint32, TargetCount)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

class FSpaceColonizationQueueCommitChildrenCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSpaceColonizationQueueCommitChildrenCS);
	SHADER_USE_PARAMETER_STRUCT(FSpaceColonizationQueueCommitChildrenCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, InitialTargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ProposalOwners)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, ProposalPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_TargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int4>, RW_State0)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int4>, RW_State1)
		SHADER_PARAMETER(uint32, TargetCount)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

// GPU vine line-building (Increment B): BranchOrder -> CountLines -> PrefixSum.
class FSpaceColonizationBranchOrderCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSpaceColonizationBranchOrderCS);
	SHADER_USE_PARAMETER_STRUCT(FSpaceColonizationBranchOrderCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, State0)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, State1)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_BranchOrder)
		SHADER_PARAMETER(uint32, TargetCount)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

class FSpaceColonizationCountLinesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSpaceColonizationCountLinesCS);
	SHADER_USE_PARAMETER_STRUCT(FSpaceColonizationCountLinesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, State0)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, State1)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BranchOrder)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_LineCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_LineLength)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_NodeLineIndex)
		SHADER_PARAMETER(uint32, TargetCount)
		SHADER_PARAMETER(int32, BackGrowCount)
		SHADER_PARAMETER(int32, ForkTaperForkOrdinal)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

class FSpaceColonizationPrefixSumLinesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSpaceColonizationPrefixSumLinesCS);
	SHADER_USE_PARAMETER_STRUCT(FSpaceColonizationPrefixSumLinesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, LineCounter)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, LineLength)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_LineOffset)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_LineCountsOut)
		SHADER_PARAMETER(uint32, TargetCount)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

class FSpaceColonizationEmitLinesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSpaceColonizationEmitLinesCS);
	SHADER_USE_PARAMETER_STRUCT(FSpaceColonizationEmitLinesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, TargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, State0)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, State1)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BranchOrder)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, TargetPointScales)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, StartSourceScales)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NodeLineIndex)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, LineOffset)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_PathPoints)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int4>, RW_PathPointMeta)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int4>, RW_SegmentMeta)
		SHADER_PARAMETER(uint32, TargetCount)
		SHADER_PARAMETER(int32, BackGrowCount)
		SHADER_PARAMETER(int32, ForkTaperForkOrdinal)
		SHADER_PARAMETER(uint32, PathPointCapacity)
		SHADER_PARAMETER(uint32, SegmentCapacity)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

class FSpaceColonizationSmoothLinesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSpaceColonizationSmoothLinesCS);
	SHADER_USE_PARAMETER_STRUCT(FSpaceColonizationSmoothLinesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, SmoothInPoints)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, SmoothMeta)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, SmoothCounts)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_SmoothOutPoints)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

class FSpaceColonizationCountResampleCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSpaceColonizationCountResampleCS);
	SHADER_USE_PARAMETER_STRUCT(FSpaceColonizationCountResampleCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, ResampleInPoints)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ResampleLineOffset)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ResampleLineLength)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ResampleLineCount)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_NewLineLength)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_NewSegLength)
		SHADER_PARAMETER(float, ResampleLength)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

class FSpaceColonizationPrefixResampleCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSpaceColonizationPrefixResampleCS);
	SHADER_USE_PARAMETER_STRUCT(FSpaceColonizationPrefixResampleCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ResampleLineCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NewLineLength)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NewSegLength)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_NewLineOffset)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_NewSegOffset)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_NewCounts)
		SHADER_PARAMETER(uint32, TargetCount)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

class FSpaceColonizationEmitResampleCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSpaceColonizationEmitResampleCS);
	SHADER_USE_PARAMETER_STRUCT(FSpaceColonizationEmitResampleCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, ResampleInPoints)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ResampleLineOffset)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ResampleLineLength)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ResampleLineCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NewLineOffset)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NewSegOffset)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_PathPoints2)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int4>, RW_PathPointMeta2)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int4>, RW_SegmentMeta2)
		SHADER_PARAMETER(float, ResampleLength)
		SHADER_PARAMETER(uint32, PathPoint2Capacity)
		SHADER_PARAMETER(uint32, Segment2Capacity)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

class FSpaceColonizationCurveCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSpaceColonizationCurveCS);
	SHADER_USE_PARAMETER_STRUCT(FSpaceColonizationCurveCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, CurveLUT)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CurveLineOffset)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CurveLineLength)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CurveLineCount)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_CurvePoints)
		SHADER_PARAMETER(uint32, CurveLUTSize)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

// Trip A scatter: per-point +/-ScatterDistance jitter applied before the smooth pass,
// porting ApplyVVSCPointOffset for visual parity with the CPU path.
class FSpaceColonizationScatterCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSpaceColonizationScatterCS);
	SHADER_USE_PARAMETER_STRUCT(FSpaceColonizationScatterCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_ScatterPoints)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ScatterCounts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, ScatterSource)
		SHADER_PARAMETER(float, ScatterDistance)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

// Concat prefix sum: turns the per-source compact counts (GPU-only) into per-source destination
// bases plus the batch totals that drive every downstream dispatch.
class FConcatPrefixSumCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FConcatPrefixSumCS);
	SHADER_USE_PARAMETER_STRUCT(FConcatPrefixSumCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ConcatSourceCounts)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_ConcatBases)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_ConcatTotals)
		SHADER_PARAMETER(uint32, ConcatSourceCount)
		SHADER_PARAMETER(uint32, ConcatPointCapacity)
		SHADER_PARAMETER(uint32, ConcatSegmentCapacity)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

// Concat: offset one source's int4 meta/segment records by its GPU-computed point base into
// the concatenated destination. Serves both PathPointMeta and SegmentMeta.
class FConcatOffsetInt4CS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FConcatOffsetInt4CS);
	SHADER_USE_PARAMETER_STRUCT(FConcatOffsetInt4CS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, ConcatSrcInt4)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ConcatBases)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int4>, RW_ConcatDstInt4)
		SHADER_PARAMETER(uint32, ConcatSourceIndex)
		SHADER_PARAMETER(uint32, ConcatRecordKind)
		SHADER_PARAMETER(uint32, ConcatCapacity)
		SHADER_PARAMETER(FIntVector4, ConcatOffsetMask)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

// Concat: relocate one source's float4 path points to their GPU-computed destination base.
class FConcatCopyFloat4CS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FConcatCopyFloat4CS);
	SHADER_USE_PARAMETER_STRUCT(FConcatCopyFloat4CS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, ConcatSrcFloat4)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ConcatBases)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_ConcatDstFloat4)
		SHADER_PARAMETER(uint32, ConcatSourceIndex)
		SHADER_PARAMETER(uint32, ConcatRecordKind)
		SHADER_PARAMETER(uint32, ConcatCapacity)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

IMPLEMENT_GLOBAL_SHADER(FSpaceColonizationQueueInitCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "InitializeSpaceColonizationQueueCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSpaceColonizationQueueMarkSourcesCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "MarkSpaceColonizationSourcesCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSpaceColonizationQueueBuildNeighborsCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "BuildSpaceColonizationNeighborsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSpaceColonizationQueueResetProposalsCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "ResetSpaceColonizationProposalsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSpaceColonizationQueueClaimCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "ClaimSpaceColonizationAssociatesCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSpaceColonizationQueueProposeCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "ProposeSpaceColonizationGrowthCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSpaceColonizationQueueCommitParentsCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "CommitSpaceColonizationParentsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSpaceColonizationQueueCommitChildrenCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "CommitSpaceColonizationChildrenCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSpaceColonizationBranchOrderCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "BuildSpaceColonizationBranchOrderCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSpaceColonizationCountLinesCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "CountSpaceColonizationLinesCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSpaceColonizationPrefixSumLinesCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "PrefixSumSpaceColonizationLinesCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSpaceColonizationEmitLinesCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "EmitSpaceColonizationLinesCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSpaceColonizationSmoothLinesCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "SmoothSpaceColonizationLinesCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSpaceColonizationCountResampleCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "CountResampleSpaceColonizationCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSpaceColonizationPrefixResampleCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "PrefixResampleSpaceColonizationCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSpaceColonizationEmitResampleCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "EmitResampleSpaceColonizationCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSpaceColonizationCurveCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "CurveSpaceColonizationCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSpaceColonizationScatterCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "ScatterSpaceColonizationCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FConcatPrefixSumCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "ConcatPrefixSumCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FConcatOffsetInt4CS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "ConcatOffsetInt4CS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FConcatCopyFloat4CS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "ConcatCopyFloat4CS", SF_Compute);


// One space-colonization source's CPU-prepped inputs. The solve itself is recorded into the vine
// mesh graph, so nothing here is a GPU resource — just the arrays the SC passes upload.
struct FVineSCPreparedSource
{
	// Single entry: xyz = world location, w = the source transform's scale. Kept as an array
	// because the SC pass uploads it as a structured buffer.
	TArray<FVector4f> SourcePositions;
	// Per target; 1.0 everywhere except the target nearest this source, which carries its scale.
	TArray<float> StartSourceScales;
	// This source's share of the batch-wide VV.MaxVinePointCount, and the SC buffers' capacity.
	uint32 PointCapacity = 0u;
};

// Everything the fused space-colonization + concat passes need, prepped on the game thread and
// carried inside FVineBuildInput. Replaces the old per-source pooled buffer hand-off: the solve
// now runs inside FVineMeshSceneProxy::BuildGeometry's single graph, so its output never has to
// outlive a graph and its counts never come back to the CPU.
// Defined at global scope (matches the header forward declaration; must not land inside the
// anonymous namespace below or the member-function signatures won't match).
struct FVineFusedSCInputs
{
	TArray<FVineSCPreparedSource> Sources;
	TArray<FVector4f> InitialTargetPositions; // xyz = world location, w = target transform scale
	TArray<float> TargetPointScales;
	TArray<float> CurveLUT;                   // baked VV.CurveControl.G, drives the tube taper

	int32 Iteration = 0;
	int32 Activetime = 0;
	float RandGrow = 0.0f;
	float Seed = 0.0f;
	float InfluenceRadius = 0.0f;
	int32 BackGrowCount = 0;
	int32 ForkTaperForkOrdinal = 0;
	float ResampleLength = 1.0f;
	float ScatterDistance = 0.0f;

	// Concat destination capacity == sum of the per-source point capacities. Segments track points.
	uint32 TotalPointCapacity = 0u;
	uint32 TotalSegmentCapacity = 0u;

	bool IsValid() const
	{
		return Sources.Num() > 0 && InitialTargetPositions.Num() > 0 && TotalPointCapacity > 0u;
	}
};

// Target-position spatial acceleration buffers for the vine surface projection. Global scope
// (matches FVineFusedSCInputs) so the self-owning FVineBuildInput bundle below can embed it and
// still be named from the leaf's public header via a forward declaration.
struct FVineTargetBucketBuffers
{
	TArray<FIntVector4> Ranges;
	TArray<uint32> RangeCounts;
	TArray<uint32> VoxelIndices;
	TArray<uint32> HashSlots;
	uint32 HashSlotCount = 0u;
	uint32 BucketCount = 0u;
	uint32 MaxBucketItemCount = 0u;
	float BucketSize = 0.0f;
	uint32 SearchRadius = 4u;
};

// Self-owning CPU-prep bundle consumed by the GPU-resident leaf (UVineMeshComponent /
// FVineMeshSceneProxy). It OWNS every CPU array that
// FVineMeshPassInputs' raw pointers reference (so those pointers stay valid for the lifetime of
// the bundle), the pooled GPU line/voxel refs, all scalar pass parameters, VineWorldToLocal, and
// a conservative world-space bounds. Copyable + movable. Produced by VineLeaf_BuildVineBuildInput
// and consumed via VineLeaf_MakePassInputs.
struct FVineBuildInput
{
	// Line geometry CPU-fallback arrays (empty on the fused GPU-lines path).
	TArray<FVector4f> PathPoints;
	TArray<FVector4f> PathPointAxes;
	TArray<FIntVector4> PathPointMeta;
	TArray<FIntVector4> SegmentMeta;

	// Repacked surface-voxel arrays (CPU-fallback upload + target-bucket source).
	TArray<FIntVector4> GPUVoxelCells;
	TArray<uint32> GPUVoxelHashSlots;
	TArray<FVector4f> GPUVoxelNormals;
	TArray<FVector4f> GPUVoxelTargetPositions;
	FVineTargetBucketBuffers TargetBuckets;
	FVector3f TargetBucketOrigin = FVector3f::ZeroVector;

	// Fused space-colonization inputs. When valid, the line geometry is solved and concatenated
	// inside the leaf's own graph instead of arriving as pooled buffers.
	bool bUseGPULines = false;
	FVineFusedSCInputs FusedSC;
	bool bUseGPUVoxels = false;
	TRefCountPtr<FRDGPooledBuffer> GPUVoxCells;
	TRefCountPtr<FRDGPooledBuffer> GPUVoxNormals;
	TRefCountPtr<FRDGPooledBuffer> GPUVoxTargets;
	TRefCountPtr<FRDGPooledBuffer> GPUVoxCounter;
	uint32 GPUVoxCount = 0u;
	uint32 GpuVoxelHashSlotCountPow2 = 0u;

	// Fused surface-voxel inputs. When set, the voxels are built inside the leaf's own graph
	// (AddCSSurfaceVoxelPasses) instead of arriving as pooled buffers from a separate graph, so
	// the GPUVox* pooled refs above stay null and the pass graph reads the in-graph refs instead.
	bool bUseFusedVoxels = false;
	FCSSurfaceVoxelPassInputs SurfaceVoxelInputs;

	// Scalars / counts (every value the pass graph reads).
	FVector3f VoxelOrigin = FVector3f::ZeroVector;
	float VoxelSize = 0.0f;
	uint32 VoxelCount = 0u;
	uint32 GPUVoxelHashSlotCount = 0u;
	uint32 PathPointCount = 0u;
	uint32 SegmentCount = 0u;
	uint32 ProfileCount = 0u;
	uint32 OutputVertexCount = 0u;
	uint32 OutputIndexCount = 0u;
	bool bTube = false;
	float CircleScale = 0.0f;
	float LineScale = 0.0f;
	float VinesOffset = 0.0f;
	float TinyZJitterStrength = 0.0f;
	int32 SafePostProjectionSmoothIterations = 0;
	int32 SafePostProjectionSmoothKernelRadius = 0;
	int32 SafePostProjectionSmallSmoothIterations = 0;
	float SafePostProjectionSmoothAngleStrength = 0.0f;
	bool bResampleSurface = false;
	float ResampleTargetDistance = 0.0f;
	float CurlNoiseStrength = 0.0f;
	float CurlNoiseFrequency = 0.0f;
	float PerlinNoiseStrength = 0.0f;
	float PerlinNoiseFrequency = 0.0f;
	uint32 SafeNoiseIterations = 0u;
	// Raw VV.UVLengthScale for the base-stream axial-V GPU passes (max()'d in the scan). Unused on
	// the legacy readback path (bBaseStreams=false), where V is recomputed on the CPU instead.
	float UVLengthScale = 1.0f;
	EVisVineGPUDebugStage DebugStage = EVisVineGPUDebugStage::Smooth;
	FLinearColor DebugLineColor = FLinearColor(0.0f, 1.0f, 0.35f, 1.0f);

	// Identity for M1: the vine renders in world space and the shader does not rotate normals by
	// this matrix, so it must stay identity (the leaf keeps an identity world transform).
	FMatrix44f VineWorldToLocal = FMatrix44f::Identity;

	// Conservative world-space bounds for the leaf (renders at identity transform => world == local).
	FBox LocalBounds = FBox(ForceInit);

	// False when the inputs cannot produce a mesh (zero counts / no voxels).
	bool bValid = false;
};

namespace
{

static FGeometryScriptPolyPath ClonePolyPath(const FGeometryScriptPolyPath& Source)
{
	FGeometryScriptPolyPath Result;
	Result.Reset();
	if (Source.Path.IsValid())
	{
		*Result.Path = *Source.Path;
	}
	return Result;
}

static double GetVinePolyPathLength(const FGeometryScriptPolyPath& Line)
{
	if (!Line.Path.IsValid())
	{
		return 0.0;
	}

	const TArray<FVector>& Points = *Line.Path;
	double Length = 0.0;
	for (int32 PointIndex = 1; PointIndex < Points.Num(); ++PointIndex)
	{
		Length += FVector::Dist(Points[PointIndex - 1], Points[PointIndex]);
	}
	return Length;
}

static float GetVineTransformScale(const FTransform& Transform)
{
	const FVector Scale = Transform.GetScale3D();
	return FMath::Max3(FMath::Abs(Scale.X), FMath::Abs(Scale.Y), FMath::Abs(Scale.Z));
}

static uint32 HashQuantizedVineCoordinate(double Value)
{
	const uint64 Quantized = uint64(FMath::RoundToInt64(Value * 1000.0));
	return HashCombine(uint32(Quantized), uint32(Quantized >> 32));
}

static uint32 BuildVVRandomSeed(const FVector& SourceLocation, const FVector& PointLocation)
{
	uint32 Seed = 0x9e3779b9u;
	Seed = HashCombine(Seed, HashQuantizedVineCoordinate(SourceLocation.X));
	Seed = HashCombine(Seed, HashQuantizedVineCoordinate(SourceLocation.Y));
	Seed = HashCombine(Seed, HashQuantizedVineCoordinate(SourceLocation.Z));
	Seed = HashCombine(Seed, HashQuantizedVineCoordinate(PointLocation.X));
	Seed = HashCombine(Seed, HashQuantizedVineCoordinate(PointLocation.Y));
	Seed = HashCombine(Seed, HashQuantizedVineCoordinate(PointLocation.Z));
	return Seed == 0u ? 1u : Seed;
}

static FVector GetVVSCPointOffset(const FVector& SourceLocation, const FVector& PointLocation)
{
	constexpr float OffsetDistance = 10.0f;
	FRandomStream RandomStream(int32(BuildVVRandomSeed(SourceLocation, PointLocation)));
	return RandomStream.VRand() * OffsetDistance;
}

static void ApplyVVSCPointOffset(FGeometryScriptPolyPath& Line, const FVector& SourceLocation)
{
	if (!Line.Path.IsValid())
	{
		return;
	}

	for (FVector& Point : *Line.Path)
	{
		Point += GetVVSCPointOffset(SourceLocation, Point);
	}
}

static bool IsFiniteVector(const FVector& Vector)
{
	return FMath::IsFinite(Vector.X) && FMath::IsFinite(Vector.Y) && FMath::IsFinite(Vector.Z);
}

static float SampleScaleArrayByAlpha(const TArray<float>& Scales, double Alpha, float FallbackScale)
{
	if (Scales.Num() == 0)
	{
		return FallbackScale;
	}

	if (Scales.Num() == 1)
	{
		return Scales[0];
	}

	const double ClampedAlpha = FMath::Clamp(Alpha, 0.0, 1.0);
	const double ScaledIndex = ClampedAlpha * double(Scales.Num() - 1);
	const int32 IndexA = FMath::Clamp(FMath::FloorToInt(ScaledIndex), 0, Scales.Num() - 1);
	const int32 IndexB = FMath::Min(IndexA + 1, Scales.Num() - 1);
	return FMath::Lerp(Scales[IndexA], Scales[IndexB], float(ScaledIndex - double(IndexA)));
}

static void BuildPreparedLinePointScales(
	const FGeometryScriptPolyPath& SourceLine,
	const TArray<float>* SourcePointScales,
	int32 OutputPointCount,
	float FallbackScale,
	TArray<float>& OutPointScales)
{
	OutPointScales.Reset();
	if (OutputPointCount <= 0)
	{
		return;
	}

	OutPointScales.SetNumUninitialized(OutputPointCount);
	if (!SourcePointScales || SourcePointScales->Num() == 0)
	{
		for (float& PointScale : OutPointScales)
		{
			PointScale = FallbackScale;
		}
		return;
	}

	const int32 SourceScaleCount = SourcePointScales->Num();
	if (SourceScaleCount == 1)
	{
		for (float& PointScale : OutPointScales)
		{
			PointScale = (*SourcePointScales)[0];
		}
		return;
	}

	if (SourceLine.Path.IsValid() && SourceLine.Path->Num() == SourceScaleCount && SourceScaleCount > 1)
	{
		const TArray<FVector>& SourcePoints = *SourceLine.Path;
		TArray<double> CumulativeLengths;
		CumulativeLengths.SetNumZeroed(SourceScaleCount);
		for (int32 PointIndex = 1; PointIndex < SourceScaleCount; ++PointIndex)
		{
			CumulativeLengths[PointIndex] = CumulativeLengths[PointIndex - 1] + FVector::Dist(SourcePoints[PointIndex - 1], SourcePoints[PointIndex]);
		}

		const double TotalLength = CumulativeLengths.Last();
		if (TotalLength > UE_SMALL_NUMBER)
		{
			int32 SourceSegmentIndex = 0;
			for (int32 OutputIndex = 0; OutputIndex < OutputPointCount; ++OutputIndex)
			{
				const double Alpha = OutputPointCount > 1 ? double(OutputIndex) / double(OutputPointCount - 1) : 0.0;
				const double TargetLength = Alpha * TotalLength;
				while (SourceSegmentIndex + 1 < SourceScaleCount && CumulativeLengths[SourceSegmentIndex + 1] < TargetLength)
				{
					++SourceSegmentIndex;
				}

				if (SourceSegmentIndex + 1 >= SourceScaleCount)
				{
					OutPointScales[OutputIndex] = (*SourcePointScales)[SourceScaleCount - 1];
					continue;
				}

				const double SegmentLength = CumulativeLengths[SourceSegmentIndex + 1] - CumulativeLengths[SourceSegmentIndex];
				const double SegmentAlpha = SegmentLength > UE_SMALL_NUMBER ? (TargetLength - CumulativeLengths[SourceSegmentIndex]) / SegmentLength : 0.0;
				OutPointScales[OutputIndex] = FMath::Lerp((*SourcePointScales)[SourceSegmentIndex], (*SourcePointScales)[SourceSegmentIndex + 1], float(SegmentAlpha));
			}
			return;
		}
	}

	for (int32 OutputIndex = 0; OutputIndex < OutputPointCount; ++OutputIndex)
	{
		const double Alpha = OutputPointCount > 1 ? double(OutputIndex) / double(OutputPointCount - 1) : 0.0;
		OutPointScales[OutputIndex] = SampleScaleArrayByAlpha(*SourcePointScales, Alpha, FallbackScale);
	}
}

static void RebuildVinePointScalesForEditedLine(
	const FGeometryScriptPolyPath& PreviousLine,
	const FGeometryScriptPolyPath& NewLine,
	float FallbackScale,
	TArray<float>& PointScales)
{
	TArray<float> NewPointScales;
	const int32 NewPointCount = NewLine.Path.IsValid() ? NewLine.Path->Num() : 0;
	const TArray<float>* ExistingPointScales = PointScales.Num() > 0 ? &PointScales : nullptr;
	BuildPreparedLinePointScales(PreviousLine, ExistingPointScales, NewPointCount, FallbackScale, NewPointScales);
	PointScales = MoveTemp(NewPointScales);
}

static bool IsFiniteVineVector(const FVector& Vector);

static FVector NormalizeVineDirectionOrFallback(const FVector& Direction, const FVector& FallbackDirection)
{
	FVector Normalized = Direction;
	if (IsFiniteVineVector(Normalized) && Normalized.Normalize()) return Normalized;

	FVector Fallback = FallbackDirection;
	if (IsFiniteVineVector(Fallback) && Fallback.Normalize()) return Fallback;
	return FVector::UpVector;
}

static FVector LerpVineDirection(const FVector& A, const FVector& B, float Alpha, const FVector& FallbackDirection)
{
	const FVector SafeA = NormalizeVineDirectionOrFallback(A, FallbackDirection);
	FVector SafeB = NormalizeVineDirectionOrFallback(B, SafeA);
	if (FVector::DotProduct(SafeA, SafeB) < 0.0) SafeB = -SafeB;
	return NormalizeVineDirectionOrFallback(FMath::Lerp(SafeA, SafeB, Alpha), SafeA);
}

static FVector SampleDirectionArrayByAlpha(const TArray<FVector>& Directions, double Alpha, const FVector& FallbackDirection)
{
	if (Directions.Num() == 0) return NormalizeVineDirectionOrFallback(FallbackDirection, FVector::UpVector);
	if (Directions.Num() == 1) return NormalizeVineDirectionOrFallback(Directions[0], FallbackDirection);

	const double ClampedAlpha = FMath::Clamp(Alpha, 0.0, 1.0);
	const double ScaledIndex = ClampedAlpha * double(Directions.Num() - 1);
	const int32 IndexA = FMath::Clamp(FMath::FloorToInt(ScaledIndex), 0, Directions.Num() - 1);
	const int32 IndexB = FMath::Min(IndexA + 1, Directions.Num() - 1);
	return LerpVineDirection(Directions[IndexA], Directions[IndexB], float(ScaledIndex - double(IndexA)), FallbackDirection);
}

static void BuildPreparedLineFrameNormals(
	const FGeometryScriptPolyPath& SourceLine,
	const TArray<FVector>* SourceFrameNormals,
	int32 OutputPointCount,
	TArray<FVector>& OutFrameNormals)
{
	OutFrameNormals.Reset();
	if (OutputPointCount <= 0) return;

	OutFrameNormals.SetNumUninitialized(OutputPointCount);
	if (!SourceFrameNormals || SourceFrameNormals->Num() == 0)
	{
		for (FVector& FrameNormal : OutFrameNormals)
		{
			FrameNormal = FVector::UpVector;
		}
		return;
	}

	const int32 SourceNormalCount = SourceFrameNormals->Num();
	if (SourceNormalCount == 1)
	{
		const FVector SafeNormal = NormalizeVineDirectionOrFallback((*SourceFrameNormals)[0], FVector::UpVector);
		for (FVector& FrameNormal : OutFrameNormals)
		{
			FrameNormal = SafeNormal;
		}
		return;
	}

	if (SourceLine.Path.IsValid() && SourceLine.Path->Num() == SourceNormalCount && SourceNormalCount > 1)
	{
		const TArray<FVector>& SourcePoints = *SourceLine.Path;
		TArray<double> CumulativeLengths;
		CumulativeLengths.SetNumZeroed(SourceNormalCount);
		for (int32 PointIndex = 1; PointIndex < SourceNormalCount; ++PointIndex)
		{
			CumulativeLengths[PointIndex] = CumulativeLengths[PointIndex - 1] + FVector::Dist(SourcePoints[PointIndex - 1], SourcePoints[PointIndex]);
		}

		const double TotalLength = CumulativeLengths.Last();
		if (TotalLength > UE_SMALL_NUMBER)
		{
			int32 SourceSegmentIndex = 0;
			for (int32 OutputIndex = 0; OutputIndex < OutputPointCount; ++OutputIndex)
			{
				const double Alpha = OutputPointCount > 1 ? double(OutputIndex) / double(OutputPointCount - 1) : 0.0;
				const double TargetLength = Alpha * TotalLength;
				while (SourceSegmentIndex + 1 < SourceNormalCount && CumulativeLengths[SourceSegmentIndex + 1] < TargetLength)
				{
					++SourceSegmentIndex;
				}

				if (SourceSegmentIndex + 1 >= SourceNormalCount)
				{
					OutFrameNormals[OutputIndex] = NormalizeVineDirectionOrFallback((*SourceFrameNormals)[SourceNormalCount - 1], FVector::UpVector);
					continue;
				}

				const double SegmentLength = CumulativeLengths[SourceSegmentIndex + 1] - CumulativeLengths[SourceSegmentIndex];
				const double SegmentAlpha = SegmentLength > UE_SMALL_NUMBER ? (TargetLength - CumulativeLengths[SourceSegmentIndex]) / SegmentLength : 0.0;
				OutFrameNormals[OutputIndex] = LerpVineDirection(
					(*SourceFrameNormals)[SourceSegmentIndex],
					(*SourceFrameNormals)[SourceSegmentIndex + 1],
					float(SegmentAlpha),
					FVector::UpVector);
			}
			return;
		}
	}

	for (int32 OutputIndex = 0; OutputIndex < OutputPointCount; ++OutputIndex)
	{
		const double Alpha = OutputPointCount > 1 ? double(OutputIndex) / double(OutputPointCount - 1) : 0.0;
		OutFrameNormals[OutputIndex] = SampleDirectionArrayByAlpha(*SourceFrameNormals, Alpha, FVector::UpVector);
	}
}

static void RebuildVineFrameNormalsForEditedLine(
	const FGeometryScriptPolyPath& PreviousLine,
	const FGeometryScriptPolyPath& NewLine,
	TArray<FVector>& FrameNormals)
{
	TArray<FVector> NewFrameNormals;
	const int32 NewPointCount = NewLine.Path.IsValid() ? NewLine.Path->Num() : 0;
	const TArray<FVector>* ExistingFrameNormals = FrameNormals.Num() > 0 ? &FrameNormals : nullptr;
	BuildPreparedLineFrameNormals(PreviousLine, ExistingFrameNormals, NewPointCount, NewFrameNormals);
	FrameNormals = MoveTemp(NewFrameNormals);
}

static uint32 BuildVVPointSortKey(const FVector& Point)
{
	return BuildVVRandomSeed(FVector::ZeroVector, Point);
}

static float GetVVTinyZJitter(const FVector& Point, int32 PointIndex)
{
	const FVector IndexSeed(double(PointIndex), double(PointIndex) * 0.37, double(PointIndex) * 0.11);
	const uint32 Seed = BuildVVRandomSeed(IndexSeed, Point);
	return (float(Seed & 0xffffu) / float(0xffffu)) * 0.1f;
}

static bool IsFiniteVineVector(const FVector& Vector)
{
	return FMath::IsFinite(Vector.X) && FMath::IsFinite(Vector.Y) && FMath::IsFinite(Vector.Z);
}


static bool PrepareVVLinesProjected(
	const TArray<FGeometryScriptPolyPath>& Lines,
	const FVV& VV,
	const TArray<float>& InLineSourceScales,
	const TArray<FVector>& InLineSourceLocations,
	const TArray<FVineLinePointScaleData>& InLinePointScales,
	const TCHAR* ProjectionLabel,
	TFunctionRef<bool(const FVector& Query, FVector& OutProjected, FVector& OutNormal)> ProjectSurfacePoint,
	bool bApplyVinesOffset,
	TArray<FGeometryScriptPolyPath>& OutLines,
	TArray<float>& OutLineSourceScales,
	TArray<FVineLinePointScaleData>& OutLinePointScales,
	TArray<TArray<FVector>>& OutLineFrameNormals)
{
	const TCHAR* SafeProjectionLabel = ProjectionLabel ? ProjectionLabel : TEXT("Unknown");
	const TCHAR* VineKindLabel = TEXT("tube");
	const double PrepStartSeconds = FPlatformTime::Seconds();
	double BuildWorkingMs = 0.0;
	double NoiseAndProjectMs = 0.0;
	double ReduceSampleMs = 0.0;
	double MergeAndFinalProjectMs = 0.0;

	OutLines.Reset();
	OutLines.Reserve(Lines.Num());
	OutLineSourceScales.Reset();
	OutLineSourceScales.Reserve(Lines.Num());
	OutLinePointScales.Reset();
	OutLinePointScales.Reserve(Lines.Num());
	OutLineFrameNormals.Reset();
	OutLineFrameNormals.Reserve(Lines.Num());

	if (VV.ResampleLength <= KINDA_SMALL_NUMBER)
	{
		return false;
	}

	const int32 SafeCPUPostProjectionSmoothIterations = FMath::Max(0, 1);

	TArray<FGeometryScriptPolyPath> WorkingLines;
	WorkingLines.Reserve(Lines.Num());
	TArray<float> WorkingLineSourceScales;
	WorkingLineSourceScales.Reserve(Lines.Num());
	TArray<FVineLinePointScaleData> WorkingLinePointScales;
	WorkingLinePointScales.Reserve(Lines.Num());

	const double BuildWorkingStartSeconds = FPlatformTime::Seconds();
	for (int32 LineIdx = 0; LineIdx < Lines.Num(); ++LineIdx)
	{
		const FGeometryScriptPolyPath& InputLine = Lines[LineIdx];
		if (!InputLine.Path.IsValid() || InputLine.Path->Num() < 2)
		{
			continue;
		}

		FGeometryScriptPolyPath Line = ClonePolyPath(InputLine);
		if (InLineSourceLocations.IsValidIndex(LineIdx))
		{
			ApplyVVSCPointOffset(Line, InLineSourceLocations[LineIdx]);
		}

		const float FallbackScale = InLineSourceScales.IsValidIndex(LineIdx) ? InLineSourceScales[LineIdx] : 1.0f;
		const TArray<float>* InputPointScales = InLinePointScales.IsValidIndex(LineIdx) ? &InLinePointScales[LineIdx].Values : nullptr;
		WorkingLines.Add(Line);
		WorkingLineSourceScales.Add(FallbackScale);
		FVineLinePointScaleData& WorkingScaleData = WorkingLinePointScales.AddDefaulted_GetRef();
		BuildPreparedLinePointScales(Line, InputPointScales, Line.Path->Num(), FallbackScale, WorkingScaleData.Values);
	}
	BuildWorkingMs = (FPlatformTime::Seconds() - BuildWorkingStartSeconds) * 1000.0;

	if (WorkingLines.Num() == 0)
	{
		return false;
	}

	TArray<FGeometryScriptPolyPath> NoiseLines;
	NoiseLines.Reserve(WorkingLines.Num());
	TArray<float> NoiseLineSourceScales;
	NoiseLineSourceScales.Reserve(WorkingLines.Num());
	TArray<FVineLinePointScaleData> NoiseLinePointScales;
	NoiseLinePointScales.Reserve(WorkingLines.Num());

	int32 ProjectionAttempts = 0;
	int32 ProjectionHits = 0;
	double ProjectionDistanceSum = 0.0;
	double ProjectionDistanceMax = 0.0;
	double ProjectionMs = 0.0;
	auto ProjectVinePoint = [&](const FVector& Query, FVector& OutProjected, FVector& OutNormal)
	{
		++ProjectionAttempts;
		const double ProjectionStartSeconds = FPlatformTime::Seconds();
		const bool bProjectionHit = ProjectSurfacePoint(Query, OutProjected, OutNormal);
		ProjectionMs += (FPlatformTime::Seconds() - ProjectionStartSeconds) * 1000.0;
		if (bProjectionHit)
		{
			++ProjectionHits;
			const double Distance = FVector::Dist(Query, OutProjected);
			ProjectionDistanceSum += Distance;
			ProjectionDistanceMax = FMath::Max(ProjectionDistanceMax, Distance);
			return true;
		}
		return false;
	};

	TArray<FVector> SampleRangePointsSum;
	const double NoiseAndProjectStartSeconds = FPlatformTime::Seconds();
	for (int32 LineIdx = 0; LineIdx < WorkingLines.Num(); ++LineIdx)
	{
		FGeometryScriptPolyPath Line = ClonePolyPath(WorkingLines[LineIdx]);
		if (!Line.Path.IsValid())
		{
			continue;
		}

		TArray<float> CurrentPointScales = WorkingLinePointScales[LineIdx].Values;
		const float FallbackScale = WorkingLineSourceScales.IsValidIndex(LineIdx) ? WorkingLineSourceScales[LineIdx] : 1.0f;
		const float ArcLength = float(GetVinePolyPathLength(Line));
		const int32 NumIterations = int32(ArcLength / VV.ResampleLength);
		if (NumIterations < 2)
		{
			continue;
		}

		FGeometryScriptPolyPath PreviousLine = ClonePolyPath(Line);
		Line = UPolyLine::SmoothLine(Line, 3);
		RebuildVinePointScalesForEditedLine(PreviousLine, Line, FallbackScale, CurrentPointScales);
		PreviousLine = ClonePolyPath(Line);
		Line = UPolyLine::ResamppleByLength(Line, VV.ResampleLength);
		RebuildVinePointScalesForEditedLine(PreviousLine, Line, FallbackScale, CurrentPointScales);
		if (!Line.Path.IsValid() || Line.Path->Num() < 3)
		{
			continue;
		}

		for (int32 IterationIndex = 0; IterationIndex < 10; ++IterationIndex)
		{
			const int32 VertexCount = Line.Path->Num();
			for (int32 PointIndex = 0; PointIndex < VertexCount; ++PointIndex)
			{
				FVector& VertexLocation = (*Line.Path)[PointIndex];
				FVector ProjectedPos;
				FVector ProjectedNormal;
				if (ProjectVinePoint(VertexLocation, ProjectedPos, ProjectedNormal))
				{
					VertexLocation = ProjectedPos;
				}

				UNoise::CurlNoise(VertexLocation, VertexLocation, FVector::ZeroVector, VV.CurlNoiseScale / 10.0f, VV.CurlNoiseFre);
				const FVector NoisePos = (VV.PerlinNoiseFre / 100.0f) * VertexLocation;
				const float OffsetNoise = VV.PerlinNoiseScale * FMath::PerlinNoise3D(NoisePos);
				const float PerlinOffset = VV.CurveControl ? VV.CurveControl->GetUnadjustedLinearColorValue(PointIndex / double(VertexCount - 1)).R : 0.0f;
			}

			PreviousLine = ClonePolyPath(Line);
			Line = UPolyLine::ResamppleByLength(Line, VV.ResampleLength);
			RebuildVinePointScalesForEditedLine(PreviousLine, Line, FallbackScale, CurrentPointScales);
			if (!Line.Path.IsValid() || Line.Path->Num() < 3)
			{
				break;
			}
		}

		if (!Line.Path.IsValid() || Line.Path->Num() < 3)
		{
			continue;
		}

		for (FVector& VertexLocation : *Line.Path)
		{
			UNoise::CurlNoise(VertexLocation, VertexLocation, FVector::ZeroVector, VV.CurlNoiseScale / 10.0f, VV.CurlNoiseFre);
		}

		const int32 SampleCount = FMath::Clamp(FMath::FloorToInt(float(Line.Path->Num()) * 0.8f), 0, Line.Path->Num());
		for (int32 PointIndex = 0; PointIndex < SampleCount; ++PointIndex)
		{
			SampleRangePointsSum.Add((*Line.Path)[PointIndex]);
		}

		NoiseLines.Add(Line);
		NoiseLineSourceScales.Add(FallbackScale);
		FVineLinePointScaleData& NoiseScaleData = NoiseLinePointScales.AddDefaulted_GetRef();
		NoiseScaleData.Values = MoveTemp(CurrentPointScales);
	}
	NoiseAndProjectMs = (FPlatformTime::Seconds() - NoiseAndProjectStartSeconds) * 1000.0;

	if (NoiseLines.Num() == 0 || SampleRangePointsSum.Num() == 0)
	{
		return false;
	}

	const double ReduceSampleStartSeconds = FPlatformTime::Seconds();
	SampleRangePointsSum.Sort([](const FVector& A, const FVector& B)
	{
		const uint32 AKey = BuildVVPointSortKey(A);
		const uint32 BKey = BuildVVPointSortKey(B);
		if (AKey != BKey)
		{
			return AKey < BKey;
		}
		if (!FMath::IsNearlyEqual(A.X, B.X))
		{
			return A.X < B.X;
		}
		if (!FMath::IsNearlyEqual(A.Y, B.Y))
		{
			return A.Y < B.Y;
		}
		return A.Z < B.Z;
	});
	const int32 ReducedSampleCount = FMath::Max(1, SampleRangePointsSum.Num() / 15);
	SampleRangePointsSum.SetNum(ReducedSampleCount);
	ReduceSampleMs = (FPlatformTime::Seconds() - ReduceSampleStartSeconds) * 1000.0;

	const double MergeAndFinalProjectStartSeconds = FPlatformTime::Seconds();
	for (int32 LineIdx = 0; LineIdx < NoiseLines.Num(); ++LineIdx)
	{
		FGeometryScriptPolyPath Line = ClonePolyPath(NoiseLines[LineIdx]);
		if (!Line.Path.IsValid() || Line.Path->Num() < 3)
		{
			continue;
		}

		TArray<float> CurrentPointScales = NoiseLinePointScales[LineIdx].Values;
		const float FallbackScale = NoiseLineSourceScales.IsValidIndex(LineIdx) ? NoiseLineSourceScales[LineIdx] : 1.0f;

		const int32 VertexCount = Line.Path->Num();
		for (int32 PointIndex = 0; PointIndex < VertexCount; ++PointIndex)
		{
			const FVector VertexLocation = (*Line.Path)[PointIndex];
			FVector ProjectedPos;
			FVector ProjectedNormal;
			if (!ProjectVinePoint(VertexLocation, ProjectedPos, ProjectedNormal))
			{
				continue;
			}

			FVector& VertexLocationFix = (*Line.Path)[PointIndex];
			VertexLocationFix = ProjectedPos;
			if (bApplyVinesOffset)
			{
				VertexLocationFix += ProjectedNormal * GetVVTinyZJitter(VertexLocation, PointIndex);
			}
		}

		FGeometryScriptPolyPath PreviousLine = ClonePolyPath(Line);
		Line = UPolyLine::ResamppleByLength(Line, VV.ResampleLength);
		RebuildVinePointScalesForEditedLine(PreviousLine, Line, FallbackScale, CurrentPointScales);
		PreviousLine = ClonePolyPath(Line);
		Line = UPolyLine::SmoothLine(Line, 1);
		RebuildVinePointScalesForEditedLine(PreviousLine, Line, FallbackScale, CurrentPointScales);

		if (!Line.Path.IsValid() || Line.Path->Num() < 3)
		{
			continue;
		}

		TArray<FVector> CurrentFrameNormals;
		CurrentFrameNormals.SetNum(Line.Path->Num());
		for (int32 PointIndex = 0; PointIndex < Line.Path->Num(); ++PointIndex)
		{
			const FVector VertexLocation = (*Line.Path)[PointIndex];
			FVector ProjectedPos;
			FVector ProjectedNormal;
			if (!ProjectVinePoint(VertexLocation, ProjectedPos, ProjectedNormal))
			{
				CurrentFrameNormals[PointIndex] = PointIndex > 0 ? CurrentFrameNormals[PointIndex - 1] : FVector::UpVector;
				continue;
			}

			FVector& VertexLocationFix = (*Line.Path)[PointIndex];
			VertexLocationFix = bApplyVinesOffset ? ProjectedPos + ProjectedNormal * VV.VinesOffset : ProjectedPos;
			if (bApplyVinesOffset)
			{
				VertexLocationFix += ProjectedNormal * GetVVTinyZJitter(VertexLocation, PointIndex);
			}
			CurrentFrameNormals[PointIndex] = NormalizeVineDirectionOrFallback(ProjectedNormal, PointIndex > 0 ? CurrentFrameNormals[PointIndex - 1] : FVector::UpVector);
		}

		PreviousLine = ClonePolyPath(Line);
		Line = UPolyLine::ResamppleByLength(Line, VV.ResampleLength);
		RebuildVinePointScalesForEditedLine(PreviousLine, Line, FallbackScale, CurrentPointScales);
		RebuildVineFrameNormalsForEditedLine(PreviousLine, Line, CurrentFrameNormals);
		if (!Line.Path.IsValid() || Line.Path->Num() < 3)
		{
			continue;
		}

		if (SafeCPUPostProjectionSmoothIterations > 0)
		{
			Line = UPolyLine::SmoothLine(Line, SafeCPUPostProjectionSmoothIterations);
			if (!Line.Path.IsValid() || Line.Path->Num() < 3)
			{
				continue;
			}
		}

		if (CurrentPointScales.Num() != Line.Path->Num())
		{
			TArray<float> FixedPointScales;
			BuildPreparedLinePointScales(Line, &CurrentPointScales, Line.Path->Num(), FallbackScale, FixedPointScales);
			CurrentPointScales = MoveTemp(FixedPointScales);
		}

		if (CurrentFrameNormals.Num() != Line.Path->Num())
		{
			TArray<FVector> FixedFrameNormals;
			BuildPreparedLineFrameNormals(Line, &CurrentFrameNormals, Line.Path->Num(), FixedFrameNormals);
			CurrentFrameNormals = MoveTemp(FixedFrameNormals);
		}

		OutLines.Add(Line);
		OutLineSourceScales.Add(FallbackScale);
		FVineLinePointScaleData& OutScaleData = OutLinePointScales.AddDefaulted_GetRef();
		OutScaleData.Values = MoveTemp(CurrentPointScales);
		OutLineFrameNormals.Add(MoveTemp(CurrentFrameNormals));
	}
	MergeAndFinalProjectMs = (FPlatformTime::Seconds() - MergeAndFinalProjectStartSeconds) * 1000.0;

	if (ProjectionAttempts > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("[VisVinePrep] %s %s hits=%d/%d (%.1f%%) distAvg=%.3f distMax=%.3f outputLines=%d"),
			SafeProjectionLabel,
			VineKindLabel,
			ProjectionHits,
			ProjectionAttempts,
			100.0 * double(ProjectionHits) / double(ProjectionAttempts),
			ProjectionHits > 0 ? ProjectionDistanceSum / double(ProjectionHits) : 0.0,
			ProjectionDistanceMax,
			OutLines.Num());
	}

	UE_LOG(LogTemp, Display,
		TEXT("[VisVinePrepTiming] %s %s total=%.3f ms buildWorking=%.3f ms noiseProject=%.3f ms reduceSample=%.3f ms mergeFinalProject=%.3f ms projectionCalls=%.3f ms"),
		SafeProjectionLabel,
		VineKindLabel,
		(FPlatformTime::Seconds() - PrepStartSeconds) * 1000.0,
		BuildWorkingMs,
		NoiseAndProjectMs,
		ReduceSampleMs,
		MergeAndFinalProjectMs,
		ProjectionMs);

	return OutLines.Num() > 0;
}

// GPU-only line preparation: no CPU BVH surface projection and no CPU noise.
// Surface projection and CurlNoise are handled entirely on the GPU (unified voxel projection).
// Mirrors the "clone + SC jitter -> SmoothLine(3) -> ResampleByLength" front of the CPU path,
// then leaves projection/noise to the GPU. Point count is fixed after the single resample.
static FVector NormalizeVineAxisOrFallback(const FVector& Axis, const FVector& FallbackAxis = FVector::ZeroVector)
{
	const FVector NormalizedAxis = Axis.GetSafeNormal();
	return NormalizedAxis.IsNearlyZero() ? FallbackAxis.GetSafeNormal() : NormalizedAxis;
}


static float EvaluateVineScale(const UCurveLinearColor* CurveControl, int32 Index, int32 Count)
{
	if (!CurveControl || Count <= 1)
	{
		return 1.0f;
	}

	return CurveControl->GetUnadjustedLinearColorValue(Index / double(Count - 1)).G;
}


static void GetAllFoliageInstanceTransforms(UWorld* World, UFoliageType* InFoliageType, TArray<FTransform>& OutTransforms)
{
	if (!World || !InFoliageType)
	{
		return;
	}

	for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It)
	{
		FFoliageInfo* FoliageInfo = It->FindInfo(InFoliageType);
		if (!FoliageInfo || !FoliageInfo->GetComponent() || FoliageInfo->Instances.Num() == 0)
		{
			continue;
		}

		const int32 InstanceCount = FoliageInfo->GetComponent()->GetInstanceCount();
		for (int32 Index = 0; Index < InstanceCount; ++Index)
		{
			FTransform Transform;
			FoliageInfo->GetComponent()->GetInstanceTransform(Index, Transform, true);
			OutTransforms.Add(Transform);
		}
	}
}

static void GetVineInstanceTransforms(UInstancedStaticMeshComponent* Component, TArray<FTransform>& OutTransforms)
{
	OutTransforms.Reset();
	if (!Component)
	{
		return;
	}

	const int32 InstanceCount = Component->GetInstanceCount();
	OutTransforms.Reserve(InstanceCount);
	for (int32 Index = 0; Index < InstanceCount; ++Index)
	{
		FTransform Transform;
		if (Component->GetInstanceTransform(Index, Transform, true))
		{
			OutTransforms.Add(Transform);
		}
	}
}

static void RefreshFoliageType(UWorld* World, UFoliageType* InFoliageType)
{
	if (!World || !InFoliageType)
	{
		return;
	}

	for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It)
	{
		if (FFoliageInfo* FoliageInfo = It->FindInfo(InFoliageType))
		{
			FoliageInfo->Refresh(true, true);
		}
	}
}

static uint32 HashVineVoxelCell(int32 X, int32 Y, int32 Z)
{
	return (static_cast<uint32>(X) * 73856093u)
		^ (static_cast<uint32>(Y) * 19349663u)
		^ (static_cast<uint32>(Z) * 83492791u);
}

static bool SameVineVoxelCell(const FIntVector4& A, const FIntVector4& B)
{
	return A.X == B.X && A.Y == B.Y && A.Z == B.Z;
}

static bool BuildVineVoxelHashSlots(const TArray<FIntVector4>& VoxelCells, TArray<uint32>& OutHashSlots, uint32& OutHashSlotCount)
{
	OutHashSlots.Reset();
	OutHashSlotCount = 0;

	const int32 VoxelCount = VoxelCells.Num();
	if (VoxelCount <= 0)
	{
		return false;
	}

	uint64 DesiredSlotCount = FMath::Max<uint64>(2ull, uint64(VoxelCount) * 2ull);
	if (DesiredSlotCount > (1ull << 30))
	{
		return false;
	}

	uint32 SlotCount = 1u;
	while (uint64(SlotCount) < DesiredSlotCount)
	{
		SlotCount <<= 1u;
	}

	OutHashSlots.Init(0u, int32(SlotCount));
	const uint32 SlotMask = SlotCount - 1u;

	for (int32 VoxelIndex = 0; VoxelIndex < VoxelCount; ++VoxelIndex)
	{
		const FIntVector4& Cell = VoxelCells[VoxelIndex];
		uint32 Slot = HashVineVoxelCell(Cell.X, Cell.Y, Cell.Z) & SlotMask;
		bool bInserted = false;

		for (uint32 Probe = 0u; Probe < SlotCount; ++Probe)
		{
			uint32& PackedVoxelIndex = OutHashSlots[int32(Slot)];
			if (PackedVoxelIndex == 0u)
			{
				PackedVoxelIndex = uint32(VoxelIndex) + 1u;
				bInserted = true;
				break;
			}

			const int32 ExistingVoxelIndex = int32(PackedVoxelIndex - 1u);
			if (VoxelCells.IsValidIndex(ExistingVoxelIndex) && SameVineVoxelCell(VoxelCells[ExistingVoxelIndex], Cell))
			{
				bInserted = true;
				break;
			}

			Slot = (Slot + 1u) & SlotMask;
		}

		if (!bInserted)
		{
			OutHashSlots.Reset();
			return false;
		}
	}

	OutHashSlotCount = SlotCount;
	return true;
}

// FVineTargetBucketBuffers is defined at global scope above (moved so FVineBuildInput can embed it).

static void EnsureVineTargetBucketDummyBuffers(FVineTargetBucketBuffers& Buffers)
{
	if (Buffers.Ranges.Num() == 0)
	{
		Buffers.Ranges.Add(FIntVector4(0, 0, 0, 0));
	}
	if (Buffers.RangeCounts.Num() == 0)
	{
		Buffers.RangeCounts.Add(0u);
	}
	if (Buffers.VoxelIndices.Num() == 0)
	{
		Buffers.VoxelIndices.Add(0u);
	}
	if (Buffers.HashSlots.Num() == 0)
	{
		Buffers.HashSlots.Add(0u);
	}
}

static FIntVector GetVineTargetBucketCell(const FVector4f& Target, const FVector3f& Origin, float BucketSize)
{
	const float SafeBucketSize = FMath::Max(BucketSize, UE_KINDA_SMALL_NUMBER);
	return FIntVector(
		FMath::FloorToInt((double(Target.X) - double(Origin.X)) / double(SafeBucketSize)),
		FMath::FloorToInt((double(Target.Y) - double(Origin.Y)) / double(SafeBucketSize)),
		FMath::FloorToInt((double(Target.Z) - double(Origin.Z)) / double(SafeBucketSize)));
}

static bool BuildVineTargetBucketBuffers(
	const TArray<FVector4f>& TargetPositions,
	const FVector3f& Origin,
	float VoxelSize,
	FVineTargetBucketBuffers& OutBuffers)
{
	OutBuffers = FVineTargetBucketBuffers();
	OutBuffers.BucketSize = FMath::Max(VoxelSize * 16.0f, 25.0f);
	OutBuffers.SearchRadius = 8u;

	TMap<FIntVector, TArray<uint32>> BucketMap;
	BucketMap.Reserve(TargetPositions.Num());
	for (int32 TargetIndex = 0; TargetIndex < TargetPositions.Num(); ++TargetIndex)
	{
		const FVector4f& Target = TargetPositions[TargetIndex];
		if (!FMath::IsFinite(Target.X) || !FMath::IsFinite(Target.Y) || !FMath::IsFinite(Target.Z))
		{
			continue;
		}

		TArray<uint32>& BucketIndices = BucketMap.FindOrAdd(GetVineTargetBucketCell(Target, Origin, OutBuffers.BucketSize));
		BucketIndices.Add(uint32(TargetIndex));
	}

	if (BucketMap.Num() == 0)
	{
		EnsureVineTargetBucketDummyBuffers(OutBuffers);
		return false;
	}

	TArray<FIntVector> BucketKeys;
	BucketMap.GetKeys(BucketKeys);
	BucketKeys.Sort([](const FIntVector& A, const FIntVector& B)
	{
		if (A.X != B.X)
		{
			return A.X < B.X;
		}
		if (A.Y != B.Y)
		{
			return A.Y < B.Y;
		}
		return A.Z < B.Z;
	});

	OutBuffers.Ranges.Reserve(BucketKeys.Num());
	OutBuffers.RangeCounts.Reserve(BucketKeys.Num());
	OutBuffers.VoxelIndices.Reserve(TargetPositions.Num());
	for (const FIntVector& BucketKey : BucketKeys)
	{
		const TArray<uint32>* BucketIndices = BucketMap.Find(BucketKey);
		if (!BucketIndices || BucketIndices->Num() == 0)
		{
			continue;
		}
		if (OutBuffers.VoxelIndices.Num() > MAX_int32)
		{
			OutBuffers = FVineTargetBucketBuffers();
			EnsureVineTargetBucketDummyBuffers(OutBuffers);
			return false;
		}

		const int32 RangeStart = OutBuffers.VoxelIndices.Num();
		OutBuffers.Ranges.Add(FIntVector4(BucketKey.X, BucketKey.Y, BucketKey.Z, RangeStart));
		OutBuffers.RangeCounts.Add(uint32(BucketIndices->Num()));
		OutBuffers.MaxBucketItemCount = FMath::Max<uint32>(OutBuffers.MaxBucketItemCount, uint32(BucketIndices->Num()));
		OutBuffers.VoxelIndices.Append(*BucketIndices);
	}

	OutBuffers.BucketCount = uint32(OutBuffers.Ranges.Num());
	if (OutBuffers.BucketCount == 0u || !BuildVineVoxelHashSlots(OutBuffers.Ranges, OutBuffers.HashSlots, OutBuffers.HashSlotCount))
	{
		OutBuffers.HashSlots.Reset();
		OutBuffers.HashSlotCount = 0u;
		EnsureVineTargetBucketDummyBuffers(OutBuffers);
		return false;
	}

	EnsureVineTargetBucketDummyBuffers(OutBuffers);
	return true;
}

// Dispatch-arg slots VineDispatchArgsCS publishes, one uint3 each, in this order.
enum : uint32
{
	VineArgSlot_Points = 0u,             // one thread per path point
	VineArgSlot_Segments = 1u,           // one thread per segment
	VineArgSlot_VerticesOrSegments = 2u, // BuildVVVoxelCS: its two halves index differently
	VineArgSlot_Vertices = 3u,           // one thread per output vertex
	VineArgSlotCount = 4u,
};

static constexpr uint32 VineArgOffset(uint32 Slot) { return Slot * 3u * uint32(sizeof(uint32)); }

// Aggregated inputs for the shared vine-mesh RDG pass graph (AddVineMeshPasses).
// Every value the graph body reads is threaded through here so the pass sequence
// itself moves verbatim. CPU-fallback arrays and the bucket table are passed by
// pointer into the caller-owned (render-command-captured) copies, which outlive the
// synchronous AddVineMeshPasses call.
struct FVineMeshPassInputs
{
	// GPU-resident line geometry recorded earlier into the SAME graph by the fused SC + concat
	// passes. Null on the CPU-array fallback below.
	bool bUseGPULines = false;
	FRDGBufferRef GPULinePoints = nullptr;
	FRDGBufferRef GPULineMeta = nullptr;
	FRDGBufferRef GPULineSeg = nullptr;
	// [0]=lineCount [1]=pointCount [2]=segmentCount. GPU-decided; drives every dispatch below.
	// Null on the CPU-array fallback, where the counts are uploaded from the array sizes.
	FRDGBufferRef LineCountsBuffer = nullptr;
	const TArray<FVector4f>* PathPoints = nullptr;
	const TArray<FVector4f>* PathPointAxes = nullptr;
	const TArray<FIntVector4>* PathPointMeta = nullptr;
	const TArray<FIntVector4>* SegmentMeta = nullptr;
	bool bUseGPUVoxels = false;
	TRefCountPtr<FRDGPooledBuffer> GPUVoxCells;
	TRefCountPtr<FRDGPooledBuffer> GPUVoxNormals;
	TRefCountPtr<FRDGPooledBuffer> GPUVoxTargets;
	TRefCountPtr<FRDGPooledBuffer> GPUVoxCounter;
	// 融合路径下体素就在本图里产出，直接给图内 ref；非空时优先于上面的 pooled ref。
	// 两者是同一份数据的两种到达方式，下游只认解析后的结果。
	FRDGBufferRef GPUVoxCellsRDG = nullptr;
	FRDGBufferRef GPUVoxNormalsRDG = nullptr;
	FRDGBufferRef GPUVoxTargetsRDG = nullptr;
	FRDGBufferRef GPUVoxCounterRDG = nullptr;
	uint32 GPUVoxCount = 0u;
	uint32 GpuVoxelHashSlotCountPow2 = 0u;
	const TArray<FIntVector4>* GPUVoxelCells = nullptr;
	const TArray<uint32>* GPUVoxelHashSlots = nullptr;
	const TArray<FVector4f>* GPUVoxelNormals = nullptr;
	const TArray<FVector4f>* GPUVoxelTargetPositions = nullptr;
	const FVineTargetBucketBuffers* TargetBuckets = nullptr;
	FVector3f TargetBucketOrigin = FVector3f::ZeroVector;
	FVector3f VoxelOrigin = FVector3f::ZeroVector;
	float VoxelSize = 0.0f;
	uint32 VoxelCount = 0u;
	uint32 GPUVoxelHashSlotCount = 0u;
	uint32 PathPointCount = 0u;
	uint32 SegmentCount = 0u;
	uint32 ProfileCount = 0u;
	uint32 OutputVertexCount = 0u;
	uint32 OutputIndexCount = 0u;
	bool bTube = false;
	float CircleScale = 0.0f;
	float LineScale = 0.0f;
	float VinesOffset = 0.0f;
	float TinyZJitterStrength = 0.0f;
	int32 SafePostProjectionSmoothIterations = 0;
	int32 SafePostProjectionSmoothKernelRadius = 0;
	int32 SafePostProjectionSmallSmoothIterations = 0;
	float SafePostProjectionSmoothAngleStrength = 0.0f;
	bool bResampleSurface = false;
	float ResampleTargetDistance = 0.0f;
	float CurlNoiseStrength = 0.0f;
	float CurlNoiseFrequency = 0.0f;
	float PerlinNoiseStrength = 0.0f;
	float PerlinNoiseFrequency = 0.0f;
	uint32 SafeNoiseIterations = 0u;
	// Raw VV.UVLengthScale for the base-stream axial-V GPU passes (max()'d in the scan).
	float UVLengthScale = 1.0f;
	EVisVineGPUDebugStage DebugStage = EVisVineGPUDebugStage::Smooth;
};

// Outputs: the three transient mesh buffers are created by the caller (so the readback
// copies can stay caller-side); their UAVs are bound here.
struct FVineMeshPassOutputs
{
	FRDGBufferUAVRef OutVerticesUAV = nullptr;
	FRDGBufferUAVRef OutUVsUAV = nullptr;
	FRDGBufferUAVRef OutIndicesUAV = nullptr;
	FRDGBufferRef* DebugCenterSourceBufferPtr = nullptr;
	FRDGBufferRef* SegmentMetaBufferPtr = nullptr;

	// When true, pass#8 selects the FVVVoxelCS::FBaseStreams permutation and emits into the
	// GPU-resident base streams below instead of the three legacy UAVs above. Default false
	// keeps the legacy path byte-identical; no current caller sets this.
	bool bBaseStreams = false;
	FRDGBufferUAVRef PositionUAV = nullptr;
	FRDGBufferUAVRef TangentUAV = nullptr;
	FRDGBufferUAVRef TexCoordUAV = nullptr;
	FRDGBufferUAVRef ColorUAV = nullptr;
	FRDGBufferUAVRef IndexUAV = nullptr;
	FRDGBufferUAVRef IndirectArgsUAV = nullptr;
	FRDGBufferUAVRef MeshCountersUAV = nullptr;
	FMatrix44f VineWorldToLocal = FMatrix44f::Identity;
};

// Records the full vine-mesh RDG pass graph (buffer registration + scratch buffers +
// the eight compute passes) into GraphBuilder. The caller owns the output buffers and
// GraphBuilder.Execute().
static void AddVineMeshPasses(FRDGBuilder& GraphBuilder, ERHIFeatureLevel::Type FeatureLevel, const FVineMeshPassInputs& In, const FVineMeshPassOutputs& Out)
{
	// Local aliases so the moved graph body below reads exactly as the original.
	const bool bUseGPULines = In.bUseGPULines;
	FRDGBufferRef GPULinePoints = In.GPULinePoints;
	FRDGBufferRef GPULineMeta = In.GPULineMeta;
	FRDGBufferRef GPULineSeg = In.GPULineSeg;
	const TArray<FVector4f>& PathPoints = *In.PathPoints;
	const TArray<FVector4f>& PathPointAxes = *In.PathPointAxes;
	const TArray<FIntVector4>& PathPointMeta = *In.PathPointMeta;
	const TArray<FIntVector4>& SegmentMeta = *In.SegmentMeta;
	const bool bUseGPUVoxels = In.bUseGPUVoxels;
	const TRefCountPtr<FRDGPooledBuffer>& GPUVoxCells = In.GPUVoxCells;
	const TRefCountPtr<FRDGPooledBuffer>& GPUVoxNormals = In.GPUVoxNormals;
	const TRefCountPtr<FRDGPooledBuffer>& GPUVoxTargets = In.GPUVoxTargets;
	const uint32 GPUVoxCount = In.GPUVoxCount;
	const uint32 GpuVoxelHashSlotCountPow2 = In.GpuVoxelHashSlotCountPow2;
	const TArray<FIntVector4>& GPUVoxelCells = *In.GPUVoxelCells;
	const TArray<uint32>& GPUVoxelHashSlots = *In.GPUVoxelHashSlots;
	const TArray<FVector4f>& GPUVoxelNormals = *In.GPUVoxelNormals;
	const TArray<FVector4f>& GPUVoxelTargetPositions = *In.GPUVoxelTargetPositions;
	const FVineTargetBucketBuffers& TargetBuckets = *In.TargetBuckets;
	const FVector3f TargetBucketOrigin = In.TargetBucketOrigin;
	const FVector3f VoxelOrigin = In.VoxelOrigin;
	const float VoxelSize = In.VoxelSize;
	const uint32 VoxelCount = In.VoxelCount;
	const uint32 GPUVoxelHashSlotCount = In.GPUVoxelHashSlotCount;
	const uint32 PathPointCount = In.PathPointCount;
	const uint32 SegmentCount = In.SegmentCount;
	const uint32 ProfileCount = In.ProfileCount;
	const uint32 OutputVertexCount = In.OutputVertexCount;
	const uint32 OutputIndexCount = In.OutputIndexCount;
	const bool bTube = In.bTube;
	const float CircleScale = In.CircleScale;
	const float LineScale = In.LineScale;
	const float VinesOffset = In.VinesOffset;
	const float TinyZJitterStrength = In.TinyZJitterStrength;
	const int32 SafePostProjectionSmoothIterations = In.SafePostProjectionSmoothIterations;
	const int32 SafePostProjectionSmoothKernelRadius = In.SafePostProjectionSmoothKernelRadius;
	const int32 SafePostProjectionSmallSmoothIterations = In.SafePostProjectionSmallSmoothIterations;
	const float SafePostProjectionSmoothAngleStrength = In.SafePostProjectionSmoothAngleStrength;
	const bool bResampleSurface = In.bResampleSurface;
	const float ResampleTargetDistance = In.ResampleTargetDistance;
	const float CurlNoiseStrength = In.CurlNoiseStrength;
	const float CurlNoiseFrequency = In.CurlNoiseFrequency;
	const float PerlinNoiseStrength = In.PerlinNoiseStrength;
	const float PerlinNoiseFrequency = In.PerlinNoiseFrequency;
	const uint32 SafeNoiseIterations = In.SafeNoiseIterations;
	const EVisVineGPUDebugStage DebugStage = In.DebugStage;

	// ------------------------------------------------------------------------
	// Stage E: every pass below is DispatchIndirect'd from GPU-decided counts.
	// PathPointCount / SegmentCount / OutputVertexCount / OutputIndexCount are now the CPU
	// ALLOCATION CAPACITIES (VV.MaxVinePointCount-derived), so dispatching directly from them
	// would run the voxel-bucket kernels — and the axial-V scan — over a quarter of a
	// million slots regardless of how short the vines are. LineCountsBuffer carries the compact
	// counts the fused SC + concat produced; VineDispatchArgsCS turns them into dispatch args.
	// ------------------------------------------------------------------------
	FRDGBufferRef LineCountsBuffer = In.LineCountsBuffer;
	if (!LineCountsBuffer)
	{
		// CPU-array fallback: the capacities ARE the real counts, so publish them as-is.
		TArray<uint32> CpuCounts;
		CpuCounts.Add(0u);
		CpuCounts.Add(PathPointCount);
		CpuCounts.Add(SegmentCount);
		CpuCounts.Add(0u);
		LineCountsBuffer = CSHelper::CreateUploadedStructuredBuffer(GraphBuilder, CpuCounts, TEXT("VineMesh.LineCounts.CPU"), false, true).Buffer;
	}
	FRDGBufferSRVRef LineCountsSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(LineCountsBuffer));

	// Four uint3 arg sets: [0] points, [1] segments, [2] max(vertices, segments), [3] vertices.
	FRDGBufferRef DispatchArgsBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateIndirectDesc(sizeof(uint32), VineArgSlotCount * 3u), TEXT("VineMesh.DispatchArgs"));
	{
		// The legacy (non base-stream) path owns no persistent indirect-draw / counter buffers;
		// hand the kernel scratch ones so a single code path publishes every argument.
		FRDGBufferUAVRef DrawIndirectUAV = Out.IndirectArgsUAV;
		if (!DrawIndirectUAV)
		{
			FRDGBufferRef Scratch = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc(sizeof(uint32), 5), TEXT("VineMesh.DrawArgs.Scratch"));
			DrawIndirectUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Scratch, PF_R32_UINT));
		}
		FRDGBufferUAVRef CountersUAV = Out.MeshCountersUAV;
		if (!CountersUAV)
		{
			FRDGBufferRef Scratch = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 2), TEXT("VineMesh.MeshCounters.Scratch"));
			CountersUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Scratch, PF_R32_UINT));
		}

		FVineDispatchArgsCS::FParameters* ArgsParameters = GraphBuilder.AllocParameters<FVineDispatchArgsCS::FParameters>();
		ArgsParameters->VineArgs_Counts = LineCountsSRV;
		ArgsParameters->RW_VineArgs_Dispatch = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(DispatchArgsBuffer, PF_R32_UINT));
		ArgsParameters->RW_VineArgs_DrawIndirect = DrawIndirectUAV;
		ArgsParameters->RW_VineArgs_MeshCounters = CountersUAV;
		ArgsParameters->VineArgs_PointCapacity = PathPointCount;
		ArgsParameters->VineArgs_SegmentCapacity = SegmentCount;
		ArgsParameters->VineArgs_ProfileCount = ProfileCount;
		ArgsParameters->VineArgs_bTube = bTube ? 1u : 0u;
		ArgsParameters->VineArgs_GroupSize = VineMeshGroupSize;
		TShaderMapRef<FVineDispatchArgsCS> ArgsShader(GetGlobalShaderMap(FeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VVVoxel.DispatchArgs"), ArgsShader, ArgsParameters, FIntVector(1, 1, 1));
	}

	CSHelper::FRDGStructuredBufferRefs PathPointBuffer;
	CSHelper::FRDGStructuredBufferRefs PathPointAxisBuffer;
	CSHelper::FRDGStructuredBufferRefs PathPointMetaBuffer;
	CSHelper::FRDGStructuredBufferRefs SegmentMetaBuffer;
	if (bUseGPULines)
	{
		// Produced earlier in this same graph, so they need an SRV view and nothing else.
		auto ViewSRVOnly = [&GraphBuilder](FRDGBufferRef Buffer)
		{
			CSHelper::FRDGStructuredBufferRefs Refs;
			Refs.Buffer = Buffer;
			Refs.SRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Buffer));
			return Refs;
		};
		PathPointBuffer = ViewSRVOnly(GPULinePoints);
		PathPointMetaBuffer = ViewSRVOnly(GPULineMeta);
		SegmentMetaBuffer = ViewSRVOnly(GPULineSeg);
		// The GPU SC path carries no per-point axis; zero-fill (matches BuildVVGPUInput's empty axes).
		PathPointAxisBuffer = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), PathPointCount, TEXT("VVVoxel.PathPointAxes.Zero"), true, true);
		AddClearUAVPass(GraphBuilder, PathPointAxisBuffer.UAV, 0u);
	}
	else
	{
		PathPointBuffer = CSHelper::CreateUploadedStructuredBuffer(GraphBuilder, PathPoints, TEXT("VVVoxel.PathPoints"));
		PathPointAxisBuffer = CSHelper::CreateUploadedStructuredBuffer(GraphBuilder, PathPointAxes, TEXT("VVVoxel.PathPointAxes"));
		PathPointMetaBuffer = CSHelper::CreateUploadedStructuredBuffer(GraphBuilder, PathPointMeta, TEXT("VVVoxel.PathPointMeta"));
		SegmentMetaBuffer = CSHelper::CreateUploadedStructuredBuffer(GraphBuilder, SegmentMeta, TEXT("VVVoxel.SegmentMeta"));
	}
	if (Out.SegmentMetaBufferPtr) *Out.SegmentMetaBufferPtr = SegmentMetaBuffer.Buffer;
	// Trip B: register the GPU-resident producer voxel buffers + rebuild the vine hash
	// on the GPU (no readback/re-upload); otherwise upload the CPU arrays as before.
	CSHelper::FRDGStructuredBufferRefs VoxelCellsBuffer;
	CSHelper::FRDGStructuredBufferRefs VoxelHashSlotsBuffer;
	CSHelper::FRDGStructuredBufferRefs VoxelNormalsBuffer;
	CSHelper::FRDGStructuredBufferRefs VoxelTargetPositionsBuffer;
	if (bUseGPUVoxels)
	{
		// The producer's voxel buffers are typed (vertex-buffer) pooled buffers, but the
		// vine shader reads structured buffers, so copy each into a fresh structured buffer
		// (GPU byte copy, no CPU round-trip) and bind a structured SRV.
		const uint32 VoxCopyCount = FMath::Max(GPUVoxCount, 1u);
		// 源可能来自两条路：融合路径下体素就在本图里（直接用图内 ref），旧路径下是另一张图
		// extract 出来的 pooled buffer（需要 register）。两者之后的处理完全一样。
		auto RegVox = [&GraphBuilder, VoxCopyCount](FRDGBufferRef Rdg, const TRefCountPtr<FRDGPooledBuffer>& Pooled, uint32 BytesPerElem, const TCHAR* Name)
		{
			FRDGBufferRef Src = Rdg ? Rdg : GraphBuilder.RegisterExternalBuffer(Pooled, Name);
			CSHelper::FRDGStructuredBufferRefs Dst = CSHelper::CreateStructuredBuffer(GraphBuilder, BytesPerElem, VoxCopyCount, Name, false, true);
			AddCopyBufferPass(GraphBuilder, Dst.Buffer, 0, Src, 0, uint64(BytesPerElem) * VoxCopyCount);
			return Dst;
		};
		VoxelCellsBuffer = RegVox(In.GPUVoxCellsRDG, GPUVoxCells, sizeof(FIntVector4), TEXT("VVVoxel.VoxelCells.GPU"));
		VoxelNormalsBuffer = RegVox(In.GPUVoxNormalsRDG, GPUVoxNormals, sizeof(FVector4f), TEXT("VVVoxel.VoxelNormals.GPU"));
		VoxelTargetPositionsBuffer = RegVox(In.GPUVoxTargetsRDG, GPUVoxTargets, sizeof(FVector4f), TEXT("VVVoxel.VoxelTargetPositions.GPU"));
		VoxelHashSlotsBuffer = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(uint32), GpuVoxelHashSlotCountPow2, TEXT("VVVoxel.VoxelHashSlots.GPU"), true, true);
		AddClearUAVPass(GraphBuilder, VoxelHashSlotsBuffer.UAV, 0u);
		{
			FVVBuildVoxelHashCS::FParameters* HP = GraphBuilder.AllocParameters<FVVBuildVoxelHashCS::FParameters>();
			HP->HashBuildCells = VoxelCellsBuffer.SRV;
			// The real voxel count only exists on the GPU; the hash build reads it from here so the
			// capacity slack never enters the table.
			HP->HashBuildCounter = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(
				In.GPUVoxCounterRDG ? In.GPUVoxCounterRDG
					: GraphBuilder.RegisterExternalBuffer(In.GPUVoxCounter, TEXT("VVVoxel.VoxelCounter.GPU")), PF_R32_UINT));
			HP->RW_HashBuildSlots = VoxelHashSlotsBuffer.UAV;
			HP->HashBuildVoxelCount = GPUVoxCount;
			HP->HashBuildSlotCount = GpuVoxelHashSlotCountPow2;
			TShaderMapRef<FVVBuildVoxelHashCS> HashShader(GetGlobalShaderMap(FeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VVVoxel.BuildVoxelHash"), HashShader, HP, FComputeShaderUtils::GetGroupCount(FMath::Max(GPUVoxCount, 1u), 64));
		}
	}
	else
	{
		VoxelCellsBuffer = CSHelper::CreateUploadedStructuredBuffer(GraphBuilder, GPUVoxelCells, TEXT("VVVoxel.VoxelCells"));
		VoxelHashSlotsBuffer = CSHelper::CreateUploadedStructuredBuffer(GraphBuilder, GPUVoxelHashSlots, TEXT("VVVoxel.VoxelHashSlots"));
		VoxelNormalsBuffer = CSHelper::CreateUploadedStructuredBuffer(GraphBuilder, GPUVoxelNormals, TEXT("VVVoxel.VoxelNormals"));
		VoxelTargetPositionsBuffer = CSHelper::CreateUploadedStructuredBuffer(GraphBuilder, GPUVoxelTargetPositions, TEXT("VVVoxel.VoxelTargetPositions"));
	}
	const CSHelper::FRDGStructuredBufferRefs TargetBucketRangesBuffer = CSHelper::CreateUploadedStructuredBuffer(GraphBuilder, TargetBuckets.Ranges, TEXT("VVVoxel.TargetBucketRanges"));
	const CSHelper::FRDGStructuredBufferRefs TargetBucketRangeCountsBuffer = CSHelper::CreateUploadedStructuredBuffer(GraphBuilder, TargetBuckets.RangeCounts, TEXT("VVVoxel.TargetBucketRangeCounts"));
	const CSHelper::FRDGStructuredBufferRefs TargetBucketVoxelIndicesBuffer = CSHelper::CreateUploadedStructuredBuffer(GraphBuilder, TargetBuckets.VoxelIndices, TEXT("VVVoxel.TargetBucketVoxelIndices"));
	const CSHelper::FRDGStructuredBufferRefs TargetBucketHashSlotsBuffer = CSHelper::CreateUploadedStructuredBuffer(GraphBuilder, TargetBuckets.HashSlots, TEXT("VVVoxel.TargetBucketHashSlots"));
	CSHelper::FRDGStructuredBufferRefs PathPointTangentA = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), PathPointCount, TEXT("VVVoxel.PathPointTangentsA"), true, true);
	CSHelper::FRDGStructuredBufferRefs PathPointNormalA = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), PathPointCount, TEXT("VVVoxel.PathPointNormalsA"), true, true);
	CSHelper::FRDGStructuredBufferRefs PathPointFrameNormalA = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), PathPointCount, TEXT("VVVoxel.PathPointFrameNormalsA"), true, true);
	CSHelper::FRDGStructuredBufferRefs PathPointSurfaceTargetA = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), PathPointCount, TEXT("VVVoxel.PathPointSurfaceTargetsA"), true, true);
	CSHelper::FRDGStructuredBufferRefs PathPointSurfaceNormalA = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), PathPointCount, TEXT("VVVoxel.PathPointSurfaceNormalsA"), true, true);
	CSHelper::FRDGStructuredBufferRefs PathPointSurfaceTargetB = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), PathPointCount, TEXT("VVVoxel.PathPointSurfaceTargetsB"), true, true);
	CSHelper::FRDGStructuredBufferRefs PathPointSurfaceNormalB = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), PathPointCount, TEXT("VVVoxel.PathPointSurfaceNormalsB"), true, true);

	// GPU noise loop output (voxel projection + CurlNoise). Downstream passes read this as the geometry source.
	CSHelper::FRDGStructuredBufferRefs PathPointNoisedBuffer = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), PathPointCount, TEXT("VVVoxel.PathPointsNoised"), true, true);

	TShaderMapRef<FVVVoxelNoiseCS> NoiseShader(GetGlobalShaderMap(FeatureLevel));
	FVVVoxelNoiseCS::FParameters* NoiseParameters = GraphBuilder.AllocParameters<FVVVoxelNoiseCS::FParameters>();
	NoiseParameters->PathPoints = PathPointBuffer.SRV;
	NoiseParameters->VoxelCells = VoxelCellsBuffer.SRV;
	NoiseParameters->VoxelHashSlots = VoxelHashSlotsBuffer.SRV;
	NoiseParameters->VoxelNormals = VoxelNormalsBuffer.SRV;
	NoiseParameters->VoxelTargetPositions = VoxelTargetPositionsBuffer.SRV;
	NoiseParameters->TargetBucketRanges = TargetBucketRangesBuffer.SRV;
	NoiseParameters->TargetBucketRangeCounts = TargetBucketRangeCountsBuffer.SRV;
	NoiseParameters->TargetBucketVoxelIndices = TargetBucketVoxelIndicesBuffer.SRV;
	NoiseParameters->TargetBucketHashSlots = TargetBucketHashSlotsBuffer.SRV;
	NoiseParameters->RW_PathPointsNoised = PathPointNoisedBuffer.UAV;
	NoiseParameters->PathPointCount = PathPointCount;
	NoiseParameters->VoxelOrigin = VoxelOrigin;
	NoiseParameters->VoxelSize = VoxelSize;
	NoiseParameters->VoxelCount = VoxelCount;
	NoiseParameters->VoxelHashSlotCount = GPUVoxelHashSlotCount;
	NoiseParameters->TargetBucketOrigin = TargetBucketOrigin;
	NoiseParameters->TargetBucketSize = TargetBuckets.BucketSize;
	NoiseParameters->TargetBucketCount = TargetBuckets.BucketCount;
	NoiseParameters->TargetBucketHashSlotCount = TargetBuckets.HashSlotCount;
	NoiseParameters->TargetBucketSearchRadius = TargetBuckets.SearchRadius;
	NoiseParameters->CurlNoiseStrength = CurlNoiseStrength;
	NoiseParameters->CurlNoiseFrequency = CurlNoiseFrequency;
	NoiseParameters->NoiseIterations = SafeNoiseIterations;
	NoiseParameters->VineDispatchArgs = DispatchArgsBuffer;

	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VVVoxel.ApplyNoise"), NoiseShader, NoiseParameters,
		DispatchArgsBuffer, VineArgOffset(VineArgSlot_Points));

	TShaderMapRef<FVVVoxelBuildAxesCS> BuildAxesShader(GetGlobalShaderMap(FeatureLevel));
	FVVVoxelBuildAxesCS::FParameters* BuildAxesParameters = GraphBuilder.AllocParameters<FVVVoxelBuildAxesCS::FParameters>();
	BuildAxesParameters->PathPoints = PathPointNoisedBuffer.SRV;
	BuildAxesParameters->PathPointAxes = PathPointAxisBuffer.SRV;
	BuildAxesParameters->PathPointMeta = PathPointMetaBuffer.SRV;
	BuildAxesParameters->VoxelCells = VoxelCellsBuffer.SRV;
	BuildAxesParameters->VoxelHashSlots = VoxelHashSlotsBuffer.SRV;
	BuildAxesParameters->VoxelNormals = VoxelNormalsBuffer.SRV;
	BuildAxesParameters->VoxelTargetPositions = VoxelTargetPositionsBuffer.SRV;
	BuildAxesParameters->TargetBucketRanges = TargetBucketRangesBuffer.SRV;
	BuildAxesParameters->TargetBucketRangeCounts = TargetBucketRangeCountsBuffer.SRV;
	BuildAxesParameters->TargetBucketVoxelIndices = TargetBucketVoxelIndicesBuffer.SRV;
	BuildAxesParameters->TargetBucketHashSlots = TargetBucketHashSlotsBuffer.SRV;
	BuildAxesParameters->RW_PathPointTangents = PathPointTangentA.UAV;
	BuildAxesParameters->RW_PathPointNormals = PathPointNormalA.UAV;
	BuildAxesParameters->RW_PathPointSurfaceTargets = PathPointSurfaceTargetA.UAV;
	BuildAxesParameters->RW_PathPointSurfaceNormals = PathPointSurfaceNormalA.UAV;
	BuildAxesParameters->PathPointCount = PathPointCount;
	BuildAxesParameters->VoxelOrigin = VoxelOrigin;
	BuildAxesParameters->VoxelSize = VoxelSize;
	BuildAxesParameters->VoxelCount = VoxelCount;
	BuildAxesParameters->VoxelHashSlotCount = GPUVoxelHashSlotCount;
	BuildAxesParameters->TargetBucketOrigin = TargetBucketOrigin;
	BuildAxesParameters->TargetBucketSize = TargetBuckets.BucketSize;
	BuildAxesParameters->TargetBucketCount = TargetBuckets.BucketCount;
	BuildAxesParameters->TargetBucketHashSlotCount = TargetBuckets.HashSlotCount;
	BuildAxesParameters->TargetBucketSearchRadius = TargetBuckets.SearchRadius;
	BuildAxesParameters->VineDispatchArgs = DispatchArgsBuffer;

	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VVVoxel.BuildAxes"), BuildAxesShader, BuildAxesParameters,
		DispatchArgsBuffer, VineArgOffset(VineArgSlot_Points));

	TShaderMapRef<FVVVoxelPerlinNoiseCS> PerlinNoiseShader(GetGlobalShaderMap(FeatureLevel));
	FVVVoxelPerlinNoiseCS::FParameters* PerlinNoiseParameters = GraphBuilder.AllocParameters<FVVVoxelPerlinNoiseCS::FParameters>();
	PerlinNoiseParameters->PathPointSurfaceTargets = PathPointSurfaceTargetA.SRV;
	PerlinNoiseParameters->PathPointSurfaceNormals = PathPointSurfaceNormalA.SRV;
	PerlinNoiseParameters->RW_PathPointSurfaceTargets = PathPointSurfaceTargetA.UAV;
	PerlinNoiseParameters->PerlinNoiseStrength = PerlinNoiseStrength;
	PerlinNoiseParameters->PerlinNoiseFrequency = PerlinNoiseFrequency;
	PerlinNoiseParameters->PathPointCount = PathPointCount;
	PerlinNoiseParameters->VineDispatchArgs = DispatchArgsBuffer;

	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VVVoxel.PerlinNoise"), PerlinNoiseShader, PerlinNoiseParameters,
		DispatchArgsBuffer, VineArgOffset(VineArgSlot_Points));

	// ===== Pass FP: Final surface projection (now runs before smoothing) =====
	// PerlinNoise left its result in the A buffers. Project those points back onto the
	// voxel surface (A -> B) first; the smoothing ping-pong below then refines the
	// already-projected surface instead of feeding the projection.
	TShaderMapRef<FVVVoxelFinalProjectCS> FinalProjectShader(GetGlobalShaderMap(FeatureLevel));
	FVVVoxelFinalProjectCS::FParameters* FinalProjectParameters = GraphBuilder.AllocParameters<FVVVoxelFinalProjectCS::FParameters>();
	FinalProjectParameters->PathPointSurfaceTargets = PathPointSurfaceTargetA.SRV;
	FinalProjectParameters->PathPointSurfaceNormals = PathPointSurfaceNormalA.SRV;
	FinalProjectParameters->VoxelCells = VoxelCellsBuffer.SRV;
	FinalProjectParameters->VoxelHashSlots = VoxelHashSlotsBuffer.SRV;
	FinalProjectParameters->VoxelNormals = VoxelNormalsBuffer.SRV;
	FinalProjectParameters->VoxelTargetPositions = VoxelTargetPositionsBuffer.SRV;
	FinalProjectParameters->TargetBucketRanges = TargetBucketRangesBuffer.SRV;
	FinalProjectParameters->TargetBucketRangeCounts = TargetBucketRangeCountsBuffer.SRV;
	FinalProjectParameters->TargetBucketVoxelIndices = TargetBucketVoxelIndicesBuffer.SRV;
	FinalProjectParameters->TargetBucketHashSlots = TargetBucketHashSlotsBuffer.SRV;
	FinalProjectParameters->RW_PathPointSurfaceTargets = PathPointSurfaceTargetB.UAV;
	FinalProjectParameters->RW_PathPointSurfaceNormals = PathPointSurfaceNormalB.UAV;
	FinalProjectParameters->PathPointCount = PathPointCount;
	FinalProjectParameters->VoxelOrigin = VoxelOrigin;
	FinalProjectParameters->VoxelSize = VoxelSize;
	FinalProjectParameters->VoxelCount = VoxelCount;
	FinalProjectParameters->VoxelHashSlotCount = GPUVoxelHashSlotCount;
	FinalProjectParameters->TargetBucketOrigin = TargetBucketOrigin;
	FinalProjectParameters->TargetBucketSize = TargetBuckets.BucketSize;
	FinalProjectParameters->TargetBucketCount = TargetBuckets.BucketCount;
	FinalProjectParameters->TargetBucketHashSlotCount = TargetBuckets.HashSlotCount;
	FinalProjectParameters->TargetBucketSearchRadius = TargetBuckets.SearchRadius;
	FinalProjectParameters->VinesOffset = VinesOffset;
	FinalProjectParameters->VineDispatchArgs = DispatchArgsBuffer;

	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VVVoxel.FinalProject"), FinalProjectShader, FinalProjectParameters,
		DispatchArgsBuffer, VineArgOffset(VineArgSlot_Points));

	if (Out.DebugCenterSourceBufferPtr && DebugStage == EVisVineGPUDebugStage::FinalProject) *Out.DebugCenterSourceBufferPtr = PathPointSurfaceTargetB.Buffer;

	// ===== Pass RS: Surface resample (moved BEFORE smoothing) =====
	// FinalProject wrote into the B buffers. Redistribute each vine's surface points to
	// uniform arc-length spacing first (B -> A), so the path smoothing ping-pong below
	// refines evenly-spaced points. Endpoints stay anchored. Read/Write pointers track the
	// latest buffer; each pass swaps them so downstream always reads the freshest result.
	CSHelper::FRDGStructuredBufferRefs* ReadSurfaceTarget = &PathPointSurfaceTargetB;
	CSHelper::FRDGStructuredBufferRefs* ReadSurfaceNormal = &PathPointSurfaceNormalB;
	CSHelper::FRDGStructuredBufferRefs* WriteSurfaceTarget = &PathPointSurfaceTargetA;
	CSHelper::FRDGStructuredBufferRefs* WriteSurfaceNormal = &PathPointSurfaceNormalA;
	if (bResampleSurface)
	{
		TShaderMapRef<FVVVoxelResampleSurfaceCS> ResampleSurfaceShader(GetGlobalShaderMap(FeatureLevel));
		FVVVoxelResampleSurfaceCS::FParameters* ResampleParams = GraphBuilder.AllocParameters<FVVVoxelResampleSurfaceCS::FParameters>();
		ResampleParams->PathPoints = PathPointNoisedBuffer.SRV;
		ResampleParams->PathPointSurfaceTargets = ReadSurfaceTarget->SRV;
		ResampleParams->PathPointSurfaceNormals = ReadSurfaceNormal->SRV;
		ResampleParams->PathPointMeta = PathPointMetaBuffer.SRV;
		ResampleParams->RW_PathPointSurfaceTargets = WriteSurfaceTarget->UAV;
		ResampleParams->RW_PathPointSurfaceNormals = WriteSurfaceNormal->UAV;
		ResampleParams->PathPointCount = PathPointCount;
		ResampleParams->ResampleTargetDistance = ResampleTargetDistance;
		ResampleParams->VineDispatchArgs = DispatchArgsBuffer;

		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VVVoxel.ResampleSurface"), ResampleSurfaceShader, ResampleParams,
			DispatchArgsBuffer, VineArgOffset(VineArgSlot_Points));

		Swap(ReadSurfaceTarget, WriteSurfaceTarget);
		Swap(ReadSurfaceNormal, WriteSurfaceNormal);
	}

	// After RS, ReadSurfaceTarget points at the resampled surface targets.
	if (Out.DebugCenterSourceBufferPtr && DebugStage == EVisVineGPUDebugStage::Resample) *Out.DebugCenterSourceBufferPtr = ReadSurfaceTarget->Buffer;

	// ===== Pass B: Path smoothing ping-pong (now smooths the resampled surface) =====
	// Wide-kernel passes round off sharp Z-folds, then a few radius-1 passes do a light
	// final cleanup using only the immediate neighbors. Pure geometric position smoothing;
	// normals are passed through and rebuilt later by BuildParallelTransportFrame.
	TShaderMapRef<FVVVoxelSmoothPathCS> SmoothPathShader(GetGlobalShaderMap(FeatureLevel));
	const int32 SafeTotalSmoothIterations = SafePostProjectionSmoothIterations + SafePostProjectionSmallSmoothIterations;
	for (int32 SmoothIterationIndex = 0; SmoothIterationIndex < SafeTotalSmoothIterations; ++SmoothIterationIndex)
	{
		// Wide passes first, then the small radius-1 cleanup passes.
		const bool bSmallPass = SmoothIterationIndex >= SafePostProjectionSmoothIterations;
		const int32 IterationKernelRadius = bSmallPass ? 1 : SafePostProjectionSmoothKernelRadius;

		FVVVoxelSmoothPathCS::FParameters* SmoothParameters = GraphBuilder.AllocParameters<FVVVoxelSmoothPathCS::FParameters>();
		SmoothParameters->PathPointSurfaceTargets = ReadSurfaceTarget->SRV;
		SmoothParameters->PathPointSurfaceNormals = ReadSurfaceNormal->SRV;
		SmoothParameters->PathPointMeta = PathPointMetaBuffer.SRV;
		SmoothParameters->RW_PathPointSurfaceTargets = WriteSurfaceTarget->UAV;
		SmoothParameters->RW_PathPointSurfaceNormals = WriteSurfaceNormal->UAV;
		SmoothParameters->PathPointCount = PathPointCount;
		SmoothParameters->SmoothPathKernelRadius = IterationKernelRadius;
		SmoothParameters->SmoothPathAngleStrength = SafePostProjectionSmoothAngleStrength;
		SmoothParameters->VineDispatchArgs = DispatchArgsBuffer;

		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VVVoxel.SmoothPath%d", SmoothIterationIndex), SmoothPathShader, SmoothParameters,
			DispatchArgsBuffer, VineArgOffset(VineArgSlot_Points));

		Swap(ReadSurfaceTarget, WriteSurfaceTarget);
		Swap(ReadSurfaceNormal, WriteSurfaceNormal);
	}

	// After resample + smoothing ping-pong, Read* points to the final geometry buffers.
	const CSHelper::FRDGStructuredBufferRefs* GeometrySurfaceTargetBuffer = ReadSurfaceTarget;
	const CSHelper::FRDGStructuredBufferRefs* GeometrySurfaceNormalBuffer = ReadSurfaceNormal;

	// B stage center line is the smoothed geometry target.
	if (Out.DebugCenterSourceBufferPtr && DebugStage == EVisVineGPUDebugStage::Smooth) *Out.DebugCenterSourceBufferPtr = GeometrySurfaceTargetBuffer->Buffer;

	// Rebuild tangents from the final (post line-smoothing) surface targets so the cross-section
	// axis stays perpendicular to the mesh geometry, then parallel-transport the roll axis along
	// each line to minimize twist. The parallel-transport frame is the final axis frame.
	TShaderMapRef<FVVVoxelBuildParallelTransportFrameCS> BuildTangentsFromSurfaceShader(GetGlobalShaderMap(FeatureLevel));
	FVVVoxelBuildParallelTransportFrameCS::FParameters* BuildTangentsParams = GraphBuilder.AllocParameters<FVVVoxelBuildParallelTransportFrameCS::FParameters>();
	BuildTangentsParams->PathPoints = PathPointNoisedBuffer.SRV;
	BuildTangentsParams->PathPointSurfaceTargets = GeometrySurfaceTargetBuffer->SRV;
	BuildTangentsParams->PathPointSurfaceNormals = GeometrySurfaceNormalBuffer->SRV;
	BuildTangentsParams->PathPointMeta = PathPointMetaBuffer.SRV;
	BuildTangentsParams->RW_PathPointTangents = PathPointTangentA.UAV;
	BuildTangentsParams->RW_PathPointFrameNormals = PathPointFrameNormalA.UAV;
	BuildTangentsParams->PathPointCount = PathPointCount;
	BuildTangentsParams->VineDispatchArgs = DispatchArgsBuffer;

	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VVVoxel.BuildParallelTransportFrame"), BuildTangentsFromSurfaceShader, BuildTangentsParams,
		DispatchArgsBuffer, VineArgOffset(VineArgSlot_Points));

	// Parallel-transport frame output (A buffers) is the final axis frame used for mesh build.
	CSHelper::FRDGStructuredBufferRefs& FinalTangentsForBuild = PathPointTangentA;
	CSHelper::FRDGStructuredBufferRefs& FinalFrameNormalsForBuild = PathPointFrameNormalA;

	FVVVoxelCS::FPermutationDomain PermVec;
	PermVec.Set<FVVVoxelCS::FBaseStreams>(Out.bBaseStreams);
	TShaderMapRef<FVVVoxelCS> ComputeShader(GetGlobalShaderMap(FeatureLevel), PermVec);
	FVVVoxelCS::FParameters* Parameters = GraphBuilder.AllocParameters<FVVVoxelCS::FParameters>();
	Parameters->PathPoints = PathPointNoisedBuffer.SRV;
	Parameters->PathPointMeta = PathPointMetaBuffer.SRV;
	Parameters->PathPointTangents = FinalTangentsForBuild.SRV;
	Parameters->PathPointNormals = PathPointNormalA.SRV;
	Parameters->PathPointFrameNormals = FinalFrameNormalsForBuild.SRV;
	Parameters->PathPointSurfaceTargets = GeometrySurfaceTargetBuffer->SRV;
	Parameters->PathPointSurfaceNormals = GeometrySurfaceNormalBuffer->SRV;
	Parameters->SegmentMeta = SegmentMetaBuffer.SRV;
	Parameters->VoxelCells = VoxelCellsBuffer.SRV;
	Parameters->VoxelHashSlots = VoxelHashSlotsBuffer.SRV;
	Parameters->VoxelNormals = VoxelNormalsBuffer.SRV;
	Parameters->VoxelTargetPositions = VoxelTargetPositionsBuffer.SRV;
	Parameters->TargetBucketRanges = TargetBucketRangesBuffer.SRV;
	Parameters->TargetBucketRangeCounts = TargetBucketRangeCountsBuffer.SRV;
	Parameters->TargetBucketVoxelIndices = TargetBucketVoxelIndicesBuffer.SRV;
	Parameters->TargetBucketHashSlots = TargetBucketHashSlotsBuffer.SRV;
	if (Out.bBaseStreams)
	{
		// Base-stream permutation: the HLSL #if references only these; leave the legacy UAVs null.
		Parameters->RWPositions = Out.PositionUAV;
		Parameters->RWTangents = Out.TangentUAV;
		Parameters->RWTexCoords = Out.TexCoordUAV;
		Parameters->RWColors = Out.ColorUAV;
		Parameters->RWBaseIndices = Out.IndexUAV;
		Parameters->VineWorldToLocal = Out.VineWorldToLocal;
		Parameters->RW_OutVertices = nullptr;
		Parameters->RW_OutUVs = nullptr;
		Parameters->RW_OutIndices = nullptr;
	}
	else
	{
		// Legacy path (byte-identical): only the three StructuredBuffer UAVs are referenced.
		Parameters->RW_OutVertices = Out.OutVerticesUAV;
		Parameters->RW_OutUVs = Out.OutUVsUAV;
		Parameters->RW_OutIndices = Out.OutIndicesUAV;
		Parameters->RWPositions = nullptr;
		Parameters->RWTangents = nullptr;
		Parameters->RWTexCoords = nullptr;
		Parameters->RWColors = nullptr;
		Parameters->RWBaseIndices = nullptr;
	}
	Parameters->VineMeshCounts = LineCountsSRV;
	Parameters->PathPointCount = PathPointCount;
	Parameters->SegmentCount = SegmentCount;
	Parameters->OutputVertexCount = OutputVertexCount;
	Parameters->OutputIndexCount = OutputIndexCount;
	Parameters->ProfileCount = ProfileCount;
	Parameters->bTube = bTube ? 1u : 0u;
	Parameters->CircleScale = CircleScale;
	Parameters->LineScale = LineScale;
	Parameters->VoxelOrigin = VoxelOrigin;
	Parameters->VoxelSize = VoxelSize;
	Parameters->VoxelCount = VoxelCount;
	Parameters->VoxelHashSlotCount = GPUVoxelHashSlotCount;
	Parameters->TargetBucketOrigin = TargetBucketOrigin;
	Parameters->TargetBucketSize = TargetBuckets.BucketSize;
	Parameters->TargetBucketCount = TargetBuckets.BucketCount;
	Parameters->TargetBucketHashSlotCount = TargetBuckets.HashSlotCount;
	Parameters->TargetBucketSearchRadius = TargetBuckets.SearchRadius;
	Parameters->VinesOffset = VinesOffset;
	Parameters->TinyZJitterStrength = TinyZJitterStrength;
	Parameters->VineDispatchArgs = DispatchArgsBuffer;

	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VVVoxelCS"), ComputeShader, Parameters,
		DispatchArgsBuffer, VineArgOffset(VineArgSlot_VerticesOrSegments));

	// ------------------------------------------------------------------------
	// Base-stream axial V: pass#8 (BuildVVVoxelCS) wrote RWTexCoords[Index*2+1] = 0.
	// Recompute the correct V on the GPU straight from the base-stream Position buffer
	// it just filled, and overwrite it. The identity leaf makes RWPositions local==world, so the ring centers,
	// circumferences and segment lengths match the CPU (which runs on world-space output
	// verts) to float precision. The legacy readback path (bBaseStreams==false) skips this
	// entirely (V stays 0 here and is recomputed on the CPU after readback, as before).
	// ------------------------------------------------------------------------
	if (Out.bBaseStreams && ProfileCount > 0u && OutputVertexCount > 0u)
	{
		const uint32 VineUVPointCount = OutputVertexCount / ProfileCount; // == CPU Vertices.Num()/ProfileCount
		if (VineUVPointCount > 0u)
		{
			// Read the base-stream positions pass#8 just wrote as a typed Buffer<float>. RDG sequences
			// this SRV read after pass#8's UAV write on the same buffer automatically.
			FRDGBufferRef PositionBuffer = Out.PositionUAV->GetParent();
			FRDGBufferSRVRef VineUVPositionSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(PositionBuffer, PF_R32_FLOAT));

			// Graph-lifetime scratch: one entry per output point.
			CSHelper::FRDGStructuredBufferRefs VineUVCenters = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), VineUVPointCount, TEXT("VineUV.Centers"), true, true);
			CSHelper::FRDGStructuredBufferRefs VineUVRingCirc = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(float), VineUVPointCount, TEXT("VineUV.RingCirc"), true, true);
			CSHelper::FRDGStructuredBufferRefs VineUVSegLen = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(float), VineUVPointCount, TEXT("VineUV.SegLen"), true, true);
			CSHelper::FRDGStructuredBufferRefs VineUVCurveV = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(float), VineUVPointCount, TEXT("VineUV.CurveV"), true, true);

			// Two-level segmented-scan scratch. RDG needs an element count at graph-build time, so
			// these are sized off the CPU capacity like every other vine buffer; the passes filling
			// them are still bounded by the GPU point count. VineUVPointCount == PathPointCount
			// (OutputVertexCount is PathPointCount * ProfileCount), so V3a's block partition lines up
			// exactly with the VineArgSlot_Points arg set it dispatches from.
			const uint32 VineUVScanBlockCount = FMath::DivideAndRoundUp(VineUVPointCount, VineMeshGroupSize);
			CSHelper::FRDGStructuredBufferRefs VineUVScanFlags = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(uint32), VineUVPointCount, TEXT("VineUV.ScanFlags"), true, true);
			CSHelper::FRDGStructuredBufferRefs VineUVScanBlockSum = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(float), VineUVScanBlockCount, TEXT("VineUV.ScanBlockSum"), true, true);
			CSHelper::FRDGStructuredBufferRefs VineUVScanBlockFlag = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(uint32), VineUVScanBlockCount, TEXT("VineUV.ScanBlockFlag"), true, true);
			CSHelper::FRDGStructuredBufferRefs VineUVScanBlockCarry = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(float), VineUVScanBlockCount, TEXT("VineUV.ScanBlockCarry"), true, true);

			// V1: ring center + circumference for every output point.
			{
				FVineUVCentersCS::FParameters* VP = GraphBuilder.AllocParameters<FVineUVCentersCS::FParameters>();
				VP->VineUV_Positions = VineUVPositionSRV;
				VP->RW_VineUV_Centers = VineUVCenters.UAV;
				VP->RW_VineUV_RingCirc = VineUVRingCirc.UAV;
				VP->VineUV_PointCount = VineUVPointCount;
				VP->VineUV_ProfileCount = ProfileCount;
				VP->VineDispatchArgs = DispatchArgsBuffer;
				TShaderMapRef<FVineUVCentersCS> Shader(GetGlobalShaderMap(FeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VineUV.Centers"), Shader, VP,
					DispatchArgsBuffer, VineArgOffset(VineArgSlot_Points));
			}

			// V2: clear SegLen to -1 (line-boundary marker), then scatter per-segment axial length.
			AddClearUAVPass(GraphBuilder, VineUVSegLen.UAV, 0xBF800000u); // asuint(-1.0f)
			{
				FVineUVSegLenCS::FParameters* VP = GraphBuilder.AllocParameters<FVineUVSegLenCS::FParameters>();
				VP->VineUV_SegmentMeta = SegmentMetaBuffer.SRV;
				VP->VineUV_Centers = VineUVCenters.SRV;
				VP->RW_VineUV_SegLen = VineUVSegLen.UAV;
				VP->VineUV_PointCount = VineUVPointCount;
				VP->VineUV_SegmentCount = SegmentCount;
				VP->VineDispatchArgs = DispatchArgsBuffer;
				TShaderMapRef<FVineUVSegLenCS> Shader(GetGlobalShaderMap(FeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VineUV.SegLen"), Shader, VP,
					DispatchArgsBuffer, VineArgOffset(VineArgSlot_Segments));
			}

			// V3: segmented prefix sum with per-line reset -> CurveV, as a two-level parallel scan.
			// V3a: block-local segmented scan + per-block aggregate (indirect, one thread per point).
			{
				FVineUVScanBlockCS::FParameters* VP = GraphBuilder.AllocParameters<FVineUVScanBlockCS::FParameters>();
				VP->VineUV_SegLen = VineUVSegLen.SRV;
				VP->VineUV_RingCirc = VineUVRingCirc.SRV;
				VP->VineUV_Counts = LineCountsSRV;
				VP->RW_VineUV_CurveV = VineUVCurveV.UAV;
				VP->RW_VineUV_ScanFlags = VineUVScanFlags.UAV;
				VP->RW_VineUV_ScanBlockSum = VineUVScanBlockSum.UAV;
				VP->RW_VineUV_ScanBlockFlag = VineUVScanBlockFlag.UAV;
				VP->VineUV_PointCount = VineUVPointCount;
				VP->VineUV_LengthScale = In.UVLengthScale; // raw; the shader applies max(.,1e-8)
				VP->VineDispatchArgs = DispatchArgsBuffer;
				TShaderMapRef<FVineUVScanBlockCS> Shader(GetGlobalShaderMap(FeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VineUV.ScanBlock"), Shader, VP,
					DispatchArgsBuffer, VineArgOffset(VineArgSlot_Points));
			}

			// V3b: scan the block aggregates into a per-block carry-in. A fixed 1x1x1 launch is
			// correct here: the kernel is a single block that walks the aggregates in tiles, and its
			// loop bound comes from the same GPU count buffer, not from the CPU capacity.
			{
				FVineUVScanBlockOffsetsCS::FParameters* VP = GraphBuilder.AllocParameters<FVineUVScanBlockOffsetsCS::FParameters>();
				VP->VineUV_ScanBlockSum = VineUVScanBlockSum.SRV;
				VP->VineUV_ScanBlockFlag = VineUVScanBlockFlag.SRV;
				VP->VineUV_Counts = LineCountsSRV;
				VP->RW_VineUV_ScanBlockCarry = VineUVScanBlockCarry.UAV;
				VP->VineUV_PointCount = VineUVPointCount;
				TShaderMapRef<FVineUVScanBlockOffsetsCS> Shader(GetGlobalShaderMap(FeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VineUV.ScanBlockOffsets"), Shader, VP, FIntVector(1, 1, 1));
			}

			// V3c: fold the carry-in back into the points whose block-local run saw no line head.
			{
				FVineUVScanApplyCS::FParameters* VP = GraphBuilder.AllocParameters<FVineUVScanApplyCS::FParameters>();
				VP->VineUV_ScanFlags = VineUVScanFlags.SRV;
				VP->VineUV_ScanBlockCarry = VineUVScanBlockCarry.SRV;
				VP->VineUV_Counts = LineCountsSRV;
				VP->RW_VineUV_CurveV = VineUVCurveV.UAV;
				VP->VineUV_PointCount = VineUVPointCount;
				VP->VineDispatchArgs = DispatchArgsBuffer;
				TShaderMapRef<FVineUVScanApplyCS> Shader(GetGlobalShaderMap(FeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VineUV.ScanApply"), Shader, VP,
					DispatchArgsBuffer, VineArgOffset(VineArgSlot_Points));
			}

			// V4: broadcast CurveV[P] into the V of every ring vertex (overwrites pass#8's 0).
			{
				FVineUVWriteCS::FParameters* VP = GraphBuilder.AllocParameters<FVineUVWriteCS::FParameters>();
				VP->VineUV_CurveV = VineUVCurveV.SRV;
				VP->RW_VineUV_TexCoords = Out.TexCoordUAV;
				VP->VineUV_OutputVertexCount = OutputVertexCount;
				VP->VineUV_ProfileCount = ProfileCount;
				VP->VineUV_PointCount = VineUVPointCount;
				VP->VineDispatchArgs = DispatchArgsBuffer;
				TShaderMapRef<FVineUVWriteCS> Shader(GetGlobalShaderMap(FeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VineUV.Write"), Shader, VP,
					DispatchArgsBuffer, VineArgOffset(VineArgSlot_Vertices));
			}
		}
	}
}

// Shared CPU-prep for the vine mesh pass graph: repacks the surface voxels into GPU-upload
// arrays, builds the voxel hash + target-position buckets, derives the output vertex/index counts
// and the sanitized scalar parameters, and captures the GPU-resident line/voxel pooled refs.
// Produces a self-owning FVineBuildInput consumed by FVineMeshSceneProxy (the GPU-resident leaf).
// On failure it returns bValid=false, with the same warning log. The optional out-params
// report the same per-phase timings the dispatch logs.
static FVineBuildInput VineLeaf_BuildVineBuildInput(
	const TArray<FVector4f>& PathPoints,
	const TArray<FVector4f>& PathPointAxes,
	const TArray<FIntVector4>& PathPointMeta,
	const TArray<FIntVector4>& SegmentMeta,
	bool bTube,
	uint32 TubeProfileCount,
	float CircleScale,
	float LineScale,
	float UVLengthScale,
	float VinesOffset,
	float TinyZJitterStrength,
	int32 PostProjectionSmoothIterations,
	int32 PostProjectionSmoothKernelRadius,
	int32 PostProjectionSmallSmoothIterations,
	float PostProjectionSmoothAngleStrength,
	bool bResampleSurface,
	float ResampleTargetDistance,
	float CurlNoiseStrength,
	float CurlNoiseFrequency,
	float PerlinNoiseStrength,
	float PerlinNoiseFrequency,
	int32 NoiseIterations,
	const FCSSurfaceVoxelData& VoxelData,
	EVisVineGPUDebugStage DebugStage,
	const FVineFusedSCInputs* GPULines,
	const FCSSurfaceVoxelGPUBuffers* GPUVoxels,
	// 融合体素输入。非空且有效时优先于 GPUVoxels：体素改在叶子自己的图里现算，
	// 省掉独立图、pooled extract 与 FlushRenderingCommands。会被 MoveTemp 进 bundle。
	FCSSurfaceVoxelPassInputs* FusedVoxels,
	double* OutBuildVoxelUploadMs = nullptr,
	double* OutBuildHashMs = nullptr,
	double* OutBuildTargetBucketsMs = nullptr)
{
	FVineBuildInput B;

	// Copy the CPU-fallback line arrays verbatim (empty on the fused GPU-lines path).
	B.PathPoints = PathPoints;
	B.PathPointAxes = PathPointAxes;
	B.PathPointMeta = PathPointMeta;
	B.SegmentMeta = SegmentMeta;

	B.bTube = bTube;
	B.CircleScale = CircleScale;
	B.LineScale = LineScale;
	B.UVLengthScale = UVLengthScale;
	B.VinesOffset = VinesOffset;
	B.TinyZJitterStrength = TinyZJitterStrength;
	B.bResampleSurface = bResampleSurface;
	B.ResampleTargetDistance = ResampleTargetDistance;
	B.CurlNoiseStrength = CurlNoiseStrength;
	B.CurlNoiseFrequency = CurlNoiseFrequency;
	B.PerlinNoiseStrength = PerlinNoiseStrength;
	B.PerlinNoiseFrequency = PerlinNoiseFrequency;
	B.DebugStage = DebugStage;

	const bool bUseGPULines = (GPULines != nullptr && GPULines->IsValid());
	// 融合体素优先：有效时体素在叶子自己的图里现算，pooled 那条路整条不走。
	const bool bUseFusedVoxels = (FusedVoxels != nullptr && FusedVoxels->IsValid());
	const bool bUseGPUVoxels = bUseFusedVoxels || (GPUVoxels != nullptr && GPUVoxels->IsValid());
	const uint32 FusedVoxelCapacity = bUseFusedVoxels ? FusedVoxels->GetVoxelCapacity() : 0u;
	const int32 VoxelCapacityForHash = bUseFusedVoxels
		? int32(FusedVoxelCapacity)
		: (GPUVoxels != nullptr ? GPUVoxels->VoxelCapacity : 0);
	const uint32 GpuVoxelHashSlotCountPow2 = bUseGPUVoxels
		? FMath::RoundUpToPowerOfTwo(uint32(FMath::Max(VoxelCapacityForHash * 2, 16))) : 0u;
	// On the fused path these are CAPACITIES, not counts: the SC solve runs inside the leaf's graph
	// and its compact counts stay in VRAM, so every buffer and stream downstream is allocated for
	// the worst case and drawn from the GPU counts instead.
	if (bUseGPULines) B.FusedSC = *GPULines;
	const uint32 PathPointCount = bUseGPULines ? GPULines->TotalPointCapacity : uint32(PathPoints.Num());
	const uint32 SegmentCount = bUseGPULines ? GPULines->TotalSegmentCapacity : uint32(SegmentMeta.Num());
	const uint32 ProfileCount = bTube ? FMath::Max(TubeProfileCount, 3u) : 2u;
	const uint32 OutputVertexCount = PathPointCount * ProfileCount;
	const uint32 OutputIndexCount = bTube ? SegmentCount * ProfileCount * 6u : SegmentCount * 6u;

	B.bUseGPULines = bUseGPULines;
	B.bUseGPUVoxels = bUseGPUVoxels;
	B.GpuVoxelHashSlotCountPow2 = GpuVoxelHashSlotCountPow2;
	B.PathPointCount = PathPointCount;
	B.SegmentCount = SegmentCount;
	B.ProfileCount = ProfileCount;
	B.OutputVertexCount = OutputVertexCount;
	B.OutputIndexCount = OutputIndexCount;

	B.SafePostProjectionSmoothIterations = FMath::Max(0, PostProjectionSmoothIterations);
	B.SafePostProjectionSmoothKernelRadius = FMath::Max(1, PostProjectionSmoothKernelRadius);
	B.SafePostProjectionSmallSmoothIterations = FMath::Max(0, PostProjectionSmallSmoothIterations);
	B.SafePostProjectionSmoothAngleStrength = FMath::Clamp(PostProjectionSmoothAngleStrength, 0.0f, 1.0f);
	B.SafeNoiseIterations = uint32(FMath::Max(0, NoiseIterations));

	if (PathPointCount == 0
		|| SegmentCount == 0
		|| OutputVertexCount == 0
		|| OutputIndexCount == 0)
	{
		return B; // bValid stays false (silent count early-out)
	}

	// 融合路径下体素还没跑，容量是 CPU 侧就知道的 MaxVoxels；旧路径下取 pooled 的容量。
	// 两条路这里都是「容量」而非真实数量 —— 真实数量只有 GPU 的 Counter[0] 知道。
	const uint32 VoxelCount = bUseFusedVoxels ? FusedVoxelCapacity
		: (bUseGPUVoxels ? uint32(GPUVoxels->VoxelCapacity) : uint32(VoxelData.Cells.Num()));
	if (VoxelCount == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU_Voxel] No voxel data available."));
		return B; // bValid stays false
	}
	B.VoxelCount = VoxelCount;
	B.VoxelOrigin = bUseFusedVoxels ? FVector3f(FusedVoxels->GetVoxelOrigin())
		: (bUseGPUVoxels ? FVector3f(GPUVoxels->VoxelOrigin) : FVector3f(VoxelData.VoxelOrigin));
	B.VoxelSize = bUseFusedVoxels ? FusedVoxels->GetVoxelSize()
		: (bUseGPUVoxels ? GPUVoxels->VoxelSize : float(VoxelData.VoxelSize));

	if (bUseGPUVoxels)
	{
		// 融合路径把 pooled ref 留空 —— 图内 buffer 要等 BuildGeometry 记录 pass 时才存在。
		B.bUseFusedVoxels = bUseFusedVoxels;
		if (bUseFusedVoxels) B.SurfaceVoxelInputs = MoveTemp(*FusedVoxels);
		else
		{
			B.GPUVoxCells = GPUVoxels->Cells;
			B.GPUVoxNormals = GPUVoxels->Normals;
			B.GPUVoxTargets = GPUVoxels->TargetPositions;
			B.GPUVoxCounter = GPUVoxels->Counter;
		}
		B.GPUVoxCount = VoxelCount;
		B.GPUVoxelHashSlotCount = GpuVoxelHashSlotCountPow2;
		B.TargetBucketOrigin = B.VoxelOrigin;
		// This path never builds the target-position buckets (the voxels stay on the GPU), and a
		// zero-length upload yields a null SRV. TargetBucketCount == 0 already makes the shaders
		// skip the bucket lookup, but the bindings still have to exist or parameter validation
		// kills the render thread, so give them the one-element dummies.
		EnsureVineTargetBucketDummyBuffers(B.TargetBuckets);
		B.LocalBounds = bUseFusedVoxels ? B.SurfaceVoxelInputs.GetWorldBounds() : GPUVoxels->WorldBounds;
		if (!B.LocalBounds.IsValid)
		{
			const FVector Origin(B.VoxelOrigin);
			B.LocalBounds = FBox(Origin, Origin + FVector(B.VoxelSize * 4.0f));
		}
		B.bValid = true;
		return B;
	}

	// Convert voxel data to GPU-compatible format (repack), accumulating a conservative bounds.
	TArray<FVector4f>& GPUVoxelTargetPositions = B.GPUVoxelTargetPositions;
	TArray<FVector4f>& GPUVoxelNormals = B.GPUVoxelNormals;
	TArray<FIntVector4>& GPUVoxelCells = B.GPUVoxelCells;
	GPUVoxelTargetPositions.Reserve(VoxelCount);
	GPUVoxelNormals.Reserve(VoxelCount);
	GPUVoxelCells.Reserve(VoxelCount);

	auto IsFiniteVector = [](const FVector& Vector)
	{
		return FMath::IsFinite(Vector.X) && FMath::IsFinite(Vector.Y) && FMath::IsFinite(Vector.Z);
	};

	FBox TargetBounds(ForceInit);
	const float SafeVoxelSize = FMath::Max(VoxelData.VoxelSize, UE_KINDA_SMALL_NUMBER);
	const double MaxTargetDistanceSq = FMath::Square(double(SafeVoxelSize * 2.0f));
	int32 InvalidTargetCount = 0;
	int32 InvalidNormalCount = 0;
	int32 CoincidentTargetCount = 0;
	double TargetCenterDistanceSum = 0.0;
	double TargetCenterDistanceMax = 0.0;
	const double BuildVoxelUploadStartSeconds = FPlatformTime::Seconds();
	for (uint32 i = 0; i < VoxelCount; ++i)
	{
		const FIntVector& Cell = VoxelData.Cells[i];
		const FVector CellCenter(
			(double(Cell.X) + 0.5) * SafeVoxelSize + VoxelData.VoxelOrigin.X,
			(double(Cell.Y) + 0.5) * SafeVoxelSize + VoxelData.VoxelOrigin.Y,
			(double(Cell.Z) + 0.5) * SafeVoxelSize + VoxelData.VoxelOrigin.Z);
		const FVector VoxelCenter = VoxelData.Positions.IsValidIndex(int32(i)) && IsFiniteVector(VoxelData.Positions[i])
			? VoxelData.Positions[i]
			: CellCenter;

		FVector Target = VoxelData.TargetPositions.IsValidIndex(int32(i)) ? VoxelData.TargetPositions[i] : VoxelCenter;
		if (!IsFiniteVector(Target) || FVector::DistSquared(Target, VoxelCenter) > MaxTargetDistanceSq)
		{
			Target = VoxelCenter;
			++InvalidTargetCount;
		}
		const double TargetCenterDistance = FVector::Dist(Target, VoxelCenter);
		TargetCenterDistanceSum += TargetCenterDistance;
		TargetCenterDistanceMax = FMath::Max(TargetCenterDistanceMax, TargetCenterDistance);
		if (TargetCenterDistance <= UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			++CoincidentTargetCount;
		}

		FVector Normal = VoxelData.Normals.IsValidIndex(int32(i)) ? VoxelData.Normals[i] : FVector::UpVector;
		if (!IsFiniteVector(Normal) || !Normal.Normalize())
		{
			Normal = FVector::UpVector;
			++InvalidNormalCount;
		}

		GPUVoxelTargetPositions.Add(FVector4f(float(Target.X), float(Target.Y), float(Target.Z), 1.0f));
		GPUVoxelNormals.Add(FVector4f(float(Normal.X), float(Normal.Y), float(Normal.Z), 0.0f));
		GPUVoxelCells.Add(FIntVector4(Cell.X, Cell.Y, Cell.Z, 0));
		TargetBounds += Target;
	}
	if (OutBuildVoxelUploadMs) *OutBuildVoxelUploadMs = (FPlatformTime::Seconds() - BuildVoxelUploadStartSeconds) * 1000.0;

	if (InvalidTargetCount > 0 || InvalidNormalCount > 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[VisVineGPU_Voxel] Sanitized voxel upload. Voxels=%u InvalidTargets=%d InvalidNormals=%d"),
			VoxelCount,
			InvalidTargetCount,
			InvalidNormalCount);
	}
	UE_LOG(LogTemp, Display,
		TEXT("[VisVineGPU_Voxel] Upload target stats. Voxels=%u TargetCenterDist(avg=%.3f max=%.3f) CoincidentTargets=%d InvalidTargets=%d InvalidNormals=%d"),
		VoxelCount,
		VoxelCount > 0u ? TargetCenterDistanceSum / double(VoxelCount) : 0.0,
		TargetCenterDistanceMax,
		CoincidentTargetCount,
		InvalidTargetCount,
		InvalidNormalCount);

	uint32 GPUVoxelHashSlotCount = 0u;
	const double BuildHashStartSeconds = FPlatformTime::Seconds();
	if (!BuildVineVoxelHashSlots(GPUVoxelCells, B.GPUVoxelHashSlots, GPUVoxelHashSlotCount))
	{
		B.GPUVoxelHashSlots.Init(0u, 1);
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU_Voxel] Failed to build voxel hash slots; shader will use linear voxel lookup fallback."));
	}
	if (OutBuildHashMs) *OutBuildHashMs = (FPlatformTime::Seconds() - BuildHashStartSeconds) * 1000.0;

	const double BuildTargetBucketsStartSeconds = FPlatformTime::Seconds();
	B.TargetBucketOrigin = FVector3f(VoxelData.VoxelOrigin);
	if (!BuildVineTargetBucketBuffers(GPUVoxelTargetPositions, B.TargetBucketOrigin, SafeVoxelSize, B.TargetBuckets))
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU_Voxel] Failed to build target-position buckets; shader will use voxel-cell fallback."));
	}
	if (OutBuildTargetBucketsMs) *OutBuildTargetBucketsMs = (FPlatformTime::Seconds() - BuildTargetBucketsStartSeconds) * 1000.0;

	// Capture the GPU-resident voxel buffers (null on the CPU-array path). On the GPU-voxel
	// path the vine hash is rebuilt on the GPU, so bind the pow2 slot count.
	B.GPUVoxCells = bUseGPUVoxels ? GPUVoxels->Cells : nullptr;
	B.GPUVoxNormals = bUseGPUVoxels ? GPUVoxels->Normals : nullptr;
	B.GPUVoxTargets = bUseGPUVoxels ? GPUVoxels->TargetPositions : nullptr;
	B.GPUVoxCounter = bUseGPUVoxels ? GPUVoxels->Counter : nullptr;
	B.GPUVoxCount = bUseGPUVoxels ? uint32(GPUVoxels->VoxelCount) : 0u;
	B.GPUVoxelHashSlotCount = bUseGPUVoxels ? GpuVoxelHashSlotCountPow2 : GPUVoxelHashSlotCount;

	// World-space render bounds for the leaf: the surface targets bound where the vine settles;
	// expand generously (vine offset + a few voxels) so the indirect draw is never view-culled.
	if (TargetBounds.IsValid)
	{
		const double Margin = double(FMath::Max3(VinesOffset * 2.0f, SafeVoxelSize * 4.0f, 50.0f));
		B.LocalBounds = TargetBounds.ExpandBy(FVector(Margin));
	}

	B.bValid = true;
	return B;
}

// Fills an FVineMeshPassInputs whose raw array pointers reference B's owned arrays. B must outlive
// the returned struct (and the AddVineMeshPasses call that consumes it).
static FVineMeshPassInputs VineLeaf_MakePassInputs(const FVineBuildInput& B)
{
	FVineMeshPassInputs In;
	// GPULine* / LineCountsBuffer are RDG refs the caller fills in after recording the producing
	// passes into its own graph; only the CPU-side values travel through the bundle.
	In.bUseGPULines = B.bUseGPULines;
	In.PathPoints = &B.PathPoints;
	In.PathPointAxes = &B.PathPointAxes;
	In.PathPointMeta = &B.PathPointMeta;
	In.SegmentMeta = &B.SegmentMeta;
	In.bUseGPUVoxels = B.bUseGPUVoxels;
	In.GPUVoxCells = B.GPUVoxCells;
	In.GPUVoxNormals = B.GPUVoxNormals;
	In.GPUVoxTargets = B.GPUVoxTargets;
	In.GPUVoxCounter = B.GPUVoxCounter;
	In.GPUVoxCount = B.GPUVoxCount;
	In.GpuVoxelHashSlotCountPow2 = B.GpuVoxelHashSlotCountPow2;
	In.GPUVoxelCells = &B.GPUVoxelCells;
	In.GPUVoxelHashSlots = &B.GPUVoxelHashSlots;
	In.GPUVoxelNormals = &B.GPUVoxelNormals;
	In.GPUVoxelTargetPositions = &B.GPUVoxelTargetPositions;
	In.TargetBuckets = &B.TargetBuckets;
	In.TargetBucketOrigin = B.TargetBucketOrigin;
	In.VoxelOrigin = B.VoxelOrigin;
	In.VoxelSize = B.VoxelSize;
	In.VoxelCount = B.VoxelCount;
	In.GPUVoxelHashSlotCount = B.GPUVoxelHashSlotCount;
	In.PathPointCount = B.PathPointCount;
	In.SegmentCount = B.SegmentCount;
	In.ProfileCount = B.ProfileCount;
	In.OutputVertexCount = B.OutputVertexCount;
	In.OutputIndexCount = B.OutputIndexCount;
	In.bTube = B.bTube;
	In.CircleScale = B.CircleScale;
	In.LineScale = B.LineScale;
	In.UVLengthScale = B.UVLengthScale;
	In.VinesOffset = B.VinesOffset;
	In.TinyZJitterStrength = B.TinyZJitterStrength;
	In.SafePostProjectionSmoothIterations = B.SafePostProjectionSmoothIterations;
	In.SafePostProjectionSmoothKernelRadius = B.SafePostProjectionSmoothKernelRadius;
	In.SafePostProjectionSmallSmoothIterations = B.SafePostProjectionSmallSmoothIterations;
	In.SafePostProjectionSmoothAngleStrength = B.SafePostProjectionSmoothAngleStrength;
	In.bResampleSurface = B.bResampleSurface;
	In.ResampleTargetDistance = B.ResampleTargetDistance;
	In.CurlNoiseStrength = B.CurlNoiseStrength;
	In.CurlNoiseFrequency = B.CurlNoiseFrequency;
	In.PerlinNoiseStrength = B.PerlinNoiseStrength;
	In.PerlinNoiseFrequency = B.PerlinNoiseFrequency;
	In.SafeNoiseIterations = B.SafeNoiseIterations;
	In.DebugStage = B.DebugStage;
	return In;
}

}

// Concatenated line geometry produced inside the leaf's own graph by the fused SC passes. All
// four are graph-lifetime transients — nothing has to survive GraphBuilder.Execute() any more.
struct FVineFusedSCOutputs
{
	FRDGBufferRef PathPoints = nullptr;
	FRDGBufferRef PathPointMeta = nullptr;
	FRDGBufferRef SegmentMeta = nullptr;
	FRDGBufferRef Counts = nullptr; // [0]=lineCount [1]=pointCount [2]=segmentCount
};

// Records the whole space-colonization solve (once per source) plus the GPU prefix sum and concat
// into GraphBuilder. Defined below, next to the SC shader helpers it needs.
static bool AddVineFusedSCConcatPasses(FRDGBuilder& GraphBuilder, const FVineFusedSCInputs& SC, FVineFusedSCOutputs& Out);

// ============================================================================
// GPU-resident vine leaf: UVineMeshComponent + FVineMeshSceneProxy. This IS the vine.
// It consumes the shared FVineBuildInput bundle and drives AddVineMeshPasses with the base-stream
// permutation, so the vine mesh is built + drawn entirely on the GPU (no readback). The
// implementation lives here in
// GeometryEditorActor.cpp so it can see the file-local AddVineMeshPasses / FVineMeshPassInputs /
// FVineMeshPassOutputs / VineLeaf_MakePassInputs without exposing them in a header.
// ============================================================================

// Scene proxy: registers the base standard-triangle streams at the batch-wide capacity (the real
// counts only exist on the GPU), then records the space colonization solve, the concat and the
// shared vine pass graph into ONE FRDGBuilder writing straight into those streams.
class FVineMeshSceneProxy final : public FCSGpuMeshSceneProxy
{
public:
	FVineMeshSceneProxy(UVineMeshComponent* Component, const FVineBuildInput& InInput)
		: FCSGpuMeshSceneProxy(Component, Component->VineMaterial, "FVineMeshSceneProxy")
		, DebugVertexFactory(GetScene().GetFeatureLevel(), "FVineDebugLineVertexFactory")
		, Input(InInput)
	{
		bDrawDebugLines = Input.DebugStage != EVisVineGPUDebugStage::None && Input.SegmentCount > 0u;
	}

	virtual SIZE_T GetTypeHash() const override
	{
		static size_t UniquePointer;
		return reinterpret_cast<size_t>(&UniquePointer);
	}

	virtual void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override
	{
		if (bDrawDebugLines)
		{
			FCSGpuDebugDraw::AllocatePositionStream(RHICmdList, DebugPositions, Input.PathPointCount,
				TEXT("VineDebug.Positions"));
			FCSGpuDebugDraw::AllocateIndexBuffer(RHICmdList, DebugIndices, FMath::Max(Input.SegmentCount * 2u, 2u),
				TEXT("VineDebug.Indices"));
			FCSGpuDebugDraw::BindPositionOnlyVertexFactory(RHICmdList, DebugVertexFactory, DebugPositions);
		}
		FCSGpuMeshSceneProxy::CreateRenderThreadResources(RHICmdList);
	}

	virtual void DestroyRenderThreadResources() override
	{
		if (bDrawDebugLines)
		{
			DebugVertexFactory.ReleaseResource();
			FCSGpuDebugDraw::ReleaseIndexBuffer(DebugIndices);
			FCSGpuDebugDraw::ReleasePositionStream(DebugPositions);
		}
		FCSGpuMeshSceneProxy::DestroyRenderThreadResources();
	}

	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap, FMeshElementCollector& Collector) const override
	{
		FCSGpuMeshSceneProxy::GetDynamicMeshElements(Views, ViewFamily, VisibilityMap, Collector);
		if (!bDrawDebugLines || !DebugIndices.Pooled.IsValid()) return;
		FCSGpuDebugDraw::SubmitColoredDraw(*this, Views, VisibilityMap, Collector, DebugVertexFactory,
			DebugIndices, PT_LineList, Input.DebugLineColor, Input.SegmentCount, Input.PathPointCount - 1u);
	}

protected:
	virtual void RegisterStreams() override
	{
		// The vine tube is a triangle soup (no shared vertices), so index capacity tracks the
		// vertex capacity via the standard set. Both are CAPACITIES derived from
		// VV.MaxVinePointCount: the real counts are decided by the space colonization solve that
		// now runs inside BuildGeometry, and asking for them here would be circular. The indirect
		// draw reads the real index count VineDispatchArgsCS publishes.
		VertexCapacity = FMath::Max(Input.OutputVertexCount, 64u);
		IndexCapacity = FMath::Max(Input.OutputIndexCount, 192u);
		AddStandardTriangleStreams();
	}

	virtual void BuildGeometry(FRHICommandListBase& RHICmdList) override
	{
		FRHICommandListImmediate& RHICmdListImmediate = FRHICommandListExecutor::GetImmediateCommandList();
		FRDGBuilder GraphBuilder(RHICmdListImmediate, RDG_EVENT_NAME("VineMesh.Build"));

		// The base marks DrawDesc valid as soon as an index buffer exists, so every bail-out below
		// still has to publish a zero-index draw: the pooled allocator hands back whatever the last
		// owner left in the indirect args, and the index buffer is now capacity-sized.
		auto PublishEmptyDraw = [this, &GraphBuilder]()
		{
			FRDGBufferRef ArgsBuf = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::IndirectArgs));
			FRDGBufferRef ZeroCountersBuf = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::MeshCounters));
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(ArgsBuf, PF_R32_UINT)), 0u);
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(ZeroCountersBuf, PF_R32_UINT)), 0u);
			GraphBuilder.SetBufferAccessFinal(ArgsBuf, ERHIAccess::IndirectArgs);
			GraphBuilder.SetBufferAccessFinal(ZeroCountersBuf, ERHIAccess::CopySrc);
			GraphBuilder.Execute();
		};

		if (!Input.bValid || Input.OutputVertexCount == 0u || Input.OutputIndexCount == 0u)
		{
			PublishEmptyDraw();
			return;
		}

		// Register the seven base-owned persistent streams and build typed UAVs matching the
		// shader's RWBuffer<float>/RWBuffer<uint> declarations (same formats the road path uses).
		FRDGBufferRef PositionBuf = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::Position));
		FRDGBufferRef TangentBuf = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::TangentBasis));
		FRDGBufferRef TexCoordBuf = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::TexCoord));
		FRDGBufferRef ColorBuf = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::Color));
		FRDGBufferRef IndexBuf = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::Index));
		FRDGBufferRef IndirectBuf = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::IndirectArgs));
		FRDGBufferRef CountersBuf = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::MeshCounters));

		FVineMeshPassInputs In = VineLeaf_MakePassInputs(Input);

		// 表面体素也记录进这张图。放在 SC 求解之前：求解要投影到表面，读的就是这里产出的
		// 体素。同图内靠 UAV 依赖自动定序，不需要中间 Execute，也不需要把结果 extract 成
		// pooled buffer 再 register 回来。
		if (Input.bUseFusedVoxels)
		{
			FCSSurfaceVoxelPassOutputs VoxelOut;
			if (!AddCSSurfaceVoxelPasses(GraphBuilder, Input.SurfaceVoxelInputs, VoxelOut))
			{
				UE_LOG(LogTemp, Warning, TEXT("[VineMesh] 融合体素 pass 记录失败（范围内没有可体素化的几何）。"));
				PublishEmptyDraw();
				return;
			}
			In.GPUVoxCellsRDG = VoxelOut.Cells;
			In.GPUVoxNormalsRDG = VoxelOut.Normals;
			In.GPUVoxTargetsRDG = VoxelOut.TargetPositions;
			In.GPUVoxCounterRDG = VoxelOut.Counter;
		}

		if (Input.bUseGPULines)
		{
			// Solve + concat straight into this graph; the results stay transient and the counts
			// never leave the GPU.
			FVineFusedSCOutputs Lines;
			if (!AddVineFusedSCConcatPasses(GraphBuilder, Input.FusedSC, Lines))
			{
				PublishEmptyDraw();
				return;
			}
			In.GPULinePoints = Lines.PathPoints;
			In.GPULineMeta = Lines.PathPointMeta;
			In.GPULineSeg = Lines.SegmentMeta;
			In.LineCountsBuffer = Lines.Counts;
		}

		FVineMeshPassOutputs Out;
		Out.bBaseStreams = true;
		Out.PositionUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(PositionBuf, PF_R32_FLOAT));
		Out.TangentUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TangentBuf, PF_R32_UINT));
		Out.TexCoordUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TexCoordBuf, PF_R32_FLOAT));
		Out.ColorUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(ColorBuf, PF_R32_UINT));
		Out.IndexUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndexBuf, PF_R32_UINT));
		Out.IndirectArgsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectBuf, PF_R32_UINT));
		Out.MeshCountersUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CountersBuf, PF_R32_UINT));
		// Identity for M1: the vine renders in world space (component sits at an identity transform).
		Out.VineWorldToLocal = Input.VineWorldToLocal;
		FRDGBufferRef DebugCenterSource = nullptr;
		FRDGBufferRef SegmentMetaSource = nullptr;
		if (bDrawDebugLines)
		{
			Out.DebugCenterSourceBufferPtr = &DebugCenterSource;
			Out.SegmentMetaBufferPtr = &SegmentMetaSource;
		}

		AddVineMeshPasses(GraphBuilder, GetScene().GetFeatureLevel(), In, Out);

		if (bDrawDebugLines)
		{
			check(DebugCenterSource && SegmentMetaSource);
			FRDGBufferRef DebugPositionBuf = GraphBuilder.RegisterExternalBuffer(DebugPositions.Buffer.Pooled, TEXT("VineDebug.Positions.External"));
			FRDGBufferRef DebugIndexBuf = GraphBuilder.RegisterExternalBuffer(DebugIndices.Pooled, TEXT("VineDebug.Indices.External"));
			// The center line is float4 on the GPU; the shared debug layout is float triples.
			FCSGpuDebugDraw::AddPositionUnpackPass(GraphBuilder, GetScene().GetFeatureLevel(), DebugCenterSource,
				Input.PathPointCount, DebugPositionBuf);

			FCSGpuDebugDraw::AddLineIndicesPass(GraphBuilder, GetScene().GetFeatureLevel(), SegmentMetaSource,
				Input.SegmentCount, DebugIndexBuf);
			GraphBuilder.SetBufferAccessFinal(DebugPositionBuf, ERHIAccess::VertexOrIndexBuffer);
			GraphBuilder.SetBufferAccessFinal(DebugIndexBuf, ERHIAccess::VertexOrIndexBuffer);
		}

		// Leave the persistent buffers in the states the draw / readback paths need; RDG's default
		// epilogue (SRVMask) is illegal for index / indirect usage (mirrors RoadMeshSceneProxy).
		GraphBuilder.SetBufferAccessFinal(PositionBuf, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
		GraphBuilder.SetBufferAccessFinal(TangentBuf, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
		GraphBuilder.SetBufferAccessFinal(TexCoordBuf, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
		GraphBuilder.SetBufferAccessFinal(ColorBuf, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
		GraphBuilder.SetBufferAccessFinal(IndexBuf, ERHIAccess::VertexOrIndexBuffer);
		GraphBuilder.SetBufferAccessFinal(IndirectBuf, ERHIAccess::IndirectArgs);
		GraphBuilder.SetBufferAccessFinal(CountersBuf, ERHIAccess::CopySrc);

		GraphBuilder.Execute();
	}

private:
	FLocalVertexFactory DebugVertexFactory;
	FCSGpuDebugPositionStream DebugPositions;
	FPooledIndexBuffer DebugIndices;
	bool bDrawDebugLines = false;
	FVineBuildInput Input;
};

UVineMeshComponent::UVineMeshComponent()
{
	// GPU-Scene instance culling overrides custom indirect args in the Virtual Shadow Map / cube-
	// shadow passes, so indirect-drawn meshes cannot cast VSM shadows; keep shadows off (as roads do).
	CastShadow = false;
}

UVineMeshComponent::~UVineMeshComponent() = default;

// Defined here (not left to UHT's generated TU) so TUniquePtr<FVineBuildInput>'s destructor is
// instantiated where the bundle type is complete.
UVineMeshComponent::UVineMeshComponent(FVTableHelper& Helper) : Super(Helper) {}

void UVineMeshComponent::SetBuildInput(FVineBuildInput&& Input)
{
	LocalBounds = Input.LocalBounds;
	PendingInput = MakeUnique<FVineBuildInput>(MoveTemp(Input));
	// Immediate recreate (not the deferred MarkRenderStateDirty) so a synchronous SaveToStaticMesh
	// right after a rebuild sees the freshly built proxy after FlushRenderingCommands.
	RecreateRenderState_Concurrent();
	UpdateBounds();
}

FPrimitiveSceneProxy* UVineMeshComponent::CreateSceneProxy()
{
	if (GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5) return nullptr;
	if (!PendingInput.IsValid() || !PendingInput->bValid) return nullptr;
	if (PendingInput->OutputVertexCount == 0u || PendingInput->OutputIndexCount == 0u) return nullptr;
	return new FVineMeshSceneProxy(this, *PendingInput);
}

#if WITH_EDITOR
UStaticMesh* UVineMeshComponent::SaveToStaticMesh(const FString& AssetPathAndName, bool bReplaceExistingAsset,
	bool bSaveAsset, bool bConvertToActorLocalSpace)
{
	return SaveRenderedMeshToStaticMesh(
		AssetPathAndName, VineMaterial, GetComponentTransform(),
		bConvertToActorLocalSpace, bReplaceExistingAsset, bSaveAsset);
}
#endif

static void ApplyVineReferenceComponentsHiddenInGame(AVineContainer* Container)
{
	if (!Container)
	{
		return;
	}

	if (Container->GrowTarget)
	{
		Container->GrowTarget->SetHiddenInGame(true);
	}
	if (Container->TubeVineSource)
	{
		Container->TubeVineSource->SetHiddenInGame(true);
	}
}

static void RefreshVineDisplayComponent(UInstancedStaticMeshComponent* Component)
{
	if (!Component)
	{
		return;
	}

	Component->SetVisibility(true, false);
	Component->SetHiddenInGame(true);
	Component->UpdateBounds();
	Component->MarkRenderStateDirty();
}

static void RebuildVineDisplayInstances(UInstancedStaticMeshComponent* Component, const TArray<FTransform>& Transforms)
{
	if (!Component)
	{
		return;
	}

	Component->ClearInstances();
	if (!Transforms.IsEmpty())
	{
		Component->AddInstances(Transforms, false, true, false);
	}
	RefreshVineDisplayComponent(Component);
}

static bool ResolveVineReferenceComponent(
	AVineContainer* Container,
	const UFoliageType* InFoliageType,
	UInstancedStaticMeshComponent*& OutDisplayComponent)
{
	OutDisplayComponent = nullptr;
	if (!Container || !InFoliageType)
	{
		return false;
	}

	const FString FoliageTypeName = InFoliageType->GetName();
	if (InFoliageType == Container->TubeType || FoliageTypeName == TEXT("SMF_TubeVine_FoliageType"))
	{
		OutDisplayComponent = Container->TubeVineSource;
		return true;
	}
	if (InFoliageType == Container->TargetType || FoliageTypeName == TEXT("SMF_Target_FoliageType"))
	{
		OutDisplayComponent = Container->GrowTarget;
		return true;
	}

	return false;
}


AVineContainer::AVineContainer(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)	
{
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	GrowTarget = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("GrowTarget"));
	GrowTarget->SetStaticMesh(Mesh);
	GrowTarget->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Overlap);
	GrowTarget->SetCollisionObjectType(ECollisionChannel::ECC_WorldDynamic);
	GrowTarget->SetHiddenInGame(true);
	GrowTarget->SetupAttachment(GetRootComponent(), TEXT("GrowTarget"));

	TubeVineSource = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("TubePoints"));
	TubeVineSource->SetStaticMesh(Mesh);
	TubeVineSource->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Overlap);
	TubeVineSource->SetCollisionObjectType(ECollisionChannel::ECC_WorldDynamic);
	TubeVineSource->SetVisibility(true, false);
	TubeVineSource->SetHiddenInGame(true);
	TubeVineSource->SetupAttachment(GetRootComponent(), TEXT("TubePoints"));

	// The GPU-resident vine leaf. The vine geometry is emitted in world space, so keep this
	// component at an identity WORLD transform regardless of where the actor is placed: mark the
	// transform absolute (ignore the parent) and leave it at identity. VineWorldToLocal is Identity
	// to match (see FVineMeshSceneProxy::BuildGeometry).
	VineGpuMesh = CreateDefaultSubobject<UVineMeshComponent>(TEXT("VineMesh"));
	VineGpuMesh->SetupAttachment(GetRootComponent());
	VineGpuMesh->SetUsingAbsoluteLocation(true);
	VineGpuMesh->SetUsingAbsoluteRotation(true);
	VineGpuMesh->SetUsingAbsoluteScale(true);
	VineGpuMesh->SetRelativeTransform(FTransform::Identity);

	ApplyVineReferenceComponentsHiddenInGame(this);
	RebuildDisplayInstancesFromTransformArrays();
}

void AVineContainer::PostLoad()
{
	Super::PostLoad();
	MigrateLegacyVineMaterial();
	ApplyVineMaterialToLeaf();
}

void AVineContainer::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();

	// MarkPackageDirty refuses to do anything while the editor is still loading the package, so the
	// upgrade below is re-flagged here, once the level is up, to surface it as an unsaved change.
	if (bPendingLegacyVineMaterialDirty)
	{
		bPendingLegacyVineMaterialDirty = false;
		MarkPackageDirty();
	}
}

#if WITH_EDITOR
void AVineContainer::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Retargeting the surface material must show up without re-running a whole generation, so push
	// it down and let the leaf rebuild its proxy against the new material.
	const FName ChangedName = PropertyChangedEvent.GetPropertyName();
	if (ChangedName == GET_MEMBER_NAME_CHECKED(AVineContainer, VineMaterial))
	{
		ApplyVineMaterialToLeaf();
		if (VineGpuMesh) VineGpuMesh->MarkRenderStateDirty();
	}
}
#endif

void AVineContainer::ApplyVineMaterialToLeaf()
{
	if (VineGpuMesh) VineGpuMesh->VineMaterial = VineMaterial;
}

// Subobject name of the UDynamicMeshComponent this actor used to own. Levels and blueprints saved
// before its removal still carry that subobject, and the linker recreates it as an orphan under the
// actor (the property that used to point at it is simply skipped), so PostLoad can still read it.
static const FName LegacyVineDynamicMeshComponentName(TEXT("DynamicMeshComponent"));

void AVineContainer::MigrateLegacyVineMaterial()
{
	// A value already set means this actor was upgraded on an earlier load, inherited one from an
	// upgraded blueprint CDO, or was authored after the removal — never clobber it.
	if (VineMaterial) return;

	UMaterialInterface* LegacyMaterial = nullptr;
	ForEachObjectWithOuter(this, [&LegacyMaterial](UObject* Child)
	{
		if (LegacyMaterial || Child->GetFName() != LegacyVineDynamicMeshComponentName) return;
		// GetMaterial is virtual on UPrimitiveComponent, so the orphan resolves its own slot 0
		// (UBaseDynamicMeshComponent::BaseMaterials) without this module knowing the concrete type.
		if (UPrimitiveComponent* LegacyComponent = Cast<UPrimitiveComponent>(Child)) LegacyMaterial = LegacyComponent->GetMaterial(0);
	}, /*bIncludeNestedObjects*/ false);

	if (!LegacyMaterial) return;

	VineMaterial = LegacyMaterial;
	// Commandlets are allowed to dirty on load; the editor is not, hence the deferred re-flag in
	// PostRegisterAllComponents. Either way the migration is idempotent, so a package that never
	// gets resaved simply migrates again on the next load rather than losing the assignment.
	MarkPackageDirty();
	bPendingLegacyVineMaterialDirty = true;
	UE_LOG(LogTemp, Display,
		TEXT("[VineContainer] Migrated legacy DynamicMeshComponent slot-0 material '%s' to AVineContainer::VineMaterial on %s. Save the owning package to persist it."),
		*LegacyMaterial->GetPathName(),
		*GetPathName());
}

void AVineContainer::RebuildDisplayInstancesFromTransformArrays()
{
	RefreshVineDisplayComponent(GrowTarget);
	RefreshVineDisplayComponent(TubeVineSource);
}

void AVineContainer::ImportFoliageToTransformArray(UFoliageType* InFoliageType)
{
	if (InFoliageType == nullptr)
		return;

	TArray<FTransform> Transforms;
	GetAllFoliageInstanceTransforms(GetWorld(), InFoliageType, Transforms);
	UInstancedStaticMeshComponent* FoliageDisplayComponent = nullptr;
	if (!ResolveVineReferenceComponent(this, InFoliageType, FoliageDisplayComponent) || !FoliageDisplayComponent)
	{
		return;
	}

	Modify();
	FoliageDisplayComponent->Modify();
	if (!Transforms.IsEmpty())
	{
		FoliageDisplayComponent->AddInstances(Transforms, false, true, false);
	}
	MarkPackageDirty();
	RefreshVineDisplayComponent(FoliageDisplayComponent);

	for (TActorIterator<AInstancedFoliageActor> It(GetWorld()); It; ++It)
	{
		AInstancedFoliageActor* IFA = (*It);
		IFA->RemoveFoliageType(&InFoliageType, 1);
	}
}

void AVineContainer::ExportTransformArrayToFoliage(UFoliageType* InFoliageType)
{
	if (InFoliageType == nullptr)
		return;

	UInstancedStaticMeshComponent* FoliageDisplayComponent = nullptr;
	if (!ResolveVineReferenceComponent(this, InFoliageType, FoliageDisplayComponent) || !FoliageDisplayComponent)
		return;

	TArray<FTransform> InstanceTransforms;
	GetVineInstanceTransforms(FoliageDisplayComponent, InstanceTransforms);

	if (InstanceTransforms.IsEmpty())
	{
		RefreshVineDisplayComponent(FoliageDisplayComponent);
		return;
	}

	UWorld* World = GetWorld();
	if (!World || !World->PersistentLevel)
	{
		return;
	}

	Modify();
	TMap<AInstancedFoliageActor*, TArray<const FFoliageInstance*>> InstancesToAdd;
	TArray<FFoliageInstance> FoliageInstances;
	FoliageInstances.Reserve(InstanceTransforms.Num());

	for (const FTransform& InstanceTransform : InstanceTransforms)
	{
		AInstancedFoliageActor* IFA = AInstancedFoliageActor::Get(World, true, World->PersistentLevel, InstanceTransform.GetLocation());
		if (!IFA)
		{
			continue;
		}

		FFoliageInstance FoliageInstance;
		FoliageInstance.Location = InstanceTransform.GetLocation();
		FoliageInstance.Rotation = InstanceTransform.GetRotation().Rotator();
		FoliageInstance.DrawScale3D = (FVector3f)InstanceTransform.GetScale3D();

		FoliageInstances.Add(FoliageInstance);
		InstancesToAdd.FindOrAdd(IFA).Add(&FoliageInstances[FoliageInstances.Num() - 1]);
	}

	for (const auto& Pair : InstancesToAdd)
	{
		FFoliageInfo* TypeInfo = nullptr;
		if (UFoliageType* FoliageType = Pair.Key->AddFoliageType(InFoliageType, &TypeInfo))
		{
			TypeInfo->AddInstances(FoliageType, Pair.Value);
		}
	}

	FoliageDisplayComponent->Modify();
	FoliageDisplayComponent->ClearInstances();
	MarkPackageDirty();
	RefreshVineDisplayComponent(FoliageDisplayComponent);
	RefreshFoliageType(GetWorld(), InFoliageType);
}

bool AVineContainer::VisVine()
{
	return VisVineGPUInternal();
}

bool AVineContainer::VisVineGPUInternal()
{
	const TArray<FGeometryScriptPolyPath>& Lines = TubeLines; // GPU path leaves this empty; used only for logging.

	if (VV.ResampleLength <= KINDA_SMALL_NUMBER)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] Invalid ResampleLength: %.4f"), VV.ResampleLength);
		return false;
	}

	if (VV.CurveControl == nullptr)
	{
		VV.CurveControl = NewObject<UCurveLinearColor>(this);
	}

	if (!VineGpuMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] Vine GPU mesh component is null."));
		return false;
	}

	if (!LastSurfaceVoxelGPUBuffers.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] No retained GPU surface voxel data for visualization."));
		return false;
	}
	const FCSSurfaceVoxelData EmptySurfaceVoxelData;

	// The space-colonization solve is recorded into the leaf's own graph, so all this side needs
	// is its CPU-prepped inputs; the resulting counts stay on the GPU.
	TArray<FTransform> SCSourceTransforms;
	TArray<FTransform> SCTargetTransforms;
	GetVineInstanceTransforms(TubeVineSource, SCSourceTransforms);
	GetVineInstanceTransforms(GrowTarget, SCTargetTransforms);
	FVineFusedSCInputs FusedSCInputs;
	if (!PrepareVineFusedSCInputs(SCSourceTransforms, SCTargetTransforms, FusedSCInputs))
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] No vine sources or targets to solve."));
		return false;
	}

	// The line geometry is produced on the GPU, so the CPU-side input arrays the shared helper
	// takes are empty — it reads everything from FusedSCInputs.
	TArray<FVector4f> PathPoints;
	TArray<FVector4f> PathPointAxes;
	TArray<FIntVector4> PathPointMeta;
	TArray<FIntVector4> SegmentMeta;

	UE_LOG(LogTemp, Display, TEXT("[VineSCFused] sources=%d targets=%d pointCapacity=%u"),
		FusedSCInputs.Sources.Num(), FusedSCInputs.InitialTargetPositions.Num(), FusedSCInputs.TotalPointCapacity);

	// 粗细自查，替代随 CPU 回读一起删掉的旧 [VisVineThickness]。管子半径是
	//   radius = 10 * CircleScale * CurveLUT(t) * targetScale * sourceScale
	// 四个乘数全部在 CPU 侧就能拿到（GPU 只负责把它们乘起来），所以“藤蔓变细了”不用再靠猜 GPU
	// 里的值：把每个乘数的分布打出来，是参数问题还是几何问题一眼就能分开。
	{
		auto ScaleStats = [](const TArray<float>& Values, float& OutMin, float& OutAvg, float& OutMax)
		{
			OutMin = 0.0f; OutAvg = 0.0f; OutMax = 0.0f;
			if (Values.Num() == 0) return;
			OutMin = TNumericLimits<float>::Max();
			OutMax = TNumericLimits<float>::Lowest();
			double Sum = 0.0;
			for (float Value : Values)
			{
				OutMin = FMath::Min(OutMin, Value);
				OutMax = FMath::Max(OutMax, Value);
				Sum += Value;
			}
			OutAvg = float(Sum / Values.Num());
		};

		float TargetMin, TargetAvg, TargetMax;
		ScaleStats(FusedSCInputs.TargetPointScales, TargetMin, TargetAvg, TargetMax);
		float CurveMin, CurveAvg, CurveMax;
		ScaleStats(FusedSCInputs.CurveLUT, CurveMin, CurveAvg, CurveMax);

		// StartSourceScales 是 1.0 铺底的 per-target 数组，取不出源本身的缩放；SourcePositions[0].W
		// 才是 GetSpaceColonizationTransformScale(SourceTransform) 的原值。
		TArray<float> SourceScales;
		SourceScales.Reserve(FusedSCInputs.Sources.Num());
		for (const FVineSCPreparedSource& PreparedSource : FusedSCInputs.Sources)
		{
			if (PreparedSource.SourcePositions.Num() > 0) SourceScales.Add(PreparedSource.SourcePositions[0].W);
		}
		float SourceMin, SourceAvg, SourceMax;
		ScaleStats(SourceScales, SourceMin, SourceAvg, SourceMax);

		UE_LOG(LogTemp, Display,
			TEXT("[VisVineThickness] CircleScale=%.4f profileCount=%u | targetScale[min=%.4f avg=%.4f max=%.4f n=%d] ")
			TEXT("sourceScale[min=%.4f avg=%.4f max=%.4f n=%d] curveLUT[min=%.4f avg=%.4f max=%.4f] ")
			TEXT("=> typicalRadius=%.4f"),
			VV.CircleScale, uint32(FMath::Max(VV.VisVineGPUTubeSegments, 3)),
			TargetMin, TargetAvg, TargetMax, FusedSCInputs.TargetPointScales.Num(),
			SourceMin, SourceAvg, SourceMax, SourceScales.Num(),
			CurveMin, CurveAvg, CurveMax,
			10.0f * VV.CircleScale * CurveAvg * TargetAvg * SourceAvg);

		// 空的 UCurveLinearColor 上 GetUnadjustedLinearColorValue().G 返回 0，会把整条藤蔓压成零
		// 半径；曲线被误改成低幅度同样直接按比例变细。这两种都是“看起来变细”的常见真因。
		if (CurveMax <= UE_KINDA_SMALL_NUMBER)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[VisVineThickness] CurveControl 求值全为 0（曲线为空或未设关键帧），藤蔓半径会塌成 0。请检查 VV.CurveControl 的 G 通道。"));
		}
	}

	const EVisVineGPUDebugStage DebugStage = SplineDebug.DebugStage;
	const bool bWantStageDraw = SplineDebug.bDrawDebugLines && DebugStage != EVisVineGPUDebugStage::None;
	uint32 LeafVertexCount = 0;
	uint32 LeafIndexCount = 0;

	// The GPU-resident leaf is the vine. The shared vine compute passes emit straight into its
	// persistent base streams (positions / tangents / texcoords / colors / indices) plus the
	// indirect args and mesh counters that drive the draw, so no vertex, index or count ever
	// comes back to the CPU. Save Mesh reads the streams back once, on demand.
	const double BuildLeafStartSeconds = FPlatformTime::Seconds();
	{
		FVineBuildInput LeafInput = VineLeaf_BuildVineBuildInput(
			PathPoints,
			PathPointAxes,
			PathPointMeta,
			SegmentMeta,
			true,
			uint32(FMath::Max(VV.VisVineGPUTubeSegments, 3)),
			VV.CircleScale,
			VV.LineScale,
			VV.UVLengthScale,
			VV.VinesOffset,
			0.1f,
			VV.VisVineGPUPostProjectionSmoothIterations,
			VV.VisVineGPUPostProjectionSmoothKernelRadius,
			VV.VisVineGPUPostProjectionSmallSmoothIterations,
			VV.VisVineGPUPostProjectionSmoothAngleStrength,
			VV.bVisVineGPUResampleSurfaceEnabled,
			VV.ResampleLength,
			VV.CurlNoiseScale / 10.0f,
			VV.CurlNoiseFre,
			VV.PerlinNoiseScale,
			VV.PerlinNoiseFre,
			VV.VisVineGPUNoiseIterations,
			EmptySurfaceVoxelData,
			bWantStageDraw ? DebugStage : EVisVineGPUDebugStage::None,
			&FusedSCInputs,
			LastSurfaceVoxelGPUBuffers.IsValid() ? &LastSurfaceVoxelGPUBuffers : nullptr,
			// GenerateVineGPU 备好的融合体素输入。有效时叶子在自己的图里现算体素，
			// 上面那个 pooled 参数就用不上了；会被 MoveTemp 进 bundle，故本次消费后即失效。
			PendingSurfaceVoxelInputs.IsValid() ? &PendingSurfaceVoxelInputs : nullptr);
		if (!LeafInput.bValid)
		{
			UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] Vine build input was invalid; nothing generated."));
			return false;
		}

		LeafInput.DebugLineColor = SplineDebug.SplineColor;
		LeafVertexCount = LeafInput.OutputVertexCount;
		LeafIndexCount = LeafInput.OutputIndexCount;
		// The actor owns the material assignment; push it down before SetBuildInput so the proxy
		// rebuilt in there already reads the current one.
		ApplyVineMaterialToLeaf();
		VineGpuMesh->SetBuildInput(MoveTemp(LeafInput));
	}
	const double BuildLeafMs = (FPlatformTime::Seconds() - BuildLeafStartSeconds) * 1000.0;

	// The selected center-line stage is rendered directly from GPU buffers by VineGpuMesh.
	// Clear any persistent CPU DrawDebug lines left by an older generation.
	ClearDebugVineSplineActor();

	UE_LOG(LogTemp, Display, TEXT("[VisVineGPUTiming] tube buildLeaf=%.3f ms"), BuildLeafMs);
	UE_LOG(LogTemp, Log, TEXT("[VisVineGPU] Built tube vine on the GPU. Lines=%d Vertices=%u Indices=%u"),
		Lines.Num(),
		LeafVertexCount,
		LeafIndexCount);
	return true;
}

inline void AVineContainer::Clean()
{
	TubeLines.Empty();
	TubeLineSourceLocations.Empty();
	TubeLinePointScales.Empty();
	TubeLinePointAxes.Empty();
	CachedSurfaceTriangles = FCSTriangleMeshData();
	ClearDebugVineSplineActor();
}


static const FName VineDebugSplineActorTag(TEXT("VineDebugSplineActor"));

void AVineContainer::ClearDebugVineSplineActor()
{
	if (DebugVineSplineActor)
	{
		DebugVineSplineActor->Modify();
		DebugVineSplineActor->Destroy();
		DebugVineSplineActor = nullptr;
	}

	TArray<AActor*> AttachedActors;
	GetAttachedActors(AttachedActors);
	for (AActor* AttachedActor : AttachedActors)
	{
		if (AttachedActor && AttachedActor->Tags.Contains(VineDebugSplineActorTag))
		{
			AttachedActor->Modify();
			AttachedActor->Destroy();
		}
	}
}

void AVineContainer::DrawDebugVineCenterLines(
	const TArray<FVector4f>& CenterPoints,
	const TArray<FIntVector4>& PathPointMeta)
{
	// Remove any leftover spline actor from older builds; this path draws transient lines instead.
	ClearDebugVineSplineActor();

	if (CenterPoints.Num() == 0 || PathPointMeta.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineSplineDebug] No stage output to draw center lines."));
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const int32 MetaCount = FMath::Min(CenterPoints.Num(), PathPointMeta.Num());

	// Group the flat point array back into per-vine point ranges using PathPointMeta:
	// component .Z == BaseIndex (vine start), .W == PointCount (vine length).
	TMap<int32, int32> BaseIndexToPointCount;
	for (int32 PointIndex = 0; PointIndex < MetaCount; ++PointIndex)
	{
		const FIntVector4& Meta = PathPointMeta[PointIndex];
		BaseIndexToPointCount.FindOrAdd(Meta.Z) = Meta.W;
	}

	if (BaseIndexToPointCount.Num() == 0)
	{
		return;
	}

	const int32 MinPoints = FMath::Max(2, SplineDebug.MinPointsPerSpline);
	const FColor LineColor = SplineDebug.SplineColor.ToFColor(true);
	const float Thickness = FMath::Max(0.0f, SplineDebug.LineThickness);
	const bool bPersistent = SplineDebug.bPersistentLines;
	const float Duration = SplineDebug.DebugDuration;

	int32 DrawnVineCount = 0;
	for (const TPair<int32, int32>& VineRange : BaseIndexToPointCount)
	{
		const int32 BaseIndex = VineRange.Key;
		const int32 PointCount = VineRange.Value;
		if (PointCount < MinPoints)
		{
			continue;
		}

		FVector PrevPoint = FVector::ZeroVector;
		bool bHasPrev = false;
		int32 DrawnSegments = 0;
		for (int32 LocalIndex = 0; LocalIndex < PointCount; ++LocalIndex)
		{
			const int32 PointIndex = BaseIndex + LocalIndex;
			if (!CenterPoints.IsValidIndex(PointIndex)) continue;

			const FVector4f& Packed = CenterPoints[PointIndex];
			const FVector Center(Packed.X, Packed.Y, Packed.Z);
			if (!IsFiniteVineVector(Center)) continue;

			if (bHasPrev)
			{
				DrawDebugLine(World, PrevPoint, Center, LineColor, bPersistent, Duration, 0, Thickness);
				++DrawnSegments;
			}

			PrevPoint = Center;
			bHasPrev = true;
		}

		if (DrawnSegments > 0)
		{
			++DrawnVineCount;
		}
	}

	const TCHAR* StageName = TEXT("?");
	switch (SplineDebug.DebugStage)
	{
		case EVisVineGPUDebugStage::FinalProject: StageName = TEXT("FP"); break;
		case EVisVineGPUDebugStage::Resample:     StageName = TEXT("RS"); break;
		case EVisVineGPUDebugStage::Smooth:       StageName = TEXT("B"); break;
		case EVisVineGPUDebugStage::None:         StageName = TEXT("None"); break;
	}
	UE_LOG(LogTemp, Log, TEXT("[VisVineSplineDebug] Drew center lines for %d vines at stage %s thickness=%.2f centers=%d."),
		DrawnVineCount, StageName, Thickness, CenterPoints.Num());
}

bool AVineContainer::GenerateVines(float ExtrudeScale, bool Result)
{
	GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.Total"));
	(void)ExtrudeScale;
	(void)Result;

	// 1. 收集 Source / Target Transforms
	TArray<FTransform> TubeSourceTransforms;
	TArray<FTransform> TargetTransforms;
	GetVineInstanceTransforms(TubeVineSource, TubeSourceTransforms);
	GetVineInstanceTransforms(GrowTarget, TargetTransforms);

	const int32 TargetCount = TargetTransforms.Num();
	const int32 TubeSourceCount = TubeSourceTransforms.Num();

	if (TargetCount == 0 || TubeSourceCount == 0)
	{
		return false;
	}

	// 2. 计算 BoundingBox 并查找场景中重叠的 Actor
	TArray<FTransform> BBoxTransforms;
	BBoxTransforms.Append(TubeSourceTransforms);
	BBoxTransforms.Append(TargetTransforms);

	TArray<FVector> BBoxVectors;
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.BuildBoundsInput"));
		BBoxVectors.Reserve(BBoxTransforms.Num());
		for (const FTransform& Transform : BBoxTransforms)
		{
			BBoxVectors.Add(Transform.GetLocation());
		}
	}

	FBox Bounds(BBoxVectors);
	Bounds = Bounds.ExpandBy(50);
	const FVector Center = Bounds.GetCenter();
	const FVector Extent = Bounds.GetExtent();

	SurfaceVoxelBlurIterations = FMath::Max(0, VV.GenerateVineVoxelNormalBlurIterations);

	// 3~5 步（三角形缓存 / 表面体素 / 建网格）全部收进 GenerateVineGPU，在那里合并成一张 RDG 图。
	return GenerateVineGPU(Bounds);
}

// 藤蔓生成的 GPU 段：三角形缓存 -> 表面体素 -> 空间殖民求解 -> concat -> 建网格。
//
// 这些以前是 4 张 RDG 图外加一次 FlushRenderingCommands：EnsureTriangleCache 之后
// PrepareBoxSceneSurfaceVoxelsGPU 自己开三张图（清零 / 分批体素化 / finalize+blur），把结果
// extract 成 pooled buffer，然后阻塞 game thread —— 而藤蔓这条路传的是 bReadbackToCPU=false，
// 没有任何数据要回读，那次 flush 纯粹是为了让 CPU 能读一个 IsValid()。
//
// 现在这里只做 CPU 侧准备，体素的 pass 由叶子的 FVineMeshSceneProxy::BuildGeometry 记录进它
// 自己那张图，和 SC 求解、建网格合并执行：没有中间 Execute，没有 pooled extract，也没有 flush。
// 分批仍然保留（见 AddCSSurfaceVoxelPasses），显存行为不变。
bool AVineContainer::GenerateVineGPU(const FBox& Bounds)
{
	GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVineGPU.Total"));
	const FVector Center = Bounds.GetCenter();
	const FVector Extent = Bounds.GetExtent();

	// 1. 三角形缓存：纯 CPU / 组件操作，只是把 GeneratorBounds 摆到这一批藤蔓的包围盒上，
	//    后面的体素查询范围（GetGeneratorBoundsWorldBox）就是从它读的。
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVineGPU.EnsureTriangleCache"));
		VoxelGridSettings.VoxelSize = SC.VoxelSize;
		VoxelGridSettings.ActivationRadius = SC.VoxelSize * 8.0f;
		FCSMeshGeneratorTriangleCacheHandle TriangleCacheHandle = EnsureTriangleCacheByBox(
			TEXT("VineGenerate"),
			Center,
			Extent,
			false);
		(void)TriangleCacheHandle;
	}

	// 2. 体素：只做 CPU 侧准备（收集并解析三角形请求、地形三角形、参数），不 dispatch。
	//    产出的 bundle 一路带到叶子的图里由 AddCSSurfaceVoxelPasses 记录。
	//
	// 参考点剔除距离。三个三角形展开 pass（Extract / FilterInitialTriangleSoup /
	// AppendNaniteSource）本来就是边展开边按参考点剔除、原子追加出紧凑 soup 的，但这里以前传
	// 0 —— 而 0 会让 ReferenceFilterDistanceSq 取 FLT_MAX，同时 bUseReferenceFilter 仍是 1
	// （ReferencePoints 是非空的 target 列表）。结果是每个三角形对全部 target 跑一遍无 early-out
	// 的最近距离循环，然后无条件通过：代价照付，一个不剔。给个真实距离这层剔除才真正生效。
	//
	// ReferencePoints 就是 target 位置，藤蔓中心线不会长到离 target 更远的地方，所以按生长影响
	// 半径的倍数取即可。下限压着体素投影的 ActivationRadius(= VoxelSize*8)：剔除范围一旦小于
	// 投影搜索范围，边界处就会找不到吸附目标，中心线会飘离表面。
	const float SurfaceTriangleFilterDistance = FMath::Max(SC.InfluenceRadius * 2.0f, SC.VoxelSize * 8.0f);
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVineGPU.PrepareSurfaceVoxelInputs"));
		PendingSurfaceVoxelInputs = FCSSurfaceVoxelPassInputs();
		if (!PrepareSurfaceVoxelPassInputs(SC.VoxelSize, SurfaceTriangleFilterDistance, PendingSurfaceVoxelInputs))
		{
			UE_LOG(LogTemp, Warning, TEXT("[GenerateVineGPU] 范围内没有可体素化的几何，%s 无法生成藤蔓。"), *GetActorNameOrLabel());
			return false;
		}
	}
	CachedSurfaceTriangles = FCSTriangleMeshData();
	// Cache generation bounds for subsequent GPU visualization.
	InstanceBound = Bounds;

	// 3. SpaceColonization 不再在这里跑：它已经和 concat、建网格合并进 VisVine 的那张 RDG 图，
	// 由 VisVine() 自行从当前的 source/target transforms 准备输入。这些 CPU 线数组随之作废
	// （GPU 路径从不填充它们），保持清空以匹配旧行为。
	TubeLines.Reset();
	TubeLineSourceLocations.Reset();
	TubeLinePointScales.Reset();
	TubeLinePointAxes.Reset();

	// 体素调试线画的是 LastSurfaceVoxelGPUBuffers，那是旧 pooled 路径的产物；融合路径下体素
	// 只活在叶子的图里，没有 pooled 副本可画。要用这个调试就得走旧接口单独跑一次体素。
	if (GPUProjectionDebug.bDrawGPUProjectionVoxelDebugPoints && GPUProjectionDebug.GPUProjectionVoxelDebugDuration > 0.0f)
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVineGPU.DrawGPUProjectionVoxelDebugPoints"));
		if (PrepareBoxSceneSurfaceVoxelsGPU(SC.VoxelSize))
		{
			FCSDebugLastVoxelDirectionOptions DebugOptions;
			DebugOptions.DirectionLength = SC.VoxelSize;
			DebugOptions.DirectionColor = GPUProjectionDebug.GPUProjectionVoxelTargetColor;
			DebugOptions.Duration = GPUProjectionDebug.GPUProjectionVoxelDebugDuration;
			DebugOptions.bPersistentLines = GPUProjectionDebug.bGPUProjectionVoxelDebugPointsPersistent;
			DebugOptions.bDrawPoints = true;
			DebugOptions.PointColor = GPUProjectionDebug.GPUProjectionVoxelCenterColor;
			DebugOptions.PointSize = GPUProjectionDebug.GPUProjectionVoxelCenterPointSize;
			DebugOptions.MaxDirectionsToDraw = GPUProjectionDebug.GPUProjectionVoxelDebugPointLimit;
			DrawDebugLastSurfaceVoxelDirections(DebugOptions);
		}
	}

	// 4. 可视化：把这一批藤蔓交给 GPU leaf。结果只存在于它的常驻 stream 里，没有 CPU 网格可返回。
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVineGPU.VisVine"));
		return VisVine();
	}
}

void AVineContainer::FetchFoliage()
{
	// SetActorLocation(FVector(0.0f, 0.0f, 0.0f));
	ImportFoliageToTransformArray(TargetType);
	ImportFoliageToTransformArray(TubeType);
	TubeVineSource->SetHiddenInGame(false);
	RebuildDisplayInstancesFromTransformArrays();
}

void AVineContainer::RevertFoliage()
{
	ExportTransformArrayToFoliage(TargetType);
	ExportTransformArrayToFoliage(TubeType);

}

FString AVineContainer::GetResultAssetBaseName() const
{
	// 基类默认用 GetName()；藤蔓历史上一直用编辑器标签（用户可见的名字），
	// 沿用之以免已烘好的资产改名。标签可能含空格等非法字符，故要 sanitize。
	const FString Label = ObjectTools::SanitizeObjectName(GetActorNameOrLabel());
	return Label.IsEmpty() ? Super::GetResultAssetBaseName() : Label;
}

void AVineContainer::GenerateVineAction()
{
	ReferencePoints.Reset();

	TArray<FTransform> TargetTransforms;
	GetVineInstanceTransforms(GrowTarget, TargetTransforms);

	const int32 LastTargetIndex = TargetTransforms.Num() - 1;
	UKismetSystemLibrary::PrintString(
		this,
		FString::FromInt(LastTargetIndex),
		true,
		true,
		FLinearColor(0.0f, 0.66f, 1.0f, 1.0f),
		2.0f);

	for (const FTransform& TargetTransform : TargetTransforms)
	{
		ReferencePoints.Add(TargetTransform.GetLocation());
	}

	UKismetSystemLibrary::PrintString(
		this,
		FString::FromInt(ReferencePoints.Num()),
		true,
		true,
		FLinearColor(0.0f, 0.66f, 1.0f, 1.0f),
		2.0f);

	if (!GenerateVines(50.0f, true)) UE_LOG(LogTemp, Warning, TEXT("[VineContainer] GenerateVineAction produced no vine on %s."), *GetActorNameOrLabel());
}

int32 AVineContainer::DrawDebugCachedVineSCStagePoints(float Duration)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return 0;
	}

	if (!SCStageDebug.bSCStageDrawTube)
	{
		return 0;
	}

	const float EffectiveDuration = Duration > 0.0f ? Duration : SCStageDebug.SCStageDebugPointDuration;
	if (EffectiveDuration <= 0.0f)
	{
		return 0;
	}

	const float SafePointSize = FMath::Max(SCStageDebug.SCStageDebugPointSize, 0.0f);
	const FColor DebugColor = SCStageDebug.SCStageTubeDebugPointColor.ToFColor(true);
	const int32 PointLimit = SCStageDebug.SCStageDebugPointLimit;
	const bool bHasLimit = PointLimit > 0;
	int32 DrawnPointCount = 0;

	for (const FGeometryScriptPolyPath& Line : TubeLines)
	{
		if (bHasLimit && DrawnPointCount >= PointLimit)
		{
			break;
		}

		if (!Line.Path.IsValid())
		{
			continue;
		}

		for (const FVector& Point : *Line.Path)
		{
			if (bHasLimit && DrawnPointCount >= PointLimit)
			{
				break;
			}

			DrawDebugPoint(World, Point, SafePointSize, DebugColor,
				SCStageDebug.bSCStageDebugPointsPersistent,
				EffectiveDuration,
				0);
			++DrawnPointCount;
		}
	}

	if (DrawnPointCount == 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[VineSCDebug] No cached SC-stage points on %s. Run GenerateVines before drawing cached SC-stage points."),
			*GetActorNameOrLabel());
	}
	else
	{
		UE_LOG(LogTemp, Display,
			TEXT("[VineSCDebug] Drew cached SC-stage points. TubeLines=%d Points=%d Duration=%.3f"),
			TubeLines.Num(),
			DrawnPointCount,
			EffectiveDuration);
	}

	return DrawnPointCount;
}

int32 AVineContainer::DrawDebugVineSurfaceVoxelArrows(float Duration, bool bUseCachedVoxels)
{
	const float SafeVoxelSize = FMath::Max(SC.VoxelSize, 1.0e-3f);

	if (bUseCachedVoxels)
	{
		if (!LastSurfaceVoxelGPUBuffers.IsValid())
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[VineVoxelDebug] No retained GPU surface voxel data on %s. Run GenerateVines before drawing cached voxel data."),
				*GetActorNameOrLabel());
			return 0;
		}
	}
	else
	{
		TArray<FTransform> TubeSourceTransforms;
		TArray<FTransform> TargetTransforms;
		GetVineInstanceTransforms(TubeVineSource, TubeSourceTransforms);
		GetVineInstanceTransforms(GrowTarget, TargetTransforms);

		const int32 TargetCount = TargetTransforms.Num();
		const int32 SourceCount = TubeSourceTransforms.Num();
		if (TargetCount == 0 || SourceCount == 0)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[VineVoxelDebug] Cannot build surface voxels on %s. Targets=%d Sources=%d"),
				*GetActorNameOrLabel(),
				TargetCount,
				SourceCount);
			return 0;
		}

		ReferencePoints.Reset();
		ReferencePoints.Reserve(TargetCount);
		for (const FTransform& TargetTransform : TargetTransforms)
		{
			ReferencePoints.Add(TargetTransform.GetLocation());
		}

		TArray<FTransform> BBoxTransforms;
		BBoxTransforms.Reserve(SourceCount + TargetCount);
		BBoxTransforms.Append(TubeSourceTransforms);
		BBoxTransforms.Append(TargetTransforms);

		TArray<FVector> BBoxVectors;
		BBoxVectors.Reserve(BBoxTransforms.Num());
		for (const FTransform& Transform : BBoxTransforms)
		{
			BBoxVectors.Add(Transform.GetLocation());
		}

		FBox Bounds(BBoxVectors);
		Bounds = Bounds.ExpandBy(50);
		if (!Bounds.IsValid)
		{
			UE_LOG(LogTemp, Warning, TEXT("[VineVoxelDebug] Invalid vine debug bounds on %s."), *GetActorNameOrLabel());
			return 0;
		}

		VoxelGridSettings.VoxelSize = SafeVoxelSize;
		VoxelGridSettings.ActivationRadius = SafeVoxelSize * 8.0f;
		EnsureTriangleCacheByBox(
			TEXT("VineDebugSurfaceVoxelArrows"),
			Bounds.GetCenter(),
			Bounds.GetExtent(),
			false);
		if (!PrepareBoxSceneSurfaceVoxelsGPU(SafeVoxelSize)) return 0;
	}

	FCSDebugLastVoxelDirectionOptions DebugOptions;
	DebugOptions.DirectionLength = SurfaceVoxelDebug.SurfaceVoxelArrowLength > 0.0f
		? SurfaceVoxelDebug.SurfaceVoxelArrowLength
		: SafeVoxelSize;
	DebugOptions.DirectionColor = SurfaceVoxelDebug.SurfaceVoxelArrowColor;
	const float EffectiveArrowDuration = Duration > 0.0f ? Duration : SurfaceVoxelDebug.SurfaceVoxelArrowDuration;
	DebugOptions.Duration = FMath::Max(0.0f, EffectiveArrowDuration);
	DebugOptions.Thickness = SurfaceVoxelDebug.SurfaceVoxelArrowThickness;
	DebugOptions.bPersistentLines = SurfaceVoxelDebug.bSurfaceVoxelArrowPersistentLines;
	DebugOptions.bDrawPoints = SurfaceVoxelDebug.bSurfaceVoxelDrawVoxelCenters;
	DebugOptions.PointColor = SurfaceVoxelDebug.SurfaceVoxelCenterColor;
	DebugOptions.PointSize = SurfaceVoxelDebug.SurfaceVoxelCenterPointSize;
	DebugOptions.MaxDirectionsToDraw = SurfaceVoxelDebug.SurfaceVoxelMaxArrowsToDraw;
	return DrawDebugLastSurfaceVoxelDirections(DebugOptions);
}

int32 AVineContainer::DrawDebugCachedSurfaceTriangles(float Duration)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return 0;
	}

	if (!TriangleDebug.bDrawTriangles)
	{
		return 0;
	}

	const int32 EffectiveVertexCount = CachedSurfaceTriangles.VertexCount >= 0
		? FMath::Min(CachedSurfaceTriangles.VertexCount, CachedSurfaceTriangles.Vertices.Num())
		: CachedSurfaceTriangles.Vertices.Num();

	const int32 EffectiveIndexCount = CachedSurfaceTriangles.IndexCount >= 0
		? FMath::Min(CachedSurfaceTriangles.IndexCount, CachedSurfaceTriangles.Indices.Num())
		: CachedSurfaceTriangles.Indices.Num();

	const bool bUseIndices = EffectiveIndexCount >= 3;
	const int32 TriangleCount = bUseIndices
		? EffectiveIndexCount / 3
		: EffectiveVertexCount / 3;

	if (TriangleCount <= 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[VineTriangleDebug] No cached surface triangles on %s. Vertices=%d EffectiveVertices=%d Indices=%d EffectiveIndices=%d. Run GenerateVines first."),
			*GetActorNameOrLabel(),
			CachedSurfaceTriangles.Vertices.Num(),
			EffectiveVertexCount,
			CachedSurfaceTriangles.Indices.Num(),
			EffectiveIndexCount);
		return 0;
	}

	const float EffectiveDuration = Duration > 0.0f ? Duration : TriangleDebug.TriangleDebugDuration;
	if (EffectiveDuration <= 0.0f)
	{
		return 0;
	}

	const int32 DrawLimit = TriangleDebug.TriangleDebugCountLimit > 0
		? FMath::Min(TriangleDebug.TriangleDebugCountLimit, TriangleCount)
		: TriangleCount;

	const float SafeThickness = FMath::Max(0.0f, TriangleDebug.TriangleLineThickness);
	const float SafePointSize = FMath::Max(0.0f, TriangleDebug.TriangleVertexPointSize);
	const float SafeNormalLength = FMath::Max(0.0f, TriangleDebug.TriangleNormalLength);
	const FColor LineColor = TriangleDebug.TriangleLineColor.ToFColor(true);
	const FColor VertexColor = TriangleDebug.TriangleVertexColor.ToFColor(true);
	const FColor NormalColor = TriangleDebug.TriangleNormalColor.ToFColor(true);
	const float TriDuration = EffectiveDuration;
	const bool bPersistent = TriangleDebug.bTriangleDebugPersistent;

	auto GetTriangleVertex = [&](int32 TriIndex, int32 LocalVertexIndex) -> FVector
	{
		if (bUseIndices)
		{
			const int32 BaseIdx = TriIndex * 3;
			const int32 VertIdx = CachedSurfaceTriangles.Indices[BaseIdx + LocalVertexIndex];
			return CachedSurfaceTriangles.Vertices.IsValidIndex(VertIdx) ? CachedSurfaceTriangles.Vertices[VertIdx] : FVector::ZeroVector;
		}
		else
		{
			const int32 VertIdx = TriIndex * 3 + LocalVertexIndex;
			return CachedSurfaceTriangles.Vertices.IsValidIndex(VertIdx) ? CachedSurfaceTriangles.Vertices[VertIdx] : FVector::ZeroVector;
		}
	};

	auto IsFiniteVec = [](const FVector& V) -> bool
	{
		return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z);
	};

	int32 DrawnLineSegments = 0;
	int32 DrawnVertexPoints = 0;
	int32 DrawnNormalArrows = 0;
	int32 SkippedTriangles = 0;

	for (int32 TriIndex = 0; TriIndex < DrawLimit; ++TriIndex)
	{
		const FVector V0 = GetTriangleVertex(TriIndex, 0);
		const FVector V1 = GetTriangleVertex(TriIndex, 1);
		const FVector V2 = GetTriangleVertex(TriIndex, 2);

		if (!IsFiniteVec(V0) || !IsFiniteVec(V1) || !IsFiniteVec(V2))
		{
			++SkippedTriangles;
			continue;
		}

		// Draw triangle wireframe
		DrawDebugLine(World, V0, V1, LineColor, bPersistent, TriDuration, 0, SafeThickness);
		DrawDebugLine(World, V1, V2, LineColor, bPersistent, TriDuration, 0, SafeThickness);
		DrawDebugLine(World, V2, V0, LineColor, bPersistent, TriDuration, 0, SafeThickness);
		DrawnLineSegments += 3;

		// Draw vertex points
		if (TriangleDebug.bDrawTriangleVertices && SafePointSize > 0.0f)
		{
			DrawDebugPoint(World, V0, SafePointSize, VertexColor, bPersistent, TriDuration, 0);
			DrawDebugPoint(World, V1, SafePointSize, VertexColor, bPersistent, TriDuration, 0);
			DrawDebugPoint(World, V2, SafePointSize, VertexColor, bPersistent, TriDuration, 0);
			DrawnVertexPoints += 3;
		}

		// Draw normal arrow from triangle centroid
		if (TriangleDebug.bDrawTriangleNormals && SafeNormalLength > 0.0f)
		{
			const FVector Centroid = (V0 + V1 + V2) / 3.0;
			FVector FaceNormal = FVector::CrossProduct(V1 - V0, V2 - V0);
			if (FaceNormal.Normalize())
			{
				const float ArrowHeadSize = FMath::Max(SafeNormalLength * 0.15f, SafeThickness * 4.0f);
				DrawDebugDirectionalArrow(
					World,
					Centroid,
					Centroid + FaceNormal * SafeNormalLength,
					ArrowHeadSize,
					NormalColor,
					bPersistent,
					TriDuration,
					0,
					SafeThickness);
				++DrawnNormalArrows;
			}
		}
	}

	UE_LOG(LogTemp, Display,
		TEXT("[VineTriangleDebug] Drew cached surface triangles on %s. Triangles=%d/%d LineSegments=%d VertexPoints=%d NormalArrows=%d Skipped=%d "
			 "bUseIndices=%d EffectiveVertices=%d EffectiveIndices=%d Duration=%.1f"),
		*GetActorNameOrLabel(),
		DrawLimit,
		TriangleCount,
		DrawnLineSegments,
		DrawnVertexPoints,
		DrawnNormalArrows,
		SkippedTriangles,
		bUseIndices ? 1 : 0,
		EffectiveVertexCount,
		EffectiveIndexCount,
		TriDuration);

	return DrawLimit;
}

void AVineContainer::SaveStaticmesh()
{
	if (!VineGpuMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineContainer] SaveStaticmesh skipped: no vine GPU mesh on %s."), *GetActorNameOrLabel());
		return;
	}

	// 路径与编号统一由基类的结果资产命名策略产出（<关卡目录>/AutoResult/SM_<基名>_<编号>）；
	// 基名沿用本类覆写的"标签优先"口径，故已烘好的资产名不变。
	const FString AssetPathAndName = BuildResultAssetPath();
	if (AssetPathAndName.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineContainer] SaveStaticmesh failed: %s has no level content path."), *GetActorNameOrLabel());
		return;
	}
	const FString AssetName = FPackageName::GetShortName(AssetPathAndName);

	// The vine only ever exists as GPU streams now, so this is the one point where it comes back:
	// the shared base reads the rendered streams once and converts them straight to a StaticMesh.
	//
	// The leaf itself is pinned to an identity world transform, so its own component transform is
	// useless for the local-space bake. Pass the ACTOR transform instead — it is what the spawned
	// StaticMeshActor below is placed at, so the asset ends up actor-local rather than pinned to
	// world coordinates.
	UStaticMesh* NewStaticMesh = VineGpuMesh->SaveRenderedMeshToStaticMesh(
		AssetPathAndName,
		VineMaterial,
		GetActorTransform(),
		true,
		true,
		true);
	if (!NewStaticMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineContainer] SaveStaticmesh failed: could not create %s (no rendered vine geometry?)."), *AssetPathAndName);
		return;
	}

	// 清场 + 生成挂接结果 actor 的整套生命周期都在基类（清旧/打标签/设网格/挂接/命名/标脏）。
	if (!SpawnAttachedResultActor(NewStaticMesh, AssetName))
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineContainer] Saved StaticMesh but could not spawn actor for %s."), *AssetPathAndName);
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("[VineContainer] Created unsaved StaticMesh asset: %s"), *AssetPathAndName);
}

namespace
{
constexpr int32 SpaceColonizationMaxNeighborsPerTarget = 128;

struct FSpaceColonizationGPUState4
{
	int32 X = 0;
	int32 Y = 0;
	int32 Z = 0;
	int32 W = 0;
};

static_assert(sizeof(FSpaceColonizationGPUState4) == 16, "Space colonization GPU state must match HLSL int4.");

static float GetSpaceColonizationTransformScale(const FTransform& Transform)
{
	const FVector Scale = Transform.GetScale3D();
	return FMath::Max3(FMath::Abs(Scale.X), FMath::Abs(Scale.Y), FMath::Abs(Scale.Z));
}

static void BuildSpaceColonizationScaleLookups(
	const TArray<FTransform>& SourceTransforms,
	const TArray<FTransform>& TargetTransforms,
	TArray<float>& OutTargetPointScales,
	TArray<float>& OutStartSourceScales)
{
	OutTargetPointScales.Reset();
	OutStartSourceScales.Reset();

	const int32 TargetCount = TargetTransforms.Num();
	OutTargetPointScales.Reserve(TargetCount);
	OutStartSourceScales.Init(1.0f, TargetCount);
	if (TargetCount == 0)
	{
		return;
	}

	TArray<FVector> TargetLocations;
	TargetLocations.Reserve(TargetCount);
	for (const FTransform& TargetTransform : TargetTransforms)
	{
		TargetLocations.Add(TargetTransform.GetLocation());
		OutTargetPointScales.Add(GetSpaceColonizationTransformScale(TargetTransform));
	}

	for (const FTransform& SourceTransform : SourceTransforms)
	{
		const int32 NearPointIndex = UPointFunction::FindNearPointIteration(TargetLocations, SourceTransform.GetLocation());
		if (NearPointIndex != -1)
		{
			OutStartSourceScales[NearPointIndex] = GetSpaceColonizationTransformScale(SourceTransform);
		}
	}
}

// One solve's post-resample output. Graph-lifetime buffers over-allocated to the source's point
// capacity; only [0, Counts[1]) / [0, Counts[2]) hold valid data, and those counts stay on the GPU.
struct FVineSCPassOutputs
{
	FRDGBufferRef PathPoints = nullptr;    // float4(xyz, finalScale = CurveScale * pointScale)
	FRDGBufferRef PathPointMeta = nullptr; // int4(Prev, Next, Base, Count)
	FRDGBufferRef SegmentMeta = nullptr;   // int4(A, B, 0, 0)
	FRDGBufferRef Counts = nullptr;        // [lineCount, points, segments, 0]
};

// Records one space-colonization solve into GraphBuilder. Everything that used to wrap this pass
// sequence — the render command, the two FlushRenderingCommands, the four-uint count readback and
// the ConvertToExternalBuffer of the result — is gone: the caller records this into the vine mesh
// graph, so the output stays transient and the counts never reach the CPU.
static bool AddVineSCPasses(
	FRDGBuilder& GraphBuilder,
	const FVineFusedSCInputs& SC,
	const FVineSCPreparedSource& Source,
	FVineSCPassOutputs& Out)
{
	const TArray<FVector4f>& SourcePositions = Source.SourcePositions;
	const TArray<FVector4f>& InitialTargetPositions = SC.InitialTargetPositions;
	const TArray<float>& EmitTargetPointScales = SC.TargetPointScales;
	const TArray<float>& EmitStartSourceScales = Source.StartSourceScales;
	const TArray<float>& EmitCurveLUT = SC.CurveLUT;

	const int32 SourceCount = SourcePositions.Num();
	const int32 TargetCount = InitialTargetPositions.Num();
	if (SourceCount == 0 || TargetCount == 0) return false;

	// Iteration <= 0 still runs Init/MarkSources so the result matches the CPU path, which returns
	// the marked queue even when the growth loop never runs.
	const int32 IterationCount = FMath::Max(SC.Iteration, 0);
	const int32 Activetime = SC.Activetime;
	const float RandGrow = SC.RandGrow;
	const float Seed = SC.Seed;
	const float InfluenceRadius = SC.InfluenceRadius;
	const int32 BackGrowCount = SC.BackGrowCount;
	const int32 ForkTaperForkOrdinal = SC.ForkTaperForkOrdinal;
	const float ResampleLength = SC.ResampleLength;
	const float ScatterDistance = SC.ScatterDistance;
	const uint32 CurveLUTSize = uint32(EmitCurveLUT.Num());

	// SC_MAX_NEIGHBORS_CAP in SpaceColonizationQueue.usf must stay >= this value.
	const int32 MaxNeighborsPerTarget = FMath::Clamp(SpaceColonizationMaxNeighborsPerTarget, 1, TargetCount);
	const uint64 NeighborIndexCount64 = uint64(TargetCount) * uint64(MaxNeighborsPerTarget);
	if (NeighborIndexCount64 > uint64(TNumericLimits<uint32>::Max()))
	{
		UE_LOG(LogTemp, Warning, TEXT("[SpaceColonizationQueueCS] GPU request too large. TargetCount=%d MaxNeighbors=%d"), TargetCount, MaxNeighborsPerTarget);
		return false;
	}
	const uint32 NeighborIndexCount = uint32(NeighborIndexCount64);

	// Stage B2 emit-kernel output. Only [0,totalPoints) is written compactly; the rest is slack.
	const uint32 PathPointCapacity = FMath::Max(1u, Source.PointCapacity);
	const uint32 SegmentCapacity = PathPointCapacity;
	// Resample can grow lines; keep a generous compact capacity for the post-resample set.
	const uint32 PathPoint2Capacity = PathPointCapacity;
	const uint32 Segment2Capacity = PathPointCapacity;

	CREATE_RDG_STRUCTURED_UPLOAD_SRV(Source, FVector4f, SourcePositions, TEXT("SpaceColonizationQueue_SourcePositions"))
	CREATE_RDG_STRUCTURED_UPLOAD_SRV(InitialTarget, FVector4f, InitialTargetPositions, TEXT("SpaceColonizationQueue_InitialTargetPositions"))
	CREATE_RDG_STRUCTURED_UAV_SRV(Target, FVector4f, TargetCount, TEXT("SpaceColonizationQueue_TargetPositions"))
	CREATE_RDG_STRUCTURED_UAV_SRV(State0, FSpaceColonizationGPUState4, TargetCount, TEXT("SpaceColonizationQueue_State0"))
	CREATE_RDG_STRUCTURED_UAV_SRV(State1, FSpaceColonizationGPUState4, TargetCount, TEXT("SpaceColonizationQueue_State1"))
	CREATE_RDG_STRUCTURED_UAV_SRV(NeighborCounts, uint32, TargetCount, TEXT("SpaceColonizationQueue_NeighborCounts"))
	CREATE_RDG_STRUCTURED_UAV_SRV(NeighborIndices, uint32, NeighborIndexCount, TEXT("SpaceColonizationQueue_NeighborIndices"))
	CREATE_RDG_STRUCTURED_UAV_SRV(ProposalOwners, uint32, TargetCount, TEXT("SpaceColonizationQueue_ProposalOwners"))
	CREATE_RDG_STRUCTURED_UAV_SRV(ProposalPositions, FVector4f, TargetCount, TEXT("SpaceColonizationQueue_ProposalPositions"))
	CREATE_RDG_STRUCTURED_UAV_SRV(Claims, uint32, TargetCount, TEXT("SpaceColonizationQueue_Claims"))
	CREATE_RDG_STRUCTURED_UAV_SRV(ClaimCounts, uint32, TargetCount, TEXT("SpaceColonizationQueue_ClaimCounts"))
	CREATE_RDG_STRUCTURED_UAV_SRV(ProposalTargets, uint32, TargetCount, TEXT("SpaceColonizationQueue_ProposalTargets"))

	TShaderMapRef<FSpaceColonizationQueueInitCS> InitShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FSpaceColonizationQueueInitCS::FParameters* InitParameters = GraphBuilder.AllocParameters<FSpaceColonizationQueueInitCS::FParameters>();
	InitParameters->InitialTargetPositions = InitialTargetSRV;
	InitParameters->RW_TargetPositions = TargetUAV;
	InitParameters->RW_State0 = State0UAV;
	InitParameters->RW_State1 = State1UAV;
	InitParameters->TargetCount = uint32(TargetCount);
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("SpaceColonizationQueue.Init"),
		InitParameters,
		ERDGPassFlags::Compute,
		[InitParameters, InitShader, TargetCount](FRHIComputeCommandList& InRHICmdList)
		{
			FComputeShaderUtils::Dispatch(InRHICmdList, InitShader, *InitParameters, FComputeShaderUtils::GetGroupCount(FIntVector(TargetCount, 1, 1), 64));
		});

	TShaderMapRef<FSpaceColonizationQueueMarkSourcesCS> MarkSourcesShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FSpaceColonizationQueueMarkSourcesCS::FParameters* MarkParameters = GraphBuilder.AllocParameters<FSpaceColonizationQueueMarkSourcesCS::FParameters>();
	MarkParameters->SourcePositions = SourceSRV;
	MarkParameters->InitialTargetPositions = InitialTargetSRV;
	MarkParameters->RW_TargetPositions = TargetUAV;
	MarkParameters->RW_State0 = State0UAV;
	MarkParameters->RW_State1 = State1UAV;
	MarkParameters->SourceCount = uint32(SourceCount);
	MarkParameters->TargetCount = uint32(TargetCount);
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("SpaceColonizationQueue.MarkSources"),
		MarkParameters,
		ERDGPassFlags::Compute,
		[MarkParameters, MarkSourcesShader, SourceCount](FRHIComputeCommandList& InRHICmdList)
		{
			FComputeShaderUtils::Dispatch(InRHICmdList, MarkSourcesShader, *MarkParameters, FComputeShaderUtils::GetGroupCount(FIntVector(SourceCount, 1, 1), 64));
		});

	TShaderMapRef<FSpaceColonizationQueueBuildNeighborsCS> BuildNeighborsShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FSpaceColonizationQueueBuildNeighborsCS::FParameters* BuildNeighborsParameters = GraphBuilder.AllocParameters<FSpaceColonizationQueueBuildNeighborsCS::FParameters>();
	BuildNeighborsParameters->InitialTargetPositions = InitialTargetSRV;
	BuildNeighborsParameters->RW_NeighborCounts = NeighborCountsUAV;
	BuildNeighborsParameters->RW_NeighborIndices = NeighborIndicesUAV;
	BuildNeighborsParameters->TargetCount = uint32(TargetCount);
	BuildNeighborsParameters->MaxNeighbors = uint32(MaxNeighborsPerTarget);
	BuildNeighborsParameters->InfluenceRadius = InfluenceRadius;
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("SpaceColonizationQueue.BuildNeighbors"),
		BuildNeighborsParameters,
		ERDGPassFlags::Compute,
		[BuildNeighborsParameters, BuildNeighborsShader, TargetCount](FRHIComputeCommandList& InRHICmdList)
		{
			FComputeShaderUtils::Dispatch(InRHICmdList, BuildNeighborsShader, *BuildNeighborsParameters, FComputeShaderUtils::GetGroupCount(FIntVector(TargetCount, 1, 1), 64));
		});

	TShaderMapRef<FSpaceColonizationQueueResetProposalsCS> ResetProposalsShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FSpaceColonizationQueueClaimCS> ClaimShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FSpaceColonizationQueueProposeCS> ProposeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FSpaceColonizationQueueCommitParentsCS> CommitParentsShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FSpaceColonizationQueueCommitChildrenCS> CommitChildrenShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	for (int32 IterationIndex = 0; IterationIndex < IterationCount; ++IterationIndex)
	{
		FSpaceColonizationQueueResetProposalsCS::FParameters* ResetParameters = GraphBuilder.AllocParameters<FSpaceColonizationQueueResetProposalsCS::FParameters>();
		ResetParameters->RW_ProposalOwners = ProposalOwnersUAV;
		ResetParameters->RW_ProposalPositions = ProposalPositionsUAV;
		ResetParameters->RW_Claims = ClaimsUAV;
		ResetParameters->RW_ClaimCounts = ClaimCountsUAV;
		ResetParameters->RW_ProposalTargets = ProposalTargetsUAV;
		ResetParameters->TargetCount = uint32(TargetCount);
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("SpaceColonizationQueue.ResetProposals"),
			ResetParameters,
			ERDGPassFlags::Compute,
			[ResetParameters, ResetProposalsShader, TargetCount](FRHIComputeCommandList& InRHICmdList)
			{
				FComputeShaderUtils::Dispatch(InRHICmdList, ResetProposalsShader, *ResetParameters, FComputeShaderUtils::GetGroupCount(FIntVector(TargetCount, 1, 1), 64));
			});

		FSpaceColonizationQueueClaimCS::FParameters* ClaimParameters = GraphBuilder.AllocParameters<FSpaceColonizationQueueClaimCS::FParameters>();
		ClaimParameters->TargetPositions = TargetSRV;
		ClaimParameters->State0 = State0SRV;
		ClaimParameters->NeighborCounts = NeighborCountsSRV;
		ClaimParameters->NeighborIndices = NeighborIndicesSRV;
		ClaimParameters->RW_Claims = ClaimsUAV;
		ClaimParameters->RW_ClaimCounts = ClaimCountsUAV;
		ClaimParameters->TargetCount = uint32(TargetCount);
		ClaimParameters->MaxNeighbors = uint32(MaxNeighborsPerTarget);
		ClaimParameters->InfluenceRadius = InfluenceRadius;
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("SpaceColonizationQueue.Claim"),
			ClaimParameters,
			ERDGPassFlags::Compute,
			[ClaimParameters, ClaimShader, TargetCount](FRHIComputeCommandList& InRHICmdList)
			{
				FComputeShaderUtils::Dispatch(InRHICmdList, ClaimShader, *ClaimParameters, FComputeShaderUtils::GetGroupCount(FIntVector(TargetCount, 1, 1), 64));
			});

		FSpaceColonizationQueueProposeCS::FParameters* ProposeParameters = GraphBuilder.AllocParameters<FSpaceColonizationQueueProposeCS::FParameters>();
		ProposeParameters->InitialTargetPositions = InitialTargetSRV;
		ProposeParameters->TargetPositions = TargetSRV;
		ProposeParameters->State0 = State0SRV;
		ProposeParameters->State1 = State1SRV;
		ProposeParameters->NeighborCounts = NeighborCountsSRV;
		ProposeParameters->NeighborIndices = NeighborIndicesSRV;
		ProposeParameters->Claims = ClaimsSRV;
		ProposeParameters->ClaimCounts = ClaimCountsSRV;
		ProposeParameters->RW_ProposalOwners = ProposalOwnersUAV;
		ProposeParameters->RW_ProposalPositions = ProposalPositionsUAV;
		ProposeParameters->RW_ProposalTargets = ProposalTargetsUAV;
		ProposeParameters->TargetCount = uint32(TargetCount);
		ProposeParameters->MaxNeighbors = uint32(MaxNeighborsPerTarget);
		ProposeParameters->Iteration = uint32(IterationIndex);
		// Negative Activetime round-trips through the uint32 cast; the shader
		// recovers it via (int)Activetime, matching the CPU's unclamped gate.
		ProposeParameters->Activetime = uint32(Activetime);
		ProposeParameters->RandGrow = RandGrow;
		ProposeParameters->Seed = Seed;
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("SpaceColonizationQueue.Propose"),
			ProposeParameters,
			ERDGPassFlags::Compute,
			[ProposeParameters, ProposeShader, TargetCount](FRHIComputeCommandList& InRHICmdList)
			{
				FComputeShaderUtils::Dispatch(InRHICmdList, ProposeShader, *ProposeParameters, FComputeShaderUtils::GetGroupCount(FIntVector(TargetCount, 1, 1), 64));
			});

		// Parents commit first so children inherit the post-increment SpawnCount,
		// matching the CPU sequential commit loop.
		FSpaceColonizationQueueCommitParentsCS::FParameters* CommitParentsParameters = GraphBuilder.AllocParameters<FSpaceColonizationQueueCommitParentsCS::FParameters>();
		CommitParentsParameters->ProposalTargets = ProposalTargetsSRV;
		CommitParentsParameters->RW_State0 = State0UAV;
		CommitParentsParameters->RW_State1 = State1UAV;
		CommitParentsParameters->TargetCount = uint32(TargetCount);
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("SpaceColonizationQueue.CommitParents"),
			CommitParentsParameters,
			ERDGPassFlags::Compute,
			[CommitParentsParameters, CommitParentsShader, TargetCount](FRHIComputeCommandList& InRHICmdList)
			{
				FComputeShaderUtils::Dispatch(InRHICmdList, CommitParentsShader, *CommitParentsParameters, FComputeShaderUtils::GetGroupCount(FIntVector(TargetCount, 1, 1), 64));
			});

		FSpaceColonizationQueueCommitChildrenCS::FParameters* CommitChildrenParameters = GraphBuilder.AllocParameters<FSpaceColonizationQueueCommitChildrenCS::FParameters>();
		CommitChildrenParameters->InitialTargetPositions = InitialTargetSRV;
		CommitChildrenParameters->ProposalOwners = ProposalOwnersSRV;
		CommitChildrenParameters->ProposalPositions = ProposalPositionsSRV;
		CommitChildrenParameters->RW_TargetPositions = TargetUAV;
		CommitChildrenParameters->RW_State0 = State0UAV;
		CommitChildrenParameters->RW_State1 = State1UAV;
		CommitChildrenParameters->TargetCount = uint32(TargetCount);
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("SpaceColonizationQueue.CommitChildren"),
			CommitChildrenParameters,
			ERDGPassFlags::Compute,
			[CommitChildrenParameters, CommitChildrenShader, TargetCount](FRHIComputeCommandList& InRHICmdList)
			{
				FComputeShaderUtils::Dispatch(InRHICmdList, CommitChildrenShader, *CommitChildrenParameters, FComputeShaderUtils::GetGroupCount(FIntVector(TargetCount, 1, 1), 64));
			});
	}

	// ---- GPU vine line-building (Increment B, Stage B1: count + validate) ----
	// Mirrors the CPU tracer: BranchOrder -> CountLines (End-gated, anti-web)
	// -> PrefixSum. Emits only a validation counter for now; the CPU tracer
	// still produces the actual lines downstream.
	CREATE_RDG_STRUCTURED_UAV_SRV(BranchOrder, uint32, TargetCount, TEXT("SpaceColonizationQueue_BranchOrder"))
	CREATE_RDG_STRUCTURED_UAV_SRV(LineCounter, uint32, 1, TEXT("SpaceColonizationQueue_LineCounter"))
	CREATE_RDG_STRUCTURED_UAV_SRV(LineLength, uint32, TargetCount, TEXT("SpaceColonizationQueue_LineLength"))
	CREATE_RDG_STRUCTURED_UAV_SRV(NodeLineIndex, uint32, TargetCount, TEXT("SpaceColonizationQueue_NodeLineIndex"))
	CREATE_RDG_STRUCTURED_UAV_SRV(LineOffset, uint32, TargetCount, TEXT("SpaceColonizationQueue_LineOffset"))
	CREATE_RDG_STRUCTURED_UAV_SRV(LineCountsOut, uint32, 4, TEXT("SpaceColonizationQueue_LineCountsOut"))

	AddClearUAVPass(GraphBuilder, LineCounterUAV, 0u);

	TShaderMapRef<FSpaceColonizationBranchOrderCS> BranchOrderShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FSpaceColonizationBranchOrderCS::FParameters* BranchOrderParameters = GraphBuilder.AllocParameters<FSpaceColonizationBranchOrderCS::FParameters>();
	BranchOrderParameters->State0 = State0SRV;
	BranchOrderParameters->State1 = State1SRV;
	BranchOrderParameters->RW_BranchOrder = BranchOrderUAV;
	BranchOrderParameters->TargetCount = uint32(TargetCount);
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("SpaceColonizationQueue.BranchOrder"),
		BranchOrderParameters,
		ERDGPassFlags::Compute,
		[BranchOrderParameters, BranchOrderShader, TargetCount](FRHIComputeCommandList& InRHICmdList)
		{
			FComputeShaderUtils::Dispatch(InRHICmdList, BranchOrderShader, *BranchOrderParameters, FComputeShaderUtils::GetGroupCount(FIntVector(TargetCount, 1, 1), 64));
		});

	TShaderMapRef<FSpaceColonizationCountLinesCS> CountLinesShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FSpaceColonizationCountLinesCS::FParameters* CountLinesParameters = GraphBuilder.AllocParameters<FSpaceColonizationCountLinesCS::FParameters>();
	CountLinesParameters->State0 = State0SRV;
	CountLinesParameters->State1 = State1SRV;
	CountLinesParameters->BranchOrder = BranchOrderSRV;
	CountLinesParameters->RW_LineCounter = LineCounterUAV;
	CountLinesParameters->RW_LineLength = LineLengthUAV;
	CountLinesParameters->RW_NodeLineIndex = NodeLineIndexUAV;
	CountLinesParameters->TargetCount = uint32(TargetCount);
	CountLinesParameters->BackGrowCount = BackGrowCount;
	CountLinesParameters->ForkTaperForkOrdinal = ForkTaperForkOrdinal;
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("SpaceColonizationQueue.CountLines"),
		CountLinesParameters,
		ERDGPassFlags::Compute,
		[CountLinesParameters, CountLinesShader, TargetCount](FRHIComputeCommandList& InRHICmdList)
		{
			FComputeShaderUtils::Dispatch(InRHICmdList, CountLinesShader, *CountLinesParameters, FComputeShaderUtils::GetGroupCount(FIntVector(TargetCount, 1, 1), 64));
		});

	TShaderMapRef<FSpaceColonizationPrefixSumLinesCS> PrefixSumShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FSpaceColonizationPrefixSumLinesCS::FParameters* PrefixSumParameters = GraphBuilder.AllocParameters<FSpaceColonizationPrefixSumLinesCS::FParameters>();
	PrefixSumParameters->LineCounter = LineCounterSRV;
	PrefixSumParameters->LineLength = LineLengthSRV;
	PrefixSumParameters->RW_LineOffset = LineOffsetUAV;
	PrefixSumParameters->RW_LineCountsOut = LineCountsOutUAV;
	PrefixSumParameters->TargetCount = uint32(TargetCount);
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("SpaceColonizationQueue.PrefixSumLines"),
		PrefixSumParameters,
		ERDGPassFlags::Compute,
		[PrefixSumParameters, PrefixSumShader](FRHIComputeCommandList& InRHICmdList)
		{
			FComputeShaderUtils::Dispatch(InRHICmdList, PrefixSumShader, *PrefixSumParameters, FIntVector(1, 1, 1));
		});
	// (LineCounts readback moved to after the resample; see the NewCounts copy below.)

	// ---- Stage B2: emit the flat PathPoints/Meta/Segment layout ----
	CREATE_RDG_STRUCTURED_UPLOAD_SRV(TargetPointScales, float, EmitTargetPointScales, TEXT("SpaceColonizationQueue_TargetPointScales"))
	CREATE_RDG_STRUCTURED_UPLOAD_SRV(StartSourceScales, float, EmitStartSourceScales, TEXT("SpaceColonizationQueue_StartSourceScales"))
	CREATE_RDG_STRUCTURED_UAV_SRV(PathPoints, FVector4f, PathPointCapacity, TEXT("SpaceColonizationQueue_PathPoints"))
	CREATE_RDG_STRUCTURED_UAV_SRV(PathPointMeta, FIntVector4, PathPointCapacity, TEXT("SpaceColonizationQueue_PathPointMeta"))
	CREATE_RDG_STRUCTURED_UAV_SRV(SegmentMeta, FIntVector4, SegmentCapacity, TEXT("SpaceColonizationQueue_SegmentMeta"))

	TShaderMapRef<FSpaceColonizationEmitLinesCS> EmitLinesShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FSpaceColonizationEmitLinesCS::FParameters* EmitLinesParameters = GraphBuilder.AllocParameters<FSpaceColonizationEmitLinesCS::FParameters>();
	EmitLinesParameters->TargetPositions = TargetSRV;
	EmitLinesParameters->State0 = State0SRV;
	EmitLinesParameters->State1 = State1SRV;
	EmitLinesParameters->BranchOrder = BranchOrderSRV;
	EmitLinesParameters->TargetPointScales = TargetPointScalesSRV;
	EmitLinesParameters->StartSourceScales = StartSourceScalesSRV;
	EmitLinesParameters->NodeLineIndex = NodeLineIndexSRV;
	EmitLinesParameters->LineOffset = LineOffsetSRV;
	EmitLinesParameters->RW_PathPoints = PathPointsUAV;
	EmitLinesParameters->RW_PathPointMeta = PathPointMetaUAV;
	EmitLinesParameters->RW_SegmentMeta = SegmentMetaUAV;
	EmitLinesParameters->TargetCount = uint32(TargetCount);
	EmitLinesParameters->BackGrowCount = BackGrowCount;
	EmitLinesParameters->ForkTaperForkOrdinal = ForkTaperForkOrdinal;
	EmitLinesParameters->PathPointCapacity = PathPointCapacity;
	EmitLinesParameters->SegmentCapacity = SegmentCapacity;
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("SpaceColonizationQueue.EmitLines"),
		EmitLinesParameters,
		ERDGPassFlags::Compute,
		[EmitLinesParameters, EmitLinesShader, TargetCount](FRHIComputeCommandList& InRHICmdList)
		{
			FComputeShaderUtils::Dispatch(InRHICmdList, EmitLinesShader, *EmitLinesParameters, FComputeShaderUtils::GetGroupCount(FIntVector(TargetCount, 1, 1), 64));
		});

	// ---- Stage B3 (prep port): scatter (ApplyVVSCPointOffset) BEFORE smooth ----
	// Jitters the raw emitted points +/-ScatterDistance in place so the arc-length
	// change flows into resample exactly like the CPU path. Skipped when disabled.
	if (ScatterDistance > 0.0f)
	{
		TShaderMapRef<FSpaceColonizationScatterCS> ScatterShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FSpaceColonizationScatterCS::FParameters* ScatterParameters = GraphBuilder.AllocParameters<FSpaceColonizationScatterCS::FParameters>();
		ScatterParameters->RW_ScatterPoints = PathPointsUAV;
		ScatterParameters->ScatterCounts = LineCountsOutSRV;
		ScatterParameters->ScatterSource = SourceSRV;
		ScatterParameters->ScatterDistance = ScatterDistance;
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("SpaceColonizationQueue.Scatter"),
			ScatterParameters,
			ERDGPassFlags::Compute,
			[ScatterParameters, ScatterShader, PathPointCapacity](FRHIComputeCommandList& InRHICmdList)
			{
				FComputeShaderUtils::Dispatch(InRHICmdList, ScatterShader, *ScatterParameters, FComputeShaderUtils::GetGroupCount(FIntVector(int32(PathPointCapacity), 1, 1), 64));
			});
	}

	// ---- Stage B3 (prep port): pre-projection SmoothLine(3) as 3 ping-pong Jacobi passes ----
	CREATE_RDG_STRUCTURED_UAV_SRV(SmoothA, FVector4f, PathPointCapacity, TEXT("SpaceColonizationQueue_SmoothA"))
	CREATE_RDG_STRUCTURED_UAV_SRV(SmoothB, FVector4f, PathPointCapacity, TEXT("SpaceColonizationQueue_SmoothB"))
	TShaderMapRef<FSpaceColonizationSmoothLinesCS> SmoothShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	auto AddSmoothPass = [&](FRDGBufferSRVRef InPoints, FRDGBufferUAVRef OutPoints)
	{
		FSpaceColonizationSmoothLinesCS::FParameters* SmoothParameters = GraphBuilder.AllocParameters<FSpaceColonizationSmoothLinesCS::FParameters>();
		SmoothParameters->SmoothInPoints = InPoints;
		SmoothParameters->SmoothMeta = PathPointMetaSRV;
		SmoothParameters->SmoothCounts = LineCountsOutSRV;
		SmoothParameters->RW_SmoothOutPoints = OutPoints;
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("SpaceColonizationQueue.SmoothLines"),
			SmoothParameters,
			ERDGPassFlags::Compute,
			[SmoothParameters, SmoothShader, PathPointCapacity](FRHIComputeCommandList& InRHICmdList)
			{
				FComputeShaderUtils::Dispatch(InRHICmdList, SmoothShader, *SmoothParameters, FComputeShaderUtils::GetGroupCount(FIntVector(int32(PathPointCapacity), 1, 1), 64));
			});
	};
	AddSmoothPass(PathPointsSRV, SmoothAUAV); // iter 1: raw emit -> A
	AddSmoothPass(SmoothASRV, SmoothBUAV);    // iter 2: A -> B
	AddSmoothPass(SmoothBSRV, SmoothAUAV);    // iter 3: B -> A (result)

	// ---- Stage B3 (prep port): pre-projection ResamppleByLength (count-changing) ----
	CREATE_RDG_STRUCTURED_UAV_SRV(NewLineLength, uint32, TargetCount, TEXT("SpaceColonizationQueue_NewLineLength"))
	CREATE_RDG_STRUCTURED_UAV_SRV(NewSegLength, uint32, TargetCount, TEXT("SpaceColonizationQueue_NewSegLength"))
	CREATE_RDG_STRUCTURED_UAV_SRV(NewLineOffset, uint32, TargetCount, TEXT("SpaceColonizationQueue_NewLineOffset"))
	CREATE_RDG_STRUCTURED_UAV_SRV(NewSegOffset, uint32, TargetCount, TEXT("SpaceColonizationQueue_NewSegOffset"))
	CREATE_RDG_STRUCTURED_UAV_SRV(NewCounts, uint32, 4, TEXT("SpaceColonizationQueue_NewCounts"))
	CREATE_RDG_STRUCTURED_UAV_SRV(PathPoints2, FVector4f, PathPoint2Capacity, TEXT("SpaceColonizationQueue_PathPoints2"))
	CREATE_RDG_STRUCTURED_UAV_SRV(PathPointMeta2, FIntVector4, PathPoint2Capacity, TEXT("SpaceColonizationQueue_PathPointMeta2"))
	CREATE_RDG_STRUCTURED_UAV_SRV(SegmentMeta2, FIntVector4, Segment2Capacity, TEXT("SpaceColonizationQueue_SegmentMeta2"))
	{
		TShaderMapRef<FSpaceColonizationCountResampleCS> CountResampleShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FSpaceColonizationCountResampleCS::FParameters* P = GraphBuilder.AllocParameters<FSpaceColonizationCountResampleCS::FParameters>();
		P->ResampleInPoints = SmoothASRV;
		P->ResampleLineOffset = LineOffsetSRV;
		P->ResampleLineLength = LineLengthSRV;
		P->ResampleLineCount = LineCountsOutSRV;
		P->RW_NewLineLength = NewLineLengthUAV;
		P->RW_NewSegLength = NewSegLengthUAV;
		P->ResampleLength = ResampleLength;
		GraphBuilder.AddPass(RDG_EVENT_NAME("SpaceColonizationQueue.CountResample"), P, ERDGPassFlags::Compute,
			[P, CountResampleShader, TargetCount](FRHIComputeCommandList& InRHICmdList)
			{ FComputeShaderUtils::Dispatch(InRHICmdList, CountResampleShader, *P, FComputeShaderUtils::GetGroupCount(FIntVector(TargetCount, 1, 1), 64)); });
	}
	{
		TShaderMapRef<FSpaceColonizationPrefixResampleCS> PrefixResampleShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FSpaceColonizationPrefixResampleCS::FParameters* P = GraphBuilder.AllocParameters<FSpaceColonizationPrefixResampleCS::FParameters>();
		P->ResampleLineCount = LineCountsOutSRV;
		P->NewLineLength = NewLineLengthSRV;
		P->NewSegLength = NewSegLengthSRV;
		P->RW_NewLineOffset = NewLineOffsetUAV;
		P->RW_NewSegOffset = NewSegOffsetUAV;
		P->RW_NewCounts = NewCountsUAV;
		P->TargetCount = uint32(TargetCount);
		GraphBuilder.AddPass(RDG_EVENT_NAME("SpaceColonizationQueue.PrefixResample"), P, ERDGPassFlags::Compute,
			[P, PrefixResampleShader](FRHIComputeCommandList& InRHICmdList)
			{ FComputeShaderUtils::Dispatch(InRHICmdList, PrefixResampleShader, *P, FIntVector(1, 1, 1)); });
	}
	{
		TShaderMapRef<FSpaceColonizationEmitResampleCS> EmitResampleShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FSpaceColonizationEmitResampleCS::FParameters* P = GraphBuilder.AllocParameters<FSpaceColonizationEmitResampleCS::FParameters>();
		P->ResampleInPoints = SmoothASRV;
		P->ResampleLineOffset = LineOffsetSRV;
		P->ResampleLineLength = LineLengthSRV;
		P->ResampleLineCount = LineCountsOutSRV;
		P->NewLineOffset = NewLineOffsetSRV;
		P->NewSegOffset = NewSegOffsetSRV;
		P->RW_PathPoints2 = PathPoints2UAV;
		P->RW_PathPointMeta2 = PathPointMeta2UAV;
		P->RW_SegmentMeta2 = SegmentMeta2UAV;
		P->ResampleLength = ResampleLength;
		P->PathPoint2Capacity = PathPoint2Capacity;
		P->Segment2Capacity = Segment2Capacity;
		GraphBuilder.AddPass(RDG_EVENT_NAME("SpaceColonizationQueue.EmitResample"), P, ERDGPassFlags::Compute,
			[P, EmitResampleShader, TargetCount](FRHIComputeCommandList& InRHICmdList)
			{ FComputeShaderUtils::Dispatch(InRHICmdList, EmitResampleShader, *P, FComputeShaderUtils::GetGroupCount(FIntVector(TargetCount, 1, 1), 64)); });
	}
	// ---- Stage B3 (prep port): CurveScale (final tube thickness written to .w) ----
	CREATE_RDG_STRUCTURED_UPLOAD_SRV(CurveLUT, float, EmitCurveLUT, TEXT("SpaceColonizationQueue_CurveLUT"))
	{
		TShaderMapRef<FSpaceColonizationCurveCS> CurveShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FSpaceColonizationCurveCS::FParameters* P = GraphBuilder.AllocParameters<FSpaceColonizationCurveCS::FParameters>();
		P->CurveLUT = CurveLUTSRV;
		P->CurveLineOffset = NewLineOffsetSRV;
		P->CurveLineLength = NewLineLengthSRV;
		P->CurveLineCount = NewCountsSRV;
		P->RW_CurvePoints = PathPoints2UAV;
		P->CurveLUTSize = CurveLUTSize;
		GraphBuilder.AddPass(RDG_EVENT_NAME("SpaceColonizationQueue.CurveScale"), P, ERDGPassFlags::Compute,
			[P, CurveShader, TargetCount](FRHIComputeCommandList& InRHICmdList)
			{ FComputeShaderUtils::Dispatch(InRHICmdList, CurveShader, *P, FComputeShaderUtils::GetGroupCount(FIntVector(TargetCount, 1, 1), 64)); });
	}
	Out.PathPoints = PathPoints2Buffer;
	Out.PathPointMeta = PathPointMeta2Buffer;
	Out.SegmentMeta = SegmentMeta2Buffer;
	Out.Counts = NewCountsBuffer;
	return true;
}

} // anonymous namespace

// Records the fused space-colonization + concat pipeline: one solve per source, a single-thread
// prefix sum over their GPU-only counts, then a per-source relocate into one contiguous batch.
// Every intermediate is graph-lifetime and no count ever reaches the CPU.
static bool AddVineFusedSCConcatPasses(FRDGBuilder& GraphBuilder, const FVineFusedSCInputs& SC, FVineFusedSCOutputs& Out)
{
	if (!SC.IsValid()) return false;

	struct FSolvedSource
	{
		FVineSCPassOutputs Buffers;
		uint32 PointCapacity = 0u; // also the segment capacity; sizes this source's copy dispatches
	};
	TArray<FSolvedSource> Solved;
	Solved.Reserve(SC.Sources.Num());
	for (const FVineSCPreparedSource& Source : SC.Sources)
	{
		FVineSCPassOutputs SolvedBuffers;
		if (!AddVineSCPasses(GraphBuilder, SC, Source, SolvedBuffers)) continue;
		FSolvedSource& Entry = Solved.AddDefaulted_GetRef();
		Entry.Buffers = SolvedBuffers;
		Entry.PointCapacity = FMath::Max(1u, Source.PointCapacity);
	}
	if (Solved.Num() == 0) return false;

	// Gather each source's four-uint count block into one buffer so a single thread can walk them.
	const uint32 SourceSlotCount = uint32(Solved.Num()) * 4u;
	FRDGBufferRef SourceCountsBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), SourceSlotCount), TEXT("VineConcat.SourceCounts"));
	for (int32 SourceIndex = 0; SourceIndex < Solved.Num(); ++SourceIndex)
	{
		AddCopyBufferPass(GraphBuilder, SourceCountsBuffer, uint64(SourceIndex) * 4u * sizeof(uint32),
			Solved[SourceIndex].Buffers.Counts, 0, 4u * sizeof(uint32));
	}

	FRDGBufferRef ConcatBasesBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), SourceSlotCount), TEXT("VineConcat.Bases"));
	FRDGBufferRef ConcatTotalsBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 4u), TEXT("VineConcat.Totals"));
	{
		FConcatPrefixSumCS::FParameters* P = GraphBuilder.AllocParameters<FConcatPrefixSumCS::FParameters>();
		P->ConcatSourceCounts = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SourceCountsBuffer));
		P->RW_ConcatBases = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(ConcatBasesBuffer));
		P->RW_ConcatTotals = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(ConcatTotalsBuffer));
		P->ConcatSourceCount = uint32(Solved.Num());
		P->ConcatPointCapacity = SC.TotalPointCapacity;
		P->ConcatSegmentCapacity = SC.TotalSegmentCapacity;
		TShaderMapRef<FConcatPrefixSumCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VineConcat.PrefixSum"), Shader, P, FIntVector(1, 1, 1));
	}

	CSHelper::FRDGStructuredBufferRefs DstPoints = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), SC.TotalPointCapacity, TEXT("VineConcat.PathPoints"), true, true);
	CSHelper::FRDGStructuredBufferRefs DstMeta = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(FIntVector4), SC.TotalPointCapacity, TEXT("VineConcat.PathPointMeta"), true, true);
	CSHelper::FRDGStructuredBufferRefs DstSeg = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(FIntVector4), SC.TotalSegmentCapacity, TEXT("VineConcat.SegmentMeta"), true, true);
	if (!DstPoints.UAV || !DstMeta.UAV || !DstSeg.UAV) return false;

	// The batch is allocated for the worst case, so everything past the compact counts is slack.
	// Zero it: PathPointMeta's Count field then reads 0 and SegmentMeta reads (0,0) for any thread
	// the group-size round-up (or the capacity-sized debug line pass) lets touch a slack slot,
	// instead of whatever the transient pool last left there.
	AddClearUAVPass(GraphBuilder, DstPoints.UAV, 0u);
	AddClearUAVPass(GraphBuilder, DstMeta.UAV, 0u);
	AddClearUAVPass(GraphBuilder, DstSeg.UAV, 0u);

	FRDGBufferSRVRef BasesSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ConcatBasesBuffer));
	TShaderMapRef<FConcatCopyFloat4CS> CopyShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FConcatOffsetInt4CS> OffsetShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	for (int32 SourceIndex = 0; SourceIndex < Solved.Num(); ++SourceIndex)
	{
		const FSolvedSource& Source = Solved[SourceIndex];
		// Sized to the source's whole slice — its real count is GPU-only, and the kernels early-out
		// on it. The slice is MaxVinePointCount/sources wide and the kernels are a load plus a
		// store, so the wasted threads cost nothing measurable.
		const FIntVector Groups = FComputeShaderUtils::GetGroupCount(FIntVector(int32(Source.PointCapacity), 1, 1), int32(VineMeshGroupSize));

		{
			FConcatCopyFloat4CS::FParameters* P = GraphBuilder.AllocParameters<FConcatCopyFloat4CS::FParameters>();
			P->ConcatSrcFloat4 = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Source.Buffers.PathPoints));
			P->ConcatBases = BasesSRV;
			P->RW_ConcatDstFloat4 = DstPoints.UAV;
			P->ConcatSourceIndex = uint32(SourceIndex);
			P->ConcatRecordKind = 0u;
			P->ConcatCapacity = SC.TotalPointCapacity;
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VineConcat.Points"), CopyShader, P, Groups);
		}
		{
			// PathPointMeta int4(Prev, Next, Base, Count): the first three are point indices.
			FConcatOffsetInt4CS::FParameters* P = GraphBuilder.AllocParameters<FConcatOffsetInt4CS::FParameters>();
			P->ConcatSrcInt4 = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Source.Buffers.PathPointMeta));
			P->ConcatBases = BasesSRV;
			P->RW_ConcatDstInt4 = DstMeta.UAV;
			P->ConcatSourceIndex = uint32(SourceIndex);
			P->ConcatRecordKind = 0u;
			P->ConcatCapacity = SC.TotalPointCapacity;
			P->ConcatOffsetMask = FIntVector4(1, 1, 1, 0);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VineConcat.Meta"), OffsetShader, P, Groups);
		}
		{
			// SegmentMeta int4(A, B, 0, 0): both endpoints are point indices.
			FConcatOffsetInt4CS::FParameters* P = GraphBuilder.AllocParameters<FConcatOffsetInt4CS::FParameters>();
			P->ConcatSrcInt4 = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Source.Buffers.SegmentMeta));
			P->ConcatBases = BasesSRV;
			P->RW_ConcatDstInt4 = DstSeg.UAV;
			P->ConcatSourceIndex = uint32(SourceIndex);
			P->ConcatRecordKind = 1u;
			P->ConcatCapacity = SC.TotalSegmentCapacity;
			P->ConcatOffsetMask = FIntVector4(1, 1, 0, 0);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VineConcat.Seg"), OffsetShader, P, Groups);
		}
	}

	Out.PathPoints = DstPoints.Buffer;
	Out.PathPointMeta = DstMeta.Buffer;
	Out.SegmentMeta = DstSeg.Buffer;
	Out.Counts = ConcatTotalsBuffer;
	return true;
}

// ---- SpaceColonization member functions (moved from UGenerateVines, params from SC) ----

// Trip A: GPU port of ApplyVVSCPointOffset. Point jitter distance (cm) applied before the
// prep smooth on the GPU SC path, for visual parity with the CPU path. 0 disables scatter.
static TAutoConsoleVariable<float> CVarVineSCScatter(
	TEXT("r.Vine.SC.Scatter"),
	10.0f,
	TEXT("Vine GPU SC point-scatter distance in cm (ApplyVVSCPointOffset parity). 0 = off."),
	ECVF_Default);

TArray<FSpaceColonizationLineResult> AVineContainer::SpaceColonizationWithScales(TArray<FTransform> /*SourceTransforms*/, TArray<FTransform> /*TargetTransforms*/, bool /*bUseComputeShader*/)
{
	// The solve now lives inside the vine mesh RDG graph (FVineMeshSceneProxy::BuildGeometry) and
	// its output never leaves the GPU, so there is nothing to return here and nothing worth
	// dispatching for a caller that only wants CPU lines. Kept so existing Blueprints still bind;
	// use GenerateVines / VisVine instead.
	return {};
}

// Prepares the CPU side of the fused space-colonization solve: source/target positions, the
// per-source scale lookups, the baked taper LUT and each source's slice of the batch-wide point
// cap. No GPU work is dispatched — the passes are recorded later, into the leaf's own graph.
bool AVineContainer::PrepareVineFusedSCInputs(const TArray<FTransform>& SourceTransforms,
	const TArray<FTransform>& TargetTransforms, FVineFusedSCInputs& OutInputs)
{
	GV_TIME_SCOPE(TEXT("SpaceColonization.PrepareInputs"));
	OutInputs = FVineFusedSCInputs();
	const int32 SourceCount = SourceTransforms.Num();
	const int32 TargetCount = TargetTransforms.Num();
	if (SourceCount == 0 || TargetCount == 0) return false;

	OutInputs.InitialTargetPositions.Reserve(TargetCount);
	OutInputs.TargetPointScales.Reserve(TargetCount);
	for (const FTransform& Transform : TargetTransforms)
	{
		const float TargetScale = GetSpaceColonizationTransformScale(Transform);
		OutInputs.InitialTargetPositions.Add(FVector4f((FVector3f)Transform.GetLocation(), TargetScale));
		OutInputs.TargetPointScales.Add(TargetScale);
	}

	// Ensure the profile curve exists BEFORE baking the LUT. On a first generate (CurveControl
	// still null) the LUT would otherwise bake flat (EvaluateVineScale(null)=1.0) and the mesh
	// would lose all thickness variation until the next generate rebuilt it.
	if (VV.CurveControl == nullptr) VV.CurveControl = NewObject<UCurveLinearColor>(this);
	// Bake CurveControl.G into a LUT so the GPU CurveScale matches EvaluateVineScale.
	{
		const int32 LUTSize = 256;
		OutInputs.CurveLUT.SetNumUninitialized(LUTSize);
		for (int32 LUTIndex = 0; LUTIndex < LUTSize; ++LUTIndex)
		{
			OutInputs.CurveLUT[LUTIndex] = EvaluateVineScale(VV.CurveControl, LUTIndex, LUTSize);
		}
	}

	OutInputs.Iteration = SC.Iteration;
	OutInputs.Activetime = SC.Activetime;
	OutInputs.RandGrow = SC.RandGrow;
	OutInputs.Seed = SC.Seed;
	OutInputs.InfluenceRadius = SC.InfluenceRadius;
	OutInputs.BackGrowCount = SC.BackGrowCount;
	OutInputs.ForkTaperForkOrdinal = SC.ForkTaperForkOrdinal;
	OutInputs.ResampleLength = FMath::Max(VV.ResampleLength, 0.01f);
	OutInputs.ScatterDistance = FMath::Max(CVarVineSCScatter.GetValueOnGameThread(), 0.0f);

	// The point cap is batch-wide; every source that will be concatenated gets an equal share.
	// Take the tighter of that share and the theoretical bound (each of <= TargetCount lines has
	// <= SC_MAX_BACKTRACK+1 points), so a small target set still allocates small.
	constexpr int32 SpaceColonizationMaxBacktrack = 100;
	const uint32 PerSourceShare = uint32(FMath::Max(1, VV.MaxVinePointCount / SourceCount));
	const uint32 TheoreticalPointBound = uint32(FMath::Min<int64>(int64(TargetCount) * int64(SpaceColonizationMaxBacktrack + 1), 4000000));
	const uint32 PerSourceCapacity = FMath::Max(1u, FMath::Min(TheoreticalPointBound, PerSourceShare));
	// Clamping here is not free: EmitSpaceColonizationLinesCS breaks out of its backtrack once it
	// reaches PathPointCapacity, so lines are silently cut short and the vine just looks sparser /
	// shorter with no other symptom. That must never be a Verbose-only message — at Verbose it is
	// invisible by default and reads as "the generator changed behaviour on its own".
	if (PerSourceCapacity < TheoreticalPointBound)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[VineSC] Vine points truncated: per-source capacity %u < theoretical bound %u (%d sources sharing MaxVinePointCount=%d). ")
			TEXT("Lines will be cut short. Raise MaxVinePointCount to at least %lld to restore the full solve."),
			PerSourceCapacity, TheoreticalPointBound, SourceCount, VV.MaxVinePointCount,
			int64(TheoreticalPointBound) * int64(SourceCount));
	}

	OutInputs.Sources.Reserve(SourceCount);
	for (const FTransform& SourceTransform : SourceTransforms)
	{
		// The scale lookups are per source: StartSourceScales marks only the target nearest THIS
		// source, so each solve gets its own array (TargetPointScales is shared and already built).
		TArray<FTransform> SingleSource;
		SingleSource.Add(SourceTransform);
		TArray<float> UnusedTargetPointScales;
		FVineSCPreparedSource& Prepared = OutInputs.Sources.AddDefaulted_GetRef();
		BuildSpaceColonizationScaleLookups(SingleSource, TargetTransforms, UnusedTargetPointScales, Prepared.StartSourceScales);
		Prepared.SourcePositions.Add(FVector4f((FVector3f)SourceTransform.GetLocation(), GetSpaceColonizationTransformScale(SourceTransform)));
		Prepared.PointCapacity = PerSourceCapacity;
	}

	OutInputs.TotalPointCapacity = PerSourceCapacity * uint32(SourceCount);
	OutInputs.TotalSegmentCapacity = OutInputs.TotalPointCapacity;
	return true;
}
