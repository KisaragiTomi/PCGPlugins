#include "MeshBooleanRepair.h"

#include "CSGpuTriangleUtilities.h"
#include "ComputeShaderGenerateHelper.h"

#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"

namespace
{
	constexpr uint32 RepairScanBlock = 512u;
	constexpr uint32 RepairNone = 0xFFFFFFFFu;

	class FMBRepClassifyCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FMBRepClassifyCS);
		SHADER_USE_PARAMETER_STRUCT(FMBRepClassifyCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector3f>, MBRepSoup)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepSource)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, MBRepSourceVertices)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepReps)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBRepFlags)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBRepStats)
			SHADER_PARAMETER(uint32, MBRepFragmentCount)
			SHADER_PARAMETER(uint32, MBRepSourceCount)
			SHADER_PARAMETER(uint32, MBRepStageB)
		END_SHADER_PARAMETER_STRUCT()
		CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
	};

	class FMBRepCountCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FMBRepCountCS);
		SHADER_USE_PARAMETER_STRUCT(FMBRepCountCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepReps)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepFlags)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBRepVertexCount)
			SHADER_PARAMETER(uint32, MBRepFragmentCount)
		END_SHADER_PARAMETER_STRUCT()
		CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
	};

	class FMBRepScatterCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FMBRepScatterCS);
		SHADER_USE_PARAMETER_STRUCT(FMBRepScatterCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepReps)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepFlags)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepVertexOffset)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBRepVertexCount)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBRepVertexEntries)
			SHADER_PARAMETER(uint32, MBRepFragmentCount)
		END_SHADER_PARAMETER_STRUCT()
		CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
	};

	class FMBRepScanBlocksCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FMBRepScanBlocksCS);
		SHADER_USE_PARAMETER_STRUCT(FMBRepScanBlocksCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepScanInput)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBRepScanOutput)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBRepScanBlockSums)
			SHADER_PARAMETER(uint32, MBRepScanCount)
		END_SHADER_PARAMETER_STRUCT()
		CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
	};

	class FMBRepScanAddOffsetsCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FMBRepScanAddOffsetsCS);
		SHADER_USE_PARAMETER_STRUCT(FMBRepScanAddOffsetsCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepScanBlockOffsets)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBRepScanOutput)
			SHADER_PARAMETER(uint32, MBRepScanCount)
		END_SHADER_PARAMETER_STRUCT()
		CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
	};

	// Topology queries through the vertex table, with the flags writable by the pass.
#define MB_REPAIR_TOPOLOGY_PARAMETERS() \
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector3f>, MBRepSoup) \
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepSource) \
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, MBRepSourceVertices) \
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepReps) \
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepVertexOffset) \
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepVertexCount) \
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepVertexEntries) \
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBRepFlags) \
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBRepStats) \
		SHADER_PARAMETER(uint32, MBRepFragmentCount)

	class FMBRepRestoreMarkCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FMBRepRestoreMarkCS);
		SHADER_USE_PARAMETER_STRUCT(FMBRepRestoreMarkCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			MB_REPAIR_TOPOLOGY_PARAMETERS()
			SHADER_PARAMETER(uint32, MBRepRound)
			SHADER_PARAMETER(float, MBRepMaxBendRadians)
		END_SHADER_PARAMETER_STRUCT()
		CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
	};

	class FMBRepRestoreExposureCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FMBRepRestoreExposureCS);
		SHADER_USE_PARAMETER_STRUCT(FMBRepRestoreExposureCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector3f>, MBRepSoup)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepSource)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, MBRepSourceVertices)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBRepFlags)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, WMTopo)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, WMMultipole)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, WSoup)
			SHADER_PARAMETER(uint32, WFastTriCount)
			SHADER_PARAMETER(float, WBetaSq)
			SHADER_PARAMETER(uint32, MBRepFragmentCount)
			SHADER_PARAMETER(float, MBRepWindingThreshold)
			SHADER_PARAMETER(float, MBRepExposureOffset)
		END_SHADER_PARAMETER_STRUCT()
		CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
	};

	class FMBRepRestoreApplyCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FMBRepRestoreApplyCS);
		SHADER_USE_PARAMETER_STRUCT(FMBRepRestoreApplyCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBRepFlags)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBRepStats)
			SHADER_PARAMETER(uint32, MBRepFragmentCount)
		END_SHADER_PARAMETER_STRUCT()
		CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
	};

	class FMBRepDedupCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FMBRepDedupCS);
		SHADER_USE_PARAMETER_STRUCT(FMBRepDedupCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			MB_REPAIR_TOPOLOGY_PARAMETERS()
		END_SHADER_PARAMETER_STRUCT()
		CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
	};

	class FMBRepBoundaryCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FMBRepBoundaryCS);
		SHADER_USE_PARAMETER_STRUCT(FMBRepBoundaryCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			MB_REPAIR_TOPOLOGY_PARAMETERS()
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBRepBoundaryList)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBRepBoundaryCounter)
			SHADER_PARAMETER(uint32, MBRepBoundaryCapacity)
			SHADER_PARAMETER(uint32, MBRepFinalPass)
		END_SHADER_PARAMETER_STRUCT()
		CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
	};

