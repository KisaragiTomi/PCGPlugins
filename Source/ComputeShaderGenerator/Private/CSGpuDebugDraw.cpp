#include "CSGpuDebugDraw.h"
#include "CSGpuMeshSceneProxy.h"

#include "Engine/Engine.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalRenderResources.h"
#include "GlobalShader.h"
#include "MeshBatch.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHICommandList.h"
#include "SceneManagement.h"
#include "ShaderParameterStruct.h"

// Every debug shape kernel lives in CSGpuDebugDraw.usf and is declared here, so a proxy that
// wants a new visual asks for a pass instead of carrying its own global shader.
#define CSGDD_DECLARE_SHADER(ShaderClass, EntryPoint, ParameterList)                          \
	class ShaderClass : public FGlobalShader                                                  \
	{                                                                                         \
	public:                                                                                   \
		DECLARE_GLOBAL_SHADER(ShaderClass);                                                   \
		SHADER_USE_PARAMETER_STRUCT(ShaderClass, FGlobalShader);                              \
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )                                          \
			ParameterList                                                                     \
		END_SHADER_PARAMETER_STRUCT()                                                         \
		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Params) \
		{                                                                                     \
			return IsFeatureLevelSupported(Params.Platform, ERHIFeatureLevel::SM5);           \
		}                                                                                     \
	};                                                                                        \
	IMPLEMENT_GLOBAL_SHADER(ShaderClass, "/Plugin/PCGPlugins/Shaders/Private/CSGpuDebugDraw.usf", EntryPoint, SF_Compute);

CSGDD_DECLARE_SHADER(FCSGpuDebugUnpackPositionsCS, "UnpackPositionsCS",
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, InSourcePositions)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RWDebugPositions)
	SHADER_PARAMETER(uint32, NumPositions))

CSGDD_DECLARE_SHADER(FCSGpuDebugVoxelDirectionsCS, "BuildVoxelDirectionsCS",
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, InVoxelPositions)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, InVoxelNormals)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RWDebugPositions)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWDebugIndices)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWDebugPointIndices)
	SHADER_PARAMETER(uint32, VoxelCapacity)
	SHADER_PARAMETER(uint32, MaxItems)
	SHADER_PARAMETER(float, DirectionLength))

CSGDD_DECLARE_SHADER(FCSGpuDebugVoxelQuadsCS, "BuildVoxelQuadsCS",
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, InVoxelPositions)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, InVoxelNormals)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RWDebugPositions)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWDebugIndices)
	SHADER_PARAMETER(uint32, VoxelCapacity)
	SHADER_PARAMETER(uint32, MaxItems)
	SHADER_PARAMETER(uint32, bReverseOrientation)
	SHADER_PARAMETER(float, VoxelSize)
	SHADER_PARAMETER(float, QuadScale)
	SHADER_PARAMETER(float, NormalOffsetScale))

CSGDD_DECLARE_SHADER(FCSGpuDebugLineIndicesCS, "BuildLineIndicesCS",
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, InLineIndexPairs)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWDebugIndices)
	SHADER_PARAMETER(uint32, NumSegments))

CSGDD_DECLARE_SHADER(FCSGpuDebugIndirectArgsCS, "BuildIndirectArgsCS",
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InVoxelCounter)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWDebugIndirectArgs)
	SHADER_PARAMETER(uint32, VoxelCapacity)
	SHADER_PARAMETER(uint32, MaxItems)
	SHADER_PARAMETER(uint32, IndicesPerItem))

#undef CSGDD_DECLARE_SHADER

UMaterialInterface* FCSGpuDebugDraw::GetDebugMaterial()
{
	UMaterialInterface* DebugMaterial = GEngine ? GEngine->DebugMeshMaterial.Get() : nullptr;
	return DebugMaterial ? DebugMaterial : UMaterial::GetDefaultMaterial(MD_Surface);
}

void FCSGpuDebugDraw::AllocatePositionStream(FRHICommandListBase& RHICmdList, FCSGpuDebugPositionStream& OutStream,
	uint32 NumVertices, const TCHAR* DebugName)
{
	FRDGBufferDesc Desc = FRDGBufferDesc::CreateBufferDesc(sizeof(float), FMath::Max(NumVertices, 1u) * 3u);
	Desc.Usage |= EBufferUsageFlags::VertexBuffer;
	OutStream.Buffer.Pooled = AllocatePooledBuffer(Desc, DebugName);
	OutStream.Buffer.InitResource(RHICmdList);
	OutStream.SRV = RHICmdList.CreateShaderResourceView(OutStream.Buffer.Pooled->GetRHI(),
		FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(PF_R32_FLOAT));
}

