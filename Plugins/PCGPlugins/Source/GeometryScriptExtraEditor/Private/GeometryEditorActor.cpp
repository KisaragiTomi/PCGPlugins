// Fill out your copyright notice in the Description page of Project Settings.
#include "GeometryEditorActor.h"

#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "GenerateVines.h"
#include "Landscape.h"
#include "ObjectTools.h"
#include "PointFunction.h"
#include "PCGPluginDebug.h"
#include "PackageTools.h"
#include "Engine/Level.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "GeometryScript/MeshBasicEditFunctions.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryGeneral.h"
#include "DynamicMesh/MeshTransforms.h"
#include "ComputeShaderGenerateHepler.h"
#include "GeometryMath/Public/Noise.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "ShaderParameterStruct.h"
#include "Spatial/MeshAABBTree3.h"
#include "Misc/PackageName.h"

#define GV_ACTOR_ENABLE_PERF_LOGS 1
#if GV_ACTOR_ENABLE_PERF_LOGS
#define GV_ACTOR_TIME_SCOPE(Label) PCG_DEBUG_TIME_SCOPE_WITH_PREFIX(TEXT("[GenerateVinesTiming]"), Label)
#else
#define GV_ACTOR_TIME_SCOPE(Label)
#endif

// Voxel-based surface projection shader for GPU-only vine visualization.
class FVineVisualizationVoxelCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVineVisualizationVoxelCS);
	SHADER_USE_PARAMETER_STRUCT(FVineVisualizationVoxelCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPoints)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, PathPointMeta)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, PathPointCurveU)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPointTangents)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPointNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPointSurfaceTargets)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPointSurfaceNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, SegmentMeta)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, VoxelCells)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VoxelHashSlots)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, VoxelNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, VoxelTargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, TargetBucketRanges)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TargetBucketRangeCounts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TargetBucketVoxelIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TargetBucketHashSlots)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_OutVertices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float2>, RW_OutUVs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_OutIndices)
		SHADER_PARAMETER(uint32, PathPointCount)
		SHADER_PARAMETER(uint32, SegmentCount)
		SHADER_PARAMETER(uint32, OutputVertexCount)
		SHADER_PARAMETER(uint32, OutputIndexCount)
		SHADER_PARAMETER(uint32, ProfileCount)
		SHADER_PARAMETER(uint32, bTube)
		SHADER_PARAMETER(float, CircleScale)
		SHADER_PARAMETER(float, LineScale)
		SHADER_PARAMETER(FVector3f, VoxelOrigin)
		SHADER_PARAMETER(float, VoxelSize)
		SHADER_PARAMETER(uint32, VoxelCount)
		SHADER_PARAMETER(uint32, VoxelHashSlotCount)
		SHADER_PARAMETER(FVector3f, TargetBucketOrigin)
		SHADER_PARAMETER(float, TargetBucketSize)
		SHADER_PARAMETER(uint32, TargetBucketCount)
		SHADER_PARAMETER(uint32, TargetBucketHashSlotCount)
		SHADER_PARAMETER(uint32, TargetBucketSearchRadius)
		SHADER_PARAMETER(float, VinesOffset)
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

IMPLEMENT_GLOBAL_SHADER(FVineVisualizationVoxelCS, "/Plugin/PCGPlugins/Shaders/Private/VineVisualizationVoxel.usf", "BuildVineVisualizationVoxelCS", SF_Compute);

class FVineVisualizationVoxelBuildAxesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVineVisualizationVoxelBuildAxesCS);
	SHADER_USE_PARAMETER_STRUCT(FVineVisualizationVoxelBuildAxesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPoints)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPointAxes)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, PathPointMeta)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, VoxelCells)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VoxelHashSlots)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, VoxelNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, VoxelTargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, TargetBucketRanges)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TargetBucketRangeCounts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TargetBucketVoxelIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TargetBucketHashSlots)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_PathPointTangents)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_PathPointNormals)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_PathPointSurfaceTargets)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_PathPointSurfaceNormals)
		SHADER_PARAMETER(uint32, PathPointCount)
		SHADER_PARAMETER(FVector3f, VoxelOrigin)
		SHADER_PARAMETER(float, VoxelSize)
		SHADER_PARAMETER(uint32, VoxelCount)
		SHADER_PARAMETER(uint32, VoxelHashSlotCount)
		SHADER_PARAMETER(FVector3f, TargetBucketOrigin)
		SHADER_PARAMETER(float, TargetBucketSize)
		SHADER_PARAMETER(uint32, TargetBucketCount)
		SHADER_PARAMETER(uint32, TargetBucketHashSlotCount)
		SHADER_PARAMETER(uint32, TargetBucketSearchRadius)
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

IMPLEMENT_GLOBAL_SHADER(FVineVisualizationVoxelBuildAxesCS, "/Plugin/PCGPlugins/Shaders/Private/VineVisualizationVoxel.usf", "BuildVineVisualizationVoxelAxesCS", SF_Compute);

class FVineVisualizationVoxelSmoothProjectionCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVineVisualizationVoxelSmoothProjectionCS);
	SHADER_USE_PARAMETER_STRUCT(FVineVisualizationVoxelSmoothProjectionCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPointSurfaceTargets)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPointSurfaceNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, PathPointMeta)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_PathPointSurfaceTargets)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_PathPointSurfaceNormals)
		SHADER_PARAMETER(uint32, PathPointCount)
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

IMPLEMENT_GLOBAL_SHADER(FVineVisualizationVoxelSmoothProjectionCS, "/Plugin/PCGPlugins/Shaders/Private/VineVisualizationVoxel.usf", "SmoothVineVisualizationVoxelProjectionCS", SF_Compute);

