#include "HairMeshSceneProxy.h"
#include "HairMeshComponent.h"
#include "HairMeshAsset.h"
#include "HairMeshShaders.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "GlobalShader.h"
#include "ShaderParameterUtils.h"
#include "RHIStaticStates.h"
#include "PipelineStateCache.h"

// ============================================================================
// Construction / Destruction
// ============================================================================

FHairMeshSceneProxy::FHairMeshSceneProxy(const UHairMeshComponent* InComponent)
	: FPrimitiveSceneProxy(InComponent)
{
	if (InComponent && InComponent->HairMeshAsset)
	{
		NumBundles = InComponent->HairMeshAsset->Bundles.Num();
		MaxStrandsPerBundle = InComponent->HairMeshAsset->DefaultMaxStrands;
		VertsPerStrand = InComponent->HairMeshAsset->MaxVerticesPerStrand;
		NumExtrusionLayers = InComponent->HairMeshAsset->NumExtrusionLayers;
		CachedTextureSet = InComponent->GetTextureSet();
		StylingParams = InComponent->StylingParams;
		LocalToWorldMatrix = InComponent->GetComponentTransform().ToMatrixWithScale();
	}

	GenerateBlueNoiseSamples(MaxStrandsPerBundle);

	ENQUEUE_RENDER_COMMAND(HairMeshCreateGPU)(
		[this](FRHICommandListImmediate& RHICmdList)
		{
			CreateGPUResources_RenderThread(RHICmdList);
		});
}

FHairMeshSceneProxy::~FHairMeshSceneProxy()
{
	GPUResources = FHairMeshGPUResources();
}

SIZE_T FHairMeshSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

uint32 FHairMeshSceneProxy::GetMemoryFootprint() const
{
	return sizeof(*this) + GetAllocatedSize();
}

// ============================================================================
// Blue Noise Generation (simple approximation)
// ============================================================================

void FHairMeshSceneProxy::GenerateBlueNoiseSamples(int32 MaxCount)
{
	BlueNoiseSamplesCPU.SetNum(MaxCount);

	// Progressive Halton sequence as a practical approximation of blue noise
	auto Halton = [](int32 Index, int32 Base) -> float
	{
		float Result = 0.0f;
		float F = 1.0f / Base;
		int32 I = Index;
		while (I > 0)
		{
			Result += F * (I % Base);
			I /= Base;
			F /= Base;
		}
		return Result;
	};

	for (int32 i = 0; i < MaxCount; ++i)
	{
		BlueNoiseSamplesCPU[i] = FVector2f(Halton(i + 1, 2), Halton(i + 1, 3));
	}
}

// ============================================================================
// GPU Resource Creation
// ============================================================================