#undef MB_REPAIR_TOPOLOGY_PARAMETERS

	class FMBRepSlitArgsCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FMBRepSlitArgsCS);
		SHADER_USE_PARAMETER_STRUCT(FMBRepSlitArgsCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepBoundaryCounterSRV)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBRepIndirectArgs)
			SHADER_PARAMETER(uint32, MBRepBoundaryCapacity)
		END_SHADER_PARAMETER_STRUCT()
		CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
	};

	class FMBRepSlitLoopCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FMBRepSlitLoopCS);
		SHADER_USE_PARAMETER_STRUCT(FMBRepSlitLoopCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector3f>, MBRepSoup)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepSource)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepReps)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepVertexOffset)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepVertexCount)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepVertexEntries)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepBoundaryList)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepBoundaryCounterSRV)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBRepFlags)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBRepStats)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBRepFillCounter)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint4>, RW_MBRepFills)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBRepLoopSlots)
			RDG_BUFFER_ACCESS(MBRepIndirectArgsBuffer, ERHIAccess::IndirectArgs)
			SHADER_PARAMETER(uint32, MBRepBoundaryCapacity)
			SHADER_PARAMETER(uint32, MBRepFillCapacity)
			SHADER_PARAMETER(float, MBRepSlitTolerance)
			SHADER_PARAMETER(uint32, MBRepSlitMarkOnly)
		END_SHADER_PARAMETER_STRUCT()
		CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
	};

	class FMBRepLoopPushCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FMBRepLoopPushCS);
		SHADER_USE_PARAMETER_STRUCT(FMBRepLoopPushCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector3f>, MBRepSoup)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepSource)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, MBRepSourceVertices)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepReps)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepFlags)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepVertexOffset)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepVertexCount)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepVertexEntries)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBRepStats)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBRepLoopSlots)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_MBRepLoopOffsets)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBRepLoopOffsetCounter)
			SHADER_PARAMETER(uint32, MBRepFragmentCount)
			SHADER_PARAMETER(uint32, MBRepLoopOffsetCapacity)
			SHADER_PARAMETER(float, MBRepNudgeDistance)
		END_SHADER_PARAMETER_STRUCT()
		CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
	};

	class FMBRepCountOpenCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FMBRepCountOpenCS);
		SHADER_USE_PARAMETER_STRUCT(FMBRepCountOpenCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepFlags)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBRepStats)
			SHADER_PARAMETER(uint32, MBRepFragmentCount)
		END_SHADER_PARAMETER_STRUCT()
		CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
	};

	// Everything MBRepTriangle / MBRepThinVertex read, over fragments and fills together.
#define MB_REPAIR_TRIANGLE_PARAMETERS() \
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector3f>, MBRepSoup) \
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepSource) \
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, MBRepSourceVertices) \
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepReps) \
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepFlags) \
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepLoopSlots) \
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, MBRepLoopOffsets) \
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint4>, MBRepFills) \
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepFillCounter) \
		SHADER_PARAMETER(uint32, MBRepFragmentCount) \
		SHADER_PARAMETER(uint32, MBRepSourceCount) \
		SHADER_PARAMETER(uint32, MBRepFillCapacity) \
		SHADER_PARAMETER(float, MBRepNudgeDistance) \
		SHADER_PARAMETER(float, MBRepSlitTolerance)

	class FMBRepNudgeClaimCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FMBRepNudgeClaimCS);
		SHADER_USE_PARAMETER_STRUCT(FMBRepNudgeClaimCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			MB_REPAIR_TRIANGLE_PARAMETERS()
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBRepClaims)
		END_SHADER_PARAMETER_STRUCT()
		CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
	};

	class FMBRepFinalCheckCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FMBRepFinalCheckCS);
		SHADER_USE_PARAMETER_STRUCT(FMBRepFinalCheckCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			MB_REPAIR_TRIANGLE_PARAMETERS()
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepClaims)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBRepStats)
		END_SHADER_PARAMETER_STRUCT()
		CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
	};

