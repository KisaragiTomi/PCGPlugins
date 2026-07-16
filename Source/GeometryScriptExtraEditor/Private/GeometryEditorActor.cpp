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
#include "ComputeShaderGenerateHepler.h"
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
#include "ComputeShaderGenerateHepler.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "ShaderParameterStruct.h"
#include "Misc/PackageName.h"

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

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPoints)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, PathPointMeta)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPointCurveU)
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
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, RW_PathPointCurveU2)
		SHADER_PARAMETER(uint32, CurveLUTSize)
		SHADER_PARAMETER(float, UVScaleInfluence)
		SHADER_PARAMETER(float, UVScaleFloor)
		SHADER_PARAMETER(float, UVScalePower)
		SHADER_PARAMETER(float, UVLengthScale)
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

// Trip A concat: verbatim copy of a float structured buffer (PathPointCurveU) into the
// concatenated slice, via compute (the byte-level AddCopyBufferPass zeroed the 4-byte float).
class FConcatCopyFloatCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FConcatCopyFloatCS);
	SHADER_USE_PARAMETER_STRUCT(FConcatCopyFloatCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, ConcatSrcFloat)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, RW_ConcatDstFloat)
		SHADER_PARAMETER(uint32, ConcatFloatCount)
		SHADER_PARAMETER(uint32, ConcatFloatDstBase)
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
IMPLEMENT_GLOBAL_SHADER(FConcatCopyFloatCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "ConcatCopyFloatCS", SF_Compute);


// Trip A: the SC solve's fully-prepped output kept GPU-resident (pooled), so the
// VisVine voxel path can consume it directly instead of reading it back to CPU,
// re-tracing the lines, and re-deriving path points a third time on the CPU.
// Buffer layout matches BuildVVGPUInput's output exactly:
//   PathPoints      float4(xyz, finalScale = CurveScale * pointScale)
//   PathPointMeta   int4(Prev, Next, Base, Count)
//   PathPointCurveU float (scale-weighted arc U)
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
	TRefCountPtr<FRDGPooledBuffer> PathPointCurveU;
	TRefCountPtr<FRDGPooledBuffer> SegmentMeta;
	// CurveU carried as a CPU array (read back from the solve). A GPU-resident 4-byte float
	// structured buffer read back as zero through the extract->register->SRV pooled path
	// (the 16-byte float4/int4 buffers work), so CurveU takes the small readback + upload
	// route instead. Aligned 1:1 with the concatenated points.
	TArray<float> CurveUCPU;
	int32 PointCount = 0;
	int32 SegmentCount = 0;
	int32 LineCount = 0;
	bool IsValid() const { return PathPoints.IsValid() && PointCount > 0 && SegmentCount > 0; }
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

static float GetSanitizedVineUVScale(float Scale)
{
	return FMath::IsFinite(Scale) ? FMath::Max(Scale, 0.0f) : 0.0f;
}

static void BuildVineScaleWeightedCurveUFromSegmentLengths(
	const TArray<float>& SegmentLengths,
	const TArray<float>& PointScales,
	const FVV& VV,
	TArray<float>& OutCurveU)
{
	OutCurveU.Reset();

	const int32 PointCount = PointScales.Num();
	if (PointCount <= 0)
	{
		return;
	}

	OutCurveU.SetNumZeroed(PointCount);
	if (PointCount == 1)
	{
		return;
	}

	float MaxScale = 1.0e-8f;
	for (const float Scale : PointScales)
	{
		MaxScale = FMath::Max(MaxScale, GetSanitizedVineUVScale(Scale));
	}

	const float Influence = FMath::Clamp(VV.UVScaleInfluence, 0.0f, 1.0f);
	const float ScaleFloor = FMath::Clamp(VV.UVScaleFloor, 0.001f, 1.0f);
	const float ScalePower = FMath::Clamp(VV.UVScalePower, 0.0f, 4.0f);
	const float LengthScale = FMath::Max(VV.UVLengthScale, 1.0e-8f);

	for (int32 PointIndex = 1; PointIndex < PointCount; ++PointIndex)
	{
		const float SegmentLength = SegmentLengths.IsValidIndex(PointIndex - 1) ? FMath::Max(SegmentLengths[PointIndex - 1], 0.0f) : 0.0f;
		const float PrevScale = FMath::Max(GetSanitizedVineUVScale(PointScales[PointIndex - 1]) / MaxScale, ScaleFloor);
		const float CurrScale = FMath::Max(GetSanitizedVineUVScale(PointScales[PointIndex]) / MaxScale, ScaleFloor);
		const float AverageScale = FMath::Max((PrevScale + CurrScale) * 0.5f, 1.0e-8f);
		const float InverseScale = 1.0f / FMath::Pow(AverageScale, ScalePower);
		const float Weight = (1.0f - Influence) + Influence * InverseScale;
		OutCurveU[PointIndex] = OutCurveU[PointIndex - 1] + SegmentLength * FMath::Max(Weight, 1.0e-8f) * LengthScale;
	}
}

