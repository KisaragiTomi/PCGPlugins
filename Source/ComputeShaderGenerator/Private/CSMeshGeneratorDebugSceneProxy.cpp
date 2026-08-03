#include "CSMeshGeneratorDebugSceneProxy.h"
#include "CSMeshGeneratorDebugComponent.h"
#include "CSGpuDebugDraw.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHICommandList.h"
#include "SceneInterface.h"
#include "ShaderParameterStruct.h"

class FCSMeshGeneratorDebugGeometryCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCSMeshGeneratorDebugGeometryCS);
	SHADER_USE_PARAMETER_STRUCT(FCSMeshGeneratorDebugGeometryCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, InVoxelPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, InVoxelNormals)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RWDebugPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWMainIndices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWPointIndices)
		SHADER_PARAMETER(uint32, VoxelCapacity)
		SHADER_PARAMETER(uint32, MaxVoxelsToDraw)
		SHADER_PARAMETER(uint32, DebugMode)
		SHADER_PARAMETER(uint32, bDrawPoints)
		SHADER_PARAMETER(uint32, bReverseOrientation)
		SHADER_PARAMETER(float, VoxelSize)
		SHADER_PARAMETER(float, DirectionLength)
		SHADER_PARAMETER(float, QuadScale)
		SHADER_PARAMETER(float, NormalOffsetScale)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FCSMeshGeneratorDebugArgsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCSMeshGeneratorDebugArgsCS);
	SHADER_USE_PARAMETER_STRUCT(FCSMeshGeneratorDebugArgsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InVoxelCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWMainIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWPointIndirectArgs)
		SHADER_PARAMETER(uint32, VoxelCapacity)
		SHADER_PARAMETER(uint32, MaxVoxelsToDraw)
		SHADER_PARAMETER(uint32, DebugMode)
		SHADER_PARAMETER(uint32, bDrawPoints)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCSMeshGeneratorDebugGeometryCS,
	"/Plugin/PCGPlugins/Shaders/Private/CSMeshGeneratorDebug.usf", "BuildDebugGeometryCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSMeshGeneratorDebugArgsCS,
	"/Plugin/PCGPlugins/Shaders/Private/CSMeshGeneratorDebug.usf", "BuildDebugIndirectArgsCS", SF_Compute);

FCSMeshGeneratorDebugSceneProxy::FCSMeshGeneratorDebugSceneProxy(
	const UCSMeshGeneratorDebugComponent* Component,
	const FCSMeshGeneratorDebugData& InData)
	: FPrimitiveSceneProxy(Component)
	, Data(InData)
	, VertexFactory(GetScene().GetFeatureLevel(), "FCSMeshGeneratorDebugVertexFactory")
	, BatchVertexFactory(GetScene().GetFeatureLevel(), "FCSMeshGeneratorDebugBatchVertexFactory")
{
	MaterialRelevance = FCSGpuDebugDraw::GetDebugMaterial()->GetRelevance_Concurrent(GetScene().GetShaderPlatform());
	bVerifyUsedMaterials = false;
	bSupportsDistanceFieldRepresentation = false;
}

FCSMeshGeneratorDebugSceneProxy::~FCSMeshGeneratorDebugSceneProxy()
{
}

SIZE_T FCSMeshGeneratorDebugSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

uint32 FCSMeshGeneratorDebugSceneProxy::GetMemoryFootprint() const
{
	return sizeof(*this) + GetAllocatedSize();
}

void FCSMeshGeneratorDebugSceneProxy::CreateRenderThreadResources(FRHICommandListBase& RHICmdList)
{
	FPrimitiveSceneProxy::CreateRenderThreadResources(RHICmdList);

	if (Data.HasVoxelSource())
	{
		const uint32 VoxelLimit = uint32(FMath::Max(Data.MaxVoxelsToDraw, 1));
		PositionCapacity = Data.Mode == ECSMeshGeneratorDebugMode::Directions ? VoxelLimit * 2u : VoxelLimit * 4u;
		MainIndexCapacity = Data.Mode == ECSMeshGeneratorDebugMode::Directions ? VoxelLimit * 2u : VoxelLimit * 6u;
		PointIndexCapacity = Data.Mode == ECSMeshGeneratorDebugMode::Directions && Data.bDrawPoints ? VoxelLimit : 1u;

		FCSGpuDebugDraw::AllocatePositionStream(RHICmdList, Positions, PositionCapacity, TEXT("CSMeshGeneratorDebug.Positions"));
		FCSGpuDebugDraw::AllocateIndexBuffer(RHICmdList, MainIndices, MainIndexCapacity, TEXT("CSMeshGeneratorDebug.MainIndices"));
		FCSGpuDebugDraw::AllocateIndexBuffer(RHICmdList, PointIndices, PointIndexCapacity, TEXT("CSMeshGeneratorDebug.PointIndices"));
		MainIndirectArgs = FCSGpuDebugDraw::AllocateIndirectArgs(TEXT("CSMeshGeneratorDebug.MainIndirectArgs"));
		PointIndirectArgs = FCSGpuDebugDraw::AllocateIndirectArgs(TEXT("CSMeshGeneratorDebug.PointIndirectArgs"));
		FCSGpuDebugDraw::BindPositionOnlyVertexFactory(RHICmdList, VertexFactory, Positions);

		BuildGeometry(RHICmdList);
	}

	if (Data.HasBatchGeometry())
	{
		FCSGpuDebugDraw::UploadGeometry(RHICmdList, BatchPositions, BatchIndices,
			Data.BatchPositions, Data.BatchIndices, TEXT("CSMeshGeneratorDebug.Batches"));
		FCSGpuDebugDraw::BindPositionOnlyVertexFactory(RHICmdList, BatchVertexFactory, BatchPositions);
	}
}

