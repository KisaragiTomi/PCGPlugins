#include "CSDirectTriangleMeshSceneProxy.h"
#include "CSDirectTriangleMeshComponent.h"

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHICommandList.h"
#include "SceneInterface.h"

// -----------------------------------------------------------------------------
// Pack / indirect-args compute shaders (Shaders/Private/CSDirectMesh.usf)
// -----------------------------------------------------------------------------

class FCSDirectMeshPackCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCSDirectMeshPackCS);
	SHADER_USE_PARAMETER_STRUCT(FCSDirectMeshPackCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, InVertices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, InNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RWPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWTangents)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RWTexCoords)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWColors)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWIndices)
		SHADER_PARAMETER(uint32, MaxVertices)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FCSDirectMeshArgsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCSDirectMeshArgsCS);
	SHADER_USE_PARAMETER_STRUCT(FCSDirectMeshArgsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWMeshCounters)
		SHADER_PARAMETER(uint32, MaxVertices)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCSDirectMeshPackCS, "/Plugin/PCGPlugins/Shaders/Private/CSDirectMesh.usf", "PackTrianglesCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSDirectMeshArgsCS, "/Plugin/PCGPlugins/Shaders/Private/CSDirectMesh.usf", "BuildIndirectArgsCS", SF_Compute);

// -----------------------------------------------------------------------------
// FCSDirectTriangleMeshSceneProxy
// -----------------------------------------------------------------------------

FCSDirectTriangleMeshSceneProxy::FCSDirectTriangleMeshSceneProxy(UCSDirectTriangleMeshComponent* Component,
	const FCSBoxScenePreparedData& InPrepared, uint32 InVertexCapacity)
	: FCSGpuMeshSceneProxy(Component, Component->MeshMaterial, "FCSDirectTriangleMeshSceneProxy")
	, Prepared(InPrepared)
	, InputVertexCapacity(InVertexCapacity)
{
}

FCSDirectTriangleMeshSceneProxy::~FCSDirectTriangleMeshSceneProxy()
{
}

SIZE_T FCSDirectTriangleMeshSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

void FCSDirectTriangleMeshSceneProxy::RegisterStreams()
{
	VertexCapacity = FMath::Max(InputVertexCapacity, 3u);
	IndexCapacity = VertexCapacity; // triangle soup: identity index buffer, V == I
	AddStandardTriangleStreams();
}

void FCSDirectTriangleMeshSceneProxy::BuildGeometry(FRHICommandListBase& RHICmdList)
{
	const uint32 MaxVertices = FMath::Max(VertexCapacity, 3u);

	FRHICommandListImmediate& RHICmdListImmediate = FRHICommandListExecutor::GetImmediateCommandList();
	FRDGBuilder GraphBuilder(RHICmdListImmediate, RDG_EVENT_NAME("CSDirectMesh.Build"));

	const FCSStaticMeshTriangleRDGOutput Soup = AComputeShaderMeshGenerator::AddPreparedBoxSceneTrianglesToRDG(
		GraphBuilder, RHICmdListImmediate, Prepared, TEXT("CSDirectMesh.Soup"));

	FRDGBufferRef Positions = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::Position));
	FRDGBufferRef Tangents = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::TangentBasis));
	FRDGBufferRef TexCoords = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::TexCoord));
	FRDGBufferRef Colors = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::Color));
	FRDGBufferRef Indices = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::Index));
	FRDGBufferRef IndirectArgs = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::IndirectArgs));
	FRDGBufferRef MeshCounters = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::MeshCounters));

	FRDGBufferUAVRef IndirectArgsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgs, PF_R32_UINT));
	FRDGBufferUAVRef MeshCountersUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(MeshCounters, PF_R32_UINT));

	if (Soup.TriangleVertices && Soup.TriangleNormals && Soup.TriangleCounter)
	{
		FRDGBufferSRVRef VerticesSRV = Soup.TriangleVerticesSRV ? Soup.TriangleVerticesSRV
			: GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Soup.TriangleVertices, PF_A32B32G32R32F));
		FRDGBufferSRVRef NormalsSRV = Soup.TriangleNormalsSRV ? Soup.TriangleNormalsSRV
			: GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Soup.TriangleNormals, PF_A32B32G32R32F));
		FRDGBufferSRVRef CounterSRV = Soup.TriangleCounterSRV ? Soup.TriangleCounterSRV
			: GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Soup.TriangleCounter, PF_R32_UINT));

		FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GetScene().GetFeatureLevel());

		{
			FCSDirectMeshPackCS::FParameters* Params = GraphBuilder.AllocParameters<FCSDirectMeshPackCS::FParameters>();
			Params->InVertices = VerticesSRV;
			Params->InNormals = NormalsSRV;
			Params->InCounter = CounterSRV;
			Params->RWPositions = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Positions, PF_R32_FLOAT));
			Params->RWTangents = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Tangents, PF_R32_UINT));
			Params->RWTexCoords = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TexCoords, PF_R32_FLOAT));
			Params->RWColors = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Colors, PF_R32_UINT));
			Params->RWIndices = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Indices, PF_R32_UINT));
			Params->MaxVertices = MaxVertices;

			TShaderMapRef<FCSDirectMeshPackCS> PackCS(ShaderMap);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSDirectMesh.Pack"), PackCS, Params,
				FComputeShaderUtils::GetGroupCount(MaxVertices, 64));
		}
		{
			FCSDirectMeshArgsCS::FParameters* Params = GraphBuilder.AllocParameters<FCSDirectMeshArgsCS::FParameters>();
			Params->InCounter = CounterSRV;
			Params->RWIndirectArgs = IndirectArgsUAV;
			Params->RWMeshCounters = MeshCountersUAV;
			Params->MaxVertices = MaxVertices;

			TShaderMapRef<FCSDirectMeshArgsCS> ArgsCS(ShaderMap);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSDirectMesh.IndirectArgs"), ArgsCS, Params,
				FIntVector(1, 1, 1));
		}
	}
	else
	{
		// No triangles: zero the indirect args (draw nothing) and the counters (readback sees 0).
		AddClearUAVPass(GraphBuilder, IndirectArgsUAV, 0u);
		AddClearUAVPass(GraphBuilder, MeshCountersUAV, 0u);
	}

	// Leave the persistent buffers in the states the draw / readback paths need; RDG's default
	// epilogue state (SRVMask) is illegal for index / indirect usage.
	GraphBuilder.SetBufferAccessFinal(Positions, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
	GraphBuilder.SetBufferAccessFinal(Tangents, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
	GraphBuilder.SetBufferAccessFinal(TexCoords, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
	GraphBuilder.SetBufferAccessFinal(Colors, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
	GraphBuilder.SetBufferAccessFinal(Indices, ERHIAccess::VertexOrIndexBuffer);
	GraphBuilder.SetBufferAccessFinal(IndirectArgs, ERHIAccess::IndirectArgs);
	GraphBuilder.SetBufferAccessFinal(MeshCounters, ERHIAccess::CopySrc);

	GraphBuilder.Execute();
}