static void BuildVineScaleWeightedCurveUFromPoints(
	const TArray<FVector>& Points,
	const TArray<float>& PointScales,
	const FVV& VV,
	TArray<float>& OutCurveU)
{
	const int32 PointCount = Points.Num();
	if (PointCount <= 0)
	{
		OutCurveU.Reset();
		return;
	}

	TArray<float> SafePointScales;
	SafePointScales.SetNumUninitialized(PointCount);
	for (int32 PointIndex = 0; PointIndex < PointCount; ++PointIndex)
	{
		SafePointScales[PointIndex] = PointScales.IsValidIndex(PointIndex) ? PointScales[PointIndex] : 1.0f;
	}

	TArray<float> SegmentLengths;
	SegmentLengths.Reserve(FMath::Max(PointCount - 1, 0));
	for (int32 PointIndex = 1; PointIndex < PointCount; ++PointIndex)
	{
		SegmentLengths.Add(float(FVector::Dist(Points[PointIndex - 1], Points[PointIndex])));
	}

	BuildVineScaleWeightedCurveUFromSegmentLengths(SegmentLengths, SafePointScales, VV, OutCurveU);
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

static bool GetVineSurfaceVoxelCenterForGPUProjection(const FCSSurfaceVoxelData& VoxelData, int32 Index, float SafeVoxelSize, FVector& OutVoxelCenter)
{
	if (VoxelData.Positions.IsValidIndex(Index) && IsFiniteVector(VoxelData.Positions[Index]))
	{
		OutVoxelCenter = VoxelData.Positions[Index];
		return true;
	}

	if (!VoxelData.Cells.IsValidIndex(Index))
	{
		return false;
	}

	const FIntVector& Cell = VoxelData.Cells[Index];
	OutVoxelCenter = FVector(
		(double(Cell.X) + 0.5) * SafeVoxelSize + VoxelData.VoxelOrigin.X,
		(double(Cell.Y) + 0.5) * SafeVoxelSize + VoxelData.VoxelOrigin.Y,
		(double(Cell.Z) + 0.5) * SafeVoxelSize + VoxelData.VoxelOrigin.Z);
	return IsFiniteVector(OutVoxelCenter);
}

static int32 DrawVineGPUProjectionVoxelDebugPoints(
	UWorld* World,
	const FCSSurfaceVoxelData& VoxelData,
	float FallbackVoxelSize,
	float Duration,
	int32 PointLimit,
	bool bPersistent,
	float CenterPointSize,
	float TargetPointSize,
	const FLinearColor& CenterColor,
	const FLinearColor& TargetColor)
{
	if (!World || VoxelData.Cells.Num() == 0)
	{
		return 0;
	}

	const int32 VoxelCount = VoxelData.Cells.Num();
	const int32 DrawLimit = PointLimit > 0 ? FMath::Min(PointLimit, VoxelCount) : VoxelCount;
	const float SafeVoxelSize = FMath::Max(VoxelData.VoxelSize > 0.0f ? VoxelData.VoxelSize : FallbackVoxelSize, UE_KINDA_SMALL_NUMBER);
	const double MaxTargetDistanceSq = FMath::Square(double(SafeVoxelSize * 2.0f));
	const float SafeDuration = FMath::Max(Duration, 0.0f);
	const FColor DrawCenterColor = CenterColor.ToFColor(true);
	const FColor DrawTargetColor = TargetColor.ToFColor(true);

	int32 DrawnCount = 0;
	int32 InvalidCenterCount = 0;
	int32 InvalidTargetCount = 0;
	for (int32 Index = 0; Index < DrawLimit; ++Index)
	{
		FVector VoxelCenter;
		if (!GetVineSurfaceVoxelCenterForGPUProjection(VoxelData, Index, SafeVoxelSize, VoxelCenter))
		{
			++InvalidCenterCount;
			continue;
		}

		FVector Target = VoxelData.TargetPositions.IsValidIndex(Index) ? VoxelData.TargetPositions[Index] : VoxelCenter;
		if (!IsFiniteVector(Target) || FVector::DistSquared(Target, VoxelCenter) > MaxTargetDistanceSq)
		{
			Target = VoxelCenter;
			++InvalidTargetCount;
		}

		DrawDebugPoint(
			World,
			VoxelCenter,
			CenterPointSize,
			DrawCenterColor,
			bPersistent,
			SafeDuration,
			0);

		DrawDebugPoint(
			World,
			Target,
			TargetPointSize,
			DrawTargetColor,
			bPersistent,
			SafeDuration,
			0);

		++DrawnCount;
	}

	UE_LOG(LogTemp, Display,
		TEXT("[VisVineGPU_VoxelDebug] Drawn GPU projection voxels. Drawn=%d Limit=%d Voxels=%d Positions=%d Targets=%d Cells=%d InvalidCenters=%d InvalidTargets=%d VoxelSize=%.3f"),
		DrawnCount,
		PointLimit,
		VoxelCount,
		VoxelData.Positions.Num(),
		VoxelData.TargetPositions.Num(),
		VoxelData.Cells.Num(),
		InvalidCenterCount,
		InvalidTargetCount,
		SafeVoxelSize);

	return DrawnCount;
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

static bool BuildVVGPUInput(
	const TArray<FGeometryScriptPolyPath>& Lines,
	const FVV& VV,
	const TArray<float>& LineSourceScales,
	const TArray<FVineLinePointScaleData>& LinePointScales,
	const TArray<FVineLinePointAxisData>& LinePointAxes,
	TArray<FVector4f>& OutPathPoints,
	TArray<FVector4f>& OutPathPointAxes,
	TArray<FIntVector4>& OutPathPointMeta,
	TArray<float>& OutPathPointCurveU,
	TArray<FIntVector4>& OutSegmentMeta)
{
	OutPathPoints.Reset();
	OutPathPointAxes.Reset();
	OutPathPointMeta.Reset();
	OutPathPointCurveU.Reset();
	OutSegmentMeta.Reset();

	int32 LineIndex = 0;
	for (const FGeometryScriptPolyPath& Line : Lines)
	{
		if (!Line.Path.IsValid() || Line.Path->Num() < 2)
		{
			++LineIndex;
			continue;
		}

		// PathPoints.w is the final profile multiplier:
		// CurveControl.G * SpaceColonization point scale (TargetPointScale * SourcePointScale).
		const float FallbackPointScale = LineSourceScales.IsValidIndex(LineIndex) ? LineSourceScales[LineIndex] : 1.0f;
		const TArray<float>* PointScales = LinePointScales.IsValidIndex(LineIndex) ? &LinePointScales[LineIndex].Values : nullptr;
		const TArray<FVector>* PointAxes = LinePointAxes.IsValidIndex(LineIndex) ? &LinePointAxes[LineIndex].Values : nullptr;

		const TArray<FVector>& Points = *Line.Path;
		const int32 BaseIndex = OutPathPoints.Num();
		const int32 PointCount = Points.Num();

		TArray<float> FinalPointScales;
		FinalPointScales.SetNumUninitialized(PointCount);
		for (int32 PointIndex = 0; PointIndex < PointCount; ++PointIndex)
		{
			const float CurveScale = EvaluateVineScale(VV.CurveControl, PointIndex, PointCount);
			const float PointScale = PointScales && PointScales->IsValidIndex(PointIndex) ? (*PointScales)[PointIndex] : FallbackPointScale;
			FinalPointScales[PointIndex] = CurveScale * FMath::Max(PointScale, 0.0f);
		}

		TArray<float> LineCurveU;
		BuildVineScaleWeightedCurveUFromPoints(Points, FinalPointScales, VV, LineCurveU);

		for (int32 PointIndex = 0; PointIndex < PointCount; ++PointIndex)
		{
			const FVector& Point = Points[PointIndex];

			const float Scale = FinalPointScales[PointIndex];
			const FVector PointAxis = PointAxes && PointAxes->IsValidIndex(PointIndex) ? NormalizeVineAxisOrFallback((*PointAxes)[PointIndex]) : FVector::ZeroVector;
			OutPathPoints.Add(FVector4f(float(Point.X), float(Point.Y), float(Point.Z), Scale));
			OutPathPointAxes.Add(FVector4f(float(PointAxis.X), float(PointAxis.Y), float(PointAxis.Z), 0.0f));
			OutPathPointCurveU.Add(LineCurveU.IsValidIndex(PointIndex) ? LineCurveU[PointIndex] : 0.0f);

			const int32 PrevIndex = BaseIndex + FMath::Max(PointIndex - 1, 0);
			const int32 NextIndex = BaseIndex + FMath::Min(PointIndex + 1, PointCount - 1);
			OutPathPointMeta.Add(FIntVector4(PrevIndex, NextIndex, BaseIndex, PointCount));

			if (PointIndex + 1 < PointCount)
			{
				OutSegmentMeta.Add(FIntVector4(BaseIndex + PointIndex, BaseIndex + PointIndex + 1, 0, 0));
			}
		}

		++LineIndex;
	}

	return OutPathPoints.Num() > 0 && OutSegmentMeta.Num() > 0;
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

// ============================================================================
// CPU 接力：在 GPU 跑完 N/A/P/FP 之后，由 CPU 执行 RS/B/FT/C 四个 pass。
// 全部用 float（FVector3f/4f）以与 shader 行为对齐。逻辑逐 pass 对照 VVVoxel.usf。
// ============================================================================
namespace VineCPUTail
{
	static FVector3f SafeNormalize3(const FVector3f& V, const FVector3f& Fallback)
	{
		const float LenSq = V.SizeSquared();
		if (LenSq <= 1.0e-8f) return Fallback;
		return V * FMath::InvSqrt(LenSq);
	}

	static FVector3f GetFallbackFrameNormal(const FVector3f& Tangent)
	{
		const FVector3f Up = FMath::Abs(Tangent.Z) < 0.9f ? FVector3f(0.f, 0.f, 1.f) : FVector3f(0.f, 1.f, 0.f);
		return SafeNormalize3(Up - Tangent * FVector3f::DotProduct(Up, Tangent), FVector3f(0.f, 1.f, 0.f));
	}

	static FVector3f OrientAxisToReference(const FVector3f& Axis, const FVector3f& Reference)
	{
		return FVector3f::DotProduct(Axis, Reference) < 0.f ? -Axis : Axis;
	}

	static FVector3f BuildFrameNormalFromTangent(const FVector3f& Tangent, const FVector3f& Normal)
	{
		const FVector3f Fallback = GetFallbackFrameNormal(Tangent);
		return SafeNormalize3(Normal - Tangent * FVector3f::DotProduct(Normal, Tangent), Fallback);
	}

	// 把 V 按"将单位向量 FromDir 最小旋转到单位向量 ToDir"的旋转作用一次（Rodrigues）。
	// 用于沿曲线平行传输 roll 轴，使截面帧以最小扭曲跟随曲线。
	static FVector3f RotateVectorByAlign(const FVector3f& V, const FVector3f& FromDir, const FVector3f& ToDir)
	{
		const FVector3f Axis = FVector3f::CrossProduct(FromDir, ToDir);
		const float AxisLenSq = Axis.SizeSquared();
		const float CosA = FVector3f::DotProduct(FromDir, ToDir);
		if (AxisLenSq < 1.0e-12f)
		{
			return CosA < 0.f ? -V : V;
		}
		const float AxisLen = FMath::Sqrt(AxisLenSq);
		const FVector3f K = Axis / AxisLen;
		const float SinA = AxisLen;
		return V * CosA + FVector3f::CrossProduct(K, V) * SinA + K * FVector3f::DotProduct(K, V) * (1.f - CosA);
	}

	static void BuildOrthonormalFrame(const FVector3f& Tangent, const FVector3f& Normal,
		FVector3f& OutTangent, FVector3f& OutNormal, FVector3f& OutBinormal)
	{
		OutTangent = SafeNormalize3(Tangent, FVector3f(1.f, 0.f, 0.f));
		OutNormal = BuildFrameNormalFromTangent(OutTangent, Normal);
		OutBinormal = FVector3f::CrossProduct(OutNormal, OutTangent);
		if (OutBinormal.SizeSquared() <= 1.0e-8f)
		{
			OutBinormal = FVector3f::CrossProduct(GetFallbackFrameNormal(OutTangent), OutTangent);
		}
		OutBinormal = SafeNormalize3(OutBinormal, FVector3f(0.f, 1.f, 0.f));
		OutNormal = SafeNormalize3(FVector3f::CrossProduct(OutTangent, OutBinormal), OutNormal);
	}

	// 等价 shader GetLinePointIndex：把 PointIndex+Offset 夹在本条 vine 的 [Base, Last] 内。
	static int32 GetLinePointIndex(const TArray<FIntVector4>& Meta, int32 PointCount, int32 PointIndex, int32 Offset)
	{
		const FIntVector4& M = Meta[PointIndex];
		const int32 BaseIndex = FMath::Max(M.Z, 0);
		const int32 LineCount = FMath::Max(M.W, 1);
		const int32 LastIndex = FMath::Min(BaseIndex + LineCount - 1, PointCount - 1);
		return FMath::Min(FMath::Max(PointIndex + Offset, BaseIndex), LastIndex);
	}

	// 等价 shader HashPointJitter：基于点索引与世界坐标的确定性 [0,1) 抖动。
	static float HashPointJitter(uint32 PointIndex, const FVector3f& Position)
	{
		const float fi = float(PointIndex);
		const float s = FMath::Sin(fi * 127.1f + FVector3f::DotProduct(Position, FVector3f(269.5f, 183.3f, 397.7f))) * 43758.5453f;
		return s - FMath::FloorToFloat(s);
	}

	static FVector2f GetProfilePoint(uint32 ProfileIndex, uint32 ProfileCount, bool bTube, float CircleScale, float LineScale)
	{
		if (bTube)
		{
			// 与 GPU 端 GetProfilePoint 对齐：半径 10、角度 = 2π·i/ProfileCount 的程序化圆周采样。
			// ProfileCount==3 还原原始三角形。
			const float Angle = 6.2831853072f * float(ProfileIndex) / float(FMath::Max(ProfileCount, 1u));
			return FVector2f(FMath::Cos(Angle), FMath::Sin(Angle)) * 10.f * CircleScale;
		}
		return FVector2f(ProfileIndex == 0u ? -5.f : 5.f, 0.f) * LineScale;
	}
	// Pass RS：重采样每条 vine 的内部点（端点锚定），点数不变。
	// TargetDistance > 0 时按「相邻点三维直线距离尽量不小于 D」贪心放点，折线不够长则后续点钳制到末端端点；
	// TargetDistance <= 0 时回退到旧的等弧长均匀分布。
	static void RunResampleSurface(
		const TArray<FVector4f>& Noised, const TArray<FIntVector4>& Meta,
		const TArray<FVector4f>& InTarget, const TArray<FVector4f>& InNormal,
		float TargetDistance,
		TArray<FVector4f>& OutTarget, TArray<FVector4f>& OutNormal)
	{
		const int32 N = InTarget.Num();
		OutTarget = InTarget;
		OutNormal = InNormal;
		for (int32 i = 0; i < N; ++i)
		{
			const FVector4f CenterNormal = InNormal[i];
			const FIntVector4& M = Meta[i];
			const int32 BaseIndex = FMath::Max(M.Z, 0);
			const int32 PointCount = FMath::Max(M.W, 1);
			const int32 LocalIndex = i - BaseIndex;
			const int32 LastLocal = PointCount - 1;
			const int32 LastGlobal = N - 1;
			if (PointCount < 3 || LocalIndex <= 0 || LocalIndex >= LastLocal) continue;

			auto SamplePos = [&](int32 GlobalIdx) -> FVector3f
			{
				const FVector4f T = InTarget[GlobalIdx];
				return T.W > 0.5f ? FVector3f(T.X, T.Y, T.Z) : FVector3f(Noised[GlobalIdx].X, Noised[GlobalIdx].Y, Noised[GlobalIdx].Z);
			};

			if (TargetDistance > 1.0e-4f)
			{
				const float D = TargetDistance;
				const int32 Idx0 = FMath::Min(BaseIndex, LastGlobal);
				FVector3f Anchor = SamplePos(Idx0);

				FVector3f ResultPos = Anchor;
				FVector3f ResultNormal(InNormal[Idx0].X, InNormal[Idx0].Y, InNormal[Idx0].Z);
				float ResultW = InTarget[Idx0].W;

				int32 Placed = 0;
				bool bFound = false;

				FVector3f SegStart = Anchor;
				FVector4f NrmStart = InNormal[Idx0];
				FVector4f TgtStart = InTarget[Idx0];

				for (int32 s = 1; s <= LastLocal && !bFound; ++s)
				{
					const int32 IdxNext = FMath::Min(BaseIndex + s, LastGlobal);
					const FVector4f TgtNext = InTarget[IdxNext];
					const FVector4f NrmNext = InNormal[IdxNext];
					const FVector3f SegEnd = SamplePos(IdxNext);

					const FVector3f d = SegEnd - SegStart;
					const float a = FVector3f::DotProduct(d, d);
					float u0 = 0.f;

					for (int32 guard = 0; guard < PointCount; ++guard)
					{
						if (a <= 1.0e-12f) break;
						const FVector3f e = SegStart - Anchor;
						const float bb = 2.f * FVector3f::DotProduct(d, e);
						const float cc = FVector3f::DotProduct(e, e) - D * D;
						const float disc = bb * bb - 4.f * a * cc;
						if (disc < 0.f) break;
						const float sq = FMath::Sqrt(disc);
						const float u = (-bb + sq) / (2.f * a);
						if (u < u0 || u > 1.f) break;
						const FVector3f P = SegStart + d * u;
						Anchor = P;
						++Placed;
						if (Placed == LocalIndex)
						{
							ResultPos = P;
							ResultNormal = FMath::Lerp(FVector3f(NrmStart.X, NrmStart.Y, NrmStart.Z), FVector3f(NrmNext.X, NrmNext.Y, NrmNext.Z), u);
							ResultW = FMath::Max(TgtStart.W, TgtNext.W);
							bFound = true;
							break;
						}
						u0 = u;
					}

					SegStart = SegEnd;
					NrmStart = NrmNext;
					TgtStart = TgtNext;
				}

				if (!bFound)
				{
					const int32 IdxEnd = FMath::Min(BaseIndex + LastLocal, LastGlobal);
					ResultPos = SamplePos(IdxEnd);
					ResultNormal = FVector3f(InNormal[IdxEnd].X, InNormal[IdxEnd].Y, InNormal[IdxEnd].Z);
					ResultW = InTarget[IdxEnd].W;
				}

				const FVector3f FinalNormalG = SafeNormalize3(ResultNormal, FVector3f(0.f, 0.f, 1.f));
				OutTarget[i] = FVector4f(ResultPos, ResultW);
				OutNormal[i] = FVector4f(FinalNormalG, CenterNormal.W);
				continue;
			}

			const int32 Idx0 = FMath::Min(BaseIndex, LastGlobal);
			const FVector4f T0 = InTarget[Idx0];
			FVector3f WalkPos = T0.W > 0.5f ? FVector3f(T0.X, T0.Y, T0.Z) : FVector3f(Noised[Idx0].X, Noised[Idx0].Y, Noised[Idx0].Z);
			float TotalLength = 0.f;
			for (int32 k = 1; k <= LastLocal; ++k)
			{
				const int32 Idx = FMath::Min(BaseIndex + k, LastGlobal);
				const FVector4f T = InTarget[Idx];
				const FVector3f Pos = T.W > 0.5f ? FVector3f(T.X, T.Y, T.Z) : FVector3f(Noised[Idx].X, Noised[Idx].Y, Noised[Idx].Z);
				TotalLength += (Pos - WalkPos).Size();
				WalkPos = Pos;
			}
			if (TotalLength <= 1.0e-6f) continue;

			const float TargetArc = TotalLength * (float(LocalIndex) / float(LastLocal));
			int32 IdxPrev = FMath::Min(BaseIndex, LastGlobal);
			FVector4f TargetPrev = InTarget[IdxPrev];
			FVector4f NormalPrev = InNormal[IdxPrev];
			FVector3f SegStartPos = TargetPrev.W > 0.5f ? FVector3f(TargetPrev.X, TargetPrev.Y, TargetPrev.Z) : FVector3f(Noised[IdxPrev].X, Noised[IdxPrev].Y, Noised[IdxPrev].Z);
			float Accum = 0.f;
			FVector3f ResultPos = SegStartPos;
			FVector3f ResultNormal(NormalPrev.X, NormalPrev.Y, NormalPrev.Z);
			float ResultW = TargetPrev.W;
			for (int32 s = 1; s <= LastLocal; ++s)
			{
				const int32 IdxNext = FMath::Min(BaseIndex + s, LastGlobal);
				const FVector4f TargetNext = InTarget[IdxNext];
				const FVector4f NormalNext = InNormal[IdxNext];
				const FVector3f SegEndPos = TargetNext.W > 0.5f ? FVector3f(TargetNext.X, TargetNext.Y, TargetNext.Z) : FVector3f(Noised[IdxNext].X, Noised[IdxNext].Y, Noised[IdxNext].Z);
				const float SegLen = (SegEndPos - SegStartPos).Size();
				const float NextAccum = Accum + SegLen;
				if (TargetArc <= NextAccum || s == LastLocal)
				{
					const float Frac = FMath::Clamp((TargetArc - Accum) / FMath::Max(SegLen, 1.0e-6f), 0.f, 1.f);
					ResultPos = FMath::Lerp(SegStartPos, SegEndPos, Frac);
					ResultNormal = FMath::Lerp(FVector3f(NormalPrev.X, NormalPrev.Y, NormalPrev.Z), FVector3f(NormalNext.X, NormalNext.Y, NormalNext.Z), Frac);
					ResultW = FMath::Max(TargetPrev.W, TargetNext.W);
					break;
				}
				Accum = NextAccum;
				SegStartPos = SegEndPos;
				TargetPrev = TargetNext;
				NormalPrev = NormalNext;
			}
			const FVector3f FinalNormal = SafeNormalize3(ResultNormal, FVector3f(0.f, 0.f, 1.f));
			OutTarget[i] = FVector4f(ResultPos, ResultW);
			OutNormal[i] = FVector4f(FinalNormal, CenterNormal.W);
		}
	}

	// Pass B 单次迭代：仅对 SurfaceTarget.xyz 做沿线加权平均（端点锚定），Normal 原样传递。
	static void RunSmoothPathIteration(
		const TArray<FIntVector4>& Meta,
		const TArray<FVector4f>& InTarget, const TArray<FVector4f>& InNormal,
		int32 KernelRadius, float AngleStrength,
		TArray<FVector4f>& OutTarget, TArray<FVector4f>& OutNormal)
	{
		const int32 N = InTarget.Num();
		OutTarget = InTarget;
		OutNormal = InNormal;
		for (int32 i = 0; i < N; ++i)
		{
			const FVector4f CenterTarget = InTarget[i];
			const FIntVector4& M = Meta[i];
			const int32 BaseIndex = FMath::Max(M.Z, 0);
			const int32 PointCount = FMath::Max(M.W, 1);
			const int32 LocalIndex = i - BaseIndex;
			if (PointCount < 3 || LocalIndex <= 0 || LocalIndex >= PointCount - 1) continue;

			int32 Radius = FMath::Max(KernelRadius, 1);
			const int32 MaxReach = FMath::Min(LocalIndex, (PointCount - 1) - LocalIndex);
			Radius = FMath::Min(Radius, MaxReach);

			FVector3f SumPos(0.f, 0.f, 0.f);
			float TotalWeight = 0.f;
			if (Radius <= 1)
			{
				const int32 PrevIndex = BaseIndex + LocalIndex - 1;
				const int32 NextIndex = BaseIndex + LocalIndex + 1;
				SumPos += FVector3f(CenterTarget.X, CenterTarget.Y, CenterTarget.Z) * 2.f;
				SumPos += FVector3f(InTarget[PrevIndex].X, InTarget[PrevIndex].Y, InTarget[PrevIndex].Z);
				SumPos += FVector3f(InTarget[NextIndex].X, InTarget[NextIndex].Y, InTarget[NextIndex].Z);
				TotalWeight = 4.f;
			}
			else
			{
				const float Sigma = FMath::Max(float(Radius) * 0.5f, 0.5f);
				const float InvTwoSigmaSq = 1.f / (2.f * Sigma * Sigma);
				for (int32 Offset = -Radius; Offset <= Radius; ++Offset)
				{
					const int32 SampleLocal = FMath::Clamp(LocalIndex + Offset, 0, PointCount - 1);
					const int32 SampleIndex = BaseIndex + SampleLocal;
					const float Weight = FMath::Exp(-float(Offset * Offset) * InvTwoSigmaSq);
					SumPos += FVector3f(InTarget[SampleIndex].X, InTarget[SampleIndex].Y, InTarget[SampleIndex].Z) * Weight;
					TotalWeight += Weight;
				}
			}
			if (TotalWeight <= 1.0e-8f) continue;
			FVector3f P = SumPos / TotalWeight;

			const float SafeAngleStrength = FMath::Clamp(AngleStrength, 0.f, 1.f);
			if (SafeAngleStrength > 1.0e-6f)
			{
				const int32 PrevNeighbor = BaseIndex + LocalIndex - 1;
				const int32 NextNeighbor = BaseIndex + LocalIndex + 1;
				const FVector3f CenterPos(CenterTarget.X, CenterTarget.Y, CenterTarget.Z);
				const FVector3f DirPrev = SafeNormalize3(FVector3f(InTarget[PrevNeighbor].X, InTarget[PrevNeighbor].Y, InTarget[PrevNeighbor].Z) - CenterPos, FVector3f::ZeroVector);
				const FVector3f DirNext = SafeNormalize3(FVector3f(InTarget[NextNeighbor].X, InTarget[NextNeighbor].Y, InTarget[NextNeighbor].Z) - CenterPos, FVector3f::ZeroVector);
				// 0 at a straight line (dot == -1), 1 at a full fold (dot == +1).
				const float CornerFactor = FMath::Clamp(FVector3f::DotProduct(DirPrev, DirNext) * 0.5f + 0.5f, 0.f, 1.f);
				const float Blend = FMath::Lerp(1.f, CornerFactor, SafeAngleStrength);
				P = FMath::Lerp(CenterPos, P, Blend);
			}

			OutTarget[i] = FVector4f(P, CenterTarget.W);
		}
	}
	// Pass PT：用最终 SurfaceTarget 重建 tangent，并沿每条线平行传输 roll 轴以最小化扭曲。
	// 仅用起点的 surface normal 作初始种子，后续点不再逐点追 surface normal。
	static void RunBuildParallelTransportFrame(
		const TArray<FVector4f>& Noised, const TArray<FIntVector4>& Meta,
		const TArray<FVector4f>& Target, const TArray<FVector4f>& Normal,
		TArray<FVector4f>& OutTangent, TArray<FVector4f>& OutFrameNormal)
	{
		const int32 N = Target.Num();
		OutTangent.SetNumUninitialized(N);
		OutFrameNormal.SetNumUninitialized(N);

		// 先重建所有点的 tangent（与点的处理顺序无关）。
		for (int32 i = 0; i < N; ++i)
		{
			const int32 PrevIndex = GetLinePointIndex(Meta, N, i, -1);
			const int32 NextIndex = GetLinePointIndex(Meta, N, i, 1);
			const FVector4f PrevTarget = Target[PrevIndex];
			const FVector4f NextTarget = Target[NextIndex];
			const FVector3f PrevPos = PrevTarget.W > 0.5f ? FVector3f(PrevTarget.X, PrevTarget.Y, PrevTarget.Z) : FVector3f(Noised[PrevIndex].X, Noised[PrevIndex].Y, Noised[PrevIndex].Z);
			const FVector3f NextPos = NextTarget.W > 0.5f ? FVector3f(NextTarget.X, NextTarget.Y, NextTarget.Z) : FVector3f(Noised[NextIndex].X, Noised[NextIndex].Y, Noised[NextIndex].Z);
			const FVector3f RawTangent = SafeNormalize3(NextPos - PrevPos, FVector3f(1.f, 0.f, 0.f));
			OutTangent[i] = FVector4f(RawTangent, 0.f);
		}

		// 沿每条线串行平行传输 roll 轴：起点用 surface normal 播种，其余点用最小对齐旋转传递。
		for (int32 i = 0; i < N; ++i)
		{
			const FIntVector4& M = Meta[i];
			const int32 BaseIndex = FMath::Max(M.Z, 0);
			const int32 PointCount = FMath::Max(M.W, 1);
			if (i != BaseIndex) continue;

			const int32 LastIndex = FMath::Min(BaseIndex + PointCount - 1, N - 1);
			FVector3f PrevTangent(1.f, 0.f, 0.f);
			FVector3f PrevFrameNormal(0.f, 0.f, 1.f);
			for (int32 LocalIndex = 0; LocalIndex < PointCount; ++LocalIndex)
			{
				const int32 Idx = FMath::Min(BaseIndex + LocalIndex, LastIndex);
				const FVector3f RawTangent(OutTangent[Idx].X, OutTangent[Idx].Y, OutTangent[Idx].Z);

				FVector3f FrameNormal;
				if (LocalIndex == 0)
				{
					const FVector4f PackedSurfaceNormal = Normal[Idx];
					const FVector3f SeedNormal = SafeNormalize3(FVector3f(PackedSurfaceNormal.X, PackedSurfaceNormal.Y, PackedSurfaceNormal.Z), GetFallbackFrameNormal(RawTangent));
					FrameNormal = BuildFrameNormalFromTangent(RawTangent, SeedNormal);
				}
				else
				{
					const FVector3f Transported = RotateVectorByAlign(PrevFrameNormal, PrevTangent, RawTangent);
					FrameNormal = BuildFrameNormalFromTangent(RawTangent, Transported);
				}
				OutFrameNormal[Idx] = FVector4f(FrameNormal, 0.f);

				PrevTangent = RawTangent;
				PrevFrameNormal = FrameNormal;
			}
		}
	}

	// Pass AS 单次迭代：邻域加权平滑 tangent（中心 2 : 前 1 : 后 1），frame normal 同步平滑并对齐正交。
	static void RunSmoothTangentsIteration(
		const TArray<FIntVector4>& Meta,
		const TArray<FVector4f>& InTangent, const TArray<FVector4f>& InFrameNormal,
		TArray<FVector4f>& OutTangent, TArray<FVector4f>& OutFrameNormal)
	{
		const int32 N = InTangent.Num();
		OutTangent = InTangent;
		OutFrameNormal = InFrameNormal;
		for (int32 i = 0; i < N; ++i)
		{
			const FVector4f CenterTangent = InTangent[i];
			const FVector4f CenterFrameNormal = InFrameNormal[i];
			const FIntVector4& M = Meta[i];
			const int32 BaseIndex = FMath::Max(M.Z, 0);
			const int32 PointCount = FMath::Max(M.W, 1);
			const int32 LocalIndex = i - BaseIndex;
			if (PointCount < 3 || LocalIndex <= 0 || LocalIndex >= PointCount - 1) continue;

			const int32 LastIndex = N - 1;
			const int32 PrevIndex = FMath::Min(FMath::Max(M.X, 0), LastIndex);
			const int32 NextIndex = FMath::Min(FMath::Max(M.Y, 0), LastIndex);

			const FVector3f CenterT(CenterTangent.X, CenterTangent.Y, CenterTangent.Z);
			FVector3f PrevTangent = SafeNormalize3(FVector3f(InTangent[PrevIndex].X, InTangent[PrevIndex].Y, InTangent[PrevIndex].Z), FVector3f(1.f, 0.f, 0.f));
			FVector3f NextTangent = SafeNormalize3(FVector3f(InTangent[NextIndex].X, InTangent[NextIndex].Y, InTangent[NextIndex].Z), FVector3f(1.f, 0.f, 0.f));
			PrevTangent = OrientAxisToReference(PrevTangent, CenterT);
			NextTangent = OrientAxisToReference(NextTangent, CenterT);
			FVector3f Smoothed = CenterT * 2.f + PrevTangent + NextTangent;
			Smoothed = SafeNormalize3(Smoothed, CenterT);

			const FVector3f CenterFN(CenterFrameNormal.X, CenterFrameNormal.Y, CenterFrameNormal.Z);
			FVector3f PrevFrameNormal = SafeNormalize3(FVector3f(InFrameNormal[PrevIndex].X, InFrameNormal[PrevIndex].Y, InFrameNormal[PrevIndex].Z), CenterFN);
			FVector3f NextFrameNormal = SafeNormalize3(FVector3f(InFrameNormal[NextIndex].X, InFrameNormal[NextIndex].Y, InFrameNormal[NextIndex].Z), CenterFN);
			PrevFrameNormal = OrientAxisToReference(PrevFrameNormal, CenterFN);
			NextFrameNormal = OrientAxisToReference(NextFrameNormal, CenterFN);
			FVector3f SmoothedFrameNormal = CenterFN * 2.f + PrevFrameNormal + NextFrameNormal;
			SmoothedFrameNormal = BuildFrameNormalFromTangent(Smoothed, SmoothedFrameNormal);

			OutTangent[i] = FVector4f(Smoothed, 0.f);
			OutFrameNormal[i] = FVector4f(SmoothedFrameNormal, 0.f);
		}
	}
	// Pass C：沿路径扫掠出三角网格（顶点 / UV / 索引）。
	// RawFrameNormal 在 shader 里来自 Pass A 的 normal，仅作退化兜底；有 voxel 采样时几乎不参与，
	// 这里用 GetFallbackFrameNormal(Tangent) 等价替代，省去额外回读。
	static void RunBuildMesh(
		const TArray<FVector4f>& Noised, const TArray<FIntVector4>& Meta,
		const TArray<float>& CurveU, const TArray<FIntVector4>& SegmentMeta,
		const TArray<FVector4f>& Tangent, const TArray<FVector4f>& FrameNormal,
		const TArray<FVector4f>& SurfaceTarget, const TArray<FVector4f>& SurfaceNormal,
		bool bTube, uint32 TubeProfileCount, float CircleScale, float LineScale, float VinesOffset, float TinyZJitterStrength,
		TArray<FVector4f>& OutVertices, TArray<FVector2f>& OutUVs, TArray<uint32>& OutIndices)
	{
		const int32 PathPointCount = Noised.Num();
		const int32 SegmentCount = SegmentMeta.Num();
		const uint32 ProfileCount = bTube ? FMath::Max(TubeProfileCount, 3u) : 2u;
		const int32 OutputVertexCount = PathPointCount * int32(ProfileCount);
		const int32 OutputIndexCount = bTube ? SegmentCount * int32(ProfileCount) * 6 : SegmentCount * 6;

		OutVertices.SetNumZeroed(OutputVertexCount);
		OutUVs.SetNumZeroed(OutputVertexCount);
		OutIndices.SetNumZeroed(OutputIndexCount);

		for (int32 Index = 0; Index < OutputVertexCount; ++Index)
		{
			const uint32 PointIndex = uint32(Index) / ProfileCount;
			const uint32 ProfileIndex = uint32(Index) - PointIndex * ProfileCount;
			if (PointIndex >= uint32(PathPointCount)) continue;

			const FVector4f PackedPoint = Noised[PointIndex];
			const FIntVector4& M = Meta[PointIndex];
			uint32 PrevIndex = FMath::Min<uint32>(uint32(FMath::Max(M.X, 0)), uint32(PathPointCount - 1));
			uint32 NextIndex = FMath::Min<uint32>(uint32(FMath::Max(M.Y, 0)), uint32(PathPointCount - 1));

			const float V = CurveU[PointIndex];
			const float U = bTube ? float(ProfileIndex) / float(ProfileCount)
								   : float(ProfileIndex) / float(ProfileCount - 1u);

			const FVector3f Query(PackedPoint.X, PackedPoint.Y, PackedPoint.Z);
			const float Scale = PackedPoint.W;
			const FVector3f PrevPos(Noised[PrevIndex].X, Noised[PrevIndex].Y, Noised[PrevIndex].Z);
			const FVector3f NextPos(Noised[NextIndex].X, Noised[NextIndex].Y, Noised[NextIndex].Z);
			const FVector3f FallbackTangent = SafeNormalize3(NextPos - PrevPos, FVector3f(1.f, 0.f, 0.f));
			const FVector3f T = SafeNormalize3(FVector3f(Tangent[PointIndex].X, Tangent[PointIndex].Y, Tangent[PointIndex].Z), FallbackTangent);
			const FVector3f RawFrameNormal = GetFallbackFrameNormal(T);

			const FVector4f PackedSurfaceTarget = SurfaceTarget[PointIndex];
			const bool bHasVoxelSample = PackedSurfaceTarget.W > 0.5f;
			const FVector3f SurfNormalRaw(SurfaceNormal[PointIndex].X, SurfaceNormal[PointIndex].Y, SurfaceNormal[PointIndex].Z);
			const FVector3f SurfNormal = bHasVoxelSample ? SafeNormalize3(SurfNormalRaw, RawFrameNormal) : RawFrameNormal;
			const FVector3f SmoothedFrameNormalSeed = SafeNormalize3(FVector3f(FrameNormal[PointIndex].X, FrameNormal[PointIndex].Y, FrameNormal[PointIndex].Z), RawFrameNormal);

			FVector3f FrameTangent, FrameN, Binormal;
			BuildOrthonormalFrame(T, SmoothedFrameNormalSeed, FrameTangent, FrameN, Binormal);

			const FVector2f Profile = GetProfilePoint(ProfileIndex, ProfileCount, bTube, CircleScale, LineScale) * Scale;
			// Mirror the GPU Pass C: always ride the smoothed SurfaceTarget. The .w flag only gates
			// the surface-hugging normal offset, it must not switch the center back to the raw Query.
			const FVector3f SurfTarget = FVector3f(PackedSurfaceTarget.X, PackedSurfaceTarget.Y, PackedSurfaceTarget.Z);
			// VinesOffset 已在 GPU Pass FP 施加并写入 SurfaceTarget，这里只补 Z 抖动，避免重复偏移。
			(void)VinesOffset;
			const float TinyJitter = HashPointJitter(PointIndex, Query) * TinyZJitterStrength;
			const FVector3f Center = SurfTarget + SurfNormal * TinyJitter;
			const FVector3f OutPosition = Center + Binormal * Profile.X + FrameN * Profile.Y;

			OutVertices[Index] = FVector4f(OutPosition, 1.f);
			OutUVs[Index] = FVector2f(U, V);
		}

		for (int32 Index = 0; Index < SegmentCount; ++Index)
		{
			const FIntVector4& Segment = SegmentMeta[Index];
			const uint32 APoint = uint32(FMath::Max(Segment.X, 0));
			const uint32 BPoint = uint32(FMath::Max(Segment.Y, 0));
			if (APoint >= uint32(PathPointCount) || BPoint >= uint32(PathPointCount)) continue;

			const uint32 ABase = APoint * ProfileCount;
			const uint32 BBase = BPoint * ProfileCount;
			if (bTube)
			{
				const uint32 SegmentIndexBase = uint32(Index) * ProfileCount * 6u;
				for (uint32 Edge = 0u; Edge < ProfileCount; ++Edge)
				{
					const uint32 NextEdge = (Edge + 1u) % ProfileCount;
					const uint32 WriteBase = SegmentIndexBase + Edge * 6u;
					if (WriteBase + 5u >= uint32(OutputIndexCount)) continue;
					const uint32 A0 = ABase + Edge, A1 = ABase + NextEdge, B0 = BBase + Edge, B1 = BBase + NextEdge;
					OutIndices[WriteBase + 0u] = A0; OutIndices[WriteBase + 1u] = B0; OutIndices[WriteBase + 2u] = B1;
					OutIndices[WriteBase + 3u] = A0; OutIndices[WriteBase + 4u] = B1; OutIndices[WriteBase + 5u] = A1;
				}
			}
			else
			{
				const uint32 WriteBase = uint32(Index) * 6u;
				if (WriteBase + 5u < uint32(OutputIndexCount) && ProfileCount >= 2u)
				{
					const uint32 A0 = ABase + 0u, A1 = ABase + 1u, B0 = BBase + 0u, B1 = BBase + 1u;
					OutIndices[WriteBase + 0u] = A0; OutIndices[WriteBase + 1u] = B0; OutIndices[WriteBase + 2u] = B1;
					OutIndices[WriteBase + 3u] = A0; OutIndices[WriteBase + 4u] = B1; OutIndices[WriteBase + 5u] = A1;
				}
			}
		}
	}

	// 主驱动：消费 GPU 回读的 noised + post-FP surface target/normal，执行 RS→B→FT→AS→C。
	static void RunPostPasses(
		const TArray<FVector4f>& Noised, const TArray<FIntVector4>& Meta,
		const TArray<float>& CurveU, const TArray<FIntVector4>& SegmentMeta,
		const TArray<FVector4f>& InSurfaceTarget, const TArray<FVector4f>& InSurfaceNormal,
		bool bResampleSurface, float ResampleTargetDistance, int32 PostProjectionSmoothIterations, int32 PostProjectionSmoothKernelRadius,
		int32 PostProjectionSmallSmoothIterations, float PostProjectionSmoothAngleStrength,
		bool bTube, uint32 TubeProfileCount, float CircleScale, float LineScale, float VinesOffset, float TinyZJitterStrength,
		TArray<FVector4f>& OutVertices, TArray<FVector2f>& OutUVs, TArray<uint32>& OutIndices,
		TArray<FVector4f>* OutSurfaceTargets)
	{
		const int32 SafeSmoothIters = FMath::Max(0, PostProjectionSmoothIterations);
		const int32 SafeKernelRadius = FMath::Max(1, PostProjectionSmoothKernelRadius);
		const int32 SafeSmallIters = FMath::Max(0, PostProjectionSmallSmoothIterations);
		const float SafeAngleStrength = FMath::Clamp(PostProjectionSmoothAngleStrength, 0.f, 1.f);

		// RS（可选）：B 缓冲 -> A 缓冲。
		TArray<FVector4f> CurTarget = InSurfaceTarget;
		TArray<FVector4f> CurNormal = InSurfaceNormal;
		if (bResampleSurface)
		{
			TArray<FVector4f> RsTarget, RsNormal;
			RunResampleSurface(Noised, Meta, CurTarget, CurNormal, ResampleTargetDistance, RsTarget, RsNormal);
			CurTarget = MoveTemp(RsTarget);
			CurNormal = MoveTemp(RsNormal);
		}

		// B：宽核 + radius-1 清理的 ping-pong 位置平滑。
		const int32 TotalSmoothIters = SafeSmoothIters + SafeSmallIters;
		for (int32 It = 0; It < TotalSmoothIters; ++It)
		{
			const bool bSmallPass = It >= SafeSmoothIters;
			const int32 Radius = bSmallPass ? 1 : SafeKernelRadius;
			TArray<FVector4f> NextTarget, NextNormal;
			RunSmoothPathIteration(Meta, CurTarget, CurNormal, Radius, SafeAngleStrength, NextTarget, NextNormal);
			CurTarget = MoveTemp(NextTarget);
			CurNormal = MoveTemp(NextNormal);
		}

		if (OutSurfaceTargets) *OutSurfaceTargets = CurTarget;

		// PT：重建 tangent，并沿线平行传输 frame normal（roll 轴）。该帧即最终轴朝向。
		TArray<FVector4f> Tangent, FrameNormal;
		RunBuildParallelTransportFrame(Noised, Meta, CurTarget, CurNormal, Tangent, FrameNormal);

		// C：扫掠网格。
		RunBuildMesh(Noised, Meta, CurveU, SegmentMeta, Tangent, FrameNormal, CurTarget, CurNormal,
			bTube, TubeProfileCount, CircleScale, LineScale, VinesOffset, TinyZJitterStrength, OutVertices, OutUVs, OutIndices);
	}
} // namespace VineCPUTail

// 方式 A 截断函数：GPU 只跑 N→A→P→FP（voxel 依赖段），回读 noised + FP 投影后的
// surface target/normal，供 CPU 接力执行 RS/B/FT/C。不创建顶点/UV/索引缓冲。
static bool DispatchVVGPU_VoxelToFP(
	const TArray<FVector4f>& PathPoints,
	const TArray<FVector4f>& PathPointAxes,
	const TArray<FIntVector4>& PathPointMeta,
	float CurlNoiseStrength,
	float CurlNoiseFrequency,
	float PerlinNoiseStrength,
	float PerlinNoiseFrequency,
	int32 NoiseIterations,
	float VinesOffset,
	const FCSSurfaceVoxelData& VoxelData,
	TArray<FVector4f>& OutNoised,
	TArray<FVector4f>& OutSurfaceTarget,
	TArray<FVector4f>& OutSurfaceNormal)
{
	const uint32 PathPointCount = uint32(PathPoints.Num());
	const uint32 SafeNoiseIterations = uint32(FMath::Max(0, NoiseIterations));
	if (PathPointCount == 0) return false;

	const uint32 VoxelCount = uint32(VoxelData.Cells.Num());
	if (VoxelCount == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU_VoxelToFP] No voxel data available."));
		return false;
	}

	TArray<FVector4f> GPUVoxelTargetPositions;
	TArray<FVector4f> GPUVoxelNormals;
	TArray<FIntVector4> GPUVoxelCells;
	GPUVoxelTargetPositions.Reserve(VoxelCount);
	GPUVoxelNormals.Reserve(VoxelCount);
	GPUVoxelCells.Reserve(VoxelCount);

	auto IsFiniteVector = [](const FVector& Vector)
	{
		return FMath::IsFinite(Vector.X) && FMath::IsFinite(Vector.Y) && FMath::IsFinite(Vector.Z);
	};

	const float SafeVoxelSize = FMath::Max(VoxelData.VoxelSize, UE_KINDA_SMALL_NUMBER);
	const double MaxTargetDistanceSq = FMath::Square(double(SafeVoxelSize * 2.0f));
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
		}

		FVector Normal = VoxelData.Normals.IsValidIndex(int32(i)) ? VoxelData.Normals[i] : FVector::UpVector;
		if (!IsFiniteVector(Normal) || !Normal.Normalize())
		{
			Normal = FVector::UpVector;
		}

		GPUVoxelTargetPositions.Add(FVector4f(float(Target.X), float(Target.Y), float(Target.Z), 1.0f));
		GPUVoxelNormals.Add(FVector4f(float(Normal.X), float(Normal.Y), float(Normal.Z), 0.0f));
		GPUVoxelCells.Add(FIntVector4(Cell.X, Cell.Y, Cell.Z, 0));
	}

	TArray<uint32> GPUVoxelHashSlots;
	uint32 GPUVoxelHashSlotCount = 0u;
	if (!BuildVineVoxelHashSlots(GPUVoxelCells, GPUVoxelHashSlots, GPUVoxelHashSlotCount))
	{
		GPUVoxelHashSlots.Init(0u, 1);
	}

	FVineTargetBucketBuffers TargetBuckets;
	const FVector3f TargetBucketOrigin = FVector3f(VoxelData.VoxelOrigin);
	BuildVineTargetBucketBuffers(GPUVoxelTargetPositions, TargetBucketOrigin, SafeVoxelSize, TargetBuckets);

	const uint64 ReadbackBytes64 = uint64(PathPointCount) * sizeof(FVector4f);
	if (ReadbackBytes64 > MAX_uint32) return false;
	const uint32 ReadbackBytes = uint32(ReadbackBytes64);

	FRHIGPUBufferReadback* NoisedReadback = new FRHIGPUBufferReadback(TEXT("VVVoxelToFP_NoisedReadback"));
	FRHIGPUBufferReadback* TargetReadback = new FRHIGPUBufferReadback(TEXT("VVVoxelToFP_TargetReadback"));
	FRHIGPUBufferReadback* NormalReadback = new FRHIGPUBufferReadback(TEXT("VVVoxelToFP_NormalReadback"));
	bool bRenderWorkQueued = false;

	ENQUEUE_RENDER_COMMAND(VVVoxelToFP)(
		[PathPoints, PathPointAxes, PathPointMeta,
		 GPUVoxelCells, GPUVoxelHashSlots, GPUVoxelNormals, GPUVoxelTargetPositions,
		 TargetBuckets, TargetBucketOrigin,
		 NoisedReadback, TargetReadback, NormalReadback, ReadbackBytes,
		 PathPointCount, VoxelCount, GPUVoxelHashSlotCount,
		 CurlNoiseStrength, CurlNoiseFrequency, PerlinNoiseStrength, PerlinNoiseFrequency, SafeNoiseIterations,
		 VinesOffset,
		 VoxelOrigin = FVector3f(VoxelData.VoxelOrigin), VoxelSize = float(VoxelData.VoxelSize),
		 &bRenderWorkQueued](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			const CSHepler::FRDGStructuredBufferRefs PathPointBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, PathPoints, TEXT("VVVoxelToFP.PathPoints"));
			const CSHepler::FRDGStructuredBufferRefs PathPointAxisBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, PathPointAxes, TEXT("VVVoxelToFP.PathPointAxes"));
			const CSHepler::FRDGStructuredBufferRefs PathPointMetaBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, PathPointMeta, TEXT("VVVoxelToFP.PathPointMeta"));
			const CSHepler::FRDGStructuredBufferRefs VoxelCellsBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, GPUVoxelCells, TEXT("VVVoxelToFP.VoxelCells"));
			const CSHepler::FRDGStructuredBufferRefs VoxelHashSlotsBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, GPUVoxelHashSlots, TEXT("VVVoxelToFP.VoxelHashSlots"));
			const CSHepler::FRDGStructuredBufferRefs VoxelNormalsBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, GPUVoxelNormals, TEXT("VVVoxelToFP.VoxelNormals"));
			const CSHepler::FRDGStructuredBufferRefs VoxelTargetPositionsBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, GPUVoxelTargetPositions, TEXT("VVVoxelToFP.VoxelTargetPositions"));
			const CSHepler::FRDGStructuredBufferRefs TargetBucketRangesBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, TargetBuckets.Ranges, TEXT("VVVoxelToFP.TargetBucketRanges"));
			const CSHepler::FRDGStructuredBufferRefs TargetBucketRangeCountsBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, TargetBuckets.RangeCounts, TEXT("VVVoxelToFP.TargetBucketRangeCounts"));
			const CSHepler::FRDGStructuredBufferRefs TargetBucketVoxelIndicesBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, TargetBuckets.VoxelIndices, TEXT("VVVoxelToFP.TargetBucketVoxelIndices"));
			const CSHepler::FRDGStructuredBufferRefs TargetBucketHashSlotsBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, TargetBuckets.HashSlots, TEXT("VVVoxelToFP.TargetBucketHashSlots"));

			CSHepler::FRDGStructuredBufferRefs PathPointTangentA = CSHepler::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), PathPointCount, TEXT("VVVoxelToFP.PathPointTangentsA"), true, true);
			CSHepler::FRDGStructuredBufferRefs PathPointNormalA = CSHepler::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), PathPointCount, TEXT("VVVoxelToFP.PathPointNormalsA"), true, true);
			CSHepler::FRDGStructuredBufferRefs PathPointSurfaceTargetA = CSHepler::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), PathPointCount, TEXT("VVVoxelToFP.PathPointSurfaceTargetsA"), true, true);
			CSHepler::FRDGStructuredBufferRefs PathPointSurfaceNormalA = CSHepler::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), PathPointCount, TEXT("VVVoxelToFP.PathPointSurfaceNormalsA"), true, true);
			CSHepler::FRDGStructuredBufferRefs PathPointSurfaceTargetB = CSHepler::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), PathPointCount, TEXT("VVVoxelToFP.PathPointSurfaceTargetsB"), true, true);
			CSHepler::FRDGStructuredBufferRefs PathPointSurfaceNormalB = CSHepler::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), PathPointCount, TEXT("VVVoxelToFP.PathPointSurfaceNormalsB"), true, true);
			CSHepler::FRDGStructuredBufferRefs PathPointNoisedBuffer = CSHepler::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), PathPointCount, TEXT("VVVoxelToFP.PathPointsNoised"), true, true);

			// Pass N
			TShaderMapRef<FVVVoxelNoiseCS> NoiseShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
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
			GraphBuilder.AddPass(RDG_EVENT_NAME("VVVoxelToFP.ApplyNoise"), NoiseParameters, ERDGPassFlags::Compute,
				[NoiseParameters, NoiseShader, PathPointCount](FRHIComputeCommandList& InRHICmdList)
				{ FComputeShaderUtils::Dispatch(InRHICmdList, NoiseShader, *NoiseParameters, FComputeShaderUtils::GetGroupCount(FIntVector(PathPointCount, 1, 1), 64)); });

			// Pass A
			TShaderMapRef<FVVVoxelBuildAxesCS> BuildAxesShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
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
			GraphBuilder.AddPass(RDG_EVENT_NAME("VVVoxelToFP.BuildAxes"), BuildAxesParameters, ERDGPassFlags::Compute,
				[BuildAxesParameters, BuildAxesShader, PathPointCount](FRHIComputeCommandList& InRHICmdList)
				{ FComputeShaderUtils::Dispatch(InRHICmdList, BuildAxesShader, *BuildAxesParameters, FComputeShaderUtils::GetGroupCount(FIntVector(PathPointCount, 1, 1), 64)); });

			// Pass P
			TShaderMapRef<FVVVoxelPerlinNoiseCS> PerlinNoiseShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FVVVoxelPerlinNoiseCS::FParameters* PerlinNoiseParameters = GraphBuilder.AllocParameters<FVVVoxelPerlinNoiseCS::FParameters>();
			PerlinNoiseParameters->PathPointSurfaceTargets = PathPointSurfaceTargetA.SRV;
			PerlinNoiseParameters->PathPointSurfaceNormals = PathPointSurfaceNormalA.SRV;
			PerlinNoiseParameters->RW_PathPointSurfaceTargets = PathPointSurfaceTargetA.UAV;
			PerlinNoiseParameters->PerlinNoiseStrength = PerlinNoiseStrength;
			PerlinNoiseParameters->PerlinNoiseFrequency = PerlinNoiseFrequency;
			PerlinNoiseParameters->PathPointCount = PathPointCount;
			GraphBuilder.AddPass(RDG_EVENT_NAME("VVVoxelToFP.PerlinNoise"), PerlinNoiseParameters, ERDGPassFlags::Compute,
				[PerlinNoiseParameters, PerlinNoiseShader, PathPointCount](FRHIComputeCommandList& InRHICmdList)
				{ FComputeShaderUtils::Dispatch(InRHICmdList, PerlinNoiseShader, *PerlinNoiseParameters, FComputeShaderUtils::GetGroupCount(FIntVector(PathPointCount, 1, 1), 64)); });

			// Pass FP（A -> B）
			TShaderMapRef<FVVVoxelFinalProjectCS> FinalProjectShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
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
			GraphBuilder.AddPass(RDG_EVENT_NAME("VVVoxelToFP.FinalProject"), FinalProjectParameters, ERDGPassFlags::Compute,
				[FinalProjectParameters, FinalProjectShader, PathPointCount](FRHIComputeCommandList& InRHICmdList)
				{ FComputeShaderUtils::Dispatch(InRHICmdList, FinalProjectShader, *FinalProjectParameters, FComputeShaderUtils::GetGroupCount(FIntVector(PathPointCount, 1, 1), 64)); });

			AddEnqueueCopyPass(GraphBuilder, NoisedReadback, PathPointNoisedBuffer.Buffer, ReadbackBytes);
			AddEnqueueCopyPass(GraphBuilder, TargetReadback, PathPointSurfaceTargetB.Buffer, ReadbackBytes);
			AddEnqueueCopyPass(GraphBuilder, NormalReadback, PathPointSurfaceNormalB.Buffer, ReadbackBytes);
			GraphBuilder.Execute();
			bRenderWorkQueued = true;
		});


	FlushRenderingCommands();
	if (!bRenderWorkQueued)
	{
		delete NoisedReadback;
		delete TargetReadback;
		delete NormalReadback;
		return false;
	}

	OutNoised.SetNumZeroed(PathPointCount);
	OutSurfaceTarget.SetNumZeroed(PathPointCount);
	OutSurfaceNormal.SetNumZeroed(PathPointCount);
	bool bReadbackSucceeded = false;

	ENQUEUE_RENDER_COMMAND(VVVoxelToFPReadback)(
		[NoisedReadback, TargetReadback, NormalReadback, ReadbackBytes, &OutNoised, &OutSurfaceTarget, &OutSurfaceNormal, &bReadbackSucceeded](FRHICommandListImmediate& RHICmdList)
		{
			if (!NoisedReadback || !TargetReadback || !NormalReadback) return;
			if (!NoisedReadback->IsReady() || !TargetReadback->IsReady() || !NormalReadback->IsReady())
			{
				RHICmdList.SubmitAndBlockUntilGPUIdle();
			}
			bool bLockedAll = true;
			if (const FVector4f* P = static_cast<const FVector4f*>(NoisedReadback->Lock(ReadbackBytes)))
			{
				FMemory::Memcpy(OutNoised.GetData(), P, ReadbackBytes); NoisedReadback->Unlock();
			}
			else bLockedAll = false;
			if (const FVector4f* P = static_cast<const FVector4f*>(TargetReadback->Lock(ReadbackBytes)))
			{
				FMemory::Memcpy(OutSurfaceTarget.GetData(), P, ReadbackBytes); TargetReadback->Unlock();
			}
			else bLockedAll = false;
			if (const FVector4f* P = static_cast<const FVector4f*>(NormalReadback->Lock(ReadbackBytes)))
			{
				FMemory::Memcpy(OutSurfaceNormal.GetData(), P, ReadbackBytes); NormalReadback->Unlock();
			}
			else bLockedAll = false;
			delete NoisedReadback;
			delete TargetReadback;
			delete NormalReadback;
			bReadbackSucceeded = bLockedAll;
		});

	FlushRenderingCommands();
	return bReadbackSucceeded;
}