void FCSMeshGeneratorDebugSceneProxy::DestroyRenderThreadResources()
{
	if (Data.HasBatchGeometry())
	{
		BatchVertexFactory.ReleaseResource();
		FCSGpuDebugDraw::ReleaseIndexBuffer(BatchIndices);
		FCSGpuDebugDraw::ReleasePositionStream(BatchPositions);
	}
	if (Data.HasVoxelSource())
	{
		VertexFactory.ReleaseResource();
		FCSGpuDebugDraw::ReleaseIndexBuffer(PointIndices);
		FCSGpuDebugDraw::ReleaseIndexBuffer(MainIndices);
		FCSGpuDebugDraw::ReleasePositionStream(Positions);
		PointIndirectArgs.SafeRelease();
		MainIndirectArgs.SafeRelease();
	}
	FPrimitiveSceneProxy::DestroyRenderThreadResources();
}

void FCSMeshGeneratorDebugSceneProxy::GetDynamicMeshElements(
	const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily,
	uint32 VisibilityMap,
	FMeshElementCollector& Collector) const
{
	// CPU-supplied primitives: one uploaded buffer pair, one draw per batch slice.
	if (BatchPositions.Buffer.Pooled.IsValid() && BatchIndices.Pooled.IsValid())
	{
		const uint32 MaxBatchVertexIndex = uint32(Data.BatchPositions.Num()) - 1u;
		for (const FCSGpuDebugBatchDraw& Draw : Data.BatchDraws)
			FCSGpuDebugDraw::SubmitColoredDraw(*this, Views, VisibilityMap, Collector, BatchVertexFactory,
				BatchIndices, Draw.PrimitiveType, Draw.Color, Draw.NumPrimitives, MaxBatchVertexIndex,
				nullptr, 0u, Draw.FirstIndex);
	}

	if (!Positions.Buffer.Pooled.IsValid() || !MainIndices.Pooled.IsValid() || !MainIndirectArgs.IsValid()) return;
	const EPrimitiveType MainType = Data.Mode == ECSMeshGeneratorDebugMode::Directions ? PT_LineList : PT_TriangleList;
	FCSGpuDebugDraw::SubmitColoredDraw(*this, Views, VisibilityMap, Collector, VertexFactory,
		MainIndices, MainType, Data.DirectionColor, 0u, PositionCapacity - 1u, MainIndirectArgs->GetRHI(), 0u);

	if (Data.Mode != ECSMeshGeneratorDebugMode::Directions || !Data.bDrawPoints || !PointIndirectArgs.IsValid()) return;
	FCSGpuDebugDraw::SubmitColoredDraw(*this, Views, VisibilityMap, Collector, VertexFactory,
		PointIndices, PT_PointList, Data.PointColor, 0u, PositionCapacity - 1u, PointIndirectArgs->GetRHI(), 0u);
}

FPrimitiveViewRelevance FCSMeshGeneratorDebugSceneProxy::GetViewRelevance(const FSceneView* View) const
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

bool FCSMeshGeneratorDebugSceneProxy::CanBeOccluded() const
{
	return !MaterialRelevance.bDisableDepthTest;
}

