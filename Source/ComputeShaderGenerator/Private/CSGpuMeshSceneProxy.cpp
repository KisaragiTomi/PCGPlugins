#include "CSGpuMeshSceneProxy.h"

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
	, VertexFactory(GetScene().GetFeatureLevel(), DebugName)
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
	VertexFactory.ReleaseResource();
	ReleaseGpuGeometry();
	FPrimitiveSceneProxy::DestroyRenderThreadResources();
}

void FCSGpuMeshSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const
{
	if (!DrawDesc.bValid || DrawDesc.IndexBuffer == nullptr) return;

	FMaterialRenderProxy* MaterialProxy = Material->GetRenderProxy();
	SubmitGpuBufferDraw(*this, Views, VisibilityMap, Collector, VertexFactory, *MaterialProxy,
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

void FCSGpuMeshSceneProxy::AddStandardTriangleStreams()
{
	{
		FCSGpuStreamDesc D;
		D.DebugName = TEXT("CSGpuMesh.Positions");
		D.Role = ECSGpuStreamRole::Position;
		D.BytesPerElement = sizeof(float);
		D.ElementsPerUnit = 3;
		D.CountSource = ECSGpuCountSource::PerVertex;
		D.SrvFormat = PF_R32_FLOAT;
		D.VfType = VET_Float3;
		D.CpuSemantic = ECSGpuMeshSemantic::Position;
		D.bReadback = true;
		AddStream(D);
	}
	{
		FCSGpuStreamDesc D;
		D.DebugName = TEXT("CSGpuMesh.Tangents");
		D.Role = ECSGpuStreamRole::TangentBasis;
		D.BytesPerElement = sizeof(uint32);
		D.ElementsPerUnit = 2;
		D.CountSource = ECSGpuCountSource::PerVertex;
		D.SrvFormat = PF_R8G8B8A8_SNORM;
		D.VfType = VET_PackedNormal;
		D.CpuSemantic = ECSGpuMeshSemantic::TangentBasis;
		D.bReadback = true;
		AddStream(D);
	}
	{
		FCSGpuStreamDesc D;
		D.DebugName = TEXT("CSGpuMesh.TexCoords");
		D.Role = ECSGpuStreamRole::TexCoord;
		D.BytesPerElement = sizeof(float);
		D.ElementsPerUnit = 2;
		D.CountSource = ECSGpuCountSource::PerVertex;
		D.SrvFormat = PF_G32R32F;
		D.VfType = VET_Float2;
		D.TexCoordIndex = 0;
		D.CpuSemantic = ECSGpuMeshSemantic::TexCoord;
		D.bReadback = true;
		AddStream(D);
	}
	{
		FCSGpuStreamDesc D;
		D.DebugName = TEXT("CSGpuMesh.Colors");
		D.Role = ECSGpuStreamRole::Color;
		D.BytesPerElement = sizeof(uint32);
		D.ElementsPerUnit = 1;
		D.CountSource = ECSGpuCountSource::PerVertex;
		D.SrvFormat = PF_R8G8B8A8;
		D.VfType = VET_Color;
		D.CpuSemantic = ECSGpuMeshSemantic::None; // vertex colours are not part of the saved mesh today
		D.bReadback = false;
		AddStream(D);
	}
	{
		FCSGpuStreamDesc D;
		D.DebugName = TEXT("CSGpuMesh.Indices");
		D.Role = ECSGpuStreamRole::Index;
		D.BytesPerElement = sizeof(uint32);
		D.ElementsPerUnit = 1;
		D.CountSource = ECSGpuCountSource::PerIndex;
		D.SrvFormat = PF_Unknown;
		D.VfType = VET_None;
		D.CpuSemantic = ECSGpuMeshSemantic::Index;
		D.bReadback = true;
		AddStream(D);
	}
	{
		FCSGpuStreamDesc D;
		D.DebugName = TEXT("CSGpuMesh.IndirectArgs");
		D.Role = ECSGpuStreamRole::IndirectArgs;
		D.BytesPerElement = sizeof(uint32);
		D.ElementsPerUnit = 5;
		D.CountSource = ECSGpuCountSource::Fixed;
		D.SrvFormat = PF_Unknown;
		D.VfType = VET_None;
		D.CpuSemantic = ECSGpuMeshSemantic::None;
		D.bReadback = false;
		AddStream(D);
	}
	{
		FCSGpuStreamDesc D;
		D.DebugName = TEXT("CSGpuMesh.MeshCounters");
		D.Role = ECSGpuStreamRole::MeshCounters;
		D.BytesPerElement = sizeof(uint32);
		D.ElementsPerUnit = 2; // [0]=vertexCount, [1]=indexCount
		D.CountSource = ECSGpuCountSource::Fixed;
		D.SrvFormat = PF_Unknown;
		D.VfType = VET_None;
		D.CpuSemantic = ECSGpuMeshSemantic::None; // read via EnqueueCountersReadback, not the mesh loop
		D.bReadback = false;
		AddStream(D);
	}
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

void FCSGpuMeshSceneProxy::GetMeshReadbackDescs(TArray<FCSGpuStreamDesc>& OutDescs) const
{
	OutDescs.Reset();
	for (const TUniquePtr<FCSGpuStreamRuntime>& S : Streams)
		if (S->Desc.bReadback && S->Desc.CpuSemantic != ECSGpuMeshSemantic::None)
			OutDescs.Add(S->Desc);
}

void FCSGpuMeshSceneProxy::InitGpuGeometry(FRHICommandListBase& RHICmdList)
{
	Streams.Reset();
	VertexCapacity = 0;
	IndexCapacity = 0;
	DrawDesc = FDrawDesc();

	RegisterStreams();                    // leaf: push descriptors + set capacities
	AllocateStreamsAndBindVF(RHICmdList); // base: alloc pooled buffers + SRVs + VF + DrawDesc handles
	BuildGeometry(RHICmdList);            // leaf: run compute into the base-owned buffers
}

void FCSGpuMeshSceneProxy::AllocateStreamsAndBindVF(FRHICommandListBase& RHICmdList)
{
	const uint32 VertUnits = FMath::Max(VertexCapacity, 1u);
	const uint32 IdxUnits = FMath::Max(IndexCapacity, 1u);

	FLocalVertexFactory::FDataType Data;

	for (TUniquePtr<FCSGpuStreamRuntime>& SPtr : Streams)
	{
		FCSGpuStreamRuntime& S = *SPtr;
		const FCSGpuStreamDesc& D = S.Desc;

		// --- allocate the pooled buffer
		FRDGBufferDesc Desc;
		if (D.Role == ECSGpuStreamRole::IndirectArgs)
		{
			Desc = FRDGBufferDesc::CreateIndirectDesc(D.BytesPerElement, D.ElementsPerUnit);
		}
		else
		{
			const uint32 Units = (D.CountSource == ECSGpuCountSource::PerVertex) ? VertUnits
				: (D.CountSource == ECSGpuCountSource::PerIndex) ? IdxUnits : 1u;
			Desc = FRDGBufferDesc::CreateBufferDesc(D.BytesPerElement, D.ElementsPerUnit * Units);
			if (D.Role == ECSGpuStreamRole::Index)
				Desc.Usage = (Desc.Usage & ~EBufferUsageFlags::VertexBuffer) | EBufferUsageFlags::IndexBuffer;
		}
		S.Pooled = AllocatePooledBuffer(Desc, D.DebugName);

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

	VertexFactory.SetData(RHICmdList, Data);
	VertexFactory.InitResource(RHICmdList);

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

bool FCSGpuMeshSceneProxy::EnqueueCountersReadback(FRHICommandListImmediate& RHICmdList, FRHIGPUBufferReadback* Readback) const
{
	if (!Readback || !DrawDesc.bValid) return false;
	const FCSGpuStreamRuntime* Counters = FindStream(ECSGpuStreamRole::MeshCounters);
	if (!Counters || !Counters->Pooled.IsValid()) return false;

	FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSGpuMesh.ReadbackCounters"));
	FRDGBufferRef CountersRDG = GraphBuilder.RegisterExternalBuffer(Counters->Pooled);
	AddEnqueueCopyPass(GraphBuilder, Readback, CountersRDG, sizeof(uint32) * 2u);
	GraphBuilder.SetBufferAccessFinal(CountersRDG, ERHIAccess::CopySrc);
	GraphBuilder.Execute();
	return true;
}

bool FCSGpuMeshSceneProxy::EnqueueMeshReadback(FRHICommandListImmediate& RHICmdList, uint32 VertexCount, uint32 IndexCount,
	const TArray<FRHIGPUBufferReadback*>& Readbacks) const
{
	if (!DrawDesc.bValid) return false;
	if (VertexCount == 0 || VertexCount > FMath::Max(VertexCapacity, 1u)) return false;
	if (IndexCount == 0 || IndexCount > FMath::Max(IndexCapacity, 1u)) return false;

	// Gather the mesh-readback streams in registration order (matches GetMeshReadbackDescs).
	TArray<const FCSGpuStreamRuntime*, TInlineAllocator<8>> ReadStreams;
	for (const TUniquePtr<FCSGpuStreamRuntime>& S : Streams)
		if (S->Desc.bReadback && S->Desc.CpuSemantic != ECSGpuMeshSemantic::None)
			ReadStreams.Add(S.Get());

	if (ReadStreams.Num() != Readbacks.Num()) return false;
	for (const FCSGpuStreamRuntime* S : ReadStreams)
		if (!S->Pooled.IsValid()) return false;
	for (FRHIGPUBufferReadback* RB : Readbacks)
		if (!RB) return false;

	FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSGpuMesh.ReadbackData"));
	for (int32 i = 0; i < ReadStreams.Num(); ++i)
	{
		const FCSGpuStreamRuntime& S = *ReadStreams[i];
		const uint32 Units = (S.Desc.CountSource == ECSGpuCountSource::PerIndex) ? IndexCount : VertexCount;
		const uint32 Bytes = Units * S.Desc.ElementsPerUnit * S.Desc.BytesPerElement;
		FRDGBufferRef BufRDG = GraphBuilder.RegisterExternalBuffer(S.Pooled);
		AddEnqueueCopyPass(GraphBuilder, Readbacks[i], BufRDG, Bytes);
		const ERHIAccess Final = (S.Desc.Role == ECSGpuStreamRole::Index)
			? ERHIAccess::VertexOrIndexBuffer
			: (ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
		GraphBuilder.SetBufferAccessFinal(BufRDG, Final);
	}
	GraphBuilder.Execute();
	return true;
}
