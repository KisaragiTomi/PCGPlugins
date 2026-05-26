// Fill out your copyright notice in the Description page of Project Settings.


#include "GenerateVines.h"

#include "GlobalShader.h"
#include "Landscape.h"
#include "PointFunction.h"
#include "PCGPluginDebug.h"
#include "GeometryAsync.h"
#include "Kismet/KismetSystemLibrary.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "ShaderParameterStruct.h"
#include "ComputeShaderBasicFunction.h"
#include "ComputeShaderGenerateHepler.h"
#include "ComputeShaderMeshGenerator.h"

using namespace UE::Geometry;

#define GV_ENABLE_PERF_LOGS 0
#if GV_ENABLE_PERF_LOGS
#define GV_TIME_SCOPE(Label) PCG_DEBUG_TIME_SCOPE_WITH_PREFIX(TEXT("[GenerateVinesTiming]"), Label)
#else
#define GV_TIME_SCOPE(Label)
#endif

class FSpaceColonizationQueueInitCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSpaceColonizationQueueInitCS);
	SHADER_USE_PARAMETER_STRUCT(FSpaceColonizationQueueInitCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, InitialTargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_TargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int4>, RW_State0)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int4>, RW_State1)
		SHADER_PARAMETER(uint32, TargetCount)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

class FSpaceColonizationQueueMarkSourcesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSpaceColonizationQueueMarkSourcesCS);
	SHADER_USE_PARAMETER_STRUCT(FSpaceColonizationQueueMarkSourcesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, SourcePositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, InitialTargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int4>, RW_State0)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int4>, RW_State1)
		SHADER_PARAMETER(uint32, SourceCount)
		SHADER_PARAMETER(uint32, TargetCount)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

class FSpaceColonizationQueueBuildNeighborsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSpaceColonizationQueueBuildNeighborsCS);
	SHADER_USE_PARAMETER_STRUCT(FSpaceColonizationQueueBuildNeighborsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, InitialTargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_NeighborCounts)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_NeighborIndices)
		SHADER_PARAMETER(uint32, TargetCount)
		SHADER_PARAMETER(uint32, MaxNeighbors)
		SHADER_PARAMETER(float, InfluenceRadius)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

class FSpaceColonizationQueueResetProposalsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSpaceColonizationQueueResetProposalsCS);
	SHADER_USE_PARAMETER_STRUCT(FSpaceColonizationQueueResetProposalsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_ProposalOwners)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_ProposalPositions)
		SHADER_PARAMETER(uint32, TargetCount)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

class FSpaceColonizationQueueProposeCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSpaceColonizationQueueProposeCS);
	SHADER_USE_PARAMETER_STRUCT(FSpaceColonizationQueueProposeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, InitialTargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, TargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, State0)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, State1)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborCounts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborIndices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_ProposalOwners)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_ProposalPositions)
		SHADER_PARAMETER(uint32, TargetCount)
		SHADER_PARAMETER(uint32, MaxNeighbors)
		SHADER_PARAMETER(uint32, Iteration)
		SHADER_PARAMETER(uint32, Activetime)
		SHADER_PARAMETER(float, RandGrow)
		SHADER_PARAMETER(float, Seed)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

class FSpaceColonizationQueueCommitCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSpaceColonizationQueueCommitCS);
	SHADER_USE_PARAMETER_STRUCT(FSpaceColonizationQueueCommitCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ProposalOwners)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, ProposalPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_TargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int4>, RW_State0)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int4>, RW_State1)
		SHADER_PARAMETER(uint32, TargetCount)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