#undef MB_REPAIR_TRIANGLE_PARAMETERS

	class FMBRepFinalizeStatsCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FMBRepFinalizeStatsCS);
		SHADER_USE_PARAMETER_STRUCT(FMBRepFinalizeStatsCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MBRepFillCounter)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MBRepStats)
			SHADER_PARAMETER(uint32, MBRepFillCapacity)
		END_SHADER_PARAMETER_STRUCT()
		CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
	};
}

#define MB_REPAIR_SHADER_PATH "/Plugin/PCGPlugins/Shaders/Private/MeshBooleanRepair.usf"
IMPLEMENT_GLOBAL_SHADER(FMBRepClassifyCS, MB_REPAIR_SHADER_PATH, "MBRepClassifyCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FMBRepCountCS, MB_REPAIR_SHADER_PATH, "MBRepCountCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FMBRepScatterCS, MB_REPAIR_SHADER_PATH, "MBRepScatterCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FMBRepScanBlocksCS, MB_REPAIR_SHADER_PATH, "MBRepScanBlocksCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FMBRepScanAddOffsetsCS, MB_REPAIR_SHADER_PATH, "MBRepScanAddOffsetsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FMBRepRestoreMarkCS, MB_REPAIR_SHADER_PATH, "MBRepRestoreMarkCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FMBRepRestoreExposureCS, MB_REPAIR_SHADER_PATH, "MBRepRestoreExposureCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FMBRepRestoreApplyCS, MB_REPAIR_SHADER_PATH, "MBRepRestoreApplyCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FMBRepDedupCS, MB_REPAIR_SHADER_PATH, "MBRepDedupCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FMBRepBoundaryCS, MB_REPAIR_SHADER_PATH, "MBRepBoundaryCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FMBRepSlitArgsCS, MB_REPAIR_SHADER_PATH, "MBRepSlitArgsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FMBRepSlitLoopCS, MB_REPAIR_SHADER_PATH, "MBRepSlitLoopCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FMBRepLoopPushCS, MB_REPAIR_SHADER_PATH, "MBRepLoopPushCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FMBRepCountOpenCS, MB_REPAIR_SHADER_PATH, "MBRepCountOpenCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FMBRepNudgeClaimCS, MB_REPAIR_SHADER_PATH, "MBRepNudgeClaimCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FMBRepFinalCheckCS, MB_REPAIR_SHADER_PATH, "MBRepFinalCheckCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FMBRepFinalizeStatsCS, MB_REPAIR_SHADER_PATH, "MBRepFinalizeStatsCS", SF_Compute);
#undef MB_REPAIR_SHADER_PATH

namespace
{
	FRDGBufferRef MBRepCreateUintBuffer(FRDGBuilder& GraphBuilder, uint32 NumElements, const TCHAR* Name)
	{
		return GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), FMath::Max(1u, NumElements)), Name);
	}

	FRDGBufferSRVRef MBRepUintSRV(FRDGBuilder& GraphBuilder, FRDGBufferRef Buffer)
	{
		return GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Buffer, PF_R32_UINT));
	}

	FRDGBufferUAVRef MBRepUintUAV(FRDGBuilder& GraphBuilder, FRDGBufferRef Buffer)
	{
		return GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Buffer, PF_R32_UINT));
	}

	/** Exclusive prefix sum of Count uints into Output, recursing over the block totals. */
	void MBRepAddScan(FRDGBuilder& GraphBuilder, FRDGBufferSRVRef Input, FRDGBufferRef Output, uint32 Count)
	{
		if (Count == 0u) return;
		const uint32 Blocks = FMath::DivideAndRoundUp(Count, RepairScanBlock);
		FRDGBufferRef BlockSums = MBRepCreateUintBuffer(GraphBuilder, Blocks, TEXT("MB.Repair.ScanBlockSums"));
		{
			FMBRepScanBlocksCS::FParameters* P = GraphBuilder.AllocParameters<FMBRepScanBlocksCS::FParameters>();
			P->MBRepScanInput = Input;
			P->RW_MBRepScanOutput = MBRepUintUAV(GraphBuilder, Output);
			P->RW_MBRepScanBlockSums = MBRepUintUAV(GraphBuilder, BlockSums);
			P->MBRepScanCount = Count;
			TShaderMapRef<FMBRepScanBlocksCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.Repair.ScanBlocks"), S, P,
				FComputeShaderUtils::GetGroupCountWrapped(int32(Blocks)));
		}
		if (Blocks <= 1u) return;

		FRDGBufferRef BlockOffsets = MBRepCreateUintBuffer(GraphBuilder, Blocks, TEXT("MB.Repair.ScanBlockOffsets"));
		MBRepAddScan(GraphBuilder, MBRepUintSRV(GraphBuilder, BlockSums), BlockOffsets, Blocks);
		{
			FMBRepScanAddOffsetsCS::FParameters* P = GraphBuilder.AllocParameters<FMBRepScanAddOffsetsCS::FParameters>();
			P->MBRepScanBlockOffsets = MBRepUintSRV(GraphBuilder, BlockOffsets);
			P->RW_MBRepScanOutput = MBRepUintUAV(GraphBuilder, Output);
			P->MBRepScanCount = Count;
			TShaderMapRef<FMBRepScanAddOffsetsCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.Repair.ScanAddOffsets"), S, P,
				FComputeShaderUtils::GetGroupCountWrapped(int32(Blocks)));
		}
	}
}

