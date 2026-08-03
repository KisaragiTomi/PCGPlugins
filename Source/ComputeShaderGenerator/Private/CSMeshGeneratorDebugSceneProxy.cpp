#include "CSMeshGeneratorDebugSceneProxy.h"
#include "CSMeshGeneratorDebugComponent.h"
#include "CSGpuMeshSceneProxy.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "Engine/Engine.h"
#include "GlobalShader.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
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
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RWDebugPositions)
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
{
	UMaterialInterface* DebugMaterial = GEngine ? GEngine->DebugMeshMaterial.Get() : UMaterial::GetDefaultMaterial(MD_Surface);
	MaterialRelevance = DebugMaterial->GetRelevance_Concurrent(GetScene().GetShaderPlatform());
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

void FCSMeshGeneratorDebugSceneProxy::FPooledVertexBuffer::InitRHI(FRHICommandListBase& RHICmdList)
{
	if (Pooled.IsValid()) VertexBufferRHI = Pooled->GetRHI();
}

void FCSMeshGeneratorDebugSceneProxy::FPooledVertexBuffer::ReleaseRHI()
{
	VertexBufferRHI.SafeRelease();
}

void FCSMeshGeneratorDebugSceneProxy::FPooledIndexBuffer::InitRHI(FRHICommandListBase& RHICmdList)
{
	if (Pooled.IsValid()) IndexBufferRHI = Pooled->GetRHI();
}

void FCSMeshGeneratorDebugSceneProxy::FPooledIndexBuffer::ReleaseRHI()
{
	IndexBufferRHI.SafeRelease();
}

void FCSMeshGeneratorDebugSceneProxy::CreateRenderThreadResources(FRHICommandListBase& RHICmdList)
{
	FPrimitiveSceneProxy::CreateRenderThreadResources(RHICmdList);

	const uint32 VoxelLimit = uint32(FMath::Max(Data.MaxVoxelsToDraw, 1));
	PositionCapacity = Data.Mode == ECSMeshGeneratorDebugMode::Directions ? VoxelLimit * 2u : VoxelLimit * 4u;
	MainIndexCapacity = Data.Mode == ECSMeshGeneratorDebugMode::Directions ? VoxelLimit * 2u : VoxelLimit * 6u;
	PointIndexCapacity = Data.Mode == ECSMeshGeneratorDebugMode::Directions && Data.bDrawPoints ? VoxelLimit : 1u;

	FRDGBufferDesc PositionDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), PositionCapacity);
	PositionDesc.Usage |= EBufferUsageFlags::VertexBuffer;
	Positions.Pooled = AllocatePooledBuffer(PositionDesc, TEXT("CSMeshGeneratorDebug.Positions"));

	FRDGBufferDesc MainIndexDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), MainIndexCapacity);
	MainIndexDesc.Usage = (MainIndexDesc.Usage & ~EBufferUsageFlags::VertexBuffer) | EBufferUsageFlags::IndexBuffer;
	MainIndices.Pooled = AllocatePooledBuffer(MainIndexDesc, TEXT("CSMeshGeneratorDebug.MainIndices"));

	FRDGBufferDesc PointIndexDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), PointIndexCapacity);
	PointIndexDesc.Usage = (PointIndexDesc.Usage & ~EBufferUsageFlags::VertexBuffer) | EBufferUsageFlags::IndexBuffer;
	PointIndices.Pooled = AllocatePooledBuffer(PointIndexDesc, TEXT("CSMeshGeneratorDebug.PointIndices"));

	MainIndirectArgs = AllocatePooledBuffer(
		FRDGBufferDesc::CreateIndirectDesc(sizeof(uint32), 5), TEXT("CSMeshGeneratorDebug.MainIndirectArgs"));
	PointIndirectArgs = AllocatePooledBuffer(
		FRDGBufferDesc::CreateIndirectDesc(sizeof(uint32), 5), TEXT("CSMeshGeneratorDebug.PointIndirectArgs"));

	Positions.InitResource(RHICmdList);
	MainIndices.InitResource(RHICmdList);
	PointIndices.InitResource(RHICmdList);

	FLocalVertexFactory::FDataType VertexData;
	VertexData.PositionComponent = FVertexStreamComponent(&Positions, 0, sizeof(FVector4f), VET_Float3);
	VertexFactory.SetData(RHICmdList, VertexData);
	VertexFactory.InitResource(RHICmdList);

	BuildGeometry(RHICmdList);
}

