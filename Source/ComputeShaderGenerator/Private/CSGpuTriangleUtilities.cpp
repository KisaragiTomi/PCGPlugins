#include "CSGpuTriangleUtilities.h"

#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"

namespace
{
	// All three facilities use the same wide-dispatch convention. Keeping the shader
	// declarations here, beside the RDG orchestration, makes them reusable without
	// giving any mesh-generator actor ownership of render-thread-only implementation.
#define CS_TRIANGLE_UTILITY_PERM() \
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) \
	{ return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5); } \
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment) \
	{ FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment); OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64); }

	class FCSLBVHMortonCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FCSLBVHMortonCS);
		SHADER_USE_PARAMETER_STRUCT(FCSLBVHMortonCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, LBVHTriVerts)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, LBVHKeys)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, LBVHPayload)
			SHADER_PARAMETER(uint32, LBVHTriCount)
			SHADER_PARAMETER(uint32, LBVHArraySize)
			SHADER_PARAMETER(FVector3f, LBVHAabbMin)
			SHADER_PARAMETER(FVector3f, LBVHInvExtent)
		END_SHADER_PARAMETER_STRUCT()
		CS_TRIANGLE_UTILITY_PERM()
	};

	class FCSLBVHBitonicCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FCSLBVHBitonicCS);
		SHADER_USE_PARAMETER_STRUCT(FCSLBVHBitonicCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, LBVHKeys)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, LBVHPayload)
			SHADER_PARAMETER(uint32, LBVHArraySize)
			SHADER_PARAMETER(uint32, LBVHSortJ)
			SHADER_PARAMETER(uint32, LBVHSortK)
		END_SHADER_PARAMETER_STRUCT()
		CS_TRIANGLE_UTILITY_PERM()
	};

	class FCSLBVHLeavesCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FCSLBVHLeavesCS);
		SHADER_USE_PARAMETER_STRUCT(FCSLBVHLeavesCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, LBVHTriVerts)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, LBVHPayload)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, LBVHNodes)
			SHADER_PARAMETER(uint32, LBVHTriCount)
		END_SHADER_PARAMETER_STRUCT()
		CS_TRIANGLE_UTILITY_PERM()
	};

	class FCSLBVHHierarchyCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FCSLBVHHierarchyCS);
		SHADER_USE_PARAMETER_STRUCT(FCSLBVHHierarchyCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, LBVHKeys)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, LBVHNodes)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, LBVHParent)
			SHADER_PARAMETER(uint32, LBVHTriCount)
		END_SHADER_PARAMETER_STRUCT()
		CS_TRIANGLE_UTILITY_PERM()
	};

	class FCSLBVHRefitAtomicCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FCSLBVHRefitAtomicCS);
		SHADER_USE_PARAMETER_STRUCT(FCSLBVHRefitAtomicCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, LBVHTriVerts)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, LBVHPayload)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, LBVHParent)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, LBVHAMin)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, LBVHAMax)
			SHADER_PARAMETER(uint32, LBVHTriCount)
		END_SHADER_PARAMETER_STRUCT()
		CS_TRIANGLE_UTILITY_PERM()
	};

	class FCSLBVHFinalizeCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FCSLBVHFinalizeCS);
		SHADER_USE_PARAMETER_STRUCT(FCSLBVHFinalizeCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, LBVHNodes)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, LBVHAMin)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, LBVHAMax)
			SHADER_PARAMETER(uint32, LBVHTriCount)
		END_SHADER_PARAMETER_STRUCT()
		CS_TRIANGLE_UTILITY_PERM()
	};

	class FCSFastWindingLeafInitCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FCSFastWindingLeafInitCS);
		SHADER_USE_PARAMETER_STRUCT(FCSFastWindingLeafInitCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, LBVHTriVerts)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, LBVHPayload)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, WMpA)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, WMpB)
			SHADER_PARAMETER(uint32, LBVHTriCount)
		END_SHADER_PARAMETER_STRUCT()
		CS_TRIANGLE_UTILITY_PERM()
	};

	class FCSFastWindingMergeCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FCSFastWindingMergeCS);
		SHADER_USE_PARAMETER_STRUCT(FCSFastWindingMergeCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, WMTopo)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, WMpIn)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, WMpOut)
			SHADER_PARAMETER(uint32, LBVHTriCount)
		END_SHADER_PARAMETER_STRUCT()
		CS_TRIANGLE_UTILITY_PERM()
	};

	class FCSVertexWeldIndirectArgsCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FCSVertexWeldIndirectArgsCS);
		SHADER_USE_PARAMETER_STRUCT(FCSVertexWeldIndirectArgsCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, WeldOutputCounter)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_WeldOutputIndirectArgs)
			SHADER_PARAMETER(uint32, WeldOutputMaxTriangles)
		END_SHADER_PARAMETER_STRUCT()
		CS_TRIANGLE_UTILITY_PERM()
	};

	// Parameters shared by the salted-table passes. Every kernel below reads the corner
	// count, the participation filter and the grid; RDG binds only what each one uses.