namespace
{
static const FName VineGeneratedStaticMeshActorTag(TEXT("VineGeneratedStaticMeshActor"));

static bool IsVineGeneratedStaticMeshActor(const AActor* Actor)
{
	return Actor && Actor->Tags.Contains(VineGeneratedStaticMeshActorTag);
}

static void TransformDynamicMeshToLocalSpace(UDynamicMesh* Mesh, const FTransform& LocalToWorld)
{
	if (!Mesh || LocalToWorld.Equals(FTransform::Identity))
	{
		return;
	}

	Mesh->EditMesh([&](FDynamicMesh3& EditMesh)
	{
		MeshTransforms::ApplyTransformInverse(EditMesh, FTransformSRT3d(LocalToWorld), true);
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
}

static FGeometryScriptPolyPath ClonePolyPath(const FGeometryScriptPolyPath& Source)
{
	FGeometryScriptPolyPath Result;
	Result.Reset();
	if (Source.Path.IsValid())
	{
		*Result.Path = *Source.Path;
	}
	return Result;
}

static double GetVinePolyPathLength(const FGeometryScriptPolyPath& Line)
{
	if (!Line.Path.IsValid())
	{
		return 0.0;
	}

	const TArray<FVector>& Points = *Line.Path;
	double Length = 0.0;
	for (int32 PointIndex = 1; PointIndex < Points.Num(); ++PointIndex)
	{
		Length += FVector::Dist(Points[PointIndex - 1], Points[PointIndex]);
	}
	return Length;
}

static float GetVineTransformScale(const FTransform& Transform)
{
	const FVector Scale = Transform.GetScale3D();
	return FMath::Max3(FMath::Abs(Scale.X), FMath::Abs(Scale.Y), FMath::Abs(Scale.Z));
}

static uint32 HashQuantizedVineCoordinate(double Value)
{
	const uint64 Quantized = uint64(FMath::RoundToInt64(Value * 1000.0));
	return HashCombine(uint32(Quantized), uint32(Quantized >> 32));
}

static uint32 BuildVineVisualizationRandomSeed(const FVector& SourceLocation, const FVector& PointLocation)
{
	uint32 Seed = 0x9e3779b9u;
	Seed = HashCombine(Seed, HashQuantizedVineCoordinate(SourceLocation.X));
	Seed = HashCombine(Seed, HashQuantizedVineCoordinate(SourceLocation.Y));
	Seed = HashCombine(Seed, HashQuantizedVineCoordinate(SourceLocation.Z));
	Seed = HashCombine(Seed, HashQuantizedVineCoordinate(PointLocation.X));
	Seed = HashCombine(Seed, HashQuantizedVineCoordinate(PointLocation.Y));
	Seed = HashCombine(Seed, HashQuantizedVineCoordinate(PointLocation.Z));
	return Seed == 0u ? 1u : Seed;
}

static FVector GetVineVisualizationSCPointOffset(const FVector& SourceLocation, const FVector& PointLocation)
{
	constexpr float OffsetDistance = 10.0f;
	FRandomStream RandomStream(int32(BuildVineVisualizationRandomSeed(SourceLocation, PointLocation)));
	return RandomStream.VRand() * OffsetDistance;
}

static void ApplyVineVisualizationSCPointOffset(FGeometryScriptPolyPath& Line, const FVector& SourceLocation)
{
	if (!Line.Path.IsValid())
	{
		return;
	}

	for (FVector& Point : *Line.Path)
	{
		Point += GetVineVisualizationSCPointOffset(SourceLocation, Point);
	}
}

static int32 DrawVineSCStageDebugPoints(
	UWorld* World,
	const TArray<FGeometryScriptPolyPath>& Lines,
	const FLinearColor& PointColor,
	float PointSize,
	float Duration,
	bool bPersistent,
	int32 PointLimit)
{
	if (!World || Lines.Num() == 0)
	{
		return 0;
	}

	const float SafePointSize = FMath::Max(PointSize, 0.0f);
	const float SafeDuration = FMath::Max(Duration, 0.0f);
	const FColor DebugColor = PointColor.ToFColor(true);
	const bool bHasLimit = PointLimit > 0;
	int32 DrawnCount = 0;

	for (const FGeometryScriptPolyPath& Line : Lines)
	{
		if (!Line.Path.IsValid())
		{
			continue;
		}

		for (const FVector& Point : *Line.Path)
		{
			if (bHasLimit && DrawnCount >= PointLimit)
			{
				return DrawnCount;
			}

			DrawDebugPoint(World, Point, SafePointSize, DebugColor, bPersistent, SafeDuration, 0);
			++DrawnCount;
		}
	}

	return DrawnCount;
}

static constexpr float VineSCStageDebugPointSize = 8.0f;
static constexpr bool bVineSCStageDebugPointsPersistent = false;
static constexpr int32 VineSCStageDebugPointLimit = 0;
static const FLinearColor VineSCStageTubeDebugPointColor(0.0f, 1.0f, 0.2f, 1.0f);
static const FLinearColor VineSCStagePlaneDebugPointColor(0.1f, 0.55f, 1.0f, 1.0f);
static constexpr float VineGPUProjectionVoxelCenterPointSize = 6.0f;
static constexpr float VineGPUProjectionVoxelTargetPointSize = 9.0f;
static const FLinearColor VineGPUProjectionVoxelCenterColor(1.0f, 0.85f, 0.0f, 1.0f);
static const FLinearColor VineGPUProjectionVoxelTargetColor(1.0f, 0.0f, 0.1f, 1.0f);

static bool IsFiniteVector(const FVector& Vector)
{
	return FMath::IsFinite(Vector.X) && FMath::IsFinite(Vector.Y) && FMath::IsFinite(Vector.Z);
}

static bool GetVineSurfaceVoxelCenterForGPUProjection(const FCSSurfaceVoxelData& VoxelData, int32 Index, float SafeVoxelSize, FVector& OutVoxelCenter)
{
	if (VoxelData.Positions.IsValidIndex(Index) && IsFiniteVector(VoxelData.Positions[Index]))
	{
		OutVoxelCenter = VoxelData.Positions[Index];
		return true;
	}

	if (!VoxelData.Cells.IsValidIndex(Index))
	{
		return false;
	}

	const FIntVector& Cell = VoxelData.Cells[Index];
	OutVoxelCenter = FVector(
		(double(Cell.X) + 0.5) * SafeVoxelSize + VoxelData.VoxelOrigin.X,
		(double(Cell.Y) + 0.5) * SafeVoxelSize + VoxelData.VoxelOrigin.Y,
		(double(Cell.Z) + 0.5) * SafeVoxelSize + VoxelData.VoxelOrigin.Z);
	return IsFiniteVector(OutVoxelCenter);
}

static int32 DrawVineGPUProjectionVoxelDebugPoints(
	UWorld* World,
	const FCSSurfaceVoxelData& VoxelData,
	float FallbackVoxelSize,
	float Duration,
	int32 PointLimit,
	bool bPersistent)
{
	if (!World || VoxelData.Cells.Num() == 0)
	{
		return 0;
	}

	const int32 VoxelCount = VoxelData.Cells.Num();
	const int32 DrawLimit = PointLimit > 0 ? FMath::Min(PointLimit, VoxelCount) : VoxelCount;
	const float SafeVoxelSize = FMath::Max(VoxelData.VoxelSize > 0.0f ? VoxelData.VoxelSize : FallbackVoxelSize, UE_KINDA_SMALL_NUMBER);
	const double MaxTargetDistanceSq = FMath::Square(double(SafeVoxelSize * 2.0f));
	const float SafeDuration = FMath::Max(Duration, 0.0f);
	const FColor CenterColor = VineGPUProjectionVoxelCenterColor.ToFColor(true);
	const FColor TargetColor = VineGPUProjectionVoxelTargetColor.ToFColor(true);

	int32 DrawnCount = 0;
	int32 InvalidCenterCount = 0;
	int32 InvalidTargetCount = 0;
	for (int32 Index = 0; Index < DrawLimit; ++Index)
	{
		FVector VoxelCenter;
		if (!GetVineSurfaceVoxelCenterForGPUProjection(VoxelData, Index, SafeVoxelSize, VoxelCenter))
		{
			++InvalidCenterCount;
			continue;
		}

		FVector Target = VoxelData.TargetPositions.IsValidIndex(Index) ? VoxelData.TargetPositions[Index] : VoxelCenter;
		if (!IsFiniteVector(Target) || FVector::DistSquared(Target, VoxelCenter) > MaxTargetDistanceSq)
		{
			Target = VoxelCenter;
			++InvalidTargetCount;
		}

		DrawDebugPoint(
			World,
			VoxelCenter,
			VineGPUProjectionVoxelCenterPointSize,
			CenterColor,
			bPersistent,
			SafeDuration,
			0);

		DrawDebugPoint(
			World,
			Target,
			VineGPUProjectionVoxelTargetPointSize,
			TargetColor,
			bPersistent,
			SafeDuration,
			0);

		++DrawnCount;
	}

	UE_LOG(LogTemp, Display,
		TEXT("[VisVineGPU_VoxelDebug] Drawn GPU projection voxels. Drawn=%d Limit=%d Voxels=%d Positions=%d Targets=%d Cells=%d InvalidCenters=%d InvalidTargets=%d VoxelSize=%.3f"),
		DrawnCount,
		PointLimit,
		VoxelCount,
		VoxelData.Positions.Num(),
		VoxelData.TargetPositions.Num(),
		VoxelData.Cells.Num(),
		InvalidCenterCount,
		InvalidTargetCount,
		SafeVoxelSize);

	return DrawnCount;
}

static float SampleScaleArrayByAlpha(const TArray<float>& Scales, double Alpha, float FallbackScale)
{
	if (Scales.Num() == 0)
	{
		return FallbackScale;
	}

	if (Scales.Num() == 1)
	{
		return Scales[0];
	}

	const double ClampedAlpha = FMath::Clamp(Alpha, 0.0, 1.0);
	const double ScaledIndex = ClampedAlpha * double(Scales.Num() - 1);
	const int32 IndexA = FMath::Clamp(FMath::FloorToInt(ScaledIndex), 0, Scales.Num() - 1);
	const int32 IndexB = FMath::Min(IndexA + 1, Scales.Num() - 1);
	return FMath::Lerp(Scales[IndexA], Scales[IndexB], float(ScaledIndex - double(IndexA)));
}

static void BuildPreparedLinePointScales(
	const FGeometryScriptPolyPath& SourceLine,
	const TArray<float>* SourcePointScales,
	int32 OutputPointCount,
	float FallbackScale,
	TArray<float>& OutPointScales)
{
	OutPointScales.Reset();
	if (OutputPointCount <= 0)
	{
		return;
	}

	OutPointScales.SetNumUninitialized(OutputPointCount);
	if (!SourcePointScales || SourcePointScales->Num() == 0)
	{
		for (float& PointScale : OutPointScales)
		{
			PointScale = FallbackScale;
		}
		return;
	}

	const int32 SourceScaleCount = SourcePointScales->Num();
	if (SourceScaleCount == 1)
	{
		for (float& PointScale : OutPointScales)
		{
			PointScale = (*SourcePointScales)[0];
		}
		return;
	}

	if (SourceLine.Path.IsValid() && SourceLine.Path->Num() == SourceScaleCount && SourceScaleCount > 1)
	{
		const TArray<FVector>& SourcePoints = *SourceLine.Path;
		TArray<double> CumulativeLengths;
		CumulativeLengths.SetNumZeroed(SourceScaleCount);
		for (int32 PointIndex = 1; PointIndex < SourceScaleCount; ++PointIndex)
		{
			CumulativeLengths[PointIndex] = CumulativeLengths[PointIndex - 1] + FVector::Dist(SourcePoints[PointIndex - 1], SourcePoints[PointIndex]);
		}

		const double TotalLength = CumulativeLengths.Last();
		if (TotalLength > UE_SMALL_NUMBER)
		{
			int32 SourceSegmentIndex = 0;
			for (int32 OutputIndex = 0; OutputIndex < OutputPointCount; ++OutputIndex)
			{
				const double Alpha = OutputPointCount > 1 ? double(OutputIndex) / double(OutputPointCount - 1) : 0.0;
				const double TargetLength = Alpha * TotalLength;
				while (SourceSegmentIndex + 1 < SourceScaleCount && CumulativeLengths[SourceSegmentIndex + 1] < TargetLength)
				{
					++SourceSegmentIndex;
				}

				if (SourceSegmentIndex + 1 >= SourceScaleCount)
				{
					OutPointScales[OutputIndex] = (*SourcePointScales)[SourceScaleCount - 1];
					continue;
				}

				const double SegmentLength = CumulativeLengths[SourceSegmentIndex + 1] - CumulativeLengths[SourceSegmentIndex];
				const double SegmentAlpha = SegmentLength > UE_SMALL_NUMBER ? (TargetLength - CumulativeLengths[SourceSegmentIndex]) / SegmentLength : 0.0;
				OutPointScales[OutputIndex] = FMath::Lerp((*SourcePointScales)[SourceSegmentIndex], (*SourcePointScales)[SourceSegmentIndex + 1], float(SegmentAlpha));
			}
			return;
		}
	}

	for (int32 OutputIndex = 0; OutputIndex < OutputPointCount; ++OutputIndex)
	{
		const double Alpha = OutputPointCount > 1 ? double(OutputIndex) / double(OutputPointCount - 1) : 0.0;
		OutPointScales[OutputIndex] = SampleScaleArrayByAlpha(*SourcePointScales, Alpha, FallbackScale);
	}
}

static void RebuildVinePointScalesForEditedLine(
	const FGeometryScriptPolyPath& PreviousLine,
	const FGeometryScriptPolyPath& NewLine,
	float FallbackScale,
	TArray<float>& PointScales)
{
	TArray<float> NewPointScales;
	const int32 NewPointCount = NewLine.Path.IsValid() ? NewLine.Path->Num() : 0;
	const TArray<float>* ExistingPointScales = PointScales.Num() > 0 ? &PointScales : nullptr;
	BuildPreparedLinePointScales(PreviousLine, ExistingPointScales, NewPointCount, FallbackScale, NewPointScales);
	PointScales = MoveTemp(NewPointScales);
}

static uint32 BuildVineVisualizationPointSortKey(const FVector& Point)
{
	return BuildVineVisualizationRandomSeed(FVector::ZeroVector, Point);
}

static float GetVineVisualizationTinyZJitter(const FVector& Point, int32 PointIndex)
{
	const FVector IndexSeed(double(PointIndex), double(PointIndex) * 0.37, double(PointIndex) * 0.11);
	const uint32 Seed = BuildVineVisualizationRandomSeed(IndexSeed, Point);
	return (float(Seed & 0xffffu) / float(0xffffu)) * 0.1f;
}

static bool IsFiniteVineVector(const FVector& Vector)
{
	return FMath::IsFinite(Vector.X) && FMath::IsFinite(Vector.Y) && FMath::IsFinite(Vector.Z);
}

static bool AppendVineProjectionTriangle(
	FDynamicMesh3& Mesh,
	const FVector& A,
	const FVector& B,
	const FVector& C)
{
	if (!IsFiniteVineVector(A) || !IsFiniteVineVector(B) || !IsFiniteVineVector(C))
	{
		return false;
	}

	const FVector AB = B - A;
	const FVector AC = C - A;
	if (FVector::CrossProduct(AB, AC).SizeSquared() <= 1.0e-8)
	{
		return false;
	}

	const int32 VA = Mesh.AppendVertex(FVector3d(A));
	const int32 VB = Mesh.AppendVertex(FVector3d(B));
	const int32 VC = Mesh.AppendVertex(FVector3d(C));
	return Mesh.AppendTriangle(VA, VB, VC) >= 0;
}

static bool BuildVineProjectionTriangleMesh(const FCSTriangleMeshData& TriangleData, FDynamicMesh3& OutMesh)
{
	OutMesh.Clear();

	const int32 EffectiveVertexCount = TriangleData.VertexCount >= 0
		? FMath::Clamp(TriangleData.VertexCount, 0, TriangleData.Vertices.Num())
		: TriangleData.Vertices.Num();
	const int32 EffectiveIndexCount = TriangleData.IndexCount >= 0
		? FMath::Clamp(TriangleData.IndexCount, 0, TriangleData.Indices.Num())
		: TriangleData.Indices.Num();
	if (EffectiveVertexCount < 3)
	{
		return false;
	}

	int32 AddedTriangles = 0;
	if (EffectiveIndexCount >= 3)
	{
		const int32 TriangleCount = EffectiveIndexCount / 3;
		for (int32 TriangleIndex = 0; TriangleIndex < TriangleCount; ++TriangleIndex)
		{
			const int32 IA = TriangleData.Indices[TriangleIndex * 3 + 0];
			const int32 IB = TriangleData.Indices[TriangleIndex * 3 + 1];
			const int32 IC = TriangleData.Indices[TriangleIndex * 3 + 2];
			if (IA < 0 || IB < 0 || IC < 0 || IA >= EffectiveVertexCount || IB >= EffectiveVertexCount || IC >= EffectiveVertexCount)
			{
				continue;
			}

			if (AppendVineProjectionTriangle(OutMesh, TriangleData.Vertices[IA], TriangleData.Vertices[IB], TriangleData.Vertices[IC]))
			{
				++AddedTriangles;
			}
		}
	}
	else
	{
		const int32 TriangleCount = EffectiveVertexCount / 3;
		for (int32 TriangleIndex = 0; TriangleIndex < TriangleCount; ++TriangleIndex)
		{
			if (AppendVineProjectionTriangle(
				OutMesh,
				TriangleData.Vertices[TriangleIndex * 3 + 0],
				TriangleData.Vertices[TriangleIndex * 3 + 1],
				TriangleData.Vertices[TriangleIndex * 3 + 2]))
			{
				++AddedTriangles;
			}
		}
	}

	return AddedTriangles > 0;
}

struct FVineTriangleBVHProjectionCache
{
	FDynamicMesh3 Mesh;
	TUniquePtr<UE::Geometry::TMeshAABBTree3<FDynamicMesh3>> Spatial;
};

static TUniquePtr<FVineTriangleBVHProjectionCache> BuildVineTriangleBVHProjectionCache(const FCSTriangleMeshData& TriangleData)
{
	TUniquePtr<FVineTriangleBVHProjectionCache> Cache = MakeUnique<FVineTriangleBVHProjectionCache>();
	if (!BuildVineProjectionTriangleMesh(TriangleData, Cache->Mesh) || Cache->Mesh.TriangleCount() == 0)
	{
		return nullptr;
	}

	Cache->Spatial = MakeUnique<UE::Geometry::TMeshAABBTree3<FDynamicMesh3>>(&Cache->Mesh, true);
	return Cache;
}

static bool ProjectVinePathToNearestTriangleBVH(
	const FVector& QueryPosition,
	const FVineTriangleBVHProjectionCache& Cache,
	FVector& OutProjected,
	FVector& OutNormal)
{
	if (!Cache.Spatial || Cache.Mesh.TriangleCount() == 0)
	{
		return false;
	}

	double NearDistSq = TNumericLimits<double>::Max();
	const int32 NearTri = Cache.Spatial->FindNearestTriangle(FVector3d(QueryPosition), NearDistSq);
	if (NearTri < 0 || !Cache.Mesh.IsTriangle(NearTri))
	{
		return false;
	}

	const FIndex3i Tri = Cache.Mesh.GetTriangle(NearTri);
	const FVector3d A = Cache.Mesh.GetVertex(Tri[0]);
	const FVector3d B = Cache.Mesh.GetVertex(Tri[1]);
	const FVector3d C = Cache.Mesh.GetVertex(Tri[2]);
	OutProjected = FMath::ClosestPointOnTriangleToPoint(QueryPosition, FVector(A), FVector(B), FVector(C));
	FVector Normal = FVector(UE::Geometry::VectorUtil::Normal(A, B, C));
	if (!IsFiniteVineVector(Normal) || !Normal.Normalize())
	{
		Normal = FVector::UpVector;
	}
	OutNormal = Normal;
	return true;
}

static bool PrepareVineVisualizationLinesProjected(
	const TArray<FGeometryScriptPolyPath>& Lines,
	const FVineVisualization& VV,
	bool bMainVine,
	const TArray<float>& InLineSourceScales,
	const TArray<FVector>& InLineSourceLocations,
	const TArray<FVineLinePointScaleData>& InLinePointScales,
	const TCHAR* ProjectionLabel,
	TFunctionRef<bool(const FVector& Query, FVector& OutProjected, FVector& OutNormal)> ProjectSurfacePoint,
	bool bApplyVinesOffset,
	TArray<FGeometryScriptPolyPath>& OutLines,
	TArray<float>& OutLineSourceScales,
	TArray<FVineLinePointScaleData>& OutLinePointScales)
{
	const TCHAR* SafeProjectionLabel = ProjectionLabel ? ProjectionLabel : TEXT("Unknown");
	const TCHAR* VineKindLabel = bMainVine ? TEXT("tube") : TEXT("plane");
	const double PrepStartSeconds = FPlatformTime::Seconds();
	double BuildWorkingMs = 0.0;
	double NoiseAndProjectMs = 0.0;
	double ReduceSampleMs = 0.0;
	double MergeAndFinalProjectMs = 0.0;

	OutLines.Reset();
	OutLines.Reserve(Lines.Num());
	OutLineSourceScales.Reset();
	OutLineSourceScales.Reserve(Lines.Num());
	OutLinePointScales.Reset();
	OutLinePointScales.Reserve(Lines.Num());

	if (VV.ResampleLength <= KINDA_SMALL_NUMBER)
	{
		return false;
	}

	TArray<FGeometryScriptPolyPath> WorkingLines;
	WorkingLines.Reserve(Lines.Num());
	TArray<float> WorkingLineSourceScales;
	WorkingLineSourceScales.Reserve(Lines.Num());
	TArray<FVineLinePointScaleData> WorkingLinePointScales;
	WorkingLinePointScales.Reserve(Lines.Num());

	const double BuildWorkingStartSeconds = FPlatformTime::Seconds();
	for (int32 LineIdx = 0; LineIdx < Lines.Num(); ++LineIdx)
	{
		const FGeometryScriptPolyPath& InputLine = Lines[LineIdx];
		if (!InputLine.Path.IsValid() || InputLine.Path->Num() < 2)
		{
			continue;
		}

		FGeometryScriptPolyPath Line = ClonePolyPath(InputLine);
		if (InLineSourceLocations.IsValidIndex(LineIdx))
		{
			ApplyVineVisualizationSCPointOffset(Line, InLineSourceLocations[LineIdx]);
		}

		const float FallbackScale = InLineSourceScales.IsValidIndex(LineIdx) ? InLineSourceScales[LineIdx] : 1.0f;
		const TArray<float>* InputPointScales = InLinePointScales.IsValidIndex(LineIdx) ? &InLinePointScales[LineIdx].Values : nullptr;
		WorkingLines.Add(Line);
		WorkingLineSourceScales.Add(FallbackScale);
		FVineLinePointScaleData& WorkingScaleData = WorkingLinePointScales.AddDefaulted_GetRef();
		BuildPreparedLinePointScales(Line, InputPointScales, Line.Path->Num(), FallbackScale, WorkingScaleData.Values);
	}
	BuildWorkingMs = (FPlatformTime::Seconds() - BuildWorkingStartSeconds) * 1000.0;

	if (WorkingLines.Num() == 0)
	{
		return false;
	}

	TArray<FGeometryScriptPolyPath> NoiseLines;
	NoiseLines.Reserve(WorkingLines.Num());
	TArray<float> NoiseLineSourceScales;
	NoiseLineSourceScales.Reserve(WorkingLines.Num());
	TArray<FVineLinePointScaleData> NoiseLinePointScales;
	NoiseLinePointScales.Reserve(WorkingLines.Num());

	int32 ProjectionAttempts = 0;
	int32 ProjectionHits = 0;
	double ProjectionDistanceSum = 0.0;
	double ProjectionDistanceMax = 0.0;
	double ProjectionMs = 0.0;
	auto ProjectVinePoint = [&](const FVector& Query, FVector& OutProjected, FVector& OutNormal)
	{
		++ProjectionAttempts;
		const double ProjectionStartSeconds = FPlatformTime::Seconds();
		const bool bProjectionHit = ProjectSurfacePoint(Query, OutProjected, OutNormal);
		ProjectionMs += (FPlatformTime::Seconds() - ProjectionStartSeconds) * 1000.0;
		if (bProjectionHit)
		{
			++ProjectionHits;
			const double Distance = FVector::Dist(Query, OutProjected);
			ProjectionDistanceSum += Distance;
			ProjectionDistanceMax = FMath::Max(ProjectionDistanceMax, Distance);
			return true;
		}
		return false;
	};

	TArray<FVector> SampleRangePointsSum;
	const double NoiseAndProjectStartSeconds = FPlatformTime::Seconds();
	for (int32 LineIdx = 0; LineIdx < WorkingLines.Num(); ++LineIdx)
	{
		FGeometryScriptPolyPath Line = ClonePolyPath(WorkingLines[LineIdx]);
		if (!Line.Path.IsValid())
		{
			continue;
		}

		TArray<float> CurrentPointScales = WorkingLinePointScales[LineIdx].Values;
		const float FallbackScale = WorkingLineSourceScales.IsValidIndex(LineIdx) ? WorkingLineSourceScales[LineIdx] : 1.0f;
		const float ArcLength = float(GetVinePolyPathLength(Line));
		const int32 NumIterations = int32(ArcLength / VV.ResampleLength);
		if (NumIterations < 2)
		{
			continue;
		}

		FGeometryScriptPolyPath PreviousLine = ClonePolyPath(Line);
		Line = UPolyLine::SmoothLine(Line, 3);
		RebuildVinePointScalesForEditedLine(PreviousLine, Line, FallbackScale, CurrentPointScales);
		PreviousLine = ClonePolyPath(Line);
		Line = UPolyLine::ResamppleByLength(Line, VV.ResampleLength);
		RebuildVinePointScalesForEditedLine(PreviousLine, Line, FallbackScale, CurrentPointScales);
		if (!Line.Path.IsValid() || Line.Path->Num() < 3)
		{
			continue;
		}

		for (int32 IterationIndex = 0; IterationIndex < 10; ++IterationIndex)
		{
			const int32 VertexCount = Line.Path->Num();
			for (int32 PointIndex = 0; PointIndex < VertexCount; ++PointIndex)
			{
				FVector& VertexLocation = (*Line.Path)[PointIndex];
				FVector ProjectedPos;
				FVector ProjectedNormal;
				if (ProjectVinePoint(VertexLocation, ProjectedPos, ProjectedNormal))
				{
					VertexLocation = ProjectedPos;
				}

				UNoise::CurlNoise(VertexLocation, VertexLocation, FVector::ZeroVector, VV.CurlNoiseScale / 10.0f, VV.CurlNoiseFre);
				const FVector NoisePos = (VV.PerlinNoiseFre / 100.0f) * VertexLocation;
				const float OffsetNoise = VV.PerlinNoiseScale * FMath::PerlinNoise3D(NoisePos);
				const float PerlinOffset = VV.CurveControl ? VV.CurveControl->GetUnadjustedLinearColorValue(PointIndex / double(VertexCount - 1)).R : 0.0f;
				VertexLocation.X += OffsetNoise * PerlinOffset * (1.0f - float(bMainVine));
			}

			PreviousLine = ClonePolyPath(Line);
			Line = UPolyLine::ResamppleByLength(Line, VV.ResampleLength);
			RebuildVinePointScalesForEditedLine(PreviousLine, Line, FallbackScale, CurrentPointScales);
			if (!Line.Path.IsValid() || Line.Path->Num() < 3)
			{
				break;
			}
		}

		if (!Line.Path.IsValid() || Line.Path->Num() < 3)
		{
			continue;
		}

		for (FVector& VertexLocation : *Line.Path)
		{
			UNoise::CurlNoise(VertexLocation, VertexLocation, FVector::ZeroVector, VV.CurlNoiseScale / 10.0f, VV.CurlNoiseFre);
		}

		const int32 SampleCount = FMath::Clamp(FMath::FloorToInt(float(Line.Path->Num()) * 0.8f), 0, Line.Path->Num());
		for (int32 PointIndex = 0; PointIndex < SampleCount; ++PointIndex)
		{
			SampleRangePointsSum.Add((*Line.Path)[PointIndex]);
		}

		NoiseLines.Add(Line);
		NoiseLineSourceScales.Add(FallbackScale);
		FVineLinePointScaleData& NoiseScaleData = NoiseLinePointScales.AddDefaulted_GetRef();
		NoiseScaleData.Values = MoveTemp(CurrentPointScales);
	}
	NoiseAndProjectMs = (FPlatformTime::Seconds() - NoiseAndProjectStartSeconds) * 1000.0;

	if (NoiseLines.Num() == 0 || SampleRangePointsSum.Num() == 0)
	{
		return false;
	}

	const double ReduceSampleStartSeconds = FPlatformTime::Seconds();
	SampleRangePointsSum.Sort([](const FVector& A, const FVector& B)
	{
		const uint32 AKey = BuildVineVisualizationPointSortKey(A);
		const uint32 BKey = BuildVineVisualizationPointSortKey(B);
		if (AKey != BKey)
		{
			return AKey < BKey;
		}
		if (!FMath::IsNearlyEqual(A.X, B.X))
		{
			return A.X < B.X;
		}
		if (!FMath::IsNearlyEqual(A.Y, B.Y))
		{
			return A.Y < B.Y;
		}
		return A.Z < B.Z;
	});
	const int32 ReducedSampleCount = FMath::Max(1, SampleRangePointsSum.Num() / 15);
	SampleRangePointsSum.SetNum(ReducedSampleCount);
	ReduceSampleMs = (FPlatformTime::Seconds() - ReduceSampleStartSeconds) * 1000.0;

	const double MergeAndFinalProjectStartSeconds = FPlatformTime::Seconds();
	for (int32 LineIdx = 0; LineIdx < NoiseLines.Num(); ++LineIdx)
	{
		FGeometryScriptPolyPath Line = ClonePolyPath(NoiseLines[LineIdx]);
		if (!Line.Path.IsValid() || Line.Path->Num() < 3)
		{
			continue;
		}

		TArray<float> CurrentPointScales = NoiseLinePointScales[LineIdx].Values;
		const float FallbackScale = NoiseLineSourceScales.IsValidIndex(LineIdx) ? NoiseLineSourceScales[LineIdx] : 1.0f;

		if (!bMainVine)
		{
			const int32 VertexCount = Line.Path->Num();
			for (int32 PointIndex = 0; PointIndex < VertexCount; ++PointIndex)
			{
				FVector& VertexLocation = (*Line.Path)[PointIndex];
				const int32 NearPointIndex = UPointFunction::FindNearPointIteration(SampleRangePointsSum, VertexLocation);
				if (!SampleRangePointsSum.IsValidIndex(NearPointIndex))
				{
					continue;
				}

				const float Dist = FVector::Dist(SampleRangePointsSum[NearPointIndex], VertexLocation);
				if (Dist > VV.ResampleLength * VV.MergeDistMult)
				{
					continue;
				}

				const FVector NoisePos = (VV.PerlinNoiseFre / 100.0f) * VertexLocation;
				float OffsetNoise = FMath::Abs(FMath::PerlinNoise3D(NoisePos + FVector::OneVector * 10.0f));
				OffsetNoise = VV.CurveControl ? VV.CurveControl->GetUnadjustedLinearColorValue(OffsetNoise).B : OffsetNoise;
				VertexLocation = FMath::Lerp(VertexLocation, SampleRangePointsSum[NearPointIndex], OffsetNoise);
			}

			FGeometryScriptPolyPath PreviousLine = ClonePolyPath(Line);
			Line = UPolyLine::ResamppleByLength(Line, VV.ResampleLength);
			RebuildVinePointScalesForEditedLine(PreviousLine, Line, FallbackScale, CurrentPointScales);
			PreviousLine = ClonePolyPath(Line);
			Line = UPolyLine::SmoothLine(Line, 3);
			RebuildVinePointScalesForEditedLine(PreviousLine, Line, FallbackScale, CurrentPointScales);
			PreviousLine = ClonePolyPath(Line);
			Line = UPolyLine::ResamppleByLength(Line, VV.ResampleLength);
			RebuildVinePointScalesForEditedLine(PreviousLine, Line, FallbackScale, CurrentPointScales);

			if (!Line.Path.IsValid() || Line.Path->Num() < 3)
			{
				continue;
			}

			for (FVector& VertexLocation : *Line.Path)
			{
				FVector ProjectedPos;
				FVector ProjectedNormal;
				if (ProjectVinePoint(VertexLocation, ProjectedPos, ProjectedNormal))
				{
					VertexLocation = ProjectedPos;
				}
			}
		}

		const int32 VertexCount = Line.Path->Num();
		for (int32 PointIndex = 0; PointIndex < VertexCount; ++PointIndex)
		{
			const FVector VertexLocation = (*Line.Path)[PointIndex];
			FVector ProjectedPos;
			FVector ProjectedNormal;
			if (!ProjectVinePoint(VertexLocation, ProjectedPos, ProjectedNormal))
			{
				continue;
			}

			FVector& VertexLocationFix = (*Line.Path)[PointIndex];
			VertexLocationFix = ProjectedPos;
			if (bApplyVinesOffset)
			{
				VertexLocationFix += ProjectedNormal * GetVineVisualizationTinyZJitter(VertexLocation, PointIndex);
			}
		}

		FGeometryScriptPolyPath PreviousLine = ClonePolyPath(Line);
		Line = UPolyLine::ResamppleByLength(Line, VV.ResampleLength);
		RebuildVinePointScalesForEditedLine(PreviousLine, Line, FallbackScale, CurrentPointScales);
		PreviousLine = ClonePolyPath(Line);
		Line = UPolyLine::SmoothLine(Line, 1);
		RebuildVinePointScalesForEditedLine(PreviousLine, Line, FallbackScale, CurrentPointScales);

		if (!Line.Path.IsValid() || Line.Path->Num() < 3)
		{
			continue;
		}

		for (int32 PointIndex = 0; PointIndex < Line.Path->Num(); ++PointIndex)
		{
			const FVector VertexLocation = (*Line.Path)[PointIndex];
			FVector ProjectedPos;
			FVector ProjectedNormal;
			if (!ProjectVinePoint(VertexLocation, ProjectedPos, ProjectedNormal))
			{
				continue;
			}

			FVector& VertexLocationFix = (*Line.Path)[PointIndex];
			VertexLocationFix = bApplyVinesOffset ? ProjectedPos + ProjectedNormal * VV.VinesOffset : ProjectedPos;
			if (bApplyVinesOffset)
			{
				VertexLocationFix += ProjectedNormal * GetVineVisualizationTinyZJitter(VertexLocation, PointIndex);
			}
		}

		if (CurrentPointScales.Num() != Line.Path->Num())
		{
			TArray<float> FixedPointScales;
			BuildPreparedLinePointScales(Line, &CurrentPointScales, Line.Path->Num(), FallbackScale, FixedPointScales);
			CurrentPointScales = MoveTemp(FixedPointScales);
		}

		OutLines.Add(Line);
		OutLineSourceScales.Add(FallbackScale);
		FVineLinePointScaleData& OutScaleData = OutLinePointScales.AddDefaulted_GetRef();
		OutScaleData.Values = MoveTemp(CurrentPointScales);
	}
	MergeAndFinalProjectMs = (FPlatformTime::Seconds() - MergeAndFinalProjectStartSeconds) * 1000.0;

	if (ProjectionAttempts > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("[VisVinePrep] %s %s hits=%d/%d (%.1f%%) distAvg=%.3f distMax=%.3f outputLines=%d"),
			SafeProjectionLabel,
			VineKindLabel,
			ProjectionHits,
			ProjectionAttempts,
			100.0 * double(ProjectionHits) / double(ProjectionAttempts),
			ProjectionHits > 0 ? ProjectionDistanceSum / double(ProjectionHits) : 0.0,
			ProjectionDistanceMax,
			OutLines.Num());
	}

	UE_LOG(LogTemp, Display,
		TEXT("[VisVinePrepTiming] %s %s total=%.3f ms buildWorking=%.3f ms noiseProject=%.3f ms reduceSample=%.3f ms mergeFinalProject=%.3f ms projectionCalls=%.3f ms"),
		SafeProjectionLabel,
		VineKindLabel,
		(FPlatformTime::Seconds() - PrepStartSeconds) * 1000.0,
		BuildWorkingMs,
		NoiseAndProjectMs,
		ReduceSampleMs,
		MergeAndFinalProjectMs,
		ProjectionMs);

	return OutLines.Num() > 0;
}

static bool PrepareVineVisualizationLinesGPU_NoCPUProjection(
	const TArray<FGeometryScriptPolyPath>& Lines,
	const FVineVisualization& VV,
	bool bMainVine,
	const TArray<float>& InLineSourceScales,
	const TArray<FVector>& InLineSourceLocations,
	const TArray<FVineLinePointScaleData>& InLinePointScales,
	TArray<FGeometryScriptPolyPath>& OutLines,
	TArray<float>& OutLineSourceScales,
	TArray<FVineLinePointScaleData>& OutLinePointScales)
{
	const double PrepStartSeconds = FPlatformTime::Seconds();
	double BuildWorkingMs = 0.0;
	double NoiseMs = 0.0;
	double ReduceSampleMs = 0.0;
	double MergeAndFinalMs = 0.0;

	OutLines.Reset();
	OutLines.Reserve(Lines.Num());
	OutLineSourceScales.Reset();
	OutLineSourceScales.Reserve(Lines.Num());
	OutLinePointScales.Reset();
	OutLinePointScales.Reserve(Lines.Num());

	if (VV.ResampleLength <= KINDA_SMALL_NUMBER)
	{
		return false;
	}

	TArray<FGeometryScriptPolyPath> WorkingLines;
	WorkingLines.Reserve(Lines.Num());
	TArray<float> WorkingLineSourceScales;
	WorkingLineSourceScales.Reserve(Lines.Num());
	TArray<FVineLinePointScaleData> WorkingLinePointScales;
	WorkingLinePointScales.Reserve(Lines.Num());

	const double BuildWorkingStartSeconds = FPlatformTime::Seconds();
	for (int32 LineIdx = 0; LineIdx < Lines.Num(); ++LineIdx)
	{
		const FGeometryScriptPolyPath& InputLine = Lines[LineIdx];
		if (!InputLine.Path.IsValid() || InputLine.Path->Num() < 2)
		{
			continue;
		}

		FGeometryScriptPolyPath Line = ClonePolyPath(InputLine);
		if (InLineSourceLocations.IsValidIndex(LineIdx))
		{
			ApplyVineVisualizationSCPointOffset(Line, InLineSourceLocations[LineIdx]);
		}

		const float FallbackScale = InLineSourceScales.IsValidIndex(LineIdx) ? InLineSourceScales[LineIdx] : 1.0f;
		const TArray<float>* InputPointScales = InLinePointScales.IsValidIndex(LineIdx) ? &InLinePointScales[LineIdx].Values : nullptr;
		WorkingLines.Add(Line);
		WorkingLineSourceScales.Add(FallbackScale);
		FVineLinePointScaleData& WorkingScaleData = WorkingLinePointScales.AddDefaulted_GetRef();
		BuildPreparedLinePointScales(Line, InputPointScales, Line.Path->Num(), FallbackScale, WorkingScaleData.Values);
	}
	BuildWorkingMs = (FPlatformTime::Seconds() - BuildWorkingStartSeconds) * 1000.0;

	if (WorkingLines.Num() == 0)
	{
		return false;
	}

	TArray<FGeometryScriptPolyPath> NoiseLines;
	NoiseLines.Reserve(WorkingLines.Num());
	TArray<float> NoiseLineSourceScales;
	NoiseLineSourceScales.Reserve(WorkingLines.Num());
	TArray<FVineLinePointScaleData> NoiseLinePointScales;
	NoiseLinePointScales.Reserve(WorkingLines.Num());

	TArray<FVector> SampleRangePointsSum;
	const double NoiseStartSeconds = FPlatformTime::Seconds();
	for (int32 LineIdx = 0; LineIdx < WorkingLines.Num(); ++LineIdx)
	{
		FGeometryScriptPolyPath Line = ClonePolyPath(WorkingLines[LineIdx]);
		if (!Line.Path.IsValid())
		{
			continue;
		}

		TArray<float> CurrentPointScales = WorkingLinePointScales[LineIdx].Values;
		const float FallbackScale = WorkingLineSourceScales.IsValidIndex(LineIdx) ? WorkingLineSourceScales[LineIdx] : 1.0f;
		const float ArcLength = float(GetVinePolyPathLength(Line));
		const int32 NumIterations = int32(ArcLength / VV.ResampleLength);
		if (NumIterations < 2)
		{
			continue;
		}

		FGeometryScriptPolyPath PreviousLine = ClonePolyPath(Line);
		Line = UPolyLine::SmoothLine(Line, 3);
		RebuildVinePointScalesForEditedLine(PreviousLine, Line, FallbackScale, CurrentPointScales);
		PreviousLine = ClonePolyPath(Line);
		Line = UPolyLine::ResamppleByLength(Line, VV.ResampleLength);
		RebuildVinePointScalesForEditedLine(PreviousLine, Line, FallbackScale, CurrentPointScales);
		if (!Line.Path.IsValid() || Line.Path->Num() < 3)
		{
			continue;
		}

		for (int32 IterationIndex = 0; IterationIndex < 10; ++IterationIndex)
		{
			const int32 VertexCount = Line.Path->Num();
			for (int32 PointIndex = 0; PointIndex < VertexCount; ++PointIndex)
			{
				FVector& VertexLocation = (*Line.Path)[PointIndex];
				UNoise::CurlNoise(VertexLocation, VertexLocation, FVector::ZeroVector, VV.CurlNoiseScale / 10.0f, VV.CurlNoiseFre);
				const FVector NoisePos = (VV.PerlinNoiseFre / 100.0f) * VertexLocation;
				const float OffsetNoise = VV.PerlinNoiseScale * FMath::PerlinNoise3D(NoisePos);
				const float PerlinOffset = VV.CurveControl ? VV.CurveControl->GetUnadjustedLinearColorValue(PointIndex / double(VertexCount - 1)).R : 0.0f;
				VertexLocation.X += OffsetNoise * PerlinOffset * (1.0f - float(bMainVine));
			}

			PreviousLine = ClonePolyPath(Line);
			Line = UPolyLine::ResamppleByLength(Line, VV.ResampleLength);
			RebuildVinePointScalesForEditedLine(PreviousLine, Line, FallbackScale, CurrentPointScales);
			if (!Line.Path.IsValid() || Line.Path->Num() < 3)
			{
				break;
			}
		}

		if (!Line.Path.IsValid() || Line.Path->Num() < 3)
		{
			continue;
		}

		for (FVector& VertexLocation : *Line.Path)
		{
			UNoise::CurlNoise(VertexLocation, VertexLocation, FVector::ZeroVector, VV.CurlNoiseScale / 10.0f, VV.CurlNoiseFre);
		}

		const int32 SampleCount = FMath::Clamp(FMath::FloorToInt(float(Line.Path->Num()) * 0.8f), 0, Line.Path->Num());
		for (int32 PointIndex = 0; PointIndex < SampleCount; ++PointIndex)
		{
			SampleRangePointsSum.Add((*Line.Path)[PointIndex]);
		}

		NoiseLines.Add(Line);
		NoiseLineSourceScales.Add(FallbackScale);
		FVineLinePointScaleData& NoiseScaleData = NoiseLinePointScales.AddDefaulted_GetRef();
		NoiseScaleData.Values = MoveTemp(CurrentPointScales);
	}
	NoiseMs = (FPlatformTime::Seconds() - NoiseStartSeconds) * 1000.0;

	if (NoiseLines.Num() == 0 || SampleRangePointsSum.Num() == 0)
	{
		return false;
	}

	const double ReduceSampleStartSeconds = FPlatformTime::Seconds();
	SampleRangePointsSum.Sort([](const FVector& A, const FVector& B)
	{
		const uint32 AKey = BuildVineVisualizationPointSortKey(A);
		const uint32 BKey = BuildVineVisualizationPointSortKey(B);
		if (AKey != BKey)
		{
			return AKey < BKey;
		}
		if (!FMath::IsNearlyEqual(A.X, B.X))
		{
			return A.X < B.X;
		}
		if (!FMath::IsNearlyEqual(A.Y, B.Y))
		{
			return A.Y < B.Y;
		}
		return A.Z < B.Z;
	});
	const int32 ReducedSampleCount = FMath::Max(1, SampleRangePointsSum.Num() / 15);
	SampleRangePointsSum.SetNum(ReducedSampleCount);
	ReduceSampleMs = (FPlatformTime::Seconds() - ReduceSampleStartSeconds) * 1000.0;

	const double MergeAndFinalStartSeconds = FPlatformTime::Seconds();
	for (int32 LineIdx = 0; LineIdx < NoiseLines.Num(); ++LineIdx)
	{
		FGeometryScriptPolyPath Line = ClonePolyPath(NoiseLines[LineIdx]);
		if (!Line.Path.IsValid() || Line.Path->Num() < 3)
		{
			continue;
		}

		TArray<float> CurrentPointScales = NoiseLinePointScales[LineIdx].Values;
		const float FallbackScale = NoiseLineSourceScales.IsValidIndex(LineIdx) ? NoiseLineSourceScales[LineIdx] : 1.0f;

		if (!bMainVine)
		{
			const int32 VertexCount = Line.Path->Num();
			for (int32 PointIndex = 0; PointIndex < VertexCount; ++PointIndex)
			{
				FVector& VertexLocation = (*Line.Path)[PointIndex];
				const int32 NearPointIndex = UPointFunction::FindNearPointIteration(SampleRangePointsSum, VertexLocation);
				if (!SampleRangePointsSum.IsValidIndex(NearPointIndex))
				{
					continue;
				}

				const float Dist = FVector::Dist(SampleRangePointsSum[NearPointIndex], VertexLocation);
				if (Dist > VV.ResampleLength * VV.MergeDistMult)
				{
					continue;
				}

				const FVector NoisePos = (VV.PerlinNoiseFre / 100.0f) * VertexLocation;
				float OffsetNoise = FMath::Abs(FMath::PerlinNoise3D(NoisePos + FVector::OneVector * 10.0f));
				OffsetNoise = VV.CurveControl ? VV.CurveControl->GetUnadjustedLinearColorValue(OffsetNoise).B : OffsetNoise;
				VertexLocation = FMath::Lerp(VertexLocation, SampleRangePointsSum[NearPointIndex], OffsetNoise);
			}

			FGeometryScriptPolyPath PreviousLine = ClonePolyPath(Line);
			Line = UPolyLine::ResamppleByLength(Line, VV.ResampleLength);
			RebuildVinePointScalesForEditedLine(PreviousLine, Line, FallbackScale, CurrentPointScales);
			PreviousLine = ClonePolyPath(Line);
			Line = UPolyLine::SmoothLine(Line, 3);
			RebuildVinePointScalesForEditedLine(PreviousLine, Line, FallbackScale, CurrentPointScales);
			PreviousLine = ClonePolyPath(Line);
			Line = UPolyLine::ResamppleByLength(Line, VV.ResampleLength);
			RebuildVinePointScalesForEditedLine(PreviousLine, Line, FallbackScale, CurrentPointScales);

			if (!Line.Path.IsValid() || Line.Path->Num() < 3)
			{
				continue;
			}
		}

		FGeometryScriptPolyPath PreviousLine = ClonePolyPath(Line);
		Line = UPolyLine::ResamppleByLength(Line, VV.ResampleLength);
		RebuildVinePointScalesForEditedLine(PreviousLine, Line, FallbackScale, CurrentPointScales);
		PreviousLine = ClonePolyPath(Line);
		Line = UPolyLine::SmoothLine(Line, 1);
		RebuildVinePointScalesForEditedLine(PreviousLine, Line, FallbackScale, CurrentPointScales);

		if (!Line.Path.IsValid() || Line.Path->Num() < 3)
		{
			continue;
		}

		if (CurrentPointScales.Num() != Line.Path->Num())
		{
			TArray<float> FixedPointScales;
			BuildPreparedLinePointScales(Line, &CurrentPointScales, Line.Path->Num(), FallbackScale, FixedPointScales);
			CurrentPointScales = MoveTemp(FixedPointScales);
		}

		OutLines.Add(Line);
		OutLineSourceScales.Add(FallbackScale);
		FVineLinePointScaleData& OutScaleData = OutLinePointScales.AddDefaulted_GetRef();
		OutScaleData.Values = MoveTemp(CurrentPointScales);
	}
	MergeAndFinalMs = (FPlatformTime::Seconds() - MergeAndFinalStartSeconds) * 1000.0;

	UE_LOG(LogTemp, Display,
		TEXT("[VisVinePrepTiming] GPU_NoCPUProjection %s total=%.3f ms buildWorking=%.3f ms noise=%.3f ms reduceSample=%.3f ms mergeFinal=%.3f ms outputLines=%d"),
		bMainVine ? TEXT("tube") : TEXT("plane"),
		(FPlatformTime::Seconds() - PrepStartSeconds) * 1000.0,
		BuildWorkingMs,
		NoiseMs,
		ReduceSampleMs,
		MergeAndFinalMs,
		OutLines.Num());

	return OutLines.Num() > 0;
}

static FVector NormalizeVineAxisOrFallback(const FVector& Axis, const FVector& FallbackAxis = FVector::ZeroVector)
{
	const FVector NormalizedAxis = Axis.GetSafeNormal();
	return NormalizedAxis.IsNearlyZero() ? FallbackAxis.GetSafeNormal() : NormalizedAxis;
}

static FVector OrientVineAxisToReference(const FVector& Axis, const FVector& Reference)
{
	return FVector::DotProduct(Axis, Reference) < 0.0 ? -Axis : Axis;
}

static FVector GetVineLineRawAxis(const TArray<FVector>& Points, int32 PointIndex)
{
	const int32 PointCount = Points.Num();
	if (PointCount < 2)
	{
		return FVector::ForwardVector;
	}

	const int32 PrevIndex = FMath::Max(PointIndex - 1, 0);
	const int32 NextIndex = FMath::Min(PointIndex + 1, PointCount - 1);
	return NormalizeVineAxisOrFallback(Points[NextIndex] - Points[PrevIndex], FVector::ForwardVector);
}

static void BuildVineVisualizationLinePointAxes(
	const TArray<FGeometryScriptPolyPath>& Lines,
	int32 SmoothIterations,
	TArray<FVineLinePointAxisData>& OutLinePointAxes)
{
	const int32 SafeSmoothIterations = FMath::Max(0, SmoothIterations);
	OutLinePointAxes.Reset();
	OutLinePointAxes.Reserve(Lines.Num());

	for (const FGeometryScriptPolyPath& Line : Lines)
	{
		FVineLinePointAxisData& AxisData = OutLinePointAxes.AddDefaulted_GetRef();
		if (!Line.Path.IsValid() || Line.Path->Num() == 0)
		{
			continue;
		}

		const TArray<FVector>& Points = *Line.Path;
		const int32 PointCount = Points.Num();
		AxisData.Values.SetNumUninitialized(PointCount);
		for (int32 PointIndex = 0; PointIndex < PointCount; ++PointIndex)
		{
			AxisData.Values[PointIndex] = GetVineLineRawAxis(Points, PointIndex);
		}

		if (PointCount < 3 || SafeSmoothIterations == 0)
		{
			continue;
		}

		TArray<FVector> ReadAxes = MoveTemp(AxisData.Values);
		TArray<FVector> WriteAxes;
		WriteAxes.SetNumUninitialized(PointCount);

		for (int32 SmoothIterationIndex = 0; SmoothIterationIndex < SafeSmoothIterations; ++SmoothIterationIndex)
		{
			for (int32 PointIndex = 0; PointIndex < PointCount; ++PointIndex)
			{
				const FVector CenterAxis = NormalizeVineAxisOrFallback(ReadAxes[PointIndex], GetVineLineRawAxis(Points, PointIndex));
				FVector AxisSum = CenterAxis * 2.0;
				double WeightSum = 2.0;

				auto AccumulateNeighborAxis = [&](int32 NeighborIndex, double Weight)
				{
					if (!ReadAxes.IsValidIndex(NeighborIndex))
					{
						return;
					}

					FVector NeighborAxis = NormalizeVineAxisOrFallback(ReadAxes[NeighborIndex], CenterAxis);
					NeighborAxis = OrientVineAxisToReference(NeighborAxis, CenterAxis);
					AxisSum += NeighborAxis * Weight;
					WeightSum += Weight;
				};

				AccumulateNeighborAxis(PointIndex - 1, 1.0);
				AccumulateNeighborAxis(PointIndex + 1, 1.0);

				const FVector SmoothedAxis = NormalizeVineAxisOrFallback(AxisSum / WeightSum, CenterAxis);
				WriteAxes[PointIndex] = OrientVineAxisToReference(SmoothedAxis, CenterAxis);
			}

			Swap(ReadAxes, WriteAxes);
		}

		AxisData.Values = MoveTemp(ReadAxes);
	}
}

static float EvaluateVineScale(const UCurveLinearColor* CurveControl, int32 Index, int32 Count)
{
	if (!CurveControl || Count <= 1)
	{
		return 1.0f;
	}

	return CurveControl->GetUnadjustedLinearColorValue(Index / double(Count - 1)).G;
}

static bool BuildVineVisualizationGPUInput(
	const TArray<FGeometryScriptPolyPath>& Lines,
	const UCurveLinearColor* CurveControl,
	const TArray<float>& LineSourceScales,
	const TArray<FVineLinePointScaleData>& LinePointScales,
	const TArray<FVineLinePointAxisData>& LinePointAxes,
	TArray<FVector4f>& OutPathPoints,
	TArray<FVector4f>& OutPathPointAxes,
	TArray<FIntVector4>& OutPathPointMeta,
	TArray<float>& OutPathPointCurveU,
	TArray<FIntVector4>& OutSegmentMeta)
{
	OutPathPoints.Reset();
	OutPathPointAxes.Reset();
	OutPathPointMeta.Reset();
	OutPathPointCurveU.Reset();
	OutSegmentMeta.Reset();

	int32 LineIndex = 0;
	for (const FGeometryScriptPolyPath& Line : Lines)
	{
		if (!Line.Path.IsValid() || Line.Path->Num() < 2)
		{
			++LineIndex;
			continue;
		}

		// PathPoints.w is the final profile multiplier:
		// CurveControl.G * SpaceColonization point scale (TargetPointScale * SourcePointScale).
		const float FallbackPointScale = LineSourceScales.IsValidIndex(LineIndex) ? LineSourceScales[LineIndex] : 1.0f;
		const TArray<float>* PointScales = LinePointScales.IsValidIndex(LineIndex) ? &LinePointScales[LineIndex].Values : nullptr;
		const TArray<FVector>* PointAxes = LinePointAxes.IsValidIndex(LineIndex) ? &LinePointAxes[LineIndex].Values : nullptr;

		const TArray<FVector>& Points = *Line.Path;
		const int32 BaseIndex = OutPathPoints.Num();
		const int32 PointCount = Points.Num();
		float CurveU = 0.0f;

		for (int32 PointIndex = 0; PointIndex < PointCount; ++PointIndex)
		{
			const FVector& Point = Points[PointIndex];
			if (PointIndex > 0)
			{
				CurveU += float(FVector::Dist(Points[PointIndex], Points[PointIndex - 1]));
			}

			const float CurveScale = EvaluateVineScale(CurveControl, PointIndex, PointCount);
			const float PointScale = PointScales && PointScales->IsValidIndex(PointIndex) ? (*PointScales)[PointIndex] : FallbackPointScale;
			const float Scale = CurveScale * FMath::Max(PointScale, 0.0f);
			const FVector PointAxis = PointAxes && PointAxes->IsValidIndex(PointIndex) ? NormalizeVineAxisOrFallback((*PointAxes)[PointIndex]) : FVector::ZeroVector;
			OutPathPoints.Add(FVector4f(float(Point.X), float(Point.Y), float(Point.Z), Scale));
			OutPathPointAxes.Add(FVector4f(float(PointAxis.X), float(PointAxis.Y), float(PointAxis.Z), 0.0f));
			OutPathPointCurveU.Add(CurveU);

			const int32 PrevIndex = BaseIndex + FMath::Max(PointIndex - 1, 0);
			const int32 NextIndex = BaseIndex + FMath::Min(PointIndex + 1, PointCount - 1);
			OutPathPointMeta.Add(FIntVector4(PrevIndex, NextIndex, BaseIndex, PointCount));

			if (PointIndex + 1 < PointCount)
			{
				OutSegmentMeta.Add(FIntVector4(BaseIndex + PointIndex, BaseIndex + PointIndex + 1, 0, 0));
			}
		}

		++LineIndex;
	}

	return OutPathPoints.Num() > 0 && OutSegmentMeta.Num() > 0;
}

static void GetAllFoliageInstanceTransforms(UWorld* World, UFoliageType* InFoliageType, TArray<FTransform>& OutTransforms)
{
	if (!World || !InFoliageType)
	{
		return;
	}

	for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It)
	{
		FFoliageInfo* FoliageInfo = It->FindInfo(InFoliageType);
		if (!FoliageInfo || !FoliageInfo->GetComponent() || FoliageInfo->Instances.Num() == 0)
		{
			continue;
		}

		const int32 InstanceCount = FoliageInfo->GetComponent()->GetInstanceCount();
		for (int32 Index = 0; Index < InstanceCount; ++Index)
		{
			FTransform Transform;
			FoliageInfo->GetComponent()->GetInstanceTransform(Index, Transform, true);
			OutTransforms.Add(Transform);
		}
	}
}

static void GetVineInstanceTransforms(UInstancedStaticMeshComponent* Component, TArray<FTransform>& OutTransforms)
{
	OutTransforms.Reset();
	if (!Component)
	{
		return;
	}

	const int32 InstanceCount = Component->GetInstanceCount();
	OutTransforms.Reserve(InstanceCount);
	for (int32 Index = 0; Index < InstanceCount; ++Index)
	{
		FTransform Transform;
		if (Component->GetInstanceTransform(Index, Transform, true))
		{
			OutTransforms.Add(Transform);
		}
	}
}

static void RefreshFoliageType(UWorld* World, UFoliageType* InFoliageType)
{
	if (!World || !InFoliageType)
	{
		return;
	}

	for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It)
	{
		if (FFoliageInfo* FoliageInfo = It->FindInfo(InFoliageType))
		{
			FoliageInfo->Refresh(true, true);
		}
	}
}