// GPU dispatch for voxel-based vine visualization.
// Surface projection samples FCSSurfaceVoxelData on the GPU.
// Trip A: merge the per-source GPU SC outputs into one contiguous VisVine batch on
// the GPU (no CPU round-trip). Point positions (.w = final scale) and CurveU are
// copied verbatim into each source's destination slice; PathPointMeta (Prev/Next/
// Base) and SegmentMeta (A/B) point indices are offset by the source's destination
// point base so they address the concatenated array. Produces one pooled buffer set
// equivalent to BuildVVGPUInput's output over all sources' lines.
static bool ConcatenateVineSCGPUBuffers(const TArray<TSharedPtr<FVineSCGPUBuffers>>& Sources, FVineSCGPUBuffers& OutConcat)
{
	OutConcat = FVineSCGPUBuffers();

	TArray<TRefCountPtr<FRDGPooledBuffer>> SrcPoints, SrcMeta, SrcCurveU, SrcSeg;
	TArray<uint32> SrcPointCounts, SrcSegCounts, DstPointBases, DstSegBases;
	uint32 TotalPoints = 0;
	uint32 TotalSegments = 0;
	for (const TSharedPtr<FVineSCGPUBuffers>& S : Sources)
	{
		if (!S.IsValid() || !S->IsValid()) continue;
		SrcPoints.Add(S->PathPoints);
		SrcMeta.Add(S->PathPointMeta);
		SrcCurveU.Add(S->PathPointCurveU);
		SrcSeg.Add(S->SegmentMeta);
		// CurveU is concatenated on the CPU in the same source order as the points.
		OutConcat.CurveUCPU.Append(S->CurveUCPU);
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
	TRefCountPtr<FRDGPooledBuffer> OutCurveU;
	TRefCountPtr<FRDGPooledBuffer> OutSeg;
	bool bConcatOK = false;

	ENQUEUE_RENDER_COMMAND(ConcatVineSCBuffers)(
		[SrcPoints = MoveTemp(SrcPoints), SrcMeta = MoveTemp(SrcMeta), SrcCurveU = MoveTemp(SrcCurveU), SrcSeg = MoveTemp(SrcSeg),
		 SrcPointCounts = MoveTemp(SrcPointCounts), SrcSegCounts = MoveTemp(SrcSegCounts),
		 DstPointBases = MoveTemp(DstPointBases), DstSegBases = MoveTemp(DstSegBases),
		 NumSrc, TotalPoints, TotalSegments,
		 &OutPathPoints, &OutMeta, &OutCurveU, &OutSeg, &bConcatOK](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			CSHepler::FRDGStructuredBufferRefs DstPoints = CSHepler::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), TotalPoints, TEXT("VineConcat.PathPoints"), true, true);
			CSHepler::FRDGStructuredBufferRefs DstMeta = CSHepler::CreateStructuredBuffer(GraphBuilder, sizeof(FIntVector4), TotalPoints, TEXT("VineConcat.PathPointMeta"), true, true);
			CSHepler::FRDGStructuredBufferRefs DstCurveU = CSHepler::CreateStructuredBuffer(GraphBuilder, sizeof(float), TotalPoints, TEXT("VineConcat.PathPointCurveU"), true, true);
			CSHepler::FRDGStructuredBufferRefs DstSeg = CSHepler::CreateStructuredBuffer(GraphBuilder, sizeof(FIntVector4), TotalSegments, TEXT("VineConcat.SegmentMeta"), true, true);

			TShaderMapRef<FConcatOffsetInt4CS> ConcatShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			TShaderMapRef<FConcatCopyFloatCS> ConcatCopyFloatShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

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
				FRDGBufferRef SrcCuBuf = GraphBuilder.RegisterExternalBuffer(SrcCurveU[s], TEXT("VineConcat.SrcCurveU"));
				FRDGBufferRef SrcMtBuf = GraphBuilder.RegisterExternalBuffer(SrcMeta[s], TEXT("VineConcat.SrcMeta"));

				// Positions: 16-byte float4, verbatim byte copy works.
				AddCopyBufferPass(GraphBuilder, DstPoints.Buffer, uint64(DstPtBase) * sizeof(FVector4f), SrcPtBuf, 0, uint64(PtCount) * sizeof(FVector4f));
				// CurveU: 4-byte float — byte copy silently zeroed it, so copy via compute.
				{
					FConcatCopyFloatCS::FParameters* P = GraphBuilder.AllocParameters<FConcatCopyFloatCS::FParameters>();
					P->ConcatSrcFloat = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SrcCuBuf));
					P->RW_ConcatDstFloat = DstCurveU.UAV;
					P->ConcatFloatCount = PtCount;
					P->ConcatFloatDstBase = DstPtBase;
					GraphBuilder.AddPass(RDG_EVENT_NAME("VineConcat.CurveU"), P, ERDGPassFlags::Compute,
						[P, ConcatCopyFloatShader, PtCount](FRHIComputeCommandList& InRHICmdList)
						{ FComputeShaderUtils::Dispatch(InRHICmdList, ConcatCopyFloatShader, *P, FComputeShaderUtils::GetGroupCount(FIntVector(PtCount, 1, 1), 64)); });
				}

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
			OutCurveU = GraphBuilder.ConvertToExternalBuffer(DstCurveU.Buffer);
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
	OutConcat.PathPointCurveU = MoveTemp(OutCurveU);
	OutConcat.SegmentMeta = MoveTemp(OutSeg);
	OutConcat.PointCount = int32(TotalPoints);
	OutConcat.SegmentCount = int32(TotalSegments);
	OutConcat.LineCount = 0;
	return true;
}