#define CS_VERTEX_WELD_COMMON_PARAMETERS() \
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector3f>, WeldOutputPositions) \
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, WeldOutputCounter) \
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, WeldTriangleFilter) \
		RDG_BUFFER_ACCESS(WeldOutputIndirectArgs, ERHIAccess::IndirectArgs) \
		SHADER_PARAMETER(uint32, WeldOutputMaxTriangles) \
		SHADER_PARAMETER(uint32, WeldTriangleFilterMask) \
		SHADER_PARAMETER(FVector3f, WeldOutputOrigin) \
		SHADER_PARAMETER(float, WeldOutputInvCellSize) \
		SHADER_PARAMETER(uint32, WeldSaltedTableBase) \
		SHADER_PARAMETER(uint32, WeldSaltedTableMask) \
		SHADER_PARAMETER(uint32, WeldSaltBase) \
		SHADER_PARAMETER(uint32, WeldSaltCount)

	class FCSVertexWeldSaltedHashCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FCSVertexWeldSaltedHashCS);
		SHADER_USE_PARAMETER_STRUCT(FCSVertexWeldSaltedHashCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			CS_VERTEX_WELD_COMMON_PARAMETERS()
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_WeldSaltedBuckets)
		END_SHADER_PARAMETER_STRUCT()
		CS_TRIANGLE_UTILITY_PERM()
	};

	class FCSVertexWeldOrphanHashCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FCSVertexWeldOrphanHashCS);
		SHADER_USE_PARAMETER_STRUCT(FCSVertexWeldOrphanHashCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			CS_VERTEX_WELD_COMMON_PARAMETERS()
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_WeldSaltedBuckets)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, WeldSaltedRepresentatives)
		END_SHADER_PARAMETER_STRUCT()
		CS_TRIANGLE_UTILITY_PERM()
	};

	/** Shared by both resolve rounds; only the entry point differs. */
	BEGIN_SHADER_PARAMETER_STRUCT(FCSVertexWeldResolveParameters, )
		CS_VERTEX_WELD_COMMON_PARAMETERS()
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, WeldSaltedBuckets)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_WeldOutputRepresentatives)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_WeldStats)
		SHADER_PARAMETER(float, WeldOutputDistanceSq)
	END_SHADER_PARAMETER_STRUCT()

	class FCSVertexWeldSaltedResolveCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FCSVertexWeldSaltedResolveCS);
		SHADER_USE_PARAMETER_STRUCT(FCSVertexWeldSaltedResolveCS, FGlobalShader);
		using FParameters = FCSVertexWeldResolveParameters;
		CS_TRIANGLE_UTILITY_PERM()
	};

	class FCSVertexWeldOrphanResolveCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FCSVertexWeldOrphanResolveCS);
		SHADER_USE_PARAMETER_STRUCT(FCSVertexWeldOrphanResolveCS, FGlobalShader);
		using FParameters = FCSVertexWeldResolveParameters;
		CS_TRIANGLE_UTILITY_PERM()
	};

	class FCSVertexWeldFlattenCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FCSVertexWeldFlattenCS);
		SHADER_USE_PARAMETER_STRUCT(FCSVertexWeldFlattenCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, WeldOutputCounter)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, WeldTriangleFilter)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_WeldOutputRepresentatives)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_WeldStats)
			RDG_BUFFER_ACCESS(WeldOutputIndirectArgs, ERHIAccess::IndirectArgs)
			SHADER_PARAMETER(uint32, WeldOutputMaxTriangles)
			SHADER_PARAMETER(uint32, WeldTriangleFilterMask)
			SHADER_PARAMETER(uint32, WeldFlattenPass)
			SHADER_PARAMETER(uint32, WeldFlattenLastPass)
		END_SHADER_PARAMETER_STRUCT()
		CS_TRIANGLE_UTILITY_PERM()
	};