static uint32 HashVineVoxelCell(int32 X, int32 Y, int32 Z)
{
	return (static_cast<uint32>(X) * 73856093u)
		^ (static_cast<uint32>(Y) * 19349663u)
		^ (static_cast<uint32>(Z) * 83492791u);
}

static bool SameVineVoxelCell(const FIntVector4& A, const FIntVector4& B)
{
	return A.X == B.X && A.Y == B.Y && A.Z == B.Z;
}

static bool BuildVineVoxelHashSlots(const TArray<FIntVector4>& VoxelCells, TArray<uint32>& OutHashSlots, uint32& OutHashSlotCount)
{
	OutHashSlots.Reset();
	OutHashSlotCount = 0;

	const int32 VoxelCount = VoxelCells.Num();
	if (VoxelCount <= 0)
	{
		return false;
	}

	uint64 DesiredSlotCount = FMath::Max<uint64>(2ull, uint64(VoxelCount) * 2ull);
	if (DesiredSlotCount > (1ull << 30))
	{
		return false;
	}

	uint32 SlotCount = 1u;
	while (uint64(SlotCount) < DesiredSlotCount)
	{
		SlotCount <<= 1u;
	}

	OutHashSlots.Init(0u, int32(SlotCount));
	const uint32 SlotMask = SlotCount - 1u;

	for (int32 VoxelIndex = 0; VoxelIndex < VoxelCount; ++VoxelIndex)
	{
		const FIntVector4& Cell = VoxelCells[VoxelIndex];
		uint32 Slot = HashVineVoxelCell(Cell.X, Cell.Y, Cell.Z) & SlotMask;
		bool bInserted = false;

		for (uint32 Probe = 0u; Probe < SlotCount; ++Probe)
		{
			uint32& PackedVoxelIndex = OutHashSlots[int32(Slot)];
			if (PackedVoxelIndex == 0u)
			{
				PackedVoxelIndex = uint32(VoxelIndex) + 1u;
				bInserted = true;
				break;
			}

			const int32 ExistingVoxelIndex = int32(PackedVoxelIndex - 1u);
			if (VoxelCells.IsValidIndex(ExistingVoxelIndex) && SameVineVoxelCell(VoxelCells[ExistingVoxelIndex], Cell))
			{
				bInserted = true;
				break;
			}

			Slot = (Slot + 1u) & SlotMask;
		}

		if (!bInserted)
		{
			OutHashSlots.Reset();
			return false;
		}
	}

	OutHashSlotCount = SlotCount;
	return true;
}