void FCSMeshGeneratorDebugSceneProxy::BuildGeometry(FRHICommandListBase& RHICmdList)
{
	FRHICommandListImmediate& Immediate = FRHICommandListExecutor::GetImmediateCommandList();
	FRDGBuilder GraphBuilder(Immediate, RDG_EVENT_NAME("CSMeshGeneratorDebug.Build"));

	FRDGBufferRef SourcePositions = GraphBuilder.RegisterExternalBuffer(Data.Positions, TEXT("CSMeshGeneratorDebug.SourcePositions"));
	FRDGBufferRef SourceNormals = GraphBuilder.RegisterExternalBuffer(Data.Normals, TEXT("CSMeshGeneratorDebug.SourceNormals"));
	FRDGBufferRef SourceCounter = GraphBuilder.RegisterExternalBuffer(Data.Counter, TEXT("CSMeshGeneratorDebug.SourceCounter"));
	FRDGBufferRef DebugPositions = GraphBuilder.RegisterExternalBuffer(Positions.Buffer.Pooled, TEXT("CSMeshGeneratorDebug.Positions.External"));
	FRDGBufferRef DebugMainIndices = GraphBuilder.RegisterExternalBuffer(MainIndices.Pooled, TEXT("CSMeshGeneratorDebug.MainIndices.External"));
	FRDGBufferRef DebugPointIndices = GraphBuilder.RegisterExternalBuffer(PointIndices.Pooled, TEXT("CSMeshGeneratorDebug.PointIndices.External"));
	FRDGBufferRef DebugMainArgs = GraphBuilder.RegisterExternalBuffer(MainIndirectArgs, TEXT("CSMeshGeneratorDebug.MainArgs.External"));
	FRDGBufferRef DebugPointArgs = GraphBuilder.RegisterExternalBuffer(PointIndirectArgs, TEXT("CSMeshGeneratorDebug.PointArgs.External"));

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GetScene().GetFeatureLevel());
	FCSMeshGeneratorDebugGeometryCS::FParameters* GeometryParams = GraphBuilder.AllocParameters<FCSMeshGeneratorDebugGeometryCS::FParameters>();
	GeometryParams->InVoxelPositions = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SourcePositions, PF_A32B32G32R32F));
	GeometryParams->InVoxelNormals = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SourceNormals, PF_A32B32G32R32F));
	GeometryParams->RWDebugPositions = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(DebugPositions, PF_R32_FLOAT));
	GeometryParams->RWMainIndices = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(DebugMainIndices, PF_R32_UINT));
	GeometryParams->RWPointIndices = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(DebugPointIndices, PF_R32_UINT));
	GeometryParams->VoxelCapacity = uint32(Data.VoxelCapacity);
	GeometryParams->MaxVoxelsToDraw = uint32(Data.MaxVoxelsToDraw);
	GeometryParams->DebugMode = Data.Mode == ECSMeshGeneratorDebugMode::Directions ? 0u : 1u;
	GeometryParams->bDrawPoints = Data.bDrawPoints ? 1u : 0u;
	GeometryParams->bReverseOrientation = Data.bReverseOrientation ? 1u : 0u;
	GeometryParams->VoxelSize = Data.VoxelSize;
	GeometryParams->DirectionLength = Data.DirectionLength;
	GeometryParams->QuadScale = Data.QuadScale;
	GeometryParams->NormalOffsetScale = Data.NormalOffsetScale;
	TShaderMapRef<FCSMeshGeneratorDebugGeometryCS> GeometryCS(ShaderMap);
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSMeshGeneratorDebug.Geometry"), GeometryCS, GeometryParams,
		FComputeShaderUtils::GetGroupCount(uint32(Data.MaxVoxelsToDraw), 64));

	FCSMeshGeneratorDebugArgsCS::FParameters* ArgsParams = GraphBuilder.AllocParameters<FCSMeshGeneratorDebugArgsCS::FParameters>();
	ArgsParams->InVoxelCounter = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SourceCounter, PF_R32_UINT));
	ArgsParams->RWMainIndirectArgs = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(DebugMainArgs, PF_R32_UINT));
	ArgsParams->RWPointIndirectArgs = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(DebugPointArgs, PF_R32_UINT));
	ArgsParams->VoxelCapacity = uint32(Data.VoxelCapacity);
	ArgsParams->MaxVoxelsToDraw = uint32(Data.MaxVoxelsToDraw);
	ArgsParams->DebugMode = Data.Mode == ECSMeshGeneratorDebugMode::Directions ? 0u : 1u;
	ArgsParams->bDrawPoints = Data.bDrawPoints ? 1u : 0u;
	TShaderMapRef<FCSMeshGeneratorDebugArgsCS> ArgsCS(ShaderMap);
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSMeshGeneratorDebug.IndirectArgs"), ArgsCS, ArgsParams,
		FIntVector(1, 1, 1));

	GraphBuilder.SetBufferAccessFinal(DebugPositions, ERHIAccess::VertexOrIndexBuffer);
	GraphBuilder.SetBufferAccessFinal(DebugMainIndices, ERHIAccess::VertexOrIndexBuffer);
	GraphBuilder.SetBufferAccessFinal(DebugPointIndices, ERHIAccess::VertexOrIndexBuffer);
	GraphBuilder.SetBufferAccessFinal(DebugMainArgs, ERHIAccess::IndirectArgs);
	GraphBuilder.SetBufferAccessFinal(DebugPointArgs, ERHIAccess::IndirectArgs);
	GraphBuilder.Execute();
}