#undef CS_VERTEX_WELD_COMMON_PARAMETERS
#undef CS_TRIANGLE_UTILITY_PERM
}

// A standalone shader compilation unit is important here: consumers can dispatch
// these facilities without registering or depending on any Boolean shader type.
IMPLEMENT_GLOBAL_SHADER(FCSLBVHMortonCS, "/Plugin/PCGPlugins/Shaders/Private/CSGpuTriangleUtilities.usf", "LBVHMortonCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSLBVHBitonicCS, "/Plugin/PCGPlugins/Shaders/Private/CSGpuTriangleUtilities.usf", "LBVHBitonicCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSLBVHLeavesCS, "/Plugin/PCGPlugins/Shaders/Private/CSGpuTriangleUtilities.usf", "LBVHLeavesCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSLBVHHierarchyCS, "/Plugin/PCGPlugins/Shaders/Private/CSGpuTriangleUtilities.usf", "LBVHHierarchyCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSLBVHRefitAtomicCS, "/Plugin/PCGPlugins/Shaders/Private/CSGpuTriangleUtilities.usf", "LBVHRefitAtomicCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSLBVHFinalizeCS, "/Plugin/PCGPlugins/Shaders/Private/CSGpuTriangleUtilities.usf", "LBVHFinalizeCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSFastWindingLeafInitCS, "/Plugin/PCGPlugins/Shaders/Private/CSGpuTriangleUtilities.usf", "WindingLeafInitCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSFastWindingMergeCS, "/Plugin/PCGPlugins/Shaders/Private/CSGpuTriangleUtilities.usf", "WindingMergeCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSVertexWeldIndirectArgsCS, "/Plugin/PCGPlugins/Shaders/Private/CSGpuTriangleUtilities.usf", "OutputWeldIndirectArgsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSVertexWeldSaltedHashCS, "/Plugin/PCGPlugins/Shaders/Private/CSGpuTriangleUtilities.usf", "OutputWeldSaltedHashCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSVertexWeldSaltedResolveCS, "/Plugin/PCGPlugins/Shaders/Private/CSGpuTriangleUtilities.usf", "OutputWeldSaltedResolveCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSVertexWeldOrphanHashCS, "/Plugin/PCGPlugins/Shaders/Private/CSGpuTriangleUtilities.usf", "OutputWeldOrphanHashCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSVertexWeldOrphanResolveCS, "/Plugin/PCGPlugins/Shaders/Private/CSGpuTriangleUtilities.usf", "OutputWeldOrphanResolveCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSVertexWeldFlattenCS, "/Plugin/PCGPlugins/Shaders/Private/CSGpuTriangleUtilities.usf", "OutputWeldFlattenCS", SF_Compute);

CSGpuTriangleUtilities::FTriangleLBVH CSGpuTriangleUtilities::AddTriangleLBVHBuildPasses(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef TriangleSoupSRV,
	int32 TriangleCount,
	int32 SortElementCount,
	const FVector3f& AabbMin,
	const FVector3f& InvExtent)
{
	const int32 NodeCount = FMath::Max(2 * TriangleCount - 1, 1);
	const int32 AtomicSlots = FMath::Max(3 * (TriangleCount - 1), 1);
	FRDGBufferRef Keys = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), FMath::Max(SortElementCount, 1)),
		TEXT("CS.TriangleLBVH.Keys"));
	FRDGBufferRef Payload = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), FMath::Max(SortElementCount, 1)),
		TEXT("CS.TriangleLBVH.Payload"));
	FRDGBufferRef Nodes = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), 2 * NodeCount),
		TEXT("CS.TriangleLBVH.Nodes"));
	FRDGBufferRef Parent = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NodeCount),
		TEXT("CS.TriangleLBVH.Parent"));
	FRDGBufferRef AtomicMin = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), AtomicSlots),
		TEXT("CS.TriangleLBVH.AtomicMin"));
	FRDGBufferRef AtomicMax = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), AtomicSlots),
		TEXT("CS.TriangleLBVH.AtomicMax"));

	FRDGBufferUAVRef KeysUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Keys, PF_R32_UINT));
	FRDGBufferUAVRef PayloadUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Payload, PF_R32_UINT));
	FRDGBufferUAVRef NodesUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Nodes, PF_A32B32G32R32F));
	FRDGBufferUAVRef ParentUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Parent, PF_R32_UINT));
	FRDGBufferUAVRef AtomicMinUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(AtomicMin, PF_R32_UINT));
	FRDGBufferUAVRef AtomicMaxUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(AtomicMax, PF_R32_UINT));
	AddClearUAVPass(GraphBuilder, ParentUAV, 0xFFFFFFFFu);
	AddClearUAVPass(GraphBuilder, AtomicMinUAV, 0xFFFFFFFFu);
	AddClearUAVPass(GraphBuilder, AtomicMaxUAV, 0u);

	{
		FCSLBVHMortonCS::FParameters* Parameters = GraphBuilder.AllocParameters<FCSLBVHMortonCS::FParameters>();
		Parameters->LBVHTriVerts = TriangleSoupSRV;
		Parameters->LBVHKeys = KeysUAV;
		Parameters->LBVHPayload = PayloadUAV;
		Parameters->LBVHTriCount = uint32(TriangleCount);
		Parameters->LBVHArraySize = uint32(SortElementCount);
		Parameters->LBVHAabbMin = AabbMin;
		Parameters->LBVHInvExtent = InvExtent;
		TShaderMapRef<FCSLBVHMortonCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CS.TriangleLBVH.Morton"), Shader, Parameters,
			FComputeShaderUtils::GetGroupCountWrapped(FMath::Max(SortElementCount, 1), 64));
	}

	for (int32 K = 2; K <= SortElementCount; K <<= 1)
	{
		for (int32 J = K >> 1; J > 0; J >>= 1)
		{
			FCSLBVHBitonicCS::FParameters* Parameters =
				GraphBuilder.AllocParameters<FCSLBVHBitonicCS::FParameters>();
			Parameters->LBVHKeys = KeysUAV;
			Parameters->LBVHPayload = PayloadUAV;
			Parameters->LBVHArraySize = uint32(SortElementCount);
			Parameters->LBVHSortJ = uint32(J);
			Parameters->LBVHSortK = uint32(K);
			TShaderMapRef<FCSLBVHBitonicCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CS.TriangleLBVH.Bitonic"), Shader, Parameters,
				FComputeShaderUtils::GetGroupCountWrapped(SortElementCount, 64));
		}
	}

	{
		FCSLBVHLeavesCS::FParameters* Parameters = GraphBuilder.AllocParameters<FCSLBVHLeavesCS::FParameters>();
		Parameters->LBVHTriVerts = TriangleSoupSRV;
		Parameters->LBVHPayload = PayloadUAV;
		Parameters->LBVHNodes = NodesUAV;
		Parameters->LBVHTriCount = uint32(TriangleCount);
		TShaderMapRef<FCSLBVHLeavesCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CS.TriangleLBVH.Leaves"), Shader, Parameters,
			FComputeShaderUtils::GetGroupCountWrapped(TriangleCount, 64));
	}

	if (TriangleCount > 1)
	{
		FCSLBVHHierarchyCS::FParameters* Parameters =
			GraphBuilder.AllocParameters<FCSLBVHHierarchyCS::FParameters>();
		Parameters->LBVHKeys = KeysUAV;
		Parameters->LBVHNodes = NodesUAV;
		Parameters->LBVHParent = ParentUAV;
		Parameters->LBVHTriCount = uint32(TriangleCount);
		TShaderMapRef<FCSLBVHHierarchyCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CS.TriangleLBVH.Hierarchy"), Shader, Parameters,
			FComputeShaderUtils::GetGroupCountWrapped(TriangleCount - 1, 64));
	}

	{
		FCSLBVHRefitAtomicCS::FParameters* Parameters =
			GraphBuilder.AllocParameters<FCSLBVHRefitAtomicCS::FParameters>();
		Parameters->LBVHTriVerts = TriangleSoupSRV;
		Parameters->LBVHPayload = PayloadUAV;
		Parameters->LBVHParent = ParentUAV;
		Parameters->LBVHAMin = AtomicMinUAV;
		Parameters->LBVHAMax = AtomicMaxUAV;
		Parameters->LBVHTriCount = uint32(TriangleCount);
		TShaderMapRef<FCSLBVHRefitAtomicCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CS.TriangleLBVH.RefitAtomic"), Shader, Parameters,
			FComputeShaderUtils::GetGroupCountWrapped(TriangleCount, 64));
	}

	if (TriangleCount > 1)
	{
		FCSLBVHFinalizeCS::FParameters* Parameters =
			GraphBuilder.AllocParameters<FCSLBVHFinalizeCS::FParameters>();
		Parameters->LBVHNodes = NodesUAV;
		Parameters->LBVHAMin = AtomicMinUAV;
		Parameters->LBVHAMax = AtomicMaxUAV;
		Parameters->LBVHTriCount = uint32(TriangleCount);
		TShaderMapRef<FCSLBVHFinalizeCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CS.TriangleLBVH.Finalize"), Shader, Parameters,
			FComputeShaderUtils::GetGroupCountWrapped(TriangleCount - 1, 64));
	}

	return { Nodes, Payload };
}