struct FVineTargetBucketBuffers
{
	TArray<FIntVector4> Ranges;
	TArray<uint32> RangeCounts;
	TArray<uint32> VoxelIndices;
	TArray<uint32> HashSlots;
	uint32 HashSlotCount = 0u;
	uint32 BucketCount = 0u;
	uint32 MaxBucketItemCount = 0u;
	float BucketSize = 0.0f;
	uint32 SearchRadius = 4u;
};

static void EnsureVineTargetBucketDummyBuffers(FVineTargetBucketBuffers& Buffers)
{
	if (Buffers.Ranges.Num() == 0)
	{
		Buffers.Ranges.Add(FIntVector4(0, 0, 0, 0));
	}
	if (Buffers.RangeCounts.Num() == 0)
	{
		Buffers.RangeCounts.Add(0u);
	}
	if (Buffers.VoxelIndices.Num() == 0)
	{
		Buffers.VoxelIndices.Add(0u);
	}
	if (Buffers.HashSlots.Num() == 0)
	{
		Buffers.HashSlots.Add(0u);
	}
}

static FIntVector GetVineTargetBucketCell(const FVector4f& Target, const FVector3f& Origin, float BucketSize)
{
	const float SafeBucketSize = FMath::Max(BucketSize, UE_KINDA_SMALL_NUMBER);
	return FIntVector(
		FMath::FloorToInt((double(Target.X) - double(Origin.X)) / double(SafeBucketSize)),
		FMath::FloorToInt((double(Target.Y) - double(Origin.Y)) / double(SafeBucketSize)),
		FMath::FloorToInt((double(Target.Z) - double(Origin.Z)) / double(SafeBucketSize)));
}

static bool BuildVineTargetBucketBuffers(
	const TArray<FVector4f>& TargetPositions,
	const FVector3f& Origin,
	float VoxelSize,
	FVineTargetBucketBuffers& OutBuffers)
{
	OutBuffers = FVineTargetBucketBuffers();
	OutBuffers.BucketSize = FMath::Max(VoxelSize * 16.0f, 25.0f);
	OutBuffers.SearchRadius = 8u;

	TMap<FIntVector, TArray<uint32>> BucketMap;
	BucketMap.Reserve(TargetPositions.Num());
	for (int32 TargetIndex = 0; TargetIndex < TargetPositions.Num(); ++TargetIndex)
	{
		const FVector4f& Target = TargetPositions[TargetIndex];
		if (!FMath::IsFinite(Target.X) || !FMath::IsFinite(Target.Y) || !FMath::IsFinite(Target.Z))
		{
			continue;
		}

		TArray<uint32>& BucketIndices = BucketMap.FindOrAdd(GetVineTargetBucketCell(Target, Origin, OutBuffers.BucketSize));
		BucketIndices.Add(uint32(TargetIndex));
	}

	if (BucketMap.Num() == 0)
	{
		EnsureVineTargetBucketDummyBuffers(OutBuffers);
		return false;
	}

	TArray<FIntVector> BucketKeys;
	BucketMap.GetKeys(BucketKeys);
	BucketKeys.Sort([](const FIntVector& A, const FIntVector& B)
	{
		if (A.X != B.X)
		{
			return A.X < B.X;
		}
		if (A.Y != B.Y)
		{
			return A.Y < B.Y;
		}
		return A.Z < B.Z;
	});

	OutBuffers.Ranges.Reserve(BucketKeys.Num());
	OutBuffers.RangeCounts.Reserve(BucketKeys.Num());
	OutBuffers.VoxelIndices.Reserve(TargetPositions.Num());
	for (const FIntVector& BucketKey : BucketKeys)
	{
		const TArray<uint32>* BucketIndices = BucketMap.Find(BucketKey);
		if (!BucketIndices || BucketIndices->Num() == 0)
		{
			continue;
		}
		if (OutBuffers.VoxelIndices.Num() > MAX_int32)
		{
			OutBuffers = FVineTargetBucketBuffers();
			EnsureVineTargetBucketDummyBuffers(OutBuffers);
			return false;
		}

		const int32 RangeStart = OutBuffers.VoxelIndices.Num();
		OutBuffers.Ranges.Add(FIntVector4(BucketKey.X, BucketKey.Y, BucketKey.Z, RangeStart));
		OutBuffers.RangeCounts.Add(uint32(BucketIndices->Num()));
		OutBuffers.MaxBucketItemCount = FMath::Max<uint32>(OutBuffers.MaxBucketItemCount, uint32(BucketIndices->Num()));
		OutBuffers.VoxelIndices.Append(*BucketIndices);
	}

	OutBuffers.BucketCount = uint32(OutBuffers.Ranges.Num());
	if (OutBuffers.BucketCount == 0u || !BuildVineVoxelHashSlots(OutBuffers.Ranges, OutBuffers.HashSlots, OutBuffers.HashSlotCount))
	{
		OutBuffers.HashSlots.Reset();
		OutBuffers.HashSlotCount = 0u;
		EnsureVineTargetBucketDummyBuffers(OutBuffers);
		return false;
	}

	EnsureVineTargetBucketDummyBuffers(OutBuffers);
	return true;
}