static bool DispatchVVGPU_Voxel(
	const TArray<FVector4f>& PathPoints,
	const TArray<FVector4f>& PathPointAxes,
	const TArray<FIntVector4>& PathPointMeta,
	const TArray<float>& PathPointCurveU,
	const TArray<FIntVector4>& SegmentMeta,
	bool bTube,
	uint32 TubeProfileCount,
	float CircleScale,
	float LineScale,
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
	TArray<FVector4f>* OutSurfaceTargets = nullptr,
	EVisVineGPUDebugStage DebugStage = EVisVineGPUDebugStage::Smooth,
	TArray<FVector4f>* OutStageCenterPoints = nullptr,
	const FVineSCGPUBuffers* GPULines = nullptr)
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

	// Trip A: when GPULines is supplied, the prepped path geometry is already GPU-resident
	// (concatenated across sources). Take counts from it and skip the CPU upload; otherwise
	// fall back to the CPU arrays.
	const bool bUseGPULines = (GPULines != nullptr && GPULines->IsValid());
	const uint32 PathPointCount = bUseGPULines ? uint32(GPULines->PointCount) : uint32(PathPoints.Num());
	const uint32 SegmentCount = bUseGPULines ? uint32(GPULines->SegmentCount) : uint32(SegmentMeta.Num());
	const uint32 ProfileCount = bTube ? FMath::Max(TubeProfileCount, 3u) : 2u;
	const uint32 OutputVertexCount = PathPointCount * ProfileCount;
	const uint32 OutputIndexCount = bTube ? SegmentCount * ProfileCount * 6u : SegmentCount * 6u;
	const int32 SafePostProjectionSmoothIterations = FMath::Max(0, PostProjectionSmoothIterations);
	const int32 SafePostProjectionSmoothKernelRadius = FMath::Max(1, PostProjectionSmoothKernelRadius);
	const int32 SafePostProjectionSmallSmoothIterations = FMath::Max(0, PostProjectionSmallSmoothIterations);
	const float SafePostProjectionSmoothAngleStrength = FMath::Clamp(PostProjectionSmoothAngleStrength, 0.0f, 1.0f);
	const uint32 SafeNoiseIterations = uint32(FMath::Max(0, NoiseIterations));
	if (PathPointCount == 0
		|| (!bUseGPULines && uint32(PathPointCurveU.Num()) != PathPointCount)
		|| SegmentCount == 0
		|| OutputVertexCount == 0
		|| OutputIndexCount == 0)
	{
		return false;
	}

	const uint32 VoxelCount = uint32(VoxelData.Cells.Num());
	if (VoxelCount == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU_Voxel] No voxel data available."));
		return false;
	}

	// Convert voxel data to GPU-compatible format
	TArray<FVector4f> GPUVoxelTargetPositions;
	TArray<FVector4f> GPUVoxelNormals;
	TArray<FIntVector4> GPUVoxelCells;
	GPUVoxelTargetPositions.Reserve(VoxelCount);
	GPUVoxelNormals.Reserve(VoxelCount);
	GPUVoxelCells.Reserve(VoxelCount);

	auto IsFiniteVector = [](const FVector& Vector)
	{
		return FMath::IsFinite(Vector.X) && FMath::IsFinite(Vector.Y) && FMath::IsFinite(Vector.Z);
	};

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
	}
	BuildVoxelUploadMs = (FPlatformTime::Seconds() - BuildVoxelUploadStartSeconds) * 1000.0;

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

	TArray<uint32> GPUVoxelHashSlots;
	uint32 GPUVoxelHashSlotCount = 0u;
	const double BuildHashStartSeconds = FPlatformTime::Seconds();
	if (!BuildVineVoxelHashSlots(GPUVoxelCells, GPUVoxelHashSlots, GPUVoxelHashSlotCount))
	{
		GPUVoxelHashSlots.Init(0u, 1);
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU_Voxel] Failed to build voxel hash slots; shader will use linear voxel lookup fallback."));
	}
	BuildHashMs = (FPlatformTime::Seconds() - BuildHashStartSeconds) * 1000.0;

	FVineTargetBucketBuffers TargetBuckets;
	const double BuildTargetBucketsStartSeconds = FPlatformTime::Seconds();
	const FVector3f TargetBucketOrigin = FVector3f(VoxelData.VoxelOrigin);
	if (!BuildVineTargetBucketBuffers(GPUVoxelTargetPositions, TargetBucketOrigin, SafeVoxelSize, TargetBuckets))
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU_Voxel] Failed to build target-position buckets; shader will use voxel-cell fallback."));
	}
	BuildTargetBucketsMs = (FPlatformTime::Seconds() - BuildTargetBucketsStartSeconds) * 1000.0;

	const uint64 VertexReadbackBytes64 = uint64(OutputVertexCount) * sizeof(FVector4f);
	const uint64 UVReadbackBytes64 = uint64(OutputVertexCount) * sizeof(FVector2f);
	const uint64 IndexReadbackBytes64 = uint64(OutputIndexCount) * sizeof(uint32);
	const uint64 SurfaceTargetReadbackBytes64 = uint64(PathPointCount) * sizeof(FVector4f);
	if (VertexReadbackBytes64 > MAX_uint32 || UVReadbackBytes64 > MAX_uint32 || IndexReadbackBytes64 > MAX_uint32 || SurfaceTargetReadbackBytes64 > MAX_uint32)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU_Voxel] Output is too large for readback. Vertices=%u Indices=%u"), OutputVertexCount, OutputIndexCount);
		return false;
	}

	const uint32 VertexReadbackBytes = uint32(VertexReadbackBytes64);
	const uint32 UVReadbackBytes = uint32(UVReadbackBytes64);
	const uint32 IndexReadbackBytes = uint32(IndexReadbackBytes64);
	const uint32 SurfaceTargetReadbackBytes = uint32(SurfaceTargetReadbackBytes64);
	FRHIGPUBufferReadback* VertexReadback = new FRHIGPUBufferReadback(TEXT("VVVoxel_VertexReadback"));
	FRHIGPUBufferReadback* UVReadback = new FRHIGPUBufferReadback(TEXT("VVVoxel_UVReadback"));
	FRHIGPUBufferReadback* IndexReadback = new FRHIGPUBufferReadback(TEXT("VVVoxel_IndexReadback"));
	FRHIGPUBufferReadback* SurfaceTargetReadback = new FRHIGPUBufferReadback(TEXT("VVVoxel_SurfaceTargetReadback"));
	// Stage-driven intermediate readbacks (only the selected stage is copied back).
	FRHIGPUBufferReadback* StageCenterReadback = OutStageCenterPoints ? new FRHIGPUBufferReadback(TEXT("VVVoxel_StageCenterReadback")) : nullptr;
	bool bRenderWorkQueued = false;

	// Trip A: the concatenated GPU-resident line buffers (null when taking the CPU path).
	TRefCountPtr<FRDGPooledBuffer> GPULinePoints = bUseGPULines ? GPULines->PathPoints : nullptr;
	TRefCountPtr<FRDGPooledBuffer> GPULineMeta = bUseGPULines ? GPULines->PathPointMeta : nullptr;
	TRefCountPtr<FRDGPooledBuffer> GPULineSeg = bUseGPULines ? GPULines->SegmentMeta : nullptr;
	// CurveU rides as a CPU array (uploaded fresh below) rather than a pooled float buffer.
	TArray<float> GPULineCurveUCPU = bUseGPULines ? GPULines->CurveUCPU : TArray<float>();
	{
		float CuMx = 0.0f;
		for (float V : GPULineCurveUCPU) CuMx = FMath::Max(CuMx, V);
		UE_LOG(LogTemp, Warning, TEXT("[VineConsumeCurveU] bUseGPULines=%d GPULineCurveUCPU.Num=%d max=%.3f PathPointCount=%u"),
			bUseGPULines ? 1 : 0, GPULineCurveUCPU.Num(), CuMx, PathPointCount);
	}

	const double EnqueueAndFlushStartSeconds = FPlatformTime::Seconds();
	ENQUEUE_RENDER_COMMAND(VVVoxelGPU)(
		[PathPoints, PathPointAxes, PathPointMeta, PathPointCurveU, SegmentMeta,
		 bUseGPULines, GPULinePoints, GPULineMeta, GPULineCurveUCPU = MoveTemp(GPULineCurveUCPU), GPULineSeg,
		 GPUVoxelCells, GPUVoxelHashSlots, GPUVoxelNormals, GPUVoxelTargetPositions,
		 TargetBuckets, TargetBucketOrigin,
		 VertexReadback, UVReadback, IndexReadback, SurfaceTargetReadback, StageCenterReadback, VertexReadbackBytes, UVReadbackBytes, IndexReadbackBytes, SurfaceTargetReadbackBytes,
		 PathPointCount, SegmentCount, VoxelCount, ProfileCount, OutputVertexCount, OutputIndexCount,
		 bTube, CircleScale, LineScale, VinesOffset, TinyZJitterStrength, SafePostProjectionSmoothIterations, SafePostProjectionSmoothKernelRadius, SafePostProjectionSmallSmoothIterations, SafePostProjectionSmoothAngleStrength, bResampleSurface, ResampleTargetDistance, GPUVoxelHashSlotCount,
		 CurlNoiseStrength, CurlNoiseFrequency, PerlinNoiseStrength, PerlinNoiseFrequency, SafeNoiseIterations, DebugStage,
		 VoxelOrigin = FVector3f(VoxelData.VoxelOrigin), VoxelSize = float(VoxelData.VoxelSize),
		 &bRenderWorkQueued](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			// Trip A: source the path geometry from the concatenated GPU buffers (register,
			// no upload) when available; otherwise upload the CPU arrays as before. The rest
			// of the graph consumes these purely via .SRV, so nothing downstream changes.
			CSHepler::FRDGStructuredBufferRefs PathPointBuffer;
			CSHepler::FRDGStructuredBufferRefs PathPointAxisBuffer;
			CSHepler::FRDGStructuredBufferRefs PathPointMetaBuffer;
			CSHepler::FRDGStructuredBufferRefs PathPointCurveUBuffer;
			CSHepler::FRDGStructuredBufferRefs SegmentMetaBuffer;
			if (bUseGPULines)
			{
				auto RegisterSRVOnly = [&GraphBuilder](const TRefCountPtr<FRDGPooledBuffer>& Pooled, const TCHAR* Name)
				{
					CSHepler::FRDGStructuredBufferRefs Refs;
					Refs.Buffer = GraphBuilder.RegisterExternalBuffer(Pooled, Name);
					Refs.SRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Refs.Buffer));
					return Refs;
				};
				PathPointBuffer = RegisterSRVOnly(GPULinePoints, TEXT("VVVoxel.PathPoints.GPU"));
				PathPointMetaBuffer = RegisterSRVOnly(GPULineMeta, TEXT("VVVoxel.PathPointMeta.GPU"));
				// CurveU as float4 (.x), uploaded fresh from the concatenated CPU array.
				// KNOWN ISSUE: this reads as 0 in FVVVoxelCS on the fused path (registered-external
				// siblings) — under investigation; r.Vine.SC.FusedPath defaults to 0 until fixed.
				TArray<FVector4f> CurveU4;
				CurveU4.SetNumUninitialized(GPULineCurveUCPU.Num());
				for (int32 i = 0; i < GPULineCurveUCPU.Num(); ++i) CurveU4[i] = FVector4f(GPULineCurveUCPU[i], 0.0f, 0.0f, 0.0f);
				PathPointCurveUBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, CurveU4, TEXT("VVVoxel.PathPointCurveU.GPU"));
				SegmentMetaBuffer = RegisterSRVOnly(GPULineSeg, TEXT("VVVoxel.SegmentMeta.GPU"));
				// The GPU SC path carries no per-point axis; zero-fill (matches BuildVVGPUInput's empty axes).
				PathPointAxisBuffer = CSHepler::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), PathPointCount, TEXT("VVVoxel.PathPointAxes.Zero"), true, true);
				AddClearUAVPass(GraphBuilder, PathPointAxisBuffer.UAV, 0u);
			}
			else
			{
				PathPointBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, PathPoints, TEXT("VVVoxel.PathPoints"));
				PathPointAxisBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, PathPointAxes, TEXT("VVVoxel.PathPointAxes"));
				PathPointMetaBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, PathPointMeta, TEXT("VVVoxel.PathPointMeta"));
				TArray<FVector4f> CurveU4;
				CurveU4.SetNumUninitialized(PathPointCurveU.Num());
				for (int32 i = 0; i < PathPointCurveU.Num(); ++i) CurveU4[i] = FVector4f(PathPointCurveU[i], 0.0f, 0.0f, 0.0f);
				PathPointCurveUBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, CurveU4, TEXT("VVVoxel.PathPointCurveU"));
				SegmentMetaBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, SegmentMeta, TEXT("VVVoxel.SegmentMeta"));
			}
			const CSHepler::FRDGStructuredBufferRefs VoxelCellsBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, GPUVoxelCells, TEXT("VVVoxel.VoxelCells"));
			const CSHepler::FRDGStructuredBufferRefs VoxelHashSlotsBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, GPUVoxelHashSlots, TEXT("VVVoxel.VoxelHashSlots"));
			const CSHepler::FRDGStructuredBufferRefs VoxelNormalsBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, GPUVoxelNormals, TEXT("VVVoxel.VoxelNormals"));
			const CSHepler::FRDGStructuredBufferRefs VoxelTargetPositionsBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, GPUVoxelTargetPositions, TEXT("VVVoxel.VoxelTargetPositions"));
			const CSHepler::FRDGStructuredBufferRefs TargetBucketRangesBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, TargetBuckets.Ranges, TEXT("VVVoxel.TargetBucketRanges"));
			const CSHepler::FRDGStructuredBufferRefs TargetBucketRangeCountsBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, TargetBuckets.RangeCounts, TEXT("VVVoxel.TargetBucketRangeCounts"));
			const CSHepler::FRDGStructuredBufferRefs TargetBucketVoxelIndicesBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, TargetBuckets.VoxelIndices, TEXT("VVVoxel.TargetBucketVoxelIndices"));
			const CSHepler::FRDGStructuredBufferRefs TargetBucketHashSlotsBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, TargetBuckets.HashSlots, TEXT("VVVoxel.TargetBucketHashSlots"));
			const CSHepler::FRDGStructuredBufferRefs OutVertexBuffer = CSHepler::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), OutputVertexCount, TEXT("VVVoxel.OutVertices"), true, true);
			const CSHepler::FRDGStructuredBufferRefs OutUVBuffer = CSHepler::CreateStructuredBuffer(GraphBuilder, sizeof(FVector2f), OutputVertexCount, TEXT("VVVoxel.OutUVs"), true, true);
			const CSHepler::FRDGStructuredBufferRefs OutIndexBuffer = CSHepler::CreateStructuredBuffer(GraphBuilder, sizeof(uint32), OutputIndexCount, TEXT("VVVoxel.OutIndices"), true, true);
			CSHepler::FRDGStructuredBufferRefs PathPointTangentA = CSHepler::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), PathPointCount, TEXT("VVVoxel.PathPointTangentsA"), true, true);
			CSHepler::FRDGStructuredBufferRefs PathPointNormalA = CSHepler::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), PathPointCount, TEXT("VVVoxel.PathPointNormalsA"), true, true);
			CSHepler::FRDGStructuredBufferRefs PathPointFrameNormalA = CSHepler::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), PathPointCount, TEXT("VVVoxel.PathPointFrameNormalsA"), true, true);
			CSHepler::FRDGStructuredBufferRefs PathPointSurfaceTargetA = CSHepler::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), PathPointCount, TEXT("VVVoxel.PathPointSurfaceTargetsA"), true, true);
			CSHepler::FRDGStructuredBufferRefs PathPointSurfaceNormalA = CSHepler::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), PathPointCount, TEXT("VVVoxel.PathPointSurfaceNormalsA"), true, true);
			CSHepler::FRDGStructuredBufferRefs PathPointSurfaceTargetB = CSHepler::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), PathPointCount, TEXT("VVVoxel.PathPointSurfaceTargetsB"), true, true);
			CSHepler::FRDGStructuredBufferRefs PathPointSurfaceNormalB = CSHepler::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), PathPointCount, TEXT("VVVoxel.PathPointSurfaceNormalsB"), true, true);

			// GPU noise loop output (voxel projection + CurlNoise). Downstream passes read this as the geometry source.
			CSHepler::FRDGStructuredBufferRefs PathPointNoisedBuffer = CSHepler::CreateStructuredBuffer(GraphBuilder, sizeof(FVector4f), PathPointCount, TEXT("VVVoxel.PathPointsNoised"), true, true);

			TShaderMapRef<FVVVoxelNoiseCS> NoiseShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
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

			TShaderMapRef<FVVVoxelBuildAxesCS> BuildAxesShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
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

			TShaderMapRef<FVVVoxelPerlinNoiseCS> PerlinNoiseShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
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
			TShaderMapRef<FVVVoxelFinalProjectCS> FinalProjectShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
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

			if (StageCenterReadback && DebugStage == EVisVineGPUDebugStage::FinalProject) AddEnqueueCopyPass(GraphBuilder, StageCenterReadback, PathPointSurfaceTargetB.Buffer, SurfaceTargetReadbackBytes);

			// ===== Pass RS: Surface resample (moved BEFORE smoothing) =====
			// FinalProject wrote into the B buffers. Redistribute each vine's surface points to
			// uniform arc-length spacing first (B -> A), so the path smoothing ping-pong below
			// refines evenly-spaced points. Endpoints stay anchored. Read/Write pointers track the
			// latest buffer; each pass swaps them so downstream always reads the freshest result.
			CSHepler::FRDGStructuredBufferRefs* ReadSurfaceTarget = &PathPointSurfaceTargetB;
			CSHepler::FRDGStructuredBufferRefs* ReadSurfaceNormal = &PathPointSurfaceNormalB;
			CSHepler::FRDGStructuredBufferRefs* WriteSurfaceTarget = &PathPointSurfaceTargetA;
			CSHepler::FRDGStructuredBufferRefs* WriteSurfaceNormal = &PathPointSurfaceNormalA;
			if (bResampleSurface)
			{
				TShaderMapRef<FVVVoxelResampleSurfaceCS> ResampleSurfaceShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
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
			if (StageCenterReadback && DebugStage == EVisVineGPUDebugStage::Resample) AddEnqueueCopyPass(GraphBuilder, StageCenterReadback, ReadSurfaceTarget->Buffer, SurfaceTargetReadbackBytes);

			// ===== Pass B: Path smoothing ping-pong (now smooths the resampled surface) =====
			// Wide-kernel passes round off sharp Z-folds, then a few radius-1 passes do a light
			// final cleanup using only the immediate neighbors. Pure geometric position smoothing;
			// normals are passed through and rebuilt later by BuildParallelTransportFrame.
			TShaderMapRef<FVVVoxelSmoothPathCS> SmoothPathShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
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
			const CSHepler::FRDGStructuredBufferRefs* GeometrySurfaceTargetBuffer = ReadSurfaceTarget;
			const CSHepler::FRDGStructuredBufferRefs* GeometrySurfaceNormalBuffer = ReadSurfaceNormal;

			// B stage center line is the smoothed geometry target.
			if (StageCenterReadback && DebugStage == EVisVineGPUDebugStage::Smooth)
			{
				AddEnqueueCopyPass(GraphBuilder, StageCenterReadback, GeometrySurfaceTargetBuffer->Buffer, SurfaceTargetReadbackBytes);
			}

			// Rebuild tangents from the final (post line-smoothing) surface targets so the cross-section
			// axis stays perpendicular to the mesh geometry, then parallel-transport the roll axis along
			// each line to minimize twist. The parallel-transport frame is the final axis frame.
			TShaderMapRef<FVVVoxelBuildParallelTransportFrameCS> BuildTangentsFromSurfaceShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
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
			CSHepler::FRDGStructuredBufferRefs& FinalTangentsForBuild = PathPointTangentA;
			CSHepler::FRDGStructuredBufferRefs& FinalFrameNormalsForBuild = PathPointFrameNormalA;

			TShaderMapRef<FVVVoxelCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FVVVoxelCS::FParameters* Parameters = GraphBuilder.AllocParameters<FVVVoxelCS::FParameters>();
			Parameters->PathPoints = PathPointNoisedBuffer.SRV;
			Parameters->PathPointMeta = PathPointMetaBuffer.SRV;
			Parameters->PathPointCurveU = PathPointCurveUBuffer.SRV;
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
			Parameters->RW_OutVertices = OutVertexBuffer.UAV;
			Parameters->RW_OutUVs = OutUVBuffer.UAV;
			Parameters->RW_OutIndices = OutIndexBuffer.UAV;
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

			AddEnqueueCopyPass(GraphBuilder, VertexReadback, OutVertexBuffer.Buffer, VertexReadbackBytes);
			AddEnqueueCopyPass(GraphBuilder, UVReadback, OutUVBuffer.Buffer, UVReadbackBytes);
			AddEnqueueCopyPass(GraphBuilder, IndexReadback, OutIndexBuffer.Buffer, IndexReadbackBytes);
			AddEnqueueCopyPass(GraphBuilder, SurfaceTargetReadback, GeometrySurfaceTargetBuffer->Buffer, SurfaceTargetReadbackBytes);
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
		delete SurfaceTargetReadback;
		delete StageCenterReadback;
		return false;
	}

	OutVertices.SetNumZeroed(OutputVertexCount);
	OutUVs.SetNumZeroed(OutputVertexCount);
	OutIndices.SetNumZeroed(OutputIndexCount);
	if (OutSurfaceTargets)
	{
		OutSurfaceTargets->SetNumZeroed(PathPointCount);
	}
	if (OutStageCenterPoints) OutStageCenterPoints->SetNumZeroed(PathPointCount);
	bool bReadbackSucceeded = false;

	const double ReadbackFlushStartSeconds = FPlatformTime::Seconds();
	ENQUEUE_RENDER_COMMAND(VVVoxelGPUReadback)(
		[VertexReadback, UVReadback, IndexReadback, SurfaceTargetReadback, StageCenterReadback, VertexReadbackBytes, UVReadbackBytes, IndexReadbackBytes, SurfaceTargetReadbackBytes, &OutVertices, &OutUVs, &OutIndices, OutSurfaceTargets, OutStageCenterPoints, &bReadbackSucceeded](FRHICommandListImmediate& RHICmdList)
		{
			if (!VertexReadback || !UVReadback || !IndexReadback || !SurfaceTargetReadback)
			{
				return;
			}

			if (!VertexReadback->IsReady() || !UVReadback->IsReady() || !IndexReadback->IsReady() || !SurfaceTargetReadback->IsReady() || (StageCenterReadback && !StageCenterReadback->IsReady()))
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

			if (OutSurfaceTargets)
			{
				if (const FVector4f* SurfaceTargetPtr = static_cast<const FVector4f*>(SurfaceTargetReadback->Lock(SurfaceTargetReadbackBytes)))
				{
					FMemory::Memcpy(OutSurfaceTargets->GetData(), SurfaceTargetPtr, SurfaceTargetReadbackBytes);
					SurfaceTargetReadback->Unlock();
				}
				else
				{
					bLockedAll = false;
				}
			}

			if (OutStageCenterPoints && StageCenterReadback)
			{
				if (const FVector4f* StageCenterPtr = static_cast<const FVector4f*>(StageCenterReadback->Lock(SurfaceTargetReadbackBytes)))
				{
					FMemory::Memcpy(OutStageCenterPoints->GetData(), StageCenterPtr, SurfaceTargetReadbackBytes);
					StageCenterReadback->Unlock();
				}
				else
				{
					bLockedAll = false;
				}
			}

			delete VertexReadback;
			delete UVReadback;
			delete IndexReadback;
			delete SurfaceTargetReadback;
			delete StageCenterReadback;
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

static void LogVineGPUProjectionStats(
	const TCHAR* Label,
	const TArray<FVector4f>& PathPoints,
	const TArray<FVector4f>& SurfaceTargets,
	const TArray<FVector4f>& OutputVertices,
	uint32 ProfileCount,
	float VinesOffset)
{
	const int32 OutputPointCount = ProfileCount > 0u ? OutputVertices.Num() / int32(ProfileCount) : 0;
	const int32 PointCount = FMath::Min(FMath::Min(PathPoints.Num(), SurfaceTargets.Num()), OutputPointCount);
	if (PointCount <= 0)
	{
		return;
	}

	int32 HitCount = 0;
	int32 NeighborHitCount = 0;
	int32 LocalHitCount = 0;
	int32 BucketHitCount = 0;
	int32 FailCount = 0;
	int32 UnknownModeCount = 0;
	double ProjectionDistanceSum = 0.0;
	double ProjectionDistanceMax = 0.0;
	double FinalTargetDistanceSum = 0.0;
	double FinalTargetDistanceMin = TNumericLimits<double>::Max();
	double FinalTargetDistanceMax = 0.0;
	double FinalOffsetErrorSum = 0.0;
	double FinalOffsetErrorMax = 0.0;

	for (int32 PointIndex = 0; PointIndex < PointCount; ++PointIndex)
	{
		const FVector4f& PackedTarget = SurfaceTargets[PointIndex];
		const int32 SampleMode = FMath::RoundToInt(PackedTarget.W);
		if (SampleMode <= 0)
		{
			++FailCount;
			continue;
		}

		switch (SampleMode)
		{
		case 1:
			++NeighborHitCount;
			break;
		case 2:
			++LocalHitCount;
			break;
		case 3:
			++BucketHitCount;
			break;
		default:
			++UnknownModeCount;
			break;
		}

		const FVector Query(PathPoints[PointIndex].X, PathPoints[PointIndex].Y, PathPoints[PointIndex].Z);
		const FVector Target(PackedTarget.X, PackedTarget.Y, PackedTarget.Z);
		if (!IsFiniteVineVector(Query) || !IsFiniteVineVector(Target))
		{
			continue;
		}

		const FVector Center = GetVineOutputProfileCenter(OutputVertices, PointIndex, ProfileCount);
		if (!IsFiniteVineVector(Center))
		{
			continue;
		}

		++HitCount;
		const double ProjectionDistance = FVector::Dist(Query, Target);
		const double FinalTargetDistance = FVector::Dist(Center, Target);
		const double FinalOffsetError = FMath::Abs(FinalTargetDistance - double(VinesOffset));
		ProjectionDistanceSum += ProjectionDistance;
		ProjectionDistanceMax = FMath::Max(ProjectionDistanceMax, ProjectionDistance);
		FinalTargetDistanceSum += FinalTargetDistance;
		FinalTargetDistanceMin = FMath::Min(FinalTargetDistanceMin, FinalTargetDistance);
		FinalTargetDistanceMax = FMath::Max(FinalTargetDistanceMax, FinalTargetDistance);
		FinalOffsetErrorSum += FinalOffsetError;
		FinalOffsetErrorMax = FMath::Max(FinalOffsetErrorMax, FinalOffsetError);
	}

	UE_LOG(LogTemp, Display,
		TEXT("[VisVineGPUProjectionStats] %s sampleHits=%d/%d modes(neighbor=%d local=%d bucket=%d fail=%d unknown=%d) projectionDist(avg=%.3f max=%.3f) finalToTarget(avg=%.3f min=%.3f max=%.3f expectedOffset=%.3f offsetError(avg=%.3f max=%.3f)"),
		Label ? Label : TEXT("unknown"),
		HitCount,
		PointCount,
		NeighborHitCount,
		LocalHitCount,
		BucketHitCount,
		FailCount,
		UnknownModeCount,
		HitCount > 0 ? ProjectionDistanceSum / double(HitCount) : 0.0,
		ProjectionDistanceMax,
		HitCount > 0 ? FinalTargetDistanceSum / double(HitCount) : 0.0,
		HitCount > 0 ? FinalTargetDistanceMin : 0.0,
		FinalTargetDistanceMax,
		VinesOffset,
		HitCount > 0 ? FinalOffsetErrorSum / double(HitCount) : 0.0,
		FinalOffsetErrorMax);
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

	ApplyVineReferenceComponentsHiddenInGame(this);
	RebuildDisplayInstancesFromTransformArrays();
}

void AVineContainer::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	// ApplyVineReferenceComponentsHiddenInGame(this);
	// RebuildDisplayInstancesFromTransformArrays();
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

// Trip A: route the VisVine voxel dispatch through the GPU-resident concatenated SC
// line buffers instead of re-uploading CPU-derived arrays. 1=on (default), 0=off
// (legacy CPU-upload path) for A/B parity verification.
static TAutoConsoleVariable<int32> CVarVineSCFusedPath(
	TEXT("r.Vine.SC.FusedPath"),
	0,
	TEXT("Vine VisVine line hand-off: 1=feed GPU-resident SC buffers directly (Trip A; UV/CurveU still broken), 0=upload CPU arrays (correct)."),
	ECVF_Default);

bool AVineContainer::VisVineGPUInternal()
{
	const TArray<FGeometryScriptPolyPath>& Lines = TubeLines;
	// Empty-Lines check is deferred: when the fused GPU path is taken the CPU TubeLines
	// are intentionally not built, so emptiness only matters for the CPU fallback below.

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

	if (CachedSurfaceVoxels.Cells.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] No cached surface voxel data for GPU voxel visualization."));
		return false;
	}

	// Trip A: decide the fused GPU hand-off up front so the whole CPU line prep
	// (smooth/resample/scale + BuildVVGPUInput; the CPU trace is skipped upstream) is
	// bypassed. r.Vine.SC.FusedPath=0 forces the CPU fallback below.
	FVineSCGPUBuffers ConcatenatedGPULines;
	const bool bFusedGPULines = (CVarVineSCFusedPath.GetValueOnGameThread() != 0)
		&& !VV.bVisVineGPUUseCPUForPostPasses
		&& TubeLineGPUBuffers.Num() > 0
		&& ConcatenateVineSCGPUBuffers(TubeLineGPUBuffers, ConcatenatedGPULines);

	if (!bFusedGPULines && Lines.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] No tube lines to visualize (CPU fallback path)."));
		return false;
	}

	// Path geometry for the voxel dispatch: filled by the CPU prep on the fallback path,
	// left empty on the fused path (DispatchVVGPU_Voxel reads ConcatenatedGPULines instead).
	TArray<FVector4f> PathPoints;
	TArray<FVector4f> PathPointAxes;
	TArray<FIntVector4> PathPointMeta;
	TArray<float> PathPointCurveU;
	TArray<FIntVector4> SegmentMeta;
	double PrepareLinesMs = 0.0;
	double BuildGPUInputMs = 0.0;

	if (!bFusedGPULines)
	{
	const TArray<float>& LineSourceScales = TubeLineSourceScales;
	const TArray<FVector>& LineSourceLocations = TubeLineSourceLocations;
	const TArray<FVineLinePointScaleData>& LinePointScales = TubeLinePointScales;

	TArray<FGeometryScriptPolyPath> PreparedLines;
	TArray<float> PreparedLineSourceScales;
	TArray<FVineLinePointScaleData> PreparedLinePointScales;
	const double PrepareLinesStartSeconds = FPlatformTime::Seconds();
	// Surface projection and noise are done entirely on the GPU (unified voxel projection),
	// so the CPU preparation only smooths and resamples the lines.
	PreparedLines.Reset();
	PreparedLines.Reserve(Lines.Num());
	PreparedLineSourceScales.Reset();
	PreparedLineSourceScales.Reserve(Lines.Num());
	PreparedLinePointScales.Reset();
	PreparedLinePointScales.Reserve(Lines.Num());

	if (VV.ResampleLength <= KINDA_SMALL_NUMBER)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] No valid tube lines after GPU preprocessing."));
		return false;
	}

	for (int32 LineIdx = 0; LineIdx < Lines.Num(); ++LineIdx)
	{
		const FGeometryScriptPolyPath& InputLine = Lines[LineIdx];
		if (!InputLine.Path.IsValid() || InputLine.Path->Num() < 2)
		{
			continue;
		}

		FGeometryScriptPolyPath Line = ClonePolyPath(InputLine);
		if (LineSourceLocations.IsValidIndex(LineIdx))
		{
			ApplyVVSCPointOffset(Line, LineSourceLocations[LineIdx]);
		}

		const float FallbackScale = LineSourceScales.IsValidIndex(LineIdx) ? LineSourceScales[LineIdx] : 1.0f;
		const TArray<float>* InputPointScales = LinePointScales.IsValidIndex(LineIdx) ? &LinePointScales[LineIdx].Values : nullptr;

		TArray<float> CurrentPointScales;
		BuildPreparedLinePointScales(Line, InputPointScales, Line.Path->Num(), FallbackScale, CurrentPointScales);

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

		if (CurrentPointScales.Num() != Line.Path->Num())
		{
			TArray<float> FixedPointScales;
			BuildPreparedLinePointScales(Line, &CurrentPointScales, Line.Path->Num(), FallbackScale, FixedPointScales);
			CurrentPointScales = MoveTemp(FixedPointScales);
		}

		PreparedLines.Add(Line);
		PreparedLineSourceScales.Add(FallbackScale);
		FVineLinePointScaleData& OutScaleData = PreparedLinePointScales.AddDefaulted_GetRef();
		OutScaleData.Values = MoveTemp(CurrentPointScales);
	}

	UE_LOG(LogTemp, Display,
		TEXT("[VisVinePrepTiming] GPU(NoCPUProjection) tube total=%.3f ms outputLines=%d"),
		(FPlatformTime::Seconds() - PrepareLinesStartSeconds) * 1000.0,
		PreparedLines.Num());

	// [Thickness diagnostic] Per-point scale (thickness) is CircleScale * CurveScale * PointScale,
	// where PointScale comes from TubeLinePointScales. If that member is empty/stale, thickness
	// silently falls back to the uniform per-line source scale, which looks very different.
	{
		int32 LinesWithPointScales = 0;
		int32 LinesUsingFallback = 0;
		float MinScale = TNumericLimits<float>::Max();
		float MaxScale = -TNumericLimits<float>::Max();
		double SumScale = 0.0;
		int32 ScaleSampleCount = 0;
		for (const FVineLinePointScaleData& ScaleData : PreparedLinePointScales)
		{
			if (ScaleData.Values.Num() > 0)
			{
				++LinesWithPointScales;
				for (const float Value : ScaleData.Values)
				{
					MinScale = FMath::Min(MinScale, Value);
					MaxScale = FMath::Max(MaxScale, Value);
					SumScale += Value;
					++ScaleSampleCount;
				}
			}
			else
			{
				++LinesUsingFallback;
			}
		}
		const float AvgScale = ScaleSampleCount > 0 ? float(SumScale / ScaleSampleCount) : 0.0f;
		UE_LOG(LogTemp, Warning,
			TEXT("[VisVineThickness] CircleScale=%.4f TubeLinePointScales.Num=%d preparedLines=%d linesWithPointScales=%d linesUsingFallback=%d pointScale[min=%.4f avg=%.4f max=%.4f n=%d]"),
			VV.CircleScale,
			TubeLinePointScales.Num(),
			PreparedLines.Num(),
			LinesWithPointScales,
			LinesUsingFallback,
			ScaleSampleCount > 0 ? MinScale : 0.0f,
			AvgScale,
			ScaleSampleCount > 0 ? MaxScale : 0.0f,
			ScaleSampleCount);
	}

	PrepareLinesMs = (FPlatformTime::Seconds() - PrepareLinesStartSeconds) * 1000.0;

	if (PreparedLines.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] No valid tube lines after GPU preprocessing."));
		return false;
	}

	const TArray<FVineLinePointAxisData> PreparedLinePointAxes;

	const double BuildGPUInputStartSeconds = FPlatformTime::Seconds();
	if (!BuildVVGPUInput(
		PreparedLines,
		VV,
		PreparedLineSourceScales,
		PreparedLinePointScales,
		PreparedLinePointAxes,
		PathPoints,
		PathPointAxes,
		PathPointMeta,
		PathPointCurveU,
		SegmentMeta))
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] Failed to build GPU input buffers."));
		return false;
	}
	BuildGPUInputMs = (FPlatformTime::Seconds() - BuildGPUInputStartSeconds) * 1000.0;
	} // end if (!bFusedGPULines): CPU line prep skipped entirely on the fused path

	if (bFusedGPULines)
	{
		UE_LOG(LogTemp, Display, TEXT("[VineSCFused] GPU-resident lines: points=%d segments=%d (CPU prep skipped)"),
			ConcatenatedGPULines.PointCount, ConcatenatedGPULines.SegmentCount);
	}

	TArray<FVector4f> OutVertices;
	TArray<FVector2f> OutUVs;
	TArray<uint32> OutIndices;
	TArray<FVector4f> SurfaceTargets;
	TArray<FVector4f> StageCenterPoints;
	const EVisVineGPUDebugStage DebugStage = SplineDebug.DebugStage;
	const bool bWantStageDraw = SplineDebug.bDrawDebugLines && DebugStage != EVisVineGPUDebugStage::None;

	const double DispatchStartSeconds = FPlatformTime::Seconds();
	if (VV.bVisVineGPUUseCPUForPostPasses)
	{
		// 方式 A：GPU 只跑 N/A/P/FP，CPU 接力 RS/B/FT/C。
		TArray<FVector4f> Noised, FPTarget, FPNormal;
		if (!DispatchVVGPU_VoxelToFP(
			PathPoints,
			PathPointAxes,
			PathPointMeta,
			VV.CurlNoiseScale / 10.0f,
			VV.CurlNoiseFre,
			VV.PerlinNoiseScale,
			VV.PerlinNoiseFre,
			VV.VisVineGPUNoiseIterations,
			VV.VinesOffset,
			CachedSurfaceVoxels,
			Noised,
			FPTarget,
			FPNormal))
		{
			UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] Voxel GPU (to-FP) dispatch/readback failed."));
			return false;
		}

		// CPU tail path only runs the GPU up to FP, so the stage debug draw can only show the
		// FinalProject center line regardless of the selected stage.
		StageCenterPoints = FPTarget;
		VineCPUTail::RunPostPasses(
			Noised,
			PathPointMeta,
			PathPointCurveU,
			SegmentMeta,
			FPTarget,
			FPNormal,
			VV.bVisVineGPUResampleSurfaceEnabled,
			VV.ResampleLength,
			VV.VisVineGPUPostProjectionSmoothIterations,
			VV.VisVineGPUPostProjectionSmoothKernelRadius,
			VV.VisVineGPUPostProjectionSmallSmoothIterations,
			VV.VisVineGPUPostProjectionSmoothAngleStrength,
			true,
			uint32(FMath::Max(VV.VisVineGPUTubeSegments, 3)),
			VV.CircleScale,
			VV.LineScale,
			VV.VinesOffset,
			0.1f,
			OutVertices,
			OutUVs,
			OutIndices,
			&SurfaceTargets);
	}
	else if (!DispatchVVGPU_Voxel(
		PathPoints,
		PathPointAxes,
		PathPointMeta,
		PathPointCurveU,
		SegmentMeta,
		true,
		uint32(FMath::Max(VV.VisVineGPUTubeSegments, 3)),
		VV.CircleScale,
		VV.LineScale,
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
		CachedSurfaceVoxels,
		OutVertices,
		OutUVs,
		OutIndices,
		&SurfaceTargets,
		DebugStage,
		bWantStageDraw ? &StageCenterPoints : nullptr,
		bFusedGPULines ? &ConcatenatedGPULines : nullptr))
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] Voxel GPU dispatch/readback failed."));
		return false;
	}
	const double DispatchMs = (FPlatformTime::Seconds() - DispatchStartSeconds) * 1000.0;

	LogVineGPUProjectionStats(
		TEXT("tube"),
		PathPoints,
		SurfaceTargets,
		OutVertices,
		uint32(FMath::Max(VV.VisVineGPUTubeSegments, 3)),
		// VinesOffset 已在 Pass FP 写入 SurfaceTarget，扫掠中心与最终中心线应几乎重合，
		// 故期望偏移为 0（仅余 TinyJitter）。
		0.0f);

	if (bWantStageDraw)
	{
		DrawDebugVineCenterLines(StageCenterPoints, PathPointMeta);
	}
	else
	{
		ClearDebugVineSplineActor();
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
	CachedSurfaceVoxels = FCSSurfaceVoxelData();
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

	// Voxelized surface data is consumed by the GPU visualization path.
	// The cache is refreshed during GenerateVines().
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.GetBoxSceneSurfaceVoxels"));
		CachedSurfaceVoxels = ReadbackBoxSceneSurfaceVoxelsSync(SC.VoxelSize, TEXT("[GenerateVines.SurfaceVoxels]"));
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
					SCSourceTransform, TargetTransforms, SC.bUseComputeShader, SourceGPUBuffers.Get());
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
		DrawVineGPUProjectionVoxelDebugPoints(
			GetWorld(),
			CachedSurfaceVoxels,
			SC.VoxelSize,
			GPUProjectionDebug.GPUProjectionVoxelDebugDuration,
			GPUProjectionDebug.GPUProjectionVoxelDebugPointLimit,
			GPUProjectionDebug.bGPUProjectionVoxelDebugPointsPersistent,
			GPUProjectionDebug.GPUProjectionVoxelCenterPointSize,
			GPUProjectionDebug.GPUProjectionVoxelTargetPointSize,
			GPUProjectionDebug.GPUProjectionVoxelCenterColor,
			GPUProjectionDebug.GPUProjectionVoxelTargetColor);
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
	UWorld* World = GetWorld();
	if (!World)
	{
		return 0;
	}

	const float SafeVoxelSize = FMath::Max(SC.VoxelSize, 1.0e-3f);

	if (bUseCachedVoxels)
	{
		if (CachedSurfaceVoxels.Cells.Num() == 0)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[VineVoxelDebug] No cached surface voxel data on %s. Run GenerateVines before drawing cached voxel data."),
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

		CachedSurfaceVoxels = ReadbackBoxSceneSurfaceVoxelsSync(SafeVoxelSize, TEXT("[VisVineGPU.SurfaceVoxels]"));
	}

	const FCSSurfaceVoxelData& VoxelData = CachedSurfaceVoxels;
	const int32 AvailableCount = VoxelData.VoxelCount >= 0
		? FMath::Min(VoxelData.VoxelCount, VoxelData.Positions.Num())
		: VoxelData.Positions.Num();
	if (AvailableCount <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineVoxelDebug] No weighted surface voxel data generated on %s."), *GetActorNameOrLabel());
		return 0;
	}

	const int32 DrawLimit = SurfaceVoxelDebug.SurfaceVoxelMaxArrowsToDraw > 0
		? FMath::Min(SurfaceVoxelDebug.SurfaceVoxelMaxArrowsToDraw, AvailableCount)
		: AvailableCount;
	const float SafeArrowLength = SurfaceVoxelDebug.SurfaceVoxelArrowLength > 0.0f
		? SurfaceVoxelDebug.SurfaceVoxelArrowLength
		: FMath::Max(VoxelData.VoxelSize, SC.VoxelSize);
	const float EffectiveArrowDuration = Duration > 0.0f ? Duration : SurfaceVoxelDebug.SurfaceVoxelArrowDuration;
	const float SafeDuration = FMath::Max(0.0f, EffectiveArrowDuration);
	const float SafeThickness = FMath::Max(0.0f, SurfaceVoxelDebug.SurfaceVoxelArrowThickness);
	const float SafeVoxelPointSize = FMath::Max(0.0f, SurfaceVoxelDebug.SurfaceVoxelCenterPointSize);
	const float SafeTargetPointSize = FMath::Max(0.0f, SurfaceVoxelDebug.SurfaceVoxelWeightedTargetPointSize);
	const float ArrowHeadSize = FMath::Max(SafeArrowLength * 0.15f, SafeThickness * 4.0f);
	const FColor ArrowDrawColor = SurfaceVoxelDebug.SurfaceVoxelArrowColor.ToFColor(true);
	const FColor VoxelCenterDrawColor = SurfaceVoxelDebug.SurfaceVoxelCenterColor.ToFColor(true);
	const FColor WeightedTargetDrawColor = SurfaceVoxelDebug.SurfaceVoxelWeightedTargetColor.ToFColor(true);
	const FColor CenterToTargetDrawColor = SurfaceVoxelDebug.SurfaceVoxelCenterToTargetColor.ToFColor(true);
	auto IsFiniteVector = [](const FVector& Vector)
	{
		return FMath::IsFinite(Vector.X) && FMath::IsFinite(Vector.Y) && FMath::IsFinite(Vector.Z);
	};

	int32 DrawnCount = 0;
	int32 SkippedInvalidPositionCount = 0;
	int32 SkippedInvalidTargetCount = 0;
	int32 CoincidentTargetCount = 0;
	int32 CenterToTargetArrowCount = 0;
	int32 NormalArrowCount = 0;
	int32 UpFallbackCount = 0;
	for (int32 Index = 0; Index < DrawLimit; ++Index)
	{
		FVector VoxelCenter = VoxelData.Positions[Index];
		if (!IsFiniteVector(VoxelCenter) && VoxelData.Cells.IsValidIndex(Index))
		{
			const FIntVector& Cell = VoxelData.Cells[Index];
			VoxelCenter = FVector(
				(double(Cell.X) + 0.5) * SafeVoxelSize + VoxelData.VoxelOrigin.X,
				(double(Cell.Y) + 0.5) * SafeVoxelSize + VoxelData.VoxelOrigin.Y,
				(double(Cell.Z) + 0.5) * SafeVoxelSize + VoxelData.VoxelOrigin.Z);
		}
		if (!IsFiniteVector(VoxelCenter))
		{
			++SkippedInvalidPositionCount;
			continue;
		}

		FVector WeightedTarget = VoxelCenter;
		bool bValidWeightedTarget = false;
		if (VoxelData.TargetPositions.IsValidIndex(Index) && IsFiniteVector(VoxelData.TargetPositions[Index]))
		{
			WeightedTarget = VoxelData.TargetPositions[Index];
			const double TargetDistanceSq = FVector::DistSquared(WeightedTarget, VoxelCenter);
			bValidWeightedTarget = true;
			if (TargetDistanceSq <= UE_DOUBLE_KINDA_SMALL_NUMBER)
			{
				++CoincidentTargetCount;
			}
		}

		if (!bValidWeightedTarget)
		{
			++SkippedInvalidTargetCount;
		}

		FVector ArrowDirection = VoxelData.Normals.IsValidIndex(Index) ? VoxelData.Normals[Index] : FVector::UpVector;
		if (IsFiniteVector(ArrowDirection) && ArrowDirection.Normalize())
		{
			++NormalArrowCount;
		}
		else
		{
			ArrowDirection = FVector::UpVector;
			++UpFallbackCount;
		}

		const FVector ArrowStart = bValidWeightedTarget ? WeightedTarget : VoxelCenter;
		DrawDebugDirectionalArrow(
			World,
			ArrowStart,
			ArrowStart + ArrowDirection * SafeArrowLength,
			ArrowHeadSize,
			ArrowDrawColor,
			SurfaceVoxelDebug.bSurfaceVoxelArrowPersistentLines,
			SafeDuration,
			0,
			SafeThickness);

		if (SurfaceVoxelDebug.bSurfaceVoxelDrawVoxelCenters && SafeVoxelPointSize > 0.0f)
		{
			DrawDebugPoint(
				World,
				VoxelCenter,
				SafeVoxelPointSize,
				VoxelCenterDrawColor,
				SurfaceVoxelDebug.bSurfaceVoxelArrowPersistentLines,
				SafeDuration,
				0);
		}

		if (bValidWeightedTarget && SurfaceVoxelDebug.bSurfaceVoxelDrawWeightedTargets && SafeTargetPointSize > 0.0f)
		{
			DrawDebugPoint(
				World,
				WeightedTarget,
				SafeTargetPointSize,
				WeightedTargetDrawColor,
				SurfaceVoxelDebug.bSurfaceVoxelArrowPersistentLines,
				SafeDuration,
				0);
		}

		if (bValidWeightedTarget && SurfaceVoxelDebug.bSurfaceVoxelDrawCenterToTargetLines)
		{
			const FVector CenterToTarget = WeightedTarget - VoxelCenter;
			const double CenterToTargetLengthSq = CenterToTarget.SizeSquared();
			if (CenterToTargetLengthSq > UE_DOUBLE_KINDA_SMALL_NUMBER)
			{
				DrawDebugDirectionalArrow(
					World,
					VoxelCenter,
					WeightedTarget,
					ArrowHeadSize,
					CenterToTargetDrawColor,
					SurfaceVoxelDebug.bSurfaceVoxelArrowPersistentLines,
					SafeDuration,
					0,
					SafeThickness);
				++CenterToTargetArrowCount;
			}
			else
			{
				DrawDebugLine(
					World,
					VoxelCenter,
					WeightedTarget,
					CenterToTargetDrawColor,
					SurfaceVoxelDebug.bSurfaceVoxelArrowPersistentLines,
					SafeDuration,
					0,
					SafeThickness);
			}
		}

		++DrawnCount;
	}

	UE_LOG(LogTemp, Log,
		TEXT("[VineVoxelDebug] Drawn weighted surface voxel arrows on %s. Drawn=%d Available=%d Positions=%d Normals=%d Targets=%d Cells=%d InvalidPositions=%d InvalidTargets=%d CoincidentTargets=%d CenterToTargetArrows=%d NormalArrows=%d UpFallbacks=%d VoxelSize=%.3f"),
		*GetActorNameOrLabel(),
		DrawnCount,
		AvailableCount,
		VoxelData.Positions.Num(),
		VoxelData.Normals.Num(),
		VoxelData.TargetPositions.Num(),
		VoxelData.Cells.Num(),
		SkippedInvalidPositionCount,
		SkippedInvalidTargetCount,
		CoincidentTargetCount,
		CenterToTargetArrowCount,
		NormalArrowCount,
		UpFallbackCount,
		VoxelData.VoxelSize);
	return DrawnCount;
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

	if (GeneratorTimeCode == -1)
	{
		const FDateTime Now = FDateTime::Now();
		GeneratorTimeCode =
			int64(Now.GetYear() % 100) * 100000000LL +
			int64(Now.GetMonth()) * 1000000LL +
			int64(Now.GetDay()) * 10000LL +
			int64(Now.GetHour()) * 100LL +
			int64(Now.GetMinute());
	}

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
	float UVScaleInfluence,
	float UVScaleFloor,
	float UVScalePower,
	float UVLengthScale,
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
		// TEMP DIAGNOSTIC: read back CurveU2 to localize the fused UV (V=CurveU) all-zero bug.
		const uint32 CurveUReadbackBytes = uint32(sizeof(float) * uint64(PathPointsReadbackCount));
		FRHIGPUBufferReadback* CurveUReadback = new FRHIGPUBufferReadback(TEXT("SpaceColonizationQueue_CurveU"));
		TArray<float> CurveUData;
		// Trip A: when requested, extract the prepped output buffers as pooled (external)
		// RDG buffers inside the render command below so they outlive the graph and can be
		// handed to VisVine. Assigned on the render thread; valid after FlushRenderingCommands.
		const bool bExportGPUBuffers = (OutGPUBuffers != nullptr);
		TRefCountPtr<FRDGPooledBuffer> ExportedPathPoints2;
		TRefCountPtr<FRDGPooledBuffer> ExportedPathPointMeta2;
		TRefCountPtr<FRDGPooledBuffer> ExportedPathPointCurveU2;
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
				 PathPointsReadback, PathPointsReadbackBytes, CurveUReadback, CurveUReadbackBytes, PathPointCapacity, SegmentCapacity,
				 ResampleLength, PathPoint2Capacity, Segment2Capacity,
				 EmitCurveLUT = MoveTemp(EmitCurveLUT), CurveLUTSize,
				 UVScaleInfluence, UVScaleFloor, UVScalePower, UVLengthScale, ScatterDistance,
				 bExportGPUBuffers, &ExportedPathPoints2, &ExportedPathPointMeta2, &ExportedPathPointCurveU2, &ExportedSegmentMeta2,
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
					// ---- Stage B3 (prep port): CurveScale (thickness .w) + scale-weighted CurveU ----
					CREATE_RDG_STRUCTURED_UPLOAD_SRV(CurveLUT, float, EmitCurveLUT, TEXT("SpaceColonizationQueue_CurveLUT"))
					CREATE_RDG_STRUCTURED_UAV_SRV(PathPointCurveU2, float, PathPoint2Capacity, TEXT("SpaceColonizationQueue_PathPointCurveU2"))
					{
						TShaderMapRef<FSpaceColonizationCurveCS> CurveShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
						FSpaceColonizationCurveCS::FParameters* P = GraphBuilder.AllocParameters<FSpaceColonizationCurveCS::FParameters>();
						P->CurveLUT = CurveLUTSRV;
						P->CurveLineOffset = NewLineOffsetSRV;
						P->CurveLineLength = NewLineLengthSRV;
						P->CurveLineCount = NewCountsSRV;
						P->RW_CurvePoints = PathPoints2UAV;
						P->RW_PathPointCurveU2 = PathPointCurveU2UAV;
						P->CurveLUTSize = CurveLUTSize;
						P->UVScaleInfluence = UVScaleInfluence;
						P->UVScaleFloor = UVScaleFloor;
						P->UVScalePower = UVScalePower;
						P->UVLengthScale = UVLengthScale;
						GraphBuilder.AddPass(RDG_EVENT_NAME("SpaceColonizationQueue.CurveScaleU"), P, ERDGPassFlags::Compute,
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
						ExportedPathPointCurveU2 = GraphBuilder.ConvertToExternalBuffer(PathPointCurveU2Buffer);
						ExportedSegmentMeta2 = GraphBuilder.ConvertToExternalBuffer(SegmentMeta2Buffer);
					}

					// The resampled counts (post-resample) drive the readback slice.
					AddEnqueueCopyPass(GraphBuilder, LineCountsReadback, NewCountsBuffer, LineCountsReadbackBytes);
					AddEnqueueCopyPass(GraphBuilder, PathPointsReadback, PathPoints2Buffer, PathPointsReadbackBytes);
					AddEnqueueCopyPass(GraphBuilder, CurveUReadback, PathPointCurveU2Buffer, CurveUReadbackBytes);

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
			delete CurveUReadback;
			CurveUReadback = nullptr;
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
				 IterationState0DebugReadbacks, IterationState1DebugReadbacks, TargetReadbackBytes, StateReadbackBytes, UIntReadbackBytes, LineCountsReadback, LineCountsReadbackBytes, &LineCountsData, PathPointsReadback, PathPointsReadbackBytes, &PathPointsData, CurveUReadback, CurveUReadbackBytes, &CurveUData,
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
					LockSpaceColonizationReadbackToArray(CurveUReadback, CurveUReadbackBytes, int32(CurveUReadbackBytes / sizeof(float)), CurveUData);

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

		// TEMP DIAGNOSTIC: SC-solve CurveU2 range. If [0,0] the CurveScaleU pass output is dead;
		// if non-zero, the loss is downstream in the concat/consume.
		{
			float CuMin = TNumericLimits<float>::Max();
			float CuMax = -TNumericLimits<float>::Max();
			const uint32 GPUTotalPoints = LineCountsData.IsValidIndex(1) ? LineCountsData[1] : 0u;
			const int32 CuCount = FMath::Min<int32>(int32(GPUTotalPoints), CurveUData.Num());
			for (int32 i = 0; i < CuCount; ++i)
			{
				CuMin = FMath::Min(CuMin, CurveUData[i]);
				CuMax = FMath::Max(CuMax, CurveUData[i]);
			}
			UE_LOG(LogTemp, Warning, TEXT("[VineCurveUCheck] SC-solve CurveU2[min=%.4f max=%.4f] n=%d"),
				CuCount > 0 ? CuMin : 0.0f, CuCount > 0 ? CuMax : 0.0f, CuCount);
		}
		delete CurveUReadback;
		CurveUReadback = nullptr;

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
			OutGPUBuffers->PathPointCurveU = MoveTemp(ExportedPathPointCurveU2);
			OutGPUBuffers->SegmentMeta = MoveTemp(ExportedSegmentMeta2);
			OutGPUBuffers->LineCount = LineCountsData.IsValidIndex(0) ? int32(LineCountsData[0]) : 0;
			OutGPUBuffers->PointCount = LineCountsData.IsValidIndex(1) ? int32(LineCountsData[1]) : 0;
			OutGPUBuffers->SegmentCount = LineCountsData.IsValidIndex(2) ? int32(LineCountsData[2]) : 0;
			// CurveU as a CPU array (see FVineSCGPUBuffers::CurveUCPU note). Sliced to the
			// compact [0, PointCount) range from the readback.
			{
				const int32 CuCount = FMath::Min<int32>(OutGPUBuffers->PointCount, CurveUData.Num());
				OutGPUBuffers->CurveUCPU.Reset();
				OutGPUBuffers->CurveUCPU.Append(CurveUData.GetData(), FMath::Max(CuCount, 0));
				float FillMx = 0.0f;
				for (float V : OutGPUBuffers->CurveUCPU) FillMx = FMath::Max(FillMx, V);
				UE_LOG(LogTemp, Warning, TEXT("[VineFillCurveU] CuCount=%d CurveUData.Num=%d CurveUCPU.Num=%d max=%.3f"),
					CuCount, CurveUData.Num(), OutGPUBuffers->CurveUCPU.Num(), FillMx);
			}
		}

		LogSpaceColonizationCSDebugData(CSDebugData);
		return true;
	}

}