uint32 MeshBooleanRepair::FillCapacity(uint32 FragmentCount)
{
	// A slit of n open edges takes n - 2 fills and open edges are a small fraction of all
	// edges in anything the weld did not tear apart; overflow is counted, never written.
	return uint32(FMath::Clamp<int64>(int64(FragmentCount) / 4, 1024, 1 << 22));
}

MeshBooleanRepair::FOutputs MeshBooleanRepair::AddRepairPasses(
	FRDGBuilder& GraphBuilder, const FInputs& Inputs, const FSettings& Settings)
{
	FOutputs Out;
	const uint32 FragmentCount = Inputs.FragmentCount;
	if (!Inputs.FragmentSoup || !Inputs.FragmentSource || !Inputs.SourceVertices || FragmentCount == 0u) return Out;
	RDG_EVENT_SCOPE(GraphBuilder, "MB.Repair");

	const uint32 CornerCount = FragmentCount * 3u;
	const uint32 FillCap = FillCapacity(FragmentCount);
	const FIntVector FragmentGroups = FComputeShaderUtils::GetGroupCountWrapped(int32(FragmentCount), 64);
	const FIntVector TriangleGroups = FComputeShaderUtils::GetGroupCountWrapped(int32(FragmentCount + FillCap), 64);

	Out.Stats = MBRepCreateUintBuffer(GraphBuilder, StatCount, TEXT("MB.Repair.Stats"));
	FRDGBufferUAVRef StatsUAV = MBRepUintUAV(GraphBuilder, Out.Stats);
	AddClearUAVPass(GraphBuilder, StatsUAV, 0u);

	// ---- Weld every fragment, dead ones included: restoration needs a deleted fragment to
	//      share its vertices with the live surface it was cut from. ----
	{
		FRDGBufferRef Counter = MBRepCreateUintBuffer(GraphBuilder, 1u, TEXT("MB.Repair.FragmentCounter"));
		AddClearUAVPass(GraphBuilder, MBRepUintUAV(GraphBuilder, Counter), FragmentCount);
		Out.Representatives = CSGpuTriangleUtilities::AddVertexWeldPasses(
			GraphBuilder, Inputs.FragmentSoup, Counter, int32(FragmentCount), int32(Inputs.SourceTriangleCount),
			Settings.GridOrigin, Settings.WeldDistance, nullptr, 0u, StatsUAV);
	}

	FRDGBufferSRVRef SoupSRV = GraphBuilder.CreateSRV(Inputs.FragmentSoup);
	FRDGBufferSRVRef SourceSRV = MBRepUintSRV(GraphBuilder, Inputs.FragmentSource);
	FRDGBufferSRVRef SourceVerticesSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Inputs.SourceVertices, PF_A32B32G32R32F));
	FRDGBufferSRVRef RepsSRV = MBRepUintSRV(GraphBuilder, Out.Representatives);

	Out.Flags = MBRepCreateUintBuffer(GraphBuilder, FragmentCount, TEXT("MB.Repair.Flags"));
	FRDGBufferUAVRef FlagsUAV = MBRepUintUAV(GraphBuilder, Out.Flags);
	{
		FMBRepClassifyCS::FParameters* P = GraphBuilder.AllocParameters<FMBRepClassifyCS::FParameters>();
		P->MBRepSoup = SoupSRV;
		P->MBRepSource = SourceSRV;
		P->MBRepSourceVertices = SourceVerticesSRV;
		P->MBRepReps = RepsSRV;
		P->RW_MBRepFlags = FlagsUAV;
		P->RW_MBRepStats = StatsUAV;
		P->MBRepFragmentCount = FragmentCount;
		P->MBRepSourceCount = Inputs.SourceTriangleCount;
		P->MBRepStageB = Settings.bStageB ? 1u : 0u;
		TShaderMapRef<FMBRepClassifyCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.Repair.Classify"), S, P, FragmentGroups);
	}

	// ---- Representative -> incident fragments ----
	FRDGBufferRef VertexCount = MBRepCreateUintBuffer(GraphBuilder, CornerCount, TEXT("MB.Repair.VertexCount"));
	FRDGBufferRef VertexOffset = MBRepCreateUintBuffer(GraphBuilder, CornerCount, TEXT("MB.Repair.VertexOffset"));
	FRDGBufferRef VertexEntries = MBRepCreateUintBuffer(GraphBuilder, CornerCount, TEXT("MB.Repair.VertexEntries"));
	AddClearUAVPass(GraphBuilder, MBRepUintUAV(GraphBuilder, VertexCount), 0u);
	{
		FMBRepCountCS::FParameters* P = GraphBuilder.AllocParameters<FMBRepCountCS::FParameters>();
		P->MBRepReps = RepsSRV;
		P->MBRepFlags = MBRepUintSRV(GraphBuilder, Out.Flags);
		P->RW_MBRepVertexCount = MBRepUintUAV(GraphBuilder, VertexCount);
		P->MBRepFragmentCount = FragmentCount;
		TShaderMapRef<FMBRepCountCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.Repair.CountVertexFragments"), S, P, FragmentGroups);
	}
	MBRepAddScan(GraphBuilder, MBRepUintSRV(GraphBuilder, VertexCount), VertexOffset, CornerCount);
	// The count doubles as the scatter cursor and ends up holding the counts again.
	AddClearUAVPass(GraphBuilder, MBRepUintUAV(GraphBuilder, VertexCount), 0u);
	{
		FMBRepScatterCS::FParameters* P = GraphBuilder.AllocParameters<FMBRepScatterCS::FParameters>();
		P->MBRepReps = RepsSRV;
		P->MBRepFlags = MBRepUintSRV(GraphBuilder, Out.Flags);
		P->MBRepVertexOffset = MBRepUintSRV(GraphBuilder, VertexOffset);
		P->RW_MBRepVertexCount = MBRepUintUAV(GraphBuilder, VertexCount);
		P->RW_MBRepVertexEntries = MBRepUintUAV(GraphBuilder, VertexEntries);
		P->MBRepFragmentCount = FragmentCount;
		TShaderMapRef<FMBRepScatterCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.Repair.ScatterVertexFragments"), S, P, FragmentGroups);
	}
	FRDGBufferSRVRef VertexOffsetSRV = MBRepUintSRV(GraphBuilder, VertexOffset);
	FRDGBufferSRVRef VertexCountSRV = MBRepUintSRV(GraphBuilder, VertexCount);
	FRDGBufferSRVRef VertexEntriesSRV = MBRepUintSRV(GraphBuilder, VertexEntries);

	auto FillTopology = [&](auto* P)
	{
		P->MBRepSoup = SoupSRV;
		P->MBRepSource = SourceSRV;
		P->MBRepSourceVertices = SourceVerticesSRV;
		P->MBRepReps = RepsSRV;
		P->MBRepVertexOffset = VertexOffsetSRV;
		P->MBRepVertexCount = VertexCountSRV;
		P->MBRepVertexEntries = VertexEntriesSRV;
		P->RW_MBRepFlags = FlagsUAV;
		P->RW_MBRepStats = StatsUAV;
		P->MBRepFragmentCount = FragmentCount;
	};

	auto AddDedup = [&](const TCHAR* Name)
	{
		FMBRepDedupCS::FParameters* P = GraphBuilder.AllocParameters<FMBRepDedupCS::FParameters>();
		FillTopology(P);
		TShaderMapRef<FMBRepDedupCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("%s", Name), S, P, FragmentGroups);
	};

	FRDGBufferRef BoundaryList = MBRepCreateUintBuffer(GraphBuilder, CornerCount, TEXT("MB.Repair.BoundaryList"));
	FRDGBufferRef BoundaryCounter = MBRepCreateUintBuffer(GraphBuilder, 1u, TEXT("MB.Repair.BoundaryCounter"));
	auto AddBoundary = [&](bool bFinal)
	{
		AddClearUAVPass(GraphBuilder, MBRepUintUAV(GraphBuilder, BoundaryCounter), 0u);
		FMBRepBoundaryCS::FParameters* P = GraphBuilder.AllocParameters<FMBRepBoundaryCS::FParameters>();
		FillTopology(P);
		P->RW_MBRepBoundaryList = MBRepUintUAV(GraphBuilder, BoundaryList);
		P->RW_MBRepBoundaryCounter = MBRepUintUAV(GraphBuilder, BoundaryCounter);
		P->MBRepBoundaryCapacity = CornerCount;
		P->MBRepFinalPass = bFinal ? 1u : 0u;
		TShaderMapRef<FMBRepBoundaryCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, bFinal ? RDG_EVENT_NAME("MB.Repair.Boundary") : RDG_EVENT_NAME("MB.Repair.BoundaryBeforeRestore"),
			S, P, FragmentGroups);
	};

	FRDGBufferRef FillCounter = MBRepCreateUintBuffer(GraphBuilder, 1u, TEXT("MB.Repair.FillCounter"));
	AddClearUAVPass(GraphBuilder, MBRepUintUAV(GraphBuilder, FillCounter), 0u);
	Out.Fills = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(FUintVector4), FillCap), TEXT("MB.Repair.Fills"));
	FRDGBufferUAVRef FillsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Out.Fills, PF_R32G32B32A32_UINT));
	// Unwritten slots are never read (every reader stops at the fill counter), but a
	// deterministic buffer is cheaper to debug than pool garbage.
	AddClearUAVPass(GraphBuilder, FillsUAV, RepairNone);
	// A loop of n vertices takes n - 2 fills, so three pushes per fill is ample.
	const uint32 LoopOffsetCap = 3u * FillCap;
	Out.LoopSlots = MBRepCreateUintBuffer(GraphBuilder, CornerCount, TEXT("MB.Repair.LoopSlots"));
	FRDGBufferUAVRef LoopSlotsUAV = MBRepUintUAV(GraphBuilder, Out.LoopSlots);
	AddClearUAVPass(GraphBuilder, LoopSlotsUAV, RepairNone);
	Out.LoopOffsets = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), LoopOffsetCap), TEXT("MB.Repair.LoopOffsets"));
	// Extracted and read by the emit even when no slit is filled.
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Out.LoopOffsets, PF_A32B32G32R32F)), 0.0f);
	FRDGBufferRef LoopOffsetCounter = MBRepCreateUintBuffer(GraphBuilder, 1u, TEXT("MB.Repair.LoopOffsetCounter"));
	AddClearUAVPass(GraphBuilder, MBRepUintUAV(GraphBuilder, LoopOffsetCounter), 0u);
	auto AddSlitLoops = [&](bool bMarkOnly)
	{
		FRDGBufferRef IndirectArgs = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(), TEXT("MB.Repair.SlitArgs"));
		{
			FMBRepSlitArgsCS::FParameters* P = GraphBuilder.AllocParameters<FMBRepSlitArgsCS::FParameters>();
			P->MBRepBoundaryCounterSRV = MBRepUintSRV(GraphBuilder, BoundaryCounter);
			P->RW_MBRepIndirectArgs = MBRepUintUAV(GraphBuilder, IndirectArgs);
			P->MBRepBoundaryCapacity = CornerCount;
			TShaderMapRef<FMBRepSlitArgsCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.Repair.SlitArgs"), S, P, FIntVector(1, 1, 1));
		}
		FMBRepSlitLoopCS::FParameters* P = GraphBuilder.AllocParameters<FMBRepSlitLoopCS::FParameters>();
		P->MBRepSoup = SoupSRV;
		P->MBRepSource = SourceSRV;
		P->MBRepReps = RepsSRV;
		P->MBRepVertexOffset = VertexOffsetSRV;
		P->MBRepVertexCount = VertexCountSRV;
		P->MBRepVertexEntries = VertexEntriesSRV;
		P->MBRepBoundaryList = MBRepUintSRV(GraphBuilder, BoundaryList);
		P->MBRepBoundaryCounterSRV = MBRepUintSRV(GraphBuilder, BoundaryCounter);
		P->RW_MBRepFlags = FlagsUAV;
		P->RW_MBRepStats = StatsUAV;
		P->RW_MBRepFillCounter = MBRepUintUAV(GraphBuilder, FillCounter);
		P->RW_MBRepFills = FillsUAV;
		P->RW_MBRepLoopSlots = LoopSlotsUAV;
		P->MBRepIndirectArgsBuffer = IndirectArgs;
		P->MBRepBoundaryCapacity = CornerCount;
		P->MBRepFillCapacity = FillCap;
		P->MBRepSlitTolerance = Settings.SlitTolerance;
		P->MBRepSlitMarkOnly = bMarkOnly ? 1u : 0u;
		TShaderMapRef<FMBRepSlitLoopCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, bMarkOnly ? RDG_EVENT_NAME("MB.Repair.SlitMark") : RDG_EVENT_NAME("MB.Repair.SlitFill"),
			S, P, IndirectArgs, 0u);
	};

	AddDedup(TEXT("MB.Repair.Dedup"));

	// ---- Hole restoration. Slits are marked first: a crack along a cut seam looks open to
	//      the pairing test, and restoring across it would bring the buried surface back.
	//      Without the Stage B field there is no exposure test, and no restoration. ----
	const bool bRestore = Settings.bStageB && Settings.bRestoreHoles && Settings.RestoreRounds > 0
		&& Inputs.WindingTopology && Inputs.WindingMultipoles && Inputs.Winding.TriangleCount > 0u;
	if (bRestore)
	{
		FRDGBufferSRVRef WindingTopologySRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Inputs.WindingTopology, PF_A32B32G32R32F));
		FRDGBufferSRVRef WindingMultipoleSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Inputs.WindingMultipoles, PF_A32B32G32R32F));
		AddBoundary(false);
		AddSlitLoops(true);
		for (int32 Round = 0; Round < Settings.RestoreRounds; ++Round)
		{
			{
				FMBRepRestoreMarkCS::FParameters* P = GraphBuilder.AllocParameters<FMBRepRestoreMarkCS::FParameters>();
				FillTopology(P);
				P->MBRepRound = uint32(Round);
				P->MBRepMaxBendRadians = Settings.RestoreMaxBendRadians;
				TShaderMapRef<FMBRepRestoreMarkCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.Repair.RestoreMark.%d", Round), S, P, FragmentGroups);
			}
			{
				FMBRepRestoreExposureCS::FParameters* P = GraphBuilder.AllocParameters<FMBRepRestoreExposureCS::FParameters>();
				P->MBRepSoup = SoupSRV;
				P->MBRepSource = SourceSRV;
				P->MBRepSourceVertices = SourceVerticesSRV;
				P->RW_MBRepFlags = FlagsUAV;
				P->WMTopo = WindingTopologySRV;
				P->WMMultipole = WindingMultipoleSRV;
				P->WSoup = SourceVerticesSRV;
				P->WFastTriCount = Inputs.Winding.TriangleCount;
				P->WBetaSq = Inputs.Winding.BetaSq;
				P->MBRepFragmentCount = FragmentCount;
				P->MBRepWindingThreshold = Inputs.Winding.Threshold;
				P->MBRepExposureOffset = Inputs.Winding.SampleOffset;
				TShaderMapRef<FMBRepRestoreExposureCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.Repair.RestoreExposure.%d", Round), S, P, FragmentGroups);
			}
			{
				FMBRepRestoreApplyCS::FParameters* P = GraphBuilder.AllocParameters<FMBRepRestoreApplyCS::FParameters>();
				P->RW_MBRepFlags = FlagsUAV;
				P->RW_MBRepStats = StatsUAV;
				P->MBRepFragmentCount = FragmentCount;
				TShaderMapRef<FMBRepRestoreApplyCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.Repair.RestoreApply.%d", Round), S, P, FragmentGroups);
			}
		}
		AddDedup(TEXT("MB.Repair.DedupRestored"));
	}

	// ---- Open edges of the output, slit fills and their vertex pushes, and what stays open ----
	AddBoundary(true);
	if (Settings.bFillSlits)
	{
		AddSlitLoops(false);
		FMBRepLoopPushCS::FParameters* P = GraphBuilder.AllocParameters<FMBRepLoopPushCS::FParameters>();
		P->MBRepSoup = SoupSRV;
		P->MBRepSource = SourceSRV;
		P->MBRepSourceVertices = SourceVerticesSRV;
		P->MBRepReps = RepsSRV;
		P->MBRepFlags = MBRepUintSRV(GraphBuilder, Out.Flags);
		P->MBRepVertexOffset = VertexOffsetSRV;
		P->MBRepVertexCount = VertexCountSRV;
		P->MBRepVertexEntries = VertexEntriesSRV;
		P->RW_MBRepStats = StatsUAV;
		P->RW_MBRepLoopSlots = LoopSlotsUAV;
		P->RW_MBRepLoopOffsets = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Out.LoopOffsets, PF_A32B32G32R32F));
		P->RW_MBRepLoopOffsetCounter = MBRepUintUAV(GraphBuilder, LoopOffsetCounter);
		P->MBRepFragmentCount = FragmentCount;
		P->MBRepLoopOffsetCapacity = LoopOffsetCap;
		P->MBRepNudgeDistance = Settings.NudgeDistance;
		TShaderMapRef<FMBRepLoopPushCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.Repair.LoopPush"), S, P,
			FComputeShaderUtils::GetGroupCountWrapped(int32(CornerCount), 64));
	}
	{
		FMBRepCountOpenCS::FParameters* P = GraphBuilder.AllocParameters<FMBRepCountOpenCS::FParameters>();
		P->MBRepFlags = MBRepUintSRV(GraphBuilder, Out.Flags);
		P->RW_MBRepStats = StatsUAV;
		P->MBRepFragmentCount = FragmentCount;
		TShaderMapRef<FMBRepCountOpenCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.Repair.CountOpen"), S, P, FragmentGroups);
	}

	// ---- Zero-area nudge ----
	Out.Claims = MBRepCreateUintBuffer(GraphBuilder, CornerCount, TEXT("MB.Repair.NudgeClaims"));
	AddClearUAVPass(GraphBuilder, MBRepUintUAV(GraphBuilder, Out.Claims), RepairNone);
	FRDGBufferSRVRef FillsSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Out.Fills, PF_R32G32B32A32_UINT));
	FRDGBufferSRVRef FillCounterSRV = MBRepUintSRV(GraphBuilder, FillCounter);
	auto FillTriangle = [&](auto* P)
	{
		P->MBRepSoup = SoupSRV;
		P->MBRepSource = SourceSRV;
		P->MBRepSourceVertices = SourceVerticesSRV;
		P->MBRepReps = RepsSRV;
		P->MBRepFlags = MBRepUintSRV(GraphBuilder, Out.Flags);
		P->MBRepLoopSlots = MBRepUintSRV(GraphBuilder, Out.LoopSlots);
		P->MBRepLoopOffsets = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Out.LoopOffsets, PF_A32B32G32R32F));
		P->MBRepFills = FillsSRV;
		P->MBRepFillCounter = FillCounterSRV;
		P->MBRepFragmentCount = FragmentCount;
		P->MBRepSourceCount = Inputs.SourceTriangleCount;
		P->MBRepFillCapacity = FillCap;
		P->MBRepNudgeDistance = Settings.NudgeDistance;
		P->MBRepSlitTolerance = Settings.SlitTolerance;
	};
	if (Settings.bFillSlits)
	{
		FMBRepNudgeClaimCS::FParameters* P = GraphBuilder.AllocParameters<FMBRepNudgeClaimCS::FParameters>();
		FillTriangle(P);
		P->RW_MBRepClaims = MBRepUintUAV(GraphBuilder, Out.Claims);
		TShaderMapRef<FMBRepNudgeClaimCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.Repair.NudgeClaim"), S, P, TriangleGroups);
	}
	{
		FMBRepFinalCheckCS::FParameters* P = GraphBuilder.AllocParameters<FMBRepFinalCheckCS::FParameters>();
		FillTriangle(P);
		P->MBRepClaims = MBRepUintSRV(GraphBuilder, Out.Claims);
		P->RW_MBRepStats = StatsUAV;
		TShaderMapRef<FMBRepFinalCheckCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.Repair.FinalCheck"), S, P, TriangleGroups);
	}
	{
		FMBRepFinalizeStatsCS::FParameters* P = GraphBuilder.AllocParameters<FMBRepFinalizeStatsCS::FParameters>();
		P->MBRepFillCounter = FillCounterSRV;
		P->RW_MBRepStats = StatsUAV;
		P->MBRepFillCapacity = FillCap;
		TShaderMapRef<FMBRepFinalizeStatsCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MB.Repair.FinalizeStats"), S, P, FIntVector(1, 1, 1));
	}
	return Out;
}