// GPU dispatch for voxel-based vine visualization.
// Surface projection samples FCSSurfaceVoxelData on the GPU.
static bool DispatchVineVisualizationGPU_Voxel(
	const TArray<FVector4f>& PathPoints,
	const TArray<FVector4f>& PathPointAxes,
	const TArray<FIntVector4>& PathPointMeta,
	const TArray<float>& PathPointCurveU,
	const TArray<FIntVector4>& SegmentMeta,
	bool bTube,
	float CircleScale,
	float LineScale,
	float VinesOffset,
	int32 PostProjectionSmoothIterations,
	const FCSSurfaceVoxelData& VoxelData,
	TArray<FVector4f>& OutVertices,
	TArray<FVector2f>& OutUVs,
	TArray<uint32>& OutIndices,
	TArray<FVector4f>* OutSurfaceTargets = nullptr)
{
	const double DispatchTotalStartSeconds = FPlatformTime::Seconds();
	double BuildVoxelUploadMs = 0.0;
	double BuildHashMs = 0.0;
	double BuildTargetBucketsMs = 0.0;
	double EnqueueAndFlushMs = 0.0;
	double ReadbackFlushMs = 0.0;

	OutVertices.Reset();
	OutUVs.Reset();
	OutIndices.Reset();

	const uint32 PathPointCount = uint32(PathPoints.Num());
	const uint32 SegmentCount = uint32(SegmentMeta.Num());
	const uint32 ProfileCount = bTube ? 3u : 2u;
	const uint32 OutputVertexCount = PathPointCount * ProfileCount;
	const uint32 OutputIndexCount = bTube ? SegmentCount * ProfileCount * 6u : SegmentCount * 6u;
	const int32 SafePostProjectionSmoothIterations = FMath::Max(0, PostProjectionSmoothIterations);
	if (PathPointCount == 0
		|| uint32(PathPointCurveU.Num()) != PathPointCount
		|| SegmentCount == 0
		|| OutputVertexCount == 0
		|| OutputIndexCount == 0)
	{
		return false;
	}

	const uint32 VoxelCount = uint32(VoxelData.Cells.Num());
	if (VoxelCount == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU_Voxel] No voxel data available."));
		return false;
	}

	// Convert voxel data to GPU-compatible format
	TArray<FVector4f> GPUVoxelTargetPositions;
	TArray<FVector4f> GPUVoxelNormals;
	TArray<FIntVector4> GPUVoxelCells;
	GPUVoxelTargetPositions.Reserve(VoxelCount);
	GPUVoxelNormals.Reserve(VoxelCount);
	GPUVoxelCells.Reserve(VoxelCount);

	auto IsFiniteVector = [](const FVector& Vector)
	{
		return FMath::IsFinite(Vector.X) && FMath::IsFinite(Vector.Y) && FMath::IsFinite(Vector.Z);
	};

	const float SafeVoxelSize = FMath::Max(VoxelData.VoxelSize, UE_KINDA_SMALL_NUMBER);
	const double MaxTargetDistanceSq = FMath::Square(double(SafeVoxelSize * 2.0f));
	int32 InvalidTargetCount = 0;
	int32 InvalidNormalCount = 0;
	int32 CoincidentTargetCount = 0;
	double TargetCenterDistanceSum = 0.0;
	double TargetCenterDistanceMax = 0.0;
	const double BuildVoxelUploadStartSeconds = FPlatformTime::Seconds();
	for (uint32 i = 0; i < VoxelCount; ++i)
	{
		const FIntVector& Cell = VoxelData.Cells[i];
		const FVector CellCenter(
			(double(Cell.X) + 0.5) * SafeVoxelSize + VoxelData.VoxelOrigin.X,
			(double(Cell.Y) + 0.5) * SafeVoxelSize + VoxelData.VoxelOrigin.Y,
			(double(Cell.Z) + 0.5) * SafeVoxelSize + VoxelData.VoxelOrigin.Z);
		const FVector VoxelCenter = VoxelData.Positions.IsValidIndex(int32(i)) && IsFiniteVector(VoxelData.Positions[i])
			? VoxelData.Positions[i]
			: CellCenter;

		FVector Target = VoxelData.TargetPositions.IsValidIndex(int32(i)) ? VoxelData.TargetPositions[i] : VoxelCenter;
		if (!IsFiniteVector(Target) || FVector::DistSquared(Target, VoxelCenter) > MaxTargetDistanceSq)
		{
			Target = VoxelCenter;
			++InvalidTargetCount;
		}
		const double TargetCenterDistance = FVector::Dist(Target, VoxelCenter);
		TargetCenterDistanceSum += TargetCenterDistance;
		TargetCenterDistanceMax = FMath::Max(TargetCenterDistanceMax, TargetCenterDistance);
		if (TargetCenterDistance <= UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			++CoincidentTargetCount;
		}

		FVector Normal = VoxelData.Normals.IsValidIndex(int32(i)) ? VoxelData.Normals[i] : FVector::UpVector;
		if (!IsFiniteVector(Normal) || !Normal.Normalize())
		{
			Normal = FVector::UpVector;
			++InvalidNormalCount;
		}

		GPUVoxelTargetPositions.Add(FVector4f(float(Target.X), float(Target.Y), float(Target.Z), 1.0f));
		GPUVoxelNormals.Add(FVector4f(float(Normal.X), float(Normal.Y), float(Normal.Z), 0.0f));
		GPUVoxelCells.Add(FIntVector4(Cell.X, Cell.Y, Cell.Z, 0));
	}
	BuildVoxelUploadMs = (FPlatformTime::Seconds() - BuildVoxelUploadStartSeconds) * 1000.0;

	if (InvalidTargetCount > 0 || InvalidNormalCount > 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[VisVineGPU_Voxel] Sanitized voxel upload. Voxels=%u InvalidTargets=%d InvalidNormals=%d"),
			VoxelCount,
			InvalidTargetCount,
			InvalidNormalCount);
	}
	UE_LOG(LogTemp, Display,
		TEXT("[VisVineGPU_Voxel] Upload target stats. Voxels=%u TargetCenterDist(avg=%.3f max=%.3f) CoincidentTargets=%d InvalidTargets=%d InvalidNormals=%d"),
		VoxelCount,
		VoxelCount > 0u ? TargetCenterDistanceSum / double(VoxelCount) : 0.0,
		TargetCenterDistanceMax,
		CoincidentTargetCount,
		InvalidTargetCount,
		InvalidNormalCount);

	TArray<uint32> GPUVoxelHashSlots;
	uint32 GPUVoxelHashSlotCount = 0u;
	const double BuildHashStartSeconds = FPlatformTime::Seconds();
	if (!BuildVineVoxelHashSlots(GPUVoxelCells, GPUVoxelHashSlots, GPUVoxelHashSlotCount))
	{
		GPUVoxelHashSlots.Init(0u, 1);
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU_Voxel] Failed to build voxel hash slots; shader will use linear voxel lookup fallback."));
	}
	BuildHashMs = (FPlatformTime::Seconds() - BuildHashStartSeconds) * 1000.0;

	FVineTargetBucketBuffers TargetBuckets;
	const double BuildTargetBucketsStartSeconds = FPlatformTime::Seconds();
	const FVector3f TargetBucketOrigin = FVector3f(VoxelData.VoxelOrigin);
	if (!BuildVineTargetBucketBuffers(GPUVoxelTargetPositions, TargetBucketOrigin, SafeVoxelSize, TargetBuckets))
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU_Voxel] Failed to build target-position buckets; shader will use voxel-cell fallback."));
	}
	BuildTargetBucketsMs = (FPlatformTime::Seconds() - BuildTargetBucketsStartSeconds) * 1000.0;

	const uint64 VertexReadbackBytes64 = uint64(OutputVertexCount) * sizeof(FVector4f);
	const uint64 UVReadbackBytes64 = uint64(OutputVertexCount) * sizeof(FVector2f);
	const uint64 IndexReadbackBytes64 = uint64(OutputIndexCount) * sizeof(uint32);
	const uint64 SurfaceTargetReadbackBytes64 = uint64(PathPointCount) * sizeof(FVector4f);
	if (VertexReadbackBytes64 > MAX_uint32 || UVReadbackBytes64 > MAX_uint32 || IndexReadbackBytes64 > MAX_uint32 || SurfaceTargetReadbackBytes64 > MAX_uint32)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU_Voxel] Output is too large for readback. Vertices=%u Indices=%u"), OutputVertexCount, OutputIndexCount);
		return false;
	}

	const uint32 VertexReadbackBytes = uint32(VertexReadbackBytes64);
	const uint32 UVReadbackBytes = uint32(UVReadbackBytes64);
	const uint32 IndexReadbackBytes = uint32(IndexReadbackBytes64);
	const uint32 SurfaceTargetReadbackBytes = uint32(SurfaceTargetReadbackBytes64);
	FRHIGPUBufferReadback* VertexReadback = new FRHIGPUBufferReadback(TEXT("VineVisualizationVoxel_VertexReadback"));
	FRHIGPUBufferReadback* UVReadback = new FRHIGPUBufferReadback(TEXT("VineVisualizationVoxel_UVReadback"));
	FRHIGPUBufferReadback* IndexReadback = new FRHIGPUBufferReadback(TEXT("VineVisualizationVoxel_IndexReadback"));
	FRHIGPUBufferReadback* SurfaceTargetReadback = new FRHIGPUBufferReadback(TEXT("VineVisualizationVoxel_SurfaceTargetReadback"));
	bool bRenderWorkQueued = false;

	const double EnqueueAndFlushStartSeconds = FPlatformTime::Seconds();
	ENQUEUE_RENDER_COMMAND(VineVisualizationVoxelGPU)(
		[PathPoints, PathPointAxes, PathPointMeta, PathPointCurveU, SegmentMeta,
		 GPUVoxelCells, GPUVoxelHashSlots, GPUVoxelNormals, GPUVoxelTargetPositions,
		 TargetBuckets, TargetBucketOrigin,
		 VertexReadback, UVReadback, IndexReadback, SurfaceTargetReadback, VertexReadbackBytes, UVReadbackBytes, IndexReadbackBytes, SurfaceTargetReadbackBytes,
		 PathPointCount, SegmentCount, VoxelCount, ProfileCount, OutputVertexCount, OutputIndexCount,
		 bTube, CircleScale, LineScale, VinesOffset, SafePostProjectionSmoothIterations, GPUVoxelHashSlotCount,
		 VoxelOrigin = FVector3f(VoxelData.VoxelOrigin), VoxelSize = float(VoxelData.VoxelSize),
		 &bRenderWorkQueued](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			const CSHepler::FRDGStructuredBufferRefs PathPointBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, PathPoints, TEXT("VineVisualizationVoxel.PathPoints"));
			const CSHepler::FRDGStructuredBufferRefs PathPointAxisBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, PathPointAxes, TEXT("VineVisualizationVoxel.PathPointAxes"));
			const CSHepler::FRDGStructuredBufferRefs PathPointMetaBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, PathPointMeta, TEXT("VineVisualizationVoxel.PathPointMeta"));
			const CSHepler::FRDGStructuredBufferRefs PathPointCurveUBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, PathPointCurveU, TEXT("VineVisualizationVoxel.PathPointCurveU"));
			const CSHepler::FRDGStructuredBufferRefs SegmentMetaBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, SegmentMeta, TEXT("VineVisualizationVoxel.SegmentMeta"));
			const CSHepler::FRDGStructuredBufferRefs VoxelCellsBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, GPUVoxelCells, TEXT("VineVisualizationVoxel.VoxelCells"));
			const CSHepler::FRDGStructuredBufferRefs VoxelHashSlotsBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, GPUVoxelHashSlots, TEXT("VineVisualizationVoxel.VoxelHashSlots"));
			const CSHepler::FRDGStructuredBufferRefs VoxelNormalsBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, GPUVoxelNormals, TEXT("VineVisualizationVoxel.VoxelNormals"));
			const CSHepler::FRDGStructuredBufferRefs VoxelTargetPositionsBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, GPUVoxelTargetPositions, TEXT("VineVisualizationVoxel.VoxelTargetPositions"));
			const CSHepler::FRDGStructuredBufferRefs TargetBucketRangesBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, TargetBuckets.Ranges, TEXT("VineVisualizationVoxel.TargetBucketRanges"));
			const CSHepler::FRDGStructuredBufferRefs TargetBucketRangeCountsBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, TargetBuckets.RangeCounts, TEXT("VineVisualizationVoxel.TargetBucketRangeCounts"));
			const CSHepler::FRDGStructuredBufferRefs TargetBucketVoxelIndicesBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, TargetBuckets.VoxelIndices, TEXT("VineVisualizationVoxel.TargetBucketVoxelIndices"));
			const CSHepler::FRDGStructuredBufferRefs TargetBucketHashSlotsBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, TargetBuckets.HashSlots, TEXT("VineVisualizationVoxel.TargetBucketHashSlots"));
			const CSHepler::FRDGStructuredBufferRefs OutVertexBuffer = CSHepler::CreateStructuredBuffer<FVector4f>(GraphBuilder, OutputVertexCount, TEXT("VineVisualizationVoxel.OutVertices"), true, true);
			const CSHepler::FRDGStructuredBufferRefs OutUVBuffer = CSHepler::CreateStructuredBuffer<FVector2f>(GraphBuilder, OutputVertexCount, TEXT("VineVisualizationVoxel.OutUVs"), true, true);
			const CSHepler::FRDGStructuredBufferRefs OutIndexBuffer = CSHepler::CreateStructuredBuffer<uint32>(GraphBuilder, OutputIndexCount, TEXT("VineVisualizationVoxel.OutIndices"), true, true);
			CSHepler::FRDGStructuredBufferRefs PathPointTangentA = CSHepler::CreateStructuredBuffer<FVector4f>(GraphBuilder, PathPointCount, TEXT("VineVisualizationVoxel.PathPointTangentsA"), true, true);
			CSHepler::FRDGStructuredBufferRefs PathPointNormalA = CSHepler::CreateStructuredBuffer<FVector4f>(GraphBuilder, PathPointCount, TEXT("VineVisualizationVoxel.PathPointNormalsA"), true, true);
			CSHepler::FRDGStructuredBufferRefs PathPointSurfaceTargetA = CSHepler::CreateStructuredBuffer<FVector4f>(GraphBuilder, PathPointCount, TEXT("VineVisualizationVoxel.PathPointSurfaceTargetsA"), true, true);
			CSHepler::FRDGStructuredBufferRefs PathPointSurfaceNormalA = CSHepler::CreateStructuredBuffer<FVector4f>(GraphBuilder, PathPointCount, TEXT("VineVisualizationVoxel.PathPointSurfaceNormalsA"), true, true);
			CSHepler::FRDGStructuredBufferRefs PathPointSurfaceTargetB = CSHepler::CreateStructuredBuffer<FVector4f>(GraphBuilder, PathPointCount, TEXT("VineVisualizationVoxel.PathPointSurfaceTargetsB"), true, true);
			CSHepler::FRDGStructuredBufferRefs PathPointSurfaceNormalB = CSHepler::CreateStructuredBuffer<FVector4f>(GraphBuilder, PathPointCount, TEXT("VineVisualizationVoxel.PathPointSurfaceNormalsB"), true, true);

			TShaderMapRef<FVineVisualizationVoxelBuildAxesCS> BuildAxesShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FVineVisualizationVoxelBuildAxesCS::FParameters* BuildAxesParameters = GraphBuilder.AllocParameters<FVineVisualizationVoxelBuildAxesCS::FParameters>();
			BuildAxesParameters->PathPoints = PathPointBuffer.SRV;
			BuildAxesParameters->PathPointAxes = PathPointAxisBuffer.SRV;
			BuildAxesParameters->PathPointMeta = PathPointMetaBuffer.SRV;
			BuildAxesParameters->VoxelCells = VoxelCellsBuffer.SRV;
			BuildAxesParameters->VoxelHashSlots = VoxelHashSlotsBuffer.SRV;
			BuildAxesParameters->VoxelNormals = VoxelNormalsBuffer.SRV;
			BuildAxesParameters->VoxelTargetPositions = VoxelTargetPositionsBuffer.SRV;
			BuildAxesParameters->TargetBucketRanges = TargetBucketRangesBuffer.SRV;
			BuildAxesParameters->TargetBucketRangeCounts = TargetBucketRangeCountsBuffer.SRV;
			BuildAxesParameters->TargetBucketVoxelIndices = TargetBucketVoxelIndicesBuffer.SRV;
			BuildAxesParameters->TargetBucketHashSlots = TargetBucketHashSlotsBuffer.SRV;
			BuildAxesParameters->RW_PathPointTangents = PathPointTangentA.UAV;
			BuildAxesParameters->RW_PathPointNormals = PathPointNormalA.UAV;
			BuildAxesParameters->RW_PathPointSurfaceTargets = PathPointSurfaceTargetA.UAV;
			BuildAxesParameters->RW_PathPointSurfaceNormals = PathPointSurfaceNormalA.UAV;
			BuildAxesParameters->PathPointCount = PathPointCount;
			BuildAxesParameters->VoxelOrigin = VoxelOrigin;
			BuildAxesParameters->VoxelSize = VoxelSize;
			BuildAxesParameters->VoxelCount = VoxelCount;
			BuildAxesParameters->VoxelHashSlotCount = GPUVoxelHashSlotCount;
			BuildAxesParameters->TargetBucketOrigin = TargetBucketOrigin;
			BuildAxesParameters->TargetBucketSize = TargetBuckets.BucketSize;
			BuildAxesParameters->TargetBucketCount = TargetBuckets.BucketCount;
			BuildAxesParameters->TargetBucketHashSlotCount = TargetBuckets.HashSlotCount;
			BuildAxesParameters->TargetBucketSearchRadius = TargetBuckets.SearchRadius;

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("VineVisualizationVoxel.BuildAxes"),
				BuildAxesParameters,
				ERDGPassFlags::Compute,
				[BuildAxesParameters, BuildAxesShader, PathPointCount](FRHIComputeCommandList& InRHICmdList)
				{
					FComputeShaderUtils::Dispatch(InRHICmdList, BuildAxesShader, *BuildAxesParameters, FComputeShaderUtils::GetGroupCount(FIntVector(PathPointCount, 1, 1), 64));
			});

			TShaderMapRef<FVineVisualizationVoxelSmoothProjectionCS> SmoothProjectionShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			for (int32 SmoothIterationIndex = 0; SmoothIterationIndex < SafePostProjectionSmoothIterations; ++SmoothIterationIndex)
			{
				const bool bReadFromA = (SmoothIterationIndex % 2) == 0;
				CSHepler::FRDGStructuredBufferRefs& ReadSurfaceTarget = bReadFromA ? PathPointSurfaceTargetA : PathPointSurfaceTargetB;
				CSHepler::FRDGStructuredBufferRefs& ReadSurfaceNormal = bReadFromA ? PathPointSurfaceNormalA : PathPointSurfaceNormalB;
				CSHepler::FRDGStructuredBufferRefs& WriteSurfaceTarget = bReadFromA ? PathPointSurfaceTargetB : PathPointSurfaceTargetA;
				CSHepler::FRDGStructuredBufferRefs& WriteSurfaceNormal = bReadFromA ? PathPointSurfaceNormalB : PathPointSurfaceNormalA;

				FVineVisualizationVoxelSmoothProjectionCS::FParameters* SmoothParameters = GraphBuilder.AllocParameters<FVineVisualizationVoxelSmoothProjectionCS::FParameters>();
				SmoothParameters->PathPointSurfaceTargets = ReadSurfaceTarget.SRV;
				SmoothParameters->PathPointSurfaceNormals = ReadSurfaceNormal.SRV;
				SmoothParameters->PathPointMeta = PathPointMetaBuffer.SRV;
				SmoothParameters->RW_PathPointSurfaceTargets = WriteSurfaceTarget.UAV;
				SmoothParameters->RW_PathPointSurfaceNormals = WriteSurfaceNormal.UAV;
				SmoothParameters->PathPointCount = PathPointCount;

				GraphBuilder.AddPass(
					RDG_EVENT_NAME("VineVisualizationVoxel.SmoothProjection%d", SmoothIterationIndex),
					SmoothParameters,
					ERDGPassFlags::Compute,
					[SmoothParameters, SmoothProjectionShader, PathPointCount](FRHIComputeCommandList& InRHICmdList)
					{
						FComputeShaderUtils::Dispatch(InRHICmdList, SmoothProjectionShader, *SmoothParameters, FComputeShaderUtils::GetGroupCount(FIntVector(PathPointCount, 1, 1), 64));
					});
			}

			const CSHepler::FRDGStructuredBufferRefs& FinalSurfaceTargetBuffer = (SafePostProjectionSmoothIterations % 2) == 0 ? PathPointSurfaceTargetA : PathPointSurfaceTargetB;
			const CSHepler::FRDGStructuredBufferRefs& FinalSurfaceNormalBuffer = (SafePostProjectionSmoothIterations % 2) == 0 ? PathPointSurfaceNormalA : PathPointSurfaceNormalB;

			TShaderMapRef<FVineVisualizationVoxelCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FVineVisualizationVoxelCS::FParameters* Parameters = GraphBuilder.AllocParameters<FVineVisualizationVoxelCS::FParameters>();
			Parameters->PathPoints = PathPointBuffer.SRV;
			Parameters->PathPointMeta = PathPointMetaBuffer.SRV;
			Parameters->PathPointCurveU = PathPointCurveUBuffer.SRV;
			Parameters->PathPointTangents = PathPointTangentA.SRV;
			Parameters->PathPointNormals = PathPointNormalA.SRV;
			Parameters->PathPointSurfaceTargets = FinalSurfaceTargetBuffer.SRV;
			Parameters->PathPointSurfaceNormals = FinalSurfaceNormalBuffer.SRV;
			Parameters->SegmentMeta = SegmentMetaBuffer.SRV;
			Parameters->VoxelCells = VoxelCellsBuffer.SRV;
			Parameters->VoxelHashSlots = VoxelHashSlotsBuffer.SRV;
			Parameters->VoxelNormals = VoxelNormalsBuffer.SRV;
			Parameters->VoxelTargetPositions = VoxelTargetPositionsBuffer.SRV;
			Parameters->TargetBucketRanges = TargetBucketRangesBuffer.SRV;
			Parameters->TargetBucketRangeCounts = TargetBucketRangeCountsBuffer.SRV;
			Parameters->TargetBucketVoxelIndices = TargetBucketVoxelIndicesBuffer.SRV;
			Parameters->TargetBucketHashSlots = TargetBucketHashSlotsBuffer.SRV;
			Parameters->RW_OutVertices = OutVertexBuffer.UAV;
			Parameters->RW_OutUVs = OutUVBuffer.UAV;
			Parameters->RW_OutIndices = OutIndexBuffer.UAV;
			Parameters->PathPointCount = PathPointCount;
			Parameters->SegmentCount = SegmentCount;
			Parameters->OutputVertexCount = OutputVertexCount;
			Parameters->OutputIndexCount = OutputIndexCount;
			Parameters->ProfileCount = ProfileCount;
			Parameters->bTube = bTube ? 1u : 0u;
			Parameters->CircleScale = CircleScale;
			Parameters->LineScale = LineScale;
			Parameters->VoxelOrigin = VoxelOrigin;
			Parameters->VoxelSize = VoxelSize;
			Parameters->VoxelCount = VoxelCount;
			Parameters->VoxelHashSlotCount = GPUVoxelHashSlotCount;
			Parameters->TargetBucketOrigin = TargetBucketOrigin;
			Parameters->TargetBucketSize = TargetBuckets.BucketSize;
			Parameters->TargetBucketCount = TargetBuckets.BucketCount;
			Parameters->TargetBucketHashSlotCount = TargetBuckets.HashSlotCount;
			Parameters->TargetBucketSearchRadius = TargetBuckets.SearchRadius;
			Parameters->VinesOffset = VinesOffset;

			const uint32 DispatchCount = FMath::Max(OutputVertexCount, SegmentCount);
			GraphBuilder.AddPass(
				RDG_EVENT_NAME("VineVisualizationVoxelCS"),
				Parameters,
				ERDGPassFlags::Compute,
				[Parameters, ComputeShader, DispatchCount](FRHIComputeCommandList& RHICmdList)
				{
					const uint32 GroupCountX = FMath::DivideAndRoundUp(DispatchCount, 64u);
					SetComputePipelineState(RHICmdList, ComputeShader.GetComputeShader());
					SetShaderParameters(RHICmdList, ComputeShader, ComputeShader.GetComputeShader(), *Parameters);
					RHICmdList.DispatchComputeShader(GroupCountX, 1, 1);
					UnsetShaderUAVs(RHICmdList, ComputeShader, ComputeShader.GetComputeShader());
				});

			AddEnqueueCopyPass(GraphBuilder, VertexReadback, OutVertexBuffer.Buffer, VertexReadbackBytes);
			AddEnqueueCopyPass(GraphBuilder, UVReadback, OutUVBuffer.Buffer, UVReadbackBytes);
			AddEnqueueCopyPass(GraphBuilder, IndexReadback, OutIndexBuffer.Buffer, IndexReadbackBytes);
			AddEnqueueCopyPass(GraphBuilder, SurfaceTargetReadback, FinalSurfaceTargetBuffer.Buffer, SurfaceTargetReadbackBytes);
			GraphBuilder.Execute();
			bRenderWorkQueued = true;
		});

	FlushRenderingCommands();
	EnqueueAndFlushMs = (FPlatformTime::Seconds() - EnqueueAndFlushStartSeconds) * 1000.0;

	if (!bRenderWorkQueued)
	{
		delete VertexReadback;
		delete UVReadback;
		delete IndexReadback;
		delete SurfaceTargetReadback;
		return false;
	}

	OutVertices.SetNumZeroed(OutputVertexCount);
	OutUVs.SetNumZeroed(OutputVertexCount);
	OutIndices.SetNumZeroed(OutputIndexCount);
	if (OutSurfaceTargets)
	{
		OutSurfaceTargets->SetNumZeroed(PathPointCount);
	}
	bool bReadbackSucceeded = false;

	const double ReadbackFlushStartSeconds = FPlatformTime::Seconds();
	ENQUEUE_RENDER_COMMAND(VineVisualizationVoxelGPUReadback)(
		[VertexReadback, UVReadback, IndexReadback, SurfaceTargetReadback, VertexReadbackBytes, UVReadbackBytes, IndexReadbackBytes, SurfaceTargetReadbackBytes, &OutVertices, &OutUVs, &OutIndices, OutSurfaceTargets, &bReadbackSucceeded](FRHICommandListImmediate& RHICmdList)
		{
			if (!VertexReadback || !UVReadback || !IndexReadback || !SurfaceTargetReadback)
			{
				return;
			}

			if (!VertexReadback->IsReady() || !UVReadback->IsReady() || !IndexReadback->IsReady() || !SurfaceTargetReadback->IsReady())
			{
				RHICmdList.SubmitAndBlockUntilGPUIdle();
			}

			bool bLockedAll = true;
			if (const FVector4f* VertexPtr = static_cast<const FVector4f*>(VertexReadback->Lock(VertexReadbackBytes)))
			{
				FMemory::Memcpy(OutVertices.GetData(), VertexPtr, VertexReadbackBytes);
				VertexReadback->Unlock();
			}
			else
			{
				bLockedAll = false;
			}

			if (const FVector2f* UVPtr = static_cast<const FVector2f*>(UVReadback->Lock(UVReadbackBytes)))
			{
				FMemory::Memcpy(OutUVs.GetData(), UVPtr, UVReadbackBytes);
				UVReadback->Unlock();
			}
			else
			{
				bLockedAll = false;
			}

			if (const uint32* IndexPtr = static_cast<const uint32*>(IndexReadback->Lock(IndexReadbackBytes)))
			{
				FMemory::Memcpy(OutIndices.GetData(), IndexPtr, IndexReadbackBytes);
				IndexReadback->Unlock();
			}
			else
			{
				bLockedAll = false;
			}

			if (OutSurfaceTargets)
			{
				if (const FVector4f* SurfaceTargetPtr = static_cast<const FVector4f*>(SurfaceTargetReadback->Lock(SurfaceTargetReadbackBytes)))
				{
					FMemory::Memcpy(OutSurfaceTargets->GetData(), SurfaceTargetPtr, SurfaceTargetReadbackBytes);
					SurfaceTargetReadback->Unlock();
				}
				else
				{
					bLockedAll = false;
				}
			}

			delete VertexReadback;
			delete UVReadback;
			delete IndexReadback;
			delete SurfaceTargetReadback;
			bReadbackSucceeded = bLockedAll;
		});

	FlushRenderingCommands();
	ReadbackFlushMs = (FPlatformTime::Seconds() - ReadbackFlushStartSeconds) * 1000.0;
	UE_LOG(LogTemp, Display,
		TEXT("[VisVineGPUDispatchTiming] %s total=%.3f ms buildVoxelUpload=%.3f ms buildHash=%.3f ms buildTargetBuckets=%.3f ms enqueueAndFlush=%.3f ms readbackFlush=%.3f ms pathPoints=%u voxels=%u targetBuckets=%u targetBucketSize=%.3f targetSearchRadius=%u targetSearchCoverage=%.3f targetBucketAvgItems=%.3f targetBucketMaxItems=%u outVerts=%u outIndices=%u"),
		bTube ? TEXT("tube") : TEXT("plane"),
		(FPlatformTime::Seconds() - DispatchTotalStartSeconds) * 1000.0,
		BuildVoxelUploadMs,
		BuildHashMs,
		BuildTargetBucketsMs,
		EnqueueAndFlushMs,
		ReadbackFlushMs,
		PathPointCount,
		VoxelCount,
		TargetBuckets.BucketCount,
		TargetBuckets.BucketSize,
		TargetBuckets.SearchRadius,
		TargetBuckets.BucketSize * float(TargetBuckets.SearchRadius),
		TargetBuckets.BucketCount > 0u ? double(TargetBuckets.VoxelIndices.Num()) / double(TargetBuckets.BucketCount) : 0.0,
		TargetBuckets.MaxBucketItemCount,
		OutputVertexCount,
		OutputIndexCount);
	return bReadbackSucceeded;
}