void FCSGpuDebugDraw::AllocateIndexBuffer(FRHICommandListBase& RHICmdList, FCSPooledIndexBuffer& OutBuffer,
	uint32 NumIndices, const TCHAR* DebugName)
{
	FRDGBufferDesc Desc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), FMath::Max(NumIndices, 1u));
	Desc.Usage = (Desc.Usage & ~EBufferUsageFlags::VertexBuffer) | EBufferUsageFlags::IndexBuffer;
	OutBuffer.Pooled = AllocatePooledBuffer(Desc, DebugName);
	OutBuffer.InitResource(RHICmdList);
}

TRefCountPtr<FRDGPooledBuffer> FCSGpuDebugDraw::AllocateIndirectArgs(const TCHAR* DebugName)
{
	return AllocatePooledBuffer(FRDGBufferDesc::CreateIndirectDesc(sizeof(uint32), 5), DebugName);
}

void FCSGpuDebugDraw::BindPositionOnlyVertexFactory(FRHICommandListBase& RHICmdList, FLocalVertexFactory& VertexFactory,
	FCSGpuDebugPositionStream& PositionStream)
{
	FLocalVertexFactory::FDataType Data;
	Data.PositionComponent = FVertexStreamComponent(&PositionStream.Buffer, 0, sizeof(float) * 3, VET_Float3);
	// Manual vertex fetch reads positions from here, not from the stream above. The tangent /
	// texcoord / colour SRVs have no engine-side fallback either (only pre-skin positions do),
	// so a position-only factory must still hand the uniform buffer non-null dummies.
	Data.PositionComponentSRV = PositionStream.SRV;
	Data.TangentsSRV = GNullColorVertexBuffer.VertexBufferSRV;
	Data.TextureCoordinatesSRV = GNullColorVertexBuffer.VertexBufferSRV;
	Data.ColorComponentsSRV = GNullColorVertexBuffer.VertexBufferSRV;
	VertexFactory.SetData(RHICmdList, Data);
	VertexFactory.InitResource(RHICmdList);
}

void FCSGpuDebugDraw::ReleasePositionStream(FCSGpuDebugPositionStream& Stream)
{
	Stream.Buffer.ReleaseResource();
	Stream.SRV.SafeRelease();
	Stream.Buffer.Pooled.SafeRelease();
}

void FCSGpuDebugDraw::ReleaseIndexBuffer(FCSPooledIndexBuffer& Buffer)
{
	Buffer.ReleaseResource();
	Buffer.Pooled.SafeRelease();
}

void FCSGpuDebugDraw::AddVoxelDirectionsPass(FRDGBuilder& GraphBuilder, ERHIFeatureLevel::Type FeatureLevel,
	const FCSGpuDebugVoxelSource& Source, float DirectionLength,
	FRDGBufferRef OutPositions, FRDGBufferRef OutLineIndices, FRDGBufferRef OutPointIndices)
{
	if (!Source.IsValid() || !OutPositions || !OutLineIndices || !OutPointIndices) return;

	FCSGpuDebugVoxelDirectionsCS::FParameters* Parameters = GraphBuilder.AllocParameters<FCSGpuDebugVoxelDirectionsCS::FParameters>();
	Parameters->InVoxelPositions = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Source.Positions, PF_A32B32G32R32F));
	Parameters->InVoxelNormals = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Source.Normals, PF_A32B32G32R32F));
	Parameters->RWDebugPositions = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutPositions, PF_R32_FLOAT));
	Parameters->RWDebugIndices = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutLineIndices, PF_R32_UINT));
	Parameters->RWDebugPointIndices = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutPointIndices, PF_R32_UINT));
	Parameters->VoxelCapacity = Source.Capacity;
	Parameters->MaxItems = Source.MaxItems;
	Parameters->DirectionLength = DirectionLength;
	TShaderMapRef<FCSGpuDebugVoxelDirectionsCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSGpuDebug.VoxelDirections"), ComputeShader, Parameters,
		FComputeShaderUtils::GetGroupCount(Source.MaxItems, 64));
}

void FCSGpuDebugDraw::AddVoxelQuadsPass(FRDGBuilder& GraphBuilder, ERHIFeatureLevel::Type FeatureLevel,
	const FCSGpuDebugVoxelSource& Source, float VoxelSize, float QuadScale, float NormalOffsetScale,
	bool bReverseOrientation, FRDGBufferRef OutPositions, FRDGBufferRef OutIndices)
{
	if (!Source.IsValid() || !OutPositions || !OutIndices) return;

	FCSGpuDebugVoxelQuadsCS::FParameters* Parameters = GraphBuilder.AllocParameters<FCSGpuDebugVoxelQuadsCS::FParameters>();
	Parameters->InVoxelPositions = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Source.Positions, PF_A32B32G32R32F));
	Parameters->InVoxelNormals = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Source.Normals, PF_A32B32G32R32F));
	Parameters->RWDebugPositions = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutPositions, PF_R32_FLOAT));
	Parameters->RWDebugIndices = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutIndices, PF_R32_UINT));
	Parameters->VoxelCapacity = Source.Capacity;
	Parameters->MaxItems = Source.MaxItems;
	Parameters->bReverseOrientation = bReverseOrientation ? 1u : 0u;
	Parameters->VoxelSize = VoxelSize;
	Parameters->QuadScale = QuadScale;
	Parameters->NormalOffsetScale = NormalOffsetScale;
	TShaderMapRef<FCSGpuDebugVoxelQuadsCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSGpuDebug.VoxelQuads"), ComputeShader, Parameters,
		FComputeShaderUtils::GetGroupCount(Source.MaxItems, 64));
}

