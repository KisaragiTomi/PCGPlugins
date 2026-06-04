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
#include "GeometryScript/MeshBasicEditFunctions.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "GeometryGeneral.h"
#include "DynamicMesh/MeshTransforms.h"
#include "ComputeShaderGenerateHepler.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "ShaderParameterStruct.h"
#include "Misc/PackageName.h"
#include "Spatial/MeshAABBTree3.h"

#define GV_ACTOR_ENABLE_PERF_LOGS 0
#if GV_ACTOR_ENABLE_PERF_LOGS
#define GV_ACTOR_TIME_SCOPE(Label) PCG_DEBUG_TIME_SCOPE_WITH_PREFIX(TEXT("[GenerateVinesTiming]"), Label)
#else
#define GV_ACTOR_TIME_SCOPE(Label)
#endif

class FVineVisualizationMeshCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVineVisualizationMeshCS);
	SHADER_USE_PARAMETER_STRUCT(FVineVisualizationMeshCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PathPoints)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, PathPointMeta)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, PathPointCurveU)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, MeshTriangleVertices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, SegmentMeta)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, GridCellOffsets)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, GridTriangleIndices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_OutVertices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float2>, RW_OutUVs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_OutIndices)
		SHADER_PARAMETER(uint32, PathPointCount)
		SHADER_PARAMETER(uint32, SegmentCount)
		SHADER_PARAMETER(uint32, MeshTriangleCount)
		SHADER_PARAMETER(uint32, OutputVertexCount)
		SHADER_PARAMETER(uint32, OutputIndexCount)
		SHADER_PARAMETER(uint32, ProfileCount)
		SHADER_PARAMETER(uint32, bTube)
		SHADER_PARAMETER(float, CircleScale)
		SHADER_PARAMETER(float, LineScale)
		SHADER_PARAMETER(FVector3f, GridOrigin)
		SHADER_PARAMETER(float, GridCellSize)
		SHADER_PARAMETER(FIntVector, GridDimensions)
		SHADER_PARAMETER(uint32, GridTotalCells)
		SHADER_PARAMETER(uint32, bUseGrid)
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

IMPLEMENT_GLOBAL_SHADER(FVineVisualizationMeshCS, "/Plugin/PCGPlugins/Shaders/Private/VineVisualization.usf", "BuildVineVisualizationMeshCS", SF_Compute);

// Uniform grid acceleration structure for GPU nearest-triangle queries
struct FVineVisualizationGrid
{
	TArray<uint32> CellOffsets;   // [GridTotalCells + 1] prefix-sum offsets into TriangleIndices
	TArray<uint32> TriangleIndices; // packed triangle indices per cell
	FVector3f Origin;
	float CellSize;
	FIntVector Dimensions;
	uint32 TotalCells;
};