// 通用 Space Colonization 队列构建：
// 只更新 TargetLocations 与 SCAttributes 中的 PrePt / NextPt / End / Associates 等关系，
// 不创建 PolyPath、不写 Container、不生成任何最终输出。
static void BuildSpaceColonizationQueueImpl(
	const TArray<FTransform>& SourceTransforms,
	const TArray<FTransform>& InTargetTransforms,
	int32 Iteration,
	int32 Activetime,
	float RandGrow,
	float Seed,
	float InfluenceRadius,
	bool bMultThread,
	TArray<FVector>& OutTargetLocations,
	TArray<FSpaceColonizationAttribute>& OutSCAttributes)
{
	GV_TIME_SCOPE(TEXT("SpaceColonization.QueueCPU.Total"));
	OutTargetLocations.Reset();
	OutSCAttributes.Reset();

	LogSpaceColonizationInput(TEXT("CPU"), SourceTransforms.Num(), InTargetTransforms.Num(), Iteration, Activetime, RandGrow, Seed, InfluenceRadius, bMultThread);

	if (SourceTransforms.Num() == 0 || InTargetTransforms.Num() == 0)
	{
		return;
	}

	TArray<FVector> SourceLocations;
	{
		GV_TIME_SCOPE(TEXT("SpaceColonization.QueueCPU.PreparePositions"));
		SourceLocations.Reserve(SourceTransforms.Num());
		OutTargetLocations.Reserve(InTargetTransforms.Num());
		OutSCAttributes.SetNum(InTargetTransforms.Num());

		for (const FTransform& Transform : SourceTransforms)
		{
			SourceLocations.Add(Transform.GetLocation());
		}

		for (const FTransform& Transform : InTargetTransforms)
		{
			OutTargetLocations.Add(Transform.GetLocation());
		}

		for (const FVector& SourceLocation : SourceLocations)
		{
			const int32 Nearpt = UPointFunction::FindNearPointIteration(OutTargetLocations, SourceLocation);
			if (Nearpt == -1)
			{
				continue;
			}

			OutSCAttributes[Nearpt].Attractor = false;
			OutSCAttributes[Nearpt].Startpt = true;
			OutSCAttributes[Nearpt].Startid = Nearpt;
		}
	}
	LogSpaceColonizationQueueState(TEXT("CPU"), TEXT("AfterMarkSources"), -1, OutTargetLocations, OutSCAttributes);

	const float Infrad = InfluenceRadius;
	const int32 ThreadPointNum = 1;
	const int32 NumPt = OutTargetLocations.Num();
	const int32 MaxNeighborsPerTarget = FMath::Clamp(SpaceColonizationMaxNeighborsPerTarget, 1, FMath::Max(NumPt, 1));
	const TArray<FVector> InitialTargetLocations = OutTargetLocations;
	TArray<uint32> NeighborCounts;
	TArray<int32> NeighborIndices;
	BuildSpaceColonizationCPUNeighbors(InitialTargetLocations, Infrad, MaxNeighborsPerTarget, NeighborCounts, NeighborIndices);
	LogSpaceColonizationNeighborCounts(TEXT("CPU"), NeighborCounts);

	double ResetAssociatesMs = 0.0;
	double FindAssociatesMs = 0.0;
	double MergeAssociatesMs = 0.0;
	double ProposeGrowthMs = 0.0;
	double CommitGrowthMs = 0.0;

	for (int32 i = 0; i < Iteration; i++)
	{
		double StageStartSeconds = FPlatformTime::Seconds();
		for (int32 p = 0; p < NumPt; p++)
		{
			OutSCAttributes[p].Associates.Reset();
		}
		ResetAssociatesMs += (FPlatformTime::Seconds() - StageStartSeconds) * 1000.0;
		LogSpaceColonizationAssociates(TEXT("CPU"), TEXT("AfterResetAssociates"), i, OutSCAttributes);

		if (bMultThread)
		{
			SCOPE_CYCLE_COUNTER(STAT_SpaceColonizationMultThread)
			StageStartSeconds = FPlatformTime::Seconds();
			PopulateSpaceColonizationAssociatesFromNeighbors(OutTargetLocations, Infrad, NeighborCounts, NeighborIndices, MaxNeighborsPerTarget, OutSCAttributes);
			FindAssociatesMs += (FPlatformTime::Seconds() - StageStartSeconds) * 1000.0;
			LogSpaceColonizationAssociates(TEXT("CPU"), TEXT("AfterFindAssociates"), i, OutSCAttributes);

			StageStartSeconds = FPlatformTime::Seconds();
			TArray<TTuple<FIndex3i, FVector>> ProcessResult = ProcessAsync::ProcessAsync<TTuple<FIndex3i, FVector>>(NumPt, ThreadPointNum, [&](const int32 p)
			{
				TTuple<FIndex3i, FVector> ThreadCalculate;
				ThreadCalculate.Key = FIndex3i(-1, -1, 0);
				const float Grow = SpaceColonizationGrowRand(p, i, Seed);
				if (OutSCAttributes[p].Attractor == true
					|| OutSCAttributes[p].SpawnCount < i - Activetime
					|| (Grow < RandGrow && i > 10)
					|| OutSCAttributes[p].Associates.Num() == 0
					|| OutSCAttributes[p].BranchCount > 2)
				{
					return ThreadCalculate;
				}

				int32 NearPt = -1;
				float NearestDist = 0.0f;
				if (!FindSpaceColonizationNearestAttractorFromNeighbors(
					p,
					OutTargetLocations,
					OutSCAttributes,
					NeighborCounts,
					NeighborIndices,
					MaxNeighborsPerTarget,
					NearPt,
					NearestDist))
				{
					return ThreadCalculate;
				}

				FVector DirSum = FVector::ZeroVector;
				for (const int32 Index : OutSCAttributes[p].Associates)
				{
					const FVector Dir = (OutTargetLocations[Index] - OutTargetLocations[p]);
					//Dir.Normalize();
					DirSum += Dir;
				}
				DirSum.Normalize();

				ThreadCalculate.Key = FIndex3i(p, NearPt, 1);
				ThreadCalculate.Value = OutTargetLocations[p] + DirSum * NearestDist;

				return ThreadCalculate;
			});
			ProposeGrowthMs += (FPlatformTime::Seconds() - StageStartSeconds) * 1000.0;

			StageStartSeconds = FPlatformTime::Seconds();
			TArray<FSpaceColonizationGrowthDebugEvent> GrowthEvents;
			for (int32 j = 0; j < ProcessResult.Num(); j++)
			{
				if (ProcessResult[j].Key.C == 0)
				{
					continue;
				}

				const int32 p = ProcessResult[j].Key.A;
				const int32 NearPt = ProcessResult[j].Key.B;
				const FVector OldTargetPosition = OutTargetLocations[NearPt];
				const int32 AssociateCount = OutSCAttributes[p].Associates.Num();
				OutSCAttributes[p].SpawnCount += 1;
				OutTargetLocations[NearPt] = ProcessResult[j].Value;
				OutSCAttributes[NearPt].Startid = OutSCAttributes[p].Startid;
				OutSCAttributes[NearPt].SpawnCount = OutSCAttributes[p].SpawnCount;
				OutSCAttributes[NearPt].Attractor = false;
				OutSCAttributes[NearPt].End = true;
				OutSCAttributes[NearPt].BranchCount = 1;
				OutSCAttributes[p].End = false;

				OutSCAttributes[NearPt].PrePt = p;
				OutSCAttributes[p].NextPt = NearPt;
				OutSCAttributes[p].BranchCount += 1;

				FSpaceColonizationGrowthDebugEvent& Event = GrowthEvents.AddDefaulted_GetRef();
				Event.SourceIndex = p;
				Event.TargetIndex = NearPt;
				Event.AssociateCount = AssociateCount;
				Event.ParentSpawnAfter = OutSCAttributes[p].SpawnCount;
				Event.ParentBranchAfter = OutSCAttributes[p].BranchCount;
				Event.OldTargetPosition = OldTargetPosition;
				Event.NewTargetPosition = OutTargetLocations[NearPt];
				Event.MoveDistance = FVector::Dist(OldTargetPosition, OutTargetLocations[NearPt]);
			}
			CommitGrowthMs += (FPlatformTime::Seconds() - StageStartSeconds) * 1000.0;
			LogSpaceColonizationGrowthEvents(TEXT("CPU"), TEXT("AfterProposeGrowth"), i, GrowthEvents);
			LogSpaceColonizationQueueState(TEXT("CPU"), TEXT("AfterCommitGrowth"), i, OutTargetLocations, OutSCAttributes);
		}
		else
		{
			SCOPE_CYCLE_COUNTER(STAT_SpaceColonization)
			StageStartSeconds = FPlatformTime::Seconds();
			PopulateSpaceColonizationAssociatesFromNeighbors(OutTargetLocations, Infrad, NeighborCounts, NeighborIndices, MaxNeighborsPerTarget, OutSCAttributes);
			FindAssociatesMs += (FPlatformTime::Seconds() - StageStartSeconds) * 1000.0;
			LogSpaceColonizationAssociates(TEXT("CPU"), TEXT("AfterFindAssociates"), i, OutSCAttributes);

			StageStartSeconds = FPlatformTime::Seconds();
			TArray<FSpaceColonizationGrowthDebugEvent> GrowthEvents;
			for (int32 p = 0; p < NumPt; p++)
			{
				const float Grow = SpaceColonizationGrowRand(p, i, Seed);
				if (OutSCAttributes[p].Attractor == true
					|| OutSCAttributes[p].SpawnCount < i - Activetime
					|| (Grow < RandGrow && i > 10)
					|| OutSCAttributes[p].Associates.Num() == 0
					|| OutSCAttributes[p].BranchCount > 2)
				{
					continue;
				}

				int32 NearPt = -1;
				float NearestDist = 0.0f;
				if (!FindSpaceColonizationNearestAttractorFromNeighbors(
					p,
					OutTargetLocations,
					OutSCAttributes,
					NeighborCounts,
					NeighborIndices,
					MaxNeighborsPerTarget,
					NearPt,
					NearestDist))
				{
					continue;
				}

				FVector DirSum = FVector::ZeroVector;
				for (const int32 Index : OutSCAttributes[p].Associates)
				{
					const FVector Dir = (OutTargetLocations[Index] - OutTargetLocations[p]);
					//Dir.Normalize();
					DirSum += Dir;
				}
				DirSum.Normalize();

				const FVector OldTargetPosition = OutTargetLocations[NearPt];
				const int32 AssociateCount = OutSCAttributes[p].Associates.Num();
				const FVector NewTargetPosition = OutTargetLocations[p] + DirSum * NearestDist;
				OutSCAttributes[p].SpawnCount += 1;
				OutTargetLocations[NearPt] = NewTargetPosition;
				OutSCAttributes[NearPt].Startid = OutSCAttributes[p].Startid;
				OutSCAttributes[NearPt].SpawnCount = OutSCAttributes[p].SpawnCount;
				OutSCAttributes[NearPt].Attractor = false;
				OutSCAttributes[NearPt].End = true;
				OutSCAttributes[NearPt].BranchCount = 1;
				OutSCAttributes[p].End = false;

				OutSCAttributes[NearPt].PrePt = p;
				OutSCAttributes[p].NextPt = NearPt;
				OutSCAttributes[p].BranchCount += 1;

				FSpaceColonizationGrowthDebugEvent& Event = GrowthEvents.AddDefaulted_GetRef();
				Event.SourceIndex = p;
				Event.TargetIndex = NearPt;
				Event.AssociateCount = AssociateCount;
				Event.ParentSpawnAfter = OutSCAttributes[p].SpawnCount;
				Event.ParentBranchAfter = OutSCAttributes[p].BranchCount;
				Event.OldTargetPosition = OldTargetPosition;
				Event.NewTargetPosition = NewTargetPosition;
				Event.MoveDistance = FVector::Dist(OldTargetPosition, NewTargetPosition);
			}
			CommitGrowthMs += (FPlatformTime::Seconds() - StageStartSeconds) * 1000.0;
			LogSpaceColonizationGrowthEvents(TEXT("CPU"), TEXT("AfterProposeGrowth"), i, GrowthEvents);
			LogSpaceColonizationQueueState(TEXT("CPU"), TEXT("AfterCommitGrowth"), i, OutTargetLocations, OutSCAttributes);
		}
	}

#if GV_ENABLE_PERF_LOGS
	PCG_DEBUG_LOG(LogTemp, Display,
		TEXT("[GenerateVinesTiming] SpaceColonization.QueueCPU.Iterations: Iterations=%d Sources=%d Targets=%d MultThread=%s ResetAssociates=%.3f ms FindAssociates=%.3f ms MergeAssociates=%.3f ms ProposeGrowth=%.3f ms CommitGrowth=%.3f ms"),
		Iteration,
		SourceTransforms.Num(),
		InTargetTransforms.Num(),
		bMultThread ? TEXT("true") : TEXT("false"),
		ResetAssociatesMs,
		FindAssociatesMs,
		MergeAssociatesMs,
		ProposeGrowthMs,
		CommitGrowthMs);
#endif
}

