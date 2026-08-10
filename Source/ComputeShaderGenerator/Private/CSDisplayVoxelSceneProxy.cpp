#include "CSDisplayVoxelSceneProxy.h"
#include "CSDisplayComponent.h"
#include "CSGpuDebugDraw.h"

#include "RenderGraphBuilder.h"
#include "RHICommandList.h"
#include "SceneInterface.h"

FCSDisplayVoxelSceneProxy::FCSDisplayVoxelSceneProxy(
	const UCSDisplayComponent* Component,
	const FCSDisplayVoxelData& InData)
	: FPrimitiveSceneProxy(Component)
	, Data(InData)
	, VertexFactory(GetScene().GetFeatureLevel(), "FCSMeshGeneratorDebugVertexFactory")
{
	MaterialRelevance = FCSGpuDebugDraw::GetDebugMaterial()->GetRelevance_Concurrent(GetScene().GetShaderPlatform());
	bVerifyUsedMaterials = false;
	bSupportsDistanceFieldRepresentation = false;
}

FCSDisplayVoxelSceneProxy::~FCSDisplayVoxelSceneProxy()
{
}

SIZE_T FCSDisplayVoxelSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

uint32 FCSDisplayVoxelSceneProxy::GetMemoryFootprint() const
{
	return sizeof(*this) + GetAllocatedSize();
}

void FCSDisplayVoxelSceneProxy::CreateRenderThreadResources(FRHICommandListBase& RHICmdList)
{
	FPrimitiveSceneProxy::CreateRenderThreadResources(RHICmdList);

	if (IsDirectionsMode()) AllocateDirectionBuffers(RHICmdList);
	else AllocateQuadBuffers(RHICmdList);

	FCSGpuDebugDraw::BindPositionOnlyVertexFactory(RHICmdList, VertexFactory, Positions);
	BuildGeometry(RHICmdList);
}

void FCSDisplayVoxelSceneProxy::AllocateDirectionBuffers(FRHICommandListBase& RHICmdList)
{
	const uint32 VoxelLimit = uint32(FMath::Max(Data.MaxVoxelsToDraw, 1));
	PositionCapacity = VoxelLimit * 2u;
	FCSGpuDebugDraw::AllocatePositionStream(RHICmdList, Positions, PositionCapacity, TEXT("CSMeshGeneratorDebug.Positions"));
	FCSGpuDebugDraw::AllocateIndexBuffer(RHICmdList, MainIndices, VoxelLimit * 2u, TEXT("CSMeshGeneratorDebug.LineIndices"));
	FCSGpuDebugDraw::AllocateIndexBuffer(RHICmdList, PointIndices, VoxelLimit, TEXT("CSMeshGeneratorDebug.PointIndices"));
	MainIndirectArgs = FCSGpuDebugDraw::AllocateIndirectArgs(TEXT("CSMeshGeneratorDebug.LineArgs"));
	PointIndirectArgs = FCSGpuDebugDraw::AllocateIndirectArgs(TEXT("CSMeshGeneratorDebug.PointArgs"));
}

void FCSDisplayVoxelSceneProxy::AllocateQuadBuffers(FRHICommandListBase& RHICmdList)
{
	const uint32 VoxelLimit = uint32(FMath::Max(Data.MaxVoxelsToDraw, 1));
	PositionCapacity = VoxelLimit * 4u;
	FCSGpuDebugDraw::AllocatePositionStream(RHICmdList, Positions, PositionCapacity, TEXT("CSMeshGeneratorDebug.Positions"));
	FCSGpuDebugDraw::AllocateIndexBuffer(RHICmdList, MainIndices, VoxelLimit * 6u, TEXT("CSMeshGeneratorDebug.QuadIndices"));
	MainIndirectArgs = FCSGpuDebugDraw::AllocateIndirectArgs(TEXT("CSMeshGeneratorDebug.QuadArgs"));
}