static FVineVisualizationGrid BuildUniformGridForTriangles(
	const TArray<FVector4f>& MeshTriangleVertices,
	float TargetCellSize)
{
	FVineVisualizationGrid Grid;
	const int32 TriangleCount = MeshTriangleVertices.Num() / 3;
	if (TriangleCount == 0 || TargetCellSize <= 0.0f)
	{
		Grid.CellSize = 1.0f;
		Grid.Origin = FVector3f::ZeroVector;
		Grid.Dimensions = FIntVector(1, 1, 1);
		Grid.TotalCells = 1;
		Grid.CellOffsets.Init(0, 2);
		return Grid;
	}

	// Compute AABB of all triangle vertices
	FVector3f BoundsMin(FLT_MAX, FLT_MAX, FLT_MAX);
	FVector3f BoundsMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
	for (const FVector4f& V : MeshTriangleVertices)
	{
		BoundsMin.X = FMath::Min(BoundsMin.X, V.X);
		BoundsMin.Y = FMath::Min(BoundsMin.Y, V.Y);
		BoundsMin.Z = FMath::Min(BoundsMin.Z, V.Z);
		BoundsMax.X = FMath::Max(BoundsMax.X, V.X);
		BoundsMax.Y = FMath::Max(BoundsMax.Y, V.Y);
		BoundsMax.Z = FMath::Max(BoundsMax.Z, V.Z);
	}

	// Expand bounds slightly to avoid edge cases
	const FVector3f Padding(TargetCellSize * 0.5f);
	BoundsMin -= Padding;
	BoundsMax += Padding;

	const FVector3f Extent = BoundsMax - BoundsMin;
	Grid.CellSize = FMath::Max(TargetCellSize, 1.0f);
	Grid.Origin = BoundsMin;
	Grid.Dimensions = FIntVector(
		FMath::Max(1, FMath::CeilToInt(Extent.X / Grid.CellSize)),
		FMath::Max(1, FMath::CeilToInt(Extent.Y / Grid.CellSize)),
		FMath::Max(1, FMath::CeilToInt(Extent.Z / Grid.CellSize)));

	// Clamp grid dimensions to avoid excessive memory
	constexpr int32 MaxDim = 128;
	Grid.Dimensions.X = FMath::Min(Grid.Dimensions.X, MaxDim);
	Grid.Dimensions.Y = FMath::Min(Grid.Dimensions.Y, MaxDim);
	Grid.Dimensions.Z = FMath::Min(Grid.Dimensions.Z, MaxDim);
	Grid.TotalCells = uint32(Grid.Dimensions.X) * uint32(Grid.Dimensions.Y) * uint32(Grid.Dimensions.Z);

	// Recalculate cell size based on clamped dimensions
	Grid.CellSize = FMath::Max(
		FMath::Max(Extent.X / float(Grid.Dimensions.X), Extent.Y / float(Grid.Dimensions.Y)),
		Extent.Z / float(Grid.Dimensions.Z));
	Grid.CellSize = FMath::Max(Grid.CellSize, 1.0f);

	// Count triangles per cell (a triangle can span multiple cells)
	TArray<uint32> CellCounts;
	CellCounts.SetNumZeroed(Grid.TotalCells);

	auto GetCellIndex = [&Grid](int32 X, int32 Y, int32 Z) -> int32
	{
		return X + Y * Grid.Dimensions.X + Z * Grid.Dimensions.X * Grid.Dimensions.Y;
	};

	auto GetCellCoord = [&Grid](float Pos, int32 Axis) -> int32
	{
		const float Local = Pos - (&Grid.Origin.X)[Axis];
		return FMath::Clamp(FMath::FloorToInt(Local / Grid.CellSize), 0, (&Grid.Dimensions.X)[Axis] - 1);
	};

	// First pass: count how many cells each triangle touches
	TArray<TArray<int32>> TriangleCellLists;
	TriangleCellLists.SetNum(TriangleCount);

	for (int32 TriIndex = 0; TriIndex < TriangleCount; ++TriIndex)
	{
		const int32 Base = TriIndex * 3;
		const FVector3f& A = reinterpret_cast<const FVector3f&>(MeshTriangleVertices[Base + 0]);
		const FVector3f& B = reinterpret_cast<const FVector3f&>(MeshTriangleVertices[Base + 1]);
		const FVector3f& C = reinterpret_cast<const FVector3f&>(MeshTriangleVertices[Base + 2]);

		// Triangle AABB
		const int32 MinX = GetCellCoord(FMath::Min3(A.X, B.X, C.X), 0);
		const int32 MinY = GetCellCoord(FMath::Min3(A.Y, B.Y, C.Y), 1);
		const int32 MinZ = GetCellCoord(FMath::Min3(A.Z, B.Z, C.Z), 2);
		const int32 MaxX = GetCellCoord(FMath::Max3(A.X, B.X, C.X), 0);
		const int32 MaxY = GetCellCoord(FMath::Max3(A.Y, B.Y, C.Y), 1);
		const int32 MaxZ = GetCellCoord(FMath::Max3(A.Z, B.Z, C.Z), 2);

		for (int32 Z = MinZ; Z <= MaxZ; ++Z)
		{
			for (int32 Y = MinY; Y <= MaxY; ++Y)
			{
				for (int32 X = MinX; X <= MaxX; ++X)
				{
					const int32 CellIdx = GetCellIndex(X, Y, Z);
					++CellCounts[CellIdx];
					TriangleCellLists[TriIndex].Add(CellIdx);
				}
			}
		}
	}

	// Build prefix-sum offsets
	Grid.CellOffsets.SetNum(Grid.TotalCells + 1);
	Grid.CellOffsets[0] = 0;
	for (uint32 CellIdx = 0; CellIdx < Grid.TotalCells; ++CellIdx)
	{
		Grid.CellOffsets[CellIdx + 1] = Grid.CellOffsets[CellIdx] + CellCounts[CellIdx];
	}

	const uint32 TotalEntries = Grid.CellOffsets[Grid.TotalCells];
	Grid.TriangleIndices.SetNumZeroed(FMath::Max(TotalEntries, 1u));

	// Fill triangle indices using write cursors
	TArray<uint32> WriteCursors;
	WriteCursors.SetNum(Grid.TotalCells);
	FMemory::Memcpy(WriteCursors.GetData(), Grid.CellOffsets.GetData(), Grid.TotalCells * sizeof(uint32));

	for (int32 TriIndex = 0; TriIndex < TriangleCount; ++TriIndex)
	{
		for (const int32 CellIdx : TriangleCellLists[TriIndex])
		{
			Grid.TriangleIndices[WriteCursors[CellIdx]++] = uint32(TriIndex);
		}
	}

	return Grid;
}

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

struct FSpaceColonizationLineDebugStats
{
	int32 LineCount = 0;
	int32 PointCount = 0;
	double TotalLength = 0.0;
};

static double GetPolyPathLength(const FGeometryScriptPolyPath& Line)
{
	const TArray<FVector>& Points = *Line.Path;
	double Length = 0.0;
	for (int32 Index = 1; Index < Points.Num(); ++Index)
	{
		Length += FVector::Dist(Points[Index - 1], Points[Index]);
	}
	return Length;
}

static FSpaceColonizationLineDebugStats GetSpaceColonizationLineStats(const TArray<FGeometryScriptPolyPath>& Lines)
{
	FSpaceColonizationLineDebugStats Stats;
	Stats.LineCount = Lines.Num();
	for (const FGeometryScriptPolyPath& Line : Lines)
	{
		const TArray<FVector>& Points = *Line.Path;
		Stats.PointCount += Points.Num();
		Stats.TotalLength += GetPolyPathLength(Line);
	}
	return Stats;
}