static TArray<FSpaceColonizationLineResult> BuildSpaceColonizationLineResultsImpl(
	const TArray<FVector>& TargetLocations,
	TArray<FSpaceColonizationAttribute>& SCAttributes,
	int32 BackGrowCount,
	int32 ForkTaperForkOrdinal,
	const TArray<float>& TargetPointScales,
	const TArray<float>& StartSourceScales)
{
	GV_TIME_SCOPE(TEXT("SpaceColonization.BuildLines"));
	TArray<FSpaceColonizationLineResult> Lines;
	const int32 NumPt = TargetLocations.Num();
	if (NumPt == 0 || SCAttributes.Num() != NumPt)
	{
		return Lines;
	}

	constexpr int32 MaxBacktrackSteps = 100;
	const int32 PreForkAncestorCount = FMath::Clamp(BackGrowCount - 1, 0, MaxBacktrackSteps);
	// ====================================================================================
	// Fork Tapering: When a minor branch backtracks into a fork, the ancestor points it
	// keeps belong to the shared parent stem. Taper their scale down to 0.1x so the branch
	// thins out toward the fork instead of inheriting the thick parent scale.
	//
	// A child's BranchOrder (computed below) ranks it among its siblings: order 1 is the
	// primary branch of the fork, order >= 2 is a non-primary branch. A child only gets
	// order >= 2 when it has a higher-priority sibling, which means its parent forked. So
	// "BranchOrder > PrimaryBranchOrder" exactly means "non-primary branch at a fork".
	// ====================================================================================
	constexpr int32 PrimaryBranchOrder = 1;
	constexpr float ForkTaperEndScale = 0.1f;
	// Fork Tapering fires only on the Nth non-primary fork hit while backtracking this line.
	const int32 TaperForkOrdinal = FMath::Max(ForkTaperForkOrdinal, 1);

	TArray<int32> BranchOrderByNode;
	BranchOrderByNode.Init(0, NumPt);
	for (int32 ChildIndex = 0; ChildIndex < NumPt; ++ChildIndex)
	{
		const int32 ParentIndex = SCAttributes[ChildIndex].PrePt;
		if (ParentIndex < 0 || ParentIndex >= NumPt)
		{
			continue;
		}

		int32 BranchOrder = 1;
		for (int32 SiblingIndex = 0; SiblingIndex < NumPt; ++SiblingIndex)
		{
			if (SiblingIndex == ChildIndex || SCAttributes[SiblingIndex].PrePt != ParentIndex)
			{
				continue;
			}

			const int32 SiblingSpawnCount = SCAttributes[SiblingIndex].SpawnCount;
			const int32 ChildSpawnCount = SCAttributes[ChildIndex].SpawnCount;
			if (SiblingSpawnCount < ChildSpawnCount || (SiblingSpawnCount == ChildSpawnCount && SiblingIndex < ChildIndex))
			{
				BranchOrder += 1;
			}
		}
		BranchOrderByNode[ChildIndex] = BranchOrder;
	}

	//CreateLineArray
	for (int32 p = 0; p < NumPt; p++)
	{
		if (SCAttributes[p].End != true)
		{
			continue;
		}

		TArray<FVector> Line;
		TArray<float> LinePointScales;
		int32 LineCount = 0;
		int32 CurrentIndex = p;
		int32 NonPrimaryForkCount = 0;
		// Fork Tapering is "armed" once we pass the start fork (the ForkTaperForkOrdinal-th
		// non-primary fork, default 1 = the first fork). After arming, the backtrack keeps
		// emitting up to PreForkAncestorCount more ancestor points and tapers their scale
		// continuously from the start-fork scale down to ForkTaperEndScale. Those ancestors
		// may freely cross additional forks: later forks are just normal tapered points, the
		// taper neither resets nor stops at them.
		bool bTaperArmed = false;
		float ForkPointScale = 1.0f;
		int32 TaperedAncestorCount = 0;

		Line.Add(TargetLocations[CurrentIndex]);
		LinePointScales.Add(ResolveSpaceColonizationOutputScale(CurrentIndex, SCAttributes, TargetPointScales, StartSourceScales));
		//float LineLength = 0;
		while (LineCount < MaxBacktrackSteps)
		{
			const int32 ChildIndex = CurrentIndex;
			const int32 PreIndex = SCAttributes[CurrentIndex].PrePt;
			if (PreIndex < 0 || PreIndex >= NumPt)
			{
				break;
			}

			Line.Add(TargetLocations[PreIndex]);
			// Arm the taper the moment we step off the start fork. ChildIndex is a non-primary
			// branch of the fork sitting at PreIndex, so reaching the ForkTaperForkOrdinal-th
			// such fork captures that fork's scale as the taper reference.
			if (!bTaperArmed && BranchOrderByNode[ChildIndex] > PrimaryBranchOrder)
			{
				NonPrimaryForkCount += 1;
				if (NonPrimaryForkCount >= TaperForkOrdinal)
				{
					bTaperArmed = true;
					ForkPointScale = LinePointScales.Num() > 0 ? LinePointScales.Last() : 1.0f;
				}
			}

			if (!bTaperArmed)
			{
				// Still on the full-scale stem before the start fork.
				LinePointScales.Add(ResolveSpaceColonizationOutputScale(PreIndex, SCAttributes, TargetPointScales, StartSourceScales));
			}
			else
			{
				// Tapered ancestor. The taper runs continuously across the whole retained
				// span regardless of how many forks it crosses.
				TaperedAncestorCount += 1;
				const float TaperAlpha = PreForkAncestorCount > 1
					? static_cast<float>(TaperedAncestorCount) / static_cast<float>(PreForkAncestorCount)
					: 1.0f;
				const float TaperScale = ForkPointScale * FMath::Lerp(1.0f, ForkTaperEndScale, TaperAlpha);
				LinePointScales.Add(TaperScale);
			}
			CurrentIndex = PreIndex;
			LineCount += 1;

			// Once armed, stop after the retained ancestor budget is spent.
			if (bTaperArmed && TaperedAncestorCount >= PreForkAncestorCount)
			{
				break;
			}

		// ====================================================================================
		// End Fork Tapering
		// ====================================================================================
		//LineLength += DistancePre;
		}

		if (Line.Num() == 0)
		{
			continue;
		}

		FGeometryScriptPolyPath PolyPath;
		PolyPath.Reset();
		*PolyPath.Path = Line;

		FSpaceColonizationLineResult LineResult;
		LineResult.Path = PolyPath;
		LineResult.PointScales = MoveTemp(LinePointScales);
		Lines.Add(MoveTemp(LineResult));
	}

	return Lines;
}

}

