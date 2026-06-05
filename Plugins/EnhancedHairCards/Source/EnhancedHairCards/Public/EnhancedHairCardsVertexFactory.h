#pragma once

#include "CoreMinimal.h"
#include "ShaderParameters.h"
#include "Components.h"
#include "VertexFactory.h"
#include "RenderGraphResources.h"
#include "PrimitiveSceneProxy.h"
#include "MeshBatch.h"

#define ENHANCED_HAIR_CARDS_MAX_UV       8

// ---------------------------------------------------------------------------
// Uniform data for manual-fetch hair-card geometry. Surface appearance is
// material-driven; no per-card texture slot routing is exposed here.
// ---------------------------------------------------------------------------
BEGIN_GLOBAL_SHADER_PARAMETER_STRUCT(FEnhancedHairCardsVFParameters, )
	SHADER_PARAMETER(uint32, Flags)
	SHADER_PARAMETER(uint32, MaxVertexCount)
	SHADER_PARAMETER(uint32, NumUVs)
	SHADER_PARAMETER_SRV(Buffer<float4>, PositionBuffer)
	SHADER_PARAMETER_SRV(Buffer<float4>, PreviousPositionBuffer)
	SHADER_PARAMETER_SRV(Buffer<float4>, NormalsBuffer)
	SHADER_PARAMETER_SRV(Buffer<float4>, UVsBuffer)
	SHADER_PARAMETER_SRV(Buffer<float4>, MaterialsBuffer)
	SHADER_PARAMETER_SRV(Buffer<float4>, VertexColorsBuffer)
	SHADER_PARAMETER_SRV(Buffer<float4>, DynamicsBuffer)
	SHADER_PARAMETER(uint32, DynamicsFlags)
	SHADER_PARAMETER(float, DynamicsStrength)
	SHADER_PARAMETER(float, DynamicsWindStrength)
	SHADER_PARAMETER(float, DynamicsFlutterStrength)
	SHADER_PARAMETER(float, DynamicsFlutterFrequency)
	SHADER_PARAMETER(float, DynamicsTipPower)
	SHADER_PARAMETER(float, DynamicsGravityStrength)
	SHADER_PARAMETER(FVector3f, DynamicsWindDirection)
END_GLOBAL_SHADER_PARAMETER_STRUCT()

typedef TUniformBufferRef<FEnhancedHairCardsVFParameters> FEnhancedHairCardsUniformBuffer;

// ---------------------------------------------------------------------------
// Vertex / index buffer container owned by the component or asset
// ---------------------------------------------------------------------------
struct FEnhancedHairCardsVertexBuffers
{
	FBufferRHIRef PositionBuffer;
	FShaderResourceViewRHIRef PositionSRV;

	FBufferRHIRef PreviousPositionBuffer;
	FShaderResourceViewRHIRef PreviousPositionSRV;

	FBufferRHIRef NormalsBuffer;
	FShaderResourceViewRHIRef NormalsSRV;

	FBufferRHIRef UVsBuffer;
	FShaderResourceViewRHIRef UVsSRV;

	FBufferRHIRef MaterialsBuffer;
	FShaderResourceViewRHIRef MaterialsSRV;

	FBufferRHIRef VertexColorsBuffer;
	FShaderResourceViewRHIRef VertexColorsSRV;

	FBufferRHIRef DynamicsBuffer;
	FShaderResourceViewRHIRef DynamicsSRV;

	FBufferRHIRef IndexBufferRHI;
	FIndexBuffer  IndexBuffer;

	uint32 NumVertices = 0;
	uint32 NumIndices  = 0;
	uint32 NumUVs      = 2;
	FBox   BoundingBox = FBox(ForceInit);
};

// ---------------------------------------------------------------------------
// Vertex Factory
// ---------------------------------------------------------------------------
class FEnhancedHairCardsVertexFactory : public FVertexFactory
{
	DECLARE_VERTEX_FACTORY_TYPE(FEnhancedHairCardsVertexFactory);

public:
	struct FDataType
	{
		FEnhancedHairCardsVertexBuffers* Buffers  = nullptr;
		FEnhancedHairCardsUniformBuffer  UniformBuffer;
	};

	FEnhancedHairCardsVertexFactory(ERHIFeatureLevel::Type InFeatureLevel, const char* InDebugName);

	static bool ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
	static void ValidateCompiledResult(const FVertexFactoryType* Type, EShaderPlatform Platform, const FShaderParameterMap& ParameterMap, TArray<FString>& OutErrors);
	static void GetPSOPrecacheVertexFetchElements(EVertexInputStreamType VertexInputStreamType, FVertexDeclarationElementList& Elements);

	EPrimitiveIdMode GetPrimitiveIdMode(ERHIFeatureLevel::Type InFeatureLevel) const;

	void SetData(const FDataType& InData);
	void InitResources(FRHICommandListBase& RHICmdList);
	virtual void ReleaseResource() override;
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
	virtual void ReleaseRHI() override;

	const FDataType& GetData() const { return Data; }

	FDataType Data;

private:
	bool bIsInitialized = false;
};

struct FEnhancedHairCardsSettings;

FEnhancedHairCardsUniformBuffer CreateEnhancedHairCardsVFUniformBuffer(
	const FEnhancedHairCardsVertexBuffers* Buffers,
	uint32 NumUVs,
	const FEnhancedHairCardsSettings* Settings = nullptr);
