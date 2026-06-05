// Fill out your copyright notice in the Description page of Project Settings.


#include "GenerateVines.h"

#include "GlobalShader.h"
#include "PointFunction.h"
#include "PCGPluginDebug.h"
#include "GeometryAsync.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "ShaderParameterStruct.h"
#include "ComputeShaderBasicFunction.h"
#include "ComputeShaderGenerateHepler.h"
#include "ComputeShaderMeshGenerator.h"

using namespace UE::Geometry;

#define GV_ENABLE_PERF_LOGS 1
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
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_TargetPositions)
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
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, InitialTargetPositions)
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

class FSpaceColonizationQueueBuildAxesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSpaceColonizationQueueBuildAxesCS);
	SHADER_USE_PARAMETER_STRUCT(FSpaceColonizationQueueBuildAxesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, TargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, State1)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_Axes)
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

class FSpaceColonizationQueueSmoothAxesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSpaceColonizationQueueSmoothAxesCS);
	SHADER_USE_PARAMETER_STRUCT(FSpaceColonizationQueueSmoothAxesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, State1)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, AxesIn)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_Axes)
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
IMPLEMENT_GLOBAL_SHADER(FSpaceColonizationQueueBuildAxesCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "BuildSpaceColonizationAxesCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSpaceColonizationQueueSmoothAxesCS, "/Plugin/PCGPlugins/Shaders/Private/SpaceColonizationQueue.usf", "SmoothSpaceColonizationAxesCS", SF_Compute);

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

static float GetSpaceColonizationTransformScale(const FTransform& Transform)
{
	const FVector Scale = Transform.GetScale3D();
	return FMath::Max3(FMath::Abs(Scale.X), FMath::Abs(Scale.Y), FMath::Abs(Scale.Z));
}

static void BuildSpaceColonizationScaleLookups(
	const TArray<FTransform>& SourceTransforms,
	const TArray<FTransform>& TargetTransforms,
	TArray<float>& OutTargetPointScales,
	TArray<float>& OutStartSourceScales)
{
	OutTargetPointScales.Reset();
	OutStartSourceScales.Reset();

	const int32 TargetCount = TargetTransforms.Num();
	OutTargetPointScales.Reserve(TargetCount);
	OutStartSourceScales.Init(1.0f, TargetCount);
	if (TargetCount == 0)
	{
		return;
	}

	TArray<FVector> TargetLocations;
	TargetLocations.Reserve(TargetCount);
	for (const FTransform& TargetTransform : TargetTransforms)
	{
		TargetLocations.Add(TargetTransform.GetLocation());
		OutTargetPointScales.Add(GetSpaceColonizationTransformScale(TargetTransform));
	}

	for (const FTransform& SourceTransform : SourceTransforms)
	{
		const int32 NearPointIndex = UPointFunction::FindNearPointIteration(TargetLocations, SourceTransform.GetLocation());
		if (NearPointIndex != -1)
		{
			OutStartSourceScales[NearPointIndex] = GetSpaceColonizationTransformScale(SourceTransform);
		}
	}
}

static float ResolveSpaceColonizationOutputScale(
	int32 TargetIndex,
	const TArray<FSpaceColonizationAttribute>& SCAttributes,
	const TArray<float>& TargetPointScales,
	const TArray<float>& StartSourceScales)
{
	const float TargetPointScale = TargetPointScales.IsValidIndex(TargetIndex) ? TargetPointScales[TargetIndex] : 1.0f;
	const int32 StartId = SCAttributes.IsValidIndex(TargetIndex) ? SCAttributes[TargetIndex].Startid : -1;
	const float SourcePointScale = StartSourceScales.IsValidIndex(StartId) ? StartSourceScales[StartId] : 1.0f;
	return TargetPointScale * SourcePointScale;
}

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
	TArray<FSpaceColonizationAttribute>& OutSCAttributes,
	const TArray<FVector4f>* AxisData = nullptr)
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
		if (AxisData && AxisData->IsValidIndex(Index))
		{
			const FVector4f& Axis = (*AxisData)[Index];
			Attribute.N = FVector(Axis.X, Axis.Y, Axis.Z).GetSafeNormal();
		}
		else
		{
			Attribute.N = FVector::ZeroVector;
		}
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
	struct FNeighborCandidate
	{
		int32 Index = -1;
		double DistSq = 0.0;
	};

	const int32 TargetCount = InitialTargetLocations.Num();
	const int32 SafeMaxNeighbors = FMath::Clamp(MaxNeighborsPerTarget, 1, FMath::Max(TargetCount, 1));
	const float Radius = FMath::Max(InfluenceRadius, 1.0f);
	const double RadiusSq = double(Radius) * double(Radius);

	OutNeighborCounts.SetNumZeroed(TargetCount);
	OutNeighborIndices.Init(-1, TargetCount * SafeMaxNeighbors);

	for (int32 Index = 0; Index < TargetCount; ++Index)
	{
		const FVector& Center = InitialTargetLocations[Index];
		TArray<FNeighborCandidate> Candidates;
		Candidates.Reserve(SafeMaxNeighbors);
		for (int32 Candidate = 0; Candidate < TargetCount; ++Candidate)
		{
			if (Candidate == Index)
			{
				continue;
			}

			const double DistSq = FVector::DistSquared(InitialTargetLocations[Candidate], Center);
			if (DistSq > RadiusSq)
			{
				continue;
			}

			Candidates.Add(FNeighborCandidate{ Candidate, DistSq });
		}

		Candidates.Sort([](const FNeighborCandidate& A, const FNeighborCandidate& B)
		{
			return A.DistSq < B.DistSq;
		});

		const int32 Count = FMath::Min(Candidates.Num(), SafeMaxNeighbors);
		for (int32 NeighborOffset = 0; NeighborOffset < Count; ++NeighborOffset)
		{
			OutNeighborIndices[Index * SafeMaxNeighbors + NeighborOffset] = Candidates[NeighborOffset].Index;
		}
		OutNeighborCounts[Index] = Count;
	}
}

