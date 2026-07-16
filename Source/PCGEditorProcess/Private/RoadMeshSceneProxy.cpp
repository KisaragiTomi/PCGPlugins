#include "RoadMeshSceneProxy.h"
#include "RoadMeshComponent.h"
#include "RoadBuilderShaders.h"

#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "MaterialDomain.h"
#include "MaterialShared.h"
#include "MeshBatch.h"
#include "SceneManagement.h"
#include "SceneInterface.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHICommandList.h"
#include "DataDrivenShaderPlatformInfo.h"

FRoadMeshSceneProxy::FRoadMeshSceneProxy(URoadMeshComponent* Component, const FRoadBuildInput& InInput)
	: FPrimitiveSceneProxy(Component)
	, Input(InInput)
	, Material(Component->RoadMaterial)
	, VertexFactory(GetScene().GetFeatureLevel(), "FRoadMeshSceneProxy")
{
	if (!Material) Material = UMaterial::GetDefaultMaterial(MD_Surface);
	MaterialRelevance = Material->GetRelevance_Concurrent(GetScene().GetShaderPlatform());

	bVerifyUsedMaterials = false;
	bSupportsDistanceFieldRepresentation = false;
}

FRoadMeshSceneProxy::~FRoadMeshSceneProxy()
{
}

SIZE_T FRoadMeshSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

uint32 FRoadMeshSceneProxy::GetMemoryFootprint() const
{
	return sizeof(*this) + GetAllocatedSize();
}

bool FRoadMeshSceneProxy::CanBeOccluded() const
{
	return !MaterialRelevance.bDisableDepthTest;
}

void FRoadMeshSceneProxy::CreateRenderThreadResources(FRHICommandListBase& RHICmdList)
{
	FPrimitiveSceneProxy::CreateRenderThreadResources(RHICmdList);

	const uint32 MaxVertices = FMath::Max(Input.MaxVertices, 64u);
	const uint32 MaxIndices = FMath::Max(Input.MaxIndices, 192u);

	// --- persistent GPU buffers (written by compute, consumed by the draw)
	{
		FRDGBufferDesc Desc = FRDGBufferDesc::CreateBufferDesc(sizeof(float), MaxVertices * 3);
		PositionPooled = AllocatePooledBuffer(Desc, TEXT("Road.Positions"));
	}
	{
		FRDGBufferDesc Desc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), MaxVertices * 2);
		TangentPooled = AllocatePooledBuffer(Desc, TEXT("Road.Tangents"));
	}
	{
		FRDGBufferDesc Desc = FRDGBufferDesc::CreateBufferDesc(sizeof(float), MaxVertices * 2);
		TexCoordPooled = AllocatePooledBuffer(Desc, TEXT("Road.TexCoords"));
	}
	{
		FRDGBufferDesc Desc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), MaxVertices);
		ColorPooled = AllocatePooledBuffer(Desc, TEXT("Road.Colors"));
	}
	{
		FRDGBufferDesc Desc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), MaxIndices);
		Desc.Usage = (Desc.Usage & ~EBufferUsageFlags::VertexBuffer) | EBufferUsageFlags::IndexBuffer;
		IndexPooled = AllocatePooledBuffer(Desc, TEXT("Road.Indices"));
	}
	{
		FRDGBufferDesc Desc = FRDGBufferDesc::CreateIndirectDesc(sizeof(uint32), 5);
		IndirectArgsPooled = AllocatePooledBuffer(Desc, TEXT("Road.IndirectArgs"));
	}

	// --- SRVs for manual vertex fetch
	PositionSRV = RHICmdList.CreateShaderResourceView(PositionPooled->GetRHI(),
		FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(PF_R32_FLOAT));
	TangentSRV = RHICmdList.CreateShaderResourceView(TangentPooled->GetRHI(),
		FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(PF_R8G8B8A8_SNORM));
	TexCoordSRV = RHICmdList.CreateShaderResourceView(TexCoordPooled->GetRHI(),
		FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(PF_G32R32F));
	ColorSRV = RHICmdList.CreateShaderResourceView(ColorPooled->GetRHI(),
		FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(PF_R8G8B8A8));

	// --- vertex factory streams over the same buffers
	PositionVB.Pooled = PositionPooled;
	TangentVB.Pooled = TangentPooled;
	TexCoordVB.Pooled = TexCoordPooled;
	ColorVB.Pooled = ColorPooled;
	IndexBuf.Pooled = IndexPooled;
	PositionVB.InitResource(RHICmdList);
	TangentVB.InitResource(RHICmdList);
	TexCoordVB.InitResource(RHICmdList);
	ColorVB.InitResource(RHICmdList);
	IndexBuf.InitResource(RHICmdList);

	FLocalVertexFactory::FDataType Data;
	Data.PositionComponent = FVertexStreamComponent(&PositionVB, 0, sizeof(float) * 3, VET_Float3);
	Data.PositionComponentSRV = PositionSRV;
	Data.TangentBasisComponents[0] = FVertexStreamComponent(&TangentVB, 0, sizeof(uint32) * 2, VET_PackedNormal);
	Data.TangentBasisComponents[1] = FVertexStreamComponent(&TangentVB, sizeof(uint32), sizeof(uint32) * 2, VET_PackedNormal);
	Data.TangentsSRV = TangentSRV;
	Data.TextureCoordinates.Add(FVertexStreamComponent(&TexCoordVB, 0, sizeof(float) * 2, VET_Float2));
	Data.TextureCoordinatesSRV = TexCoordSRV;
	Data.NumTexCoords = 1;
	Data.LightMapCoordinateIndex = 0;
	Data.LightMapCoordinateComponent = FVertexStreamComponent(&TexCoordVB, 0, sizeof(float) * 2, VET_Float2);
	Data.ColorComponent = FVertexStreamComponent(&ColorVB, 0, sizeof(uint32), VET_Color);
	Data.ColorComponentsSRV = ColorSRV;
	Data.ColorIndexMask = ~0u;
	VertexFactory.SetData(RHICmdList, Data);
	VertexFactory.InitResource(RHICmdList);

	// --- run the road build now; afterwards only the indirect draw remains
	BuildRoadNetwork(FRHICommandListExecutor::GetImmediateCommandList());
}

