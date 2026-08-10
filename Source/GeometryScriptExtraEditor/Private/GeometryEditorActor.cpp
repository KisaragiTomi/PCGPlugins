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
#include "Components/StaticMeshComponent.h"
#include "Components/SplineComponent.h"
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
#include "CSGpuMeshSave.h"
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
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>,  RWIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>,  RWMeshCounters)
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

// ============================================================================
// Base-stream axial-V (mesh UV.y) GPU passes — port of the CPU
// RecomputeVineOutputUVsFromGeneratedLength, dispatched only on the base-stream
// path (see AddVineMeshPasses). Four kernels over scratch structured buffers.
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

class FVineUVScanCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVineUVScanCS);
	SHADER_USE_PARAMETER_STRUCT(FVineUVScanCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, VineUV_SegLen)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, VineUV_RingCirc)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, RW_VineUV_CurveV)
		SHADER_PARAMETER(uint32, VineUV_PointCount)
		SHADER_PARAMETER(float, VineUV_LengthScale)
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

IMPLEMENT_GLOBAL_SHADER(FVineUVScanCS, "/Plugin/PCGPlugins/Shaders/Private/VVVoxel.usf", "VineUVScanCS", SF_Compute);

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

// Trip A concat: offset one source's int4 meta/segment records by a point base into
// the concatenated destination. Serves both PathPointMeta and SegmentMeta.
class FConcatOffsetInt4CS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FConcatOffsetInt4CS);
	SHADER_USE_PARAMETER_STRUCT(FConcatOffsetInt4CS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, ConcatSrcInt4)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int4>, RW_ConcatDstInt4)
		SHADER_PARAMETER(uint32, ConcatCount)
		SHADER_PARAMETER(uint32, ConcatDstBase)
		SHADER_PARAMETER(int32, ConcatOffsetValue)
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
IMPLEMENT_GLOBAL_SHADER(FConcatOffsetInt4CS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "ConcatOffsetInt4CS", SF_Compute);


// Trip A: the SC solve's fully-prepped output kept GPU-resident (pooled), so the
// VisVine voxel path can consume it directly instead of reading it back to CPU,
// re-tracing the lines, and re-deriving path points a third time on the CPU.
// Buffer layout matches BuildVVGPUInput's output exactly:
//   PathPoints      float4(xyz, finalScale = CurveScale * pointScale)
//   PathPointMeta   int4(Prev, Next, Base, Count)
//   SegmentMeta     int4(A, B, 0, 0)
// PointCount/SegmentCount/LineCount are the post-resample compact counts
// (NewCounts[1]/[2]/[0]); the pooled buffers are over-allocated to the worst-case
// capacity, so only [0, PointCount)/[0, SegmentCount) hold valid data.
// Defined at global scope (matches the header forward declaration; must not land
// inside the anonymous namespace below or the member-function signatures won't match).
struct FVineSCGPUBuffers
{
	TRefCountPtr<FRDGPooledBuffer> PathPoints;
	TRefCountPtr<FRDGPooledBuffer> PathPointMeta;
	TRefCountPtr<FRDGPooledBuffer> SegmentMeta;
	int32 PointCount = 0;
	int32 SegmentCount = 0;
	int32 LineCount = 0;
	bool IsValid() const { return PathPoints.IsValid() && PointCount > 0 && SegmentCount > 0; }
};

// Target-position spatial acceleration buffers for the vine surface projection. Global scope
// (matches FVineSCGPUBuffers) so the self-owning FVineBuildInput bundle below can embed it and
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

// Self-owning CPU-prep bundle shared by the legacy readback dispatch (DispatchVVGPU_Voxel) and
// the GPU-resident leaf (UVineMeshComponent / FVineMeshSceneProxy). It OWNS every CPU array that
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

	// GPU-resident inputs (pooled refs kept alive by the bundle).
	bool bUseGPULines = false;
	TRefCountPtr<FRDGPooledBuffer> GPULinePoints;
	TRefCountPtr<FRDGPooledBuffer> GPULineMeta;
	TRefCountPtr<FRDGPooledBuffer> GPULineSeg;
	bool bUseGPUVoxels = false;
	TRefCountPtr<FRDGPooledBuffer> GPUVoxCells;
	TRefCountPtr<FRDGPooledBuffer> GPUVoxNormals;
	TRefCountPtr<FRDGPooledBuffer> GPUVoxTargets;
	TRefCountPtr<FRDGPooledBuffer> GPUVoxCounter;
	uint32 GPUVoxCount = 0u;
	uint32 GpuVoxelHashSlotCountPow2 = 0u;

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

	// False when the inputs cannot produce a mesh (zero counts / no voxels); mirrors the early
	// returns of DispatchVVGPU_Voxel.
	bool bValid = false;
};

namespace
{
static const FName VineGeneratedStaticMeshActorTag(TEXT("VineGeneratedStaticMeshActor"));

static bool IsVineGeneratedStaticMeshActor(const AActor* Actor)
{
	return Actor && Actor->Tags.Contains(VineGeneratedStaticMeshActorTag);
}

static void TransformDynamicMeshToLocalSpace(UDynamicMesh* Mesh, const FTransform& LocalToWorld)
{
	if (!Mesh || LocalToWorld.Equals(FTransform::Identity))
	{
		return;
	}

	Mesh->EditMesh([&](FDynamicMesh3& EditMesh)
	{
		MeshTransforms::ApplyTransformInverse(EditMesh, FTransformSRT3d(LocalToWorld), true);
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
}

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

static void LogVineSCStageTargetTransformMatch(
	const TCHAR* Label,
	const TArray<FGeometryScriptPolyPath>& Lines,
	const TArray<FTransform>& TargetTransforms)
{
	TArray<FVector> TargetLocations;
	TargetLocations.Reserve(TargetTransforms.Num());
	for (const FTransform& TargetTransform : TargetTransforms)
	{
		TargetLocations.Add(TargetTransform.GetLocation());
	}

	constexpr double MatchTolerance = 0.5;
	int32 PointCount = 0;
	int32 MatchCount = 0;
	double TotalNearestDistance = 0.0;
	double MaxNearestDistance = 0.0;
	FString Samples;

	for (const FGeometryScriptPolyPath& Line : Lines)
	{
		if (!Line.Path.IsValid())
		{
			continue;
		}

		for (const FVector& Point : *Line.Path)
		{
			double NearestDistance = TNumericLimits<double>::Max();
			int32 NearestTargetIndex = INDEX_NONE;
			for (int32 TargetIndex = 0; TargetIndex < TargetLocations.Num(); ++TargetIndex)
			{
				const double Distance = FVector::Dist(Point, TargetLocations[TargetIndex]);
				if (Distance < NearestDistance)
				{
					NearestDistance = Distance;
					NearestTargetIndex = TargetIndex;
				}
			}

			if (NearestDistance <= MatchTolerance)
			{
				++MatchCount;
			}
			if (NearestDistance < TNumericLimits<double>::Max())
			{
				TotalNearestDistance += NearestDistance;
				MaxNearestDistance = FMath::Max(MaxNearestDistance, NearestDistance);
			}

			if (PointCount < 6)
			{
				if (!Samples.IsEmpty())
				{
					Samples += TEXT(" | ");
				}
				Samples += FString::Printf(
					TEXT("#%d Point=(%.2f, %.2f, %.2f) NearestTarget=%d Dist=%.4f"),
					PointCount,
					Point.X,
					Point.Y,
					Point.Z,
					NearestTargetIndex,
					NearestDistance);
			}
			++PointCount;
		}
	}

	const bool bAllPointsMatchTargetTransforms = PointCount > 0 && MatchCount == PointCount;
	const double AverageNearestDistance = PointCount > 0 ? TotalNearestDistance / double(PointCount) : 0.0;
	UE_LOG(LogTemp, Display,
		TEXT("[VineSCStageTargetTransformCheck][%s] Lines=%d Targets=%d Points=%d Matches=%d AllPointsMatchTargetTransforms=%s AvgNearestDist=%.4f MaxNearestDist=%.4f Samples=%s"),
		Label,
		Lines.Num(),
		TargetTransforms.Num(),
		PointCount,
		MatchCount,
		bAllPointsMatchTargetTransforms ? TEXT("true") : TEXT("false"),
		AverageNearestDistance,
		MaxNearestDistance,
		Samples.IsEmpty() ? TEXT("none") : *Samples);
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

// Trip A: merge the per-source GPU SC outputs into one contiguous VisVine batch on
// the GPU (no CPU round-trip). Point positions (.w = final scale) and CurveU are
// copied verbatim into each source's destination slice; PathPointMeta (Prev/Next/
// Base) and SegmentMeta (A/B) point indices are offset by the source's destination
// point base so they address the concatenated array. Produces one pooled buffer set
// equivalent to BuildVVGPUInput's output over all sources' lines.
static bool ConcatenateVineSCGPUBuffers(const TArray<TSharedPtr<FVineSCGPUBuffers>>& Sources, FVineSCGPUBuffers& OutConcat)
{
	OutConcat = FVineSCGPUBuffers();

	TArray<TRefCountPtr<FRDGPooledBuffer>> SrcPoints, SrcMeta, SrcSeg;
	TArray<uint32> SrcPointCounts, SrcSegCounts, DstPointBases, DstSegBases;
	uint32 TotalPoints = 0;
	uint32 TotalSegments = 0;
	for (const TSharedPtr<FVineSCGPUBuffers>& S : Sources)
	{
		if (!S.IsValid() || !S->IsValid()) continue;
		SrcPoints.Add(S->PathPoints);
		SrcMeta.Add(S->PathPointMeta);
		SrcSeg.Add(S->SegmentMeta);
		SrcPointCounts.Add(uint32(S->PointCount));
		SrcSegCounts.Add(uint32(S->SegmentCount));
		DstPointBases.Add(TotalPoints);
		DstSegBases.Add(TotalSegments);
		TotalPoints += uint32(S->PointCount);
		TotalSegments += uint32(S->SegmentCount);
	}
	if (TotalPoints == 0 || TotalSegments == 0)
	{
		return false;
	}

	const int32 NumSrc = SrcPoints.Num();

	TRefCountPtr<FRDGPooledBuffer> OutPathPoints;
	TRefCountPtr<FRDGPooledBuffer> OutMeta;
	TRefCountPtr<FRDGPooledBuffer> OutSeg;
	bool bConcatOK = false;

	ENQUEUE_RENDER_COMMAND(ConcatVineSCBuffers)(
		[SrcPoints = MoveTemp(SrcPoints), SrcMeta = MoveTemp(SrcMeta), SrcSeg = MoveTemp(SrcSeg),
		 SrcPointCounts = MoveTemp(SrcPointCounts), SrcSegCounts = MoveTemp(SrcSegCounts),
		 DstPointBases = MoveTemp(DstPointBases), DstSegBases = MoveTemp(DstSegBases),
		 NumSrc, TotalPoints, TotalSegments,
		 &OutPathPoints, &OutMeta, &OutSeg, &bConcatOK](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			CSHelper::FRDGStructuredBufferRefs DstPoints = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), TotalPoints, TEXT("VineConcat.PathPoints"), true, true);
			CSHelper::FRDGStructuredBufferRefs DstMeta = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(FIntVector4), TotalPoints, TEXT("VineConcat.PathPointMeta"), true, true);
			CSHelper::FRDGStructuredBufferRefs DstSeg = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(FIntVector4), TotalSegments, TEXT("VineConcat.SegmentMeta"), true, true);

			TShaderMapRef<FConcatOffsetInt4CS> ConcatShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

			for (int32 s = 0; s < NumSrc; ++s)
			{
				const uint32 PtCount = SrcPointCounts[s];
				const uint32 SgCount = SrcSegCounts[s];
				const uint32 DstPtBase = DstPointBases[s];
				const uint32 DstSgBase = DstSegBases[s];
				if (PtCount == 0u)
				{
					continue;
				}

				FRDGBufferRef SrcPtBuf = GraphBuilder.RegisterExternalBuffer(SrcPoints[s], TEXT("VineConcat.SrcPathPoints"));
				FRDGBufferRef SrcMtBuf = GraphBuilder.RegisterExternalBuffer(SrcMeta[s], TEXT("VineConcat.SrcMeta"));

				// Positions: 16-byte float4, verbatim byte copy works.
				AddCopyBufferPass(GraphBuilder, DstPoints.Buffer, uint64(DstPtBase) * sizeof(FVector4f), SrcPtBuf, 0, uint64(PtCount) * sizeof(FVector4f));
				// CurveU is uploaded whole (float4) above from the aggregated CPU array; no per-source copy.

				// Meta: offset Prev/Next/Base by the point base, keep Count.
				{
					FConcatOffsetInt4CS::FParameters* P = GraphBuilder.AllocParameters<FConcatOffsetInt4CS::FParameters>();
					P->ConcatSrcInt4 = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SrcMtBuf));
					P->RW_ConcatDstInt4 = DstMeta.UAV;
					P->ConcatCount = PtCount;
					P->ConcatDstBase = DstPtBase;
					P->ConcatOffsetValue = int32(DstPtBase);
					P->ConcatOffsetMask = FIntVector4(1, 1, 1, 0);
					GraphBuilder.AddPass(RDG_EVENT_NAME("VineConcat.Meta"), P, ERDGPassFlags::Compute,
						[P, ConcatShader, PtCount](FRHIComputeCommandList& InRHICmdList)
						{ FComputeShaderUtils::Dispatch(InRHICmdList, ConcatShader, *P, FComputeShaderUtils::GetGroupCount(FIntVector(PtCount, 1, 1), 64)); });
				}

				// Segments: offset A/B by the point base.
				if (SgCount > 0u)
				{
					FRDGBufferRef SrcSgBuf = GraphBuilder.RegisterExternalBuffer(SrcSeg[s], TEXT("VineConcat.SrcSeg"));
					FConcatOffsetInt4CS::FParameters* P = GraphBuilder.AllocParameters<FConcatOffsetInt4CS::FParameters>();
					P->ConcatSrcInt4 = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SrcSgBuf));
					P->RW_ConcatDstInt4 = DstSeg.UAV;
					P->ConcatCount = SgCount;
					P->ConcatDstBase = DstSgBase;
					P->ConcatOffsetValue = int32(DstPtBase);
					P->ConcatOffsetMask = FIntVector4(1, 1, 0, 0);
					GraphBuilder.AddPass(RDG_EVENT_NAME("VineConcat.Seg"), P, ERDGPassFlags::Compute,
						[P, ConcatShader, SgCount](FRHIComputeCommandList& InRHICmdList)
						{ FComputeShaderUtils::Dispatch(InRHICmdList, ConcatShader, *P, FComputeShaderUtils::GetGroupCount(FIntVector(SgCount, 1, 1), 64)); });
				}
			}

			OutPathPoints = GraphBuilder.ConvertToExternalBuffer(DstPoints.Buffer);
			OutMeta = GraphBuilder.ConvertToExternalBuffer(DstMeta.Buffer);
			OutSeg = GraphBuilder.ConvertToExternalBuffer(DstSeg.Buffer);
			GraphBuilder.Execute();
			bConcatOK = true;
		});

	FlushRenderingCommands();

	if (!bConcatOK)
	{
		return false;
	}
	OutConcat.PathPoints = MoveTemp(OutPathPoints);
	OutConcat.PathPointMeta = MoveTemp(OutMeta);
	OutConcat.SegmentMeta = MoveTemp(OutSeg);
	OutConcat.PointCount = int32(TotalPoints);
	OutConcat.SegmentCount = int32(TotalSegments);
	OutConcat.LineCount = 0;
	return true;
}

