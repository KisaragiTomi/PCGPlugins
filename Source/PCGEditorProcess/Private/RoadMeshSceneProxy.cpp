#include "RoadMeshSceneProxy.h"
#include "RoadMeshComponent.h"
#include "RoadBuilderShaders.h"

#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHICommandList.h"
#include "SceneInterface.h"

FRoadMeshSceneProxy::FRoadMeshSceneProxy(URoadMeshComponent* Component, const FRoadBuildInput& InInput)
	: FCSGpuMeshSceneProxy(Component, Component->RoadMaterial, "FRoadMeshSceneProxy")
	, Input(InInput)
{
}

FRoadMeshSceneProxy::~FRoadMeshSceneProxy()
{
}

SIZE_T FRoadMeshSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

void FRoadMeshSceneProxy::RegisterStreams()
{
	VertexCapacity = FMath::Max(Input.MaxVertices, 64u);
	IndexCapacity = FMath::Max(Input.MaxIndices, 192u); // indexed mesh: independent index capacity
	AddStandardTriangleStreams();
}

void FRoadMeshSceneProxy::BuildGeometry(FRHICommandListBase& RHICmdList)
{
	if (Input.Splines.Num() == 0) return;

	FRHICommandListImmediate& RHICmdListImmediate = FRHICommandListExecutor::GetImmediateCommandList();
	FRDGBuilder GraphBuilder(RHICmdListImmediate, RDG_EVENT_NAME("RoadGenerator.Build"));

	// The compute pipeline is shared with the CS-landscape road heightmap path.
	FRoadGeometryBuffers Out;
	Out.Positions = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::Position));
	Out.Tangents = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::TangentBasis));
	Out.TexCoords = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::TexCoord));
	Out.Colors = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::Color));
	Out.Indices = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::Index));
	Out.IndirectArgs = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::IndirectArgs));
	Out.MeshCounters = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::MeshCounters));

	BuildRoadGeometryRDG(GraphBuilder, GetScene().GetFeatureLevel(), Input, Out);

	// Leave the persistent buffers in the states the draw / readback paths need; RDG's
	// default epilogue state (SRVMask) is illegal for index / indirect usage.
	GraphBuilder.SetBufferAccessFinal(Out.Positions, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
	GraphBuilder.SetBufferAccessFinal(Out.Tangents, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
	GraphBuilder.SetBufferAccessFinal(Out.TexCoords, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
	GraphBuilder.SetBufferAccessFinal(Out.Colors, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
	GraphBuilder.SetBufferAccessFinal(Out.Indices, ERHIAccess::VertexOrIndexBuffer);
	GraphBuilder.SetBufferAccessFinal(Out.IndirectArgs, ERHIAccess::IndirectArgs);
	GraphBuilder.SetBufferAccessFinal(Out.MeshCounters, ERHIAccess::CopySrc);

	GraphBuilder.Execute();
}