static FVector GetVineOutputProfileCenter(
	const TArray<FVector4f>& Vertices,
	int32 PointIndex,
	uint32 ProfileCount)
{
	FVector Center = FVector::ZeroVector;
	if (ProfileCount == 0)
	{
		return Center;
	}

	const int32 BaseIndex = PointIndex * int32(ProfileCount);
	for (uint32 ProfileIndex = 0; ProfileIndex < ProfileCount; ++ProfileIndex)
	{
		const int32 VertexIndex = BaseIndex + int32(ProfileIndex);
		if (!Vertices.IsValidIndex(VertexIndex))
		{
			continue;
		}

		const FVector4f& Vertex = Vertices[VertexIndex];
		Center += FVector(Vertex.X, Vertex.Y, Vertex.Z);
	}
	return Center / double(ProfileCount);
}

static void LogVineGPUProjectionStats(
	const TCHAR* Label,
	const TArray<FVector4f>& PathPoints,
	const TArray<FVector4f>& SurfaceTargets,
	const TArray<FVector4f>& OutputVertices,
	uint32 ProfileCount,
	float VinesOffset)
{
	const int32 OutputPointCount = ProfileCount > 0u ? OutputVertices.Num() / int32(ProfileCount) : 0;
	const int32 PointCount = FMath::Min(FMath::Min(PathPoints.Num(), SurfaceTargets.Num()), OutputPointCount);
	if (PointCount <= 0)
	{
		return;
	}

	int32 HitCount = 0;
	int32 NeighborHitCount = 0;
	int32 LocalHitCount = 0;
	int32 BucketHitCount = 0;
	int32 FailCount = 0;
	int32 UnknownModeCount = 0;
	double ProjectionDistanceSum = 0.0;
	double ProjectionDistanceMax = 0.0;
	double FinalTargetDistanceSum = 0.0;
	double FinalTargetDistanceMin = TNumericLimits<double>::Max();
	double FinalTargetDistanceMax = 0.0;
	double FinalOffsetErrorSum = 0.0;
	double FinalOffsetErrorMax = 0.0;

	for (int32 PointIndex = 0; PointIndex < PointCount; ++PointIndex)
	{
		const FVector4f& PackedTarget = SurfaceTargets[PointIndex];
		const int32 SampleMode = FMath::RoundToInt(PackedTarget.W);
		if (SampleMode <= 0)
		{
			++FailCount;
			continue;
		}

		switch (SampleMode)
		{
		case 1:
			++NeighborHitCount;
			break;
		case 2:
			++LocalHitCount;
			break;
		case 3:
			++BucketHitCount;
			break;
		default:
			++UnknownModeCount;
			break;
		}

		const FVector Query(PathPoints[PointIndex].X, PathPoints[PointIndex].Y, PathPoints[PointIndex].Z);
		const FVector Target(PackedTarget.X, PackedTarget.Y, PackedTarget.Z);
		if (!IsFiniteVineVector(Query) || !IsFiniteVineVector(Target))
		{
			continue;
		}

		const FVector Center = GetVineOutputProfileCenter(OutputVertices, PointIndex, ProfileCount);
		if (!IsFiniteVineVector(Center))
		{
			continue;
		}

		++HitCount;
		const double ProjectionDistance = FVector::Dist(Query, Target);
		const double FinalTargetDistance = FVector::Dist(Center, Target);
		const double FinalOffsetError = FMath::Abs(FinalTargetDistance - double(VinesOffset));
		ProjectionDistanceSum += ProjectionDistance;
		ProjectionDistanceMax = FMath::Max(ProjectionDistanceMax, ProjectionDistance);
		FinalTargetDistanceSum += FinalTargetDistance;
		FinalTargetDistanceMin = FMath::Min(FinalTargetDistanceMin, FinalTargetDistance);
		FinalTargetDistanceMax = FMath::Max(FinalTargetDistanceMax, FinalTargetDistance);
		FinalOffsetErrorSum += FinalOffsetError;
		FinalOffsetErrorMax = FMath::Max(FinalOffsetErrorMax, FinalOffsetError);
	}

	UE_LOG(LogTemp, Display,
		TEXT("[VisVineGPUProjectionStats] %s sampleHits=%d/%d modes(neighbor=%d local=%d bucket=%d fail=%d unknown=%d) projectionDist(avg=%.3f max=%.3f) finalToTarget(avg=%.3f min=%.3f max=%.3f expectedOffset=%.3f offsetError(avg=%.3f max=%.3f)"),
		Label ? Label : TEXT("unknown"),
		HitCount,
		PointCount,
		NeighborHitCount,
		LocalHitCount,
		BucketHitCount,
		FailCount,
		UnknownModeCount,
		HitCount > 0 ? ProjectionDistanceSum / double(HitCount) : 0.0,
		ProjectionDistanceMax,
		HitCount > 0 ? FinalTargetDistanceSum / double(HitCount) : 0.0,
		HitCount > 0 ? FinalTargetDistanceMin : 0.0,
		FinalTargetDistanceMax,
		VinesOffset,
		HitCount > 0 ? FinalOffsetErrorSum / double(HitCount) : 0.0,
		FinalOffsetErrorMax);
}

static void RecomputeVineOutputUVsFromGeneratedLength(
	const TArray<FVector4f>& Vertices,
	const TArray<FIntVector4>& SegmentMeta,
	uint32 ProfileCount,
	TArray<FVector2f>& UVs)
{
	if (ProfileCount == 0 || Vertices.Num() == 0 || UVs.Num() != Vertices.Num())
	{
		return;
	}

	const int32 PointCount = Vertices.Num() / int32(ProfileCount);
	if (PointCount <= 0)
	{
		return;
	}

	TArray<float> GeneratedCurveU;
	GeneratedCurveU.SetNumZeroed(PointCount);

	for (const FIntVector4& Segment : SegmentMeta)
	{
		const int32 APoint = Segment.X;
		const int32 BPoint = Segment.Y;
		if (!GeneratedCurveU.IsValidIndex(APoint) || !GeneratedCurveU.IsValidIndex(BPoint))
		{
			continue;
		}

		const FVector ACenter = GetVineOutputProfileCenter(Vertices, APoint, ProfileCount);
		const FVector BCenter = GetVineOutputProfileCenter(Vertices, BPoint, ProfileCount);
		GeneratedCurveU[BPoint] = GeneratedCurveU[APoint] + float(FVector::Dist(ACenter, BCenter));
	}

	for (int32 PointIndex = 0; PointIndex < PointCount; ++PointIndex)
	{
		const float V = GeneratedCurveU[PointIndex];
		const int32 BaseIndex = PointIndex * int32(ProfileCount);
		for (uint32 ProfileIndex = 0; ProfileIndex < ProfileCount; ++ProfileIndex)
		{
			const int32 VertexIndex = BaseIndex + int32(ProfileIndex);
			if (UVs.IsValidIndex(VertexIndex))
			{
				UVs[VertexIndex].Y = V;
			}
		}
	}
}

static UDynamicMesh* BuildDynamicMeshFromGPUVineOutput(
	UObject* Outer,
	const TArray<FVector4f>& Vertices,
	const TArray<FVector2f>& UVs,
	const TArray<uint32>& Indices,
	int32 MaterialID,
	bool bRecomputeNormals)
{
	if (Vertices.Num() == 0 || UVs.Num() != Vertices.Num() || Indices.Num() < 3)
	{
		return nullptr;
	}

	UDynamicMesh* OutMesh = NewObject<UDynamicMesh>(Outer);
	if (!OutMesh)
	{
		return nullptr;
	}

	FDynamicMesh3 Mesh;
	Mesh.EnableAttributes();
	Mesh.Attributes()->EnableMaterialID();
	Mesh.Attributes()->SetNumUVLayers(1);

	// Append vertices (position only at this stage)
	for (const FVector4f& Vertex : Vertices)
	{
		Mesh.AppendVertex(FVector3d(Vertex.X, Vertex.Y, Vertex.Z));
	}

	// Append triangles and set per-triangle UVs via the UV overlay
	UE::Geometry::FDynamicMeshUVOverlay* UVOverlay = Mesh.Attributes()->GetUVLayer(0);

	// Pre-create UV elements for each vertex (shared UV per vertex)
	TArray<int32> UVElementIDs;
	UVElementIDs.SetNum(Vertices.Num());
	for (int32 i = 0; i < Vertices.Num(); ++i)
	{
		UVElementIDs[i] = UVOverlay->AppendElement(UVs[i]);
	}

	for (int32 Index = 0; Index + 2 < Indices.Num(); Index += 3)
	{
		const int32 A = int32(Indices[Index + 0]);
		const int32 B = int32(Indices[Index + 1]);
		const int32 C = int32(Indices[Index + 2]);
		if (A < 0 || B < 0 || C < 0 || A >= Vertices.Num() || B >= Vertices.Num() || C >= Vertices.Num() || A == B || B == C || A == C)
		{
			continue;
		}

		const int32 TriangleID = Mesh.AppendTriangle(A, C, B);
		if (TriangleID >= 0)
		{
			Mesh.Attributes()->GetMaterialID()->SetNewValue(TriangleID, MaterialID);
			UVOverlay->SetTriangle(TriangleID, UE::Geometry::FIndex3i(UVElementIDs[A], UVElementIDs[C], UVElementIDs[B]));
		}
	}

	OutMesh->SetMesh(MoveTemp(Mesh));
	if (bRecomputeNormals)
	{
		FGeometryScriptCalculateNormalsOptions CalculateOptions;
		UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(OutMesh, CalculateOptions);
	}
	return OutMesh;
}
}

static void ApplyVineReferenceComponentsHiddenInGame(AVineContainer* Container)
{
	if (!Container)
	{
		return;
	}

	if (Container->GrowTarget)
	{
		Container->GrowTarget->SetHiddenInGame(true);
	}
	if (Container->TubeVineSource)
	{
		Container->TubeVineSource->SetHiddenInGame(true);
	}
	if (Container->PlaneVineSource)
	{
		Container->PlaneVineSource->SetHiddenInGame(true);
	}
}

static void RefreshVineDisplayComponent(UInstancedStaticMeshComponent* Component)
{
	if (!Component)
	{
		return;
	}

	Component->SetVisibility(true, false);
	Component->SetHiddenInGame(true);
	Component->UpdateBounds();
	Component->MarkRenderStateDirty();
}

static void RebuildVineDisplayInstances(UInstancedStaticMeshComponent* Component, const TArray<FTransform>& Transforms)
{
	if (!Component)
	{
		return;
	}

	Component->ClearInstances();
	if (!Transforms.IsEmpty())
	{
		Component->AddInstances(Transforms, false, true, false);
	}
	RefreshVineDisplayComponent(Component);
}

static bool ResolveVineReferenceComponent(
	AVineContainer* Container,
	const UFoliageType* InFoliageType,
	UInstancedStaticMeshComponent*& OutDisplayComponent)
{
	OutDisplayComponent = nullptr;
	if (!Container || !InFoliageType)
	{
		return false;
	}

	const FString FoliageTypeName = InFoliageType->GetName();
	if (InFoliageType == Container->TubeType || FoliageTypeName == TEXT("SMF_TubeVine_FoliageType"))
	{
		OutDisplayComponent = Container->TubeVineSource;
		return true;
	}
	if (InFoliageType == Container->PlaneType || FoliageTypeName == TEXT("SMF_PlaneVine_FoliageType"))
	{
		OutDisplayComponent = Container->PlaneVineSource;
		return true;
	}
	if (InFoliageType == Container->TargetType || FoliageTypeName == TEXT("SMF_Target_FoliageType"))
	{
		OutDisplayComponent = Container->GrowTarget;
		return true;
	}

	return false;
}


AVineContainer::AVineContainer(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)	
{
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	GrowTarget = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("GrowTarget"));
	GrowTarget->SetStaticMesh(Mesh);
	GrowTarget->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Overlap);
	GrowTarget->SetCollisionObjectType(ECollisionChannel::ECC_WorldDynamic);
	GrowTarget->SetHiddenInGame(true);
	GrowTarget->SetupAttachment(GetRootComponent(), TEXT("GrowTarget"));

	TubeVineSource = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("TubePoints"));
	TubeVineSource->SetStaticMesh(Mesh);
	TubeVineSource->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Overlap);
	TubeVineSource->SetCollisionObjectType(ECollisionChannel::ECC_WorldDynamic);
	TubeVineSource->SetVisibility(true, false);
	TubeVineSource->SetHiddenInGame(true);
	TubeVineSource->SetupAttachment(GetRootComponent(), TEXT("TubePoints"));
	
	PlaneVineSource = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("PlanePoints"));
	PlaneVineSource->SetStaticMesh(Mesh);
	PlaneVineSource->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Overlap);
	PlaneVineSource->SetCollisionObjectType(ECollisionChannel::ECC_WorldDynamic);
	PlaneVineSource->SetVisibility(true, false);
	PlaneVineSource->SetHiddenInGame(true);
	PlaneVineSource->SetupAttachment(GetRootComponent(), TEXT("PlanePoints"));

	ApplyVineReferenceComponentsHiddenInGame(this);
	RebuildDisplayInstancesFromTransformArrays();
}

void AVineContainer::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	// ApplyVineReferenceComponentsHiddenInGame(this);
	// RebuildDisplayInstancesFromTransformArrays();
}

void AVineContainer::RebuildDisplayInstancesFromTransformArrays()
{
	RefreshVineDisplayComponent(GrowTarget);
	RefreshVineDisplayComponent(TubeVineSource);
	RefreshVineDisplayComponent(PlaneVineSource);
}

void AVineContainer::ImportFoliageToTransformArray(UFoliageType* InFoliageType)
{
	if (InFoliageType == nullptr)
		return;

	TArray<FTransform> Transforms;
	GetAllFoliageInstanceTransforms(GetWorld(), InFoliageType, Transforms);
	UInstancedStaticMeshComponent* DisplayComponent = nullptr;
	if (!ResolveVineReferenceComponent(this, InFoliageType, DisplayComponent) || !DisplayComponent)
	{
		return;
	}

	Modify();
	DisplayComponent->Modify();
	if (!Transforms.IsEmpty())
	{
		DisplayComponent->AddInstances(Transforms, false, true, false);
	}
	MarkPackageDirty();
	RefreshVineDisplayComponent(DisplayComponent);

	for (TActorIterator<AInstancedFoliageActor> It(GetWorld()); It; ++It)
	{
		AInstancedFoliageActor* IFA = (*It);
		IFA->RemoveFoliageType(&InFoliageType, 1);
	}
}

void AVineContainer::ExportTransformArrayToFoliage(UFoliageType* InFoliageType)
{
	if (InFoliageType == nullptr)
		return;

	UInstancedStaticMeshComponent* DisplayComponent = nullptr;
	if (!ResolveVineReferenceComponent(this, InFoliageType, DisplayComponent) || !DisplayComponent)
		return;

	TArray<FTransform> InstanceTransforms;
	GetVineInstanceTransforms(DisplayComponent, InstanceTransforms);

	if (InstanceTransforms.IsEmpty())
	{
		RefreshVineDisplayComponent(DisplayComponent);
		return;
	}

	UWorld* World = GetWorld();
	if (!World || !World->PersistentLevel)
	{
		return;
	}

	Modify();
	TMap<AInstancedFoliageActor*, TArray<const FFoliageInstance*>> InstancesToAdd;
	TArray<FFoliageInstance> FoliageInstances;
	FoliageInstances.Reserve(InstanceTransforms.Num());

	for (const FTransform& InstanceTransform : InstanceTransforms)
	{
		AInstancedFoliageActor* IFA = AInstancedFoliageActor::Get(World, true, World->PersistentLevel, InstanceTransform.GetLocation());
		if (!IFA)
		{
			continue;
		}

		FFoliageInstance FoliageInstance;
		FoliageInstance.Location = InstanceTransform.GetLocation();
		FoliageInstance.Rotation = InstanceTransform.GetRotation().Rotator();
		FoliageInstance.DrawScale3D = (FVector3f)InstanceTransform.GetScale3D();

		FoliageInstances.Add(FoliageInstance);
		InstancesToAdd.FindOrAdd(IFA).Add(&FoliageInstances[FoliageInstances.Num() - 1]);
	}

	for (const auto& Pair : InstancesToAdd)
	{
		FFoliageInfo* TypeInfo = nullptr;
		if (UFoliageType* FoliageType = Pair.Key->AddFoliageType(InFoliageType, &TypeInfo))
		{
			TypeInfo->AddInstances(FoliageType, Pair.Value);
		}
	}

	DisplayComponent->Modify();
	DisplayComponent->ClearInstances();
	MarkPackageDirty();
	RefreshVineDisplayComponent(DisplayComponent);
	RefreshFoliageType(GetWorld(), InFoliageType);
}

bool AVineContainer::VisVine(bool MainVine, bool bUseGPU)
{
	bUseGPUMode = bUseGPU;
	if (bUseGPU)
	{
		return VisVineGPUInternal(MainVine);
	}

	return VisVineCPU(MainVine);
}

