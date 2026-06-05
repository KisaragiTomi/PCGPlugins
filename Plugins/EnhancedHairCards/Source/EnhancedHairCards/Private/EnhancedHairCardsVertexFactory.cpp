#include "EnhancedHairCardsVertexFactory.h"
#include "EnhancedHairCardsDatas.h"

#include "SceneView.h"
#include "MeshBatch.h"
#include "ShaderParameterUtils.h"
#include "RenderGraphResources.h"
#include "MeshMaterialShader.h"
#include "RenderGraphUtils.h"
#include "MeshDrawShaderBindings.h"
#include "DataDrivenShaderPlatformInfo.h"

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FEnhancedHairCardsVFParameters, "EnhancedHairCardsVF");

#define ENHANCED_HAIR_CARDS_VF_PRIMITIVEID_STREAM_INDEX 13

// ---------------------------------------------------------------------------
// Shader parameter binding
// ---------------------------------------------------------------------------

class FEnhancedHairCardsVFShaderParameters : public FVertexFactoryShaderParameters
{
	DECLARE_TYPE_LAYOUT(FEnhancedHairCardsVFShaderParameters, NonVirtual);
public:
	void Bind(const FShaderParameterMap& ParameterMap) {}

	void GetElementShaderBindings(
		const class FSceneInterface* Scene,
		const FSceneView* View,
		const FMeshMaterialShader* Shader,
		const EVertexInputStreamType InputStreamType,
		ERHIFeatureLevel::Type FeatureLevel,
		const FVertexFactory* VertexFactory,
		const FMeshBatchElement& BatchElement,
		class FMeshDrawSingleShaderBindings& ShaderBindings,
		FVertexInputStreamArray& VertexStreams) const
	{
		const auto* VF = static_cast<const FEnhancedHairCardsVertexFactory*>(VertexFactory);
		check(VF && VF->Data.UniformBuffer);
		ShaderBindings.Add(
			Shader->GetUniformBufferParameter<FEnhancedHairCardsVFParameters>(),
			VF->Data.UniformBuffer);
	}
};

IMPLEMENT_TYPE_LAYOUT(FEnhancedHairCardsVFShaderParameters);

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

FEnhancedHairCardsVertexFactory::FEnhancedHairCardsVertexFactory(
	ERHIFeatureLevel::Type InFeatureLevel, const char* InDebugName)
	: FVertexFactory(InFeatureLevel)
{
}

bool FEnhancedHairCardsVertexFactory::ShouldCompilePermutation(
	const FVertexFactoryShaderPermutationParameters& Parameters)
{
	return Parameters.MaterialParameters.MaterialDomain == MD_Surface;
}

void FEnhancedHairCardsVertexFactory::ModifyCompilationEnvironment(
	const FVertexFactoryShaderPermutationParameters& Parameters,
	FShaderCompilerEnvironment& OutEnvironment)
{
	FVertexFactory::ModifyCompilationEnvironment(Parameters, OutEnvironment);

	OutEnvironment.SetDefine(TEXT("ENHANCED_HAIR_CARD_MESH_FACTORY"), 1);
	OutEnvironment.SetDefine(TEXT("HAIR_CARD_MESH_FACTORY"), 1);
	const bool bUseGPUSceneAndPrimitiveIdStream = Parameters.VertexFactoryType->SupportsPrimitiveIdStream()
		&& UseGPUScene(Parameters.Platform, GetMaxSupportedFeatureLevel(Parameters.Platform));
	OutEnvironment.SetDefine(TEXT("VF_SUPPORTS_PRIMITIVE_SCENE_DATA"), bUseGPUSceneAndPrimitiveIdStream);
	OutEnvironment.SetDefine(TEXT("MANUAL_VERTEX_FETCH"), RHISupportsManualVertexFetch(Parameters.Platform) ? TEXT("1") : TEXT("0"));
	OutEnvironment.SetDefine(TEXT("ENHANCED_HAIR_CARDS_MAX_UV"), ENHANCED_HAIR_CARDS_MAX_UV);
	OutEnvironment.IncludeVirtualPathToContentsMap.Add(
		TEXT("/Engine/Private/HairStrands/HairCardsAttributeCommon.ush"),
		TEXT("#pragma once\n#include \"/Plugin/EnhancedHairCards/Private/EnhancedHairCardsAttributes.ush\"\n"));
}

void FEnhancedHairCardsVertexFactory::ValidateCompiledResult(
	const FVertexFactoryType* Type, EShaderPlatform Platform,
	const FShaderParameterMap& ParameterMap, TArray<FString>& OutErrors)
{
}

void FEnhancedHairCardsVertexFactory::GetPSOPrecacheVertexFetchElements(
	EVertexInputStreamType VertexInputStreamType,
	FVertexDeclarationElementList& Elements)
{
	if (!PlatformGPUSceneUsesUniformBufferView(GMaxRHIShaderPlatform))
	{
		Elements.Add(FVertexElement(0, 0, VET_UInt, ENHANCED_HAIR_CARDS_VF_PRIMITIVEID_STREAM_INDEX, 0, true));
	}
}

EPrimitiveIdMode FEnhancedHairCardsVertexFactory::GetPrimitiveIdMode(ERHIFeatureLevel::Type InFeatureLevel) const
{
	return PrimID_DynamicPrimitiveShaderData;
}

void FEnhancedHairCardsVertexFactory::SetData(const FDataType& InData)
{
	Data = InData;
	UpdateRHI(FRHICommandListImmediate::Get());
}