// Aggregated inputs for the shared vine-mesh RDG pass graph (AddVineMeshPasses).
// Every value the graph body reads is threaded through here so the pass sequence
// itself moves verbatim. CPU-fallback arrays and the bucket table are passed by
// pointer into the caller-owned (render-command-captured) copies, which outlive the
// synchronous AddVineMeshPasses call.
struct FVineMeshPassInputs
{
	bool bUseGPULines = false;
	TRefCountPtr<FRDGPooledBuffer> GPULinePoints;
	TRefCountPtr<FRDGPooledBuffer> GPULineMeta;
	TRefCountPtr<FRDGPooledBuffer> GPULineSeg;
	const TArray<FVector4f>* PathPoints = nullptr;
	const TArray<FVector4f>* PathPointAxes = nullptr;
	const TArray<FIntVector4>* PathPointMeta = nullptr;
	const TArray<FIntVector4>* SegmentMeta = nullptr;
	bool bUseGPUVoxels = false;
	TRefCountPtr<FRDGPooledBuffer> GPUVoxCells;
	TRefCountPtr<FRDGPooledBuffer> GPUVoxNormals;
	TRefCountPtr<FRDGPooledBuffer> GPUVoxTargets;
	TRefCountPtr<FRDGPooledBuffer> GPUVoxCounter;
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
// the eight compute passes) into GraphBuilder. Behavior-identical extraction of the
// graph body formerly inlined in DispatchVVGPU_Voxel; the caller still owns the output
// buffers, the readback copies and GraphBuilder.Execute().
static void AddVineMeshPasses(FRDGBuilder& GraphBuilder, ERHIFeatureLevel::Type FeatureLevel, const FVineMeshPassInputs& In, const FVineMeshPassOutputs& Out)
{
	// Local aliases so the moved graph body below reads exactly as the original.
	const bool bUseGPULines = In.bUseGPULines;
	const TRefCountPtr<FRDGPooledBuffer>& GPULinePoints = In.GPULinePoints;
	const TRefCountPtr<FRDGPooledBuffer>& GPULineMeta = In.GPULineMeta;
	const TRefCountPtr<FRDGPooledBuffer>& GPULineSeg = In.GPULineSeg;
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

	CSHelper::FRDGStructuredBufferRefs PathPointBuffer;
	CSHelper::FRDGStructuredBufferRefs PathPointAxisBuffer;
	CSHelper::FRDGStructuredBufferRefs PathPointMetaBuffer;
	CSHelper::FRDGStructuredBufferRefs SegmentMetaBuffer;
	if (bUseGPULines)
	{
		auto RegisterSRVOnly = [&GraphBuilder](const TRefCountPtr<FRDGPooledBuffer>& Pooled, const TCHAR* Name)
		{
			CSHelper::FRDGStructuredBufferRefs Refs;
			Refs.Buffer = GraphBuilder.RegisterExternalBuffer(Pooled, Name);
			Refs.SRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Refs.Buffer));
			return Refs;
		};
		PathPointBuffer = RegisterSRVOnly(GPULinePoints, TEXT("VVVoxel.PathPoints.GPU"));
		PathPointMetaBuffer = RegisterSRVOnly(GPULineMeta, TEXT("VVVoxel.PathPointMeta.GPU"));
		SegmentMetaBuffer = RegisterSRVOnly(GPULineSeg, TEXT("VVVoxel.SegmentMeta.GPU"));
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
		auto RegVox = [&GraphBuilder, VoxCopyCount](const TRefCountPtr<FRDGPooledBuffer>& Pooled, uint32 BytesPerElem, const TCHAR* Name)
		{
			FRDGBufferRef Src = GraphBuilder.RegisterExternalBuffer(Pooled, Name);
			CSHelper::FRDGStructuredBufferRefs Dst = CSHelper::CreateStructuredBuffer(GraphBuilder, BytesPerElem, VoxCopyCount, Name, false, true);
			AddCopyBufferPass(GraphBuilder, Dst.Buffer, 0, Src, 0, uint64(BytesPerElem) * VoxCopyCount);
			return Dst;
		};
		VoxelCellsBuffer = RegVox(GPUVoxCells, sizeof(FIntVector4), TEXT("VVVoxel.VoxelCells.GPU"));
		VoxelNormalsBuffer = RegVox(GPUVoxNormals, sizeof(FVector4f), TEXT("VVVoxel.VoxelNormals.GPU"));
		VoxelTargetPositionsBuffer = RegVox(GPUVoxTargets, sizeof(FVector4f), TEXT("VVVoxel.VoxelTargetPositions.GPU"));
		VoxelHashSlotsBuffer = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(uint32), GpuVoxelHashSlotCountPow2, TEXT("VVVoxel.VoxelHashSlots.GPU"), true, true);
		AddClearUAVPass(GraphBuilder, VoxelHashSlotsBuffer.UAV, 0u);
		{
			FVVBuildVoxelHashCS::FParameters* HP = GraphBuilder.AllocParameters<FVVBuildVoxelHashCS::FParameters>();
			HP->HashBuildCells = VoxelCellsBuffer.SRV;
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

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("VVVoxel.ApplyNoise"),
		NoiseParameters,
		ERDGPassFlags::Compute,
		[NoiseParameters, NoiseShader, PathPointCount](FRHIComputeCommandList& InRHICmdList)
		{
			FComputeShaderUtils::Dispatch(InRHICmdList, NoiseShader, *NoiseParameters, FComputeShaderUtils::GetGroupCount(FIntVector(PathPointCount, 1, 1), 64));
	});

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

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("VVVoxel.BuildAxes"),
		BuildAxesParameters,
		ERDGPassFlags::Compute,
		[BuildAxesParameters, BuildAxesShader, PathPointCount](FRHIComputeCommandList& InRHICmdList)
		{
			FComputeShaderUtils::Dispatch(InRHICmdList, BuildAxesShader, *BuildAxesParameters, FComputeShaderUtils::GetGroupCount(FIntVector(PathPointCount, 1, 1), 64));
	});

	TShaderMapRef<FVVVoxelPerlinNoiseCS> PerlinNoiseShader(GetGlobalShaderMap(FeatureLevel));
	FVVVoxelPerlinNoiseCS::FParameters* PerlinNoiseParameters = GraphBuilder.AllocParameters<FVVVoxelPerlinNoiseCS::FParameters>();
	PerlinNoiseParameters->PathPointSurfaceTargets = PathPointSurfaceTargetA.SRV;
	PerlinNoiseParameters->PathPointSurfaceNormals = PathPointSurfaceNormalA.SRV;
	PerlinNoiseParameters->RW_PathPointSurfaceTargets = PathPointSurfaceTargetA.UAV;
	PerlinNoiseParameters->PerlinNoiseStrength = PerlinNoiseStrength;
	PerlinNoiseParameters->PerlinNoiseFrequency = PerlinNoiseFrequency;
	PerlinNoiseParameters->PathPointCount = PathPointCount;

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("VVVoxel.PerlinNoise"),
		PerlinNoiseParameters,
		ERDGPassFlags::Compute,
		[PerlinNoiseParameters, PerlinNoiseShader, PathPointCount](FRHIComputeCommandList& InRHICmdList)
		{
			FComputeShaderUtils::Dispatch(InRHICmdList, PerlinNoiseShader, *PerlinNoiseParameters, FComputeShaderUtils::GetGroupCount(FIntVector(PathPointCount, 1, 1), 64));
	});

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

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("VVVoxel.FinalProject"),
		FinalProjectParameters,
		ERDGPassFlags::Compute,
		[FinalProjectParameters, FinalProjectShader, PathPointCount](FRHIComputeCommandList& InRHICmdList)
		{
			FComputeShaderUtils::Dispatch(InRHICmdList, FinalProjectShader, *FinalProjectParameters, FComputeShaderUtils::GetGroupCount(FIntVector(PathPointCount, 1, 1), 64));
		});

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

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("VVVoxel.ResampleSurface"),
			ResampleParams,
			ERDGPassFlags::Compute,
			[ResampleParams, ResampleSurfaceShader, PathPointCount](FRHIComputeCommandList& InRHICmdList)
			{
				FComputeShaderUtils::Dispatch(InRHICmdList, ResampleSurfaceShader, *ResampleParams, FComputeShaderUtils::GetGroupCount(FIntVector(PathPointCount, 1, 1), 64));
			});

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

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("VVVoxel.SmoothPath%d", SmoothIterationIndex),
			SmoothParameters,
			ERDGPassFlags::Compute,
			[SmoothParameters, SmoothPathShader, PathPointCount](FRHIComputeCommandList& InRHICmdList)
			{
				FComputeShaderUtils::Dispatch(InRHICmdList, SmoothPathShader, *SmoothParameters, FComputeShaderUtils::GetGroupCount(FIntVector(PathPointCount, 1, 1), 64));
			});

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

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("VVVoxel.BuildParallelTransportFrame"),
		BuildTangentsParams,
		ERDGPassFlags::Compute,
		[BuildTangentsParams, BuildTangentsFromSurfaceShader, PathPointCount](FRHIComputeCommandList& InRHICmdList)
		{
			FComputeShaderUtils::Dispatch(InRHICmdList, BuildTangentsFromSurfaceShader, *BuildTangentsParams, FComputeShaderUtils::GetGroupCount(FIntVector(PathPointCount, 1, 1), 64));
		});

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
		Parameters->RWIndirectArgs = Out.IndirectArgsUAV;
		Parameters->RWMeshCounters = Out.MeshCountersUAV;
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
		Parameters->RWIndirectArgs = nullptr;
		Parameters->RWMeshCounters = nullptr;
	}
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

	const uint32 DispatchCount = FMath::Max(OutputVertexCount, SegmentCount);
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("VVVoxelCS"),
		Parameters,
		ERDGPassFlags::Compute,
		[Parameters, ComputeShader, DispatchCount](FRHIComputeCommandList& RHICmdList)
		{
			const uint32 GroupCountX = FMath::DivideAndRoundUp(DispatchCount, 64u);
			SetComputePipelineState(RHICmdList, ComputeShader.GetComputeShader());
			SetShaderParameters(RHICmdList, ComputeShader, ComputeShader.GetComputeShader(), *Parameters);
			RHICmdList.DispatchComputeShader(GroupCountX, 1, 1);
			UnsetShaderUAVs(RHICmdList, ComputeShader, ComputeShader.GetComputeShader());
		});

	// ------------------------------------------------------------------------
	// Base-stream axial V: pass#8 (BuildVVVoxelCS) wrote RWTexCoords[Index*2+1] = 0.
	// Recompute the correct V on the GPU straight from the base-stream Position buffer
	// it just filled, reproducing the CPU RecomputeVineOutputUVsFromGeneratedLength, and
	// overwrite it. The identity leaf makes RWPositions local==world, so the ring centers,
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

			// V1: ring center + circumference for every output point.
			{
				FVineUVCentersCS::FParameters* VP = GraphBuilder.AllocParameters<FVineUVCentersCS::FParameters>();
				VP->VineUV_Positions = VineUVPositionSRV;
				VP->RW_VineUV_Centers = VineUVCenters.UAV;
				VP->RW_VineUV_RingCirc = VineUVRingCirc.UAV;
				VP->VineUV_PointCount = VineUVPointCount;
				VP->VineUV_ProfileCount = ProfileCount;
				TShaderMapRef<FVineUVCentersCS> Shader(GetGlobalShaderMap(FeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VineUV.Centers"), Shader, VP, FComputeShaderUtils::GetGroupCount(VineUVPointCount, 64));
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
				TShaderMapRef<FVineUVSegLenCS> Shader(GetGlobalShaderMap(FeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VineUV.SegLen"), Shader, VP, FComputeShaderUtils::GetGroupCount(FMath::Max(SegmentCount, 1u), 64));
			}

			// V3: serial prefix scan with per-line reset -> CurveV (single thread, one group).
			{
				FVineUVScanCS::FParameters* VP = GraphBuilder.AllocParameters<FVineUVScanCS::FParameters>();
				VP->VineUV_SegLen = VineUVSegLen.SRV;
				VP->VineUV_RingCirc = VineUVRingCirc.SRV;
				VP->RW_VineUV_CurveV = VineUVCurveV.UAV;
				VP->VineUV_PointCount = VineUVPointCount;
				VP->VineUV_LengthScale = In.UVLengthScale; // raw; the shader applies max(.,1e-8)
				TShaderMapRef<FVineUVScanCS> Shader(GetGlobalShaderMap(FeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VineUV.Scan"), Shader, VP, FIntVector(1, 1, 1));
			}

			// V4: broadcast CurveV[P] into the V of every ring vertex (overwrites pass#8's 0).
			{
				FVineUVWriteCS::FParameters* VP = GraphBuilder.AllocParameters<FVineUVWriteCS::FParameters>();
				VP->VineUV_CurveV = VineUVCurveV.SRV;
				VP->RW_VineUV_TexCoords = Out.TexCoordUAV;
				VP->VineUV_OutputVertexCount = OutputVertexCount;
				VP->VineUV_ProfileCount = ProfileCount;
				VP->VineUV_PointCount = VineUVPointCount;
				TShaderMapRef<FVineUVWriteCS> Shader(GetGlobalShaderMap(FeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VineUV.Write"), Shader, VP, FComputeShaderUtils::GetGroupCount(OutputVertexCount, 64));
			}
		}
	}
}

// Shared CPU-prep for the vine mesh pass graph: repacks the surface voxels into GPU-upload
// arrays, builds the voxel hash + target-position buckets, derives the output vertex/index counts
// and the sanitized scalar parameters, and captures the GPU-resident line/voxel pooled refs.
// Produces a self-owning FVineBuildInput consumed by BOTH DispatchVVGPU_Voxel (legacy readback
// path) and FVineMeshSceneProxy (GPU-resident leaf) — a behavior-identical extraction of the CPU
// work formerly inlined at the top of DispatchVVGPU_Voxel. On failure it returns bValid=false,
// mirroring the dispatch's early returns (including the same warning log). The optional out-params
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
	const FVineSCGPUBuffers* GPULines,
	const FCSSurfaceVoxelGPUBuffers* GPUVoxels,
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
	const bool bUseGPUVoxels = (GPUVoxels != nullptr && GPUVoxels->IsValid());
	const uint32 GpuVoxelHashSlotCountPow2 = bUseGPUVoxels
		? FMath::RoundUpToPowerOfTwo(uint32(FMath::Max(GPUVoxels->VoxelCapacity * 2, 16))) : 0u;
	const uint32 PathPointCount = bUseGPULines ? uint32(GPULines->PointCount) : uint32(PathPoints.Num());
	const uint32 SegmentCount = bUseGPULines ? uint32(GPULines->SegmentCount) : uint32(SegmentMeta.Num());
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
		return B; // bValid stays false (silent, mirrors DispatchVVGPU_Voxel's count early return)
	}

	const uint32 VoxelCount = bUseGPUVoxels ? uint32(GPUVoxels->VoxelCapacity) : uint32(VoxelData.Cells.Num());
	if (VoxelCount == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU_Voxel] No voxel data available."));
		return B; // bValid stays false
	}
	B.VoxelCount = VoxelCount;
	B.VoxelOrigin = bUseGPUVoxels ? FVector3f(GPUVoxels->VoxelOrigin) : FVector3f(VoxelData.VoxelOrigin);
	B.VoxelSize = bUseGPUVoxels ? GPUVoxels->VoxelSize : float(VoxelData.VoxelSize);

	if (bUseGPUVoxels)
	{
		B.GPULinePoints = bUseGPULines ? GPULines->PathPoints : nullptr;
		B.GPULineMeta = bUseGPULines ? GPULines->PathPointMeta : nullptr;
		B.GPULineSeg = bUseGPULines ? GPULines->SegmentMeta : nullptr;
		B.GPUVoxCells = GPUVoxels->Cells;
		B.GPUVoxNormals = GPUVoxels->Normals;
		B.GPUVoxTargets = GPUVoxels->TargetPositions;
		B.GPUVoxCounter = GPUVoxels->Counter;
		B.GPUVoxCount = VoxelCount;
		B.GPUVoxelHashSlotCount = GpuVoxelHashSlotCountPow2;
		B.TargetBucketOrigin = B.VoxelOrigin;
		B.LocalBounds = GPUVoxels->WorldBounds;
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

	// Capture the GPU-resident line / voxel buffers (null on the CPU-array paths). On the GPU-voxel
	// path the vine hash is rebuilt on the GPU, so bind the pow2 slot count.
	B.GPULinePoints = bUseGPULines ? GPULines->PathPoints : nullptr;
	B.GPULineMeta = bUseGPULines ? GPULines->PathPointMeta : nullptr;
	B.GPULineSeg = bUseGPULines ? GPULines->SegmentMeta : nullptr;
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
	In.bUseGPULines = B.bUseGPULines;
	In.GPULinePoints = B.GPULinePoints;
	In.GPULineMeta = B.GPULineMeta;
	In.GPULineSeg = B.GPULineSeg;
	In.PathPoints = &B.PathPoints;
	In.PathPointAxes = &B.PathPointAxes;
	In.PathPointMeta = &B.PathPointMeta;
	In.SegmentMeta = &B.SegmentMeta;
	In.bUseGPUVoxels = B.bUseGPUVoxels;
	In.GPUVoxCells = B.GPUVoxCells;
	In.GPUVoxNormals = B.GPUVoxNormals;
	In.GPUVoxTargets = B.GPUVoxTargets;
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

static bool DispatchVVGPU_Voxel(
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
	TArray<FVector4f>& OutVertices,
	TArray<FVector2f>& OutUVs,
	TArray<uint32>& OutIndices,
	EVisVineGPUDebugStage DebugStage = EVisVineGPUDebugStage::Smooth,
	const FVineSCGPUBuffers* GPULines = nullptr,
	const FCSSurfaceVoxelGPUBuffers* GPUVoxels = nullptr)
{
	const double DispatchTotalStartSeconds = FPlatformTime::Seconds();
	double BuildVoxelUploadMs = 0.0;
	double BuildHashMs = 0.0;
	double BuildTargetBucketsMs = 0.0;
	double EnqueueAndFlushMs = 0.0;
	double ReadbackFlushMs = 0.0;

	OutVertices.Reset();
	OutUVs.Reset();
	OutIndices.Reset();

	// Shared CPU-prep (voxel repack + hash + target buckets + counts + sanitized scalars + GPU
	// line/voxel pooled refs). Identical to the leaf's prep; the returned bundle owns every array
	// the pass graph reads. bValid=false reproduces the original early returns (zero counts / no
	// voxels), including the same warning logs, so the legacy readback path's behavior is unchanged.
	FVineBuildInput Bundle = VineLeaf_BuildVineBuildInput(
		PathPoints, PathPointAxes, PathPointMeta, SegmentMeta,
		bTube, TubeProfileCount, CircleScale, LineScale, UVLengthScale, VinesOffset, TinyZJitterStrength,
		PostProjectionSmoothIterations, PostProjectionSmoothKernelRadius, PostProjectionSmallSmoothIterations, PostProjectionSmoothAngleStrength,
		bResampleSurface, ResampleTargetDistance, CurlNoiseStrength, CurlNoiseFrequency, PerlinNoiseStrength, PerlinNoiseFrequency, NoiseIterations,
		VoxelData, DebugStage, GPULines, GPUVoxels,
		&BuildVoxelUploadMs, &BuildHashMs, &BuildTargetBucketsMs);
	if (!Bundle.bValid)
	{
		return false;
	}

	// Locals the readback path / timing log still need (read from the shared bundle).
	const uint32 PathPointCount = Bundle.PathPointCount;
	const uint32 VoxelCount = Bundle.VoxelCount;
	const uint32 OutputVertexCount = Bundle.OutputVertexCount;
	const uint32 OutputIndexCount = Bundle.OutputIndexCount;
	const FVineTargetBucketBuffers& TargetBuckets = Bundle.TargetBuckets;

	const uint64 VertexReadbackBytes64 = uint64(OutputVertexCount) * sizeof(FVector4f);
	const uint64 UVReadbackBytes64 = uint64(OutputVertexCount) * sizeof(FVector2f);
	const uint64 IndexReadbackBytes64 = uint64(OutputIndexCount) * sizeof(uint32);
	if (VertexReadbackBytes64 > MAX_uint32 || UVReadbackBytes64 > MAX_uint32 || IndexReadbackBytes64 > MAX_uint32)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU_Voxel] Output is too large for readback. Vertices=%u Indices=%u"), OutputVertexCount, OutputIndexCount);
		return false;
	}

	const uint32 VertexReadbackBytes = uint32(VertexReadbackBytes64);
	const uint32 UVReadbackBytes = uint32(UVReadbackBytes64);
	const uint32 IndexReadbackBytes = uint32(IndexReadbackBytes64);
	FRHIGPUBufferReadback* VertexReadback = new FRHIGPUBufferReadback(TEXT("VVVoxel_VertexReadback"));
	FRHIGPUBufferReadback* UVReadback = new FRHIGPUBufferReadback(TEXT("VVVoxel_UVReadback"));
	FRHIGPUBufferReadback* IndexReadback = new FRHIGPUBufferReadback(TEXT("VVVoxel_IndexReadback"));
	bool bRenderWorkQueued = false;

	const double EnqueueAndFlushStartSeconds = FPlatformTime::Seconds();
	ENQUEUE_RENDER_COMMAND(VVVoxelGPU)(
		[Bundle, VertexReadback, UVReadback, IndexReadback,
		 VertexReadbackBytes, UVReadbackBytes, IndexReadbackBytes,
		 OutputVertexCount, OutputIndexCount,
		 &bRenderWorkQueued](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			// The bundle (captured by value) owns every array the pass graph reads. Create the three
			// transient output buffers here (caller-owned so the readback copies below can reference
			// them), then run the shared vine-mesh pass graph fed from the bundle.
			const CSHelper::FRDGStructuredBufferRefs OutVertexBuffer = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), OutputVertexCount, TEXT("VVVoxel.OutVertices"), true, true);
			const CSHelper::FRDGStructuredBufferRefs OutUVBuffer = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(FVector2f), OutputVertexCount, TEXT("VVVoxel.OutUVs"), true, true);
			const CSHelper::FRDGStructuredBufferRefs OutIndexBuffer = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(uint32), OutputIndexCount, TEXT("VVVoxel.OutIndices"), true, true);

			// Legacy readback path: bBaseStreams stays false (default), so the shader writes the three
			// transient StructuredBuffer UAVs below — byte-identical to before. The readback-only
			// fields are set here (they are not part of the shared bundle).
			FVineMeshPassInputs In = VineLeaf_MakePassInputs(Bundle);
			// Already set by VineLeaf_MakePassInputs from the bundle; restated to make the LengthScale
			// plumbing explicit at the legacy dispatch (bBaseStreams=false => the V passes never run).
			In.UVLengthScale = Bundle.UVLengthScale;

			FVineMeshPassOutputs Out;
			Out.OutVerticesUAV = OutVertexBuffer.UAV;
			Out.OutUVsUAV = OutUVBuffer.UAV;
			Out.OutIndicesUAV = OutIndexBuffer.UAV;

			AddVineMeshPasses(GraphBuilder, GMaxRHIFeatureLevel, In, Out);

			AddEnqueueCopyPass(GraphBuilder, VertexReadback, OutVertexBuffer.Buffer, VertexReadbackBytes);
			AddEnqueueCopyPass(GraphBuilder, UVReadback, OutUVBuffer.Buffer, UVReadbackBytes);
			AddEnqueueCopyPass(GraphBuilder, IndexReadback, OutIndexBuffer.Buffer, IndexReadbackBytes);
			GraphBuilder.Execute();
			bRenderWorkQueued = true;
		});

	FlushRenderingCommands();
	EnqueueAndFlushMs = (FPlatformTime::Seconds() - EnqueueAndFlushStartSeconds) * 1000.0;

	if (!bRenderWorkQueued)
	{
		delete VertexReadback;
		delete UVReadback;
		delete IndexReadback;
		return false;
	}

	OutVertices.SetNumZeroed(OutputVertexCount);
	OutUVs.SetNumZeroed(OutputVertexCount);
	OutIndices.SetNumZeroed(OutputIndexCount);
	bool bReadbackSucceeded = false;

	const double ReadbackFlushStartSeconds = FPlatformTime::Seconds();
	ENQUEUE_RENDER_COMMAND(VVVoxelGPUReadback)(
		[VertexReadback, UVReadback, IndexReadback, VertexReadbackBytes, UVReadbackBytes, IndexReadbackBytes, &OutVertices, &OutUVs, &OutIndices, &bReadbackSucceeded](FRHICommandListImmediate& RHICmdList)
		{
			if (!VertexReadback || !UVReadback || !IndexReadback)
			{
				return;
			}

			if (!VertexReadback->IsReady() || !UVReadback->IsReady() || !IndexReadback->IsReady())
			{
				RHICmdList.SubmitAndBlockUntilGPUIdle();
			}

			bool bLockedAll = true;
			if (const FVector4f* VertexPtr = static_cast<const FVector4f*>(VertexReadback->Lock(VertexReadbackBytes)))
			{
				FMemory::Memcpy(OutVertices.GetData(), VertexPtr, VertexReadbackBytes);
				VertexReadback->Unlock();
			}
			else
			{
				bLockedAll = false;
			}

			if (const FVector2f* UVPtr = static_cast<const FVector2f*>(UVReadback->Lock(UVReadbackBytes)))
			{
				FMemory::Memcpy(OutUVs.GetData(), UVPtr, UVReadbackBytes);
				UVReadback->Unlock();
			}
			else
			{
				bLockedAll = false;
			}

			if (const uint32* IndexPtr = static_cast<const uint32*>(IndexReadback->Lock(IndexReadbackBytes)))
			{
				FMemory::Memcpy(OutIndices.GetData(), IndexPtr, IndexReadbackBytes);
				IndexReadback->Unlock();
			}
			else
			{
				bLockedAll = false;
			}

			delete VertexReadback;
			delete UVReadback;
			delete IndexReadback;
			bReadbackSucceeded = bLockedAll;
		});

	FlushRenderingCommands();
	ReadbackFlushMs = (FPlatformTime::Seconds() - ReadbackFlushStartSeconds) * 1000.0;
	UE_LOG(LogTemp, Display,
		TEXT("[VisVineGPUDispatchTiming] %s total=%.3f ms buildVoxelUpload=%.3f ms buildHash=%.3f ms buildTargetBuckets=%.3f ms enqueueAndFlush=%.3f ms readbackFlush=%.3f ms pathPoints=%u voxels=%u targetBuckets=%u targetBucketSize=%.3f targetSearchRadius=%u targetSearchCoverage=%.3f targetBucketAvgItems=%.3f targetBucketMaxItems=%u outVerts=%u outIndices=%u"),
		bTube ? TEXT("tube") : TEXT("plane"),
		(FPlatformTime::Seconds() - DispatchTotalStartSeconds) * 1000.0,
		BuildVoxelUploadMs,
		BuildHashMs,
		BuildTargetBucketsMs,
		EnqueueAndFlushMs,
		ReadbackFlushMs,
		PathPointCount,
		VoxelCount,
		TargetBuckets.BucketCount,
		TargetBuckets.BucketSize,
		TargetBuckets.SearchRadius,
		TargetBuckets.BucketSize * float(TargetBuckets.SearchRadius),
		TargetBuckets.BucketCount > 0u ? double(TargetBuckets.VoxelIndices.Num()) / double(TargetBuckets.BucketCount) : 0.0,
		TargetBuckets.MaxBucketItemCount,
		OutputVertexCount,
		OutputIndexCount);
	return bReadbackSucceeded;
}