void FCSDisplayVoxelSceneProxy::DestroyRenderThreadResources()
{
	VertexFactory.ReleaseResource();
	if (PointIndices.Pooled.IsValid()) FCSGpuDebugDraw::ReleaseIndexBuffer(PointIndices);
	FCSGpuDebugDraw::ReleaseIndexBuffer(MainIndices);
	FCSGpuDebugDraw::ReleasePositionStream(Positions);
	PointIndirectArgs.SafeRelease();
	MainIndirectArgs.SafeRelease();
	FPrimitiveSceneProxy::DestroyRenderThreadResources();
}

void FCSDisplayVoxelSceneProxy::GetDynamicMeshElements(
	const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily,
	uint32 VisibilityMap,
	FMeshElementCollector& Collector) const
{
	if (!Positions.Buffer.Pooled.IsValid() || !MainIndices.Pooled.IsValid() || !MainIndirectArgs.IsValid()) return;

	if (IsDirectionsMode()) SubmitDirectionDraws(Views, VisibilityMap, Collector);
	else SubmitQuadDraws(Views, VisibilityMap, Collector);
}

void FCSDisplayVoxelSceneProxy::SubmitDirectionDraws(const TArray<const FSceneView*>& Views,
	uint32 VisibilityMap, FMeshElementCollector& Collector) const
{
	FCSGpuDebugDraw::SubmitColoredDraw(*this, Views, VisibilityMap, Collector, VertexFactory,
		MainIndices, PT_LineList, Data.DirectionColor, 0u, PositionCapacity - 1u, MainIndirectArgs->GetRHI(), 0u);

	if (!Data.bDrawPoints || !PointIndirectArgs.IsValid()) return;
	FCSGpuDebugDraw::SubmitColoredDraw(*this, Views, VisibilityMap, Collector, VertexFactory,
		PointIndices, PT_PointList, Data.PointColor, 0u, PositionCapacity - 1u, PointIndirectArgs->GetRHI(), 0u);
}

void FCSDisplayVoxelSceneProxy::SubmitQuadDraws(const TArray<const FSceneView*>& Views,
	uint32 VisibilityMap, FMeshElementCollector& Collector) const
{
	FCSGpuDebugDraw::SubmitColoredDraw(*this, Views, VisibilityMap, Collector, VertexFactory,
		MainIndices, PT_TriangleList, Data.DirectionColor, 0u, PositionCapacity - 1u, MainIndirectArgs->GetRHI(), 0u);
}

FPrimitiveViewRelevance FCSDisplayVoxelSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View);
	Result.bDynamicRelevance = true;
	Result.bStaticRelevance = false;
	Result.bRenderInMainPass = ShouldRenderInMainPass();
	Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
	Result.bRenderCustomDepth = ShouldRenderCustomDepth();
	Result.bVelocityRelevance = false;
	MaterialRelevance.SetPrimitiveViewRelevance(Result);
	return Result;
}

bool FCSDisplayVoxelSceneProxy::CanBeOccluded() const
{
	return !MaterialRelevance.bDisableDepthTest;
}