void FRoadMeshSceneProxy::DestroyRenderThreadResources()
{
	VertexFactory.ReleaseResource();
	PositionVB.ReleaseResource();
	TangentVB.ReleaseResource();
	TexCoordVB.ReleaseResource();
	ColorVB.ReleaseResource();
	IndexBuf.ReleaseResource();
	PositionSRV.SafeRelease();
	TangentSRV.SafeRelease();
	TexCoordSRV.SafeRelease();
	ColorSRV.SafeRelease();
	PositionPooled.SafeRelease();
	TangentPooled.SafeRelease();
	TexCoordPooled.SafeRelease();
	ColorPooled.SafeRelease();
	IndexPooled.SafeRelease();
	IndirectArgsPooled.SafeRelease();
	FPrimitiveSceneProxy::DestroyRenderThreadResources();
}

void FRoadMeshSceneProxy::BuildRoadNetwork(FRHICommandListImmediate& RHICmdList)
{
	if (Input.Splines.Num() == 0) return;

	FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("RoadGenerator.Build"));

	// The compute pipeline is shared with the CS-landscape road heightmap path.
	FRoadGeometryBuffers Out;
	Out.Positions = GraphBuilder.RegisterExternalBuffer(PositionPooled);
	Out.Tangents = GraphBuilder.RegisterExternalBuffer(TangentPooled);
	Out.TexCoords = GraphBuilder.RegisterExternalBuffer(TexCoordPooled);
	Out.Colors = GraphBuilder.RegisterExternalBuffer(ColorPooled);
	Out.Indices = GraphBuilder.RegisterExternalBuffer(IndexPooled);
	Out.IndirectArgs = GraphBuilder.RegisterExternalBuffer(IndirectArgsPooled);

	BuildRoadGeometryRDG(GraphBuilder, GetScene().GetFeatureLevel(), Input, Out);

	// Leave the persistent buffers in the states the draw path needs; RDG's
	// default epilogue state (SRVMask) is illegal for index / indirect usage.
	GraphBuilder.SetBufferAccessFinal(Out.Positions, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
	GraphBuilder.SetBufferAccessFinal(Out.Tangents, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
	GraphBuilder.SetBufferAccessFinal(Out.TexCoords, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
	GraphBuilder.SetBufferAccessFinal(Out.Colors, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
	GraphBuilder.SetBufferAccessFinal(Out.Indices, ERHIAccess::VertexOrIndexBuffer);
	GraphBuilder.SetBufferAccessFinal(Out.IndirectArgs, ERHIAccess::IndirectArgs);

	GraphBuilder.Execute();
}

void FRoadMeshSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const
{
	if (!IndirectArgsPooled.IsValid()) return;

	FMaterialRenderProxy* MaterialProxy = Material->GetRenderProxy();

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		if ((VisibilityMap & (1 << ViewIndex)) == 0) continue;

		FMeshBatch& Mesh = Collector.AllocateMesh();
		Mesh.VertexFactory = &VertexFactory;
		Mesh.MaterialRenderProxy = MaterialProxy;
		Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
		Mesh.Type = PT_TriangleList;
		Mesh.DepthPriorityGroup = SDPG_World;
		Mesh.bCanApplyViewModeOverrides = false;
		Mesh.CastShadow = true;

		FMeshBatchElement& BatchElement = Mesh.Elements[0];
		BatchElement.IndexBuffer = &IndexBuf;
		BatchElement.IndirectArgsBuffer = IndirectArgsPooled->GetRHI();
		BatchElement.IndirectArgsOffset = 0;
		BatchElement.NumPrimitives = 0; // 0 => use IndirectArgsBuffer
		BatchElement.FirstIndex = 0;
		BatchElement.MinVertexIndex = 0;
		BatchElement.MaxVertexIndex = FMath::Max(Input.MaxVertices, 64u) - 1;

		FDynamicPrimitiveUniformBuffer& DynamicPrimitiveUniformBuffer =
			Collector.AllocateOneFrameResource<FDynamicPrimitiveUniformBuffer>();
		DynamicPrimitiveUniformBuffer.Set(Collector.GetRHICommandList(), GetLocalToWorld(), GetLocalToWorld(),
			GetBounds(), GetLocalBounds(), GetLocalBounds(), ReceivesDecals(), false, false, nullptr);
		BatchElement.PrimitiveUniformBufferResource = &DynamicPrimitiveUniformBuffer.UniformBuffer;

		Collector.AddMesh(ViewIndex, Mesh);
	}
}

FPrimitiveViewRelevance FRoadMeshSceneProxy::GetViewRelevance(const FSceneView* View) const
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