void FHairMeshSceneProxy::CreateGPUResources_RenderThread(FRHICommandListImmediate& RHICmdList)
{
	check(IsInRenderingThread());

	if (!CachedTextureSet.IsValid()) return;

	// --- 5 Hair Mesh Textures ---
	GPUResources.HairMeshTexture3D = CreateVolumeTexture(
		CachedTextureSet.HairMeshVolume, PF_A32B32G32R32F, TEXT("HairMeshTex3D"));
	GPUResources.UVTexture2D = Create2DTexture(
		CachedTextureSet.UVSlice, PF_A32B32G32R32F, TEXT("HairUVTex2D"));
	GPUResources.WTexture3D = CreateVolumeTexture(
		CachedTextureSet.WVolume, PF_A32B32G32R32F, TEXT("HairWTex3D"));
	GPUResources.UDirTexture3D = CreateVolumeTexture(
		CachedTextureSet.UDirVolume, PF_A32B32G32R32F, TEXT("HairUDirTex3D"));
	GPUResources.VDirTexture3D = CreateVolumeTexture(
		CachedTextureSet.VDirVolume, PF_A32B32G32R32F, TEXT("HairVDirTex3D"));

	FSamplerStateInitializerRHI SamplerInit(SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp);
	GPUResources.LinearSampler = RHICreateSamplerState(SamplerInit);

	// --- Bundle Mappings ---
	{
		const int32 NumMappings = CachedTextureSet.BundleMappings.Num();
		const uint32 BufSize = sizeof(FIntPoint) * NumMappings;
		GPUResources.BundleMappingsBuffer = RHICmdList.CreateBuffer(
			FRHIBufferCreateDesc::CreateStructured(TEXT("HairBundleMappings"), BufSize, sizeof(FIntPoint))
			.AddUsage(BUF_ShaderResource));

		void* Mapped = RHICmdList.LockBuffer(GPUResources.BundleMappingsBuffer, 0, BufSize, RLM_WriteOnly);
		for (int32 i = 0; i < NumMappings; ++i)
		{
			FIntPoint& P = static_cast<FIntPoint*>(Mapped)[i];
			P.X = CachedTextureSet.BundleMappings[i].BlockX;
			P.Y = CachedTextureSet.BundleMappings[i].BlockY;
		}
		RHICmdList.UnlockBuffer(GPUResources.BundleMappingsBuffer);

		GPUResources.BundleMappingsSRV = RHICmdList.CreateShaderResourceView(
			GPUResources.BundleMappingsBuffer,
			FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(GPUResources.BundleMappingsBuffer));
	}

	// --- Blue Noise Samples ---
	{
		const int32 NumSamples = BlueNoiseSamplesCPU.Num();
		const uint32 BufSize = sizeof(FVector2f) * NumSamples;
		GPUResources.BlueNoiseSamplesBuffer = RHICmdList.CreateBuffer(
			FRHIBufferCreateDesc::CreateStructured(TEXT("HairBlueNoise"), BufSize, sizeof(FVector2f))
			.AddUsage(BUF_ShaderResource));

		void* Mapped = RHICmdList.LockBuffer(GPUResources.BlueNoiseSamplesBuffer, 0, BufSize, RLM_WriteOnly);
		FMemory::Memcpy(Mapped, BlueNoiseSamplesCPU.GetData(), BufSize);
		RHICmdList.UnlockBuffer(GPUResources.BlueNoiseSamplesBuffer);

		GPUResources.BlueNoiseSamplesSRV = RHICmdList.CreateShaderResourceView(
			GPUResources.BlueNoiseSamplesBuffer,
			FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(GPUResources.BlueNoiseSamplesBuffer));
		GPUResources.NumBlueNoiseSamples = NumSamples;
	}

	// --- Output Vertex Buffer ---
	{
		const int32 TotalStrands = NumBundles * MaxStrandsPerBundle;
		const int32 VertsPerStrip = VertsPerStrand * 2;
		const int32 TotalOutputVerts = FMath::Max(TotalStrands * VertsPerStrip, 1);
		const uint32 BufSize = sizeof(FHairTriStripVertex) * TotalOutputVerts;

		GPUResources.OutputVertexBuffer = RHICmdList.CreateBuffer(
			FRHIBufferCreateDesc::CreateStructured(TEXT("HairOutputVerts"), BufSize, sizeof(FHairTriStripVertex))
			.AddUsage(BUF_UnorderedAccess | BUF_ShaderResource));

		GPUResources.OutputVertexUAV = RHICmdList.CreateUnorderedAccessView(
			GPUResources.OutputVertexBuffer,
			FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(GPUResources.OutputVertexBuffer));
		GPUResources.OutputVertexSRV = RHICmdList.CreateShaderResourceView(
			GPUResources.OutputVertexBuffer,
			FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(GPUResources.OutputVertexBuffer));
	}

	// --- Indirect Draw Args ---
	{
		GPUResources.IndirectArgsBuffer = RHICmdList.CreateBuffer(
			FRHIBufferCreateDesc::Create(TEXT("HairIndirectArgs"),
				sizeof(uint32) * 4, 0,
				BUF_UnorderedAccess | BUF_DrawIndirect | BUF_VertexBuffer));

		GPUResources.IndirectArgsUAV = RHICmdList.CreateUnorderedAccessView(
			GPUResources.IndirectArgsBuffer,
			FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Typed)
				.SetFormat(PF_R32_UINT));
	}

	CachedTextureSet = FHairMeshTextureSet();
	bGPUResourcesReady = true;
}