static void LogSpaceColonizationLineComparison(
	const TCHAR* Label,
	const TArray<FGeometryScriptPolyPath>& CpuLines,
	const TArray<FGeometryScriptPolyPath>& CsLines)
{
	const FSpaceColonizationLineDebugStats CpuStats = GetSpaceColonizationLineStats(CpuLines);
	const FSpaceColonizationLineDebugStats CsStats = GetSpaceColonizationLineStats(CsLines);

	const int32 MatchedLineCount = FMath::Min(CpuLines.Num(), CsLines.Num());
	int32 MatchedPointCount = 0;
	int32 TotalPointCountDeltaAbs = 0;
	double SumPointDistance = 0.0;
	double MaxPointDistance = 0.0;
	double SumLineLengthDeltaAbs = 0.0;
	double MaxLineLengthDeltaAbs = 0.0;

	for (int32 LineIndex = 0; LineIndex < MatchedLineCount; ++LineIndex)
	{
		const TArray<FVector>& CpuPoints = *CpuLines[LineIndex].Path;
		const TArray<FVector>& CsPoints = *CsLines[LineIndex].Path;
		const int32 MatchedPointsInLine = FMath::Min(CpuPoints.Num(), CsPoints.Num());
		MatchedPointCount += MatchedPointsInLine;
		TotalPointCountDeltaAbs += FMath::Abs(CpuPoints.Num() - CsPoints.Num());

		for (int32 PointIndex = 0; PointIndex < MatchedPointsInLine; ++PointIndex)
		{
			const double PointDistance = FVector::Dist(CpuPoints[PointIndex], CsPoints[PointIndex]);
			SumPointDistance += PointDistance;
			MaxPointDistance = FMath::Max(MaxPointDistance, PointDistance);
		}

		const double LineLengthDeltaAbs = FMath::Abs(GetPolyPathLength(CpuLines[LineIndex]) - GetPolyPathLength(CsLines[LineIndex]));
		SumLineLengthDeltaAbs += LineLengthDeltaAbs;
		MaxLineLengthDeltaAbs = FMath::Max(MaxLineLengthDeltaAbs, LineLengthDeltaAbs);
	}

	const double AveragePointDistance = MatchedPointCount > 0 ? SumPointDistance / double(MatchedPointCount) : 0.0;
	const double AverageLineLengthDeltaAbs = MatchedLineCount > 0 ? SumLineLengthDeltaAbs / double(MatchedLineCount) : 0.0;

	UE_LOG(LogTemp, Warning,
		TEXT("[SpaceColonizationCompare][%s] CPU Lines=%d Points=%d Length=%.3f | CS Lines=%d Points=%d Length=%.3f | Delta Lines=%d Points=%d Length=%.3f"),
		Label,
		CpuStats.LineCount,
		CpuStats.PointCount,
		CpuStats.TotalLength,
		CsStats.LineCount,
		CsStats.PointCount,
		CsStats.TotalLength,
		CsStats.LineCount - CpuStats.LineCount,
		CsStats.PointCount - CpuStats.PointCount,
		CsStats.TotalLength - CpuStats.TotalLength);

	UE_LOG(LogTemp, Warning,
		TEXT("[SpaceColonizationCompare][%s] MatchedLines=%d MatchedPoints=%d PointDeltaAvg=%.3f PointDeltaMax=%.3f LineLengthDeltaAvgAbs=%.3f LineLengthDeltaMaxAbs=%.3f PointCountDeltaAbs=%d"),
		Label,
		MatchedLineCount,
		MatchedPointCount,
		AveragePointDistance,
		MaxPointDistance,
		AverageLineLengthDeltaAbs,
		MaxLineLengthDeltaAbs,
		TotalPointCountDeltaAbs);
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

static bool TryFindNearestPointOnVinePrefixMesh(
	UDynamicMesh* PrefixMesh,
	const FGeometryScriptDynamicMeshBVH& BVH,
	const FVector& QueryPosition,
	const FGeometryScriptSpatialQueryOptions& Options,
	FGeometryScriptTrianglePoint& OutNearestPoint,
	const TCHAR* Context);

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

static bool PrepareVineVisualizationLinesCPU(
	const TArray<FGeometryScriptPolyPath>& Lines,
	const FVineVisualization& VV,
	bool bMainVine,
	const TArray<float>& InLineSourceScales,
	const TArray<FVector>& InLineSourceLocations,
	const TArray<FVineLinePointScaleData>& InLinePointScales,
	UDynamicMesh* PrefixMesh,
	const FGeometryScriptDynamicMeshBVH& BVH,
	TArray<FGeometryScriptPolyPath>& OutLines,
	TArray<float>& OutLineSourceScales,
	TArray<FVineLinePointScaleData>& OutLinePointScales)
{
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
	FGeometryScriptSpatialQueryOptions Options;
	for (int32 LineIdx = 0; LineIdx < WorkingLines.Num(); ++LineIdx)
	{
		FGeometryScriptPolyPath Line = ClonePolyPath(WorkingLines[LineIdx]);
		if (!Line.Path.IsValid())
		{
			continue;
		}

		TArray<float> CurrentPointScales = WorkingLinePointScales[LineIdx].Values;
		const float FallbackScale = WorkingLineSourceScales.IsValidIndex(LineIdx) ? WorkingLineSourceScales[LineIdx] : 1.0f;
		const float ArcLength = UE::Geometry::CurveUtil::ArcLength<float, FVector>(*Line.Path, false);
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
				FGeometryScriptTrianglePoint NearestPoint;
				if (TryFindNearestPointOnVinePrefixMesh(PrefixMesh, BVH, VertexLocation, Options, NearestPoint, TEXT("VisVine.PrepareNoiseProject")))
				{
					VertexLocation = NearestPoint.Position;
				}

				UNoise::CurlNoise(VertexLocation, VertexLocation, FVector::ZeroVector, VV.CurlNoiseScale / 10.0f, VV.CurlNoiseFre);
				const FVector NoisePos = (FVector)(VV.PerlinNoiseFre / 100.0f * VertexLocation);
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

	if (NoiseLines.Num() == 0 || SampleRangePointsSum.Num() == 0)
	{
		return false;
	}

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

				const FVector NoisePos = (FVector)(VV.PerlinNoiseFre / 100.0f * VertexLocation);
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
				FGeometryScriptTrianglePoint NearestPoint;
				if (TryFindNearestPointOnVinePrefixMesh(PrefixMesh, BVH, VertexLocation, Options, NearestPoint, TEXT("VisVine.PrepareMergeProject")))
				{
					VertexLocation = NearestPoint.Position;
				}
			}
		}

		const int32 VertexCount = Line.Path->Num();
		for (int32 PointIndex = 0; PointIndex < VertexCount; ++PointIndex)
		{
			const FVector VertexLocation = (*Line.Path)[PointIndex];
			FGeometryScriptTrianglePoint NearestPoint;
			if (!TryFindNearestPointOnVinePrefixMesh(PrefixMesh, BVH, VertexLocation, Options, NearestPoint, TEXT("VisVine.PrepareOffsetNormal")))
			{
				continue;
			}

			FVector Normal = FVector::UpVector;
			PrefixMesh->ProcessMesh([&](const FDynamicMesh3& EditMesh)
			{
				Normal = EditMesh.GetTriNormal(NearestPoint.TriangleID);
			});

			FVector& VertexLocationFix = (*Line.Path)[PointIndex];
			VertexLocationFix += Normal * VV.VinesOffset;
			VertexLocationFix.Z += GetVineVisualizationTinyZJitter(VertexLocation, PointIndex);
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

	return OutLines.Num() > 0;
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
	TArray<FVector4f>& OutPathPoints,
	TArray<FIntVector4>& OutPathPointMeta,
	TArray<float>& OutPathPointCurveU,
	TArray<FIntVector4>& OutSegmentMeta)
{
	OutPathPoints.Reset();
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
			OutPathPoints.Add(FVector4f(float(Point.X), float(Point.Y), float(Point.Z), Scale));
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

static bool ExtractDynamicMeshTrianglesForGPU(UDynamicMesh* Mesh, TArray<FVector4f>& OutTriangleVertices)
{
	OutTriangleVertices.Reset();
	if (!Mesh)
	{
		return false;
	}

	Mesh->ProcessMesh([&](const FDynamicMesh3& EditMesh)
	{
		OutTriangleVertices.Reserve(EditMesh.TriangleCount() * 3);
		for (const int32 TriangleID : EditMesh.TriangleIndicesItr())
		{
			const UE::Geometry::FIndex3i Tri = EditMesh.GetTriangle(TriangleID);
			const int32 CornerOrder[3] = {0, 2, 1};
			for (int32 CornerIndex = 0; CornerIndex < 3; ++CornerIndex)
			{
				const FVector3d Position = EditMesh.GetVertex(Tri[CornerOrder[CornerIndex]]);
				OutTriangleVertices.Add(FVector4f(float(Position.X), float(Position.Y), float(Position.Z), 1.0f));
			}
		}
	});

	return OutTriangleVertices.Num() >= 3;
}

static bool RebuildVineBVHForPrefixMesh(TObjectPtr<UDynamicMesh>& PrefixMesh, FGeometryScriptDynamicMeshBVH& BVH, const TCHAR* Context)
{
	if (!PrefixMesh || PrefixMesh->GetTriangleCount() <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[%s] PrefixMesh is empty, skip vine visualization."), Context);
		return false;
	}

	FGeometryScriptDynamicMeshBVH RebuiltBVH;
	PrefixMesh = UGeometryScriptLibrary_MeshSpatial::BuildBVHForMesh(PrefixMesh.Get(), RebuiltBVH, nullptr);
	BVH = RebuiltBVH;

	if (!BVH.Spatial.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[%s] Failed to rebuild BVH for PrefixMesh."), Context);
		return false;
	}

	return true;
}

static bool TryFindNearestPointOnVinePrefixMesh(
	UDynamicMesh* PrefixMesh,
	const FGeometryScriptDynamicMeshBVH& BVH,
	const FVector& QueryPosition,
	const FGeometryScriptSpatialQueryOptions& Options,
	FGeometryScriptTrianglePoint& OutNearestPoint,
	const TCHAR* Context)
{
	if (!PrefixMesh || PrefixMesh->GetTriangleCount() <= 0 || !BVH.Spatial.IsValid())
	{
		static int32 InvalidLogCount = 0;
		if (InvalidLogCount < 8)
		{
			UE_LOG(LogTemp, Warning, TEXT("[%s] Skip nearest query. PrefixMesh=%s Triangles=%d BVHValid=%d"),
				Context,
				PrefixMesh ? TEXT("Valid") : TEXT("Null"),
				PrefixMesh ? PrefixMesh->GetTriangleCount() : 0,
				BVH.Spatial.IsValid() ? 1 : 0);
			++InvalidLogCount;
		}
		return false;
	}

	EGeometryScriptSearchOutcomePins Outcome;
	UGeometryScriptLibrary_MeshSpatial::FindNearestPointOnMesh(
		PrefixMesh, BVH, QueryPosition, Options, OutNearestPoint, Outcome, nullptr);

	if (Outcome != EGeometryScriptSearchOutcomePins::Found || OutNearestPoint.TriangleID < 0)
	{
		static int32 FailLogCount = 0;
		if (FailLogCount < 8)
		{
			UE_LOG(LogTemp, Warning, TEXT("[%s] FindNearestPointOnMesh failed. Outcome=%d TriangleID=%d Query=(%.2f, %.2f, %.2f)"),
				Context,
				int32(Outcome),
				OutNearestPoint.TriangleID,
				QueryPosition.X,
				QueryPosition.Y,
				QueryPosition.Z);
			++FailLogCount;
		}
		return false;
	}

	return true;
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

static bool DispatchVineVisualizationGPU(
	const TArray<FVector4f>& PathPoints,
	const TArray<FIntVector4>& PathPointMeta,
	const TArray<float>& PathPointCurveU,
	const TArray<FVector4f>& MeshTriangleVertices,
	const TArray<FIntVector4>& SegmentMeta,
	bool bTube,
	float CircleScale,
	float LineScale,
	TArray<FVector4f>& OutVertices,
	TArray<FVector2f>& OutUVs,
	TArray<uint32>& OutIndices)
{
	OutVertices.Reset();
	OutUVs.Reset();
	OutIndices.Reset();

	const uint32 PathPointCount = uint32(PathPoints.Num());
	const uint32 SegmentCount = uint32(SegmentMeta.Num());
	const uint32 MeshTriangleCount = uint32(MeshTriangleVertices.Num() / 3);
	const uint32 ProfileCount = bTube ? 3u : 2u;
	const uint32 OutputVertexCount = PathPointCount * ProfileCount;
	const uint32 OutputIndexCount = bTube ? SegmentCount * ProfileCount * 6u : SegmentCount * 6u;
	if (PathPointCount == 0 || uint32(PathPointCurveU.Num()) != PathPointCount || SegmentCount == 0 || MeshTriangleCount == 0 || OutputVertexCount == 0 || OutputIndexCount == 0)
	{
		return false;
	}

	const uint64 VertexReadbackBytes64 = uint64(OutputVertexCount) * sizeof(FVector4f);
	const uint64 UVReadbackBytes64 = uint64(OutputVertexCount) * sizeof(FVector2f);
	const uint64 IndexReadbackBytes64 = uint64(OutputIndexCount) * sizeof(uint32);
	if (VertexReadbackBytes64 > MAX_uint32 || UVReadbackBytes64 > MAX_uint32 || IndexReadbackBytes64 > MAX_uint32)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] Output is too large for readback. Vertices=%u Indices=%u"), OutputVertexCount, OutputIndexCount);
		return false;
	}

	const uint32 VertexReadbackBytes = uint32(VertexReadbackBytes64);
	const uint32 UVReadbackBytes = uint32(UVReadbackBytes64);
	const uint32 IndexReadbackBytes = uint32(IndexReadbackBytes64);
	FRHIGPUBufferReadback* VertexReadback = new FRHIGPUBufferReadback(TEXT("VineVisualization_VertexReadback"));
	FRHIGPUBufferReadback* UVReadback = new FRHIGPUBufferReadback(TEXT("VineVisualization_UVReadback"));
	FRHIGPUBufferReadback* IndexReadback = new FRHIGPUBufferReadback(TEXT("VineVisualization_IndexReadback"));
	bool bRenderWorkQueued = false;

	// Build uniform grid acceleration structure on CPU
	const float GridCellSize = FMath::Max(200.0f, float(MeshTriangleCount) * 0.01f);
	FVineVisualizationGrid Grid = BuildUniformGridForTriangles(MeshTriangleVertices, GridCellSize);
	const bool bUseGrid = Grid.TotalCells > 1 && Grid.TriangleIndices.Num() > 0;

	ENQUEUE_RENDER_COMMAND(VineVisualizationGPU)(
		[PathPoints, PathPointMeta, PathPointCurveU, MeshTriangleVertices, SegmentMeta, Grid, bUseGrid,
		 VertexReadback, UVReadback, IndexReadback, VertexReadbackBytes, UVReadbackBytes, IndexReadbackBytes,
		 PathPointCount, SegmentCount, MeshTriangleCount, ProfileCount, OutputVertexCount, OutputIndexCount,
		 bTube, CircleScale, LineScale, &bRenderWorkQueued](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			const CSHepler::FRDGStructuredBufferRefs PathPointBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, PathPoints, TEXT("VineVisualization.PathPoints"));
			const CSHepler::FRDGStructuredBufferRefs PathPointMetaBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, PathPointMeta, TEXT("VineVisualization.PathPointMeta"));
			const CSHepler::FRDGStructuredBufferRefs PathPointCurveUBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, PathPointCurveU, TEXT("VineVisualization.PathPointCurveU"));
			const CSHepler::FRDGStructuredBufferRefs MeshTriangleBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, MeshTriangleVertices, TEXT("VineVisualization.MeshTriangleVertices"));
			const CSHepler::FRDGStructuredBufferRefs SegmentMetaBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, SegmentMeta, TEXT("VineVisualization.SegmentMeta"));
			const CSHepler::FRDGStructuredBufferRefs OutVertexBuffer = CSHepler::CreateStructuredBuffer<FVector4f>(GraphBuilder, OutputVertexCount, TEXT("VineVisualization.OutVertices"), true, true);
			const CSHepler::FRDGStructuredBufferRefs OutUVBuffer = CSHepler::CreateStructuredBuffer<FVector2f>(GraphBuilder, OutputVertexCount, TEXT("VineVisualization.OutUVs"), true, true);
			const CSHepler::FRDGStructuredBufferRefs OutIndexBuffer = CSHepler::CreateStructuredBuffer<uint32>(GraphBuilder, OutputIndexCount, TEXT("VineVisualization.OutIndices"), true, true);

			// Upload grid buffers
			const CSHepler::FRDGStructuredBufferRefs GridCellOffsetsBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, Grid.CellOffsets, TEXT("VineVisualization.GridCellOffsets"));
			const CSHepler::FRDGStructuredBufferRefs GridTriangleIndicesBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, Grid.TriangleIndices, TEXT("VineVisualization.GridTriangleIndices"));

			TShaderMapRef<FVineVisualizationMeshCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FVineVisualizationMeshCS::FParameters* Parameters = GraphBuilder.AllocParameters<FVineVisualizationMeshCS::FParameters>();
			Parameters->PathPoints = PathPointBuffer.SRV;
			Parameters->PathPointMeta = PathPointMetaBuffer.SRV;
			Parameters->PathPointCurveU = PathPointCurveUBuffer.SRV;
			Parameters->MeshTriangleVertices = MeshTriangleBuffer.SRV;
			Parameters->SegmentMeta = SegmentMetaBuffer.SRV;
			Parameters->GridCellOffsets = GridCellOffsetsBuffer.SRV;
			Parameters->GridTriangleIndices = GridTriangleIndicesBuffer.SRV;
			Parameters->RW_OutVertices = OutVertexBuffer.UAV;
			Parameters->RW_OutUVs = OutUVBuffer.UAV;
			Parameters->RW_OutIndices = OutIndexBuffer.UAV;
			Parameters->PathPointCount = PathPointCount;
			Parameters->SegmentCount = SegmentCount;
			Parameters->MeshTriangleCount = MeshTriangleCount;
			Parameters->OutputVertexCount = OutputVertexCount;
			Parameters->OutputIndexCount = OutputIndexCount;
			Parameters->ProfileCount = ProfileCount;
			Parameters->bTube = bTube ? 1u : 0u;
			Parameters->CircleScale = CircleScale;
			Parameters->LineScale = LineScale;
			Parameters->GridOrigin = Grid.Origin;
			Parameters->GridCellSize = Grid.CellSize;
			Parameters->GridDimensions = Grid.Dimensions;
			Parameters->GridTotalCells = Grid.TotalCells;
			Parameters->bUseGrid = bUseGrid ? 1u : 0u;

			const uint32 DispatchCount = FMath::Max(OutputVertexCount, SegmentCount);
			GraphBuilder.AddPass(
				RDG_EVENT_NAME("VineVisualization.BuildMesh"),
				Parameters,
				ERDGPassFlags::Compute,
				[Parameters, ComputeShader, DispatchCount](FRHIComputeCommandList& InRHICmdList)
				{
					FComputeShaderUtils::Dispatch(InRHICmdList, ComputeShader, *Parameters, FComputeShaderUtils::GetGroupCount(FIntVector(DispatchCount, 1, 1), 64));
				});

			AddEnqueueCopyPass(GraphBuilder, VertexReadback, OutVertexBuffer.Buffer, VertexReadbackBytes);
			AddEnqueueCopyPass(GraphBuilder, UVReadback, OutUVBuffer.Buffer, UVReadbackBytes);
			AddEnqueueCopyPass(GraphBuilder, IndexReadback, OutIndexBuffer.Buffer, IndexReadbackBytes);
			GraphBuilder.Execute();
			bRenderWorkQueued = true;
		});

	FlushRenderingCommands();

	if (!bRenderWorkQueued)
	{
		delete VertexReadback;
		delete UVReadback;
		delete IndexReadback;
		return false;
	}

	OutVertices.SetNumZeroed(OutputVertexCount);
	OutUVs.SetNumZeroed(OutputVertexCount);
	OutIndices.SetNumZeroed(OutputIndexCount);
	bool bReadbackSucceeded = false;

	ENQUEUE_RENDER_COMMAND(VineVisualizationGPUReadback)(
		[VertexReadback, UVReadback, IndexReadback, VertexReadbackBytes, UVReadbackBytes, IndexReadbackBytes, &OutVertices, &OutUVs, &OutIndices, &bReadbackSucceeded](FRHICommandListImmediate& RHICmdList)
		{
			if (!VertexReadback || !UVReadback || !IndexReadback)
			{
				return;
			}

			if (!VertexReadback->IsReady() || !UVReadback->IsReady() || !IndexReadback->IsReady())
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

			delete VertexReadback;
			delete UVReadback;
			delete IndexReadback;
			bReadbackSucceeded = bLockedAll;
		});

	FlushRenderingCommands();
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

