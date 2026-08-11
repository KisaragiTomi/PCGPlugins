#pragma once

#include "CoreMinimal.h"
#include "LocalVertexFactory.h"
#include "Engine/InstancedStaticMesh.h" // FInstancedStaticMeshVertexFactoryUniformShaderParameters ("InstanceVF")

/**
 * Vertex factory for UCSGpuInstancedMeshComponent: one GPU-resident copy of the base mesh,
 * drawn N times with per-instance transforms that the vertex shader MANUAL-FETCHES from
 * plain GPU buffers indexed by SV_InstanceID.
 *
 * It reuses the engine's /Engine/Private/LocalVertexFactory.ush unchanged. That file has two
 * instancing paths:
 *   USE_INSTANCE_CULLING — instance data comes from GPU Scene (what ISM/HISM get on desktop).
 *   USE_INSTANCING       — instance data comes from InstanceVF.VertexFetch_Instance*Buffer,
 *                          i.e. from SRVs the vertex factory supplies (the ES3.1 ISM path).
 * FInstancedStaticMeshVertexFactory picks the first whenever UseGPUScene() is true, which is
 * always on SM5 — and GPU Scene instance data can only be filled from the CPU. This factory
 * therefore forces USE_INSTANCING and declares itself WITHOUT SupportsPrimitiveIdStream so
 * VF_USE_PRIMITIVE_SCENE_DATA compiles to 0: the shader then reads the instance transform out
 * of *our* compute-written buffers, which is the whole point — the visible-instance list is
 * produced on the GPU by the cull pass and never round-trips to the CPU.
 *
 * That also matches what FCSGpuMeshSceneProxy already does: it draws through dynamic relevance
 * with an FDynamicPrimitiveUniformBuffer rather than a GPU-Scene primitive, so dropping the
 * primitive-id stream costs nothing here.
 *
 * Instance layout expected by LocalVertexFactory.ush (see GetInstanceTransform there):
 *   TransformBuffer: Buffer<float4>, 3 per instance — rows of the instance-to-component 3x3,
 *                    .w of row 0 carries hitproxy/selection (write 0).
 *   OriginBuffer:    Buffer<float4>, 1 per instance — xyz = instance origin in component space,
 *                    w = per-instance random (material's PerInstanceRandom).
 *   LightmapBuffer:  Buffer<float4>, 1 per instance — lightmap/shadowmap UV bias (write 0).
 * The final vertex transform is InstanceToComponent * Primitive.LocalToWorld, so instance
 * transforms are component-local exactly like HISM's.
 */
struct FCSGpuInstancedMeshVertexFactory : public FLocalVertexFactory
{
	DECLARE_VERTEX_FACTORY_TYPE(FCSGpuInstancedMeshVertexFactory);

public:
	FCSGpuInstancedMeshVertexFactory(ERHIFeatureLevel::Type InFeatureLevel, const char* InDebugName)
		: FLocalVertexFactory(InFeatureLevel, InDebugName)
	{
	}

	static bool ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);

	/** Hand the factory the per-instance SRVs. Must be called before InitResource(); the
	 *  InstanceVF uniform buffer is built from them in InitRHI. */
	void SetInstanceStreams(FRHIShaderResourceView* InOriginSRV, FRHIShaderResourceView* InTransformSRV, FRHIShaderResourceView* InLightmapSRV)
	{
		InstanceOriginSRV = InOriginSRV;
		InstanceTransformSRV = InTransformSRV;
		InstanceLightmapSRV = InLightmapSRV;
	}

	//~ FRenderResource interface
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
	virtual void ReleaseRHI() override;

	FRHIShaderResourceView* GetInstanceOriginSRV() const { return InstanceOriginSRV; }
	FRHIShaderResourceView* GetInstanceTransformSRV() const { return InstanceTransformSRV; }
	FRHIShaderResourceView* GetInstanceLightmapSRV() const { return InstanceLightmapSRV; }
	FRHIUniformBuffer* GetInstanceUniformBuffer() const { return InstanceUniformBuffer.GetReference(); }

private:
	// Not owned: these live on the scene proxy's stream set, which outlives the factory.
	FRHIShaderResourceView* InstanceOriginSRV = nullptr;
	FRHIShaderResourceView* InstanceTransformSRV = nullptr;
	FRHIShaderResourceView* InstanceLightmapSRV = nullptr;

	TUniformBufferRef<FInstancedStaticMeshVertexFactoryUniformShaderParameters> InstanceUniformBuffer;
};

/**
 * Binds the per-instance SRVs / InstanceVF uniform buffer on top of the standard local-VF
 * bindings. FInstancedStaticMeshVertexFactoryShaderParameters cannot be reused: it skips every
 * instance binding when UseGPUScene() is true, which is exactly the case we are overriding.
 *
 * FMeshBatchElement::UserIndex carries the instance-buffer offset for the batch — the instanced
 * leaf gives each LOD its own fixed region in the visible-instance buffers and passes that
 * region's start here, since SV_InstanceID always restarts at 0 per draw.
 */
class FCSGpuInstancedMeshVFShaderParameters : public FLocalVertexFactoryShaderParametersBase
{
	DECLARE_TYPE_LAYOUT(FCSGpuInstancedMeshVFShaderParameters, NonVirtual);

public:
	void Bind(const FShaderParameterMap& ParameterMap)
	{
		FLocalVertexFactoryShaderParametersBase::Bind(ParameterMap);

		InstancingOffsetParameter.Bind(ParameterMap, TEXT("InstancingOffset"));
		VertexFetch_InstanceOriginBufferParameter.Bind(ParameterMap, TEXT("VertexFetch_InstanceOriginBuffer"));
		VertexFetch_InstanceTransformBufferParameter.Bind(ParameterMap, TEXT("VertexFetch_InstanceTransformBuffer"));
		VertexFetch_InstanceLightmapBufferParameter.Bind(ParameterMap, TEXT("VertexFetch_InstanceLightmapBuffer"));
		InstanceOffsetParameter.Bind(ParameterMap, TEXT("InstanceOffset"));
	}

	void GetElementShaderBindings(
		const class FSceneInterface* Scene,
		const FSceneView* View,
		const FMeshMaterialShader* Shader,
		const EVertexInputStreamType InputStreamType,
		ERHIFeatureLevel::Type FeatureLevel,
		const FVertexFactory* VertexFactory,
		const FMeshBatchElement& BatchElement,
		FMeshDrawSingleShaderBindings& ShaderBindings,
		FVertexInputStreamArray& VertexStreams) const;

private:
	LAYOUT_FIELD(FShaderParameter, InstancingOffsetParameter);
	LAYOUT_FIELD(FShaderResourceParameter, VertexFetch_InstanceOriginBufferParameter);
	LAYOUT_FIELD(FShaderResourceParameter, VertexFetch_InstanceTransformBufferParameter);
	LAYOUT_FIELD(FShaderResourceParameter, VertexFetch_InstanceLightmapBufferParameter);
	LAYOUT_FIELD(FShaderParameter, InstanceOffsetParameter);
};