static void PopulateSpaceColonizationAssociatesFromNeighbors(
	const TArray<FVector>& TargetLocations,
	float InfluenceRadius,
	const TArray<uint32>& NeighborCounts,
	const TArray<int32>& NeighborIndices,
	int32 MaxNeighborsPerTarget,
	TArray<FSpaceColonizationAttribute>& SCAttributes)
{
	const int32 TargetCount = FMath::Min(TargetLocations.Num(), SCAttributes.Num());
	const int32 SafeMaxNeighbors = FMath::Max(MaxNeighborsPerTarget, 1);
	for (int32 AttractorIndex = 0; AttractorIndex < TargetCount; ++AttractorIndex)
	{
		if (!SCAttributes[AttractorIndex].Attractor)
		{
			continue;
		}

		int32 NearestSourceIndex = -1;
		double NearestDistSq = TNumericLimits<double>::Max();
		const uint32 NeighborCount = NeighborCounts.IsValidIndex(AttractorIndex)
			? FMath::Min(NeighborCounts[AttractorIndex], uint32(SafeMaxNeighbors))
			: 0u;
		const int32 NeighborBase = AttractorIndex * SafeMaxNeighbors;
		for (uint32 NeighborOffset = 0; NeighborOffset < NeighborCount; ++NeighborOffset)
		{
			const int32 NeighborIndex = NeighborIndices.IsValidIndex(NeighborBase + int32(NeighborOffset))
				? NeighborIndices[NeighborBase + int32(NeighborOffset)]
				: -1;
			if (NeighborIndex < 0 || NeighborIndex >= TargetCount || SCAttributes[NeighborIndex].Attractor)
			{
				continue;
			}

			const double DistSq = FVector::DistSquared(TargetLocations[NeighborIndex], TargetLocations[AttractorIndex]);
			if (DistSq < NearestDistSq)
			{
				NearestDistSq = DistSq;
				NearestSourceIndex = NeighborIndex;
			}
		}

		if (NearestSourceIndex != -1 && FMath::Sqrt(float(NearestDistSq)) * 1.1f < InfluenceRadius)
		{
			SCAttributes[NearestSourceIndex].Associates.Add(AttractorIndex);
		}
	}
}

