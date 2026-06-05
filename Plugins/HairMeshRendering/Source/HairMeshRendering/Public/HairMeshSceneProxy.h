#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "HairMeshTextureBuilder.h"
#include "HairMeshStylingParams.h"
#include "RenderResource.h"
#include "RHIResources.h"

class UHairMeshComponent;

/**
 * GPU-side resources for the 5 hair mesh textures + generation buffers.
 */
struct FHairMeshGPUResources
{
	// 5 hair mesh textures
	FTextureRHIRef HairMeshTexture3D;
	FTextureRHIRef UVTexture2D;
	FTextureRHIRef WTexture3D;
	FTextureRHIRef UDirTexture3D;
	FTextureRHIRef VDirTexture3D;
	FSamplerStateRHIRef LinearSampler;

	// Bundle mappings SRV
	FBufferRHIRef BundleMappingsBuffer;
	FShaderResourceViewRHIRef BundleMappingsSRV;

	// Blue noise samples SRV
	FBufferRHIRef BlueNoiseSamplesBuffer;
	FShaderResourceViewRHIRef BlueNoiseSamplesSRV;
	int32 NumBlueNoiseSamples = 0;

	// Generated hair vertex output
	FBufferRHIRef OutputVertexBuffer;
	FUnorderedAccessViewRHIRef OutputVertexUAV;
	FShaderResourceViewRHIRef OutputVertexSRV;

	// Indirect draw args
	FBufferRHIRef IndirectArgsBuffer;
	FUnorderedAccessViewRHIRef IndirectArgsUAV;

	bool IsValid() const { return HairMeshTexture3D.IsValid() && OutputVertexBuffer.IsValid(); }
};

/**
 * Render-thread proxy for hair-mesh rendering.
 * Dispatches compute shader for strand generation, then draws with indirect.
 */
class FHairMeshSceneProxy : public FPrimitiveSceneProxy
{
public:
	explicit FHairMeshSceneProxy(const UHairMeshComponent* InComponent);
	virtual ~FHairMeshSceneProxy() override;

	virtual SIZE_T GetTypeHash() const override;
	virtual uint32 GetMemoryFootprint() const override;
	virtual void GetDynamicMeshElements(
		const TArray<const FSceneView*>& Views,
		const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap,
		FMeshElementCollector& Collector) const override;
	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;

private:
	void CreateGPUResources_RenderThread(FRHICommandListImmediate& RHICmdList);
	void GenerateBlueNoiseSamples(int32 MaxCount);

	static FTextureRHIRef CreateVolumeTexture(
		const FHairMeshVolumeData& Data, EPixelFormat Format, const TCHAR* DebugName);
	static FTextureRHIRef Create2DTexture(
		const FHairMeshSliceData& Data, EPixelFormat Format, const TCHAR* DebugName);

	// Cached from game thread
	FHairMeshTextureSet CachedTextureSet;
	FHairMeshStylingParams StylingParams;
	FMatrix LocalToWorldMatrix;

	int32 NumBundles = 0;
	int32 MaxStrandsPerBundle = 0;
	int32 VertsPerStrand = 0;
	int32 NumExtrusionLayers = 0;

	// Blue noise CPU data (transferred to GPU)
	TArray<FVector2f> BlueNoiseSamplesCPU;

	// GPU resources
	FHairMeshGPUResources GPUResources;
	bool bGPUResourcesReady = false;
};