static FVector GetVineOutputProfileCenter(
	const TArray<FVector4f>& Vertices,
	int32 PointIndex,
	uint32 ProfileCount)
{
	FVector Center = FVector::ZeroVector;
	if (ProfileCount == 0)
	{
		return Center;
	}

	const int32 BaseIndex = PointIndex * int32(ProfileCount);
	for (uint32 ProfileIndex = 0; ProfileIndex < ProfileCount; ++ProfileIndex)
	{
		const int32 VertexIndex = BaseIndex + int32(ProfileIndex);
		if (!Vertices.IsValidIndex(VertexIndex))
		{
			continue;
		}

		const FVector4f& Vertex = Vertices[VertexIndex];
		Center += FVector(Vertex.X, Vertex.Y, Vertex.Z);
	}
	return Center / double(ProfileCount);
}

static void RecomputeVineOutputUVsFromGeneratedLength(
	const TArray<FVector4f>& Vertices,
	const TArray<FVector4f>& PathPoints,
	const TArray<FIntVector4>& SegmentMeta,
	uint32 ProfileCount,
	const FVV& VV,
	TArray<FVector2f>& UVs)
{
	if (ProfileCount == 0 || Vertices.Num() == 0 || UVs.Num() != Vertices.Num())
	{
		return;
	}

	const int32 PointCount = Vertices.Num() / int32(ProfileCount);
	if (PointCount <= 0)
	{
		return;
	}

	TArray<float> SegmentLengths;
	TArray<float> PointScales;
	SegmentLengths.Init(-1.0f, FMath::Max(PointCount - 1, 0));
	PointScales.SetNumUninitialized(PointCount);
	for (int32 PointIndex = 0; PointIndex < PointCount; ++PointIndex)
	{
		PointScales[PointIndex] = PathPoints.IsValidIndex(PointIndex) ? PathPoints[PointIndex].W : 1.0f;
	}

	for (const FIntVector4& Segment : SegmentMeta)
	{
		const int32 APoint = Segment.X;
		const int32 BPoint = Segment.Y;
		if (!PointScales.IsValidIndex(APoint) || !PointScales.IsValidIndex(BPoint) || BPoint != APoint + 1 || !SegmentLengths.IsValidIndex(APoint))
		{
			continue;
		}

		const FVector ACenter = GetVineOutputProfileCenter(Vertices, APoint, ProfileCount);
		const FVector BCenter = GetVineOutputProfileCenter(Vertices, BPoint, ProfileCount);
		SegmentLengths[APoint] = float(FVector::Dist(ACenter, BCenter));
	}

	// 每个 path point 的真实环向周长（多边形闭合周长，cm）。直接用输出顶点几何，
	// 自动包含 CircleScale 与 per-point Scale，无需再读参数。
	TArray<float> RingCircumference;
	RingCircumference.SetNumZeroed(PointCount);
	for (int32 PointIndex = 0; PointIndex < PointCount; ++PointIndex)
	{
		const int32 BaseIndex = PointIndex * int32(ProfileCount);
		float Perimeter = 0.0f;
		for (uint32 ProfileIndex = 0; ProfileIndex < ProfileCount; ++ProfileIndex)
		{
			const int32 CurrIndex = BaseIndex + int32(ProfileIndex);
			const int32 NextIndex = BaseIndex + int32((ProfileIndex + 1u) % ProfileCount);
			if (!Vertices.IsValidIndex(CurrIndex) || !Vertices.IsValidIndex(NextIndex))
			{
				continue;
			}
			const FVector4f& C = Vertices[CurrIndex];
			const FVector4f& N = Vertices[NextIndex];
			Perimeter += float(FVector::Dist(FVector(C.X, C.Y, C.Z), FVector(N.X, N.Y, N.Z)));
		}
		RingCircumference[PointIndex] = Perimeter;
	}

	// V 以“局部周长”为单位累加：藤蔓每沿轴向走过一整圈周长，V 就 +1，正好与环向
	// U 的 0→1 对齐 → 方格各向同性。粗藤蔓周长大 V 走得慢（纹理疏），细藤蔓周长小
	// V 走得快（纹理密），scale 自动兼顾。UVLengthScale 退化为整体倍率微调。
	const float LengthScale = FMath::Max(VV.UVLengthScale, 1.0e-8f);
	TArray<float> GeneratedCurveU;
	GeneratedCurveU.SetNumZeroed(PointCount);
	for (int32 PointIndex = 1; PointIndex < PointCount; ++PointIndex)
	{
		const float AxialLength = SegmentLengths.IsValidIndex(PointIndex - 1) ? SegmentLengths[PointIndex - 1] : -1.0f;
		if (AxialLength < 0.0f)
		{
			// 段断开：V 重置，新的一段从 0 重新累加。
			GeneratedCurveU[PointIndex] = 0.0f;
			continue;
		}

		const float PrevCirc = RingCircumference.IsValidIndex(PointIndex - 1) ? RingCircumference[PointIndex - 1] : 0.0f;
		const float CurrCirc = RingCircumference.IsValidIndex(PointIndex) ? RingCircumference[PointIndex] : 0.0f;
		const float AvgCirc = FMath::Max((PrevCirc + CurrCirc) * 0.5f, 1.0e-4f);
		GeneratedCurveU[PointIndex] = GeneratedCurveU[PointIndex - 1] + (AxialLength / AvgCirc) * LengthScale;
	}

	for (int32 PointIndex = 0; PointIndex < PointCount; ++PointIndex)
	{
		const float V = GeneratedCurveU.IsValidIndex(PointIndex) ? GeneratedCurveU[PointIndex] : 0.0f;
		const int32 BaseIndex = PointIndex * int32(ProfileCount);
		for (uint32 ProfileIndex = 0; ProfileIndex < ProfileCount; ++ProfileIndex)
		{
			const int32 VertexIndex = BaseIndex + int32(ProfileIndex);
			if (UVs.IsValidIndex(VertexIndex))
			{
				UVs[VertexIndex].Y = V;
			}
		}
	}
}

static UDynamicMesh* BuildDynamicMeshFromGPUVineOutput(
	UObject* Outer,
	const TArray<FVector4f>& Vertices,
	const TArray<FVector2f>& UVs,
	const TArray<uint32>& Indices,
	int32 MaterialID,
	bool bRecomputeNormals)
{
	if (Vertices.Num() == 0 || UVs.Num() != Vertices.Num() || Indices.Num() < 3)
	{
		return nullptr;
	}

	UDynamicMesh* OutMesh = NewObject<UDynamicMesh>(Outer);
	if (!OutMesh)
	{
		return nullptr;
	}

	FDynamicMesh3 Mesh;
	Mesh.EnableAttributes();
	Mesh.Attributes()->EnableMaterialID();
	Mesh.Attributes()->SetNumUVLayers(1);

	// Append vertices (position only at this stage)
	for (const FVector4f& Vertex : Vertices)
	{
		Mesh.AppendVertex(FVector3d(Vertex.X, Vertex.Y, Vertex.Z));
	}

	// Append triangles and set per-triangle UVs via the UV overlay
	UE::Geometry::FDynamicMeshUVOverlay* UVOverlay = Mesh.Attributes()->GetUVLayer(0);

	// Pre-create UV elements for each vertex (shared UV per vertex)
	TArray<int32> UVElementIDs;
	UVElementIDs.SetNum(Vertices.Num());
	for (int32 i = 0; i < Vertices.Num(); ++i)
	{
		UVElementIDs[i] = UVOverlay->AppendElement(UVs[i]);
	}

	for (int32 Index = 0; Index + 2 < Indices.Num(); Index += 3)
	{
		const int32 A = int32(Indices[Index + 0]);
		const int32 B = int32(Indices[Index + 1]);
		const int32 C = int32(Indices[Index + 2]);
		if (A < 0 || B < 0 || C < 0 || A >= Vertices.Num() || B >= Vertices.Num() || C >= Vertices.Num() || A == B || B == C || A == C)
		{
			continue;
		}

		const int32 TriangleID = Mesh.AppendTriangle(A, C, B);
		if (TriangleID >= 0)
		{
			Mesh.Attributes()->GetMaterialID()->SetNewValue(TriangleID, MaterialID);
			UVOverlay->SetTriangle(TriangleID, UE::Geometry::FIndex3i(UVElementIDs[A], UVElementIDs[C], UVElementIDs[B]));
		}
	}

	OutMesh->SetMesh(MoveTemp(Mesh));
	if (bRecomputeNormals)
	{
		FGeometryScriptCalculateNormalsOptions CalculateOptions;
		UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(OutMesh, CalculateOptions);
	}
	return OutMesh;
}
}

// ============================================================================
// GPU-resident vine leaf (M1): UVineMeshComponent + FVineMeshSceneProxy.
// Runs in PARALLEL with the legacy UDynamicMesh path — it consumes the same shared
// FVineBuildInput bundle and drives AddVineMeshPasses with the base-stream permutation, so the
// vine mesh is built + drawn entirely on the GPU (no readback). The implementation lives here in
// GeometryEditorActor.cpp so it can see the file-local AddVineMeshPasses / FVineMeshPassInputs /
// FVineMeshPassOutputs / VineLeaf_MakePassInputs without exposing them in a header.
// ============================================================================