// ---- SpaceColonization member functions (moved from UGenerateVines, params from SC) ----

// Force the SC execution path regardless of SC.bUseComputeShader. Lets GPU-vs-CPU parity
// be A/B-tested at runtime (the per-actor bool cannot be set via the editor script bridge).
//   0 = use SC.bUseComputeShader (default)   1 = force GPU   2 = force CPU
static TAutoConsoleVariable<int32> CVarVineSCForceMode(
	TEXT("r.Vine.SC.ForceMode"),
	0,
	TEXT("Vine space-colonization path override: 0=use SC.bUseComputeShader, 1=force GPU, 2=force CPU."),
	ECVF_Default);

// Trip A: GPU port of ApplyVVSCPointOffset. Point jitter distance (cm) applied before the
// prep smooth on the GPU SC path, for visual parity with the CPU path. 0 disables scatter.
static TAutoConsoleVariable<float> CVarVineSCScatter(
	TEXT("r.Vine.SC.Scatter"),
	10.0f,
	TEXT("Vine GPU SC point-scatter distance in cm (ApplyVVSCPointOffset parity). 0 = off."),
	ECVF_Default);

TArray<FSpaceColonizationLineResult> AVineContainer::SpaceColonizationWithScales(TArray<FTransform> SourceTransforms, TArray<FTransform> TargetTransforms, bool bUseComputeShader)
{
	return SpaceColonizationWithScalesInternal(MoveTemp(SourceTransforms), MoveTemp(TargetTransforms), bUseComputeShader, nullptr);
}