FRDGBufferRef CSGpuTriangleUtilities::AddFastWindingMultipolePasses(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef TriangleSoupSRV,
	const FTriangleLBVH& LBVH,
	int32 TriangleCount)
{
	const int32 NodeCount = FMath::Max(2 * TriangleCount - 1, 1);
	FRDGBufferRef MultipoleA = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), 5 * NodeCount),
		TEXT("CS.FastWinding.MultipoleA"));
	FRDGBufferRef MultipoleB = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), 5 * NodeCount),
		TEXT("CS.FastWinding.MultipoleB"));
	FRDGBufferUAVRef MultipoleAUAV =
		GraphBuilder.CreateUAV(FRDGBufferUAVDesc(MultipoleA, PF_A32B32G32R32F));
	FRDGBufferUAVRef MultipoleBUAV =
		GraphBuilder.CreateUAV(FRDGBufferUAVDesc(MultipoleB, PF_A32B32G32R32F));
	FRDGBufferSRVRef MultipoleASRV =
		GraphBuilder.CreateSRV(FRDGBufferSRVDesc(MultipoleA, PF_A32B32G32R32F));
	FRDGBufferSRVRef MultipoleBSRV =
		GraphBuilder.CreateSRV(FRDGBufferSRVDesc(MultipoleB, PF_A32B32G32R32F));
	AddClearUAVPass(GraphBuilder, MultipoleAUAV, 0.0f);
	AddClearUAVPass(GraphBuilder, MultipoleBUAV, 0.0f);

	{
		FCSFastWindingLeafInitCS::FParameters* Parameters =
			GraphBuilder.AllocParameters<FCSFastWindingLeafInitCS::FParameters>();
		Parameters->LBVHTriVerts = TriangleSoupSRV;
		Parameters->LBVHPayload =
			GraphBuilder.CreateUAV(FRDGBufferUAVDesc(LBVH.Payload, PF_R32_UINT));
		Parameters->WMpA = MultipoleAUAV;
		Parameters->WMpB = MultipoleBUAV;
		Parameters->LBVHTriCount = uint32(TriangleCount);
		TShaderMapRef<FCSFastWindingLeafInitCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CS.FastWinding.LeafInit"), Shader, Parameters,
			FComputeShaderUtils::GetGroupCountWrapped(TriangleCount, 64));
	}

	if (TriangleCount > 1)
	{
		FRDGBufferSRVRef TopologySRV =
			GraphBuilder.CreateSRV(FRDGBufferSRVDesc(LBVH.Nodes, PF_A32B32G32R32F));

		// A 32-bit binary tree has at most 32 meaningful levels, but the existing
		// implementation used 64 ping-pong passes. Preserve that conservative bound
		// here so extracting the facility cannot change numerical or scheduling behavior.
		for (int32 Pass = 0; Pass < 64; ++Pass)
		{
			const bool bEven = (Pass & 1) == 0;
			FCSFastWindingMergeCS::FParameters* Parameters =
				GraphBuilder.AllocParameters<FCSFastWindingMergeCS::FParameters>();
			Parameters->WMTopo = TopologySRV;
			Parameters->WMpIn = bEven ? MultipoleASRV : MultipoleBSRV;
			Parameters->WMpOut = bEven ? MultipoleBUAV : MultipoleAUAV;
			Parameters->LBVHTriCount = uint32(TriangleCount);
			TShaderMapRef<FCSFastWindingMergeCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CS.FastWinding.Merge.%d", Pass),
				Shader, Parameters, FComputeShaderUtils::GetGroupCountWrapped(TriangleCount - 1, 64));
		}
	}

	return MultipoleA;
}