void FCSMeshGeneratorDebugSceneProxy::DestroyRenderThreadResources()
{
	VertexFactory.ReleaseResource();
	PointIndices.ReleaseResource();
	MainIndices.ReleaseResource();
	Positions.ReleaseResource();
	PointIndirectArgs.SafeRelease();
	MainIndirectArgs.SafeRelease();
	PointIndices.Pooled.SafeRelease();
	MainIndices.Pooled.SafeRelease();
	Positions.Pooled.SafeRelease();
	FPrimitiveSceneProxy::DestroyRenderThreadResources();
}

void FCSMeshGeneratorDebugSceneProxy::GetDynamicMeshElements(
	const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily,
	uint32 VisibilityMap,
	FMeshElementCollector& Collector) const
{
	if (!Positions.Pooled.IsValid() || !MainIndices.Pooled.IsValid() || !MainIndirectArgs.IsValid()) return;
	UMaterialInterface* DebugMaterial = GEngine ? GEngine->DebugMeshMaterial.Get() : UMaterial::GetDefaultMaterial(MD_Surface);
	FColoredMaterialRenderProxy& MainMaterial = Collector.AllocateOneFrameResource<FColoredMaterialRenderProxy>(
		DebugMaterial->GetRenderProxy(), Data.DirectionColor);
	const EPrimitiveType MainType = Data.Mode == ECSMeshGeneratorDebugMode::Directions ? PT_LineList : PT_TriangleList;
	FCSGpuMeshSceneProxy::SubmitGpuBufferDraw(*this, Views, VisibilityMap, Collector, VertexFactory, MainMaterial,
		MainIndices, MainType, 0u, PositionCapacity - 1u, false, MainIndirectArgs->GetRHI(), 0u);

	if (Data.Mode != ECSMeshGeneratorDebugMode::Directions || !Data.bDrawPoints || !PointIndirectArgs.IsValid()) return;
	FColoredMaterialRenderProxy& PointMaterial = Collector.AllocateOneFrameResource<FColoredMaterialRenderProxy>(
		DebugMaterial->GetRenderProxy(), Data.PointColor);
	FCSGpuMeshSceneProxy::SubmitGpuBufferDraw(*this, Views, VisibilityMap, Collector, VertexFactory, PointMaterial,
		PointIndices, PT_PointList, 0u, PositionCapacity - 1u, false, PointIndirectArgs->GetRHI(), 0u);
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
	FRDGBufferRef DebugPositions = GraphBuilder.RegisterExternalBuffer(Positions.Pooled, TEXT("CSMeshGeneratorDebug.Positions.External"));
	FRDGBufferRef DebugMainIndices = GraphBuilder.RegisterExternalBuffer(MainIndices.Pooled, TEXT("CSMeshGeneratorDebug.MainIndices.External"));
	FRDGBufferRef DebugPointIndices = GraphBuilder.RegisterExternalBuffer(PointIndices.Pooled, TEXT("CSMeshGeneratorDebug.PointIndices.External"));
	FRDGBufferRef DebugMainArgs = GraphBuilder.RegisterExternalBuffer(MainIndirectArgs, TEXT("CSMeshGeneratorDebug.MainArgs.External"));
	FRDGBufferRef DebugPointArgs = GraphBuilder.RegisterExternalBuffer(PointIndirectArgs, TEXT("CSMeshGeneratorDebug.PointArgs.External"));

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GetScene().GetFeatureLevel());
	FCSMeshGeneratorDebugGeometryCS::FParameters* GeometryParams = GraphBuilder.AllocParameters<FCSMeshGeneratorDebugGeometryCS::FParameters>();
	GeometryParams->InVoxelPositions = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SourcePositions, PF_A32B32G32R32F));
	GeometryParams->InVoxelNormals = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SourceNormals, PF_A32B32G32R32F));
	GeometryParams->RWDebugPositions = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(DebugPositions, PF_A32B32G32R32F));
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