// Scene proxy: registers the base standard-triangle streams sized to the CPU-known output counts,
// then records the shared vine pass graph into an FRDGBuilder writing straight into those streams.
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
		// vertex capacity via the standard set; both come from the CPU-known counts in the bundle.
		VertexCapacity = FMath::Max(Input.OutputVertexCount, 64u);
		IndexCapacity = FMath::Max(Input.OutputIndexCount, 192u);
		AddStandardTriangleStreams();
	}

	virtual void BuildGeometry(FRHICommandListBase& RHICmdList) override
	{
		if (!Input.bValid || Input.OutputVertexCount == 0u || Input.OutputIndexCount == 0u) return;

		FRHICommandListImmediate& RHICmdListImmediate = FRHICommandListExecutor::GetImmediateCommandList();
		FRDGBuilder GraphBuilder(RHICmdListImmediate, RDG_EVENT_NAME("VineMesh.Build"));

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
	return CSGpuMeshSave::SaveGpuMeshComponentToStaticMesh(
		this, AssetPathAndName, VineMaterial, GetComponentTransform(),
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
	DynamicMeshComponent = CreateDefaultSubobject<UDynamicMeshComponent>(TEXT("DynamicMeshComponent"));
	DynamicMeshComponent->SetupAttachment(GetRootComponent());
	DynamicMeshComponent->bUseAttachParentBound = false;
	DynamicMeshComponent->bNeverDistanceCull = true;
	DynamicMeshComponent->bAllowCullDistanceVolume = false;
	DynamicMeshComponent->SetCachedMaxDrawDistance(0.0f);
	DynamicMeshComponent->SetBoundsScale(DynamicMeshCullBoundsScale);

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

	// M1 GPU-resident vine leaf, mounted in parallel with the DynamicMesh path. The vine geometry
	// is emitted in world space, so keep this component at an identity WORLD transform regardless of
	// where the actor is placed: mark the transform absolute (ignore the parent) and leave it at
	// identity. VineWorldToLocal is Identity to match (see FVineMeshSceneProxy::BuildGeometry).
	VineGpuMesh = CreateDefaultSubobject<UVineMeshComponent>(TEXT("VineMesh"));
	VineGpuMesh->SetupAttachment(GetRootComponent());
	VineGpuMesh->SetUsingAbsoluteLocation(true);
	VineGpuMesh->SetUsingAbsoluteRotation(true);
	VineGpuMesh->SetUsingAbsoluteScale(true);
	VineGpuMesh->SetRelativeTransform(FTransform::Identity);

	ApplyVineReferenceComponentsHiddenInGame(this);
	RebuildDisplayInstancesFromTransformArrays();
}

void AVineContainer::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	RefreshDynamicMeshComponentCullingBounds();
	// ApplyVineReferenceComponentsHiddenInGame(this);
	// RebuildDisplayInstancesFromTransformArrays();
}

void AVineContainer::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	RefreshDynamicMeshComponentCullingBounds();
}

void AVineContainer::RefreshDynamicMeshComponentCullingBounds(float BoundsScale)
{
	UDynamicMeshComponent* MeshComponent = GetDynamicMeshComponent();
	if (!MeshComponent) return;

	const float SafeBoundsScale = FMath::Max(
		BoundsScale > 0.0f ? BoundsScale : DynamicMeshCullBoundsScale,
		1.0f);

	// DynamicMeshComponent is attached under the actor root. If it uses the attach
	// parent's tiny bounds, the mesh is culled when the actor/root origin leaves the
	// view, even if the generated DynamicMesh is still visible.
	MeshComponent->bUseAttachParentBound = false;
	MeshComponent->bNeverDistanceCull = true;
	MeshComponent->bAllowCullDistanceVolume = false;
	MeshComponent->SetCachedMaxDrawDistance(0.0f);
	MeshComponent->SetBoundsScale(SafeBoundsScale);

	// Rebuild render proxy + recompute LocalBounds from the actual mesh. This fixes
	// stale bounds after replacing or editing UDynamicMesh data through
	// Blueprint/GeometryScript paths.
	MeshComponent->NotifyMeshUpdated();
	MeshComponent->UpdateBounds();
	MeshComponent->MarkRenderTransformDirty();
	MeshComponent->MarkRenderStateDirty();
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
	UInstancedStaticMeshComponent* DisplayComponent = nullptr;
	if (!ResolveVineReferenceComponent(this, InFoliageType, DisplayComponent) || !DisplayComponent)
	{
		return;
	}

	Modify();
	DisplayComponent->Modify();
	if (!Transforms.IsEmpty())
	{
		DisplayComponent->AddInstances(Transforms, false, true, false);
	}
	MarkPackageDirty();
	RefreshVineDisplayComponent(DisplayComponent);

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

	UInstancedStaticMeshComponent* DisplayComponent = nullptr;
	if (!ResolveVineReferenceComponent(this, InFoliageType, DisplayComponent) || !DisplayComponent)
		return;

	TArray<FTransform> InstanceTransforms;
	GetVineInstanceTransforms(DisplayComponent, InstanceTransforms);

	if (InstanceTransforms.IsEmpty())
	{
		RefreshVineDisplayComponent(DisplayComponent);
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

	DisplayComponent->Modify();
	DisplayComponent->ClearInstances();
	MarkPackageDirty();
	RefreshVineDisplayComponent(DisplayComponent);
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

	UDynamicMeshComponent* MeshComponent = GetDynamicMeshComponent();
	if (!MeshComponent)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] DynamicMeshComponent is null."));
		return false;
	}

	UDynamicMesh* ContainerMesh = MeshComponent->GetDynamicMesh();
	if (!ContainerMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] Container dynamic mesh is null."));
		return false;
	}

	if (!LastSurfaceVoxelGPUBuffers.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] No retained GPU surface voxel data for visualization."));
		return false;
	}
	const FCSSurfaceVoxelData EmptySurfaceVoxelData;

	// The GPU space-colonization output is the only path now: the per-source prepped lines are
	// merged into one GPU-resident batch that DispatchVVGPU_Voxel consumes directly.
	FVineSCGPUBuffers ConcatenatedGPULines;
	if (TubeLineGPUBuffers.Num() == 0
		|| !ConcatenateVineSCGPUBuffers(TubeLineGPUBuffers, ConcatenatedGPULines)
		|| ConcatenatedGPULines.PointCount == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] No GPU-resident tube lines to visualize."));
		return false;
	}

	// DispatchVVGPU_Voxel reads the geometry from ConcatenatedGPULines; these locals stay empty.
	TArray<FVector4f> PathPoints;
	TArray<FVector4f> PathPointAxes;
	TArray<FIntVector4> PathPointMeta;
	TArray<FIntVector4> SegmentMeta;
	double PrepareLinesMs = 0.0;
	double BuildGPUInputMs = 0.0;

	UE_LOG(LogTemp, Display, TEXT("[VineSCFused] GPU-resident lines: points=%d segments=%d"),
		ConcatenatedGPULines.PointCount, ConcatenatedGPULines.SegmentCount);

	TArray<FVector4f> OutVertices;
	TArray<FVector2f> OutUVs;
	TArray<uint32> OutIndices;
	const EVisVineGPUDebugStage DebugStage = SplineDebug.DebugStage;
	const bool bWantStageDraw = SplineDebug.bDrawDebugLines && DebugStage != EVisVineGPUDebugStage::None;

	const double DispatchStartSeconds = FPlatformTime::Seconds();
	if (!DispatchVVGPU_Voxel(
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
		OutVertices,
		OutUVs,
		OutIndices,
		DebugStage,
		&ConcatenatedGPULines,
		LastSurfaceVoxelGPUBuffers.IsValid() ? &LastSurfaceVoxelGPUBuffers : nullptr))
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] Voxel GPU dispatch/readback failed."));
		return false;
	}
	const double DispatchMs = (FPlatformTime::Seconds() - DispatchStartSeconds) * 1000.0;

	// M1 parallel path: the DynamicMesh path above stays authoritative and untouched. ADDITIONALLY,
	// feed the SAME inputs through the shared CPU-prep helper and hand the resulting bundle to the
	// GPU-resident leaf, which builds + draws the tube vine entirely on the GPU (base-stream
	// permutation, no readback). The two representations render side by side for visual comparison.
	if (VineGpuMesh)
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
			&ConcatenatedGPULines,
			LastSurfaceVoxelGPUBuffers.IsValid() ? &LastSurfaceVoxelGPUBuffers : nullptr);
		if (LeafInput.bValid)
		{
			LeafInput.DebugLineColor = SplineDebug.SplineColor;
			const uint32 LeafVertexCount = LeafInput.OutputVertexCount;
			const uint32 LeafIndexCount = LeafInput.OutputIndexCount;
			// Mirror the legacy DynamicMeshComponent's slot-0 material onto the leaf so the base-stream
			// render matches the old path instead of the gray default-material fallback (M1 verification).
			if (UDynamicMeshComponent* VineDMC = GetDynamicMeshComponent()) VineGpuMesh->VineMaterial = VineDMC->GetMaterial(0);
			VineGpuMesh->SetBuildInput(MoveTemp(LeafInput));
			UE_LOG(LogTemp, Log, TEXT("[VisVineGPU] GPU-resident vine leaf built (parallel). Verts=%u Indices=%u"),
				LeafVertexCount, LeafIndexCount);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] GPU-resident vine leaf input was invalid; leaf not rebuilt."));
		}
	}

	// The selected center-line stage is rendered directly from GPU buffers by VineGpuMesh.
	// Clear any persistent CPU DrawDebug lines left by an older generation.
	ClearDebugVineSplineActor();

	// On the fused path the CPU-side SegmentMeta array is never populated (the prepped line
	// geometry stays GPU-resident), but RecomputeVineOutputUVsFromGeneratedLength below needs
	// the within-line consecutive point pairs to accumulate the axial V (it resets V at line
	// boundaries, where no segment exists). Reconstruct that connectivity from the output tube
	// topology: a segment (A, A+1) exists iff a triangle spans rings A and A+1
	// (pointIndex = vertexIndex / ProfileCount). Without this the recompute sees all-(-1)
	// segment lengths and writes V=0 everywhere.
	if (SegmentMeta.Num() == 0)
	{
		const int32 ProfileCount = 3;
		const int32 FusedPointCount = OutVertices.Num() / ProfileCount;
		if (FusedPointCount > 1 && OutIndices.Num() >= 3)
		{
			TArray<bool> HasSegment;
			HasSegment.Init(false, FusedPointCount - 1);
			for (int32 i = 0; i + 2 < OutIndices.Num(); i += 3)
			{
				const int32 P0 = int32(OutIndices[i] / uint32(ProfileCount));
				const int32 P1 = int32(OutIndices[i + 1] / uint32(ProfileCount));
				const int32 P2 = int32(OutIndices[i + 2] / uint32(ProfileCount));
				const int32 PMin = FMath::Min3(P0, P1, P2);
				const int32 PMax = FMath::Max3(P0, P1, P2);
				if (PMax == PMin + 1 && HasSegment.IsValidIndex(PMin)) HasSegment[PMin] = true;
			}
			SegmentMeta.Reserve(FusedPointCount - 1);
			for (int32 A = 0; A < HasSegment.Num(); ++A)
				if (HasSegment[A]) SegmentMeta.Add(FIntVector4(A, A + 1, 0, 0));
		}
	}

	const double RecomputeUVStartSeconds = FPlatformTime::Seconds();
	RecomputeVineOutputUVsFromGeneratedLength(OutVertices, PathPoints, SegmentMeta, 3u, VV, OutUVs);
	const double RecomputeUVMs = (FPlatformTime::Seconds() - RecomputeUVStartSeconds) * 1000.0;

	const int32 MaterialID = 0;
	const double BuildDynamicMeshStartSeconds = FPlatformTime::Seconds();
	UDynamicMesh* VineMesh = BuildDynamicMeshFromGPUVineOutput(this, OutVertices, OutUVs, OutIndices, MaterialID, true);
	if (!VineMesh || VineMesh->GetTriangleCount() <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] GPU output produced no valid triangles."));
		return false;
	}
	const double BuildDynamicMeshMs = (FPlatformTime::Seconds() - BuildDynamicMeshStartSeconds) * 1000.0;

	const double AppendStartSeconds = FPlatformTime::Seconds();
	int32 PreviousMaxTriangleID = 0;
	ContainerMesh->ProcessMesh([&](const FDynamicMesh3& Mesh)
	{
		PreviousMaxTriangleID = Mesh.MaxTriangleID();
	});

	FGeometryScriptAppendMeshOptions AppendOptions;
	UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(
		ContainerMesh,
		VineMesh,
		FTransform::Identity,
		false,
		AppendOptions);

	ContainerMesh->EditMesh([&](FDynamicMesh3& Mesh)
	{
		if (!Mesh.HasAttributes())
		{
			Mesh.EnableAttributes();
		}

		if (!Mesh.Attributes()->HasMaterialID())
		{
			Mesh.Attributes()->EnableMaterialID();
		}

		UE::Geometry::FDynamicMeshMaterialAttribute* MaterialIDs = Mesh.Attributes()->GetMaterialID();
		for (const int32 TriangleID : Mesh.TriangleIndicesItr())
		{
			if (TriangleID >= PreviousMaxTriangleID)
			{
				MaterialIDs->SetNewValue(TriangleID, MaterialID);
			}
		}
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);

	TransformDynamicMeshToLocalSpace(ContainerMesh, MeshComponent->GetComponentTransform());
	MeshComponent->NotifyMeshUpdated();
	MeshComponent->UpdateBounds();
	MeshComponent->MarkRenderTransformDirty();
	MeshComponent->MarkRenderStateDirty();
	const double AppendMs = (FPlatformTime::Seconds() - AppendStartSeconds) * 1000.0;

	UE_LOG(LogTemp, Display,
		TEXT("[VisVineGPUTiming] tube prepareLines=%.3f ms buildGPUInput=%.3f ms dispatchReadback=%.3f ms recomputeUV=%.3f ms buildDynamicMesh=%.3f ms appendAndNotify=%.3f ms"),
		PrepareLinesMs,
		BuildGPUInputMs,
		DispatchMs,
		RecomputeUVMs,
		BuildDynamicMeshMs,
		AppendMs);

	UE_LOG(LogTemp, Log, TEXT("[VisVineGPU] Appended tube vine mesh. Lines=%d Vertices=%d Indices=%d Triangles=%d"),
		Lines.Num(),
		OutVertices.Num(),
		OutIndices.Num(),
		VineMesh->GetTriangleCount());
	return true;
}

inline void AVineContainer::Clean()
{
	TubeLines.Empty();
	TubeLineSourceScales.Empty();
	TubeLineSourceLocations.Empty();
	TubeLinePointScales.Empty();
	TubeLinePointAxes.Empty();
	CachedSurfaceTriangles = FCSTriangleMeshData();
	ClearDebugVineSplineActor();
	DynamicMeshComponent->GetDynamicMesh()->Reset();
}

void AVineContainer::ClearAttachedStaticMeshActors()
{
	TArray<AActor*> ActorsToDestroy;
	if (GeneratedStaticMeshActor)
	{
		ActorsToDestroy.Add(GeneratedStaticMeshActor);
	}

	TArray<AActor*> AttachedActors;
	GetAttachedActors(AttachedActors);
	for (AActor* AttachedActor : AttachedActors)
	{
		if (IsVineGeneratedStaticMeshActor(AttachedActor))
		{
			ActorsToDestroy.AddUnique(AttachedActor);
		}
	}

	for (AActor* ActorToDestroy : ActorsToDestroy)
	{
		if (!IsVineGeneratedStaticMeshActor(ActorToDestroy))
		{
			continue;
		}

		ActorToDestroy->Modify();
		ActorToDestroy->Destroy();
	}

	GeneratedStaticMeshActor = nullptr;
	MarkPackageDirty();
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

UDynamicMesh* AVineContainer::GenerateVines(float ExtrudeScale, bool Result)
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
		return nullptr;
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

	// 3. Prepare GPU surface voxel cache inputs.
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.EnsureTriangleCache"));
		VoxelGridSettings.VoxelSize = SC.VoxelSize;
		VoxelGridSettings.ActivationRadius = SC.VoxelSize * 8.0f;
		FCSMeshGeneratorTriangleCacheHandle TriangleCacheHandle = EnsureTriangleCacheByBox(
			TEXT("VineGenerate"),
			Center,
			Extent,
			false);
		(void)TriangleCacheHandle;
	}

	// Retain the voxel buffers for GPU consumers. The valid count stays in Counter[0].
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.PrepareBoxSceneSurfaceVoxelsGPU"));
		if (!PrepareBoxSceneSurfaceVoxelsGPU(SC.VoxelSize))
		{
			UE_LOG(LogTemp, Warning, TEXT("[GenerateVines] Failed to prepare GPU surface voxels on %s."), *GetActorNameOrLabel());
			return nullptr;
		}
	}
	CachedSurfaceTriangles = FCSTriangleMeshData();
	// Cache generation bounds for subsequent GPU visualization.
	InstanceBound = Bounds;

	// 如果只需要输出 Debug Mesh 或不需要最终结果
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.CreateContainerMesh"));
		UDynamicMesh* ContainerMesh = NewObject<UDynamicMesh>(this);
		GetDynamicMeshComponent()->SetDynamicMesh(ContainerMesh);
	}

	// 4. 执行 SpaceColonization
	// Tube Lines
	TArray<FGeometryScriptPolyPath> GeneratedTubeLines;
	TArray<float> GeneratedTubeLineScales;
	TArray<FVector> GeneratedTubeLineSourceLocations;
	TArray<FVineLinePointScaleData> GeneratedTubeLinePointScales;
	TArray<FVineLinePointAxisData> GeneratedTubeLinePointAxes;
	// Trip A: per-source GPU-resident SC output, published to TubeLineGPUBuffers for VisVine.
	TArray<TSharedPtr<FVineSCGPUBuffers>> GeneratedTubeLineGPUBuffers;
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.GenerateTubeLines"));
		for (int32 i = 0; i < TubeSourceCount; i++)
		{
			TArray<FTransform> SCSourceTransform;
			SCSourceTransform.Add(TubeSourceTransforms[i]);
				TSharedPtr<FVineSCGPUBuffers> SourceGPUBuffers = MakeShared<FVineSCGPUBuffers>();
				TArray<FSpaceColonizationLineResult> LinesFromSource = SpaceColonizationWithScalesInternal(
					SCSourceTransform, TargetTransforms, SourceGPUBuffers.Get());
				if (SourceGPUBuffers->IsValid())
				{
					GeneratedTubeLineGPUBuffers.Add(SourceGPUBuffers);
				}
			const float SourceScale = GetVineTransformScale(TubeSourceTransforms[i]);
			for (FSpaceColonizationLineResult& LineResult : LinesFromSource)
			{
				GeneratedTubeLines.Add(LineResult.Path);
				GeneratedTubeLineScales.Add(SourceScale);
				GeneratedTubeLineSourceLocations.Add(TubeSourceTransforms[i].GetLocation());
				FVineLinePointScaleData& ScaleData = GeneratedTubeLinePointScales.AddDefaulted_GetRef();
				ScaleData.Values = MoveTemp(LineResult.PointScales);
				FVineLinePointAxisData& AxisData = GeneratedTubeLinePointAxes.AddDefaulted_GetRef();
				AxisData.Values = MoveTemp(LineResult.PointAxes);
			}
		}
	}
	TubeLines = GeneratedTubeLines;
	TubeLineSourceScales = GeneratedTubeLineScales;
	TubeLineSourceLocations = GeneratedTubeLineSourceLocations;
	TubeLinePointScales = GeneratedTubeLinePointScales;
	TubeLinePointAxes = GeneratedTubeLinePointAxes;
	TubeLineGPUBuffers = MoveTemp(GeneratedTubeLineGPUBuffers);

	{
		int32 GPUSourceCount = TubeLineGPUBuffers.Num();
		int32 GPUPointTotal = 0;
		int32 GPUSegmentTotal = 0;
		for (const TSharedPtr<FVineSCGPUBuffers>& Buffers : TubeLineGPUBuffers)
		{
			if (!Buffers.IsValid()) continue;
			GPUPointTotal += Buffers->PointCount;
			GPUSegmentTotal += Buffers->SegmentCount;
		}
		UE_LOG(LogTemp, Display, TEXT("[VineSCGPUHandoff] captured sources=%d totalPoints=%d totalSegments=%d (Trip A persist)"), GPUSourceCount, GPUPointTotal, GPUSegmentTotal);
	}

	LogVineSCStageTargetTransformMatch(TEXT("Tube"), TubeLines, TargetTransforms);

	if (GPUProjectionDebug.bDrawGPUProjectionVoxelDebugPoints && GPUProjectionDebug.GPUProjectionVoxelDebugDuration > 0.0f)
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.DrawGPUProjectionVoxelDebugPoints"));
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

	// 5. 可视化
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.VisVine"));
		VisVine();
	}



	return GetDynamicMeshComponent() ? GetDynamicMeshComponent()->GetDynamicMesh() : nullptr;
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
	DynamicMeshComponent->SetHiddenInGame(true);
	ExportTransformArrayToFoliage(TargetType);
	ExportTransformArrayToFoliage(TubeType);

}

