#include "CSGpuInstancedMeshVertexFactory.h"

#include "MeshBatch.h"
#include "MeshDrawShaderBindings.h"
#include "MeshMaterialShader.h"
#include "RHIStaticStates.h"
#include "ShaderParameterUtils.h"

IMPLEMENT_TYPE_LAYOUT(FCSGpuInstancedMeshVFShaderParameters);

bool FCSGpuInstancedMeshVertexFactory::ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters)
{
	// Same gate as the engine's instanced factory: only materials that opted into instancing
	// pay for this extra permutation. Reusing the existing "Used with Instanced Static Meshes"
	// flag keeps the material setup identical to HISM's.
	return (Parameters.MaterialParameters.bIsUsedWithInstancedStaticMeshes || Parameters.MaterialParameters.bIsSpecialEngineMaterial)
		&& FLocalVertexFactory::ShouldCompilePermutation(Parameters);
}

void FCSGpuInstancedMeshVertexFactory::ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FLocalVertexFactory::ModifyCompilationEnvironment(Parameters, OutEnvironment);

	// Force the manual-fetch instancing path unconditionally. FInstancedStaticMeshVertexFactory
	// picks USE_INSTANCE_CULLING (GPU Scene) whenever UseGPUScene() is true, which would take the
	// instance transforms out of our hands; VF_SUPPORTS_PRIMITIVE_SCENE_DATA is already 0 for this
	// factory because it is registered without SupportsPrimitiveIdStream.
	OutEnvironment.SetDefine(TEXT("USE_INSTANCING"), TEXT("1"));

	// No dithered LOD cross-fade: LOD is picked per instance by the cull pass and the instance
	// simply moves to another draw, so there is no transition to dither. Defining it to 0 also
	// keeps NEEDS_PER_INSTANCE_PARAMS off the RandomID-driven code path.
	OutEnvironment.SetDefine(TEXT("USE_DITHERED_LOD_TRANSITION_FOR_INSTANCED"), TEXT("0"));
}

void FCSGpuInstancedMeshVertexFactory::InitRHI(FRHICommandListBase& RHICmdList)
{
	// The base builds the declaration and the LocalVF uniform buffer (manual vertex fetch).
	// AddPrimitiveIdStreamElement inside it is a no-op for this factory type, and the instance
	// attributes are not vertex streams here — they are fetched from InstanceVF.
	FLocalVertexFactory::InitRHI(RHICmdList);

	FInstancedStaticMeshVertexFactoryUniformShaderParameters UniformParameters;
	UniformParameters.VertexFetch_InstanceOriginBuffer = InstanceOriginSRV;
	UniformParameters.VertexFetch_InstanceTransformBuffer = InstanceTransformSRV;
	UniformParameters.VertexFetch_InstanceLightmapBuffer = InstanceLightmapSRV;
	UniformParameters.InstanceCustomDataBuffer = InstanceOriginSRV; // unused; must be non-null
	UniformParameters.NumCustomDataFloats = 0;
	InstanceUniformBuffer = TUniformBufferRef<FInstancedStaticMeshVertexFactoryUniformShaderParameters>::CreateUniformBufferImmediate(
		UniformParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);
}

void FCSGpuInstancedMeshVertexFactory::ReleaseRHI()
{
	InstanceUniformBuffer.SafeRelease();
	FLocalVertexFactory::ReleaseRHI();
}

void FCSGpuInstancedMeshVFShaderParameters::GetElementShaderBindings(
	const class FSceneInterface* Scene,
	const FSceneView* View,
	const FMeshMaterialShader* Shader,
	const EVertexInputStreamType InputStreamType,
	ERHIFeatureLevel::Type FeatureLevel,
	const FVertexFactory* VertexFactory,
	const FMeshBatchElement& BatchElement,
	FMeshDrawSingleShaderBindings& ShaderBindings,
	FVertexInputStreamArray& VertexStreams) const
{
	const auto* InstancedVF = static_cast<const FCSGpuInstancedMeshVertexFactory*>(VertexFactory);

	// The batch never sets VertexFactoryUserData, so fall back to the factory's own LocalVF
	// uniform buffer (which manual vertex fetch needs).
	FRHIUniformBuffer* VertexFactoryUniformBuffer = static_cast<FRHIUniformBuffer*>(BatchElement.VertexFactoryUserData);
	if (!VertexFactoryUniformBuffer) VertexFactoryUniformBuffer = InstancedVF->GetUniformBuffer();

	FLocalVertexFactoryShaderParametersBase::GetElementShaderBindingsBase(
		Scene, View, Shader, InputStreamType, FeatureLevel, VertexFactory, BatchElement,
		VertexFactoryUniformBuffer, ShaderBindings, VertexStreams);

	// Start of this batch's region in the visible-instance buffers; SV_InstanceID is added to it.
	ShaderBindings.Add(InstanceOffsetParameter, (uint32)FMath::Max(BatchElement.UserIndex, 0));

	ShaderBindings.Add(Shader->GetUniformBufferParameter<FInstancedStaticMeshVertexFactoryUniformShaderParameters>(),
		InstancedVF->GetInstanceUniformBuffer());
	ShaderBindings.Add(VertexFetch_InstanceOriginBufferParameter, InstancedVF->GetInstanceOriginSRV());
	ShaderBindings.Add(VertexFetch_InstanceTransformBufferParameter, InstancedVF->GetInstanceTransformSRV());
	ShaderBindings.Add(VertexFetch_InstanceLightmapBufferParameter, InstancedVF->GetInstanceLightmapSRV());

	// Per-LOD pivot correction (HISM uses it for its LOD-range batches); nothing to offset here.
	ShaderBindings.Add(InstancingOffsetParameter, FVector4f(ForceInit));

	// InstancingFadeOutParams / view-Z compare live here; the leaf fills them per batch so that
	// PerInstanceParams resolves to "fully visible, no fade" (see FCSGpuInstancedMeshSceneProxy).
	ShaderBindings.Add(Shader->GetUniformBufferParameter<FInstancedStaticMeshVFLooseUniformShaderParameters>(),
		BatchElement.LooseParametersUniformBuffer);
}

IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FCSGpuInstancedMeshVertexFactory, SF_Vertex, FCSGpuInstancedMeshVFShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FCSGpuInstancedMeshVertexFactory, SF_Pixel, FCSGpuInstancedMeshVFShaderParameters);

// No SupportsPrimitiveIdStream: that flag is what would switch LocalVertexFactory.ush over to
// GPU-Scene instance data. No SupportsCachingMeshDrawCommands / SupportsStaticLighting either —
// this factory only ever draws through the dynamic-relevance path of FCSGpuMeshSceneProxy.
IMPLEMENT_VERTEX_FACTORY_TYPE(FCSGpuInstancedMeshVertexFactory, "/Engine/Private/LocalVertexFactory.ush",
	  EVertexFactoryFlags::UsedWithMaterials
	| EVertexFactoryFlags::SupportsDynamicLighting
	| EVertexFactoryFlags::SupportsPrecisePrevWorldPos
	| EVertexFactoryFlags::SupportsManualVertexFetch
	| EVertexFactoryFlags::DoesNotSupportNullPixelShader
	| EVertexFactoryFlags::SupportsPSOPrecaching
);
