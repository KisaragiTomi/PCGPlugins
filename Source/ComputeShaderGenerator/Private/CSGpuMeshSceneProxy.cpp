#include "CSGpuMeshSceneProxy.h"
#include "CSMesh.h"

#include "Components/PrimitiveComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "MaterialDomain.h"
#include "MaterialShared.h"
#include "MeshBatch.h"
#include "SceneManagement.h"
#include "SceneInterface.h"
#include "RHICommandList.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"

FCSGpuMeshSceneProxy::FCSGpuMeshSceneProxy(const UPrimitiveComponent* Component, UMaterialInterface* InMaterial, const char* DebugName)
	: FPrimitiveSceneProxy(Component)
	, VertexFactoryDebugName(DebugName)
	, Material(InMaterial)
{
	if (!Material) Material = UMaterial::GetDefaultMaterial(MD_Surface);
	MaterialRelevance = Material->GetRelevance_Concurrent(GetScene().GetShaderPlatform());

	bVerifyUsedMaterials = false;
	bSupportsDistanceFieldRepresentation = false;
}

FCSGpuMeshSceneProxy::~FCSGpuMeshSceneProxy()
{
}

uint32 FCSGpuMeshSceneProxy::GetMemoryFootprint() const
{
	return sizeof(*this) + GetAllocatedSize() + Streams.GetAllocatedSize();
}

bool FCSGpuMeshSceneProxy::CanBeOccluded() const
{
	return !MaterialRelevance.bDisableDepthTest;
}

void FCSGpuMeshSceneProxy::CreateRenderThreadResources(FRHICommandListBase& RHICmdList)
{
	FPrimitiveSceneProxy::CreateRenderThreadResources(RHICmdList);
	InitGpuGeometry(RHICmdList);
}

void FCSGpuMeshSceneProxy::DestroyRenderThreadResources()
{
	// Release the vertex factory before the buffers it streams from (matches the
	// original per-proxy teardown order).
	if (VertexFactory)
	{
		VertexFactory->ReleaseResource();
		VertexFactory.Reset();
	}
	ReleaseGpuGeometry();
	FPrimitiveSceneProxy::DestroyRenderThreadResources();
}

void FCSGpuMeshSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const
{
	if (!DrawDesc.bValid || DrawDesc.IndexBuffer == nullptr || !VertexFactory) return;

	FMaterialRenderProxy* MaterialProxy = Material->GetRenderProxy();
	SubmitGpuBufferDraw(*this, Views, VisibilityMap, Collector, *VertexFactory, *MaterialProxy,
		*DrawDesc.IndexBuffer, PT_TriangleList, DrawDesc.NumPrimitives, DrawDesc.MaxVertexIndex,
		bBatchCastShadow, DrawDesc.IndirectArgsBuffer, DrawDesc.IndirectArgsOffset);
}