FRDGBufferRef CSGpuTriangleUtilities::AddVertexWeldPasses(
	FRDGBuilder& GraphBuilder,
	FRDGBufferRef OutputTriangleSoup,
	FRDGBufferRef OutputTriangleCounter,
	int32 OutputTriangleCapacity,
	int32 SourceTriangleCapacity,
	const FVector3f& GridOrigin,
	float WeldDistance,
	FRDGBufferSRVRef TriangleFilter,
	uint32 TriangleFilterMask,
	FRDGBufferUAVRef StatsUAV)
{
	// Two independent tables per round took cross-cell collisions to zero in the Houdini
	// replica of this weld (Docs/meshboolean-weld-failure-cases.md); one table left 3.4% of
	// the corners with their own cell's bucket held by another cell.
	constexpr uint32 TablesPerRound = 2u;
	// A chain link exists only where a representative itself picked a lower one; four
	// jumps flatten chains of sixteen links.
	constexpr int32 FlattenPasses = 4;

	const int32 CornerCapacity = FMath::Max(1, OutputTriangleCapacity * 3);
	const uint32 DesiredBuckets = uint32(FMath::Clamp<int64>(
		int64(SourceTriangleCapacity) * 6ll, 1024ll, 1ll << 24));
	uint32 BucketCount = 1u;
	while (BucketCount < DesiredBuckets) BucketCount <<= 1u;
	// Round 1 hashes only the orphans, a small fraction of the corners.
	const uint32 OrphanBucketCount = FMath::Max(1024u, BucketCount / 4u);
	const uint32 TotalBuckets = TablesPerRound * (BucketCount + OrphanBucketCount);

	FRDGBufferRef Buckets = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), int32(TotalBuckets)),
		TEXT("CS.VertexWeld.SaltedBuckets"));
	FRDGBufferRef Representatives = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), CornerCapacity),
		TEXT("CS.VertexWeld.Representatives"));
	FRDGBufferRef IndirectArgs = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(),
		TEXT("CS.VertexWeld.IndirectArgs"));
	FRDGBufferUAVRef BucketUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Buckets, PF_R32_UINT));
	FRDGBufferSRVRef BucketSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Buckets, PF_R32_UINT));
	FRDGBufferUAVRef RepresentativeUAV =
		GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Representatives, PF_R32_UINT));
	FRDGBufferUAVRef IndirectArgsUAV =
		GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgs, PF_R32_UINT));
	FRDGBufferSRVRef PositionSRV = GraphBuilder.CreateSRV(OutputTriangleSoup);
	FRDGBufferSRVRef CounterSRV =
		GraphBuilder.CreateSRV(FRDGBufferSRVDesc(OutputTriangleCounter, PF_R32_UINT));
	AddClearUAVPass(GraphBuilder, BucketUAV, 0xFFFFFFFFu);
	AddClearUAVPass(GraphBuilder, RepresentativeUAV, 0xFFFFFFFFu);

	if (!StatsUAV)
	{
		FRDGBufferRef Stats = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), VertexWeldStatCount), TEXT("CS.VertexWeld.Stats"));
		StatsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Stats, PF_R32_UINT));
		AddClearUAVPass(GraphBuilder, StatsUAV, 0u);
	}

	// RDG requires every declared resource to be bound. With filtering off the shader
	// short-circuits on the mask before touching the buffer, so any uint SRV will do.
	const uint32 FilterMask = TriangleFilter ? TriangleFilterMask : 0u;
	FRDGBufferSRVRef FilterSRV = TriangleFilter ? TriangleFilter : CounterSRV;

	{
		FCSVertexWeldIndirectArgsCS::FParameters* Parameters =
			GraphBuilder.AllocParameters<FCSVertexWeldIndirectArgsCS::FParameters>();
		Parameters->WeldOutputCounter = CounterSRV;
		Parameters->RW_WeldOutputIndirectArgs = IndirectArgsUAV;
		Parameters->WeldOutputMaxTriangles = uint32(OutputTriangleCapacity);
		TShaderMapRef<FCSVertexWeldIndirectArgsCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CS.VertexWeld.IndirectArgs"),
			Shader, Parameters, FIntVector(1, 1, 1));
	}

	auto FillCommon = [&](auto* Parameters, uint32 TableBase, uint32 TableBucketCount, uint32 SaltBase)
	{
		Parameters->WeldOutputPositions = PositionSRV;
		Parameters->WeldOutputCounter = CounterSRV;
		Parameters->WeldTriangleFilter = FilterSRV;
		Parameters->WeldOutputIndirectArgs = IndirectArgs;
		Parameters->WeldOutputMaxTriangles = uint32(OutputTriangleCapacity);
		Parameters->WeldTriangleFilterMask = FilterMask;
		Parameters->WeldOutputOrigin = GridOrigin;
		Parameters->WeldOutputInvCellSize = 1.0f / WeldDistance;
		Parameters->WeldSaltedTableBase = TableBase;
		Parameters->WeldSaltedTableMask = TableBucketCount - 1u;
		Parameters->WeldSaltBase = SaltBase;
		Parameters->WeldSaltCount = TablesPerRound;
	};
	auto FillResolve = [&](uint32 TableBase, uint32 TableBucketCount, uint32 SaltBase)
	{
		FCSVertexWeldResolveParameters* Parameters = GraphBuilder.AllocParameters<FCSVertexWeldResolveParameters>();
		FillCommon(Parameters, TableBase, TableBucketCount, SaltBase);
		Parameters->WeldSaltedBuckets = BucketSRV;
		Parameters->RW_WeldOutputRepresentatives = RepresentativeUAV;
		Parameters->RW_WeldStats = StatsUAV;
		Parameters->WeldOutputDistanceSq = WeldDistance * WeldDistance;
		return Parameters;
	};

	// Round 0: every participating corner.
	{
		FCSVertexWeldSaltedHashCS::FParameters* Parameters =
			GraphBuilder.AllocParameters<FCSVertexWeldSaltedHashCS::FParameters>();
		FillCommon(Parameters, 0u, BucketCount, 0u);
		Parameters->RW_WeldSaltedBuckets = BucketUAV;
		TShaderMapRef<FCSVertexWeldSaltedHashCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CS.VertexWeld.SaltedHash"),
			Shader, Parameters, IndirectArgs, 0u);
	}
	{
		TShaderMapRef<FCSVertexWeldSaltedResolveCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CS.VertexWeld.SaltedResolve"),
			Shader, FillResolve(0u, BucketCount, 0u), IndirectArgs, 0u);
	}

	// Round 1: orphans only, in their own tables with fresh salts.
	const uint32 OrphanTableBase = TablesPerRound * BucketCount;
	{
		FCSVertexWeldOrphanHashCS::FParameters* Parameters =
			GraphBuilder.AllocParameters<FCSVertexWeldOrphanHashCS::FParameters>();
		FillCommon(Parameters, OrphanTableBase, OrphanBucketCount, TablesPerRound);
		Parameters->RW_WeldSaltedBuckets = BucketUAV;
		Parameters->WeldSaltedRepresentatives =
			GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Representatives, PF_R32_UINT));
		TShaderMapRef<FCSVertexWeldOrphanHashCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CS.VertexWeld.OrphanHash"),
			Shader, Parameters, IndirectArgs, 0u);
	}
	{
		TShaderMapRef<FCSVertexWeldOrphanResolveCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CS.VertexWeld.OrphanResolve"),
			Shader, FillResolve(OrphanTableBase, OrphanBucketCount, TablesPerRound), IndirectArgs, 0u);
	}

	for (int32 Pass = 0; Pass < FlattenPasses; ++Pass)
	{
		FCSVertexWeldFlattenCS::FParameters* Parameters =
			GraphBuilder.AllocParameters<FCSVertexWeldFlattenCS::FParameters>();
		Parameters->WeldOutputCounter = CounterSRV;
		Parameters->WeldTriangleFilter = FilterSRV;
		Parameters->RW_WeldOutputRepresentatives = RepresentativeUAV;
		Parameters->RW_WeldStats = StatsUAV;
		Parameters->WeldOutputIndirectArgs = IndirectArgs;
		Parameters->WeldOutputMaxTriangles = uint32(OutputTriangleCapacity);
		Parameters->WeldTriangleFilterMask = FilterMask;
		Parameters->WeldFlattenPass = uint32(Pass);
		Parameters->WeldFlattenLastPass = Pass == FlattenPasses - 1 ? 1u : 0u;
		TShaderMapRef<FCSVertexWeldFlattenCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CS.VertexWeld.Flatten.%d", Pass),
			Shader, Parameters, IndirectArgs, 0u);
	}

	return Representatives;
}
