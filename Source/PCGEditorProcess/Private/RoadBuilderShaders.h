#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "RoadTypes.h"

class FRDGBuilder;

// Persistent GPU output buffers the road build writes into. The caller creates or
// registers these (transient RDG buffers, or external-registered pooled buffers) and
// owns their lifetime, final access states, and GraphBuilder.Execute().
struct FRoadGeometryBuffers
{
	FRDGBufferRef Positions = nullptr;    // RWBuffer<float>, >= MaxVertices*3 (xyz per vertex)
	FRDGBufferRef Tangents = nullptr;     // RWBuffer<uint>,  >= MaxVertices*2
	FRDGBufferRef TexCoords = nullptr;    // RWBuffer<float>, >= MaxVertices*2
	FRDGBufferRef Colors = nullptr;       // RWBuffer<uint>,  >= MaxVertices
	FRDGBufferRef Indices = nullptr;      // RWBuffer<uint>,  >= MaxIndices (triangle list)
	FRDGBufferRef IndirectArgs = nullptr; // RWBuffer<uint>, 5 (DrawIndexedIndirect args)
};

// Runs the full road compute pipeline (Clear -> Intersect -> Cluster -> CornerSolve ->
// EmitRoad -> EmitCorner -> Finalize), adding the passes to GraphBuilder and writing into
// Out. Shared by FRoadMeshSceneProxy (renders the result) and the CS-landscape road
// heightmap path. The caller sets final buffer access and calls GraphBuilder.Execute().
void BuildRoadGeometryRDG(
	FRDGBuilder& GraphBuilder,
	ERHIFeatureLevel::Type FeatureLevel,
	const FRoadBuildInput& Input,
	const FRoadGeometryBuffers& Out);

// Compute kernels of Shaders/Private/RoadBuilder.usf. One class per entry
// point; parameter structs list exactly the resources each kernel touches.

class FRoadClearCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRoadClearCS);
	SHADER_USE_PARAMETER_STRUCT(FRoadClearCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWCounters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWSplineFlags)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FJunctionData>, RWJunctions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWIndices)
		SHADER_PARAMETER(uint32, NumSplines)
		SHADER_PARAMETER(uint32, MaxJunctions)
		SHADER_PARAMETER(uint32, MaxIndices)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FRoadIntersectCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRoadIntersectCS);
	SHADER_USE_PARAMETER_STRUCT(FRoadIntersectCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, SplinePoints)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FSplineInfo>, Splines)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, SegPrefix)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FCrossRec>, RWCrossRecs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWCounters)
		SHADER_PARAMETER(uint32, NumSplines)
		SHADER_PARAMETER(uint32, NumSegments)
		SHADER_PARAMETER(float, EndSnapRadius)
		SHADER_PARAMETER(uint32, MaxCrossRecs)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FRoadClusterCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRoadClusterCS);
	SHADER_USE_PARAMETER_STRUCT(FRoadClusterCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, SplinePoints)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FSplineInfo>, Splines)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FCrossRec>, RWCrossRecs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FJunctionData>, RWJunctions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FLegData>, RWLegs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWCounters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWSplineFlags)
		SHADER_PARAMETER(float, SampleStep)
		SHADER_PARAMETER(float, HalfWidth)
		SHADER_PARAMETER(float, MergeRadius)
		SHADER_PARAMETER(uint32, MaxCrossRecs)
		SHADER_PARAMETER(uint32, MaxJunctions)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FRoadCornerSolveCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRoadCornerSolveCS);
	SHADER_USE_PARAMETER_STRUCT(FRoadCornerSolveCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, SplinePoints)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FSplineInfo>, Splines)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FJunctionData>, Junctions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FLegData>, Legs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, Counters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FCornerData>, RWCorners)
		SHADER_PARAMETER(float, SampleStep)
		SHADER_PARAMETER(float, HalfWidth)
		SHADER_PARAMETER(float, CornerDist)
		SHADER_PARAMETER(float, MiterDotThreshold)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FRoadEmitRoadCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRoadEmitRoadCS);
	SHADER_USE_PARAMETER_STRUCT(FRoadEmitRoadCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, SplinePoints)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FSplineInfo>, Splines)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SplineFlags)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWCounters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RWPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWTangents)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RWTexCoords)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWColors)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWIndices)
		SHADER_PARAMETER(uint32, NumSplines)
		SHADER_PARAMETER(float, SampleStep)
		SHADER_PARAMETER(float, HalfWidth)
		SHADER_PARAMETER(float, UVInvTile)
		SHADER_PARAMETER(uint32, MaxVertices)
		SHADER_PARAMETER(uint32, MaxIndices)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FRoadEmitCornerCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRoadEmitCornerCS);
	SHADER_USE_PARAMETER_STRUCT(FRoadEmitCornerCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, SplinePoints)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FSplineInfo>, Splines)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FJunctionData>, Junctions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FLegData>, Legs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FCornerData>, Corners)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWCounters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RWPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWTangents)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RWTexCoords)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWColors)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWIndices)
		SHADER_PARAMETER(float, SampleStep)
		SHADER_PARAMETER(float, HalfWidth)
		SHADER_PARAMETER(float, UVInvTile)
		SHADER_PARAMETER(uint32, MaxVertices)
		SHADER_PARAMETER(uint32, MaxIndices)
		SHADER_PARAMETER(uint32, MaxCornerSamples)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FRoadFinalizeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRoadFinalizeCS);
	SHADER_USE_PARAMETER_STRUCT(FRoadFinalizeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWCounters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWIndirectArgs)
		SHADER_PARAMETER(uint32, MaxIndices)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