void FCSGpuMeshSceneProxy::SubmitGpuBufferDraw(
	const FPrimitiveSceneProxy& SceneProxy,
	const TArray<const FSceneView*>& Views,
	uint32 VisibilityMap,
	FMeshElementCollector& Collector,
	const FVertexFactory& InVertexFactory,
	FMaterialRenderProxy& MaterialProxy,
	const FIndexBuffer& IndexBuffer,
	EPrimitiveType PrimitiveType,
	uint32 NumPrimitives,
	uint32 MaxVertexIndex,
	bool bCastShadow,
	FRHIBuffer* IndirectArgsBuffer,
	uint32 IndirectArgsOffset)
{

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		if ((VisibilityMap & (1 << ViewIndex)) == 0) continue;

		FMeshBatch& Mesh = Collector.AllocateMesh();
		Mesh.VertexFactory = &InVertexFactory;
		Mesh.MaterialRenderProxy = &MaterialProxy;
		Mesh.ReverseCulling = SceneProxy.IsLocalToWorldDeterminantNegative();
		Mesh.Type = PrimitiveType;
		Mesh.DepthPriorityGroup = SDPG_World;
		Mesh.bCanApplyViewModeOverrides = false;
		Mesh.CastShadow = bCastShadow;

		FMeshBatchElement& BatchElement = Mesh.Elements[0];
		BatchElement.IndexBuffer = &IndexBuffer;
		BatchElement.FirstIndex = 0;
		BatchElement.MinVertexIndex = 0;
		BatchElement.MaxVertexIndex = MaxVertexIndex;
		if (IndirectArgsBuffer)
		{
			BatchElement.IndirectArgsBuffer = IndirectArgsBuffer;
			BatchElement.IndirectArgsOffset = IndirectArgsOffset;
			BatchElement.NumPrimitives = 0; // 0 => use IndirectArgsBuffer
		}
		else
		{
			BatchElement.NumPrimitives = NumPrimitives;
		}

		FDynamicPrimitiveUniformBuffer& DynamicPrimitiveUniformBuffer =
			Collector.AllocateOneFrameResource<FDynamicPrimitiveUniformBuffer>();
		DynamicPrimitiveUniformBuffer.Set(Collector.GetRHICommandList(), SceneProxy.GetLocalToWorld(), SceneProxy.GetLocalToWorld(),
			SceneProxy.GetBounds(), SceneProxy.GetLocalBounds(), SceneProxy.GetLocalBounds(), SceneProxy.ReceivesDecals(), false, false, nullptr);
		BatchElement.PrimitiveUniformBufferResource = &DynamicPrimitiveUniformBuffer.UniformBuffer;

		Collector.AddMesh(ViewIndex, Mesh);
	}
}

FPrimitiveViewRelevance FCSGpuMeshSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View);
	Result.bShadowRelevance = IsShadowCast(View);
	Result.bDynamicRelevance = true;
	Result.bStaticRelevance = false;
	Result.bRenderInMainPass = ShouldRenderInMainPass();
	Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
	Result.bRenderCustomDepth = ShouldRenderCustomDepth();
	Result.bVelocityRelevance = false;
	MaterialRelevance.SetPrimitiveViewRelevance(Result);
	return Result;
}

// -----------------------------------------------------------------------------
// Descriptor-driven buffer set
// -----------------------------------------------------------------------------

void FCSGpuMeshSceneProxy::AddStream(const FCSGpuStreamDesc& Desc)
{
	TUniquePtr<FCSGpuStreamRuntime> Runtime = MakeUnique<FCSGpuStreamRuntime>();
	Runtime->Desc = Desc;
	Streams.Add(MoveTemp(Runtime));
}

void FCSGpuMeshSceneProxy::AddStandardTriangleStreams(uint32 NumIndirectDraws)
{
	// The descriptor list itself lives in CSGpuMeshStreams so the retained UCSMesh set and
	// this proxy-owned set cannot drift apart.
	CSGpuMeshStreams::FStandardStreamOptions Options;
	Options.NumIndirectDraws = NumIndirectDraws;

	TArray<FCSGpuStreamDesc> Descs;
	CSGpuMeshStreams::BuildStandardTriangleStreamDescs(Descs, Options);
	for (const FCSGpuStreamDesc& D : Descs) AddStream(D);
}

const FCSGpuMeshSceneProxy::FCSGpuStreamRuntime* FCSGpuMeshSceneProxy::FindStream(ECSGpuStreamRole Role, uint8 Index) const
{
	for (const TUniquePtr<FCSGpuStreamRuntime>& S : Streams)
		if (S->Desc.Role == Role && S->Desc.TexCoordIndex == Index) return S.Get();
	return nullptr;
}

TRefCountPtr<FRDGPooledBuffer> FCSGpuMeshSceneProxy::GetStreamBuffer(ECSGpuStreamRole Role, uint8 Index) const
{
	const FCSGpuStreamRuntime* S = FindStream(Role, Index);
	return S ? S->Pooled : TRefCountPtr<FRDGPooledBuffer>();
}

FRHIShaderResourceView* FCSGpuMeshSceneProxy::GetStreamSRV(ECSGpuStreamRole Role, uint8 Index) const
{
	const FCSGpuStreamRuntime* S = FindStream(Role, Index);
	return S ? S->SRV.GetReference() : nullptr;
}

