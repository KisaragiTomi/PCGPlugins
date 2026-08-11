#include "CSDisplayPointArrowSceneProxy.h"

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHICommandList.h"
#include "SceneInterface.h"

// -----------------------------------------------------------------------------
// 箭头展开 / indirect-args compute shaders（Shaders/Private/CSPointArrowMesh.usf）
// -----------------------------------------------------------------------------

class FCSPointArrowBuildCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCSPointArrowBuildCS);
	SHADER_USE_PARAMETER_STRUCT(FCSPointArrowBuildCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, InPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, InNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RWPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWTangents)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RWTexCoords)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWColors)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWIndices)
		SHADER_PARAMETER(uint32, MaxArrows)
		SHADER_PARAMETER(float, ArrowLength)
		SHADER_PARAMETER(float, ShaftRadius)
		SHADER_PARAMETER(float, HeadRadius)
		SHADER_PARAMETER(float, HeadFraction)
		SHADER_PARAMETER(uint32, ArrowColor)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FCSPointArrowArgsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCSPointArrowArgsCS);
	SHADER_USE_PARAMETER_STRUCT(FCSPointArrowArgsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWMeshCounters)
		SHADER_PARAMETER(uint32, MaxArrows)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCSPointArrowBuildCS, "/Plugin/PCGPlugins/Shaders/Private/CSPointArrowMesh.usf", "BuildPointArrowsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSPointArrowArgsCS, "/Plugin/PCGPlugins/Shaders/Private/CSPointArrowMesh.usf", "BuildPointArrowArgsCS", SF_Compute);

// -----------------------------------------------------------------------------
// FCSDisplayPointArrowSceneProxy
// -----------------------------------------------------------------------------

FCSDisplayPointArrowSceneProxy::FCSDisplayPointArrowSceneProxy(UCSDisplayComponent* Component,
	const FCSDisplayPointArrowData& InData)
	: FCSGpuMeshSceneProxy(Component, Component->MeshMaterial, "FCSDisplayPointArrowSceneProxy")
	, Data(InData)
{
}

FCSDisplayPointArrowSceneProxy::~FCSDisplayPointArrowSceneProxy()
{
}

SIZE_T FCSDisplayPointArrowSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

void FCSDisplayPointArrowSceneProxy::RegisterStreams()
{
	// 每箭头顶点/索引数固定，容量直接由点数算出——不需要回读 GPU 计数。
	ArrowCapacity = FMath::Max<uint32>(Data.MaxArrowsToDraw, 1u);
	VertexCapacity = ArrowCapacity * VertsPerArrow;
	IndexCapacity = ArrowCapacity * IndicesPerArrow;
	AddStandardTriangleStreams();
}

void FCSDisplayPointArrowSceneProxy::BuildGeometry(FRHICommandListBase& RHICmdList)
{
	FRHICommandListImmediate& RHICmdListImmediate = FRHICommandListExecutor::GetImmediateCommandList();
	FRDGBuilder GraphBuilder(RHICmdListImmediate, RDG_EVENT_NAME("CSPointArrow.Build"));

	FRDGBufferRef Positions = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::Position));
	FRDGBufferRef Tangents = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::TangentBasis));
	FRDGBufferRef TexCoords = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::TexCoord));
	FRDGBufferRef Colors = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::Color));
	FRDGBufferRef Indices = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::Index));
	FRDGBufferRef IndirectArgs = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::IndirectArgs));
	FRDGBufferRef MeshCounters = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::MeshCounters));

	FRDGBufferUAVRef IndirectArgsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgs, PF_R32_UINT));
	FRDGBufferUAVRef MeshCountersUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(MeshCounters, PF_R32_UINT));

	const bool bSourceValid = Data.Positions.IsValid() && Data.Normals.IsValid() && Data.Counter.IsValid();
	if (bSourceValid)
	{
		FRDGBufferRef SrcPositions = GraphBuilder.RegisterExternalBuffer(Data.Positions, TEXT("CSPointArrow.SrcPositions"));
		FRDGBufferRef SrcNormals = GraphBuilder.RegisterExternalBuffer(Data.Normals, TEXT("CSPointArrow.SrcNormals"));
		FRDGBufferRef SrcCounter = GraphBuilder.RegisterExternalBuffer(Data.Counter, TEXT("CSPointArrow.SrcCounter"));

		FRDGBufferSRVRef PositionsSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SrcPositions, PF_A32B32G32R32F));
		FRDGBufferSRVRef NormalsSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SrcNormals, PF_A32B32G32R32F));
		FRDGBufferSRVRef CounterSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SrcCounter, PF_R32_UINT));

		FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GetScene().GetFeatureLevel());

		{
			FCSPointArrowBuildCS::FParameters* Params = GraphBuilder.AllocParameters<FCSPointArrowBuildCS::FParameters>();
			Params->InPositions = PositionsSRV;
			Params->InNormals = NormalsSRV;
			Params->InCounter = CounterSRV;
			Params->RWPositions = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Positions, PF_R32_FLOAT));
			Params->RWTangents = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Tangents, PF_R32_UINT));
			Params->RWTexCoords = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TexCoords, PF_R32_FLOAT));
			Params->RWColors = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Colors, PF_R32_UINT));
			Params->RWIndices = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Indices, PF_R32_UINT));
			Params->MaxArrows = ArrowCapacity;
			Params->ArrowLength = Data.ArrowLength;
			Params->ShaftRadius = Data.ShaftRadius;
			Params->HeadRadius = Data.HeadRadius;
			Params->HeadFraction = Data.HeadFraction;
			Params->ArrowColor = Data.ArrowColor.ToFColor(/*bSRGB*/ true).ToPackedABGR();

			TShaderMapRef<FCSPointArrowBuildCS> BuildCS(ShaderMap);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSPointArrow.Expand"), BuildCS, Params,
				FComputeShaderUtils::GetGroupCount(ArrowCapacity, 64));
		}
		{
			FCSPointArrowArgsCS::FParameters* Params = GraphBuilder.AllocParameters<FCSPointArrowArgsCS::FParameters>();
			Params->InCounter = CounterSRV;
			Params->RWIndirectArgs = IndirectArgsUAV;
			Params->RWMeshCounters = MeshCountersUAV;
			Params->MaxArrows = ArrowCapacity;

			TShaderMapRef<FCSPointArrowArgsCS> ArgsCS(ShaderMap);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSPointArrow.IndirectArgs"), ArgsCS, Params,
				FIntVector(1, 1, 1));
		}
	}
	else
	{
		// 无有效源：indirect args 与计数清零（不画、回读为 0）。
		AddClearUAVPass(GraphBuilder, IndirectArgsUAV, 0u);
		AddClearUAVPass(GraphBuilder, MeshCountersUAV, 0u);
	}

	// 与三角汤路径一致：RDG 默认的 SRVMask 尾态对 index / indirect 用途非法，必须显式指定。
	GraphBuilder.SetBufferAccessFinal(Positions, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
	GraphBuilder.SetBufferAccessFinal(Tangents, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
	GraphBuilder.SetBufferAccessFinal(TexCoords, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
	GraphBuilder.SetBufferAccessFinal(Colors, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
	GraphBuilder.SetBufferAccessFinal(Indices, ERHIAccess::VertexOrIndexBuffer);
	GraphBuilder.SetBufferAccessFinal(IndirectArgs, ERHIAccess::IndirectArgs);
	GraphBuilder.SetBufferAccessFinal(MeshCounters, ERHIAccess::CopySrc);

	GraphBuilder.Execute();
}