TArray<FSpaceColonizationLineResult> AVineContainer::SpaceColonizationWithScalesInternal(TArray<FTransform> SourceTransforms, TArray<FTransform> TargetTransforms, bool bUseComputeShader, FVineSCGPUBuffers* OutGPUBuffers)
{
	const int32 ForceMode = CVarVineSCForceMode.GetValueOnGameThread();
	if (ForceMode == 1) bUseComputeShader = true;
	else if (ForceMode == 2) bUseComputeShader = false;

	if (bUseComputeShader)
	{
		GV_TIME_SCOPE(TEXT("SpaceColonization.TotalCS"));
		TArray<FVector> TargetLocations;
		TArray<FSpaceColonizationAttribute> SCAttributes;
		TArray<float> TargetPointScales;
		TArray<float> StartSourceScales;
		BuildSpaceColonizationScaleLookups(SourceTransforms, TargetTransforms, TargetPointScales, StartSourceScales);
		// Ensure the profile curve exists BEFORE baking the LUT. VisVineGPUInternal lazily
		// creates this default identity ramp, but it runs after the SC prep — so on a first
		// generate (CurveControl still null) the LUT baked flat (EvaluateVineScale(null)=1.0)
		// and the fused path lost all thickness variation until the next generate rebuilt it.
		// Create the same default here so the very first generate already has the ramp.
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
			VV.UVScaleInfluence,
			VV.UVScaleFloor,
			VV.UVScalePower,
			VV.UVLengthScale,
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

		// Thickness check: FinalScale (PathPoints2.w) range. min << max => taper present
		// (CurveControl ramp baked correctly); min ~= max => flat (the CurveControl-null-at-bake
		// bug). Runs before the fused early-out so it covers the first generate too.
		{
			float WMin = TNumericLimits<float>::Max();
			float WMax = -TNumericLimits<float>::Max();
			for (const FVector4f& P : GPUPathPoints)
			{
				WMin = FMath::Min(WMin, P.W);
				WMax = FMath::Max(WMax, P.W);
			}
			UE_LOG(LogTemp, Warning, TEXT("[VineThickCheck] CurveControl=%p FinalScale[min=%.4f max=%.4f] n=%d"),
				VV.CurveControl,
				GPUPathPoints.Num() ? WMin : 0.0f,
				GPUPathPoints.Num() ? WMax : 0.0f,
				GPUPathPoints.Num());
		}

		// Trip A (4c): when this call produces GPU buffers for the fused VisVine path, the CPU
		// tree re-trace + downstream prep are redundant (the GPU PathPoints2 already carry the
		// full smoothed/resampled/scaled geometry). Skip them — TubeLines stays empty and the
		// fused path drives the mesh from the GPU buffers. r.Vine.SC.FusedPath=0 restores this.
		if (OutGPUBuffers != nullptr
			&& CVarVineSCFusedPath.GetValueOnGameThread() != 0
			&& !VV.bVisVineGPUUseCPUForPostPasses)
		{
			return {};
		}

		TArray<FSpaceColonizationLineResult> Results = BuildSpaceColonizationLineResultsImpl(TargetLocations, SCAttributes, SC.BackGrowCount, SC.ForkTaperForkOrdinal, TargetPointScales, StartSourceScales);

		// Stage B3 validation: GPUPathPoints now holds the full prep — SmoothLine(3),
		// ResamppleByLength, per-point scale remap, and CurveScale (in .w). It must match
		// the CPU tracer lines put through the same chain (BuildPreparedLinePointScales +
		// EvaluateVineScale). Compare order-independent position + final-scale sums.
		// (Note: the CPU also re-maps scales during smooth; the GPU treats smooth as an
		// identity for scales, a documented sub-visual approximation.)
		// Skipped when GPU scatter is on: the GPU jitter is deliberately not bit-identical to
		// the CPU FRandomStream::VRand, so positions/counts diverge by design — comparing here
		// would flag an expected difference. The scatter-free prep was already verified.
		if (CVarVineSCScatter.GetValueOnGameThread() <= 0.0f)
		{
			const float SafeResampleLength = FMath::Max(VV.ResampleLength, 0.01f);
			FVector CPUPosSum = FVector::ZeroVector;
			double CPUScaleSum = 0.0;
			int32 CPUPointCount = 0;
			int32 CPULineCount = 0;
			for (const FSpaceColonizationLineResult& Line : Results)
			{
				if (!Line.Path.Path.IsValid() || Line.Path.Path->Num() < 2)
				{
					continue;
				}
				const FGeometryScriptPolyPath Smoothed = UPolyLine::SmoothLine(Line.Path, 3);
				const FGeometryScriptPolyPath Resampled = UPolyLine::ResamppleByLength(Smoothed, SafeResampleLength);
				if (!Resampled.Path.IsValid() || Resampled.Path->Num() < 2)
				{
					continue;
				}
				const int32 N = Resampled.Path->Num();
				TArray<float> ResampledScales;
				BuildPreparedLinePointScales(Smoothed, &Line.PointScales, N, 1.0f, ResampledScales);
				++CPULineCount;
				for (int32 PointIndex = 0; PointIndex < N; ++PointIndex)
				{
					CPUPosSum += (*Resampled.Path)[PointIndex];
					const float ResScale = ResampledScales.IsValidIndex(PointIndex) ? ResampledScales[PointIndex] : 0.0f;
					const float FinalScale = EvaluateVineScale(VV.CurveControl, PointIndex, N) * FMath::Max(ResScale, 0.0f);
					CPUScaleSum += double(FinalScale);
					++CPUPointCount;
				}
			}

			FVector GPUPosSum = FVector::ZeroVector;
			double GPUScaleSum = 0.0;
			for (const FVector4f& Point : GPUPathPoints)
			{
				GPUPosSum += FVector(Point.X, Point.Y, Point.Z);
				GPUScaleSum += double(Point.W);
			}

			const double PosDiff = (CPUPosSum - GPUPosSum).Size();
			const double ScaleDiff = FMath::Abs(CPUScaleSum - GPUScaleSum);
			UE_LOG(LogTemp, Display,
				TEXT("[SpaceColonizationPrepCS] lines CPU=%d | points CPU=%d GPU=%d | posSumDiff=%.4f scaleSumDiff=%.4f | CPUscale=%.3f GPUscale=%.3f"),
				CPULineCount, CPUPointCount, GPUPathPoints.Num(), PosDiff, ScaleDiff, CPUScaleSum, GPUScaleSum);
		}

		return Results;
	}

	GV_TIME_SCOPE(TEXT("SpaceColonization.TotalCPU"));

	TArray<float> TargetPointScales;
	TArray<float> StartSourceScales;
	BuildSpaceColonizationScaleLookups(SourceTransforms, TargetTransforms, TargetPointScales, StartSourceScales);

	TArray<FVector> TargetLocations;
	TArray<FSpaceColonizationAttribute> SCAttributes;
	BuildSpaceColonizationQueueImpl(
		SourceTransforms,
		TargetTransforms,
		SC.Iteration,
		SC.Activetime,
		SC.RandGrow,
		SC.Seed,
		SC.InfluenceRadius,
		true,
		TargetLocations,
		SCAttributes);

	if (TargetLocations.Num() == 0 || SCAttributes.Num() != TargetLocations.Num())
	{
		return {};
	}

	TArray<FSpaceColonizationLineResult> Results = BuildSpaceColonizationLineResultsImpl(TargetLocations, SCAttributes, SC.BackGrowCount, SC.ForkTaperForkOrdinal, TargetPointScales, StartSourceScales);
	return Results;
}