TUniquePtr<FLocalVertexFactory> FCSGpuMeshSceneProxy::CreateVertexFactory(ERHIFeatureLevel::Type InFeatureLevel, const char* InDebugName) const
{
	return MakeUnique<FLocalVertexFactory>(InFeatureLevel, InDebugName);
}

void FCSGpuMeshSceneProxy::BuildResidentView(FCSMeshResident& OutResident) const
{
	OutResident.Streams.Reset(Streams.Num());
	for (const TUniquePtr<FCSGpuStreamRuntime>& S : Streams)
	{
		FCSMeshResident::FStream& Stream = OutResident.Streams.AddDefaulted_GetRef();
		Stream.Desc = S->Desc;
		Stream.Pooled = S->Pooled;
	}
	OutResident.VertexCapacity = VertexCapacity;
	OutResident.IndexCapacity = IndexCapacity;
}

void FCSGpuMeshSceneProxy::SetExternalStreams(TSharedPtr<FCSMeshResident, ESPMode::ThreadSafe> InResident)
{
	ExternalResident = MoveTemp(InResident);
}

void FCSGpuMeshSceneProxy::InitGpuGeometry(FRHICommandListBase& RHICmdList)
{
	Streams.Reset();
	VertexCapacity = 0;
	IndexCapacity = 0;
	DrawDesc = FDrawDesc();

	if (ExternalResident.IsValid())
	{
		// Adopt: the buffers already exist and already hold geometry, so there is nothing to
		// allocate and nothing to generate. This is what turns a render-state recreation from
		// "re-run the generation compute" into "rebind the vertex factory".
		for (const FCSMeshResident::FStream& Stream : ExternalResident->Streams)
		{
			AddStream(Stream.Desc);
			Streams.Last()->Pooled = Stream.Pooled;
		}
		VertexCapacity = ExternalResident->VertexCapacity;
		IndexCapacity = ExternalResident->IndexCapacity;
		AllocateStreamsAndBindVF(RHICmdList, /*bAllocateBuffers*/ false);
		return;
	}

	RegisterStreams();                    // leaf: push descriptors + set capacities
	AllocateStreamsAndBindVF(RHICmdList); // base: alloc pooled buffers + SRVs + VF + DrawDesc handles
	BuildGeometry(RHICmdList);            // leaf: run compute into the base-owned buffers
}