bool AVineContainer::VisVineCPU(bool MainVine)
{
	const TArray<FGeometryScriptPolyPath>& Lines = MainVine ? TubeLines : PlaneLines;
	if (Lines.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineCPU] No %s lines to visualize."), MainVine ? TEXT("tube") : TEXT("plane"));
		return false;
	}

	if (VV.ResampleLength <= KINDA_SMALL_NUMBER)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineCPU] Invalid ResampleLength: %.4f"), VV.ResampleLength);
		return false;
	}

	if (VV.CurveControl == nullptr)
	{
		VV.CurveControl = NewObject<UCurveLinearColor>(this);
	}

	UDynamicMeshComponent* MeshComponent = GetDynamicMeshComponent();
	if (!MeshComponent)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineCPU] DynamicMeshComponent is null."));
		return false;
	}

	UDynamicMesh* ContainerMesh = MeshComponent->GetDynamicMesh();
	if (!ContainerMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineCPU] Container dynamic mesh is null."));
		return false;
	}

	TUniquePtr<FVineTriangleBVHProjectionCache> TriangleProjectionCache = BuildVineTriangleBVHProjectionCache(CachedSurfaceTriangles);
	if (!TriangleProjectionCache || !TriangleProjectionCache->Spatial)
	{
		UE_LOG(LogTemp, Log, TEXT("[VisVineCPU] Refreshing triangle data for CPU BVH visualization."));
		CachedSurfaceTriangles = GetBoxSceneTrianglesFromGPUFiltered(0.0f);
		TriangleProjectionCache = BuildVineTriangleBVHProjectionCache(CachedSurfaceTriangles);
		if (!TriangleProjectionCache || !TriangleProjectionCache->Spatial)
		{
			UE_LOG(LogTemp, Warning, TEXT("[VisVineCPU] No cached triangle data for CPU BVH visualization."));
			return false;
		}
	}

	const TArray<float>& LineSourceScales = MainVine ? TubeLineSourceScales : PlaneLineSourceScales;
	const TArray<FVector>& LineSourceLocations = MainVine ? TubeLineSourceLocations : PlaneLineSourceLocations;
	const TArray<FVineLinePointScaleData>& LinePointScales = MainVine ? TubeLinePointScales : PlaneLinePointScales;

	TArray<FGeometryScriptPolyPath> PreparedLines;
	TArray<float> PreparedLineSourceScales;
	TArray<FVineLinePointScaleData> PreparedLinePointScales;
	auto ProjectCPUToNearestTriangle = [&](const FVector& Query, FVector& OutProjected, FVector& OutNormal)
	{
		return ProjectVinePathToNearestTriangleBVH(Query, *TriangleProjectionCache, OutProjected, OutNormal);
	};
	if (!PrepareVineVisualizationLinesProjected(
		Lines,
		VV,
		MainVine,
		LineSourceScales,
		LineSourceLocations,
		LinePointScales,
		TEXT("CPU_BVH"),
		ProjectCPUToNearestTriangle,
		true,
		PreparedLines,
		PreparedLineSourceScales,
		PreparedLinePointScales))
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineCPU] No valid %s lines after BVH preprocessing."), MainVine ? TEXT("tube") : TEXT("plane"));
		return false;
	}

	UDynamicMesh* VineMesh = NewObject<UDynamicMesh>(this);
	if (!VineMesh)
	{
		return false;
	}

	TArray<FVector2D> Circle =
	{
		FVector2D(10.0, 0.0) * VV.CircleScale,
		FVector2D(-5.0, 8.66) * VV.CircleScale,
		FVector2D(-5.0, -8.66) * VV.CircleScale
	};
	TArray<FVector2D> Line2D =
	{
		FVector2D(-5.0, 0.0) * VV.LineScale,
		FVector2D(5.0, 0.0) * VV.LineScale
	};

	for (int32 LineIdx = 0; LineIdx < PreparedLines.Num(); ++LineIdx)
	{
		FGeometryScriptPolyPath& Line = PreparedLines[LineIdx];
		if (!Line.Path.IsValid())
		{
			continue;
		}

		const TArray<float>* CurrentPointScales = PreparedLinePointScales.IsValidIndex(LineIdx) ? &PreparedLinePointScales[LineIdx].Values : nullptr;
		const float FallbackScale = PreparedLineSourceScales.IsValidIndex(LineIdx) ? PreparedLineSourceScales[LineIdx] : 1.0f;
		const TArray<FVector>& LineVectors = *Line.Path;
		TArray<FTransform> Transforms = UPolyLine::ConvertPolyPathToTransforms(Line, true);
		const int32 TransformCount = Transforms.Num();
		if (TransformCount < 3)
		{
			continue;
		}

		for (int32 TransformIndex = 0; TransformIndex < TransformCount; ++TransformIndex)
		{
			FTransform& Transform = Transforms[TransformIndex];
			const float CurveScale = VV.CurveControl->GetUnadjustedLinearColorValue(TransformIndex / double(TransformCount - 1)).G;
			const float PointScale = CurrentPointScales && CurrentPointScales->IsValidIndex(TransformIndex) ? (*CurrentPointScales)[TransformIndex] : FallbackScale;
			Transform.SetScale3D(FVector::OneVector * CurveScale * FMath::Max(PointScale, 0.0f));
		}

		TArray<float> WorldSpaceV;
		WorldSpaceV.Reserve(TransformCount);
		float AccumulatedDistance = 0.0f;
		for (int32 PointIndex = 0; PointIndex < TransformCount; ++PointIndex)
		{
			if (PointIndex > 0 && LineVectors.IsValidIndex(PointIndex) && LineVectors.IsValidIndex(PointIndex - 1))
			{
				AccumulatedDistance += float(FVector::Dist(LineVectors[PointIndex], LineVectors[PointIndex - 1]));
			}
			WorldSpaceV.Add(AccumulatedDistance);
		}

		FGeometryScriptPrimitiveOptions PrimitiveOptions;
		if (MainVine)
		{
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSweepPolygon(
				VineMesh,
				PrimitiveOptions,
				FTransform::Identity,
				Circle,
				Transforms,
				false);
		}
		else
		{
			TArray<float> Line2DU = {0.0f, 1.0f};
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSweepPolyline(
				VineMesh,
				PrimitiveOptions,
				FTransform::Identity,
				Line2D,
				Transforms,
				Line2DU,
				WorldSpaceV,
				false);
		}
	}

	if (VineMesh->GetTriangleCount() <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineCPU] CPU output produced no valid triangles."));
		return false;
	}

	if (!MainVine)
	{
		FGeometryScriptCalculateNormalsOptions CalculateOptions;
		UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(VineMesh, CalculateOptions);
	}

	int32 PreviousMaxTriangleID = 0;
	ContainerMesh->ProcessMesh([&](const FDynamicMesh3& Mesh)
	{
		PreviousMaxTriangleID = Mesh.MaxTriangleID();
	});

	FGeometryScriptAppendMeshOptions AppendOptions;
	UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(
		ContainerMesh,
		VineMesh,
		FTransform::Identity,
		false,
		AppendOptions);

	const int32 MaterialID = MainVine ? 0 : 1;
	ContainerMesh->EditMesh([&](FDynamicMesh3& Mesh)
	{
		if (!Mesh.HasAttributes())
		{
			Mesh.EnableAttributes();
		}

		if (!Mesh.Attributes()->HasMaterialID())
		{
			Mesh.Attributes()->EnableMaterialID();
		}

		UE::Geometry::FDynamicMeshMaterialAttribute* MaterialIDs = Mesh.Attributes()->GetMaterialID();
		for (const int32 TriangleID : Mesh.TriangleIndicesItr())
		{
			if (TriangleID >= PreviousMaxTriangleID)
			{
				MaterialIDs->SetNewValue(TriangleID, MaterialID);
			}
		}
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);

	TransformDynamicMeshToLocalSpace(ContainerMesh, MeshComponent->GetComponentTransform());
	MeshComponent->NotifyMeshUpdated();
	MeshComponent->UpdateBounds();
	MeshComponent->MarkRenderTransformDirty();
	MeshComponent->MarkRenderStateDirty();

	UE_LOG(LogTemp, Log, TEXT("[VisVineCPU] Appended %s vine mesh. Lines=%d PreparedLines=%d Triangles=%d"),
		MainVine ? TEXT("tube") : TEXT("plane"),
		Lines.Num(),
		PreparedLines.Num(),
		VineMesh->GetTriangleCount());
	return true;
}

bool AVineContainer::VisVineGPUInternal(bool MainVine)
{
	const TArray<FGeometryScriptPolyPath>& Lines = MainVine ? TubeLines : PlaneLines;
	if (Lines.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] No %s lines to visualize."), MainVine ? TEXT("tube") : TEXT("plane"));
		return false;
	}

	if (VV.ResampleLength <= KINDA_SMALL_NUMBER)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] Invalid ResampleLength: %.4f"), VV.ResampleLength);
		return false;
	}

	if (VV.CurveControl == nullptr)
	{
		VV.CurveControl = NewObject<UCurveLinearColor>(this);
	}

	UDynamicMeshComponent* MeshComponent = GetDynamicMeshComponent();
	if (!MeshComponent)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] DynamicMeshComponent is null."));
		return false;
	}

	UDynamicMesh* ContainerMesh = MeshComponent->GetDynamicMesh();
	if (!ContainerMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] Container dynamic mesh is null."));
		return false;
	}

	if (CachedSurfaceVoxels.Cells.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] No cached surface voxel data for GPU voxel visualization."));
		return false;
	}

	const TArray<float>& LineSourceScales = MainVine ? TubeLineSourceScales : PlaneLineSourceScales;
	const TArray<FVector>& LineSourceLocations = MainVine ? TubeLineSourceLocations : PlaneLineSourceLocations;
	const TArray<FVineLinePointScaleData>& LinePointScales = MainVine ? TubeLinePointScales : PlaneLinePointScales;

	TArray<FGeometryScriptPolyPath> PreparedLines;
	TArray<float> PreparedLineSourceScales;
	TArray<FVineLinePointScaleData> PreparedLinePointScales;
	const double PrepareLinesStartSeconds = FPlatformTime::Seconds();
	if (!PrepareVineVisualizationLinesGPU_NoCPUProjection(
		Lines,
		VV,
		MainVine,
		LineSourceScales,
		LineSourceLocations,
		LinePointScales,
		PreparedLines,
		PreparedLineSourceScales,
		PreparedLinePointScales))
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] No valid %s lines after GPU preprocessing."), MainVine ? TEXT("tube") : TEXT("plane"));
		return false;
	}
	const double PrepareLinesMs = (FPlatformTime::Seconds() - PrepareLinesStartSeconds) * 1000.0;

	// Prepared lines have new point counts after CPU-compatible smoothing/resampling,
	// so derive GPU frames from the prepared path instead of reusing stale SC axes.
	TArray<FVineLinePointAxisData> PreparedLinePointAxes;
	BuildVineVisualizationLinePointAxes(PreparedLines, VisVineGPUAxisSmoothIterations, PreparedLinePointAxes);

	TArray<FVector4f> PathPoints;
	TArray<FVector4f> PathPointAxes;
	TArray<FIntVector4> PathPointMeta;
	TArray<float> PathPointCurveU;
	TArray<FIntVector4> SegmentMeta;
	const double BuildGPUInputStartSeconds = FPlatformTime::Seconds();
	if (!BuildVineVisualizationGPUInput(
		PreparedLines,
		VV.CurveControl,
		PreparedLineSourceScales,
		PreparedLinePointScales,
		PreparedLinePointAxes,
		PathPoints,
		PathPointAxes,
		PathPointMeta,
		PathPointCurveU,
		SegmentMeta))
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] Failed to build GPU input buffers."));
		return false;
	}
	const double BuildGPUInputMs = (FPlatformTime::Seconds() - BuildGPUInputStartSeconds) * 1000.0;

	TArray<FVector4f> OutVertices;
	TArray<FVector2f> OutUVs;
	TArray<uint32> OutIndices;
	TArray<FVector4f> SurfaceTargets;

	const double DispatchStartSeconds = FPlatformTime::Seconds();
	if (!DispatchVineVisualizationGPU_Voxel(
		PathPoints,
		PathPointAxes,
		PathPointMeta,
		PathPointCurveU,
		SegmentMeta,
		MainVine,
		VV.CircleScale,
		VV.LineScale,
		VV.VinesOffset,
		VisVineGPUPostProjectionSmoothIterations,
		CachedSurfaceVoxels,
		OutVertices,
		OutUVs,
		OutIndices,
		&SurfaceTargets))
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] Voxel GPU dispatch/readback failed."));
		return false;
	}
	const double DispatchMs = (FPlatformTime::Seconds() - DispatchStartSeconds) * 1000.0;

	LogVineGPUProjectionStats(
		MainVine ? TEXT("tube") : TEXT("plane"),
		PathPoints,
		SurfaceTargets,
		OutVertices,
		MainVine ? 3u : 2u,
		VV.VinesOffset);

	const double RecomputeUVStartSeconds = FPlatformTime::Seconds();
	RecomputeVineOutputUVsFromGeneratedLength(OutVertices, SegmentMeta, MainVine ? 3u : 2u, OutUVs);
	const double RecomputeUVMs = (FPlatformTime::Seconds() - RecomputeUVStartSeconds) * 1000.0;

	const int32 MaterialID = MainVine ? 0 : 1;
	const double BuildDynamicMeshStartSeconds = FPlatformTime::Seconds();
	UDynamicMesh* VineMesh = BuildDynamicMeshFromGPUVineOutput(this, OutVertices, OutUVs, OutIndices, MaterialID, true);
	if (!VineMesh || VineMesh->GetTriangleCount() <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] GPU output produced no valid triangles."));
		return false;
	}
	const double BuildDynamicMeshMs = (FPlatformTime::Seconds() - BuildDynamicMeshStartSeconds) * 1000.0;

	const double AppendStartSeconds = FPlatformTime::Seconds();
	int32 PreviousMaxTriangleID = 0;
	ContainerMesh->ProcessMesh([&](const FDynamicMesh3& Mesh)
	{
		PreviousMaxTriangleID = Mesh.MaxTriangleID();
	});

	FGeometryScriptAppendMeshOptions AppendOptions;
	UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(
		ContainerMesh,
		VineMesh,
		FTransform::Identity,
		false,
		AppendOptions);

	ContainerMesh->EditMesh([&](FDynamicMesh3& Mesh)
	{
		if (!Mesh.HasAttributes())
		{
			Mesh.EnableAttributes();
		}

		if (!Mesh.Attributes()->HasMaterialID())
		{
			Mesh.Attributes()->EnableMaterialID();
		}

		UE::Geometry::FDynamicMeshMaterialAttribute* MaterialIDs = Mesh.Attributes()->GetMaterialID();
		for (const int32 TriangleID : Mesh.TriangleIndicesItr())
		{
			if (TriangleID >= PreviousMaxTriangleID)
			{
				MaterialIDs->SetNewValue(TriangleID, MaterialID);
			}
		}
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);

	TransformDynamicMeshToLocalSpace(ContainerMesh, MeshComponent->GetComponentTransform());
	MeshComponent->NotifyMeshUpdated();
	MeshComponent->UpdateBounds();
	MeshComponent->MarkRenderTransformDirty();
	MeshComponent->MarkRenderStateDirty();
	const double AppendMs = (FPlatformTime::Seconds() - AppendStartSeconds) * 1000.0;

	UE_LOG(LogTemp, Display,
		TEXT("[VisVineGPUTiming] %s prepareLinesNoCPUProjection=%.3f ms buildGPUInput=%.3f ms dispatchReadback=%.3f ms recomputeUV=%.3f ms buildDynamicMesh=%.3f ms appendAndNotify=%.3f ms"),
		MainVine ? TEXT("tube") : TEXT("plane"),
		PrepareLinesMs,
		BuildGPUInputMs,
		DispatchMs,
		RecomputeUVMs,
		BuildDynamicMeshMs,
		AppendMs);

	UE_LOG(LogTemp, Log, TEXT("[VisVineGPU] Appended %s vine mesh. Lines=%d Vertices=%d Indices=%d Triangles=%d"),
		MainVine ? TEXT("tube") : TEXT("plane"),
		Lines.Num(),
		OutVertices.Num(),
		OutIndices.Num(),
		VineMesh->GetTriangleCount());
	return true;
}

inline void AVineContainer::Clean()
{
	TubeLines.Empty();
	PlaneLines.Empty();
	TubeLineSourceScales.Empty();
	PlaneLineSourceScales.Empty();
	TubeLineSourceLocations.Empty();
	PlaneLineSourceLocations.Empty();
	TubeLinePointScales.Empty();
	PlaneLinePointScales.Empty();
	TubeLinePointAxes.Empty();
	PlaneLinePointAxes.Empty();
	CachedSurfaceTriangles = FCSTriangleMeshData();
	CachedSurfaceVoxels = FCSSurfaceVoxelData();
	DynamicMeshComponent->GetDynamicMesh()->Reset();
}

void AVineContainer::ClearAttachedStaticMeshActors()
{
	TArray<AActor*> ActorsToDestroy;
	if (GeneratedStaticMeshActor)
	{
		ActorsToDestroy.Add(GeneratedStaticMeshActor);
	}

	TArray<AActor*> AttachedActors;
	GetAttachedActors(AttachedActors);
	for (AActor* AttachedActor : AttachedActors)
	{
		if (IsVineGeneratedStaticMeshActor(AttachedActor))
		{
			ActorsToDestroy.AddUnique(AttachedActor);
		}
	}

	for (AActor* ActorToDestroy : ActorsToDestroy)
	{
		if (!IsVineGeneratedStaticMeshActor(ActorToDestroy))
		{
			continue;
		}

		ActorToDestroy->Modify();
		ActorToDestroy->Destroy();
	}

	GeneratedStaticMeshActor = nullptr;
	MarkPackageDirty();
}

UDynamicMesh* AVineContainer::GenerateVines(float ExtrudeScale, bool Result, bool MultThread)
{
	GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.Total"));
	(void)ExtrudeScale;
	(void)Result;
	(void)MultThread;

	// 1. 收集 Source / Target Transforms
	TArray<FTransform> TubeSourceTransforms;
	TArray<FTransform> PlaneSourceTransforms;
	TArray<FTransform> TargetTransforms;
	GetVineInstanceTransforms(TubeVineSource, TubeSourceTransforms);
	GetVineInstanceTransforms(PlaneVineSource, PlaneSourceTransforms);
	GetVineInstanceTransforms(GrowTarget, TargetTransforms);

	const int32 TargetCount = TargetTransforms.Num();
	const int32 TubeSourceCount = TubeSourceTransforms.Num();
	const int32 PlaneSourceCount = PlaneSourceTransforms.Num();

	if (TargetCount == 0 || (TubeSourceCount == 0 && PlaneSourceCount == 0))
	{
		return nullptr;
	}

	// 2. 计算 BoundingBox 并查找场景中重叠的 Actor
	TArray<FTransform> BBoxTransforms;
	BBoxTransforms.Append(TubeSourceTransforms);
	BBoxTransforms.Append(PlaneSourceTransforms);
	BBoxTransforms.Append(TargetTransforms);

	TArray<FVector> BBoxVectors;
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.BuildBoundsInput"));
		BBoxVectors.Reserve(BBoxTransforms.Num());
		for (const FTransform& Transform : BBoxTransforms)
		{
			BBoxVectors.Add(Transform.GetLocation());
		}
	}

	FBox Bounds(BBoxVectors);
	Bounds = Bounds.ExpandBy(50);
	const FVector Center = Bounds.GetCenter();
	const FVector Extent = Bounds.GetExtent();

	SurfaceVoxelBlurIterations = FMath::Max(0, GenerateVineVoxelNormalBlurIterations);

	// 3. Prepare GPU surface voxel cache inputs.
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.EnsureTriangleCache"));
		VoxelGridSettings.VoxelSize = SC.VoxelSize;
		VoxelGridSettings.ActivationRadius = SC.VoxelSize * 8.0f;
		FCSMeshGeneratorTriangleCacheHandle TriangleCacheHandle = EnsureTriangleCacheByBox(
			TEXT("VineGenerate"),
			Center,
			Extent,
			false);
		(void)TriangleCacheHandle;
	}

	// Voxelized surface data is consumed by the GPU visualization path.
	// The cache is refreshed during GenerateVines().
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.GetBoxSceneSurfaceVoxels"));
		CachedSurfaceVoxels = GetBoxSceneSurfaceVoxelsFromGPU(SC.VoxelSize);
	}
	CachedSurfaceTriangles = FCSTriangleMeshData();
	if (!bUseGPUMode)
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.GetBoxSceneTrianglesForCPU"));
		CachedSurfaceTriangles = GetBoxSceneTrianglesFromGPUFiltered(0.0f);
	}
	// Cache generation bounds for subsequent GPU visualization.
	InstanceBound = Bounds;

	// 如果只需要输出 Debug Mesh 或不需要最终结果
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.CreateContainerMesh"));
		UDynamicMesh* ContainerMesh = NewObject<UDynamicMesh>(this);
		GetDynamicMeshComponent()->SetDynamicMesh(ContainerMesh);
	}

	// 4. 执行 SpaceColonization
	// Tube Lines
	TArray<FGeometryScriptPolyPath> GeneratedTubeLines;
	TArray<float> GeneratedTubeLineScales;
	TArray<FVector> GeneratedTubeLineSourceLocations;
	TArray<FVineLinePointScaleData> GeneratedTubeLinePointScales;
	TArray<FVineLinePointAxisData> GeneratedTubeLinePointAxes;
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.GenerateTubeLines"));
		for (int32 i = 0; i < TubeSourceCount; i++)
		{
			TArray<FTransform> SCSourceTransform;
			SCSourceTransform.Add(TubeSourceTransforms[i]);
			TArray<FSpaceColonizationLineResult> LinesFromSource = UGenerateVines::SpaceColonizationWithScales(
				SCSourceTransform, TargetTransforms,
				SC.Iteration, SC.Activetime, 3,
				SC.RandGrow, SC.Seed, SC.BackGrowRange, MultThread);
			const float SourceScale = GetVineTransformScale(TubeSourceTransforms[i]);
			for (FSpaceColonizationLineResult& LineResult : LinesFromSource)
			{
				GeneratedTubeLines.Add(LineResult.Path);
				GeneratedTubeLineScales.Add(SourceScale);
				GeneratedTubeLineSourceLocations.Add(TubeSourceTransforms[i].GetLocation());
				FVineLinePointScaleData& ScaleData = GeneratedTubeLinePointScales.AddDefaulted_GetRef();
				ScaleData.Values = MoveTemp(LineResult.PointScales);
				FVineLinePointAxisData& AxisData = GeneratedTubeLinePointAxes.AddDefaulted_GetRef();
				AxisData.Values = MoveTemp(LineResult.PointAxes);
			}
		}
	}
	TubeLines = GeneratedTubeLines;
	TubeLineSourceScales = GeneratedTubeLineScales;
	TubeLineSourceLocations = GeneratedTubeLineSourceLocations;
	TubeLinePointScales = GeneratedTubeLinePointScales;
	TubeLinePointAxes = GeneratedTubeLinePointAxes;

	// Plane Lines
	TArray<FGeometryScriptPolyPath> GeneratedPlaneLines;
	TArray<float> GeneratedPlaneLineScales;
	TArray<FVector> GeneratedPlaneLineSourceLocations;
	TArray<FVineLinePointScaleData> GeneratedPlaneLinePointScales;
	TArray<FVineLinePointAxisData> GeneratedPlaneLinePointAxes;
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.GeneratePlaneLines"));
		for (int32 i = 0; i < PlaneSourceCount; i++)
		{
			TArray<FTransform> SCSourceTransform;
			SCSourceTransform.Add(PlaneSourceTransforms[i]);
			TArray<FSpaceColonizationLineResult> LinesFromSource = UGenerateVines::SpaceColonizationWithScales(
				SCSourceTransform, TargetTransforms,
				SC.Iteration, SC.Activetime, 3,
				SC.RandGrow, SC.Seed, SC.BackGrowRange, MultThread);
			const float SourceScale = GetVineTransformScale(PlaneSourceTransforms[i]);
			for (FSpaceColonizationLineResult& LineResult : LinesFromSource)
			{
				GeneratedPlaneLines.Add(LineResult.Path);
				GeneratedPlaneLineScales.Add(SourceScale);
				GeneratedPlaneLineSourceLocations.Add(PlaneSourceTransforms[i].GetLocation());
				FVineLinePointScaleData& ScaleData = GeneratedPlaneLinePointScales.AddDefaulted_GetRef();
				ScaleData.Values = MoveTemp(LineResult.PointScales);
				FVineLinePointAxisData& AxisData = GeneratedPlaneLinePointAxes.AddDefaulted_GetRef();
				AxisData.Values = MoveTemp(LineResult.PointAxes);
			}
		}
	}
	PlaneLines = GeneratedPlaneLines;
	PlaneLineSourceScales = GeneratedPlaneLineScales;
	PlaneLineSourceLocations = GeneratedPlaneLineSourceLocations;
	PlaneLinePointScales = GeneratedPlaneLinePointScales;
	PlaneLinePointAxes = GeneratedPlaneLinePointAxes;

	if (bUseGPUMode && bDrawGPUProjectionVoxelDebugPoints && GPUProjectionVoxelDebugDuration > 0.0f)
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.DrawGPUProjectionVoxelDebugPoints"));
		DrawVineGPUProjectionVoxelDebugPoints(
			GetWorld(),
			CachedSurfaceVoxels,
			SC.VoxelSize,
			GPUProjectionVoxelDebugDuration,
			GPUProjectionVoxelDebugPointLimit,
			false);
	}

	// 5. 可视化
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.VisTubeVine"));
		VisVine(true, bUseGPUMode);   // Tube 可视化 (swept polygon)
	}
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.VisPlaneVine"));
		VisVine(false, bUseGPUMode);  // Plane 可视化 (swept polyline)
	}



	return GetDynamicMeshComponent() ? GetDynamicMeshComponent()->GetDynamicMesh() : nullptr;
}