static bool FindSpaceColonizationNearestAttractorFromNeighbors(
	int32 SourceIndex,
	const TArray<FVector>& TargetLocations,
	const TArray<FSpaceColonizationAttribute>& SCAttributes,
	const TArray<uint32>& NeighborCounts,
	const TArray<int32>& NeighborIndices,
	int32 MaxNeighborsPerTarget,
	int32& OutNearAttractorIndex,
	float& OutNearestDistance)
{
	OutNearAttractorIndex = -1;
	OutNearestDistance = 0.0f;

	const int32 TargetCount = FMath::Min(TargetLocations.Num(), SCAttributes.Num());
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

		const double DistSq = FVector::DistSquared(TargetLocations[NeighborIndex], TargetLocations[SourceIndex]);
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
	FRHIGPUBufferReadback*& AxisReadback,
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
	delete AxisReadback;
	delete InitialTargetDebugReadback;
	delete InitialState0DebugReadback;
	delete InitialState1DebugReadback;
	delete NeighborCountsDebugReadback;
	TargetReadback = nullptr;
	State0Readback = nullptr;
	State1Readback = nullptr;
	AxisReadback = nullptr;
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
	FRHIGPUBufferReadback* AxisReadback,
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
		&& AxisReadback && AxisReadback->IsReady()
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
				SourcePositions.Add(FVector4f((FVector3f)Location, GetSpaceColonizationTransformScale(Transform)));
			}

			InitialTargetPositions.Reserve(TargetCount);
			for (const FTransform& Transform : InTargetTransforms)
			{
				const FVector Location = Transform.GetLocation();
				InitialTargetPositions.Add(FVector4f((FVector3f)Location, GetSpaceColonizationTransformScale(Transform)));
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
		FRHIGPUBufferReadback* AxisReadback = new FRHIGPUBufferReadback(TEXT("SpaceColonizationQueue_AxisReadback"));
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
				 TargetReadback, State0Readback, State1Readback, AxisReadback, TargetReadbackBytes, StateReadbackBytes,
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
					CREATE_RDG_STRUCTURED_UAV_SRV(AxisA, FVector4f, TargetCount, TEXT("SpaceColonizationQueue_AxisA"))
					CREATE_RDG_STRUCTURED_UAV_SRV(AxisB, FVector4f, TargetCount, TEXT("SpaceColonizationQueue_AxisB"))

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
					MarkParameters->RW_TargetPositions = TargetUAV;
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
						CommitParameters->InitialTargetPositions = InitialTargetSRV;
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

					TShaderMapRef<FSpaceColonizationQueueBuildAxesCS> BuildAxesShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					FSpaceColonizationQueueBuildAxesCS::FParameters* BuildAxesParameters = GraphBuilder.AllocParameters<FSpaceColonizationQueueBuildAxesCS::FParameters>();
					BuildAxesParameters->TargetPositions = TargetSRV;
					BuildAxesParameters->State1 = State1SRV;
					BuildAxesParameters->RW_Axes = AxisAUAV;
					BuildAxesParameters->TargetCount = uint32(TargetCount);
					GraphBuilder.AddPass(
						RDG_EVENT_NAME("SpaceColonizationQueue.BuildAxes"),
						BuildAxesParameters,
						ERDGPassFlags::Compute,
						[BuildAxesParameters, BuildAxesShader, TargetCount](FRHIComputeCommandList& InRHICmdList)
						{
							FComputeShaderUtils::Dispatch(InRHICmdList, BuildAxesShader, *BuildAxesParameters, FComputeShaderUtils::GetGroupCount(FIntVector(TargetCount, 1, 1), 64));
						});

					bool bReadAxisA = true;
					TShaderMapRef<FSpaceColonizationQueueSmoothAxesCS> SmoothAxesShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					for (int32 SmoothIterationIndex = 0; SmoothIterationIndex < 3; ++SmoothIterationIndex)
					{
						FSpaceColonizationQueueSmoothAxesCS::FParameters* SmoothAxesParameters = GraphBuilder.AllocParameters<FSpaceColonizationQueueSmoothAxesCS::FParameters>();
						SmoothAxesParameters->State1 = State1SRV;
						SmoothAxesParameters->AxesIn = bReadAxisA ? AxisASRV : AxisBSRV;
						SmoothAxesParameters->RW_Axes = bReadAxisA ? AxisBUAV : AxisAUAV;
						SmoothAxesParameters->TargetCount = uint32(TargetCount);
						GraphBuilder.AddPass(
							RDG_EVENT_NAME("SpaceColonizationQueue.SmoothAxes.%d", SmoothIterationIndex + 1),
							SmoothAxesParameters,
							ERDGPassFlags::Compute,
							[SmoothAxesParameters, SmoothAxesShader, TargetCount](FRHIComputeCommandList& InRHICmdList)
							{
								FComputeShaderUtils::Dispatch(InRHICmdList, SmoothAxesShader, *SmoothAxesParameters, FComputeShaderUtils::GetGroupCount(FIntVector(TargetCount, 1, 1), 64));
							});
						bReadAxisA = !bReadAxisA;
					}

					AddEnqueueCopyPass(GraphBuilder, TargetReadback, TargetBuffer, TargetReadbackBytes);
					AddEnqueueCopyPass(GraphBuilder, State0Readback, State0Buffer, StateReadbackBytes);
					AddEnqueueCopyPass(GraphBuilder, State1Readback, State1Buffer, StateReadbackBytes);
					AddEnqueueCopyPass(GraphBuilder, AxisReadback, bReadAxisA ? AxisABuffer : AxisBBuffer, TargetReadbackBytes);

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
				AxisReadback,
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
		TArray<FVector4f> AxisData;
		TArray<FSpaceColonizationGPUState4> State0Data;
		TArray<FSpaceColonizationGPUState4> State1Data;
		TargetPositionData.SetNumZeroed(TargetCount);
		AxisData.SetNumZeroed(TargetCount);
		State0Data.SetNumZeroed(TargetCount);
		State1Data.SetNumZeroed(TargetCount);
		bool bReadbackSucceeded = false;

		{
			GV_TIME_SCOPE(TEXT("SpaceColonizationCS.Queue.ReadbackAndFlush"));
			ENQUEUE_RENDER_COMMAND(SpaceColonizationQueueCSReadback)(
				[TargetReadback, State0Readback, State1Readback, AxisReadback, InitialTargetDebugReadback, InitialState0DebugReadback, InitialState1DebugReadback,
				 NeighborCountsDebugReadback, ResetProposalOwnerDebugReadbacks, ProposalOwnerDebugReadbacks, IterationTargetDebugReadbacks,
				 IterationState0DebugReadbacks, IterationState1DebugReadbacks, TargetReadbackBytes, StateReadbackBytes, UIntReadbackBytes,
				 TargetCount, &TargetPositionData, &AxisData, &State0Data, &State1Data, &CSDebugData, &bReadbackSucceeded](FRHICommandListImmediate& RHICmdList) mutable
				{
					if (!TargetReadback || !State0Readback || !State1Readback || !AxisReadback)
					{
						return;
					}

					if (!AreSpaceColonizationCSReadbacksReady(
						TargetReadback,
						State0Readback,
						State1Readback,
						AxisReadback,
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
						AxisReadback,
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
							AxisReadback,
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
						LockSpaceColonizationReadbackToArray(AxisReadback, TargetReadbackBytes, TargetCount, AxisData) &&
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
						AxisReadback,
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
			ConvertSpaceColonizationGPUStateToAttributes(TargetPositionData, State0Data, State1Data, OutTargetLocations, OutSCAttributes, &AxisData);
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
			PopulateSpaceColonizationAssociatesFromNeighbors(OutTargetLocations, Infrad, NeighborCounts, NeighborIndices, MaxNeighborsPerTarget, OutSCAttributes);
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
					OutTargetLocations,
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
					const FVector Dir = (OutTargetLocations[Index] - OutTargetLocations[p]);
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
			PopulateSpaceColonizationAssociatesFromNeighbors(OutTargetLocations, Infrad, NeighborCounts, NeighborIndices, MaxNeighborsPerTarget, OutSCAttributes);
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
					OutTargetLocations,
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
					const FVector Dir = (OutTargetLocations[Index] - OutTargetLocations[p]);
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

static TArray<FSpaceColonizationLineResult> BuildSpaceColonizationLineResultsImpl(
	const TArray<FVector>& TargetLocations,
	TArray<FSpaceColonizationAttribute>& SCAttributes,
	int32 BackGrowCount,
	const TArray<float>& TargetPointScales,
	const TArray<float>& StartSourceScales)
{
	GV_TIME_SCOPE(TEXT("SpaceColonization.BuildLines"));
	TArray<FSpaceColonizationLineResult> Lines;
	const int32 NumPt = TargetLocations.Num();
	if (NumPt == 0 || SCAttributes.Num() != NumPt)
	{
		return Lines;
	}

	constexpr int32 MaxBacktrackSteps = 100;
	const int32 PreForkAncestorCount = FMath::Clamp(BackGrowCount, 0, MaxBacktrackSteps);
	constexpr int32 PreservedBranchCount = 3;

	TArray<int32> BranchOrderByNode;
	BranchOrderByNode.Init(0, NumPt);
	for (int32 ChildIndex = 0; ChildIndex < NumPt; ++ChildIndex)
	{
		const int32 ParentIndex = SCAttributes[ChildIndex].PrePt;
		if (ParentIndex < 0 || ParentIndex >= NumPt)
		{
			continue;
		}

		int32 BranchOrder = 1;
		for (int32 SiblingIndex = 0; SiblingIndex < NumPt; ++SiblingIndex)
		{
			if (SiblingIndex == ChildIndex || SCAttributes[SiblingIndex].PrePt != ParentIndex)
			{
				continue;
			}

			const int32 SiblingSpawnCount = SCAttributes[SiblingIndex].SpawnCount;
			const int32 ChildSpawnCount = SCAttributes[ChildIndex].SpawnCount;
			if (SiblingSpawnCount < ChildSpawnCount || (SiblingSpawnCount == ChildSpawnCount && SiblingIndex < ChildIndex))
			{
				BranchOrder += 1;
			}
		}
		BranchOrderByNode[ChildIndex] = BranchOrder;
	}

	//CreateLineArray
	for (int32 p = 0; p < NumPt; p++)
	{
		if (SCAttributes[p].End != true)
		{
			continue;
		}

		TArray<FVector> Line;
		TArray<float> LinePointScales;
		TArray<FVector> LinePointAxes;
		TArray<int32> PathNodeIndices;
		int32 LineCount = 0;
		int32 CurrentIndex = p;
		int32 ForkAttenuationStartPathIndex = INDEX_NONE;

		Line.Add(TargetLocations[CurrentIndex]);
		LinePointScales.Add(ResolveSpaceColonizationOutputScale(CurrentIndex, SCAttributes, TargetPointScales, StartSourceScales));
		LinePointAxes.Add(SCAttributes[CurrentIndex].N.GetSafeNormal());
		PathNodeIndices.Add(CurrentIndex);
		//float LineLength = 0;
		while (LineCount < MaxBacktrackSteps)
		{
			const int32 ChildIndex = CurrentIndex;
			const int32 PreIndex = SCAttributes[CurrentIndex].PrePt;
			if (PreIndex < 0 || PreIndex >= NumPt)
			{
				break;
			}

			Line.Add(TargetLocations[PreIndex]);
			LinePointScales.Add(ResolveSpaceColonizationOutputScale(PreIndex, SCAttributes, TargetPointScales, StartSourceScales));
			LinePointAxes.Add(SCAttributes[PreIndex].N.GetSafeNormal());
			CurrentIndex = PreIndex;
			LineCount += 1;
			PathNodeIndices.Add(CurrentIndex);
			if (SCAttributes[CurrentIndex].BranchCount > PreservedBranchCount && BranchOrderByNode[ChildIndex] > PreservedBranchCount)
			{
				ForkAttenuationStartPathIndex = PathNodeIndices.Num() - 1;

				// Keep a fixed number of ancestors before the cutoff fork. Fork ancestors count
				// the same as regular points, so consecutive forks are preserved as points.
				for (int32 PreForkAncestorIndex = 0; PreForkAncestorIndex < PreForkAncestorCount && LineCount < MaxBacktrackSteps; ++PreForkAncestorIndex)
				{
					const int32 BeforeForkIndex = SCAttributes[CurrentIndex].PrePt;
					if (BeforeForkIndex < 0 || BeforeForkIndex >= NumPt)
					{
						break;
					}

					Line.Add(TargetLocations[BeforeForkIndex]);
					LinePointScales.Add(ResolveSpaceColonizationOutputScale(BeforeForkIndex, SCAttributes, TargetPointScales, StartSourceScales));
					LinePointAxes.Add(SCAttributes[BeforeForkIndex].N.GetSafeNormal());
					CurrentIndex = BeforeForkIndex;
					LineCount += 1;
					PathNodeIndices.Add(CurrentIndex);
				}
				break;
			}
			//LineLength += DistancePre;
		}

		if (Line.Num() == 0)
		{
			continue;
		}

		// Only taper the retained root-to-fork connector after a cutoff fork.
		// The independent branch body before the fork should keep its normal thickness.
		const int32 PathPointCount = PathNodeIndices.Num();
		if (ForkAttenuationStartPathIndex != INDEX_NONE)
		{
			const int32 AttenuationStart = FMath::Clamp(ForkAttenuationStartPathIndex, 0, PathPointCount - 1);
			const int32 AttenuationPointCount = PathPointCount - AttenuationStart;
			for (int32 PathIdx = AttenuationStart; PathIdx < PathPointCount; ++PathIdx)
			{
				// Line is built from tip to root: PathIdx=0 is tip, PathIdx=N-1 is rootmost
				const float Alpha = AttenuationPointCount > 1
					? static_cast<float>(PathIdx - AttenuationStart) / static_cast<float>(AttenuationPointCount - 1)
					: 0.0f;
				const float Attenuation = FMath::Lerp(1.0f, 0.1f, Alpha);
				LinePointScales[PathIdx] *= Attenuation;
			}
		}

		FGeometryScriptPolyPath PolyPath;
		PolyPath.Reset();
		*PolyPath.Path = Line;

		FSpaceColonizationLineResult LineResult;
		LineResult.Path = PolyPath;
		LineResult.PointScales = MoveTemp(LinePointScales);
		LineResult.PointAxes = MoveTemp(LinePointAxes);
		Lines.Add(MoveTemp(LineResult));
	}

	return Lines;
}

static TArray<FGeometryScriptPolyPath> ExtractSpaceColonizationPaths(const TArray<FSpaceColonizationLineResult>& LineResults)
{
	TArray<FGeometryScriptPolyPath> Lines;
	Lines.Reserve(LineResults.Num());
	for (const FSpaceColonizationLineResult& LineResult : LineResults)
	{
		Lines.Add(LineResult.Path);
	}
	return Lines;
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
	return ExtractSpaceColonizationPaths(SpaceColonizationWithScales(
		TubeSourceTransforms,
		TargetTransforms,
		Iteration,
		Activetime,
		BackGrowCount,
		RandGrow,
		Seed,
		BackGrowRange,
		MultThread));
}

TArray<FSpaceColonizationLineResult> UGenerateVines::SpaceColonizationWithScales(TArray<FTransform> SourceTransforms, TArray<FTransform> TargetTransforms, int32 Iteration, int32 Activetime, int32 BackGrowCount, float RandGrow, float Seed, float BackGrowRange, bool MultThread)
{
	GV_TIME_SCOPE(TEXT("SpaceColonization.TotalCPU"));
	(void)BackGrowRange;
	TArray<FVector> TargetLocations;
	TArray<FSpaceColonizationAttribute> SCAttributes;
	TArray<float> TargetPointScales;
	TArray<float> StartSourceScales;
	BuildSpaceColonizationScaleLookups(SourceTransforms, TargetTransforms, TargetPointScales, StartSourceScales);
	BuildSpaceColonizationQueueImpl(
		SourceTransforms,
		TargetTransforms,
		Iteration,
		Activetime,
		RandGrow,
		Seed,
		MultThread,
		TargetLocations,
		SCAttributes);

	return BuildSpaceColonizationLineResultsImpl(TargetLocations, SCAttributes, BackGrowCount, TargetPointScales, StartSourceScales);
}

TArray<FGeometryScriptPolyPath> UGenerateVines::SpaceColonizationCS(TArray<FTransform> SourceTransforms, TArray<FTransform> TargetTransforms, int32 Iterations, int32 Activetime, int32 BackGrowCount, float Ranggrow, float Seed, float BackGrowRange, float InfluenceRadius)
{
	return ExtractSpaceColonizationPaths(SpaceColonizationCSWithScales(
		SourceTransforms,
		TargetTransforms,
		Iterations,
		Activetime,
		BackGrowCount,
		Ranggrow,
		Seed,
		BackGrowRange,
		InfluenceRadius));
}

TArray<FSpaceColonizationLineResult> UGenerateVines::SpaceColonizationCSWithScales(TArray<FTransform> SourceTransforms, TArray<FTransform> TargetTransforms, int32 Iterations, int32 Activetime, int32 BackGrowCount, float Ranggrow, float Seed, float BackGrowRange, float InfluenceRadius)
{
	GV_TIME_SCOPE(TEXT("SpaceColonization.TotalCS"));
	(void)BackGrowRange;
	TArray<FVector> TargetLocations;
	TArray<FSpaceColonizationAttribute> SCAttributes;
	TArray<float> TargetPointScales;
	TArray<float> StartSourceScales;
	BuildSpaceColonizationScaleLookups(SourceTransforms, TargetTransforms, TargetPointScales, StartSourceScales);
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

	return BuildSpaceColonizationLineResultsImpl(TargetLocations, SCAttributes, BackGrowCount, TargetPointScales, StartSourceScales);
}