void AVineContainer::GenerateVineAction()
{
	ClearAttachedStaticMeshActors();
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

	UDynamicMesh* GeneratedMesh = GenerateVines( 50.0f, true);
	if (!GeneratedMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineContainer] GenerateVineAction produced no generated mesh on %s."), *GetActorNameOrLabel());
		return;
	}

	UDynamicMeshComponent* MeshComponent = GetDynamicMeshComponent();
	if (MeshComponent)
	{
		MeshComponent->SetHiddenInGame(false);
		MeshComponent->NotifyMeshUpdated();
		MeshComponent->UpdateBounds();
		MeshComponent->MarkRenderTransformDirty();
		MeshComponent->MarkRenderStateDirty();
	}
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
	UDynamicMeshComponent* MeshComponent = GetDynamicMeshComponent();
	UDynamicMesh* TargetMesh = MeshComponent ? MeshComponent->GetDynamicMesh() : nullptr;
	if (!TargetMesh || TargetMesh->GetTriangleCount() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineContainer] SaveStaticmesh skipped: no generated DynamicMesh on %s."), *GetActorNameOrLabel());
		return;
	}

	// 编号的生成（含随 actor 存盘）统一由基类负责，见 AComputeShaderMeshGenerator。
	EnsureGeneratorTimeCode();

	ULevel* ActorLevel = GetLevel();
	UPackage* LevelPackage = ActorLevel ? ActorLevel->GetOutermost() : nullptr;
	if (!LevelPackage)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineContainer] SaveStaticmesh failed: %s has no level package."), *GetActorNameOrLabel());
		return;
	}

	const FString LevelFolderPath = FPackageName::GetLongPackagePath(LevelPackage->GetName());
	if (LevelFolderPath.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineContainer] SaveStaticmesh failed: empty level folder path for %s."), *GetActorNameOrLabel());
		return;
	}

	const FString AssetFolderPath = UPackageTools::SanitizePackageName(LevelFolderPath / TEXT("AutoResult"));

	const FString ActorName = ObjectTools::SanitizeObjectName(GetActorNameOrLabel());
	FString AssetName = ObjectTools::SanitizeObjectName(FString::Printf(TEXT("%s_%s"), *ActorName, *LexToString(GeneratorTimeCode)));
	if (!AssetName.StartsWith(TEXT("SM_")))
	{
		AssetName = FString(TEXT("SM_")) + AssetName;
	}
	const FString AssetPathAndName = UPackageTools::SanitizePackageName(AssetFolderPath / AssetName);

	UStaticMesh* NewStaticMesh = UGeometryGeneral::SaveDynamicMeshToStaticMesh(
		TargetMesh,
		AssetPathAndName,
		MeshComponent,
		true,
		false,
		true);
	if (!NewStaticMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineContainer] SaveStaticmesh failed: could not create %s."), *AssetPathAndName);
		return;
	}

	ClearAttachedStaticMeshActors();

	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineContainer] Saved StaticMesh but could not spawn actor: invalid world on %s."), *GetActorNameOrLabel());
		return;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = this;
	SpawnParams.OverrideLevel = GetLevel();
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
#if WITH_EDITOR
	SpawnParams.InitialActorLabel = AssetName;
#endif

	AStaticMeshActor* SpawnedStaticMeshActor = World->SpawnActor<AStaticMeshActor>(
		AStaticMeshActor::StaticClass(),
		MeshComponent->GetComponentTransform(),
		SpawnParams);
	if (!SpawnedStaticMeshActor)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineContainer] Saved StaticMesh but could not spawn actor for %s."), *AssetPathAndName);
		return;
	}

	SpawnedStaticMeshActor->Modify();
	SpawnedStaticMeshActor->Tags.AddUnique(VineGeneratedStaticMeshActorTag);
	if (UStaticMeshComponent* StaticMeshComponent = SpawnedStaticMeshActor->GetStaticMeshComponent())
	{
		StaticMeshComponent->SetMobility(EComponentMobility::Movable);
		StaticMeshComponent->SetStaticMesh(NewStaticMesh);
		StaticMeshComponent->UpdateBounds();
		StaticMeshComponent->MarkRenderStateDirty();
	}
	SpawnedStaticMeshActor->AttachToActor(this, FAttachmentTransformRules::KeepWorldTransform);
	SpawnedStaticMeshActor->SetActorLabel(AssetName);
	SpawnedStaticMeshActor->MarkPackageDirty();
	GeneratedStaticMeshActor = SpawnedStaticMeshActor;
	MarkPackageDirty();
	DynamicMeshComponent->SetHiddenInGame(true);
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

// Doubles as the SC compute-shader validation switch. When true, the GPU queue path
// collects the full per-iteration debug readbacks and emits the [SpaceColonizationStep]
// logs so GPU output can be diffed against the CPU reference. When false (production),
// the debug readbacks/copy-passes are compiled out via `if constexpr` and the GPU path
// runs lean (only the Target/State0/State1 result readbacks remain).
constexpr bool bSpaceColonizationStepLogs = false;
constexpr int32 SpaceColonizationStepLogSampleCount = 6;
constexpr uint32 SpaceColonizationInvalidProposalOwner = 0xffffffffu;

// Mirrors HashUint/HashFloat01 in PCGMathCommon.ush; the CPU and GPU growth
// gates must hash identically per (node, iteration, seed) to stay comparable.
static uint32 SpaceColonizationHashUint(uint32 Value)
{
	Value ^= Value >> 16;
	Value *= 0x7feb352du;
	Value ^= Value >> 15;
	Value *= 0x846ca68bu;
	Value ^= Value >> 16;
	return Value;
}

static float SpaceColonizationHashFloat01(uint32 Value)
{
	return float(SpaceColonizationHashUint(Value) & 0x00ffffffu) / 16777215.0f;
}

static float SpaceColonizationGrowRand(int32 PointIndex, int32 IterationIndex, float Seed)
{
	// D3D ftou clamps to [0, UINT_MAX]; match it so both paths hash the same seed bits.
	const uint32 SeedBits = uint32(FMath::Clamp<double>(double(Seed) * 100000.0, 0.0, 4294967295.0));
	return SpaceColonizationHashFloat01(uint32(PointIndex) * 1664525u + uint32(IterationIndex) * 1013904223u + SeedBits);
}

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

static float ResolveSpaceColonizationOutputScale(
	int32 TargetIndex,
	const TArray<FSpaceColonizationAttribute>& SCAttributes,
	const TArray<float>& TargetPointScales,
	const TArray<float>& StartSourceScales)
{
	const float TargetPointScale = TargetPointScales.IsValidIndex(TargetIndex) ? TargetPointScales[TargetIndex] : 1.0f;
	const int32 StartId = SCAttributes.IsValidIndex(TargetIndex) ? SCAttributes[TargetIndex].Startid : -1;
	const float SourcePointScale = StartSourceScales.IsValidIndex(StartId) ? StartSourceScales[StartId] : 1.0f;
	return TargetPointScale * SourcePointScale;
}

struct FSpaceColonizationQueueDebugStats
{
	int32 TargetCount = 0;
	int32 AttractorCount = 0;
	int32 ActiveCount = 0;
	int32 StartCount = 0;
	int32 EndCount = 0;
	int32 PreSetCount = 0;
	int32 NextSetCount = 0;
	int32 InvalidPreCount = 0;
	int32 InvalidNextCount = 0;
	int32 AssociateOwnerCount = 0;
	int32 AssociateLinkCount = 0;
	int32 MaxAssociateCount = 0;
	int32 SpawnTotal = 0;
	int32 SpawnMax = 0;
	int32 BranchTotal = 0;
	int32 BranchMax = 0;
	FVector BoundsMin = FVector::ZeroVector;
	FVector BoundsMax = FVector::ZeroVector;
	FVector AveragePosition = FVector::ZeroVector;
};

struct FSpaceColonizationGrowthDebugEvent
{
	int32 SourceIndex = -1;
	int32 TargetIndex = -1;
	int32 AssociateCount = 0;
	int32 ParentSpawnAfter = 0;
	int32 ParentBranchAfter = 0;
	double MoveDistance = 0.0;
	FVector OldTargetPosition = FVector::ZeroVector;
	FVector NewTargetPosition = FVector::ZeroVector;
};

struct FSpaceColonizationCSIterationDebugSnapshot
{
	TArray<uint32> ResetProposalOwners;
	TArray<uint32> ProposalOwners;
	TArray<FVector4f> TargetPositions;
	TArray<FSpaceColonizationGPUState4> State0;
	TArray<FSpaceColonizationGPUState4> State1;
	bool bResetReadbackSucceeded = false;
	bool bProposalReadbackSucceeded = false;
	bool bStateReadbackSucceeded = false;
};

struct FSpaceColonizationCSDebugData
{
	TArray<FVector4f> InitialTargetPositions;
	TArray<FSpaceColonizationGPUState4> InitialState0;
	TArray<FSpaceColonizationGPUState4> InitialState1;
	TArray<uint32> NeighborCounts;
	TArray<FSpaceColonizationCSIterationDebugSnapshot> IterationSnapshots;
	bool bInitialReadbackSucceeded = false;
	bool bNeighborReadbackSucceeded = false;
};

static FString FormatSpaceColonizationVector(const FVector& Vector)
{
	return FString::Printf(TEXT("(%.2f, %.2f, %.2f)"), Vector.X, Vector.Y, Vector.Z);
}

static FSpaceColonizationQueueDebugStats BuildSpaceColonizationQueueDebugStats(
	const TArray<FVector>& TargetLocations,
	const TArray<FSpaceColonizationAttribute>& SCAttributes)
{
	FSpaceColonizationQueueDebugStats Stats;
	Stats.TargetCount = FMath::Min(TargetLocations.Num(), SCAttributes.Num());
	if (Stats.TargetCount <= 0)
	{
		return Stats;
	}

	FVector BoundsMin(TNumericLimits<double>::Max(), TNumericLimits<double>::Max(), TNumericLimits<double>::Max());
	FVector BoundsMax(-TNumericLimits<double>::Max(), -TNumericLimits<double>::Max(), -TNumericLimits<double>::Max());
	FVector PositionSum = FVector::ZeroVector;

	for (int32 Index = 0; Index < Stats.TargetCount; ++Index)
	{
		const FVector& Position = TargetLocations[Index];
		const FSpaceColonizationAttribute& Attribute = SCAttributes[Index];

		BoundsMin.X = FMath::Min(BoundsMin.X, Position.X);
		BoundsMin.Y = FMath::Min(BoundsMin.Y, Position.Y);
		BoundsMin.Z = FMath::Min(BoundsMin.Z, Position.Z);
		BoundsMax.X = FMath::Max(BoundsMax.X, Position.X);
		BoundsMax.Y = FMath::Max(BoundsMax.Y, Position.Y);
		BoundsMax.Z = FMath::Max(BoundsMax.Z, Position.Z);
		PositionSum += Position;

		Stats.AttractorCount += Attribute.Attractor ? 1 : 0;
		Stats.ActiveCount += Attribute.Attractor ? 0 : 1;
		Stats.StartCount += Attribute.Startpt ? 1 : 0;
		Stats.EndCount += Attribute.End ? 1 : 0;
		Stats.PreSetCount += Attribute.PrePt != -1 ? 1 : 0;
		Stats.NextSetCount += Attribute.NextPt != -1 ? 1 : 0;
		Stats.InvalidPreCount += (Attribute.PrePt < -1 || Attribute.PrePt >= Stats.TargetCount) ? 1 : 0;
		Stats.InvalidNextCount += (Attribute.NextPt < -1 || Attribute.NextPt >= Stats.TargetCount) ? 1 : 0;

		const int32 AssociateCount = Attribute.Associates.Num();
		Stats.AssociateOwnerCount += AssociateCount > 0 ? 1 : 0;
		Stats.AssociateLinkCount += AssociateCount;
		Stats.MaxAssociateCount = FMath::Max(Stats.MaxAssociateCount, AssociateCount);
		Stats.SpawnTotal += Attribute.SpawnCount;
		Stats.SpawnMax = FMath::Max(Stats.SpawnMax, Attribute.SpawnCount);
		Stats.BranchTotal += Attribute.BranchCount;
		Stats.BranchMax = FMath::Max(Stats.BranchMax, Attribute.BranchCount);
	}

	Stats.BoundsMin = BoundsMin;
	Stats.BoundsMax = BoundsMax;
	Stats.AveragePosition = PositionSum / double(Stats.TargetCount);
	return Stats;
}

static FString BuildSpaceColonizationNodeSamples(
	const TArray<FVector>& TargetLocations,
	const TArray<FSpaceColonizationAttribute>& SCAttributes,
	bool bEndSamples)
{
	const int32 TargetCount = FMath::Min(TargetLocations.Num(), SCAttributes.Num());
	FString Samples;
	int32 LoggedCount = 0;
	for (int32 Index = 0; Index < TargetCount && LoggedCount < SpaceColonizationStepLogSampleCount; ++Index)
	{
		const FSpaceColonizationAttribute& Attribute = SCAttributes[Index];
		const bool bUseSample = bEndSamples ? Attribute.End : !Attribute.Attractor;
		if (!bUseSample)
		{
			continue;
		}

		if (!Samples.IsEmpty())
		{
			Samples += TEXT(" | ");
		}
		Samples += FString::Printf(
			TEXT("#%d Pos=%s Pre=%d Next=%d Spawn=%d Branch=%d End=%d Start=%d"),
			Index,
			*FormatSpaceColonizationVector(TargetLocations[Index]),
			Attribute.PrePt,
			Attribute.NextPt,
			Attribute.SpawnCount,
			Attribute.BranchCount,
			Attribute.End ? 1 : 0,
			Attribute.Startpt ? 1 : 0);
		++LoggedCount;
	}

	return Samples.IsEmpty() ? TEXT("none") : Samples;
}

static FString BuildSpaceColonizationAssociateSamples(const TArray<FSpaceColonizationAttribute>& SCAttributes)
{
	FString Samples;
	int32 LoggedCount = 0;
	for (int32 Index = 0; Index < SCAttributes.Num() && LoggedCount < SpaceColonizationStepLogSampleCount; ++Index)
	{
		const TArray<int32>& Associates = SCAttributes[Index].Associates;
		if (Associates.Num() == 0)
		{
			continue;
		}

		FString AssociateList;
		const int32 AssociateSampleCount = FMath::Min(Associates.Num(), SpaceColonizationStepLogSampleCount);
		for (int32 SampleIndex = 0; SampleIndex < AssociateSampleCount; ++SampleIndex)
		{
			if (!AssociateList.IsEmpty())
			{
				AssociateList += TEXT(",");
			}
			AssociateList += FString::FromInt(Associates[SampleIndex]);
		}
		if (Associates.Num() > AssociateSampleCount)
		{
			AssociateList += TEXT(",...");
		}

		if (!Samples.IsEmpty())
		{
			Samples += TEXT(" | ");
		}
		Samples += FString::Printf(TEXT("#%d<=[%s]"), Index, *AssociateList);
		++LoggedCount;
	}

	return Samples.IsEmpty() ? TEXT("none") : Samples;
}

