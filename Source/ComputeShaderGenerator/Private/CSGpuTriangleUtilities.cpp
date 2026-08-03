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

	class FCSVertexWeldHashCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FCSVertexWeldHashCS);
		SHADER_USE_PARAMETER_STRUCT(FCSVertexWeldHashCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector3f>, WeldOutputPositions)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, WeldOutputCounter)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_WeldOutputBuckets)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, WeldTriangleFilter)
			RDG_BUFFER_ACCESS(WeldOutputIndirectArgs, ERHIAccess::IndirectArgs)
			SHADER_PARAMETER(uint32, WeldOutputMaxTriangles)
			SHADER_PARAMETER(uint32, WeldOutputBucketMask)
			SHADER_PARAMETER(uint32, WeldTriangleFilterMask)
			SHADER_PARAMETER(FVector3f, WeldOutputOrigin)
			SHADER_PARAMETER(float, WeldOutputInvCellSize)
		END_SHADER_PARAMETER_STRUCT()
		CS_TRIANGLE_UTILITY_PERM()
	};

	class FCSVertexWeldResolveCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FCSVertexWeldResolveCS);
		SHADER_USE_PARAMETER_STRUCT(FCSVertexWeldResolveCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector3f>, WeldOutputPositions)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, WeldOutputCounter)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_WeldOutputBuckets)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_WeldOutputRepresentatives)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, WeldTriangleFilter)
			RDG_BUFFER_ACCESS(WeldOutputIndirectArgs, ERHIAccess::IndirectArgs)
			SHADER_PARAMETER(uint32, WeldOutputMaxTriangles)
			SHADER_PARAMETER(uint32, WeldOutputBucketMask)
			SHADER_PARAMETER(uint32, WeldTriangleFilterMask)
			SHADER_PARAMETER(FVector3f, WeldOutputOrigin)
			SHADER_PARAMETER(float, WeldOutputInvCellSize)
			SHADER_PARAMETER(float, WeldOutputDistanceSq)
		END_SHADER_PARAMETER_STRUCT()
		CS_TRIANGLE_UTILITY_PERM()
	};

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
IMPLEMENT_GLOBAL_SHADER(FCSVertexWeldHashCS, "/Plugin/PCGPlugins/Shaders/Private/CSGpuTriangleUtilities.usf", "OutputWeldHashCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSVertexWeldResolveCS, "/Plugin/PCGPlugins/Shaders/Private/CSGpuTriangleUtilities.usf", "OutputWeldResolveCS", SF_Compute);

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
	uint32 TriangleFilterMask)
{
	const int32 CornerCapacity = FMath::Max(1, OutputTriangleCapacity * 3);
	const uint32 DesiredBuckets = uint32(FMath::Clamp<int64>(
		int64(SourceTriangleCapacity) * 6ll, 1024ll, 1ll << 24));
	uint32 BucketCount = 1u;
	while (BucketCount < DesiredBuckets) BucketCount <<= 1u;

	FRDGBufferRef Buckets = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), int32(BucketCount)),
		TEXT("CS.VertexWeld.Buckets"));
	FRDGBufferRef Representatives = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), CornerCapacity),
		TEXT("CS.VertexWeld.Representatives"));
	FRDGBufferRef IndirectArgs = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(),
		TEXT("CS.VertexWeld.IndirectArgs"));
	FRDGBufferUAVRef BucketUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Buckets, PF_R32_UINT));
	FRDGBufferUAVRef RepresentativeUAV =
		GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Representatives, PF_R32_UINT));
	FRDGBufferUAVRef IndirectArgsUAV =
		GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgs, PF_R32_UINT));
	FRDGBufferSRVRef PositionSRV = GraphBuilder.CreateSRV(OutputTriangleSoup);
	FRDGBufferSRVRef CounterSRV =
		GraphBuilder.CreateSRV(FRDGBufferSRVDesc(OutputTriangleCounter, PF_R32_UINT));
	AddClearUAVPass(GraphBuilder, BucketUAV, 0xFFFFFFFFu);
	AddClearUAVPass(GraphBuilder, RepresentativeUAV, 0xFFFFFFFFu);

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

	{
		FCSVertexWeldHashCS::FParameters* Parameters =
			GraphBuilder.AllocParameters<FCSVertexWeldHashCS::FParameters>();
		Parameters->WeldOutputPositions = PositionSRV;
		Parameters->WeldOutputCounter = CounterSRV;
		Parameters->RW_WeldOutputBuckets = BucketUAV;
		Parameters->WeldTriangleFilter = FilterSRV;
		Parameters->WeldOutputIndirectArgs = IndirectArgs;
		Parameters->WeldOutputMaxTriangles = uint32(OutputTriangleCapacity);
		Parameters->WeldOutputBucketMask = BucketCount - 1u;
		Parameters->WeldTriangleFilterMask = FilterMask;
		Parameters->WeldOutputOrigin = GridOrigin;
		Parameters->WeldOutputInvCellSize = 1.0f / WeldDistance;
		TShaderMapRef<FCSVertexWeldHashCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CS.VertexWeld.Hash"),
			Shader, Parameters, IndirectArgs, 0u);
	}

	{
		FCSVertexWeldResolveCS::FParameters* Parameters =
			GraphBuilder.AllocParameters<FCSVertexWeldResolveCS::FParameters>();
		Parameters->WeldOutputPositions = PositionSRV;
		Parameters->WeldOutputCounter = CounterSRV;
		Parameters->RW_WeldOutputBuckets = BucketUAV;
		Parameters->RW_WeldOutputRepresentatives = RepresentativeUAV;
		Parameters->WeldTriangleFilter = FilterSRV;
		Parameters->WeldOutputIndirectArgs = IndirectArgs;
		Parameters->WeldOutputMaxTriangles = uint32(OutputTriangleCapacity);
		Parameters->WeldOutputBucketMask = BucketCount - 1u;
		Parameters->WeldTriangleFilterMask = FilterMask;
		Parameters->WeldOutputOrigin = GridOrigin;
		Parameters->WeldOutputInvCellSize = 1.0f / WeldDistance;
		Parameters->WeldOutputDistanceSq = WeldDistance * WeldDistance;
		TShaderMapRef<FCSVertexWeldResolveCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CS.VertexWeld.Resolve"),
			Shader, Parameters, IndirectArgs, 0u);
	}

	return Representatives;
}