void FCSDisplayVoxelSceneProxy::BuildGeometry(FRHICommandListBase& RHICmdList)
{
	FRHICommandListImmediate& Immediate = FRHICommandListExecutor::GetImmediateCommandList();
	FRDGBuilder GraphBuilder(Immediate, RDG_EVENT_NAME("CSMeshGeneratorDebug.Build"));
	const ERHIFeatureLevel::Type FeatureLevel = GetScene().GetFeatureLevel();

	FCSGpuDebugVoxelSource Source;
	Source.Positions = GraphBuilder.RegisterExternalBuffer(Data.Positions, TEXT("CSMeshGeneratorDebug.SourcePositions"));
	Source.Normals = GraphBuilder.RegisterExternalBuffer(Data.Normals, TEXT("CSMeshGeneratorDebug.SourceNormals"));
	Source.Counter = GraphBuilder.RegisterExternalBuffer(Data.Counter, TEXT("CSMeshGeneratorDebug.SourceCounter"));
	Source.Capacity = uint32(Data.VoxelCapacity);
	Source.MaxItems = uint32(Data.MaxVoxelsToDraw);

	FRDGBufferRef DebugPositions = GraphBuilder.RegisterExternalBuffer(Positions.Buffer.Pooled, TEXT("CSMeshGeneratorDebug.Positions.External"));
	FRDGBufferRef DebugIndices = GraphBuilder.RegisterExternalBuffer(MainIndices.Pooled, TEXT("CSMeshGeneratorDebug.MainIndices.External"));
	FRDGBufferRef DebugArgs = GraphBuilder.RegisterExternalBuffer(MainIndirectArgs, TEXT("CSMeshGeneratorDebug.MainArgs.External"));

	if (IsDirectionsMode()) BuildDirectionGeometry(GraphBuilder, FeatureLevel, Source, DebugPositions, DebugIndices, DebugArgs);
	else BuildQuadGeometry(GraphBuilder, FeatureLevel, Source, DebugPositions, DebugIndices, DebugArgs);

	GraphBuilder.SetBufferAccessFinal(DebugPositions, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
	GraphBuilder.SetBufferAccessFinal(DebugIndices, ERHIAccess::VertexOrIndexBuffer);
	GraphBuilder.SetBufferAccessFinal(DebugArgs, ERHIAccess::IndirectArgs);
	GraphBuilder.Execute();
}

void FCSDisplayVoxelSceneProxy::BuildDirectionGeometry(FRDGBuilder& GraphBuilder, ERHIFeatureLevel::Type FeatureLevel,
	const FCSGpuDebugVoxelSource& Source, FRDGBufferRef DebugPositions, FRDGBufferRef DebugIndices, FRDGBufferRef DebugArgs)
{
	FRDGBufferRef DebugPointIndices = GraphBuilder.RegisterExternalBuffer(PointIndices.Pooled, TEXT("CSMeshGeneratorDebug.PointIndices.External"));
	FRDGBufferRef DebugPointArgs = GraphBuilder.RegisterExternalBuffer(PointIndirectArgs, TEXT("CSMeshGeneratorDebug.PointArgs.External"));

	FCSGpuDebugDraw::AddVoxelDirectionsPass(GraphBuilder, FeatureLevel, Source, Data.DirectionLength,
		DebugPositions, DebugIndices, DebugPointIndices);
	FCSGpuDebugDraw::AddIndirectArgsPass(GraphBuilder, FeatureLevel, Source.Counter, Source.Capacity, Source.MaxItems,
		2u, DebugArgs);
	FCSGpuDebugDraw::AddIndirectArgsPass(GraphBuilder, FeatureLevel, Source.Counter, Source.Capacity, Source.MaxItems,
		1u, DebugPointArgs);

	GraphBuilder.SetBufferAccessFinal(DebugPointIndices, ERHIAccess::VertexOrIndexBuffer);
	GraphBuilder.SetBufferAccessFinal(DebugPointArgs, ERHIAccess::IndirectArgs);
}

void FCSDisplayVoxelSceneProxy::BuildQuadGeometry(FRDGBuilder& GraphBuilder, ERHIFeatureLevel::Type FeatureLevel,
	const FCSGpuDebugVoxelSource& Source, FRDGBufferRef DebugPositions, FRDGBufferRef DebugIndices, FRDGBufferRef DebugArgs)
{
	FCSGpuDebugDraw::AddVoxelQuadsPass(GraphBuilder, FeatureLevel, Source, Data.VoxelSize, Data.QuadScale,
		Data.NormalOffsetScale, Data.bReverseOrientation, DebugPositions, DebugIndices);
	FCSGpuDebugDraw::AddIndirectArgsPass(GraphBuilder, FeatureLevel, Source.Counter, Source.Capacity, Source.MaxItems,
		6u, DebugArgs);
}