void FCSGpuMeshSceneProxy::AllocateStreamsAndBindVF(FRHICommandListBase& RHICmdList, bool bAllocateBuffers)
{
	const uint32 VertUnits = FMath::Max(VertexCapacity, 1u);
	const uint32 IdxUnits = FMath::Max(IndexCapacity, 1u);

	// Leaf-selected vertex factory; created here (not in the ctor) so the virtual dispatch works.
	if (!VertexFactory) VertexFactory = CreateVertexFactory(GetScene().GetFeatureLevel(), VertexFactoryDebugName);

	FLocalVertexFactory::FDataType Data;

	for (TUniquePtr<FCSGpuStreamRuntime>& SPtr : Streams)
	{
		FCSGpuStreamRuntime& S = *SPtr;
		const FCSGpuStreamDesc& D = S.Desc;

		// --- allocate the pooled buffer (adopt mode arrives with one already attached)
		if (bAllocateBuffers)
		{
			FRDGBufferDesc Desc;
			if (D.Role == ECSGpuStreamRole::IndirectArgs)
			{
				Desc = FRDGBufferDesc::CreateIndirectDesc(D.BytesPerElement, D.ElementsPerUnit);
			}
			else
			{
				const uint32 Units = FMath::Max(CSGpuMeshStreams::UnitsForCountSource(D.CountSource, VertUnits, IdxUnits), 1u);
				Desc = FRDGBufferDesc::CreateBufferDesc(D.BytesPerElement, D.ElementsPerUnit * Units);
				if (D.Role == ECSGpuStreamRole::Index)
					Desc.Usage = (Desc.Usage & ~EBufferUsageFlags::VertexBuffer) | EBufferUsageFlags::IndexBuffer;
			}
			S.Pooled = AllocatePooledBuffer(Desc, D.DebugName);
		}
		if (!S.Pooled.IsValid()) continue;

		// --- manual-fetch SRV
		if (D.SrvFormat != PF_Unknown)
			S.SRV = RHICmdList.CreateShaderResourceView(S.Pooled->GetRHI(),
				FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(D.SrvFormat));

		// --- render-resource wrappers for the VF streams / index buffer
		if (D.Role == ECSGpuStreamRole::Index)
		{
			S.IB.Pooled = S.Pooled;
			S.IB.InitResource(RHICmdList);
		}
		else if (D.VfType != VET_None)
		{
			S.VB.Pooled = S.Pooled;
			S.VB.InitResource(RHICmdList);
		}

		// --- vertex-factory binding
		const uint32 VertexStride = D.BytesPerElement * D.ElementsPerUnit;
		switch (D.Role)
		{
		case ECSGpuStreamRole::Position:
			Data.PositionComponent = FVertexStreamComponent(&S.VB, 0, VertexStride, D.VfType);
			Data.PositionComponentSRV = S.SRV;
			break;
		case ECSGpuStreamRole::TangentBasis:
			Data.TangentBasisComponents[0] = FVertexStreamComponent(&S.VB, 0, VertexStride, VET_PackedNormal);
			Data.TangentBasisComponents[1] = FVertexStreamComponent(&S.VB, sizeof(uint32), VertexStride, VET_PackedNormal);
			Data.TangentsSRV = S.SRV;
			break;
		case ECSGpuStreamRole::TexCoord:
		{
			Data.TextureCoordinates.Add(FVertexStreamComponent(&S.VB, 0, VertexStride, VET_Float2));
			Data.TextureCoordinatesSRV = S.SRV;
			const int32 DesiredTexCoords = int32(D.TexCoordIndex) + 1;
			if (int32(Data.NumTexCoords) < DesiredTexCoords) Data.NumTexCoords = DesiredTexCoords;
			if (D.TexCoordIndex == 0)
			{
				Data.LightMapCoordinateIndex = 0;
				Data.LightMapCoordinateComponent = FVertexStreamComponent(&S.VB, 0, VertexStride, VET_Float2);
			}
			break;
		}
		case ECSGpuStreamRole::Color:
			Data.ColorComponent = FVertexStreamComponent(&S.VB, 0, VertexStride, VET_Color);
			Data.ColorComponentsSRV = S.SRV;
			Data.ColorIndexMask = ~0u;
			break;
		case ECSGpuStreamRole::Index:
			DrawDesc.IndexBuffer = &S.IB;
			break;
		case ECSGpuStreamRole::IndirectArgs:
			DrawDesc.IndirectArgsBuffer = S.Pooled->GetRHI();
			DrawDesc.IndirectArgsOffset = 0;
			break;
		default:
			break; // MeshCounters, AuxVertex: no VF / draw binding
		}
	}

	OnStreamsAllocated(RHICmdList);

	VertexFactory->SetData(RHICmdList, Data);
	VertexFactory->InitResource(RHICmdList);

	DrawDesc.FirstIndex = 0;
	DrawDesc.MinVertexIndex = 0;
	DrawDesc.MaxVertexIndex = VertUnits - 1;
	DrawDesc.bValid = (DrawDesc.IndexBuffer != nullptr);
}

void FCSGpuMeshSceneProxy::ReleaseGpuGeometry()
{
	DrawDesc = FDrawDesc();
	for (TUniquePtr<FCSGpuStreamRuntime>& SPtr : Streams)
	{
		FCSGpuStreamRuntime& S = *SPtr;
		// Release exactly the wrappers that were initialised in AllocateStreamsAndBindVF.
		if (S.Desc.Role == ECSGpuStreamRole::Index) S.IB.ReleaseResource();
		else if (S.Desc.VfType != VET_None) S.VB.ReleaseResource();
		S.SRV.SafeRelease();
		S.Pooled.SafeRelease();
	}
	Streams.Reset();
}