void FCSGpuDebugDraw::AddLineIndicesPass(FRDGBuilder& GraphBuilder, ERHIFeatureLevel::Type FeatureLevel,
	FRDGBufferRef IndexPairs, uint32 NumSegments, FRDGBufferRef OutIndices)
{
	if (!IndexPairs || !OutIndices || NumSegments == 0u) return;

	FCSGpuDebugLineIndicesCS::FParameters* Parameters = GraphBuilder.AllocParameters<FCSGpuDebugLineIndicesCS::FParameters>();
	Parameters->InLineIndexPairs = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(IndexPairs));
	Parameters->RWDebugIndices = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutIndices, PF_R32_UINT));
	Parameters->NumSegments = NumSegments;
	TShaderMapRef<FCSGpuDebugLineIndicesCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSGpuDebug.LineIndices"), ComputeShader, Parameters,
		FComputeShaderUtils::GetGroupCount(NumSegments, 64));
}

void FCSGpuDebugDraw::AddIndirectArgsPass(FRDGBuilder& GraphBuilder, ERHIFeatureLevel::Type FeatureLevel,
	FRDGBufferRef Counter, uint32 Capacity, uint32 MaxItems, uint32 IndicesPerItem, FRDGBufferRef OutArgs)
{
	if (!Counter || !OutArgs) return;

	FCSGpuDebugIndirectArgsCS::FParameters* Parameters = GraphBuilder.AllocParameters<FCSGpuDebugIndirectArgsCS::FParameters>();
	Parameters->InVoxelCounter = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Counter, PF_R32_UINT));
	Parameters->RWDebugIndirectArgs = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutArgs, PF_R32_UINT));
	Parameters->VoxelCapacity = Capacity;
	Parameters->MaxItems = MaxItems;
	Parameters->IndicesPerItem = IndicesPerItem;
	TShaderMapRef<FCSGpuDebugIndirectArgsCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSGpuDebug.IndirectArgs"), ComputeShader, Parameters,
		FIntVector(1, 1, 1));
}

void FCSGpuDebugDraw::AddPositionUnpackPass(FRDGBuilder& GraphBuilder, ERHIFeatureLevel::Type FeatureLevel,
	FRDGBufferRef SourceFloat4, uint32 NumVertices, FRDGBufferRef DestFloat3)
{
	if (!SourceFloat4 || !DestFloat3 || NumVertices == 0u) return;

	FCSGpuDebugUnpackPositionsCS::FParameters* Parameters = GraphBuilder.AllocParameters<FCSGpuDebugUnpackPositionsCS::FParameters>();
	Parameters->InSourcePositions = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SourceFloat4));
	Parameters->RWDebugPositions = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(DestFloat3, PF_R32_FLOAT));
	Parameters->NumPositions = NumVertices;
	TShaderMapRef<FCSGpuDebugUnpackPositionsCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSGpuDebug.UnpackPositions"), ComputeShader, Parameters,
		FComputeShaderUtils::GetGroupCount(NumVertices, 64));
}

void FCSGpuDebugDraw::SubmitColoredDraw(
	const FPrimitiveSceneProxy& SceneProxy,
	const TArray<const FSceneView*>& Views,
	uint32 VisibilityMap,
	FMeshElementCollector& Collector,
	const FVertexFactory& VertexFactory,
	const FIndexBuffer& IndexBuffer,
	EPrimitiveType PrimitiveType,
	const FLinearColor& Color,
	uint32 NumPrimitives,
	uint32 MaxVertexIndex,
	FRHIBuffer* IndirectArgsBuffer,
	uint32 IndirectArgsOffset)
{
	FColoredMaterialRenderProxy& MaterialProxy = Collector.AllocateOneFrameResource<FColoredMaterialRenderProxy>(
		GetDebugMaterial()->GetRenderProxy(), Color);
	FCSGpuMeshSceneProxy::SubmitGpuBufferDraw(SceneProxy, Views, VisibilityMap, Collector, VertexFactory, MaterialProxy,
		IndexBuffer, PrimitiveType, NumPrimitives, MaxVertexIndex, false, IndirectArgsBuffer, IndirectArgsOffset);
}
