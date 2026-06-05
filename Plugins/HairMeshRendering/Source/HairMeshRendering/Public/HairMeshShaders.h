#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "ShaderParameterMacros.h"
#include "RHIResources.h"

// ============================================================================
// GPU data structures (must match HLSL in HairMeshCommon.usf)
// ============================================================================

struct FHairTriStripVertex
{
	FVector3f Position;
	float     Pad0;
	FVector3f Normal;
	float     Pad1;
	FVector2f UV;
	float     Pad2;
	float     Pad3;
};

// ============================================================================
// Compute Shader — Hair Strand Generation (with LOD + full styling)
// ============================================================================

class FHairMeshGenerationCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairMeshGenerationCS);
	SHADER_USE_PARAMETER_STRUCT(FHairMeshGenerationCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Hair mesh textures
		SHADER_PARAMETER_TEXTURE(Texture3D, HairMeshTexture)
		SHADER_PARAMETER_TEXTURE(Texture2D, HairUVTexture)
		SHADER_PARAMETER_TEXTURE(Texture3D, HairWTexture)
		SHADER_PARAMETER_TEXTURE(Texture3D, HairUDirTexture)
		SHADER_PARAMETER_TEXTURE(Texture3D, HairVDirTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, HairLinearSampler)

		// Dimensions
		SHADER_PARAMETER(FIntVector, TextureSize3D)
		SHADER_PARAMETER(FIntPoint,  TextureSize2D)

		// Hair mesh params
		SHADER_PARAMETER(int32, NumBundles)
		SHADER_PARAMETER(int32, MaxStrandsPerBundle)
		SHADER_PARAMETER(int32, VertsPerStrand)
		SHADER_PARAMETER(int32, NumExtrusionLayers)

		// Bundle mappings
		SHADER_PARAMETER_SRV(StructuredBuffer<FIntPoint>, BundleMappings)

		// Blue noise
		SHADER_PARAMETER_SRV(StructuredBuffer<FVector2f>, BlueNoiseSamples)
		SHADER_PARAMETER(int32, NumBlueNoiseSamples)

		// Styling
		SHADER_PARAMETER(float, CurlAmplitude)
		SHADER_PARAMETER(float, CurlFrequency)
		SHADER_PARAMETER(float, WaveAmplitude)
		SHADER_PARAMETER(float, WaveFrequency)
		SHADER_PARAMETER(float, FrizzAmplitude)
		SHADER_PARAMETER(float, ClumpAmplitude)
		SHADER_PARAMETER(float, StrandWidth)
		SHADER_PARAMETER(float, TipThinning)
		SHADER_PARAMETER(float, LengthVariation)
		SHADER_PARAMETER(int32, CurlSeed)
		SHADER_PARAMETER(int32, WaveSeed)
		SHADER_PARAMETER(int32, FrizzSeed)

		// Camera & Transform
		SHADER_PARAMETER(FVector3f, CameraPosition)
		SHADER_PARAMETER(FMatrix44f, LocalToWorld)

		// LOD (Section 3.6)
		SHADER_PARAMETER(float, StrandLODAlpha)
		SHADER_PARAMETER(float, VertexLODAlpha)
		SHADER_PARAMETER(float, LODTransitionLambda)
		SHADER_PARAMETER(float, CameraFOVTanHalf)
		SHADER_PARAMETER(float, ScreenHeight)

		// Output
		SHADER_PARAMETER_UAV(RWStructuredBuffer<FHairTriStripVertex>, OutputVertices)
		SHADER_PARAMETER_UAV(RWBuffer<uint>, IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), 64);
	}
};

// ============================================================================
// Vertex Shader — Hair Rasterization
// ============================================================================

class FHairMeshRasterVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairMeshRasterVS);
	SHADER_USE_PARAMETER_STRUCT(FHairMeshRasterVS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(StructuredBuffer<FHairTriStripVertex>, VertexBuffer)
		SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

// ============================================================================
// Pixel Shader — Dual-Highlight Kajiya-Kay Shading
// ============================================================================

class FHairMeshRasterPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairMeshRasterPS);
	SHADER_USE_PARAMETER_STRUCT(FHairMeshRasterPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FVector3f, LightDirection)
		SHADER_PARAMETER(FVector3f, LightColor)
		SHADER_PARAMETER(FVector3f, HairBaseColor)
		SHADER_PARAMETER(float,     HairRoughness)
		SHADER_PARAMETER(FVector3f, CameraPosition)
		SHADER_PARAMETER(FVector3f, HairSpecularTint)
		SHADER_PARAMETER(float,     HairSecondaryShift)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};