static void LogSpaceColonizationQueueState(
	const TCHAR* Version,
	const TCHAR* Phase,
	int32 IterationIndex,
	const TArray<FVector>& TargetLocations,
	const TArray<FSpaceColonizationAttribute>& SCAttributes)
{
	if (!bSpaceColonizationStepLogs)
	{
		return;
	}

	const FSpaceColonizationQueueDebugStats Stats = BuildSpaceColonizationQueueDebugStats(TargetLocations, SCAttributes);
	UE_LOG(LogTemp, Warning,
		TEXT("[SpaceColonizationStep][%s][%s][Iter=%d] Targets=%d Attractors=%d Active=%d Start=%d End=%d Pre=%d Next=%d InvalidPre=%d InvalidNext=%d SpawnTotal=%d SpawnMax=%d BranchTotal=%d BranchMax=%d AssocOwners=%d AssocLinks=%d AssocMax=%d PosAvg=%s BoundsMin=%s BoundsMax=%s"),
		Version,
		Phase,
		IterationIndex,
		Stats.TargetCount,
		Stats.AttractorCount,
		Stats.ActiveCount,
		Stats.StartCount,
		Stats.EndCount,
		Stats.PreSetCount,
		Stats.NextSetCount,
		Stats.InvalidPreCount,
		Stats.InvalidNextCount,
		Stats.SpawnTotal,
		Stats.SpawnMax,
		Stats.BranchTotal,
		Stats.BranchMax,
		Stats.AssociateOwnerCount,
		Stats.AssociateLinkCount,
		Stats.MaxAssociateCount,
		*FormatSpaceColonizationVector(Stats.AveragePosition),
		*FormatSpaceColonizationVector(Stats.BoundsMin),
		*FormatSpaceColonizationVector(Stats.BoundsMax));

	UE_LOG(LogTemp, Warning,
		TEXT("[SpaceColonizationStep][%s][%s][Iter=%d][Samples] Active=%s | Ends=%s"),
		Version,
		Phase,
		IterationIndex,
		*BuildSpaceColonizationNodeSamples(TargetLocations, SCAttributes, false),
		*BuildSpaceColonizationNodeSamples(TargetLocations, SCAttributes, true));
}

static void LogSpaceColonizationAssociates(
	const TCHAR* Version,
	const TCHAR* Phase,
	int32 IterationIndex,
	const TArray<FSpaceColonizationAttribute>& SCAttributes)
{
	if (!bSpaceColonizationStepLogs)
	{
		return;
	}

	int32 OwnerCount = 0;
	int32 LinkCount = 0;
	int32 MaxAssociateCount = 0;
	for (const FSpaceColonizationAttribute& Attribute : SCAttributes)
	{
		const int32 AssociateCount = Attribute.Associates.Num();
		OwnerCount += AssociateCount > 0 ? 1 : 0;
		LinkCount += AssociateCount;
		MaxAssociateCount = FMath::Max(MaxAssociateCount, AssociateCount);
	}

	UE_LOG(LogTemp, Warning,
		TEXT("[SpaceColonizationStep][%s][%s][Iter=%d] AssocOwners=%d AssocLinks=%d AssocMax=%d Samples=%s"),
		Version,
		Phase,
		IterationIndex,
		OwnerCount,
		LinkCount,
		MaxAssociateCount,
		*BuildSpaceColonizationAssociateSamples(SCAttributes));
}

static void LogSpaceColonizationGrowthEvents(
	const TCHAR* Version,
	const TCHAR* Phase,
	int32 IterationIndex,
	const TArray<FSpaceColonizationGrowthDebugEvent>& GrowthEvents)
{
	if (!bSpaceColonizationStepLogs)
	{
		return;
	}

	double TotalMoveDistance = 0.0;
	double MaxMoveDistance = 0.0;
	FString Samples;
	const int32 SampleCount = FMath::Min(GrowthEvents.Num(), SpaceColonizationStepLogSampleCount);
	for (int32 Index = 0; Index < GrowthEvents.Num(); ++Index)
	{
		const FSpaceColonizationGrowthDebugEvent& Event = GrowthEvents[Index];
		TotalMoveDistance += Event.MoveDistance;
		MaxMoveDistance = FMath::Max(MaxMoveDistance, Event.MoveDistance);
		if (Index < SampleCount)
		{
			if (!Samples.IsEmpty())
			{
				Samples += TEXT(" | ");
			}
			Samples += FString::Printf(
				TEXT("%d->%d Assoc=%d Move=%.2f Old=%s New=%s Spawn=%d Branch=%d"),
				Event.SourceIndex,
				Event.TargetIndex,
				Event.AssociateCount,
				Event.MoveDistance,
				*FormatSpaceColonizationVector(Event.OldTargetPosition),
				*FormatSpaceColonizationVector(Event.NewTargetPosition),
				Event.ParentSpawnAfter,
				Event.ParentBranchAfter);
		}
	}

	UE_LOG(LogTemp, Warning,
		TEXT("[SpaceColonizationStep][%s][%s][Iter=%d] GrowthCount=%d MoveTotal=%.2f MoveMax=%.2f Samples=%s"),
		Version,
		Phase,
		IterationIndex,
		GrowthEvents.Num(),
		TotalMoveDistance,
		MaxMoveDistance,
		Samples.IsEmpty() ? TEXT("none") : *Samples);
}

static void LogSpaceColonizationInput(
	const TCHAR* Version,
	int32 SourceCount,
	int32 TargetCount,
	int32 Iteration,
	int32 Activetime,
	float RandGrow,
	float Seed,
	float InfluenceRadius,
	bool bMultThread)
{
	if (!bSpaceColonizationStepLogs)
	{
		return;
	}

	UE_LOG(LogTemp, Warning,
		TEXT("[SpaceColonizationStep][%s][Input] Sources=%d Targets=%d Iterations=%d Activetime=%d RandGrow=%.3f Seed=%.3f InfluenceRadius=%.3f MultThread=%s"),
		Version,
		SourceCount,
		TargetCount,
		Iteration,
		Activetime,
		RandGrow,
		Seed,
		InfluenceRadius,
		bMultThread ? TEXT("true") : TEXT("false"));
}

static void ConvertSpaceColonizationGPUStateToAttributes(
	const TArray<FVector4f>& TargetPositionData,
	const TArray<FSpaceColonizationGPUState4>& State0Data,
	const TArray<FSpaceColonizationGPUState4>& State1Data,
	TArray<FVector>& OutTargetLocations,
	TArray<FSpaceColonizationAttribute>& OutSCAttributes)
{
	const int32 TargetCount = FMath::Min(TargetPositionData.Num(), FMath::Min(State0Data.Num(), State1Data.Num()));
	OutTargetLocations.SetNum(TargetCount);
	OutSCAttributes.SetNum(TargetCount);
	for (int32 Index = 0; Index < TargetCount; ++Index)
	{
		const FVector4f& Position = TargetPositionData[Index];
		OutTargetLocations[Index] = FVector(Position.X, Position.Y, Position.Z);

		const FSpaceColonizationGPUState4& State0 = State0Data[Index];
		const FSpaceColonizationGPUState4& State1 = State1Data[Index];
		FSpaceColonizationAttribute& Attribute = OutSCAttributes[Index];
		Attribute.Attractor = State0.X != 0;
		Attribute.End = State0.Y != 0;
		Attribute.Startpt = State0.Z != 0;
		Attribute.SpawnCount = State0.W;
		Attribute.Startid = State1.X;
		Attribute.PrePt = State1.Y;
		Attribute.NextPt = State1.Z;
		Attribute.BranchCount = State1.W;
	}
}

static void LogSpaceColonizationProposalOwners(
	const TCHAR* Version,
	const TCHAR* Phase,
	int32 IterationIndex,
	const TArray<uint32>& ProposalOwners)
{
	if (!bSpaceColonizationStepLogs)
	{
		return;
	}

	int32 ProposalCount = 0;
	FString Samples;
	for (int32 TargetIndex = 0; TargetIndex < ProposalOwners.Num(); ++TargetIndex)
	{
		// Owners are encoded as ProposerIndex + 1; 0 means "no proposal".
		const uint32 EncodedOwner = ProposalOwners[TargetIndex];
		if (EncodedOwner == 0u) continue;

		if (ProposalCount < SpaceColonizationStepLogSampleCount)
		{
			if (!Samples.IsEmpty())
			{
				Samples += TEXT(" | ");
			}
			Samples += FString::Printf(TEXT("%d<=%u"), TargetIndex, EncodedOwner - 1u);
		}
		++ProposalCount;
	}

	UE_LOG(LogTemp, Warning,
		TEXT("[SpaceColonizationStep][%s][%s][Iter=%d] ProposalCount=%d Samples=%s"),
		Version,
		Phase,
		IterationIndex,
		ProposalCount,
		Samples.IsEmpty() ? TEXT("none") : *Samples);
}

static void LogSpaceColonizationNeighborCounts(const TCHAR* Version, const TArray<uint32>& NeighborCounts)
{
	if (!bSpaceColonizationStepLogs)
	{
		return;
	}

	int32 NonZeroCount = 0;
	uint32 TotalCount = 0;
	uint32 MaxCount = 0;
	FString Samples;
	for (int32 Index = 0; Index < NeighborCounts.Num(); ++Index)
	{
		const uint32 Count = NeighborCounts[Index];
		NonZeroCount += Count > 0 ? 1 : 0;
		TotalCount += Count;
		MaxCount = FMath::Max(MaxCount, Count);
		if (Count > 0 && NonZeroCount <= SpaceColonizationStepLogSampleCount)
		{
			if (!Samples.IsEmpty())
			{
				Samples += TEXT(" | ");
			}
			Samples += FString::Printf(TEXT("#%d=%u"), Index, Count);
		}
	}

	UE_LOG(LogTemp, Warning,
		TEXT("[SpaceColonizationStep][%s][BuildNeighbors] Targets=%d NonZero=%d TotalNeighbors=%u MaxNeighbors=%u Samples=%s"),
		Version,
		NeighborCounts.Num(),
		NonZeroCount,
		TotalCount,
		MaxCount,
		Samples.IsEmpty() ? TEXT("none") : *Samples);
}

static void BuildSpaceColonizationCPUNeighbors(
	const TArray<FVector>& InitialTargetLocations,
	float InfluenceRadius,
	int32 MaxNeighborsPerTarget,
	TArray<uint32>& OutNeighborCounts,
	TArray<int32>& OutNeighborIndices)
{
	struct FNeighborCandidate
	{
		int32 Index = -1;
		double DistSq = 0.0;
	};

	const int32 TargetCount = InitialTargetLocations.Num();
	const int32 SafeMaxNeighbors = FMath::Clamp(MaxNeighborsPerTarget, 1, FMath::Max(TargetCount, 1));
	const float Radius = FMath::Max(InfluenceRadius, 1.0f);
	const double RadiusSq = double(Radius) * double(Radius);

	OutNeighborCounts.SetNumZeroed(TargetCount);
	OutNeighborIndices.Init(-1, TargetCount * SafeMaxNeighbors);

	for (int32 Index = 0; Index < TargetCount; ++Index)
	{
		const FVector& Center = InitialTargetLocations[Index];
		TArray<FNeighborCandidate> Candidates;
		Candidates.Reserve(SafeMaxNeighbors);
		for (int32 Candidate = 0; Candidate < TargetCount; ++Candidate)
		{
			if (Candidate == Index)
			{
				continue;
			}

			const double DistSq = FVector::DistSquared(InitialTargetLocations[Candidate], Center);
			if (DistSq > RadiusSq)
			{
				continue;
			}

			Candidates.Add(FNeighborCandidate{ Candidate, DistSq });
		}

		Candidates.Sort([](const FNeighborCandidate& A, const FNeighborCandidate& B)
		{
			return A.DistSq < B.DistSq;
		});

		const int32 Count = FMath::Min(Candidates.Num(), SafeMaxNeighbors);
		for (int32 NeighborOffset = 0; NeighborOffset < Count; ++NeighborOffset)
		{
			OutNeighborIndices[Index * SafeMaxNeighbors + NeighborOffset] = Candidates[NeighborOffset].Index;
		}
		OutNeighborCounts[Index] = Count;
	}
}

static void PopulateSpaceColonizationAssociatesFromNeighbors(
	const TArray<FVector>& TargetLocations,
	float InfluenceRadius,
	const TArray<uint32>& NeighborCounts,
	const TArray<int32>& NeighborIndices,
	int32 MaxNeighborsPerTarget,
	TArray<FSpaceColonizationAttribute>& SCAttributes)
{
	const int32 TargetCount = FMath::Min(TargetLocations.Num(), SCAttributes.Num());
	const int32 SafeMaxNeighbors = FMath::Max(MaxNeighborsPerTarget, 1);
	for (int32 AttractorIndex = 0; AttractorIndex < TargetCount; ++AttractorIndex)
	{
		if (!SCAttributes[AttractorIndex].Attractor)
		{
			continue;
		}

		int32 NearestSourceIndex = -1;
		double NearestDistSq = TNumericLimits<double>::Max();
		const uint32 NeighborCount = NeighborCounts.IsValidIndex(AttractorIndex)
			? FMath::Min(NeighborCounts[AttractorIndex], uint32(SafeMaxNeighbors))
			: 0u;
		const int32 NeighborBase = AttractorIndex * SafeMaxNeighbors;
		for (uint32 NeighborOffset = 0; NeighborOffset < NeighborCount; ++NeighborOffset)
		{
			const int32 NeighborIndex = NeighborIndices.IsValidIndex(NeighborBase + int32(NeighborOffset))
				? NeighborIndices[NeighborBase + int32(NeighborOffset)]
				: -1;
			if (NeighborIndex < 0 || NeighborIndex >= TargetCount || SCAttributes[NeighborIndex].Attractor)
			{
				continue;
			}

			const double DistSq = FVector::DistSquared(TargetLocations[NeighborIndex], TargetLocations[AttractorIndex]);
			if (DistSq < NearestDistSq)
			{
				NearestDistSq = DistSq;
				NearestSourceIndex = NeighborIndex;
			}
		}

		if (NearestSourceIndex != -1 && FMath::Sqrt(float(NearestDistSq)) * 1.1f < InfluenceRadius)
		{
			SCAttributes[NearestSourceIndex].Associates.Add(AttractorIndex);
		}
	}
}

static bool FindSpaceColonizationNearestAttractorFromNeighbors(
	int32 SourceIndex,
	const TArray<FVector>& TargetLocations,
	const TArray<FSpaceColonizationAttribute>& SCAttributes,
	const TArray<uint32>& NeighborCounts,
	const TArray<int32>& NeighborIndices,
	int32 MaxNeighborsPerTarget,
	int32& OutNearAttractorIndex,
	float& OutNearestDistance)
{
	OutNearAttractorIndex = -1;
	OutNearestDistance = 0.0f;

	const int32 TargetCount = FMath::Min(TargetLocations.Num(), SCAttributes.Num());
	if (SourceIndex < 0 || SourceIndex >= TargetCount)
	{
		return false;
	}

	const int32 SafeMaxNeighbors = FMath::Max(MaxNeighborsPerTarget, 1);
	const uint32 NeighborCount = NeighborCounts.IsValidIndex(SourceIndex)
		? FMath::Min(NeighborCounts[SourceIndex], uint32(SafeMaxNeighbors))
		: 0u;
	const int32 NeighborBase = SourceIndex * SafeMaxNeighbors;
	double NearestDistSq = TNumericLimits<double>::Max();
	for (uint32 NeighborOffset = 0; NeighborOffset < NeighborCount; ++NeighborOffset)
	{
		const int32 NeighborIndex = NeighborIndices.IsValidIndex(NeighborBase + int32(NeighborOffset))
			? NeighborIndices[NeighborBase + int32(NeighborOffset)]
			: -1;
		if (NeighborIndex < 0 || NeighborIndex >= TargetCount || !SCAttributes[NeighborIndex].Attractor)
		{
			continue;
		}

		const double DistSq = FVector::DistSquared(TargetLocations[NeighborIndex], TargetLocations[SourceIndex]);
		if (DistSq < NearestDistSq)
		{
			NearestDistSq = DistSq;
			OutNearAttractorIndex = NeighborIndex;
		}
	}

	if (OutNearAttractorIndex == -1)
	{
		return false;
	}

	OutNearestDistance = FMath::Sqrt(float(NearestDistSq));
	return true;
}

static void LogSpaceColonizationCSDebugData(const FSpaceColonizationCSDebugData& DebugData)
{
	if (!bSpaceColonizationStepLogs)
	{
		return;
	}

	TArray<FVector> TargetLocations;
	TArray<FSpaceColonizationAttribute> SCAttributes;
	if (DebugData.bInitialReadbackSucceeded)
	{
		ConvertSpaceColonizationGPUStateToAttributes(
			DebugData.InitialTargetPositions,
			DebugData.InitialState0,
			DebugData.InitialState1,
			TargetLocations,
			SCAttributes);
		LogSpaceColonizationQueueState(TEXT("CS"), TEXT("AfterMarkSources"), -1, TargetLocations, SCAttributes);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[SpaceColonizationStep][CS][AfterMarkSources] Readback failed."));
	}

	if (DebugData.bNeighborReadbackSucceeded)
	{
		LogSpaceColonizationNeighborCounts(TEXT("CS"), DebugData.NeighborCounts);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[SpaceColonizationStep][CS][BuildNeighbors] Readback failed."));
	}

	for (int32 IterationIndex = 0; IterationIndex < DebugData.IterationSnapshots.Num(); ++IterationIndex)
	{
		const FSpaceColonizationCSIterationDebugSnapshot& Snapshot = DebugData.IterationSnapshots[IterationIndex];
		if (Snapshot.bResetReadbackSucceeded)
		{
			LogSpaceColonizationProposalOwners(TEXT("CS"), TEXT("AfterResetProposals"), IterationIndex, Snapshot.ResetProposalOwners);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[SpaceColonizationStep][CS][AfterResetProposals][Iter=%d] Readback failed."), IterationIndex);
		}

		if (Snapshot.bProposalReadbackSucceeded)
		{
			LogSpaceColonizationProposalOwners(TEXT("CS"), TEXT("AfterProposeGrowth"), IterationIndex, Snapshot.ProposalOwners);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[SpaceColonizationStep][CS][AfterProposeGrowth][Iter=%d] Readback failed."), IterationIndex);
		}

		if (Snapshot.bStateReadbackSucceeded)
		{
			ConvertSpaceColonizationGPUStateToAttributes(
				Snapshot.TargetPositions,
				Snapshot.State0,
				Snapshot.State1,
				TargetLocations,
				SCAttributes);
			LogSpaceColonizationQueueState(TEXT("CS"), TEXT("AfterCommitGrowth"), IterationIndex, TargetLocations, SCAttributes);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[SpaceColonizationStep][CS][AfterCommitGrowth][Iter=%d] Readback failed."), IterationIndex);
		}
	}
}