IMPLEMENT_GLOBAL_SHADER(FSpaceColonizationQueueInitCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "InitializeSpaceColonizationQueueCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSpaceColonizationQueueMarkSourcesCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "MarkSpaceColonizationSourcesCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSpaceColonizationQueueBuildNeighborsCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "BuildSpaceColonizationNeighborsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSpaceColonizationQueueResetProposalsCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "ResetSpaceColonizationProposalsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSpaceColonizationQueueProposeCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "ProposeSpaceColonizationGrowthCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSpaceColonizationQueueCommitCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "CommitSpaceColonizationGrowthCS", SF_Compute);

namespace
{
constexpr int32 SpaceColonizationMaxNeighborsPerTarget = 128;

struct FSpaceColonizationGPUState4
{
	int32 X = 0;
	int32 Y = 0;
	int32 Z = 0;
	int32 W = 0;
};

static_assert(sizeof(FSpaceColonizationGPUState4) == 16, "Space colonization GPU state must match HLSL int4.");

constexpr bool bSpaceColonizationStepLogs = true;
constexpr int32 SpaceColonizationStepLogSampleCount = 6;
constexpr uint32 SpaceColonizationInvalidProposalOwner = 0xffffffffu;

struct FSpaceColonizationQueueDebugStats
{
	int32 TargetCount = 0;
	int32 AttractorCount = 0;
	int32 ActiveCount = 0;
	int32 StartCount = 0;
	int32 EndCount = 0;
	int32 PreSetCount = 0;
	int32 NextSetCount = 0;
	int32 InvalidPreCount = 0;
	int32 InvalidNextCount = 0;
	int32 AssociateOwnerCount = 0;
	int32 AssociateLinkCount = 0;
	int32 MaxAssociateCount = 0;
	int32 SpawnTotal = 0;
	int32 SpawnMax = 0;
	int32 BranchTotal = 0;
	int32 BranchMax = 0;
	FVector BoundsMin = FVector::ZeroVector;
	FVector BoundsMax = FVector::ZeroVector;
	FVector AveragePosition = FVector::ZeroVector;
};

struct FSpaceColonizationGrowthDebugEvent
{
	int32 SourceIndex = -1;
	int32 TargetIndex = -1;
	int32 AssociateCount = 0;
	int32 ParentSpawnAfter = 0;
	int32 ParentBranchAfter = 0;
	double MoveDistance = 0.0;
	FVector OldTargetPosition = FVector::ZeroVector;
	FVector NewTargetPosition = FVector::ZeroVector;
};

struct FSpaceColonizationCSIterationDebugSnapshot
{
	TArray<uint32> ResetProposalOwners;
	TArray<uint32> ProposalOwners;
	TArray<FVector4f> TargetPositions;
	TArray<FSpaceColonizationGPUState4> State0;
	TArray<FSpaceColonizationGPUState4> State1;
	bool bResetReadbackSucceeded = false;
	bool bProposalReadbackSucceeded = false;
	bool bStateReadbackSucceeded = false;
};

struct FSpaceColonizationCSDebugData
{
	TArray<FVector4f> InitialTargetPositions;
	TArray<FSpaceColonizationGPUState4> InitialState0;
	TArray<FSpaceColonizationGPUState4> InitialState1;
	TArray<uint32> NeighborCounts;
	TArray<FSpaceColonizationCSIterationDebugSnapshot> IterationSnapshots;
	bool bInitialReadbackSucceeded = false;
	bool bNeighborReadbackSucceeded = false;
};

static FString FormatSpaceColonizationVector(const FVector& Vector)
{
	return FString::Printf(TEXT("(%.2f, %.2f, %.2f)"), Vector.X, Vector.Y, Vector.Z);
}

static FSpaceColonizationQueueDebugStats BuildSpaceColonizationQueueDebugStats(
	const TArray<FVector>& TargetLocations,
	const TArray<FSpaceColonizationAttribute>& SCAttributes)
{
	FSpaceColonizationQueueDebugStats Stats;
	Stats.TargetCount = FMath::Min(TargetLocations.Num(), SCAttributes.Num());
	if (Stats.TargetCount <= 0)
	{
		return Stats;
	}

	FVector BoundsMin(TNumericLimits<double>::Max(), TNumericLimits<double>::Max(), TNumericLimits<double>::Max());
	FVector BoundsMax(-TNumericLimits<double>::Max(), -TNumericLimits<double>::Max(), -TNumericLimits<double>::Max());
	FVector PositionSum = FVector::ZeroVector;

	for (int32 Index = 0; Index < Stats.TargetCount; ++Index)
	{
		const FVector& Position = TargetLocations[Index];
		const FSpaceColonizationAttribute& Attribute = SCAttributes[Index];

		BoundsMin.X = FMath::Min(BoundsMin.X, Position.X);
		BoundsMin.Y = FMath::Min(BoundsMin.Y, Position.Y);
		BoundsMin.Z = FMath::Min(BoundsMin.Z, Position.Z);
		BoundsMax.X = FMath::Max(BoundsMax.X, Position.X);
		BoundsMax.Y = FMath::Max(BoundsMax.Y, Position.Y);
		BoundsMax.Z = FMath::Max(BoundsMax.Z, Position.Z);
		PositionSum += Position;

		Stats.AttractorCount += Attribute.Attractor ? 1 : 0;
		Stats.ActiveCount += Attribute.Attractor ? 0 : 1;
		Stats.StartCount += Attribute.Startpt ? 1 : 0;
		Stats.EndCount += Attribute.End ? 1 : 0;
		Stats.PreSetCount += Attribute.PrePt != -1 ? 1 : 0;
		Stats.NextSetCount += Attribute.NextPt != -1 ? 1 : 0;
		Stats.InvalidPreCount += (Attribute.PrePt < -1 || Attribute.PrePt >= Stats.TargetCount) ? 1 : 0;
		Stats.InvalidNextCount += (Attribute.NextPt < -1 || Attribute.NextPt >= Stats.TargetCount) ? 1 : 0;

		const int32 AssociateCount = Attribute.Associates.Num();
		Stats.AssociateOwnerCount += AssociateCount > 0 ? 1 : 0;
		Stats.AssociateLinkCount += AssociateCount;
		Stats.MaxAssociateCount = FMath::Max(Stats.MaxAssociateCount, AssociateCount);
		Stats.SpawnTotal += Attribute.SpawnCount;
		Stats.SpawnMax = FMath::Max(Stats.SpawnMax, Attribute.SpawnCount);
		Stats.BranchTotal += Attribute.BranchCount;
		Stats.BranchMax = FMath::Max(Stats.BranchMax, Attribute.BranchCount);
	}

	Stats.BoundsMin = BoundsMin;
	Stats.BoundsMax = BoundsMax;
	Stats.AveragePosition = PositionSum / double(Stats.TargetCount);
	return Stats;
}

static FString BuildSpaceColonizationNodeSamples(
	const TArray<FVector>& TargetLocations,
	const TArray<FSpaceColonizationAttribute>& SCAttributes,
	bool bEndSamples)
{
	const int32 TargetCount = FMath::Min(TargetLocations.Num(), SCAttributes.Num());
	FString Samples;
	int32 LoggedCount = 0;
	for (int32 Index = 0; Index < TargetCount && LoggedCount < SpaceColonizationStepLogSampleCount; ++Index)
	{
		const FSpaceColonizationAttribute& Attribute = SCAttributes[Index];
		const bool bUseSample = bEndSamples ? Attribute.End : !Attribute.Attractor;
		if (!bUseSample)
		{
			continue;
		}

		if (!Samples.IsEmpty())
		{
			Samples += TEXT(" | ");
		}
		Samples += FString::Printf(
			TEXT("#%d Pos=%s Pre=%d Next=%d Spawn=%d Branch=%d End=%d Start=%d"),
			Index,
			*FormatSpaceColonizationVector(TargetLocations[Index]),
			Attribute.PrePt,
			Attribute.NextPt,
			Attribute.SpawnCount,
			Attribute.BranchCount,
			Attribute.End ? 1 : 0,
			Attribute.Startpt ? 1 : 0);
		++LoggedCount;
	}

	return Samples.IsEmpty() ? TEXT("none") : Samples;
}

static FString BuildSpaceColonizationAssociateSamples(const TArray<FSpaceColonizationAttribute>& SCAttributes)
{
	FString Samples;
	int32 LoggedCount = 0;
	for (int32 Index = 0; Index < SCAttributes.Num() && LoggedCount < SpaceColonizationStepLogSampleCount; ++Index)
	{
		const TArray<int32>& Associates = SCAttributes[Index].Associates;
		if (Associates.Num() == 0)
		{
			continue;
		}

		FString AssociateList;
		const int32 AssociateSampleCount = FMath::Min(Associates.Num(), SpaceColonizationStepLogSampleCount);
		for (int32 SampleIndex = 0; SampleIndex < AssociateSampleCount; ++SampleIndex)
		{
			if (!AssociateList.IsEmpty())
			{
				AssociateList += TEXT(",");
			}
			AssociateList += FString::FromInt(Associates[SampleIndex]);
		}
		if (Associates.Num() > AssociateSampleCount)
		{
			AssociateList += TEXT(",...");
		}

		if (!Samples.IsEmpty())
		{
			Samples += TEXT(" | ");
		}
		Samples += FString::Printf(TEXT("#%d<=[%s]"), Index, *AssociateList);
		++LoggedCount;
	}

	return Samples.IsEmpty() ? TEXT("none") : Samples;
}

static void LogSpaceColonizationQueueState(
	const TCHAR* Version,
	const TCHAR* Phase,
	int32 IterationIndex,
	const TArray<FVector>& TargetLocations,
	const TArray<FSpaceColonizationAttribute>& SCAttributes)
{
	if (!bSpaceColonizationStepLogs)
	{
		return;
	}

	const FSpaceColonizationQueueDebugStats Stats = BuildSpaceColonizationQueueDebugStats(TargetLocations, SCAttributes);
	UE_LOG(LogTemp, Warning,
		TEXT("[SpaceColonizationStep][%s][%s][Iter=%d] Targets=%d Attractors=%d Active=%d Start=%d End=%d Pre=%d Next=%d InvalidPre=%d InvalidNext=%d SpawnTotal=%d SpawnMax=%d BranchTotal=%d BranchMax=%d AssocOwners=%d AssocLinks=%d AssocMax=%d PosAvg=%s BoundsMin=%s BoundsMax=%s"),
		Version,
		Phase,
		IterationIndex,
		Stats.TargetCount,
		Stats.AttractorCount,
		Stats.ActiveCount,
		Stats.StartCount,
		Stats.EndCount,
		Stats.PreSetCount,
		Stats.NextSetCount,
		Stats.InvalidPreCount,
		Stats.InvalidNextCount,
		Stats.SpawnTotal,
		Stats.SpawnMax,
		Stats.BranchTotal,
		Stats.BranchMax,
		Stats.AssociateOwnerCount,
		Stats.AssociateLinkCount,
		Stats.MaxAssociateCount,
		*FormatSpaceColonizationVector(Stats.AveragePosition),
		*FormatSpaceColonizationVector(Stats.BoundsMin),
		*FormatSpaceColonizationVector(Stats.BoundsMax));

	UE_LOG(LogTemp, Warning,
		TEXT("[SpaceColonizationStep][%s][%s][Iter=%d][Samples] Active=%s | Ends=%s"),
		Version,
		Phase,
		IterationIndex,
		*BuildSpaceColonizationNodeSamples(TargetLocations, SCAttributes, false),
		*BuildSpaceColonizationNodeSamples(TargetLocations, SCAttributes, true));
}

static void LogSpaceColonizationAssociates(
	const TCHAR* Version,
	const TCHAR* Phase,
	int32 IterationIndex,
	const TArray<FSpaceColonizationAttribute>& SCAttributes)
{
	if (!bSpaceColonizationStepLogs)
	{
		return;
	}

	int32 OwnerCount = 0;
	int32 LinkCount = 0;
	int32 MaxAssociateCount = 0;
	for (const FSpaceColonizationAttribute& Attribute : SCAttributes)
	{
		const int32 AssociateCount = Attribute.Associates.Num();
		OwnerCount += AssociateCount > 0 ? 1 : 0;
		LinkCount += AssociateCount;
		MaxAssociateCount = FMath::Max(MaxAssociateCount, AssociateCount);
	}

	UE_LOG(LogTemp, Warning,
		TEXT("[SpaceColonizationStep][%s][%s][Iter=%d] AssocOwners=%d AssocLinks=%d AssocMax=%d Samples=%s"),
		Version,
		Phase,
		IterationIndex,
		OwnerCount,
		LinkCount,
		MaxAssociateCount,
		*BuildSpaceColonizationAssociateSamples(SCAttributes));
}

static void LogSpaceColonizationGrowthEvents(
	const TCHAR* Version,
	const TCHAR* Phase,
	int32 IterationIndex,
	const TArray<FSpaceColonizationGrowthDebugEvent>& GrowthEvents)
{
	if (!bSpaceColonizationStepLogs)
	{
		return;
	}

	double TotalMoveDistance = 0.0;
	double MaxMoveDistance = 0.0;
	FString Samples;
	const int32 SampleCount = FMath::Min(GrowthEvents.Num(), SpaceColonizationStepLogSampleCount);
	for (int32 Index = 0; Index < GrowthEvents.Num(); ++Index)
	{
		const FSpaceColonizationGrowthDebugEvent& Event = GrowthEvents[Index];
		TotalMoveDistance += Event.MoveDistance;
		MaxMoveDistance = FMath::Max(MaxMoveDistance, Event.MoveDistance);
		if (Index < SampleCount)
		{
			if (!Samples.IsEmpty())
			{
				Samples += TEXT(" | ");
			}
			Samples += FString::Printf(
				TEXT("%d->%d Assoc=%d Move=%.2f Old=%s New=%s Spawn=%d Branch=%d"),
				Event.SourceIndex,
				Event.TargetIndex,
				Event.AssociateCount,
				Event.MoveDistance,
				*FormatSpaceColonizationVector(Event.OldTargetPosition),
				*FormatSpaceColonizationVector(Event.NewTargetPosition),
				Event.ParentSpawnAfter,
				Event.ParentBranchAfter);
		}
	}

	UE_LOG(LogTemp, Warning,
		TEXT("[SpaceColonizationStep][%s][%s][Iter=%d] GrowthCount=%d MoveTotal=%.2f MoveMax=%.2f Samples=%s"),
		Version,
		Phase,
		IterationIndex,
		GrowthEvents.Num(),
		TotalMoveDistance,
		MaxMoveDistance,
		Samples.IsEmpty() ? TEXT("none") : *Samples);
}

static void LogSpaceColonizationInput(
	const TCHAR* Version,
	int32 SourceCount,
	int32 TargetCount,
	int32 Iteration,
	int32 Activetime,
	float RandGrow,
	float Seed,
	float InfluenceRadius,
	bool bMultThread)
{
	if (!bSpaceColonizationStepLogs)
	{
		return;
	}

	UE_LOG(LogTemp, Warning,
		TEXT("[SpaceColonizationStep][%s][Input] Sources=%d Targets=%d Iterations=%d Activetime=%d RandGrow=%.3f Seed=%.3f InfluenceRadius=%.3f MultThread=%s"),
		Version,
		SourceCount,
		TargetCount,
		Iteration,
		Activetime,
		RandGrow,
		Seed,
		InfluenceRadius,
		bMultThread ? TEXT("true") : TEXT("false"));
}

static void ConvertSpaceColonizationGPUStateToAttributes(
	const TArray<FVector4f>& TargetPositionData,
	const TArray<FSpaceColonizationGPUState4>& State0Data,
	const TArray<FSpaceColonizationGPUState4>& State1Data,
	TArray<FVector>& OutTargetLocations,
	TArray<FSpaceColonizationAttribute>& OutSCAttributes)
{
	const int32 TargetCount = FMath::Min(TargetPositionData.Num(), FMath::Min(State0Data.Num(), State1Data.Num()));
	OutTargetLocations.SetNum(TargetCount);
	OutSCAttributes.SetNum(TargetCount);
	for (int32 Index = 0; Index < TargetCount; ++Index)
	{
		const FVector4f& Position = TargetPositionData[Index];
		OutTargetLocations[Index] = FVector(Position.X, Position.Y, Position.Z);

		const FSpaceColonizationGPUState4& State0 = State0Data[Index];
		const FSpaceColonizationGPUState4& State1 = State1Data[Index];
		FSpaceColonizationAttribute& Attribute = OutSCAttributes[Index];
		Attribute.Attractor = State0.X != 0;
		Attribute.End = State0.Y != 0;
		Attribute.Startpt = State0.Z != 0;
		Attribute.SpawnCount = State0.W;
		Attribute.Startid = State1.X;
		Attribute.PrePt = State1.Y;
		Attribute.NextPt = State1.Z;
		Attribute.BranchCount = State1.W;
	}
}

static void LogSpaceColonizationProposalOwners(
	const TCHAR* Version,
	const TCHAR* Phase,
	int32 IterationIndex,
	const TArray<uint32>& ProposalOwners)
{
	if (!bSpaceColonizationStepLogs)
	{
		return;
	}

	int32 ProposalCount = 0;
	FString Samples;
	for (int32 TargetIndex = 0; TargetIndex < ProposalOwners.Num(); ++TargetIndex)
	{
		const uint32 SourceIndex = ProposalOwners[TargetIndex];
		if (SourceIndex == SpaceColonizationInvalidProposalOwner)
		{
			continue;
		}

		if (ProposalCount < SpaceColonizationStepLogSampleCount)
		{
			if (!Samples.IsEmpty())
			{
				Samples += TEXT(" | ");
			}
			Samples += FString::Printf(TEXT("%d<=%u"), TargetIndex, SourceIndex);
		}
		++ProposalCount;
	}

	UE_LOG(LogTemp, Warning,
		TEXT("[SpaceColonizationStep][%s][%s][Iter=%d] ProposalCount=%d Samples=%s"),
		Version,
		Phase,
		IterationIndex,
		ProposalCount,
		Samples.IsEmpty() ? TEXT("none") : *Samples);
}

static void LogSpaceColonizationNeighborCounts(const TCHAR* Version, const TArray<uint32>& NeighborCounts)
{
	if (!bSpaceColonizationStepLogs)
	{
		return;
	}

	int32 NonZeroCount = 0;
	uint32 TotalCount = 0;
	uint32 MaxCount = 0;
	FString Samples;
	for (int32 Index = 0; Index < NeighborCounts.Num(); ++Index)
	{
		const uint32 Count = NeighborCounts[Index];
		NonZeroCount += Count > 0 ? 1 : 0;
		TotalCount += Count;
		MaxCount = FMath::Max(MaxCount, Count);
		if (Count > 0 && NonZeroCount <= SpaceColonizationStepLogSampleCount)
		{
			if (!Samples.IsEmpty())
			{
				Samples += TEXT(" | ");
			}
			Samples += FString::Printf(TEXT("#%d=%u"), Index, Count);
		}
	}

	UE_LOG(LogTemp, Warning,
		TEXT("[SpaceColonizationStep][%s][BuildNeighbors] Targets=%d NonZero=%d TotalNeighbors=%u MaxNeighbors=%u Samples=%s"),
		Version,
		NeighborCounts.Num(),
		NonZeroCount,
		TotalCount,
		MaxCount,
		Samples.IsEmpty() ? TEXT("none") : *Samples);
}

static void BuildSpaceColonizationCPUNeighbors(
	const TArray<FVector>& InitialTargetLocations,
	float InfluenceRadius,
	int32 MaxNeighborsPerTarget,
	TArray<uint32>& OutNeighborCounts,
	TArray<int32>& OutNeighborIndices)
{
	const int32 TargetCount = InitialTargetLocations.Num();
	const int32 SafeMaxNeighbors = FMath::Clamp(MaxNeighborsPerTarget, 1, FMath::Max(TargetCount, 1));
	const float Radius = FMath::Max(InfluenceRadius, 1.0f);
	const double RadiusSq = double(Radius) * double(Radius);

	OutNeighborCounts.SetNumZeroed(TargetCount);
	OutNeighborIndices.Init(-1, TargetCount * SafeMaxNeighbors);

	for (int32 Index = 0; Index < TargetCount; ++Index)
	{
		uint32 Count = 0;
		const FVector& Center = InitialTargetLocations[Index];
		for (int32 Candidate = 0; Candidate < TargetCount; ++Candidate)
		{
			if (Candidate == Index)
			{
				continue;
			}

			if (FVector::DistSquared(InitialTargetLocations[Candidate], Center) > RadiusSq)
			{
				continue;
			}

			if (Count < uint32(SafeMaxNeighbors))
			{
				OutNeighborIndices[Index * SafeMaxNeighbors + int32(Count)] = Candidate;
				++Count;
			}
		}
		OutNeighborCounts[Index] = Count;
	}
}

static void PopulateSpaceColonizationAssociatesFromNeighbors(
	const TArray<FVector>& InitialTargetLocations,
	const TArray<uint32>& NeighborCounts,
	const TArray<int32>& NeighborIndices,
	int32 MaxNeighborsPerTarget,
	TArray<FSpaceColonizationAttribute>& SCAttributes)
{
	const int32 TargetCount = FMath::Min(InitialTargetLocations.Num(), SCAttributes.Num());
	const int32 SafeMaxNeighbors = FMath::Max(MaxNeighborsPerTarget, 1);
	for (int32 SourceIndex = 0; SourceIndex < TargetCount; ++SourceIndex)
	{
		if (SCAttributes[SourceIndex].Attractor)
		{
			continue;
		}

		const uint32 NeighborCount = NeighborCounts.IsValidIndex(SourceIndex)
			? FMath::Min(NeighborCounts[SourceIndex], uint32(SafeMaxNeighbors))
			: 0u;
		const int32 NeighborBase = SourceIndex * SafeMaxNeighbors;
		for (uint32 NeighborOffset = 0; NeighborOffset < NeighborCount; ++NeighborOffset)
		{
			const int32 NeighborIndex = NeighborIndices.IsValidIndex(NeighborBase + int32(NeighborOffset))
				? NeighborIndices[NeighborBase + int32(NeighborOffset)]
				: -1;
			if (NeighborIndex < 0 || NeighborIndex >= TargetCount || !SCAttributes[NeighborIndex].Attractor)
			{
				continue;
			}

			SCAttributes[SourceIndex].Associates.Add(NeighborIndex);
		}
	}
}

static bool FindSpaceColonizationNearestAttractorFromNeighbors(
	int32 SourceIndex,
	const TArray<FVector>& InitialTargetLocations,
	const TArray<FSpaceColonizationAttribute>& SCAttributes,
	const TArray<uint32>& NeighborCounts,
	const TArray<int32>& NeighborIndices,
	int32 MaxNeighborsPerTarget,
	int32& OutNearAttractorIndex,
	float& OutNearestDistance)
{
	OutNearAttractorIndex = -1;
	OutNearestDistance = 0.0f;

	const int32 TargetCount = FMath::Min(InitialTargetLocations.Num(), SCAttributes.Num());
	if (SourceIndex < 0 || SourceIndex >= TargetCount)
	{
		return false;
	}

	const int32 SafeMaxNeighbors = FMath::Max(MaxNeighborsPerTarget, 1);
	const uint32 NeighborCount = NeighborCounts.IsValidIndex(SourceIndex)
		? FMath::Min(NeighborCounts[SourceIndex], uint32(SafeMaxNeighbors))
		: 0u;
	const int32 NeighborBase = SourceIndex * SafeMaxNeighbors;
	double NearestDistSq = TNumericLimits<double>::Max();
	for (uint32 NeighborOffset = 0; NeighborOffset < NeighborCount; ++NeighborOffset)
	{
		const int32 NeighborIndex = NeighborIndices.IsValidIndex(NeighborBase + int32(NeighborOffset))
			? NeighborIndices[NeighborBase + int32(NeighborOffset)]
			: -1;
		if (NeighborIndex < 0 || NeighborIndex >= TargetCount || !SCAttributes[NeighborIndex].Attractor)
		{
			continue;
		}

		const double DistSq = FVector::DistSquared(InitialTargetLocations[NeighborIndex], InitialTargetLocations[SourceIndex]);
		if (DistSq < NearestDistSq)
		{
			NearestDistSq = DistSq;
			OutNearAttractorIndex = NeighborIndex;
		}
	}

	if (OutNearAttractorIndex == -1)
	{
		return false;
	}

	OutNearestDistance = FMath::Sqrt(float(NearestDistSq));
	return true;
}

static void LogSpaceColonizationCSDebugData(const FSpaceColonizationCSDebugData& DebugData)
{
	if (!bSpaceColonizationStepLogs)
	{
		return;
	}

	TArray<FVector> TargetLocations;
	TArray<FSpaceColonizationAttribute> SCAttributes;
	if (DebugData.bInitialReadbackSucceeded)
	{
		ConvertSpaceColonizationGPUStateToAttributes(
			DebugData.InitialTargetPositions,
			DebugData.InitialState0,
			DebugData.InitialState1,
			TargetLocations,
			SCAttributes);
		LogSpaceColonizationQueueState(TEXT("CS"), TEXT("AfterMarkSources"), -1, TargetLocations, SCAttributes);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[SpaceColonizationStep][CS][AfterMarkSources] Readback failed."));
	}

	if (DebugData.bNeighborReadbackSucceeded)
	{
		LogSpaceColonizationNeighborCounts(TEXT("CS"), DebugData.NeighborCounts);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[SpaceColonizationStep][CS][BuildNeighbors] Readback failed."));
	}

	for (int32 IterationIndex = 0; IterationIndex < DebugData.IterationSnapshots.Num(); ++IterationIndex)
	{
		const FSpaceColonizationCSIterationDebugSnapshot& Snapshot = DebugData.IterationSnapshots[IterationIndex];
		if (Snapshot.bResetReadbackSucceeded)
		{
			LogSpaceColonizationProposalOwners(TEXT("CS"), TEXT("AfterResetProposals"), IterationIndex, Snapshot.ResetProposalOwners);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[SpaceColonizationStep][CS][AfterResetProposals][Iter=%d] Readback failed."), IterationIndex);
		}

		if (Snapshot.bProposalReadbackSucceeded)
		{
			LogSpaceColonizationProposalOwners(TEXT("CS"), TEXT("AfterProposeGrowth"), IterationIndex, Snapshot.ProposalOwners);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[SpaceColonizationStep][CS][AfterProposeGrowth][Iter=%d] Readback failed."), IterationIndex);
		}

		if (Snapshot.bStateReadbackSucceeded)
		{
			ConvertSpaceColonizationGPUStateToAttributes(
				Snapshot.TargetPositions,
				Snapshot.State0,
				Snapshot.State1,
				TargetLocations,
				SCAttributes);
			LogSpaceColonizationQueueState(TEXT("CS"), TEXT("AfterCommitGrowth"), IterationIndex, TargetLocations, SCAttributes);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[SpaceColonizationStep][CS][AfterCommitGrowth][Iter=%d] Readback failed."), IterationIndex);
		}
	}
}

template <typename ElementType>
static bool LockSpaceColonizationReadbackToArray(
	FRHIGPUBufferReadback* Readback,
	uint32 ReadbackBytes,
	int32 ElementCount,
	TArray<ElementType>& OutData)
{
	OutData.SetNumZeroed(ElementCount);
	if (ElementCount <= 0)
	{
		return true;
	}

	if (!Readback)
	{
		return false;
	}

	if (const ElementType* ReadbackPtr = static_cast<const ElementType*>(Readback->Lock(ReadbackBytes)))
	{
		FMemory::Memcpy(OutData.GetData(), ReadbackPtr, ReadbackBytes);
		Readback->Unlock();
		return true;
	}

	return false;
}

static void DeleteSpaceColonizationReadbackArray(TArray<FRHIGPUBufferReadback*>& Readbacks)
{
	for (FRHIGPUBufferReadback*& Readback : Readbacks)
	{
		delete Readback;
		Readback = nullptr;
	}
	Readbacks.Reset();
}

static void DeleteSpaceColonizationCSReadbacks(
	FRHIGPUBufferReadback*& TargetReadback,
	FRHIGPUBufferReadback*& State0Readback,
	FRHIGPUBufferReadback*& State1Readback,
	FRHIGPUBufferReadback*& InitialTargetDebugReadback,
	FRHIGPUBufferReadback*& InitialState0DebugReadback,
	FRHIGPUBufferReadback*& InitialState1DebugReadback,
	FRHIGPUBufferReadback*& NeighborCountsDebugReadback,
	TArray<FRHIGPUBufferReadback*>& ResetProposalOwnerDebugReadbacks,
	TArray<FRHIGPUBufferReadback*>& ProposalOwnerDebugReadbacks,
	TArray<FRHIGPUBufferReadback*>& IterationTargetDebugReadbacks,
	TArray<FRHIGPUBufferReadback*>& IterationState0DebugReadbacks,
	TArray<FRHIGPUBufferReadback*>& IterationState1DebugReadbacks)
{
	delete TargetReadback;
	delete State0Readback;
	delete State1Readback;
	delete InitialTargetDebugReadback;
	delete InitialState0DebugReadback;
	delete InitialState1DebugReadback;
	delete NeighborCountsDebugReadback;
	TargetReadback = nullptr;
	State0Readback = nullptr;
	State1Readback = nullptr;
	InitialTargetDebugReadback = nullptr;
	InitialState0DebugReadback = nullptr;
	InitialState1DebugReadback = nullptr;
	NeighborCountsDebugReadback = nullptr;
	DeleteSpaceColonizationReadbackArray(ResetProposalOwnerDebugReadbacks);
	DeleteSpaceColonizationReadbackArray(ProposalOwnerDebugReadbacks);
	DeleteSpaceColonizationReadbackArray(IterationTargetDebugReadbacks);
	DeleteSpaceColonizationReadbackArray(IterationState0DebugReadbacks);
	DeleteSpaceColonizationReadbackArray(IterationState1DebugReadbacks);
}

static bool AreSpaceColonizationReadbacksReady(const TArray<FRHIGPUBufferReadback*>& Readbacks)
{
	for (FRHIGPUBufferReadback* Readback : Readbacks)
	{
		if (!Readback || !Readback->IsReady())
		{
			return false;
		}
	}
	return true;
}

static bool AreSpaceColonizationCSReadbacksReady(
	FRHIGPUBufferReadback* TargetReadback,
	FRHIGPUBufferReadback* State0Readback,
	FRHIGPUBufferReadback* State1Readback,
	FRHIGPUBufferReadback* InitialTargetDebugReadback,
	FRHIGPUBufferReadback* InitialState0DebugReadback,
	FRHIGPUBufferReadback* InitialState1DebugReadback,
	FRHIGPUBufferReadback* NeighborCountsDebugReadback,
	const TArray<FRHIGPUBufferReadback*>& ResetProposalOwnerDebugReadbacks,
	const TArray<FRHIGPUBufferReadback*>& ProposalOwnerDebugReadbacks,
	const TArray<FRHIGPUBufferReadback*>& IterationTargetDebugReadbacks,
	const TArray<FRHIGPUBufferReadback*>& IterationState0DebugReadbacks,
	const TArray<FRHIGPUBufferReadback*>& IterationState1DebugReadbacks)
{
	return TargetReadback && TargetReadback->IsReady()
		&& State0Readback && State0Readback->IsReady()
		&& State1Readback && State1Readback->IsReady()
		&& InitialTargetDebugReadback && InitialTargetDebugReadback->IsReady()
		&& InitialState0DebugReadback && InitialState0DebugReadback->IsReady()
		&& InitialState1DebugReadback && InitialState1DebugReadback->IsReady()
		&& NeighborCountsDebugReadback && NeighborCountsDebugReadback->IsReady()
		&& AreSpaceColonizationReadbacksReady(ResetProposalOwnerDebugReadbacks)
		&& AreSpaceColonizationReadbacksReady(ProposalOwnerDebugReadbacks)
		&& AreSpaceColonizationReadbacksReady(IterationTargetDebugReadbacks)
		&& AreSpaceColonizationReadbacksReady(IterationState0DebugReadbacks)
		&& AreSpaceColonizationReadbacksReady(IterationState1DebugReadbacks);
}

static bool BuildSpaceColonizationQueueCSImpl(
	const TArray<FTransform>& SourceTransforms,
	const TArray<FTransform>& InTargetTransforms,
	int32 Iteration,
	int32 Activetime,
	float RandGrow,
	float Seed,
	float InfluenceRadius,
	TArray<FVector>& OutTargetLocations,
	TArray<FSpaceColonizationAttribute>& OutSCAttributes)
{
	GV_TIME_SCOPE(TEXT("SpaceColonizationCS.Queue.Total"));
	OutTargetLocations.Reset();
	OutSCAttributes.Reset();

	const int32 SourceCount = SourceTransforms.Num();
	const int32 TargetCount = InTargetTransforms.Num();
	LogSpaceColonizationInput(TEXT("CS"), SourceCount, TargetCount, Iteration, Activetime, RandGrow, Seed, InfluenceRadius, false);
	if (SourceCount == 0 || TargetCount == 0 || Iteration <= 0)
	{
		return false;
	}

	{
		TArray<FVector4f> SourcePositions;
		TArray<FVector4f> InitialTargetPositions;
		{
			GV_TIME_SCOPE(TEXT("SpaceColonizationCS.Queue.PreparePositions"));
			SourcePositions.Reserve(SourceCount);
			for (const FTransform& Transform : SourceTransforms)
			{
				const FVector Location = Transform.GetLocation();
				SourcePositions.Add(FVector4f((FVector3f)Location, 1.0f));
			}

			InitialTargetPositions.Reserve(TargetCount);
			for (const FTransform& Transform : InTargetTransforms)
			{
				const FVector Location = Transform.GetLocation();
				InitialTargetPositions.Add(FVector4f((FVector3f)Location, 1.0f));
			}
		}

		const uint64 TargetReadbackBytes64 = sizeof(FVector4f) * uint64(TargetCount);
		const uint64 StateReadbackBytes64 = sizeof(FSpaceColonizationGPUState4) * uint64(TargetCount);
		const uint64 UIntReadbackBytes64 = sizeof(uint32) * uint64(TargetCount);
		const int32 MaxNeighborsPerTarget = FMath::Clamp(SpaceColonizationMaxNeighborsPerTarget, 1, TargetCount);
		const uint64 NeighborIndexCount64 = uint64(TargetCount) * uint64(MaxNeighborsPerTarget);
		if (TargetReadbackBytes64 > uint64(TNumericLimits<uint32>::Max()) ||
			StateReadbackBytes64 > uint64(TNumericLimits<uint32>::Max()) ||
			UIntReadbackBytes64 > uint64(TNumericLimits<uint32>::Max()) ||
			NeighborIndexCount64 > uint64(TNumericLimits<uint32>::Max()))
		{
			UE_LOG(LogTemp, Warning, TEXT("[SpaceColonizationQueueCS] GPU request too large. TargetCount=%d MaxNeighbors=%d"), TargetCount, MaxNeighborsPerTarget);
			return false;
		}

		const uint32 TargetReadbackBytes = uint32(TargetReadbackBytes64);
		const uint32 StateReadbackBytes = uint32(StateReadbackBytes64);
		const uint32 UIntReadbackBytes = uint32(UIntReadbackBytes64);
		const uint32 NeighborIndexCount = uint32(NeighborIndexCount64);
		FRHIGPUBufferReadback* TargetReadback = new FRHIGPUBufferReadback(TEXT("SpaceColonizationQueue_TargetReadback"));
		FRHIGPUBufferReadback* State0Readback = new FRHIGPUBufferReadback(TEXT("SpaceColonizationQueue_State0Readback"));
		FRHIGPUBufferReadback* State1Readback = new FRHIGPUBufferReadback(TEXT("SpaceColonizationQueue_State1Readback"));
		FRHIGPUBufferReadback* InitialTargetDebugReadback = new FRHIGPUBufferReadback(TEXT("SpaceColonizationQueue_Debug_InitialTarget"));
		FRHIGPUBufferReadback* InitialState0DebugReadback = new FRHIGPUBufferReadback(TEXT("SpaceColonizationQueue_Debug_InitialState0"));
		FRHIGPUBufferReadback* InitialState1DebugReadback = new FRHIGPUBufferReadback(TEXT("SpaceColonizationQueue_Debug_InitialState1"));
		FRHIGPUBufferReadback* NeighborCountsDebugReadback = new FRHIGPUBufferReadback(TEXT("SpaceColonizationQueue_Debug_NeighborCounts"));
		TArray<FRHIGPUBufferReadback*> ResetProposalOwnerDebugReadbacks;
		TArray<FRHIGPUBufferReadback*> ProposalOwnerDebugReadbacks;
		TArray<FRHIGPUBufferReadback*> IterationTargetDebugReadbacks;
		TArray<FRHIGPUBufferReadback*> IterationState0DebugReadbacks;
		TArray<FRHIGPUBufferReadback*> IterationState1DebugReadbacks;
		ResetProposalOwnerDebugReadbacks.Reserve(Iteration);
		ProposalOwnerDebugReadbacks.Reserve(Iteration);
		IterationTargetDebugReadbacks.Reserve(Iteration);
		IterationState0DebugReadbacks.Reserve(Iteration);
		IterationState1DebugReadbacks.Reserve(Iteration);
		for (int32 IterationIndex = 0; IterationIndex < Iteration; ++IterationIndex)
		{
			ResetProposalOwnerDebugReadbacks.Add(new FRHIGPUBufferReadback(*FString::Printf(TEXT("SpaceColonizationQueue_Debug_ResetOwners_%d"), IterationIndex)));
			ProposalOwnerDebugReadbacks.Add(new FRHIGPUBufferReadback(*FString::Printf(TEXT("SpaceColonizationQueue_Debug_ProposalOwners_%d"), IterationIndex)));
			IterationTargetDebugReadbacks.Add(new FRHIGPUBufferReadback(*FString::Printf(TEXT("SpaceColonizationQueue_Debug_Target_%d"), IterationIndex)));
			IterationState0DebugReadbacks.Add(new FRHIGPUBufferReadback(*FString::Printf(TEXT("SpaceColonizationQueue_Debug_State0_%d"), IterationIndex)));
			IterationState1DebugReadbacks.Add(new FRHIGPUBufferReadback(*FString::Printf(TEXT("SpaceColonizationQueue_Debug_State1_%d"), IterationIndex)));
		}
		FSpaceColonizationCSDebugData CSDebugData;
		CSDebugData.IterationSnapshots.SetNum(Iteration);
		bool bRenderWorkQueued = false;

		{
			GV_TIME_SCOPE(TEXT("SpaceColonizationCS.Queue.DispatchAndFlush"));
			ENQUEUE_RENDER_COMMAND(SpaceColonizationQueueCS)(
				[SourcePositions = MoveTemp(SourcePositions), InitialTargetPositions = MoveTemp(InitialTargetPositions),
				 TargetReadback, State0Readback, State1Readback, TargetReadbackBytes, StateReadbackBytes,
				 UIntReadbackBytes, InitialTargetDebugReadback, InitialState0DebugReadback, InitialState1DebugReadback,
				 NeighborCountsDebugReadback, ResetProposalOwnerDebugReadbacks, ProposalOwnerDebugReadbacks,
				 IterationTargetDebugReadbacks, IterationState0DebugReadbacks, IterationState1DebugReadbacks,
				 TargetCount, SourceCount, Iteration, Activetime, RandGrow, Seed, InfluenceRadius,
				 MaxNeighborsPerTarget, NeighborIndexCount, &bRenderWorkQueued](FRHICommandListImmediate& RHICmdList)
				{
					FRDGBuilder GraphBuilder(RHICmdList);

					CREATE_RDG_STRUCTURED_UPLOAD_SRV(Source, FVector4f, SourcePositions, TEXT("SpaceColonizationQueue_SourcePositions"))
					CREATE_RDG_STRUCTURED_UPLOAD_SRV(InitialTarget, FVector4f, InitialTargetPositions, TEXT("SpaceColonizationQueue_InitialTargetPositions"))
					CREATE_RDG_STRUCTURED_UAV_SRV(Target, FVector4f, TargetCount, TEXT("SpaceColonizationQueue_TargetPositions"))
					CREATE_RDG_STRUCTURED_UAV_SRV(State0, FSpaceColonizationGPUState4, TargetCount, TEXT("SpaceColonizationQueue_State0"))
					CREATE_RDG_STRUCTURED_UAV_SRV(State1, FSpaceColonizationGPUState4, TargetCount, TEXT("SpaceColonizationQueue_State1"))
					CREATE_RDG_STRUCTURED_UAV_SRV(NeighborCounts, uint32, TargetCount, TEXT("SpaceColonizationQueue_NeighborCounts"))
					CREATE_RDG_STRUCTURED_UAV_SRV(NeighborIndices, uint32, NeighborIndexCount, TEXT("SpaceColonizationQueue_NeighborIndices"))
					CREATE_RDG_STRUCTURED_UAV_SRV(ProposalOwners, uint32, TargetCount, TEXT("SpaceColonizationQueue_ProposalOwners"))
					CREATE_RDG_STRUCTURED_UAV_SRV(ProposalPositions, FVector4f, TargetCount, TEXT("SpaceColonizationQueue_ProposalPositions"))

					TShaderMapRef<FSpaceColonizationQueueInitCS> InitShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					FSpaceColonizationQueueInitCS::FParameters* InitParameters = GraphBuilder.AllocParameters<FSpaceColonizationQueueInitCS::FParameters>();
					InitParameters->InitialTargetPositions = InitialTargetSRV;
					InitParameters->RW_TargetPositions = TargetUAV;
					InitParameters->RW_State0 = State0UAV;
					InitParameters->RW_State1 = State1UAV;
					InitParameters->TargetCount = uint32(TargetCount);
					GraphBuilder.AddPass(
						RDG_EVENT_NAME("SpaceColonizationQueue.Init"),
						InitParameters,
						ERDGPassFlags::Compute,
						[InitParameters, InitShader, TargetCount](FRHIComputeCommandList& InRHICmdList)
						{
							FComputeShaderUtils::Dispatch(InRHICmdList, InitShader, *InitParameters, FComputeShaderUtils::GetGroupCount(FIntVector(TargetCount, 1, 1), 64));
						});

					TShaderMapRef<FSpaceColonizationQueueMarkSourcesCS> MarkSourcesShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					FSpaceColonizationQueueMarkSourcesCS::FParameters* MarkParameters = GraphBuilder.AllocParameters<FSpaceColonizationQueueMarkSourcesCS::FParameters>();
					MarkParameters->SourcePositions = SourceSRV;
					MarkParameters->InitialTargetPositions = InitialTargetSRV;
					MarkParameters->RW_State0 = State0UAV;
					MarkParameters->RW_State1 = State1UAV;
					MarkParameters->SourceCount = uint32(SourceCount);
					MarkParameters->TargetCount = uint32(TargetCount);
					GraphBuilder.AddPass(
						RDG_EVENT_NAME("SpaceColonizationQueue.MarkSources"),
						MarkParameters,
						ERDGPassFlags::Compute,
						[MarkParameters, MarkSourcesShader, SourceCount](FRHIComputeCommandList& InRHICmdList)
						{
							FComputeShaderUtils::Dispatch(InRHICmdList, MarkSourcesShader, *MarkParameters, FComputeShaderUtils::GetGroupCount(FIntVector(SourceCount, 1, 1), 64));
						});
					AddEnqueueCopyPass(GraphBuilder, InitialTargetDebugReadback, TargetBuffer, TargetReadbackBytes);
					AddEnqueueCopyPass(GraphBuilder, InitialState0DebugReadback, State0Buffer, StateReadbackBytes);
					AddEnqueueCopyPass(GraphBuilder, InitialState1DebugReadback, State1Buffer, StateReadbackBytes);

					TShaderMapRef<FSpaceColonizationQueueBuildNeighborsCS> BuildNeighborsShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					FSpaceColonizationQueueBuildNeighborsCS::FParameters* BuildNeighborsParameters = GraphBuilder.AllocParameters<FSpaceColonizationQueueBuildNeighborsCS::FParameters>();
					BuildNeighborsParameters->InitialTargetPositions = InitialTargetSRV;
					BuildNeighborsParameters->RW_NeighborCounts = NeighborCountsUAV;
					BuildNeighborsParameters->RW_NeighborIndices = NeighborIndicesUAV;
					BuildNeighborsParameters->TargetCount = uint32(TargetCount);
					BuildNeighborsParameters->MaxNeighbors = uint32(MaxNeighborsPerTarget);
					BuildNeighborsParameters->InfluenceRadius = InfluenceRadius;
					GraphBuilder.AddPass(
						RDG_EVENT_NAME("SpaceColonizationQueue.BuildNeighbors"),
						BuildNeighborsParameters,
						ERDGPassFlags::Compute,
						[BuildNeighborsParameters, BuildNeighborsShader, TargetCount](FRHIComputeCommandList& InRHICmdList)
						{
							FComputeShaderUtils::Dispatch(InRHICmdList, BuildNeighborsShader, *BuildNeighborsParameters, FComputeShaderUtils::GetGroupCount(FIntVector(TargetCount, 1, 1), 64));
						});
					AddEnqueueCopyPass(GraphBuilder, NeighborCountsDebugReadback, NeighborCountsBuffer, UIntReadbackBytes);

					TShaderMapRef<FSpaceColonizationQueueResetProposalsCS> ResetProposalsShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					TShaderMapRef<FSpaceColonizationQueueProposeCS> ProposeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					TShaderMapRef<FSpaceColonizationQueueCommitCS> CommitShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					for (int32 IterationIndex = 0; IterationIndex < Iteration; ++IterationIndex)
					{
						FSpaceColonizationQueueResetProposalsCS::FParameters* ResetParameters = GraphBuilder.AllocParameters<FSpaceColonizationQueueResetProposalsCS::FParameters>();
						ResetParameters->RW_ProposalOwners = ProposalOwnersUAV;
						ResetParameters->RW_ProposalPositions = ProposalPositionsUAV;
						ResetParameters->TargetCount = uint32(TargetCount);
						GraphBuilder.AddPass(
							RDG_EVENT_NAME("SpaceColonizationQueue.ResetProposals"),
							ResetParameters,
							ERDGPassFlags::Compute,
							[ResetParameters, ResetProposalsShader, TargetCount](FRHIComputeCommandList& InRHICmdList)
							{
								FComputeShaderUtils::Dispatch(InRHICmdList, ResetProposalsShader, *ResetParameters, FComputeShaderUtils::GetGroupCount(FIntVector(TargetCount, 1, 1), 64));
							});
						AddEnqueueCopyPass(GraphBuilder, ResetProposalOwnerDebugReadbacks[IterationIndex], ProposalOwnersBuffer, UIntReadbackBytes);

						FSpaceColonizationQueueProposeCS::FParameters* ProposeParameters = GraphBuilder.AllocParameters<FSpaceColonizationQueueProposeCS::FParameters>();
						ProposeParameters->InitialTargetPositions = InitialTargetSRV;
						ProposeParameters->TargetPositions = TargetSRV;
						ProposeParameters->State0 = State0SRV;
						ProposeParameters->State1 = State1SRV;
						ProposeParameters->NeighborCounts = NeighborCountsSRV;
						ProposeParameters->NeighborIndices = NeighborIndicesSRV;
						ProposeParameters->RW_ProposalOwners = ProposalOwnersUAV;
						ProposeParameters->RW_ProposalPositions = ProposalPositionsUAV;
						ProposeParameters->TargetCount = uint32(TargetCount);
						ProposeParameters->MaxNeighbors = uint32(MaxNeighborsPerTarget);
						ProposeParameters->Iteration = uint32(IterationIndex);
						ProposeParameters->Activetime = uint32(FMath::Max(Activetime, 0));
						ProposeParameters->RandGrow = RandGrow;
						ProposeParameters->Seed = Seed;
						GraphBuilder.AddPass(
							RDG_EVENT_NAME("SpaceColonizationQueue.Propose"),
							ProposeParameters,
							ERDGPassFlags::Compute,
							[ProposeParameters, ProposeShader, TargetCount](FRHIComputeCommandList& InRHICmdList)
							{
								FComputeShaderUtils::Dispatch(InRHICmdList, ProposeShader, *ProposeParameters, FComputeShaderUtils::GetGroupCount(FIntVector(TargetCount, 1, 1), 64));
							});
						AddEnqueueCopyPass(GraphBuilder, ProposalOwnerDebugReadbacks[IterationIndex], ProposalOwnersBuffer, UIntReadbackBytes);

						FSpaceColonizationQueueCommitCS::FParameters* CommitParameters = GraphBuilder.AllocParameters<FSpaceColonizationQueueCommitCS::FParameters>();
						CommitParameters->ProposalOwners = ProposalOwnersSRV;
						CommitParameters->ProposalPositions = ProposalPositionsSRV;
						CommitParameters->RW_TargetPositions = TargetUAV;
						CommitParameters->RW_State0 = State0UAV;
						CommitParameters->RW_State1 = State1UAV;
						CommitParameters->TargetCount = uint32(TargetCount);
						GraphBuilder.AddPass(
							RDG_EVENT_NAME("SpaceColonizationQueue.Commit"),
							CommitParameters,
							ERDGPassFlags::Compute,
							[CommitParameters, CommitShader, TargetCount](FRHIComputeCommandList& InRHICmdList)
							{
								FComputeShaderUtils::Dispatch(InRHICmdList, CommitShader, *CommitParameters, FComputeShaderUtils::GetGroupCount(FIntVector(TargetCount, 1, 1), 64));
							});
						AddEnqueueCopyPass(GraphBuilder, IterationTargetDebugReadbacks[IterationIndex], TargetBuffer, TargetReadbackBytes);
						AddEnqueueCopyPass(GraphBuilder, IterationState0DebugReadbacks[IterationIndex], State0Buffer, StateReadbackBytes);
						AddEnqueueCopyPass(GraphBuilder, IterationState1DebugReadbacks[IterationIndex], State1Buffer, StateReadbackBytes);
					}

					AddEnqueueCopyPass(GraphBuilder, TargetReadback, TargetBuffer, TargetReadbackBytes);
					AddEnqueueCopyPass(GraphBuilder, State0Readback, State0Buffer, StateReadbackBytes);
					AddEnqueueCopyPass(GraphBuilder, State1Readback, State1Buffer, StateReadbackBytes);

					GraphBuilder.Execute();
					bRenderWorkQueued = true;
				});

			FlushRenderingCommands();
		}

		if (!bRenderWorkQueued)
		{
			DeleteSpaceColonizationCSReadbacks(
				TargetReadback,
				State0Readback,
				State1Readback,
				InitialTargetDebugReadback,
				InitialState0DebugReadback,
				InitialState1DebugReadback,
				NeighborCountsDebugReadback,
				ResetProposalOwnerDebugReadbacks,
				ProposalOwnerDebugReadbacks,
				IterationTargetDebugReadbacks,
				IterationState0DebugReadbacks,
				IterationState1DebugReadbacks);
			return false;
		}

		TArray<FVector4f> TargetPositionData;
		TArray<FSpaceColonizationGPUState4> State0Data;
		TArray<FSpaceColonizationGPUState4> State1Data;
		TargetPositionData.SetNumZeroed(TargetCount);
		State0Data.SetNumZeroed(TargetCount);
		State1Data.SetNumZeroed(TargetCount);
		bool bReadbackSucceeded = false;

		{
			GV_TIME_SCOPE(TEXT("SpaceColonizationCS.Queue.ReadbackAndFlush"));
			ENQUEUE_RENDER_COMMAND(SpaceColonizationQueueCSReadback)(
				[TargetReadback, State0Readback, State1Readback, InitialTargetDebugReadback, InitialState0DebugReadback, InitialState1DebugReadback,
				 NeighborCountsDebugReadback, ResetProposalOwnerDebugReadbacks, ProposalOwnerDebugReadbacks, IterationTargetDebugReadbacks,
				 IterationState0DebugReadbacks, IterationState1DebugReadbacks, TargetReadbackBytes, StateReadbackBytes, UIntReadbackBytes,
				 TargetCount, &TargetPositionData, &State0Data, &State1Data, &CSDebugData, &bReadbackSucceeded](FRHICommandListImmediate& RHICmdList) mutable
				{
					if (!TargetReadback || !State0Readback || !State1Readback)
					{
						return;
					}

					if (!AreSpaceColonizationCSReadbacksReady(
						TargetReadback,
						State0Readback,
						State1Readback,
						InitialTargetDebugReadback,
						InitialState0DebugReadback,
						InitialState1DebugReadback,
						NeighborCountsDebugReadback,
						ResetProposalOwnerDebugReadbacks,
						ProposalOwnerDebugReadbacks,
						IterationTargetDebugReadbacks,
						IterationState0DebugReadbacks,
						IterationState1DebugReadbacks))
					{
						RHICmdList.SubmitAndBlockUntilGPUIdle();
					}

					if (!AreSpaceColonizationCSReadbacksReady(
						TargetReadback,
						State0Readback,
						State1Readback,
						InitialTargetDebugReadback,
						InitialState0DebugReadback,
						InitialState1DebugReadback,
						NeighborCountsDebugReadback,
						ResetProposalOwnerDebugReadbacks,
						ProposalOwnerDebugReadbacks,
						IterationTargetDebugReadbacks,
						IterationState0DebugReadbacks,
						IterationState1DebugReadbacks))
					{
						UE_LOG(LogTemp, Warning, TEXT("[SpaceColonizationQueueCS] GPU readback was not ready after flush."));
						DeleteSpaceColonizationCSReadbacks(
							TargetReadback,
							State0Readback,
							State1Readback,
							InitialTargetDebugReadback,
							InitialState0DebugReadback,
							InitialState1DebugReadback,
							NeighborCountsDebugReadback,
							ResetProposalOwnerDebugReadbacks,
							ProposalOwnerDebugReadbacks,
							IterationTargetDebugReadbacks,
							IterationState0DebugReadbacks,
							IterationState1DebugReadbacks);
						return;
					}

					bool bLockedAll =
						LockSpaceColonizationReadbackToArray(TargetReadback, TargetReadbackBytes, TargetCount, TargetPositionData) &&
						LockSpaceColonizationReadbackToArray(State0Readback, StateReadbackBytes, TargetCount, State0Data) &&
						LockSpaceColonizationReadbackToArray(State1Readback, StateReadbackBytes, TargetCount, State1Data);

					CSDebugData.bInitialReadbackSucceeded =
						LockSpaceColonizationReadbackToArray(InitialTargetDebugReadback, TargetReadbackBytes, TargetCount, CSDebugData.InitialTargetPositions) &&
						LockSpaceColonizationReadbackToArray(InitialState0DebugReadback, StateReadbackBytes, TargetCount, CSDebugData.InitialState0) &&
						LockSpaceColonizationReadbackToArray(InitialState1DebugReadback, StateReadbackBytes, TargetCount, CSDebugData.InitialState1);
					CSDebugData.bNeighborReadbackSucceeded =
						LockSpaceColonizationReadbackToArray(NeighborCountsDebugReadback, UIntReadbackBytes, TargetCount, CSDebugData.NeighborCounts);

					const int32 SnapshotCount = FMath::Min(CSDebugData.IterationSnapshots.Num(), IterationTargetDebugReadbacks.Num());
					for (int32 IterationIndex = 0; IterationIndex < SnapshotCount; ++IterationIndex)
					{
						FSpaceColonizationCSIterationDebugSnapshot& Snapshot = CSDebugData.IterationSnapshots[IterationIndex];
						Snapshot.bResetReadbackSucceeded = LockSpaceColonizationReadbackToArray(
							ResetProposalOwnerDebugReadbacks[IterationIndex],
							UIntReadbackBytes,
							TargetCount,
							Snapshot.ResetProposalOwners);
						Snapshot.bProposalReadbackSucceeded = LockSpaceColonizationReadbackToArray(
							ProposalOwnerDebugReadbacks[IterationIndex],
							UIntReadbackBytes,
							TargetCount,
							Snapshot.ProposalOwners);
						Snapshot.bStateReadbackSucceeded =
							LockSpaceColonizationReadbackToArray(
								IterationTargetDebugReadbacks[IterationIndex],
								TargetReadbackBytes,
								TargetCount,
								Snapshot.TargetPositions) &&
							LockSpaceColonizationReadbackToArray(
								IterationState0DebugReadbacks[IterationIndex],
								StateReadbackBytes,
								TargetCount,
								Snapshot.State0) &&
							LockSpaceColonizationReadbackToArray(
								IterationState1DebugReadbacks[IterationIndex],
								StateReadbackBytes,
								TargetCount,
								Snapshot.State1);
					}

					DeleteSpaceColonizationCSReadbacks(
						TargetReadback,
						State0Readback,
						State1Readback,
						InitialTargetDebugReadback,
						InitialState0DebugReadback,
						InitialState1DebugReadback,
						NeighborCountsDebugReadback,
						ResetProposalOwnerDebugReadbacks,
						ProposalOwnerDebugReadbacks,
						IterationTargetDebugReadbacks,
						IterationState0DebugReadbacks,
						IterationState1DebugReadbacks);
					bReadbackSucceeded = bLockedAll;
				});

			FlushRenderingCommands();
		}

		if (!bReadbackSucceeded)
		{
			return false;
		}

		{
			GV_TIME_SCOPE(TEXT("SpaceColonizationCS.Queue.CopyResults"));
			ConvertSpaceColonizationGPUStateToAttributes(TargetPositionData, State0Data, State1Data, OutTargetLocations, OutSCAttributes);
		}

		LogSpaceColonizationCSDebugData(CSDebugData);
		return true;
	}

}

// 通用 Space Colonization 队列构建：
// 只更新 TargetLocations 与 SCAttributes 中的 PrePt / NextPt / End / Associates 等关系，
// 不创建 PolyPath、不写 Container、不生成任何最终输出。
static void BuildSpaceColonizationQueueImpl(
	const TArray<FTransform>& SourceTransforms,
	const TArray<FTransform>& InTargetTransforms,
	int32 Iteration,
	int32 Activetime,
	float RandGrow,
	float Seed,
	bool bMultThread,
	TArray<FVector>& OutTargetLocations,
	TArray<FSpaceColonizationAttribute>& OutSCAttributes)
{
	GV_TIME_SCOPE(TEXT("SpaceColonization.QueueCPU.Total"));
	OutTargetLocations.Reset();
	OutSCAttributes.Reset();

	LogSpaceColonizationInput(TEXT("CPU"), SourceTransforms.Num(), InTargetTransforms.Num(), Iteration, Activetime, RandGrow, Seed, 200.0f, bMultThread);

	if (SourceTransforms.Num() == 0 || InTargetTransforms.Num() == 0)
	{
		return;
	}

	TArray<FVector> SourceLocations;
	{
		GV_TIME_SCOPE(TEXT("SpaceColonization.QueueCPU.PreparePositions"));
		SourceLocations.Reserve(SourceTransforms.Num());
		OutTargetLocations.Reserve(InTargetTransforms.Num());
		OutSCAttributes.SetNum(InTargetTransforms.Num());

		for (const FTransform& Transform : SourceTransforms)
		{
			SourceLocations.Add(Transform.GetLocation());
		}

		for (const FTransform& Transform : InTargetTransforms)
		{
			OutTargetLocations.Add(Transform.GetLocation());
		}

		for (const FVector& SourceLocation : SourceLocations)
		{
			const int32 Nearpt = UPointFunction::FindNearPointIteration(OutTargetLocations, SourceLocation);
			if (Nearpt == -1)
			{
				continue;
			}

			OutSCAttributes[Nearpt].Attractor = false;
			OutSCAttributes[Nearpt].Startpt = true;
			OutSCAttributes[Nearpt].Startid = Nearpt;
		}
	}
	LogSpaceColonizationQueueState(TEXT("CPU"), TEXT("AfterMarkSources"), -1, OutTargetLocations, OutSCAttributes);

	float Infrad = 200;
	const int32 ThreadPointNum = 1;
	const int32 NumPt = OutTargetLocations.Num();
	const int32 MaxNeighborsPerTarget = FMath::Clamp(SpaceColonizationMaxNeighborsPerTarget, 1, FMath::Max(NumPt, 1));
	const TArray<FVector> InitialTargetLocations = OutTargetLocations;
	TArray<uint32> NeighborCounts;
	TArray<int32> NeighborIndices;
	BuildSpaceColonizationCPUNeighbors(InitialTargetLocations, Infrad, MaxNeighborsPerTarget, NeighborCounts, NeighborIndices);
	LogSpaceColonizationNeighborCounts(TEXT("CPU"), NeighborCounts);

	double ResetAssociatesMs = 0.0;
	double FindAssociatesMs = 0.0;
	double MergeAssociatesMs = 0.0;
	double ProposeGrowthMs = 0.0;
	double CommitGrowthMs = 0.0;

	for (int32 i = 0; i < Iteration; i++)
	{
		double StageStartSeconds = FPlatformTime::Seconds();
		for (int32 p = 0; p < NumPt; p++)
		{
			OutSCAttributes[p].Associates.Reset();
		}
		ResetAssociatesMs += (FPlatformTime::Seconds() - StageStartSeconds) * 1000.0;
		LogSpaceColonizationAssociates(TEXT("CPU"), TEXT("AfterResetAssociates"), i, OutSCAttributes);

		if (bMultThread)
		{
			SCOPE_CYCLE_COUNTER(STAT_SpaceColonizationMultThread)
			StageStartSeconds = FPlatformTime::Seconds();
			PopulateSpaceColonizationAssociatesFromNeighbors(InitialTargetLocations, NeighborCounts, NeighborIndices, MaxNeighborsPerTarget, OutSCAttributes);
			FindAssociatesMs += (FPlatformTime::Seconds() - StageStartSeconds) * 1000.0;
			LogSpaceColonizationAssociates(TEXT("CPU"), TEXT("AfterFindAssociates"), i, OutSCAttributes);

			StageStartSeconds = FPlatformTime::Seconds();
			TArray<TTuple<FIndex3i, FVector>> ProcessResult = ProcessAsync::ProcessAsync<TTuple<FIndex3i, FVector>>(NumPt, ThreadPointNum, [&](const int32 p)
			{
				TTuple<FIndex3i, FVector> ThreadCalculate;
				ThreadCalculate.Key = FIndex3i(-1, -1, 0);
				const float Grow = FMath::RandRange(0, 1);
				if (OutSCAttributes[p].Attractor == true
					|| OutSCAttributes[p].SpawnCount < i - Activetime
					|| (Grow < RandGrow && i > 10)
					|| OutSCAttributes[p].Associates.Num() == 0
					|| OutSCAttributes[p].BranchCount > 2)
				{
					return ThreadCalculate;
				}

				int32 NearPt = -1;
				float NearestDist = 0.0f;
				if (!FindSpaceColonizationNearestAttractorFromNeighbors(
					p,
					InitialTargetLocations,
					OutSCAttributes,
					NeighborCounts,
					NeighborIndices,
					MaxNeighborsPerTarget,
					NearPt,
					NearestDist))
				{
					return ThreadCalculate;
				}

				FVector DirSum = FVector::ZeroVector;
				for (const int32 Index : OutSCAttributes[p].Associates)
				{
					const FVector Dir = (InitialTargetLocations[Index] - InitialTargetLocations[p]);
					//Dir.Normalize();
					DirSum += Dir;
				}
				DirSum.Normalize();

				ThreadCalculate.Key = FIndex3i(p, NearPt, 1);
				ThreadCalculate.Value = OutTargetLocations[p] + DirSum * NearestDist;

				return ThreadCalculate;
			});
			ProposeGrowthMs += (FPlatformTime::Seconds() - StageStartSeconds) * 1000.0;

			StageStartSeconds = FPlatformTime::Seconds();
			TArray<FSpaceColonizationGrowthDebugEvent> GrowthEvents;
			for (int32 j = 0; j < ProcessResult.Num(); j++)
			{
				if (ProcessResult[j].Key.C == 0)
				{
					continue;
				}

				const int32 p = ProcessResult[j].Key.A;
				const int32 NearPt = ProcessResult[j].Key.B;
				const FVector OldTargetPosition = OutTargetLocations[NearPt];
				const int32 AssociateCount = OutSCAttributes[p].Associates.Num();
				OutSCAttributes[p].SpawnCount += 1;
				OutTargetLocations[NearPt] = ProcessResult[j].Value;
				OutSCAttributes[NearPt].Startid = OutSCAttributes[p].Startid;
				OutSCAttributes[NearPt].SpawnCount = OutSCAttributes[p].SpawnCount;
				OutSCAttributes[NearPt].Attractor = false;
				OutSCAttributes[NearPt].End = true;
				OutSCAttributes[NearPt].BranchCount = 1;
				OutSCAttributes[p].End = false;

				OutSCAttributes[NearPt].PrePt = p;
				OutSCAttributes[p].NextPt = NearPt;
				OutSCAttributes[p].BranchCount += 1;

				FSpaceColonizationGrowthDebugEvent& Event = GrowthEvents.AddDefaulted_GetRef();
				Event.SourceIndex = p;
				Event.TargetIndex = NearPt;
				Event.AssociateCount = AssociateCount;
				Event.ParentSpawnAfter = OutSCAttributes[p].SpawnCount;
				Event.ParentBranchAfter = OutSCAttributes[p].BranchCount;
				Event.OldTargetPosition = OldTargetPosition;
				Event.NewTargetPosition = OutTargetLocations[NearPt];
				Event.MoveDistance = FVector::Dist(OldTargetPosition, OutTargetLocations[NearPt]);
			}
			CommitGrowthMs += (FPlatformTime::Seconds() - StageStartSeconds) * 1000.0;
			LogSpaceColonizationGrowthEvents(TEXT("CPU"), TEXT("AfterProposeGrowth"), i, GrowthEvents);
			LogSpaceColonizationQueueState(TEXT("CPU"), TEXT("AfterCommitGrowth"), i, OutTargetLocations, OutSCAttributes);
		}
		else
		{
			SCOPE_CYCLE_COUNTER(STAT_SpaceColonization)
			StageStartSeconds = FPlatformTime::Seconds();
			PopulateSpaceColonizationAssociatesFromNeighbors(InitialTargetLocations, NeighborCounts, NeighborIndices, MaxNeighborsPerTarget, OutSCAttributes);
			FindAssociatesMs += (FPlatformTime::Seconds() - StageStartSeconds) * 1000.0;
			LogSpaceColonizationAssociates(TEXT("CPU"), TEXT("AfterFindAssociates"), i, OutSCAttributes);

			StageStartSeconds = FPlatformTime::Seconds();
			TArray<FSpaceColonizationGrowthDebugEvent> GrowthEvents;
			for (int32 p = 0; p < NumPt; p++)
			{
				const float Grow = FMath::RandRange(0, 1);
				if (OutSCAttributes[p].Attractor == true
					|| OutSCAttributes[p].SpawnCount < i - Activetime
					|| (Grow < RandGrow && i > 10)
					|| OutSCAttributes[p].Associates.Num() == 0
					|| OutSCAttributes[p].BranchCount > 2)
				{
					continue;
				}

				int32 NearPt = -1;
				float NearestDist = 0.0f;
				if (!FindSpaceColonizationNearestAttractorFromNeighbors(
					p,
					InitialTargetLocations,
					OutSCAttributes,
					NeighborCounts,
					NeighborIndices,
					MaxNeighborsPerTarget,
					NearPt,
					NearestDist))
				{
					continue;
				}

				FVector DirSum = FVector::ZeroVector;
				for (const int32 Index : OutSCAttributes[p].Associates)
				{
					const FVector Dir = (InitialTargetLocations[Index] - InitialTargetLocations[p]);
					//Dir.Normalize();
					DirSum += Dir;
				}
				DirSum.Normalize();

				const FVector OldTargetPosition = OutTargetLocations[NearPt];
				const int32 AssociateCount = OutSCAttributes[p].Associates.Num();
				const FVector NewTargetPosition = OutTargetLocations[p] + DirSum * NearestDist;
				OutSCAttributes[p].SpawnCount += 1;
				OutTargetLocations[NearPt] = NewTargetPosition;
				OutSCAttributes[NearPt].Startid = OutSCAttributes[p].Startid;
				OutSCAttributes[NearPt].SpawnCount = OutSCAttributes[p].SpawnCount;
				OutSCAttributes[NearPt].Attractor = false;
				OutSCAttributes[NearPt].End = true;
				OutSCAttributes[NearPt].BranchCount = 1;
				OutSCAttributes[p].End = false;

				OutSCAttributes[NearPt].PrePt = p;
				OutSCAttributes[p].NextPt = NearPt;
				OutSCAttributes[p].BranchCount += 1;

				FSpaceColonizationGrowthDebugEvent& Event = GrowthEvents.AddDefaulted_GetRef();
				Event.SourceIndex = p;
				Event.TargetIndex = NearPt;
				Event.AssociateCount = AssociateCount;
				Event.ParentSpawnAfter = OutSCAttributes[p].SpawnCount;
				Event.ParentBranchAfter = OutSCAttributes[p].BranchCount;
				Event.OldTargetPosition = OldTargetPosition;
				Event.NewTargetPosition = NewTargetPosition;
				Event.MoveDistance = FVector::Dist(OldTargetPosition, NewTargetPosition);
			}
			CommitGrowthMs += (FPlatformTime::Seconds() - StageStartSeconds) * 1000.0;
			LogSpaceColonizationGrowthEvents(TEXT("CPU"), TEXT("AfterProposeGrowth"), i, GrowthEvents);
			LogSpaceColonizationQueueState(TEXT("CPU"), TEXT("AfterCommitGrowth"), i, OutTargetLocations, OutSCAttributes);
		}
	}

#if GV_ENABLE_PERF_LOGS
	PCG_DEBUG_LOG(LogTemp, Display,
		TEXT("[GenerateVinesTiming] SpaceColonization.QueueCPU.Iterations: Iterations=%d Sources=%d Targets=%d MultThread=%s ResetAssociates=%.3f ms FindAssociates=%.3f ms MergeAssociates=%.3f ms ProposeGrowth=%.3f ms CommitGrowth=%.3f ms"),
		Iteration,
		SourceTransforms.Num(),
		InTargetTransforms.Num(),
		bMultThread ? TEXT("true") : TEXT("false"),
		ResetAssociatesMs,
		FindAssociatesMs,
		MergeAssociatesMs,
		ProposeGrowthMs,
		CommitGrowthMs);
#endif
}

static TArray<FGeometryScriptPolyPath> BuildSpaceColonizationLinesImpl(
	const TArray<FVector>& TargetLocations,
	TArray<FSpaceColonizationAttribute>& SCAttributes,
	int32 BackGrowCount)
{
	GV_TIME_SCOPE(TEXT("SpaceColonization.BuildLines"));
	TArray<FGeometryScriptPolyPath> Lines;
	const int32 NumPt = TargetLocations.Num();
	if (NumPt == 0 || SCAttributes.Num() != NumPt)
	{
		return Lines;
	}

	//CreateLineArray
	for (int32 p = 0; p < NumPt; p++)
	{
		if (SCAttributes[p].End != true)
		{
			continue;
		}

		TArray<FVector> Line;
		int32 LineCount = 0;
		int32 CurrentIndex = p;

		Line.Add(TargetLocations[CurrentIndex]);
		//float LineLength = 0;
		while (SCAttributes[CurrentIndex].PrePt != -1 || LineCount < 100)
		{
			const int32 PreIndex = SCAttributes[CurrentIndex].PrePt;
			if (PreIndex == -1)
			{
				break;
			}

			const float DistancePre = FVector::Dist(TargetLocations[PreIndex], TargetLocations[CurrentIndex]);
			Line.Add(TargetLocations[PreIndex]);
			CurrentIndex = PreIndex;
			LineCount += 1;
			FRandomStream Random(332 + DistancePre);
			const float RandomOffset = Random.FRand();
			SCAttributes[CurrentIndex].BackCount += 1;
			if (SCAttributes[CurrentIndex].BackCount > BackGrowCount)
			{
				break;
			}
			//LineLength += DistancePre;
		}

		if (Line.Num() == 0)
		{
			continue;
		}

		FGeometryScriptPolyPath PolyPath;
		PolyPath.Reset();
		*PolyPath.Path = Line;

		Lines.Add(PolyPath);
	}

	return Lines;
}
}

void UGenerateVines::GenerateVines_L(AVineContainer* Container, FSpaceColonizationOptions SC, float ExtrudeScale, bool Result, bool OutDebugMesh, bool MultThread)
{
	GV_TIME_SCOPE(TEXT("GenerateVines_L.Total"));
	// FLevelEditorViewportClient* SelectedViewport = NULL;
	//
	// for(FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
	// {
	// 	if (!ViewportClient->IsOrtho())
	// 	{
	// 		SelectedViewport = ViewportClient;
	// 	}
	// }
	// FViewport* Viewport =  SelectedViewport->Viewport;
	if (!Container)
		return;

	UDynamicMesh* ContainerMesh = Container->GetDynamicMeshComponent()->GetDynamicMesh();
	{
		GV_TIME_SCOPE(TEXT("GenerateVines_L.ResetContainerMesh"));
		ContainerMesh->Reset();
	}

	TArray<FTransform> TubeSourceTransforms;
	TArray<FTransform> PlaneSourceTransforms;
	TArray<FTransform> TargetTransforms;
//	Container->GrowTarget->GetInstanceTransform(0, TubeSourceTransforms[0], true);

	if (!Container->GrowTarget || !Container->TubeVineSource || !Container->PlaneVineSource)
		return;

	int32 TargetCount = Container->GrowTarget->GetInstanceCount();
	{
		GV_TIME_SCOPE(TEXT("GenerateVines_L.CollectTargetTransforms"));
		TargetTransforms.Reserve(TargetCount);

		for (int32 i = 0; i < TargetCount; i++)
		{
			FTransform Transform;
			Container->GrowTarget->GetInstanceTransform(i, Transform, true);
			TargetTransforms.Add(Transform);
		}
	}

	int32 TubeSourceCount = Container->TubeVineSource->GetInstanceCount();
	{
		GV_TIME_SCOPE(TEXT("GenerateVines_L.CollectTubeSourceTransforms"));
		TubeSourceTransforms.Reserve(TubeSourceCount);

		for (int32 i = 0; i < TubeSourceCount; i++)
		{
			FTransform Transform;
			Container->TubeVineSource->GetInstanceTransform(i, Transform, true);
			TubeSourceTransforms.Add(Transform);
		}
	}

	int32 PlaneSourceCount = Container->PlaneVineSource->GetInstanceCount();
	{
		GV_TIME_SCOPE(TEXT("GenerateVines_L.CollectPlaneSourceTransforms"));
		PlaneSourceTransforms.Reserve(PlaneSourceCount);

		for (int32 i = 0; i < PlaneSourceCount; i++)
		{
			FTransform Transform;
			Container->PlaneVineSource->GetInstanceTransform(i, Transform, true);
			PlaneSourceTransforms.Add(Transform);
		}
	}
	if (TargetCount == 0 || (TubeSourceCount == 0 && PlaneSourceCount == 0))
		return;

	TArray<FTransform> BBoxTransforms;
	TArray<FVector> BBoxVectors;
	{
		GV_TIME_SCOPE(TEXT("GenerateVines_L.BuildBoundsInput"));
		BBoxTransforms.Append(TubeSourceTransforms);
		BBoxTransforms.Append(PlaneSourceTransforms);
		BBoxTransforms.Append(TargetTransforms);
		BBoxVectors.Reserve(BBoxTransforms.Num());
		for (FTransform Transform : BBoxTransforms)
		{
			BBoxVectors.Add(Transform.GetLocation());
		}
	}

	FBox Bounds(BBoxVectors);
	Bounds = Bounds.ExpandBy(50);
	FVector Center = Bounds.GetCenter();
	FVector Extent = Bounds.GetExtent();
	TArray<TEnumAsByte<EObjectTypeQuery>> ObjectTypes = {
		UEngineTypes::ConvertToObjectType(ECollisionChannel::ECC_WorldStatic)
	};
	TArray<AActor*> OverlapActors;
	TArray<AActor*> ActorsToIgnore;
	TArray<AActor*> MeshActors;
	{
		GV_TIME_SCOPE(TEXT("GenerateVines_L.BoxOverlapAndFilterActors"));
		UKismetSystemLibrary::BoxOverlapActors(GWorld, Center, Extent, ObjectTypes, nullptr, ActorsToIgnore, OverlapActors);
		for (AActor* Actor : OverlapActors)
		{
			if (!Cast<ALandscape>(Actor) && !Cast<ALandscapeProxy>(Actor))
			{
				MeshActors.Add(Actor);
			}
		}
	}

	{
		GV_TIME_SCOPE(TEXT("GenerateVines_L.EnsureTriangleCache"));
		Container->VoxelGridSettings.VoxelSize = SC.VoxelSize;
		Container->VoxelGridSettings.ActivationRadius = SC.VoxelSize * 8.0f;
		Container->ReferencePoints = BBoxVectors;
		FCSMeshGeneratorTriangleCacheHandle TriangleCacheHandle = Container->EnsureTriangleCacheByBox(
			TEXT("VineGenerate"),
			Center,
			Extent,
			false);
		(void)TriangleCacheHandle;
	}
	FGeometryScriptDynamicMeshBVH BVH;
	UDynamicMesh* MeshCombine = nullptr;
	// if (Container->BVH.Spatial.IsValid() == false || Container->PrefixMesh == nullptr || !Container->
	// 	CheckActors(MeshActors) || Container->InstanceBound != Bounds)
	// {
	{
		GV_TIME_SCOPE(TEXT("GenerateVines_L.VDBMeshFromActors"));
		MeshCombine = UGeometryGenerate::VDBMeshFromActors(MeshActors, BBoxVectors, (OutDebugMesh?false:true), SC.ExtentPlus, SC.VoxelSize, ExtrudeScale, MultThread);
	}
	{
		GV_TIME_SCOPE(TEXT("GenerateVines_L.BuildBVHForMesh"));
		MeshCombine = UGeometryScriptLibrary_MeshSpatial::BuildBVHForMesh(MeshCombine, BVH, nullptr);
	}
	Container->BVH = BVH;
	Container->PrefixMesh = MeshCombine;
	Container->InstanceBound = Bounds;
	Container->PickActors = MeshActors;
	// }
	// else
	// {
	// 	BVH = Container->BVH;
	// 	MeshCombine = Container->PrefixMesh;
	// }
	if (OutDebugMesh || !Result)
	{
		if (!MeshCombine)
		{
			return;
		}
		FDynamicMesh3 MeshCopy;
		{
			GV_TIME_SCOPE(TEXT("GenerateVines_L.CopyDebugMesh"));
			MeshCombine->ProcessMesh([&](const FDynamicMesh3& EditMesh)
			{
				MeshCopy = EditMesh;
			});

			ContainerMesh->SetMesh(MoveTemp(MeshCopy));
		}
		return;
	}

	TArray<FGeometryScriptPolyPath> Lines;
	//TubeLines
	{
		GV_TIME_SCOPE(TEXT("GenerateVines_L.GenerateTubeLines"));
		for (int32 i = 0; i < TubeSourceCount; i++)
		{
			TArray<FTransform> SCSourceTransform;
			SCSourceTransform.Add(TubeSourceTransforms[i]);
			Lines.Append(SpaceColonization(SCSourceTransform, TargetTransforms, SC.Iteration, SC.Activetime, 5, SC.RandGrow, SC.Seed, SC.BackGrowRange, MultThread));
		}
	}
	UDynamicMesh* OutMesh = Container->GetDynamicMeshComponent()->GetDynamicMesh();
	{
		GV_TIME_SCOPE(TEXT("GenerateVines_L.ResetOutputMesh"));
		OutMesh->Reset();
	}

	Container->TubeLines.Reset();
	Container->TubeLines = Lines;
	{
		GV_TIME_SCOPE(TEXT("GenerateVines_L.VisTubeVine"));
		Container->VisVine(true);
	}

	//PlaneLines
	Lines.Reset();
	TArray<FGeometryScriptPolyPath> LinesCS;
	{
		GV_TIME_SCOPE(TEXT("GenerateVines_L.GeneratePlaneLinesCPUAndCS"));
		for (int32 i = 0; i < PlaneSourceCount; i++)
		{
			TArray<FTransform> SCSourceTransform;
			SCSourceTransform.Add(PlaneSourceTransforms[i]);
			Lines.Append(SpaceColonization(SCSourceTransform, TargetTransforms, SC.Iteration, SC.Activetime, 12, SC.RandGrow, SC.Seed, SC.BackGrowRange, MultThread));
			LinesCS.Append(SpaceColonizationCS(SCSourceTransform, TargetTransforms, SC.Iteration, SC.Activetime, 12, SC.RandGrow, SC.Seed, SC.BackGrowRange));
		}
	}
	UE_LOG(LogTemp, Warning, TEXT("[SpaceColonizationCompare][GenerateVines_L.PlaneLines] CPU Lines=%d | CS Lines=%d | Delta Lines=%d"),
		Lines.Num(),
		LinesCS.Num(),
		LinesCS.Num() - Lines.Num());
	Container->PlaneLines.Reset();
	Container->PlaneLines = LinesCS;
	{
		GV_TIME_SCOPE(TEXT("GenerateVines_L.VisPlaneVine"));
		Container->VisVine(false);
	}
}

void UGenerateVines::BuildSpaceColonizationQueue(TArray<FTransform> SourceTransforms, TArray<FTransform> TargetTransforms,
	int32 Iterations, int32 Activetime, float RandGrow, float Seed, bool MultThread,
	TArray<FVector>& OutTargetLocations, TArray<FSpaceColonizationAttribute>& OutSCAttributes)
{
	BuildSpaceColonizationQueueImpl(
		SourceTransforms,
		TargetTransforms,
		Iterations,
		Activetime,
		RandGrow,
		Seed,
		MultThread,
		OutTargetLocations,
		OutSCAttributes);
}

bool UGenerateVines::BuildSpaceColonizationQueueCS(TArray<FTransform> SourceTransforms, TArray<FTransform> TargetTransforms,
	int32 Iterations, int32 Activetime, float RandGrow, float Seed, float InfluenceRadius,
	TArray<FVector>& OutTargetLocations, TArray<FSpaceColonizationAttribute>& OutSCAttributes)
{
	return BuildSpaceColonizationQueueCSImpl(
		SourceTransforms,
		TargetTransforms,
		Iterations,
		Activetime,
		RandGrow,
		Seed,
		InfluenceRadius,
		OutTargetLocations,
		OutSCAttributes);
}

TArray<FGeometryScriptPolyPath> UGenerateVines::SpaceColonization(TArray<FTransform> TubeSourceTransforms, TArray<FTransform> TargetTransforms, int32 Iteration, int32 Activetime, int32 BackGrowCount, float RandGrow, float Seed, float BackGrowRange, bool MultThread)
{
	GV_TIME_SCOPE(TEXT("SpaceColonization.TotalCPU"));
	TArray<FVector> TargetLocations;
	TArray<FSpaceColonizationAttribute> SCAttributes;
	BuildSpaceColonizationQueueImpl(
		TubeSourceTransforms,
		TargetTransforms,
		Iteration,
		Activetime,
		RandGrow,
		Seed,
		MultThread,
		TargetLocations,
		SCAttributes);

	return BuildSpaceColonizationLinesImpl(TargetLocations, SCAttributes, BackGrowCount);
}

TArray<FGeometryScriptPolyPath> UGenerateVines::SpaceColonizationCS(TArray<FTransform> SourceTransforms, TArray<FTransform> TargetTransforms, int32 Iterations, int32 Activetime, int32 BackGrowCount, float Ranggrow, float Seed, float BackGrowRange, float InfluenceRadius)
{
	GV_TIME_SCOPE(TEXT("SpaceColonization.TotalCS"));
	TArray<FVector> TargetLocations;
	TArray<FSpaceColonizationAttribute> SCAttributes;
	if (!BuildSpaceColonizationQueueCSImpl(
		SourceTransforms,
		TargetTransforms,
		Iterations,
		Activetime,
		Ranggrow,
		Seed,
		InfluenceRadius,
		TargetLocations,
		SCAttributes))
	{
		return {};
	}

	return BuildSpaceColonizationLinesImpl(TargetLocations, SCAttributes, BackGrowCount);
}