// Converts the road heightmap (depth = CameraHeight - WorldZ, NaN off-road) into the
// CS-landscape edit-layer merge format: RW_RoadResult = (WorldZ, 0, 0, Coverage).
class FRoadDepthToResultCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRoadDepthToResultCS);
	SHADER_USE_PARAMETER_STRUCT(FRoadDepthToResultCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_RoadDepth)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_RoadResult)
		SHADER_PARAMETER(float, RoadCameraHeight)
		SHADER_PARAMETER(float, RoadInfluence)
		SHADER_PARAMETER(float, RoadHeightOffset)
		SHADER_PARAMETER(FIntPoint, RoadResultSize)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

// Height diffusion (temporal-blocking / groupshared). State (WorldZ*Weight, Weight, isSource);
// roads are fixed Dirichlet sources, gaps relax to a coverage-weighted neighbour average.
class FRoadDiffuseCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRoadDiffuseCS);
	SHADER_USE_PARAMETER_STRUCT(FRoadDiffuseCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_RoadDiffuseIn)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_RoadDiffuseOut)
		SHADER_PARAMETER(int32, RoadDiffuseIterations)
		SHADER_PARAMETER(FIntPoint, RoadDiffuseSize)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

// Un-premultiply: (WorldZ*Weight, Weight, isSource, _) -> (WorldZ, 0, 0, Weight).
class FRoadUnpremultCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRoadUnpremultCS);
	SHADER_USE_PARAMETER_STRUCT(FRoadUnpremultCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_RoadUnpremult)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_RoadUnpremult)
		SHADER_PARAMETER(FIntPoint, RoadUnpremultSize)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

// Converts a road depth texture -> (WorldZ, coverage) result, with strength/offset and a
// height-diffusion shoulder. DiffuseIterations is the total Jacobi budget, temporal-blocked
// into ceil(N/ROAD_DIFF_LOCAL) global ping-pong passes.
void AddRoadDepthToResultPass(
	FRDGBuilder& GraphBuilder,
	ERHIFeatureLevel::Type FeatureLevel,
	FRDGTextureRef DepthTexture,
	FRDGTextureRef ResultTexture,
	float CameraHeight,
	float Influence,
	float HeightOffset,
	int32 DiffuseIterations);