template <typename ElementType>
static bool LockSpaceColonizationReadbackToArray(
	FRHIGPUBufferReadback* Readback,
	uint32 ReadbackBytes,
	int32 ElementCount,
	TArray<ElementType>& OutData)
{
	OutData.SetNumZeroed(ElementCount);
	if (ElementCount <= 0)
	{
		return true;
	}

	if (!Readback)
	{
		return false;
	}

	if (const ElementType* ReadbackPtr = static_cast<const ElementType*>(Readback->Lock(ReadbackBytes)))
	{
		FMemory::Memcpy(OutData.GetData(), ReadbackPtr, ReadbackBytes);
		Readback->Unlock();
		return true;
	}

	return false;
}

static void DeleteSpaceColonizationReadbackArray(TArray<FRHIGPUBufferReadback*>& Readbacks)
{
	for (FRHIGPUBufferReadback*& Readback : Readbacks)
	{
		delete Readback;
		Readback = nullptr;
	}
	Readbacks.Reset();
}

static void DeleteSpaceColonizationCSReadbacks(
	FRHIGPUBufferReadback*& TargetReadback,
	FRHIGPUBufferReadback*& State0Readback,
	FRHIGPUBufferReadback*& State1Readback,
	FRHIGPUBufferReadback*& InitialTargetDebugReadback,
	FRHIGPUBufferReadback*& InitialState0DebugReadback,
	FRHIGPUBufferReadback*& InitialState1DebugReadback,
	FRHIGPUBufferReadback*& NeighborCountsDebugReadback,
	TArray<FRHIGPUBufferReadback*>& ResetProposalOwnerDebugReadbacks,
	TArray<FRHIGPUBufferReadback*>& ProposalOwnerDebugReadbacks,
	TArray<FRHIGPUBufferReadback*>& IterationTargetDebugReadbacks,
	TArray<FRHIGPUBufferReadback*>& IterationState0DebugReadbacks,
	TArray<FRHIGPUBufferReadback*>& IterationState1DebugReadbacks)
{
	delete TargetReadback;
	delete State0Readback;
	delete State1Readback;
	delete InitialTargetDebugReadback;
	delete InitialState0DebugReadback;
	delete InitialState1DebugReadback;
	delete NeighborCountsDebugReadback;
	TargetReadback = nullptr;
	State0Readback = nullptr;
	State1Readback = nullptr;
	InitialTargetDebugReadback = nullptr;
	InitialState0DebugReadback = nullptr;
	InitialState1DebugReadback = nullptr;
	NeighborCountsDebugReadback = nullptr;
	DeleteSpaceColonizationReadbackArray(ResetProposalOwnerDebugReadbacks);
	DeleteSpaceColonizationReadbackArray(ProposalOwnerDebugReadbacks);
	DeleteSpaceColonizationReadbackArray(IterationTargetDebugReadbacks);
	DeleteSpaceColonizationReadbackArray(IterationState0DebugReadbacks);
	DeleteSpaceColonizationReadbackArray(IterationState1DebugReadbacks);
}

static bool AreSpaceColonizationReadbacksReady(const TArray<FRHIGPUBufferReadback*>& Readbacks)
{
	for (FRHIGPUBufferReadback* Readback : Readbacks)
	{
		if (!Readback || !Readback->IsReady())
		{
			return false;
		}
	}
	return true;
}

static bool AreSpaceColonizationCSReadbacksReady(
	FRHIGPUBufferReadback* TargetReadback,
	FRHIGPUBufferReadback* State0Readback,
	FRHIGPUBufferReadback* State1Readback,
	FRHIGPUBufferReadback* InitialTargetDebugReadback,
	FRHIGPUBufferReadback* InitialState0DebugReadback,
	FRHIGPUBufferReadback* InitialState1DebugReadback,
	FRHIGPUBufferReadback* NeighborCountsDebugReadback,
	const TArray<FRHIGPUBufferReadback*>& ResetProposalOwnerDebugReadbacks,
	const TArray<FRHIGPUBufferReadback*>& ProposalOwnerDebugReadbacks,
	const TArray<FRHIGPUBufferReadback*>& IterationTargetDebugReadbacks,
	const TArray<FRHIGPUBufferReadback*>& IterationState0DebugReadbacks,
	const TArray<FRHIGPUBufferReadback*>& IterationState1DebugReadbacks)
{
	const bool bResultReady = TargetReadback && TargetReadback->IsReady()
		&& State0Readback && State0Readback->IsReady()
		&& State1Readback && State1Readback->IsReady();
	// Production collects no debug readbacks; only the three result buffers must be ready.
	if constexpr (bSpaceColonizationStepLogs)
	{
		return bResultReady
			&& InitialTargetDebugReadback && InitialTargetDebugReadback->IsReady()
			&& InitialState0DebugReadback && InitialState0DebugReadback->IsReady()
			&& InitialState1DebugReadback && InitialState1DebugReadback->IsReady()
			&& NeighborCountsDebugReadback && NeighborCountsDebugReadback->IsReady()
			&& AreSpaceColonizationReadbacksReady(ResetProposalOwnerDebugReadbacks)
			&& AreSpaceColonizationReadbacksReady(ProposalOwnerDebugReadbacks)
			&& AreSpaceColonizationReadbacksReady(IterationTargetDebugReadbacks)
			&& AreSpaceColonizationReadbacksReady(IterationState0DebugReadbacks)
			&& AreSpaceColonizationReadbacksReady(IterationState1DebugReadbacks);
	}
	else
	{
		return bResultReady;
	}
}