void FEnhancedHairCardsVertexFactory::InitResources(FRHICommandListBase& RHICmdList)
{
	if (bIsInitialized)
	{
		return;
	}

	bIsInitialized = true;
	bNeedsDeclaration = true;
	check(HasValidFeatureLevel());

	FVertexDeclarationElementList Elements;
	SetPrimitiveIdStreamIndex(GetFeatureLevel(), EVertexInputStreamType::Default, -1);
	AddPrimitiveIdStreamElement(
		EVertexInputStreamType::Default,
		Elements,
		ENHANCED_HAIR_CARDS_VF_PRIMITIVEID_STREAM_INDEX,
		ENHANCED_HAIR_CARDS_VF_PRIMITIVEID_STREAM_INDEX);
	InitDeclaration(Elements);
}

void FEnhancedHairCardsVertexFactory::InitRHI(FRHICommandListBase& RHICmdList)
{
	if (!bIsInitialized)
	{
		InitResources(RHICmdList);
	}
}

void FEnhancedHairCardsVertexFactory::ReleaseRHI()
{
	FVertexFactory::ReleaseRHI();
	bIsInitialized = false;
}

void FEnhancedHairCardsVertexFactory::ReleaseResource()
{
	FVertexFactory::ReleaseResource();
}

// ---------------------------------------------------------------------------
// Uniform buffer creation
// ---------------------------------------------------------------------------

FEnhancedHairCardsUniformBuffer CreateEnhancedHairCardsVFUniformBuffer(
	const FEnhancedHairCardsVertexBuffers* Buffers,
	uint32 NumUVs,
	const FEnhancedHairCardsSettings* Settings)
{
	FEnhancedHairCardsVFParameters Params;
	FMemory::Memzero(Params);

	if (!Buffers)
	{
		return FEnhancedHairCardsUniformBuffer::CreateUniformBufferImmediate(
			Params, UniformBuffer_MultiFrame);
	}

	Params.MaxVertexCount         = Buffers->NumVertices;
	Params.NumUVs                 = FMath::Clamp(NumUVs, 1u, (uint32)ENHANCED_HAIR_CARDS_MAX_UV);

	if (Settings)
	{
		Params.Flags = Settings->PackFlags();
	}
	else
	{
		Params.Flags = 0x80; // VertexColor always on
	}

	Params.PositionBuffer         = Buffers->PositionSRV;
	Params.PreviousPositionBuffer = Buffers->PreviousPositionSRV;
	Params.NormalsBuffer          = Buffers->NormalsSRV;
	Params.UVsBuffer              = Buffers->UVsSRV;
	Params.MaterialsBuffer        = Buffers->MaterialsSRV;
	Params.VertexColorsBuffer     = Buffers->VertexColorsSRV;
	Params.DynamicsBuffer         = Buffers->DynamicsSRV;

	if (Settings)
	{
		const FEnhancedHairCardsDynamicsSettings& Dynamics = Settings->Dynamics;
		FVector LocalWindDirection = Dynamics.LocalWindDirection;
		if (!LocalWindDirection.Normalize())
		{
			LocalWindDirection = FVector(0.f, 1.f, 0.f);
		}

		Params.DynamicsFlags = Dynamics.PackFlags();
		Params.DynamicsStrength = FMath::Max(0.f, Dynamics.Strength);
		Params.DynamicsWindStrength = FMath::Max(0.f, Dynamics.WindStrength);
		Params.DynamicsFlutterStrength = FMath::Max(0.f, Dynamics.FlutterStrength);
		Params.DynamicsFlutterFrequency = FMath::Max(0.01f, Dynamics.FlutterFrequency);
		Params.DynamicsTipPower = FMath::Max(0.01f, Dynamics.TipPower);
		Params.DynamicsGravityStrength = Dynamics.GravityStrength;
		Params.DynamicsWindDirection = FVector3f(LocalWindDirection);
	}
	else
	{
		Params.DynamicsFlags = 0;
		Params.DynamicsStrength = 0.f;
		Params.DynamicsWindStrength = 0.f;
		Params.DynamicsFlutterStrength = 0.f;
		Params.DynamicsFlutterFrequency = 1.f;
		Params.DynamicsTipPower = 1.f;
		Params.DynamicsGravityStrength = 0.f;
		Params.DynamicsWindDirection = FVector3f(0.f, 1.f, 0.f);
	}

	return FEnhancedHairCardsUniformBuffer::CreateUniformBufferImmediate(
		Params, UniformBuffer_MultiFrame);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

IMPLEMENT_VERTEX_FACTORY_TYPE(
	FEnhancedHairCardsVertexFactory,
	"/Plugin/EnhancedHairCards/Private/EnhancedHairCardsVertexFactory.ush",
	EVertexFactoryFlags::UsedWithMaterials
	| EVertexFactoryFlags::SupportsDynamicLighting
	| EVertexFactoryFlags::SupportsPrecisePrevWorldPos
	| EVertexFactoryFlags::SupportsCachingMeshDrawCommands
	| EVertexFactoryFlags::SupportsPrimitiveIdStream
	| EVertexFactoryFlags::SupportsPSOPrecaching
	| EVertexFactoryFlags::SupportsManualVertexFetch
);

IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(
	FEnhancedHairCardsVertexFactory, SF_Vertex, FEnhancedHairCardsVFShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(
	FEnhancedHairCardsVertexFactory, SF_Pixel,  FEnhancedHairCardsVFShaderParameters);