UDynamicMesh* AVineContainer::GetTubeMesh()
{
	return OutTubeMesh;
}

UDynamicMesh* AVineContainer::GetPlaneMesh()
{
	return OutPlaneMesh;
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
	return bUseGPU ? VisVineGPUInternal(MainVine) : VisVineCPU(MainVine);
}

bool AVineContainer::VisVineCPU(bool MainVine)
{
	TArray<FGeometryScriptPolyPath> Lines ;
	if (MainVine)
	{
		Lines = TubeLines;
	}
	else
	{
		Lines = PlaneLines;
	}
	const TArray<float>& LineSourceScales = MainVine ? TubeLineSourceScales : PlaneLineSourceScales;
	const TArray<FVector>& LineSourceLocations = MainVine ? TubeLineSourceLocations : PlaneLineSourceLocations;
	const TArray<FVineLinePointScaleData>& LinePointScales = MainVine ? TubeLinePointScales : PlaneLinePointScales;
	
	if (Lines.Num() == 0)
		return false;

	if (VV.CurveControl == nullptr)
		VV.CurveControl = NewObject<UCurveLinearColor>();

	if (!RebuildVineBVHForPrefixMesh(PrefixMesh, BVH, TEXT("VisVine")))
		return false;
	
	UDynamicMesh* ContainerMesh = GetDynamicMeshComponent()->GetDynamicMesh();
	UDynamicMesh* TubeMesh = NewObject<UDynamicMesh>();
	UDynamicMesh* PlaneMesh = NewObject<UDynamicMesh>();
	//ContainerMesh->Reset();
	TArray<FVector2D> Circle = {FVector2D(10, 0) * VV.CircleScale, FVector2D(-5, 8.66) * VV.CircleScale, FVector2D(-5, -8.66) * VV.CircleScale};
	TArray<FVector2D> Line2D = {FVector2D(-5, 0) * VV.LineScale, FVector2D(5, 0) * VV.LineScale};
	
	TArray<FGeometryScriptPolyPath> PreparedLines;
	TArray<float> PreparedLineSourceScales;
	TArray<FVineLinePointScaleData> PreparedLinePointScales;
	if (!PrepareVineVisualizationLinesCPU(
		Lines,
		VV,
		MainVine,
		LineSourceScales,
		LineSourceLocations,
		LinePointScales,
		PrefixMesh,
		BVH,
		PreparedLines,
		PreparedLineSourceScales,
		PreparedLinePointScales))
	{
		return false;
	}

	FGeometryScriptSpatialQueryOptions Options;
	for (int32 LineIdx = 0; LineIdx < PreparedLines.Num(); ++LineIdx)
	{
		FGeometryScriptPolyPath& Line = PreparedLines[LineIdx];
		const TArray<float>& CurrentPointScales = PreparedLinePointScales[LineIdx].Values;
		const float FallbackScale = PreparedLineSourceScales.IsValidIndex(LineIdx) ? PreparedLineSourceScales[LineIdx] : 1.0f;

		//CaluclateVineTransforms
		TArray<FVector> LineVectors = *Line.Path;
		int32 LineVertexNum = LineVectors.Num();
		TArray<FTransform> Transforms;
		Transforms.Reserve(LineVertexNum);
		TArray<float> TransformPointScales;
		TransformPointScales.Reserve(LineVertexNum);
		
		for (int32 i = 0; i < LineVertexNum; i++)
		{
			FVector Normal;
			FVector VertexLocation = (*Line.Path)[i];
			FGeometryScriptTrianglePoint NearestPoint;
			if (!TryFindNearestPointOnVinePrefixMesh(PrefixMesh, BVH, VertexLocation, Options, NearestPoint, TEXT("VisVine.TransformNormal")))
			{
				continue;
			}

			PrefixMesh->ProcessMesh([&](const FDynamicMesh3& EditMesh)
			{
				//Sometimes it will Calculates a downward normal it is wrong
				Normal = EditMesh.GetTriNormal(NearestPoint.TriangleID);
			});
			FVector Tangent = UE::Geometry::CurveUtil::Tangent<double, FVector>(LineVectors, i);
			Transforms.Add(FTransform(FRotationMatrix::MakeFromXZ(Tangent, Normal).Rotator(), LineVectors[i], FVector::OneVector));
			TransformPointScales.Add(CurrentPointScales.IsValidIndex(i) ? CurrentPointScales[i] : FallbackScale);
			//FRotationMatrix::MakeFromXZ()
		}
		
		int32 TransformCount = Transforms.Num();
		if (TransformCount < 3)
			continue;
		
		for (int32 i = 0; i < TransformCount; i++)
		{
			FTransform& Transform = Transforms[i];
			const float CurveScale = VV.CurveControl->GetUnadjustedLinearColorValue(i / (TransformCount - 1.0)).G;
			const float PointScale = TransformPointScales.IsValidIndex(i) ? TransformPointScales[i] : FallbackScale;
			const float SweepScale = CurveScale * FMath::Max(PointScale, 0.0f);
			Transform.SetScale3D(FVector::OneVector * SweepScale);
		}

		//AddMeshToReuslt
		if (MainVine)
		{
			FGeometryScriptPrimitiveOptions PrimitiveOptions;
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSweepPolygon(
				TubeMesh, PrimitiveOptions, FTransform::Identity, Circle, Transforms, false);
		}
		else
		{
			TArray<float> Line2DU = {0, 1};
			TArray<float> Line2DV = UPolyLine::CurveU(Line, false);
			FGeometryScriptPrimitiveOptions PrimitiveOptions;
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSweepPolyline(
				PlaneMesh, PrimitiveOptions, FTransform::Identity, Line2D, Transforms, Line2DU, Line2DV, false);

			// TArray<FVector2D> Line2DTemp = {FVector2D(10, 0) * VV.LineScale / 2, FVector2D(-5, 8.66) * VV.LineScale / 2, FVector2D(-5, -8.66) * VV.LineScale / 2};
			// UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSweepPolygon(
			// 	PlaneMesh, PrimitiveOptions, FTransform::Identity, Line2DTemp, Transforms, false);
		}
	}
	
	//GenerateReuslt
	FGeometryScriptAppendMeshOptions AppendOptions;
	if (MainVine)
	{
		TubeMesh->EditMesh([&](FDynamicMesh3& Mesh)
		{
			UE::Geometry::FDynamicMeshMaterialAttribute* MaterialIDs = Mesh.Attributes()->GetMaterialID();
			Mesh.TrianglesItr();
			int32 TriCount = Mesh.MaxTriangleID();
			for (int32 i = 0; i < TriCount; i++)
			{
				MaterialIDs->SetNewValue(i, 0);
			}
		}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);

		UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(ContainerMesh, TubeMesh, FTransform::Identity, false,
												  AppendOptions);
		
		// OutTubeMesh = NewObject<UDynamicMesh>();
		// OutTubeMesh->Reset();
		// UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(OutTubeMesh, TubeMesh, FTransform::Identity, false,
		// 														  AppendOptions);
	}
	else
	{
		int32 ContainerMeshTri = ContainerMesh->GetTriangleCount();
		int32 PlaneMeshTri = PlaneMesh->GetTriangleCount();
		
		FGeometryScriptCalculateNormalsOptions CalculateOptions;
		PlaneMesh = UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(PlaneMesh, CalculateOptions);
		UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(ContainerMesh, PlaneMesh, FTransform::Identity, false,
														  AppendOptions);

		// OutPlaneMesh = NewObject<UDynamicMesh>();
		// OutPlaneMesh->Reset();
		// UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(OutPlaneMesh, PlaneMesh, FTransform::Identity, false,
		// 											  AppendOptions);
		
		ContainerMesh->EditMesh([&](FDynamicMesh3& Mesh)
		{
			UE::Geometry::FDynamicMeshMaterialAttribute* MaterialIDs = Mesh.Attributes()->GetMaterialID();
			Mesh.TrianglesItr();
			int32 TriCount = Mesh.MaxTriangleID();
			for (int32 i = ContainerMeshTri; i < ContainerMeshTri + PlaneMeshTri; i++)
			{
				MaterialIDs->SetNewValue(i, 1);
			}
		}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	}
	TArray<int32> MaterialIDsTemp;
	ContainerMesh->EditMesh([&](FDynamicMesh3& Mesh)
	{
		UE::Geometry::FDynamicMeshMaterialAttribute* MaterialIDs = Mesh.Attributes()->GetMaterialID();
		Mesh.TrianglesItr();
		int32 TriCount = Mesh.MaxTriangleID();
		MaterialIDsTemp.Reserve(TriCount);
		for (int32 i = 0; i < TriCount; i++)
		{
			int32 Test = MaterialIDs->GetValue(i);
			MaterialIDsTemp.Add(Test);
		}
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);

	// FDynamicMesh3 MeshCopy;
	// ContainerMesh->ProcessMesh([&](const FDynamicMesh3& EditMesh)
	// {
	// 	MeshCopy = EditMesh;
	// });
	if (UDynamicMeshComponent* MeshComponent = GetDynamicMeshComponent())
	{
		TransformDynamicMeshToLocalSpace(ContainerMesh, MeshComponent->GetComponentTransform());
		MeshComponent->NotifyMeshUpdated();
		MeshComponent->UpdateBounds();
		MeshComponent->MarkRenderTransformDirty();
		MeshComponent->MarkRenderStateDirty();
	}
	
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

	if (!RebuildVineBVHForPrefixMesh(PrefixMesh, BVH, TEXT("VisVineGPU")))
	{
		return false;
	}

	TArray<FGeometryScriptPolyPath> PreparedLines;
	const TArray<float>& LineSourceScales = MainVine ? TubeLineSourceScales : PlaneLineSourceScales;
	const TArray<FVector>& LineSourceLocations = MainVine ? TubeLineSourceLocations : PlaneLineSourceLocations;
	const TArray<FVineLinePointScaleData>& LinePointScales = MainVine ? TubeLinePointScales : PlaneLinePointScales;
	TArray<float> PreparedLineSourceScales;
	TArray<FVineLinePointScaleData> PreparedLinePointScales;
	if (!PrepareVineVisualizationLinesCPU(
		Lines,
		VV,
		MainVine,
		LineSourceScales,
		LineSourceLocations,
		LinePointScales,
		PrefixMesh,
		BVH,
		PreparedLines,
		PreparedLineSourceScales,
		PreparedLinePointScales))
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] No valid lines after preprocessing."));
		return false;
	}

	TArray<FVector4f> PathPoints;
	TArray<FIntVector4> PathPointMeta;
	TArray<float> PathPointCurveU;
	TArray<FIntVector4> SegmentMeta;
	if (!BuildVineVisualizationGPUInput(PreparedLines, VV.CurveControl, PreparedLineSourceScales, PreparedLinePointScales, PathPoints, PathPointMeta, PathPointCurveU, SegmentMeta))
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] Failed to build GPU input buffers."));
		return false;
	}

	TArray<FVector4f> MeshTriangleVertices;
	if (!ExtractDynamicMeshTrianglesForGPU(PrefixMesh, MeshTriangleVertices))
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] Failed to extract PrefixMesh triangles."));
		return false;
	}

	TArray<FVector4f> OutVertices;
	TArray<FVector2f> OutUVs;
	TArray<uint32> OutIndices;
	if (!DispatchVineVisualizationGPU(
		PathPoints,
		PathPointMeta,
		PathPointCurveU,
		MeshTriangleVertices,
		SegmentMeta,
		MainVine,
		VV.CircleScale,
		VV.LineScale,
		OutVertices,
		OutUVs,
		OutIndices))
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] GPU dispatch/readback failed."));
		return false;
	}

	RecomputeVineOutputUVsFromGeneratedLength(OutVertices, SegmentMeta, MainVine ? 3u : 2u, OutUVs);

	const int32 MaterialID = MainVine ? 0 : 1;
	UDynamicMesh* VineMesh = BuildDynamicMeshFromGPUVineOutput(this, OutVertices, OutUVs, OutIndices, MaterialID, true);
	if (!VineMesh || VineMesh->GetTriangleCount() <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] GPU output produced no valid triangles."));
		return false;
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

	UE_LOG(LogTemp, Log, TEXT("[VisVineGPU] Appended %s vine mesh. Lines=%d Vertices=%d Indices=%d Triangles=%d"),
		MainVine ? TEXT("tube") : TEXT("plane"),
		PreparedLines.Num(),
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

	// 3. 生成 Prefix 投影 Mesh
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

	FGeometryScriptDynamicMeshBVH LocalBVH;
	// Keep the default bReverseOrientation=true here.
	// Although the collected triangle buffer is normalized against source normals, the current
	// DynamicMesh/BVH vine path was verified to project and render correctly with reversed winding.
	UDynamicMesh* MeshCombine = nullptr;
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.GetBoxSceneTrianglesFilteredToDynamicMesh"));
		MeshCombine = GetBoxSceneTrianglesFilteredToDynamicMesh(100.0f);
	}
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.BuildBVHForMesh"));
		MeshCombine = UGeometryScriptLibrary_MeshSpatial::BuildBVHForMesh(MeshCombine, LocalBVH, nullptr);
	}
	// 体素化表面计算（ResinRattan移植），数据在GenerateVines返回后自动释放
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.GetBoxSceneSurfaceVoxels"));
		(void)GetBoxSceneSurfaceVoxelsFromGPU(SC.VoxelSize);
	}
	// 缓存 PrefixMesh 和 BVH，用于投影查询
	BVH = LocalBVH;
	PrefixMesh = MeshCombine;
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
			}
		}
	}
	TubeLines = GeneratedTubeLines;
	TubeLineSourceScales = GeneratedTubeLineScales;
	TubeLineSourceLocations = GeneratedTubeLineSourceLocations;
	TubeLinePointScales = GeneratedTubeLinePointScales;

	// Plane Lines (CPU reference + GPU accelerated comparison)
	TArray<FGeometryScriptPolyPath> GeneratedPlaneLinesCPU;
	TArray<FGeometryScriptPolyPath> GeneratedPlaneLines;
	TArray<float> GeneratedPlaneLineScales;
	TArray<FVector> GeneratedPlaneLineSourceLocations;
	TArray<FVineLinePointScaleData> GeneratedPlaneLinePointScales;
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.GeneratePlaneLinesCPUAndCS"));
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
			}
		}
	}
	// LogSpaceColonizationLineComparison(TEXT("PlaneLines.Total"), GeneratedPlaneLinesCPU, GeneratedPlaneLines);
	PlaneLines = GeneratedPlaneLines;
	PlaneLineSourceScales = GeneratedPlaneLineScales;
	PlaneLineSourceLocations = GeneratedPlaneLineSourceLocations;
	PlaneLinePointScales = GeneratedPlaneLinePointScales;

	// 5. 可视化
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.VisTubeVine"));
		VisVine(true, bUseGPUMode);   // Tube 可视化 (swept polygon)
	}
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.VisPlaneVine"));
		VisVine(false, bUseGPUMode);  // Plane 可视化 (swept polyline)
	}



	return MeshCombine;
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
		UE_LOG(LogTemp, Warning, TEXT("[VineContainer] GenerateVineAction produced no prefix mesh on %s."), *GetActorNameOrLabel());
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