// ============================================================================
// Dynamic Mesh Elements — Dispatch + Draw
// ============================================================================

void FHairMeshSceneProxy::GetDynamicMeshElements(
	const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily,
	uint32 VisibilityMap,
	FMeshElementCollector& Collector) const
{
	if (!bGPUResourcesReady || !GPUResources.IsValid()) return;

	// Note: Full RDG integration would use FMeshPassProcessor.
	// For Phase 2 we demonstrate the dispatch logic.
	// Actual rendering will be wired up when integrated into UE's rendering pipeline.

	// The compute shader dispatch + indirect draw would be performed in a custom
	// render pass registered via GetViewRelevance + a custom MeshPassProcessor,
	// or via FRendererModule::RegisterPostOpaqueRenderDelegate.
	// This is left as the integration point for Phase 5.
}

FPrimitiveViewRelevance FHairMeshSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View);
	Result.bShadowRelevance = IsShadowCast(View);
	Result.bDynamicRelevance = true;
	Result.bRenderInMainPass = ShouldRenderInMainPass();
	return Result;
}

// ============================================================================
// Texture Creation Helpers
// ============================================================================

FTextureRHIRef FHairMeshSceneProxy::CreateVolumeTexture(
	const FHairMeshVolumeData& Data, EPixelFormat Format, const TCHAR* DebugName)
{
	if (Data.SizeX <= 0 || Data.SizeY <= 0 || Data.SizeZ <= 0) return nullptr;

	const FRHITextureCreateDesc Desc =
		FRHITextureCreateDesc::Create3D(DebugName)
		.SetExtent(Data.SizeX, Data.SizeY)
		.SetDepth(Data.SizeZ)
		.SetFormat(Format)
		.SetNumMips(1)
		.SetFlags(TexCreate_ShaderResource);

	FTextureRHIRef Texture = RHICreateTexture(Desc);

	{
		const int32 PixelSize = sizeof(FLinearColor);
		const uint32 SrcRowPitch = Data.SizeX * PixelSize;
		const uint32 SrcDepthPitch = Data.SizeX * Data.SizeY * PixelSize;

		FUpdateTextureRegion3D UpdateRegion(0, 0, 0, 0, 0, 0, Data.SizeX, Data.SizeY, Data.SizeZ);
		FRHICommandListImmediate& RHICmdList = FRHICommandListImmediate::Get();
		RHICmdList.UpdateTexture3D(
			Texture, 0, UpdateRegion,
			SrcRowPitch, SrcDepthPitch,
			reinterpret_cast<const uint8*>(Data.Pixels.GetData()));
	}

	return Texture;
}

FTextureRHIRef FHairMeshSceneProxy::Create2DTexture(
	const FHairMeshSliceData& Data, EPixelFormat Format, const TCHAR* DebugName)
{
	if (Data.SizeX <= 0 || Data.SizeY <= 0) return nullptr;

	const FRHITextureCreateDesc Desc =
		FRHITextureCreateDesc::Create2D(DebugName)
		.SetExtent(Data.SizeX, Data.SizeY)
		.SetFormat(Format)
		.SetNumMips(1)
		.SetFlags(TexCreate_ShaderResource);

	FTextureRHIRef Texture = RHICreateTexture(Desc);

	uint32 DestStride;
	void* MappedData = RHILockTexture2D(Texture, 0, RLM_WriteOnly, DestStride, false);
	if (MappedData)
	{
		const int32 PixelSize = sizeof(FLinearColor);
		const int32 SrcRowPitch = Data.SizeX * PixelSize;

		for (int32 Y = 0; Y < Data.SizeY; ++Y)
		{
			const FLinearColor* SrcRow = &Data.Pixels[Y * Data.SizeX];
			uint8* DstRow = static_cast<uint8*>(MappedData) + Y * DestStride;
			FMemory::Memcpy(DstRow, SrcRow, SrcRowPitch);
		}
		RHIUnlockTexture2D(Texture, 0, false);
	}

	return Texture;
}