static bool BuildSpaceColonizationQueueCSImpl(
	const TArray<FTransform>& SourceTransforms,
	const TArray<FTransform>& InTargetTransforms,
	int32 Iteration,
	int32 Activetime,
	float RandGrow,
	float Seed,
	float InfluenceRadius,
	int32 BackGrowCount,
	int32 ForkTaperForkOrdinal,
	float ResampleLength,
	const TArray<float>& CurveLUT,
	const TArray<float>& TargetPointScales,
	const TArray<float>& StartSourceScales,
	float ScatterDistance,
	TArray<FVector4f>& OutGPUPathPoints,
	TArray<FVector>& OutTargetLocations,
	TArray<FSpaceColonizationAttribute>& OutSCAttributes,
	FVineSCGPUBuffers* OutGPUBuffers)
{
	GV_TIME_SCOPE(TEXT("SpaceColonizationCS.Queue.Total"));
	OutGPUPathPoints.Reset();
	OutTargetLocations.Reset();
	OutSCAttributes.Reset();

	const int32 SourceCount = SourceTransforms.Num();
	const int32 TargetCount = InTargetTransforms.Num();
	LogSpaceColonizationInput(TEXT("CS"), SourceCount, TargetCount, Iteration, Activetime, RandGrow, Seed, InfluenceRadius, false);
	// Iteration <= 0 still runs Init/MarkSources so the result matches the CPU
	// path, which returns the marked queue even when the growth loop never runs.
	const int32 IterationCount = FMath::Max(Iteration, 0);
	if (SourceCount == 0 || TargetCount == 0)
	{
		return false;
	}

	{
		TArray<FVector4f> SourcePositions;
		TArray<FVector4f> InitialTargetPositions;
		{
			GV_TIME_SCOPE(TEXT("SpaceColonizationCS.Queue.PreparePositions"));
			SourcePositions.Reserve(SourceCount);
			for (const FTransform& Transform : SourceTransforms)
			{
				const FVector Location = Transform.GetLocation();
				SourcePositions.Add(FVector4f((FVector3f)Location, GetSpaceColonizationTransformScale(Transform)));
			}

			InitialTargetPositions.Reserve(TargetCount);
			for (const FTransform& Transform : InTargetTransforms)
			{
				const FVector Location = Transform.GetLocation();
				InitialTargetPositions.Add(FVector4f((FVector3f)Location, GetSpaceColonizationTransformScale(Transform)));
			}
		}

		const uint64 TargetReadbackBytes64 = sizeof(FVector4f) * uint64(TargetCount);
		const uint64 StateReadbackBytes64 = sizeof(FSpaceColonizationGPUState4) * uint64(TargetCount);
		const uint64 UIntReadbackBytes64 = sizeof(uint32) * uint64(TargetCount);
		// SC_MAX_NEIGHBORS_CAP in SpaceColonizationQueue.usf must stay >= this value.
		const int32 MaxNeighborsPerTarget = FMath::Clamp(SpaceColonizationMaxNeighborsPerTarget, 1, TargetCount);
		const uint64 NeighborIndexCount64 = uint64(TargetCount) * uint64(MaxNeighborsPerTarget);
		if (TargetReadbackBytes64 > uint64(TNumericLimits<uint32>::Max()) ||
			StateReadbackBytes64 > uint64(TNumericLimits<uint32>::Max()) ||
			UIntReadbackBytes64 > uint64(TNumericLimits<uint32>::Max()) ||
			NeighborIndexCount64 > uint64(TNumericLimits<uint32>::Max()))
		{
			UE_LOG(LogTemp, Warning, TEXT("[SpaceColonizationQueueCS] GPU request too large. TargetCount=%d MaxNeighbors=%d"), TargetCount, MaxNeighborsPerTarget);
			return false;
		}

		const uint32 TargetReadbackBytes = uint32(TargetReadbackBytes64);
		const uint32 StateReadbackBytes = uint32(StateReadbackBytes64);
		const uint32 UIntReadbackBytes = uint32(UIntReadbackBytes64);
		const uint32 NeighborIndexCount = uint32(NeighborIndexCount64);
		FRHIGPUBufferReadback* TargetReadback = new FRHIGPUBufferReadback(TEXT("SpaceColonizationQueue_TargetReadback"));
		FRHIGPUBufferReadback* State0Readback = new FRHIGPUBufferReadback(TEXT("SpaceColonizationQueue_State0Readback"));
		FRHIGPUBufferReadback* State1Readback = new FRHIGPUBufferReadback(TEXT("SpaceColonizationQueue_State1Readback"));
		// GPU line-building (Increment B) validation counters: [lineCount, totalPoints, totalSegments, 0].
		FRHIGPUBufferReadback* LineCountsReadback = new FRHIGPUBufferReadback(TEXT("SpaceColonizationQueue_LineCounts"));
		TArray<uint32> LineCountsData;
		LineCountsData.SetNumZeroed(4);
		const uint32 LineCountsReadbackBytes = uint32(sizeof(uint32) * 4);
		// Stage B2 emit-kernel output. Buffer is over-allocated to the worst-case bound
		// (each of <=TargetCount lines has <= SC_MAX_BACKTRACK+1 points); only [0,totalPoints)
		// is written compactly. Readback is capped to keep the validation copy small.
		constexpr int32 SpaceColonizationMaxBacktrack = 100;
		const uint32 PathPointCapacity = uint32(FMath::Min<int64>(int64(TargetCount) * int64(SpaceColonizationMaxBacktrack + 1), 4000000));
		const uint32 SegmentCapacity = PathPointCapacity;
		// Resample can grow lines; keep a generous compact capacity for the post-resample set.
		const uint32 PathPoint2Capacity = PathPointCapacity;
		const uint32 Segment2Capacity = PathPointCapacity;
		const uint32 PathPointsReadbackCount = FMath::Min<uint32>(PathPointCapacity, 65536u);
		const uint32 PathPointsReadbackBytes = uint32(sizeof(FVector4f) * uint64(PathPointsReadbackCount));
		FRHIGPUBufferReadback* PathPointsReadback = new FRHIGPUBufferReadback(TEXT("SpaceColonizationQueue_PathPoints"));
		TArray<FVector4f> PathPointsData;
		// Trip A: when requested, extract the prepped output buffers as pooled (external)
		// RDG buffers inside the render command below so they outlive the graph and can be
		// handed to VisVine. Assigned on the render thread; valid after FlushRenderingCommands.
		const bool bExportGPUBuffers = (OutGPUBuffers != nullptr);
		TRefCountPtr<FRDGPooledBuffer> ExportedPathPoints2;
		TRefCountPtr<FRDGPooledBuffer> ExportedPathPointMeta2;
		TRefCountPtr<FRDGPooledBuffer> ExportedSegmentMeta2;
		TArray<float> EmitTargetPointScales = TargetPointScales;
		TArray<float> EmitStartSourceScales = StartSourceScales;
		TArray<float> EmitCurveLUT = CurveLUT;
		const uint32 CurveLUTSize = uint32(CurveLUT.Num());
		// Debug readbacks exist solely to feed the [SpaceColonizationStep] validation logs.
		// In production (bSpaceColonizationStepLogs == false) they stay null and every
		// debug copy-pass / lock below is compiled out, so the GPU path only pays for the
		// three result readbacks (Target/State0/State1).
		FRHIGPUBufferReadback* InitialTargetDebugReadback = nullptr;
		FRHIGPUBufferReadback* InitialState0DebugReadback = nullptr;
		FRHIGPUBufferReadback* InitialState1DebugReadback = nullptr;
		FRHIGPUBufferReadback* NeighborCountsDebugReadback = nullptr;
		TArray<FRHIGPUBufferReadback*> ResetProposalOwnerDebugReadbacks;
		TArray<FRHIGPUBufferReadback*> ProposalOwnerDebugReadbacks;
		TArray<FRHIGPUBufferReadback*> IterationTargetDebugReadbacks;
		TArray<FRHIGPUBufferReadback*> IterationState0DebugReadbacks;
		TArray<FRHIGPUBufferReadback*> IterationState1DebugReadbacks;
		FSpaceColonizationCSDebugData CSDebugData;
		if constexpr (bSpaceColonizationStepLogs)
		{
			InitialTargetDebugReadback = new FRHIGPUBufferReadback(TEXT("SpaceColonizationQueue_Debug_InitialTarget"));
			InitialState0DebugReadback = new FRHIGPUBufferReadback(TEXT("SpaceColonizationQueue_Debug_InitialState0"));
			InitialState1DebugReadback = new FRHIGPUBufferReadback(TEXT("SpaceColonizationQueue_Debug_InitialState1"));
			NeighborCountsDebugReadback = new FRHIGPUBufferReadback(TEXT("SpaceColonizationQueue_Debug_NeighborCounts"));
			ResetProposalOwnerDebugReadbacks.Reserve(IterationCount);
			ProposalOwnerDebugReadbacks.Reserve(IterationCount);
			IterationTargetDebugReadbacks.Reserve(IterationCount);
			IterationState0DebugReadbacks.Reserve(IterationCount);
			IterationState1DebugReadbacks.Reserve(IterationCount);
			for (int32 IterationIndex = 0; IterationIndex < IterationCount; ++IterationIndex)
			{
				ResetProposalOwnerDebugReadbacks.Add(new FRHIGPUBufferReadback(*FString::Printf(TEXT("SpaceColonizationQueue_Debug_ResetOwners_%d"), IterationIndex)));
				ProposalOwnerDebugReadbacks.Add(new FRHIGPUBufferReadback(*FString::Printf(TEXT("SpaceColonizationQueue_Debug_ProposalOwners_%d"), IterationIndex)));
				IterationTargetDebugReadbacks.Add(new FRHIGPUBufferReadback(*FString::Printf(TEXT("SpaceColonizationQueue_Debug_Target_%d"), IterationIndex)));
				IterationState0DebugReadbacks.Add(new FRHIGPUBufferReadback(*FString::Printf(TEXT("SpaceColonizationQueue_Debug_State0_%d"), IterationIndex)));
				IterationState1DebugReadbacks.Add(new FRHIGPUBufferReadback(*FString::Printf(TEXT("SpaceColonizationQueue_Debug_State1_%d"), IterationIndex)));
			}
			CSDebugData.IterationSnapshots.SetNum(IterationCount);
		}
		bool bRenderWorkQueued = false;

		{
			GV_TIME_SCOPE(TEXT("SpaceColonizationCS.Queue.DispatchAndFlush"));
			ENQUEUE_RENDER_COMMAND(SpaceColonizationQueueCS)(
				[SourcePositions = MoveTemp(SourcePositions), InitialTargetPositions = MoveTemp(InitialTargetPositions),
				 TargetReadback, State0Readback, State1Readback, TargetReadbackBytes, StateReadbackBytes,
				 UIntReadbackBytes, InitialTargetDebugReadback, InitialState0DebugReadback, InitialState1DebugReadback,
				 NeighborCountsDebugReadback, ResetProposalOwnerDebugReadbacks, ProposalOwnerDebugReadbacks,
				 IterationTargetDebugReadbacks, IterationState0DebugReadbacks, IterationState1DebugReadbacks,
				 TargetCount, SourceCount, IterationCount, Activetime, RandGrow, Seed, InfluenceRadius,
				 MaxNeighborsPerTarget, NeighborIndexCount, BackGrowCount, ForkTaperForkOrdinal,
				 LineCountsReadback, LineCountsReadbackBytes,
				 EmitTargetPointScales = MoveTemp(EmitTargetPointScales), EmitStartSourceScales = MoveTemp(EmitStartSourceScales),
				 PathPointsReadback, PathPointsReadbackBytes, PathPointCapacity, SegmentCapacity,
				 ResampleLength, PathPoint2Capacity, Segment2Capacity,
				 EmitCurveLUT = MoveTemp(EmitCurveLUT), CurveLUTSize,
				 ScatterDistance,
				 bExportGPUBuffers, &ExportedPathPoints2, &ExportedPathPointMeta2, &ExportedSegmentMeta2,
				 &bRenderWorkQueued](FRHICommandListImmediate& RHICmdList)
				{
					FRDGBuilder GraphBuilder(RHICmdList);

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
					if constexpr (bSpaceColonizationStepLogs)
					{
						AddEnqueueCopyPass(GraphBuilder, InitialTargetDebugReadback, TargetBuffer, TargetReadbackBytes);
						AddEnqueueCopyPass(GraphBuilder, InitialState0DebugReadback, State0Buffer, StateReadbackBytes);
						AddEnqueueCopyPass(GraphBuilder, InitialState1DebugReadback, State1Buffer, StateReadbackBytes);
					}

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
					if constexpr (bSpaceColonizationStepLogs)
					{
						AddEnqueueCopyPass(GraphBuilder, NeighborCountsDebugReadback, NeighborCountsBuffer, UIntReadbackBytes);
					}

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
						if constexpr (bSpaceColonizationStepLogs)
						{
							AddEnqueueCopyPass(GraphBuilder, ResetProposalOwnerDebugReadbacks[IterationIndex], ProposalOwnersBuffer, UIntReadbackBytes);
						}

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
						if constexpr (bSpaceColonizationStepLogs)
						{
							AddEnqueueCopyPass(GraphBuilder, ProposalOwnerDebugReadbacks[IterationIndex], ProposalOwnersBuffer, UIntReadbackBytes);
						}

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
						if constexpr (bSpaceColonizationStepLogs)
						{
							AddEnqueueCopyPass(GraphBuilder, IterationTargetDebugReadbacks[IterationIndex], TargetBuffer, TargetReadbackBytes);
							AddEnqueueCopyPass(GraphBuilder, IterationState0DebugReadbacks[IterationIndex], State0Buffer, StateReadbackBytes);
							AddEnqueueCopyPass(GraphBuilder, IterationState1DebugReadbacks[IterationIndex], State1Buffer, StateReadbackBytes);
						}
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

					// Trip A: hand the prepped output to VisVine GPU-resident. Extract the four
					// post-resample buffers as pooled (external) so they survive graph execution.
					// Layout is byte-equivalent to BuildVVGPUInput's output.
					if (bExportGPUBuffers)
					{
						ExportedPathPoints2 = GraphBuilder.ConvertToExternalBuffer(PathPoints2Buffer);
						ExportedPathPointMeta2 = GraphBuilder.ConvertToExternalBuffer(PathPointMeta2Buffer);
						ExportedSegmentMeta2 = GraphBuilder.ConvertToExternalBuffer(SegmentMeta2Buffer);
					}

					// The resampled counts (post-resample) drive the readback slice.
					AddEnqueueCopyPass(GraphBuilder, LineCountsReadback, NewCountsBuffer, LineCountsReadbackBytes);
					AddEnqueueCopyPass(GraphBuilder, PathPointsReadback, PathPoints2Buffer, PathPointsReadbackBytes);

					AddEnqueueCopyPass(GraphBuilder, TargetReadback, TargetBuffer, TargetReadbackBytes);
					AddEnqueueCopyPass(GraphBuilder, State0Readback, State0Buffer, StateReadbackBytes);
					AddEnqueueCopyPass(GraphBuilder, State1Readback, State1Buffer, StateReadbackBytes);

					GraphBuilder.Execute();
					bRenderWorkQueued = true;
				});

			FlushRenderingCommands();
		}

		if (!bRenderWorkQueued)
		{
			DeleteSpaceColonizationCSReadbacks(
				TargetReadback,
				State0Readback,
				State1Readback,
				InitialTargetDebugReadback,
				InitialState0DebugReadback,
				InitialState1DebugReadback,
				NeighborCountsDebugReadback,
				ResetProposalOwnerDebugReadbacks,
				ProposalOwnerDebugReadbacks,
				IterationTargetDebugReadbacks,
				IterationState0DebugReadbacks,
				IterationState1DebugReadbacks);
			delete LineCountsReadback;
			LineCountsReadback = nullptr;
			delete PathPointsReadback;
			PathPointsReadback = nullptr;
			return false;
		}

		TArray<FVector4f> TargetPositionData;
		TArray<FSpaceColonizationGPUState4> State0Data;
		TArray<FSpaceColonizationGPUState4> State1Data;
		TargetPositionData.SetNumZeroed(TargetCount);
		State0Data.SetNumZeroed(TargetCount);
		State1Data.SetNumZeroed(TargetCount);
		bool bReadbackSucceeded = false;

		{
			GV_TIME_SCOPE(TEXT("SpaceColonizationCS.Queue.ReadbackAndFlush"));
			ENQUEUE_RENDER_COMMAND(SpaceColonizationQueueCSReadback)(
				[TargetReadback, State0Readback, State1Readback, InitialTargetDebugReadback, InitialState0DebugReadback, InitialState1DebugReadback,
				 NeighborCountsDebugReadback, ResetProposalOwnerDebugReadbacks, ProposalOwnerDebugReadbacks, IterationTargetDebugReadbacks,
				 IterationState0DebugReadbacks, IterationState1DebugReadbacks, TargetReadbackBytes, StateReadbackBytes, UIntReadbackBytes, LineCountsReadback, LineCountsReadbackBytes, &LineCountsData, PathPointsReadback, PathPointsReadbackBytes, &PathPointsData,
				 TargetCount, &TargetPositionData, &State0Data, &State1Data, &CSDebugData, &bReadbackSucceeded](FRHICommandListImmediate& RHICmdList) mutable
				{
					if (!TargetReadback || !State0Readback || !State1Readback)
					{
						return;
					}

					if (!AreSpaceColonizationCSReadbacksReady(
						TargetReadback,
						State0Readback,
						State1Readback,
						InitialTargetDebugReadback,
						InitialState0DebugReadback,
						InitialState1DebugReadback,
						NeighborCountsDebugReadback,
						ResetProposalOwnerDebugReadbacks,
						ProposalOwnerDebugReadbacks,
						IterationTargetDebugReadbacks,
						IterationState0DebugReadbacks,
						IterationState1DebugReadbacks))
					{
						RHICmdList.SubmitAndBlockUntilGPUIdle();
					}

					if (!AreSpaceColonizationCSReadbacksReady(
						TargetReadback,
						State0Readback,
						State1Readback,
						InitialTargetDebugReadback,
						InitialState0DebugReadback,
						InitialState1DebugReadback,
						NeighborCountsDebugReadback,
						ResetProposalOwnerDebugReadbacks,
						ProposalOwnerDebugReadbacks,
						IterationTargetDebugReadbacks,
						IterationState0DebugReadbacks,
						IterationState1DebugReadbacks))
					{
						UE_LOG(LogTemp, Warning, TEXT("[SpaceColonizationQueueCS] GPU readback was not ready after flush."));
						DeleteSpaceColonizationCSReadbacks(
							TargetReadback,
							State0Readback,
							State1Readback,
							InitialTargetDebugReadback,
							InitialState0DebugReadback,
							InitialState1DebugReadback,
							NeighborCountsDebugReadback,
							ResetProposalOwnerDebugReadbacks,
							ProposalOwnerDebugReadbacks,
							IterationTargetDebugReadbacks,
							IterationState0DebugReadbacks,
							IterationState1DebugReadbacks);
						return;
					}

					bool bLockedAll =
						LockSpaceColonizationReadbackToArray(TargetReadback, TargetReadbackBytes, TargetCount, TargetPositionData) &&
						LockSpaceColonizationReadbackToArray(State0Readback, StateReadbackBytes, TargetCount, State0Data) &&
						LockSpaceColonizationReadbackToArray(State1Readback, StateReadbackBytes, TargetCount, State1Data);

					LockSpaceColonizationReadbackToArray(LineCountsReadback, LineCountsReadbackBytes, 4, LineCountsData);
					LockSpaceColonizationReadbackToArray(PathPointsReadback, PathPointsReadbackBytes, int32(PathPointsReadbackBytes / sizeof(FVector4f)), PathPointsData);

					CSDebugData.bInitialReadbackSucceeded =
						LockSpaceColonizationReadbackToArray(InitialTargetDebugReadback, TargetReadbackBytes, TargetCount, CSDebugData.InitialTargetPositions) &&
						LockSpaceColonizationReadbackToArray(InitialState0DebugReadback, StateReadbackBytes, TargetCount, CSDebugData.InitialState0) &&
						LockSpaceColonizationReadbackToArray(InitialState1DebugReadback, StateReadbackBytes, TargetCount, CSDebugData.InitialState1);
					CSDebugData.bNeighborReadbackSucceeded =
						LockSpaceColonizationReadbackToArray(NeighborCountsDebugReadback, UIntReadbackBytes, TargetCount, CSDebugData.NeighborCounts);

					const int32 SnapshotCount = FMath::Min(CSDebugData.IterationSnapshots.Num(), IterationTargetDebugReadbacks.Num());
					for (int32 IterationIndex = 0; IterationIndex < SnapshotCount; ++IterationIndex)
					{
						FSpaceColonizationCSIterationDebugSnapshot& Snapshot = CSDebugData.IterationSnapshots[IterationIndex];
						Snapshot.bResetReadbackSucceeded = LockSpaceColonizationReadbackToArray(
							ResetProposalOwnerDebugReadbacks[IterationIndex],
							UIntReadbackBytes,
							TargetCount,
							Snapshot.ResetProposalOwners);
						Snapshot.bProposalReadbackSucceeded = LockSpaceColonizationReadbackToArray(
							ProposalOwnerDebugReadbacks[IterationIndex],
							UIntReadbackBytes,
							TargetCount,
							Snapshot.ProposalOwners);
						Snapshot.bStateReadbackSucceeded =
							LockSpaceColonizationReadbackToArray(
								IterationTargetDebugReadbacks[IterationIndex],
								TargetReadbackBytes,
								TargetCount,
								Snapshot.TargetPositions) &&
							LockSpaceColonizationReadbackToArray(
								IterationState0DebugReadbacks[IterationIndex],
								StateReadbackBytes,
								TargetCount,
								Snapshot.State0) &&
							LockSpaceColonizationReadbackToArray(
								IterationState1DebugReadbacks[IterationIndex],
								StateReadbackBytes,
								TargetCount,
								Snapshot.State1);
					}

					DeleteSpaceColonizationCSReadbacks(
						TargetReadback,
						State0Readback,
						State1Readback,
						InitialTargetDebugReadback,
						InitialState0DebugReadback,
						InitialState1DebugReadback,
						NeighborCountsDebugReadback,
						ResetProposalOwnerDebugReadbacks,
						ProposalOwnerDebugReadbacks,
						IterationTargetDebugReadbacks,
						IterationState0DebugReadbacks,
						IterationState1DebugReadbacks);
					bReadbackSucceeded = bLockedAll;
				});

			FlushRenderingCommands();
		}

		// GPU line-building (Increment B, Stage B1) validation: the GPU tree walk
		// must produce the SAME line/point counts as the CPU tracer (anti-web check).
		UE_LOG(LogTemp, Display, TEXT("[SpaceColonizationLinesCS] GPU lineCount=%u totalPoints=%u totalSegments=%u (Targets=%d)"),
			LineCountsData.IsValidIndex(0) ? LineCountsData[0] : 0u,
			LineCountsData.IsValidIndex(1) ? LineCountsData[1] : 0u,
			LineCountsData.IsValidIndex(2) ? LineCountsData[2] : 0u,
			TargetCount);
		delete LineCountsReadback;
		LineCountsReadback = nullptr;

		// Slice the emit readback to the compact [0, totalPoints) range for validation.
		{
			const uint32 GPUTotalPoints = LineCountsData.IsValidIndex(1) ? LineCountsData[1] : 0u;
			const int32 CopyCount = FMath::Min<int32>(int32(GPUTotalPoints), PathPointsData.Num());
			OutGPUPathPoints.Reset();
			OutGPUPathPoints.Append(PathPointsData.GetData(), FMath::Max(CopyCount, 0));
			if (int32(GPUTotalPoints) > PathPointsData.Num())
			{
				UE_LOG(LogTemp, Warning, TEXT("[SpaceColonizationEmitCS] totalPoints=%u exceeds readback cap=%d; checksum is partial."), GPUTotalPoints, PathPointsData.Num());
			}
		}
		delete PathPointsReadback;
		PathPointsReadback = nullptr;

		if (!bReadbackSucceeded)
		{
			return false;
		}

		{
			GV_TIME_SCOPE(TEXT("SpaceColonizationCS.Queue.CopyResults"));
			ConvertSpaceColonizationGPUStateToAttributes(TargetPositionData, State0Data, State1Data, OutTargetLocations, OutSCAttributes);
		}

		// Trip A: publish the GPU-resident prepped buffers + their compact counts.
		if (OutGPUBuffers)
		{
			OutGPUBuffers->PathPoints = MoveTemp(ExportedPathPoints2);
			OutGPUBuffers->PathPointMeta = MoveTemp(ExportedPathPointMeta2);
			OutGPUBuffers->SegmentMeta = MoveTemp(ExportedSegmentMeta2);
			OutGPUBuffers->LineCount = LineCountsData.IsValidIndex(0) ? int32(LineCountsData[0]) : 0;
			OutGPUBuffers->PointCount = LineCountsData.IsValidIndex(1) ? int32(LineCountsData[1]) : 0;
			OutGPUBuffers->SegmentCount = LineCountsData.IsValidIndex(2) ? int32(LineCountsData[2]) : 0;
		}

		LogSpaceColonizationCSDebugData(CSDebugData);
		return true;
	}

}

} // anonymous namespace

// ---- SpaceColonization member functions (moved from UGenerateVines, params from SC) ----

// Trip A: GPU port of ApplyVVSCPointOffset. Point jitter distance (cm) applied before the
// prep smooth on the GPU SC path, for visual parity with the CPU path. 0 disables scatter.
static TAutoConsoleVariable<float> CVarVineSCScatter(
	TEXT("r.Vine.SC.Scatter"),
	10.0f,
	TEXT("Vine GPU SC point-scatter distance in cm (ApplyVVSCPointOffset parity). 0 = off."),
	ECVF_Default);

TArray<FSpaceColonizationLineResult> AVineContainer::SpaceColonizationWithScales(TArray<FTransform> SourceTransforms, TArray<FTransform> TargetTransforms, bool /*bUseComputeShader*/)
{
	// The vine is fully GPU now; the legacy bUseComputeShader selector is ignored and the GPU
	// path emits its line geometry directly into GPU buffers (there are no CPU line results).
	return SpaceColonizationWithScalesInternal(MoveTemp(SourceTransforms), MoveTemp(TargetTransforms), nullptr);
}

TArray<FSpaceColonizationLineResult> AVineContainer::SpaceColonizationWithScalesInternal(TArray<FTransform> SourceTransforms, TArray<FTransform> TargetTransforms, FVineSCGPUBuffers* OutGPUBuffers)
{
	GV_TIME_SCOPE(TEXT("SpaceColonization.TotalCS"));
	TArray<FVector> TargetLocations;
	TArray<FSpaceColonizationAttribute> SCAttributes;
	TArray<float> TargetPointScales;
	TArray<float> StartSourceScales;
	BuildSpaceColonizationScaleLookups(SourceTransforms, TargetTransforms, TargetPointScales, StartSourceScales);
	// Ensure the profile curve exists BEFORE baking the LUT. On a first generate (CurveControl
	// still null) the LUT would otherwise bake flat (EvaluateVineScale(null)=1.0) and the mesh
	// would lose all thickness variation until the next generate rebuilt it.
	if (VV.CurveControl == nullptr)
	{
		VV.CurveControl = NewObject<UCurveLinearColor>(this);
	}
	// Bake CurveControl.G into a LUT so the GPU CurveScale matches EvaluateVineScale.
	TArray<float> CurveLUT;
	{
		const int32 LUTSize = 256;
		CurveLUT.SetNumUninitialized(LUTSize);
		for (int32 LUTIndex = 0; LUTIndex < LUTSize; ++LUTIndex)
		{
			CurveLUT[LUTIndex] = EvaluateVineScale(VV.CurveControl, LUTIndex, LUTSize);
		}
	}
	// The GPU space-colonization pass emits the fully smoothed / resampled / scaled line geometry
	// straight into OutGPUBuffers, which the fused VisVine path consumes. No CPU tree re-trace.
	TArray<FVector4f> GPUPathPoints;
	if (!BuildSpaceColonizationQueueCSImpl(
		SourceTransforms,
		TargetTransforms,
		SC.Iteration,
		SC.Activetime,
		SC.RandGrow,
		SC.Seed,
		SC.InfluenceRadius,
		SC.BackGrowCount,
		SC.ForkTaperForkOrdinal,
		FMath::Max(VV.ResampleLength, 0.01f),
		CurveLUT,
		TargetPointScales,
		StartSourceScales,
		FMath::Max(CVarVineSCScatter.GetValueOnGameThread(), 0.0f),
		GPUPathPoints,
		TargetLocations,
		SCAttributes,
		OutGPUBuffers))
	{
		return {};
	}

	return {};
}
