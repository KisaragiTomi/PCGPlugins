#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"

// Compute shader for static mode (copy rest → deformed)
class FEnhancedCardsDeformCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FEnhancedCardsDeformCS);
	SHADER_USE_PARAMETER_STRUCT(FEnhancedCardsDeformCS, FGlobalShader);

	class FDeformMode : SHADER_PERMUTATION_INT("PERMUTATION_DEFORM_MODE", 2);
	using FPermutationDomain = TShaderPermutationDomain<FDeformMode>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, VertexCount)
		SHADER_PARAMETER_SRV(Buffer<float4>, RestPositionBuffer)
		SHADER_PARAMETER_SRV(Buffer<float4>, RestTangentBuffer)
		SHADER_PARAMETER_UAV(RWBuffer<float4>, DeformedPositionBuffer)
		SHADER_PARAMETER_UAV(RWBuffer<float4>, DeformedTangentBuffer)
		// Guide-mode only (mode 1)
		SHADER_PARAMETER(uint32, GuideVertexCount)
		SHADER_PARAMETER(FVector3f, GuideRestPositionOffset)
		SHADER_PARAMETER(FVector3f, GuideDeformedPositionOffset)
		SHADER_PARAMETER_SRV(ByteAddressBuffer, GuideRestPositionBuffer)
		SHADER_PARAMETER_SRV(ByteAddressBuffer, GuideDeformedPositionBuffer)
		SHADER_PARAMETER_SRV(ByteAddressBuffer, CardsInterpolationBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("GROUP_SIZE"), 64);
	}
};

// Utility: dispatch deformation for a set of vertex buffers
namespace EnhancedHairCardsDeformation
{
	void DispatchStatic(
		FRHICommandList& RHICmdList,
		uint32 VertexCount,
		FRHIShaderResourceView* RestPositionSRV,
		FRHIShaderResourceView* RestTangentSRV,
		FRHIUnorderedAccessView* DeformedPositionUAV,
		FRHIUnorderedAccessView* DeformedTangentUAV);
}