void AVineContainer::FetchFoliage()
{
	// SetActorLocation(FVector(0.0f, 0.0f, 0.0f));
	ImportFoliageToTransformArray(TargetType);
	ImportFoliageToTransformArray(TubeType);
	ImportFoliageToTransformArray(PlaneType);
	TubeVineSource->SetHiddenInGame(false);
	RebuildDisplayInstancesFromTransformArrays();
}

void AVineContainer::RevertFoliage()
{
	DynamicMeshComponent->SetHiddenInGame(true);
	ExportTransformArrayToFoliage(TargetType);
	ExportTransformArrayToFoliage(TubeType);
	ExportTransformArrayToFoliage(PlaneType);

}

void AVineContainer::GenerateVineAction()
{
	ClearAttachedStaticMeshActors();
	ReferencePoints.Reset();

	TArray<FTransform> TargetTransforms;
	GetVineInstanceTransforms(GrowTarget, TargetTransforms);

	const int32 LastTargetIndex = TargetTransforms.Num() - 1;
	UKismetSystemLibrary::PrintString(
		this,
		FString::FromInt(LastTargetIndex),
		true,
		true,
		FLinearColor(0.0f, 0.66f, 1.0f, 1.0f),
		2.0f);

	for (const FTransform& TargetTransform : TargetTransforms)
	{
		ReferencePoints.Add(TargetTransform.GetLocation());
	}

	UKismetSystemLibrary::PrintString(
		this,
		FString::FromInt(ReferencePoints.Num()),
		true,
		true,
		FLinearColor(0.0f, 0.66f, 1.0f, 1.0f),
		2.0f);

	UDynamicMesh* GeneratedMesh = GenerateVines( 50.0f, true, false);
	if (!GeneratedMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineContainer] GenerateVineAction produced no generated mesh on %s."), *GetActorNameOrLabel());
		return;
	}

	UDynamicMeshComponent* MeshComponent = GetDynamicMeshComponent();
	if (MeshComponent)
	{
		MeshComponent->SetHiddenInGame(false);
		MeshComponent->NotifyMeshUpdated();
		MeshComponent->UpdateBounds();
		MeshComponent->MarkRenderTransformDirty();
		MeshComponent->MarkRenderStateDirty();
	}
}

int32 AVineContainer::DrawDebugCachedVineSurfaceVoxelPoints()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return 0;
	}

	if (CachedSurfaceVoxels.Cells.Num() == 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[VineVoxelDebug] No cached surface voxel data on %s. Run GenerateVines before drawing cached voxel points."),
			*GetActorNameOrLabel());
		return 0;
	}

	if (GPUProjectionVoxelDebugDuration <= 0.0f)
	{
		return 0;
	}

	return DrawVineGPUProjectionVoxelDebugPoints(
		World,
		CachedSurfaceVoxels,
		SC.VoxelSize,
		GPUProjectionVoxelDebugDuration,
		GPUProjectionVoxelDebugPointLimit,
		false);
}

int32 AVineContainer::DrawDebugCachedVineSCStagePoints(bool bDrawTube, bool bDrawPlane)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return 0;
	}

	if (!bDrawTube && !bDrawPlane)
	{
		return 0;
	}

	if (SCStageDebugPointDuration <= 0.0f)
	{
		return 0;
	}

	int32 DrawnPointCount = 0;
	if (bDrawTube)
	{
		DrawnPointCount += DrawVineSCStageDebugPoints(
			World,
			TubeLines,
			VineSCStageTubeDebugPointColor,
			VineSCStageDebugPointSize,
			SCStageDebugPointDuration,
			bVineSCStageDebugPointsPersistent,
			VineSCStageDebugPointLimit);
	}

	if (bDrawPlane)
	{
		DrawnPointCount += DrawVineSCStageDebugPoints(
			World,
			PlaneLines,
			VineSCStagePlaneDebugPointColor,
			VineSCStageDebugPointSize,
			SCStageDebugPointDuration,
			bVineSCStageDebugPointsPersistent,
			VineSCStageDebugPointLimit);
	}

	if (DrawnPointCount == 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[VineSCDebug] No cached SC-stage points on %s. Run GenerateVines before drawing cached SC-stage points."),
			*GetActorNameOrLabel());
	}
	else
	{
		UE_LOG(LogTemp, Display,
			TEXT("[VineSCDebug] Drew cached SC-stage points. TubeLines=%d PlaneLines=%d Points=%d Duration=%.3f"),
			bDrawTube ? TubeLines.Num() : 0,
			bDrawPlane ? PlaneLines.Num() : 0,
			DrawnPointCount,
			SCStageDebugPointDuration);
	}

	return DrawnPointCount;
}

int32 AVineContainer::DrawDebugVineSurfaceVoxelArrows(
	float ArrowLength,
	FLinearColor ArrowColor,
	float Duration,
	float Thickness,
	bool bPersistentLines,
	bool bDrawVoxelCenters,
	FLinearColor VoxelCenterColor,
	float VoxelCenterPointSize,
	bool bDrawWeightedTargets,
	FLinearColor WeightedTargetColor,
	float WeightedTargetPointSize,
	bool bDrawCenterToTargetLines,
	FLinearColor CenterToTargetColor,
	int32 MaxArrowsToDraw)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return 0;
	}

	TArray<FTransform> TubeSourceTransforms;
	TArray<FTransform> PlaneSourceTransforms;
	TArray<FTransform> TargetTransforms;
	GetVineInstanceTransforms(TubeVineSource, TubeSourceTransforms);
	GetVineInstanceTransforms(PlaneVineSource, PlaneSourceTransforms);
	GetVineInstanceTransforms(GrowTarget, TargetTransforms);

	const int32 TargetCount = TargetTransforms.Num();
	const int32 SourceCount = TubeSourceTransforms.Num() + PlaneSourceTransforms.Num();
	if (TargetCount == 0 || SourceCount == 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[VineVoxelDebug] Cannot build surface voxels on %s. Targets=%d Sources=%d"),
			*GetActorNameOrLabel(),
			TargetCount,
			SourceCount);
		return 0;
	}

	ReferencePoints.Reset();
	ReferencePoints.Reserve(TargetCount);
	for (const FTransform& TargetTransform : TargetTransforms)
	{
		ReferencePoints.Add(TargetTransform.GetLocation());
	}

	TArray<FTransform> BBoxTransforms;
	BBoxTransforms.Reserve(SourceCount + TargetCount);
	BBoxTransforms.Append(TubeSourceTransforms);
	BBoxTransforms.Append(PlaneSourceTransforms);
	BBoxTransforms.Append(TargetTransforms);

	TArray<FVector> BBoxVectors;
	BBoxVectors.Reserve(BBoxTransforms.Num());
	for (const FTransform& Transform : BBoxTransforms)
	{
		BBoxVectors.Add(Transform.GetLocation());
	}

	FBox Bounds(BBoxVectors);
	Bounds = Bounds.ExpandBy(50);
	if (!Bounds.IsValid)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineVoxelDebug] Invalid vine debug bounds on %s."), *GetActorNameOrLabel());
		return 0;
	}

	const float SafeVoxelSize = FMath::Max(SC.VoxelSize, 1.0e-3f);
	VoxelGridSettings.VoxelSize = SafeVoxelSize;
	VoxelGridSettings.ActivationRadius = SafeVoxelSize * 8.0f;
	EnsureTriangleCacheByBox(
		TEXT("VineDebugSurfaceVoxelArrows"),
		Bounds.GetCenter(),
		Bounds.GetExtent(),
		false);

	CachedSurfaceVoxels = GetBoxSceneSurfaceVoxelsFromGPU(SafeVoxelSize);
	const FCSSurfaceVoxelData& VoxelData = CachedSurfaceVoxels;
	const int32 AvailableCount = VoxelData.VoxelCount >= 0
		? FMath::Min(VoxelData.VoxelCount, VoxelData.Positions.Num())
		: VoxelData.Positions.Num();
	if (AvailableCount <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineVoxelDebug] No weighted surface voxel data generated on %s."), *GetActorNameOrLabel());
		return 0;
	}

	const int32 DrawLimit = MaxArrowsToDraw > 0
		? FMath::Min(MaxArrowsToDraw, AvailableCount)
		: AvailableCount;
	const float SafeArrowLength = ArrowLength > 0.0f
		? ArrowLength
		: FMath::Max(VoxelData.VoxelSize, SC.VoxelSize);
	const float SafeDuration = FMath::Max(0.0f, Duration);
	const float SafeThickness = FMath::Max(0.0f, Thickness);
	const float SafeVoxelPointSize = FMath::Max(0.0f, VoxelCenterPointSize);
	const float SafeTargetPointSize = FMath::Max(0.0f, WeightedTargetPointSize);
	const float ArrowHeadSize = FMath::Max(SafeArrowLength * 0.15f, SafeThickness * 4.0f);
	const FColor ArrowDrawColor = ArrowColor.ToFColor(true);
	const FColor VoxelCenterDrawColor = VoxelCenterColor.ToFColor(true);
	const FColor WeightedTargetDrawColor = WeightedTargetColor.ToFColor(true);
	const FColor CenterToTargetDrawColor = CenterToTargetColor.ToFColor(true);
	auto IsFiniteVector = [](const FVector& Vector)
	{
		return FMath::IsFinite(Vector.X) && FMath::IsFinite(Vector.Y) && FMath::IsFinite(Vector.Z);
	};

	int32 DrawnCount = 0;
	int32 SkippedInvalidPositionCount = 0;
	int32 SkippedInvalidTargetCount = 0;
	int32 CoincidentTargetCount = 0;
	int32 CenterToTargetArrowCount = 0;
	int32 NormalArrowCount = 0;
	int32 UpFallbackCount = 0;
	for (int32 Index = 0; Index < DrawLimit; ++Index)
	{
		FVector VoxelCenter = VoxelData.Positions[Index];
		if (!IsFiniteVector(VoxelCenter) && VoxelData.Cells.IsValidIndex(Index))
		{
			const FIntVector& Cell = VoxelData.Cells[Index];
			VoxelCenter = FVector(
				(double(Cell.X) + 0.5) * SafeVoxelSize + VoxelData.VoxelOrigin.X,
				(double(Cell.Y) + 0.5) * SafeVoxelSize + VoxelData.VoxelOrigin.Y,
				(double(Cell.Z) + 0.5) * SafeVoxelSize + VoxelData.VoxelOrigin.Z);
		}
		if (!IsFiniteVector(VoxelCenter))
		{
			++SkippedInvalidPositionCount;
			continue;
		}

		FVector WeightedTarget = VoxelCenter;
		bool bValidWeightedTarget = false;
		if (VoxelData.TargetPositions.IsValidIndex(Index) && IsFiniteVector(VoxelData.TargetPositions[Index]))
		{
			WeightedTarget = VoxelData.TargetPositions[Index];
			const double TargetDistanceSq = FVector::DistSquared(WeightedTarget, VoxelCenter);
			bValidWeightedTarget = true;
			if (TargetDistanceSq <= UE_DOUBLE_KINDA_SMALL_NUMBER)
			{
				++CoincidentTargetCount;
			}
		}

		if (!bValidWeightedTarget)
		{
			++SkippedInvalidTargetCount;
		}

		FVector ArrowDirection = VoxelData.Normals.IsValidIndex(Index) ? VoxelData.Normals[Index] : FVector::UpVector;
		if (IsFiniteVector(ArrowDirection) && ArrowDirection.Normalize())
		{
			++NormalArrowCount;
		}
		else
		{
			ArrowDirection = FVector::UpVector;
			++UpFallbackCount;
		}

		const FVector ArrowStart = bValidWeightedTarget ? WeightedTarget : VoxelCenter;
		DrawDebugDirectionalArrow(
			World,
			ArrowStart,
			ArrowStart + ArrowDirection * SafeArrowLength,
			ArrowHeadSize,
			ArrowDrawColor,
			bPersistentLines,
			SafeDuration,
			0,
			SafeThickness);

		if (bDrawVoxelCenters && SafeVoxelPointSize > 0.0f)
		{
			DrawDebugPoint(
				World,
				VoxelCenter,
				SafeVoxelPointSize,
				VoxelCenterDrawColor,
				bPersistentLines,
				SafeDuration,
				0);
		}

		if (bValidWeightedTarget && bDrawWeightedTargets && SafeTargetPointSize > 0.0f)
		{
			DrawDebugPoint(
				World,
				WeightedTarget,
				SafeTargetPointSize,
				WeightedTargetDrawColor,
				bPersistentLines,
				SafeDuration,
				0);
		}

		if (bValidWeightedTarget && bDrawCenterToTargetLines)
		{
			const FVector CenterToTarget = WeightedTarget - VoxelCenter;
			const double CenterToTargetLengthSq = CenterToTarget.SizeSquared();
			if (CenterToTargetLengthSq > UE_DOUBLE_KINDA_SMALL_NUMBER)
			{
				DrawDebugDirectionalArrow(
					World,
					VoxelCenter,
					WeightedTarget,
					ArrowHeadSize,
					CenterToTargetDrawColor,
					bPersistentLines,
					SafeDuration,
					0,
					SafeThickness);
				++CenterToTargetArrowCount;
			}
			else
			{
				DrawDebugLine(
					World,
					VoxelCenter,
					WeightedTarget,
					CenterToTargetDrawColor,
					bPersistentLines,
					SafeDuration,
					0,
					SafeThickness);
			}
		}

		++DrawnCount;
	}

	UE_LOG(LogTemp, Log,
		TEXT("[VineVoxelDebug] Drawn weighted surface voxel arrows on %s. Drawn=%d Available=%d Positions=%d Normals=%d Targets=%d Cells=%d InvalidPositions=%d InvalidTargets=%d CoincidentTargets=%d CenterToTargetArrows=%d NormalArrows=%d UpFallbacks=%d VoxelSize=%.3f"),
		*GetActorNameOrLabel(),
		DrawnCount,
		AvailableCount,
		VoxelData.Positions.Num(),
		VoxelData.Normals.Num(),
		VoxelData.TargetPositions.Num(),
		VoxelData.Cells.Num(),
		SkippedInvalidPositionCount,
		SkippedInvalidTargetCount,
		CoincidentTargetCount,
		CenterToTargetArrowCount,
		NormalArrowCount,
		UpFallbackCount,
		VoxelData.VoxelSize);
	return DrawnCount;
}

void AVineContainer::SaveStaticmesh()
{
	UDynamicMeshComponent* MeshComponent = GetDynamicMeshComponent();
	UDynamicMesh* TargetMesh = MeshComponent ? MeshComponent->GetDynamicMesh() : nullptr;
	if (!TargetMesh || TargetMesh->GetTriangleCount() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineContainer] SaveStaticmesh skipped: no generated DynamicMesh on %s."), *GetActorNameOrLabel());
		return;
	}

	if (GeneratorTimeCode == -1)
	{
		const FDateTime Now = FDateTime::Now();
		GeneratorTimeCode =
			int64(Now.GetYear() % 100) * 100000000LL +
			int64(Now.GetMonth()) * 1000000LL +
			int64(Now.GetDay()) * 10000LL +
			int64(Now.GetHour()) * 100LL +
			int64(Now.GetMinute());
	}

	ULevel* ActorLevel = GetLevel();
	UPackage* LevelPackage = ActorLevel ? ActorLevel->GetOutermost() : nullptr;
	if (!LevelPackage)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineContainer] SaveStaticmesh failed: %s has no level package."), *GetActorNameOrLabel());
		return;
	}

	const FString LevelFolderPath = FPackageName::GetLongPackagePath(LevelPackage->GetName());
	if (LevelFolderPath.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineContainer] SaveStaticmesh failed: empty level folder path for %s."), *GetActorNameOrLabel());
		return;
	}

	const FString AssetFolderPath = UPackageTools::SanitizePackageName(LevelFolderPath / TEXT("AutoResult"));

	const FString ActorName = ObjectTools::SanitizeObjectName(GetActorNameOrLabel());
	FString AssetName = ObjectTools::SanitizeObjectName(FString::Printf(TEXT("%s_%s"), *ActorName, *LexToString(GeneratorTimeCode)));
	if (!AssetName.StartsWith(TEXT("SM_")))
	{
		AssetName = FString(TEXT("SM_")) + AssetName;
	}
	const FString AssetPathAndName = UPackageTools::SanitizePackageName(AssetFolderPath / AssetName);

	UStaticMesh* NewStaticMesh = UGeometryGeneral::SaveDynamicMeshToStaticMesh(
		TargetMesh,
		AssetPathAndName,
		MeshComponent,
		true,
		false,
		true);
	if (!NewStaticMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineContainer] SaveStaticmesh failed: could not create %s."), *AssetPathAndName);
		return;
	}

	ClearAttachedStaticMeshActors();

	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineContainer] Saved StaticMesh but could not spawn actor: invalid world on %s."), *GetActorNameOrLabel());
		return;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = this;
	SpawnParams.OverrideLevel = GetLevel();
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
#if WITH_EDITOR
	SpawnParams.InitialActorLabel = AssetName;
#endif

	AStaticMeshActor* SpawnedStaticMeshActor = World->SpawnActor<AStaticMeshActor>(
		AStaticMeshActor::StaticClass(),
		MeshComponent->GetComponentTransform(),
		SpawnParams);
	if (!SpawnedStaticMeshActor)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineContainer] Saved StaticMesh but could not spawn actor for %s."), *AssetPathAndName);
		return;
	}

	SpawnedStaticMeshActor->Modify();
	SpawnedStaticMeshActor->Tags.AddUnique(VineGeneratedStaticMeshActorTag);
	if (UStaticMeshComponent* StaticMeshComponent = SpawnedStaticMeshActor->GetStaticMeshComponent())
	{
		StaticMeshComponent->SetMobility(EComponentMobility::Movable);
		StaticMeshComponent->SetStaticMesh(NewStaticMesh);
		StaticMeshComponent->UpdateBounds();
		StaticMeshComponent->MarkRenderStateDirty();
	}
	SpawnedStaticMeshActor->AttachToActor(this, FAttachmentTransformRules::KeepWorldTransform);
	SpawnedStaticMeshActor->SetActorLabel(AssetName);
	SpawnedStaticMeshActor->MarkPackageDirty();
	GeneratedStaticMeshActor = SpawnedStaticMeshActor;
	MarkPackageDirty();
	DynamicMeshComponent->SetHiddenInGame(true);
	UE_LOG(LogTemp, Log, TEXT("[VineContainer] Created unsaved StaticMesh asset: %s"), *AssetPathAndName);
}
