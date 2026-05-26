// Fill out your copyright notice in the Description page of Project Settings.
#include "GeometryEditorActor.h"

#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "GenerateVines.h"
#include "Landscape.h"
#include "PointFunction.h"
#include "PCGPluginDebug.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "GeometryScript/MeshBasicEditFunctions.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "ComputeShaderGenerateHepler.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "ShaderParameterStruct.h"
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
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, MeshTriangleVertices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, SegmentMeta)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, GridCellOffsets)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, GridTriangleIndices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_OutVertices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_OutIndices)
		SHADER_PARAMETER(uint32, PathPointCount)
		SHADER_PARAMETER(uint32, SegmentCount)
		SHADER_PARAMETER(uint32, MeshTriangleCount)
		SHADER_PARAMETER(uint32, OutputVertexCount)
		SHADER_PARAMETER(uint32, OutputIndexCount)
		SHADER_PARAMETER(uint32, ProfileCount)
		SHADER_PARAMETER(uint32, bTube)
		SHADER_PARAMETER(float, VinesOffset)
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

static bool PrepareVineVisualizationLinesCPU(
	const TArray<FGeometryScriptPolyPath>& Lines,
	const FVineVisualization& VV,
	bool bMainVine,
	TArray<FGeometryScriptPolyPath>& OutLines)
{
	OutLines.Reset();
	OutLines.Reserve(Lines.Num());
	for (const FGeometryScriptPolyPath& InputLine : Lines)
	{
		if (!InputLine.Path.IsValid() || InputLine.Path->Num() < 2)
		{
			continue;
		}

		FGeometryScriptPolyPath Line = ClonePolyPath(InputLine);
		const float ArcLength = UE::Geometry::CurveUtil::ArcLength<float, FVector>(*Line.Path, false);
		const int32 NumIterations = int32(ArcLength / VV.ResampleLength);
		if (NumIterations < 2)
		{
			continue;
		}

		Line = UPolyLine::SmoothLine(Line, 3);
		Line = UPolyLine::ResamppleByLength(Line, VV.ResampleLength);
		if (!Line.Path.IsValid() || Line.Path->Num() < 3)
		{
			continue;
		}

		Line = UPolyLine::SmoothLine(Line, 1);
		Line = UPolyLine::ResamppleByLength(Line, VV.ResampleLength);
		if (Line.Path.IsValid() && Line.Path->Num() >= 3)
		{
			OutLines.Add(Line);
		}
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
	TArray<FVector4f>& OutPathPoints,
	TArray<FIntVector4>& OutPathPointMeta,
	TArray<FIntVector4>& OutSegmentMeta)
{
	OutPathPoints.Reset();
	OutPathPointMeta.Reset();
	OutSegmentMeta.Reset();

	for (const FGeometryScriptPolyPath& Line : Lines)
	{
		if (!Line.Path.IsValid() || Line.Path->Num() < 2)
		{
			continue;
		}

		const TArray<FVector>& Points = *Line.Path;
		const int32 BaseIndex = OutPathPoints.Num();
		const int32 PointCount = Points.Num();

		for (int32 PointIndex = 0; PointIndex < PointCount; ++PointIndex)
		{
			const FVector& Point = Points[PointIndex];
			const float Scale = EvaluateVineScale(CurveControl, PointIndex, PointCount);
			OutPathPoints.Add(FVector4f(float(Point.X), float(Point.Y), float(Point.Z), Scale));

			const int32 PrevIndex = BaseIndex + FMath::Max(PointIndex - 1, 0);
			const int32 NextIndex = BaseIndex + FMath::Min(PointIndex + 1, PointCount - 1);
			OutPathPointMeta.Add(FIntVector4(PrevIndex, NextIndex, BaseIndex, PointCount));

			if (PointIndex + 1 < PointCount)
			{
				OutSegmentMeta.Add(FIntVector4(BaseIndex + PointIndex, BaseIndex + PointIndex + 1, 0, 0));
			}
		}
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
			for (int32 CornerIndex = 0; CornerIndex < 3; ++CornerIndex)
			{
				const FVector3d Position = EditMesh.GetVertex(Tri[CornerIndex]);
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
	const TArray<FVector4f>& MeshTriangleVertices,
	const TArray<FIntVector4>& SegmentMeta,
	bool bTube,
	float VinesOffset,
	float CircleScale,
	float LineScale,
	TArray<FVector4f>& OutVertices,
	TArray<uint32>& OutIndices)
{
	OutVertices.Reset();
	OutIndices.Reset();

	const uint32 PathPointCount = uint32(PathPoints.Num());
	const uint32 SegmentCount = uint32(SegmentMeta.Num());
	const uint32 MeshTriangleCount = uint32(MeshTriangleVertices.Num() / 3);
	const uint32 ProfileCount = bTube ? 3u : 2u;
	const uint32 OutputVertexCount = PathPointCount * ProfileCount;
	const uint32 OutputIndexCount = bTube ? SegmentCount * ProfileCount * 6u : SegmentCount * 6u;
	if (PathPointCount == 0 || SegmentCount == 0 || MeshTriangleCount == 0 || OutputVertexCount == 0 || OutputIndexCount == 0)
	{
		return false;
	}

	const uint64 VertexReadbackBytes64 = uint64(OutputVertexCount) * sizeof(FVector4f);
	const uint64 IndexReadbackBytes64 = uint64(OutputIndexCount) * sizeof(uint32);
	if (VertexReadbackBytes64 > MAX_uint32 || IndexReadbackBytes64 > MAX_uint32)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] Output is too large for readback. Vertices=%u Indices=%u"), OutputVertexCount, OutputIndexCount);
		return false;
	}

	const uint32 VertexReadbackBytes = uint32(VertexReadbackBytes64);
	const uint32 IndexReadbackBytes = uint32(IndexReadbackBytes64);
	FRHIGPUBufferReadback* VertexReadback = new FRHIGPUBufferReadback(TEXT("VineVisualization_VertexReadback"));
	FRHIGPUBufferReadback* IndexReadback = new FRHIGPUBufferReadback(TEXT("VineVisualization_IndexReadback"));
	bool bRenderWorkQueued = false;

	// Build uniform grid acceleration structure on CPU
	const float GridCellSize = FMath::Max(200.0f, float(MeshTriangleCount) * 0.01f);
	FVineVisualizationGrid Grid = BuildUniformGridForTriangles(MeshTriangleVertices, GridCellSize);
	const bool bUseGrid = Grid.TotalCells > 1 && Grid.TriangleIndices.Num() > 0;

	ENQUEUE_RENDER_COMMAND(VineVisualizationGPU)(
		[PathPoints, PathPointMeta, MeshTriangleVertices, SegmentMeta, Grid, bUseGrid,
		 VertexReadback, IndexReadback, VertexReadbackBytes, IndexReadbackBytes,
		 PathPointCount, SegmentCount, MeshTriangleCount, ProfileCount, OutputVertexCount, OutputIndexCount,
		 bTube, VinesOffset, CircleScale, LineScale, &bRenderWorkQueued](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			const CSHepler::FRDGStructuredBufferRefs PathPointBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, PathPoints, TEXT("VineVisualization.PathPoints"));
			const CSHepler::FRDGStructuredBufferRefs PathPointMetaBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, PathPointMeta, TEXT("VineVisualization.PathPointMeta"));
			const CSHepler::FRDGStructuredBufferRefs MeshTriangleBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, MeshTriangleVertices, TEXT("VineVisualization.MeshTriangleVertices"));
			const CSHepler::FRDGStructuredBufferRefs SegmentMetaBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, SegmentMeta, TEXT("VineVisualization.SegmentMeta"));
			const CSHepler::FRDGStructuredBufferRefs OutVertexBuffer = CSHepler::CreateStructuredBuffer<FVector4f>(GraphBuilder, OutputVertexCount, TEXT("VineVisualization.OutVertices"), true, true);
			const CSHepler::FRDGStructuredBufferRefs OutIndexBuffer = CSHepler::CreateStructuredBuffer<uint32>(GraphBuilder, OutputIndexCount, TEXT("VineVisualization.OutIndices"), true, true);

			// Upload grid buffers
			const CSHepler::FRDGStructuredBufferRefs GridCellOffsetsBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, Grid.CellOffsets, TEXT("VineVisualization.GridCellOffsets"));
			const CSHepler::FRDGStructuredBufferRefs GridTriangleIndicesBuffer = CSHepler::CreateUploadedStructuredBuffer(GraphBuilder, Grid.TriangleIndices, TEXT("VineVisualization.GridTriangleIndices"));

			TShaderMapRef<FVineVisualizationMeshCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FVineVisualizationMeshCS::FParameters* Parameters = GraphBuilder.AllocParameters<FVineVisualizationMeshCS::FParameters>();
			Parameters->PathPoints = PathPointBuffer.SRV;
			Parameters->PathPointMeta = PathPointMetaBuffer.SRV;
			Parameters->MeshTriangleVertices = MeshTriangleBuffer.SRV;
			Parameters->SegmentMeta = SegmentMetaBuffer.SRV;
			Parameters->GridCellOffsets = GridCellOffsetsBuffer.SRV;
			Parameters->GridTriangleIndices = GridTriangleIndicesBuffer.SRV;
			Parameters->RW_OutVertices = OutVertexBuffer.UAV;
			Parameters->RW_OutIndices = OutIndexBuffer.UAV;
			Parameters->PathPointCount = PathPointCount;
			Parameters->SegmentCount = SegmentCount;
			Parameters->MeshTriangleCount = MeshTriangleCount;
			Parameters->OutputVertexCount = OutputVertexCount;
			Parameters->OutputIndexCount = OutputIndexCount;
			Parameters->ProfileCount = ProfileCount;
			Parameters->bTube = bTube ? 1u : 0u;
			Parameters->VinesOffset = VinesOffset;
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
			AddEnqueueCopyPass(GraphBuilder, IndexReadback, OutIndexBuffer.Buffer, IndexReadbackBytes);
			GraphBuilder.Execute();
			bRenderWorkQueued = true;
		});

	FlushRenderingCommands();

	if (!bRenderWorkQueued)
	{
		delete VertexReadback;
		delete IndexReadback;
		return false;
	}

	OutVertices.SetNumZeroed(OutputVertexCount);
	OutIndices.SetNumZeroed(OutputIndexCount);
	bool bReadbackSucceeded = false;

	ENQUEUE_RENDER_COMMAND(VineVisualizationGPUReadback)(
		[VertexReadback, IndexReadback, VertexReadbackBytes, IndexReadbackBytes, &OutVertices, &OutIndices, &bReadbackSucceeded](FRHICommandListImmediate& RHICmdList)
		{
			if (!VertexReadback || !IndexReadback)
			{
				return;
			}

			if (!VertexReadback->IsReady() || !IndexReadback->IsReady())
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
			delete IndexReadback;
			bReadbackSucceeded = bLockedAll;
		});

	FlushRenderingCommands();
	return bReadbackSucceeded;
}

static UDynamicMesh* BuildDynamicMeshFromGPUVineOutput(
	UObject* Outer,
	const TArray<FVector4f>& Vertices,
	const TArray<uint32>& Indices,
	int32 MaterialID,
	bool bRecomputeNormals)
{
	if (Vertices.Num() == 0 || Indices.Num() < 3)
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
	for (const FVector4f& Vertex : Vertices)
	{
		Mesh.AppendVertex(FVector3d(Vertex.X, Vertex.Y, Vertex.Z));
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

		const int32 TriangleID = Mesh.AppendTriangle(A, B, C);
		if (TriangleID >= 0)
		{
			Mesh.Attributes()->GetMaterialID()->SetNewValue(TriangleID, MaterialID);
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
}

bool AVineContainer::CheckActors(TArray<AActor*> CheckActors)
{
	for (AActor* Actor : CheckActors)
	{
		if (!PickActors.Contains(Actor))
			return false;
	}
	FVector Center = BVH.Spatial.Get()->GetBoundingBox().Center();
	FVector Extent = BVH.Spatial.Get()->GetBoundingBox().Extents();
	if (InstanceBound.GetCenter() != Center || InstanceBound.GetExtent() != Extent)
	{
		return false;
	}
	return true;
}

void AVineContainer::AddInstanceFromFoliageType(UFoliageType* InFoliageType)
{
	if (InFoliageType == nullptr)
		return;

	
	
	TArray<FTransform> Transforms;
	GetAllFoliageInstanceTransforms(GetWorld(), InFoliageType, Transforms);
	FString FoliageTypeName = InFoliageType->GetName();
	if (FoliageTypeName == TEXT("SMF_TubeVine_FoliageType"))
	{
		TubeVineSource->AddInstances(Transforms, false, true, false);

	}
	if (FoliageTypeName == TEXT("SMF_PlaneVine_FoliageType"))
	{
		PlaneVineSource->AddInstances(Transforms, false, true, false);

	}
	if (FoliageTypeName == TEXT("SMF_Target_FoliageType"))
	{
		GrowTarget->AddInstances(Transforms, false, true, false);
	}
	for (TActorIterator<AInstancedFoliageActor> It(GetWorld()); It; ++It)
	{
		AInstancedFoliageActor* IFA = (*It);
		IFA->RemoveFoliageType(&InFoliageType, 1);
	}
}

void AVineContainer::ConvertInstance(UFoliageType* InFoliageType)
{
	ConvertInstanceToFoliage(InFoliageType);
}

UDynamicMesh* AVineContainer::GetTubeMesh()
{
	return OutTubeMesh;
}

UDynamicMesh* AVineContainer::GetPlaneMesh()
{
	return OutPlaneMesh;
}

void AVineContainer::ConvertInstanceToFoliage(UFoliageType* InFoliageType)
{
	if (InFoliageType == nullptr)
		return;
	
	FString FoliageTypeName = InFoliageType->GetName();
	UInstancedStaticMeshComponent* InstanceContainerTemp = nullptr;
	
	if ( FoliageTypeName == TEXT("SMF_TubeVine_FoliageType"))
		InstanceContainerTemp = TubeVineSource;
	
	if ( FoliageTypeName == TEXT("SMF_PlaneVine_FoliageType"))
		InstanceContainerTemp = PlaneVineSource;
	
	if ( FoliageTypeName == TEXT("SMF_Target_FoliageType"))
		InstanceContainerTemp = GrowTarget;

	if (InstanceContainerTemp == nullptr)
		return;
	
	int32 InstanceCount = InstanceContainerTemp->GetInstanceCount();
	if (InstanceCount == 0)
		return;

	TArray<FTransform> Transforms;
	Transforms.Reserve(InstanceCount);
	for (int32 i = 0; i < InstanceCount; i++)
	{
		FTransform Transform;
		InstanceContainerTemp->GetInstanceTransform(i, Transform, true);
		Transforms.Add(Transform);
	}

	TMap<AInstancedFoliageActor*, TArray<const FFoliageInstance*>> InstancesToAdd;
	TArray<FFoliageInstance> FoliageInstances;
	FoliageInstances.Reserve(Transforms.Num()); // Reserve 

	for (const FTransform& InstanceTransfo : Transforms)
	{
		AInstancedFoliageActor* IFA = AInstancedFoliageActor::Get(GWorld, true, GWorld->PersistentLevel, InstanceTransfo.GetLocation());
		FFoliageInstance FoliageInstance;
		FoliageInstance.Location = InstanceTransfo.GetLocation();
		FoliageInstance.Rotation = InstanceTransfo.GetRotation().Rotator();
		FoliageInstance.DrawScale3D = (FVector3f)InstanceTransfo.GetScale3D();

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

	InstanceContainerTemp->ClearInstances();
	RefreshFoliageType(GetWorld(), InFoliageType);
}

void AVineContainer::VisVine( bool MainVine)
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
	
	if (Lines.Num() == 0)
		return;

	if (VV.CurveControl == nullptr)
		VV.CurveControl = NewObject<UCurveLinearColor>();

	if (!RebuildVineBVHForPrefixMesh(PrefixMesh, BVH, TEXT("VisVine")))
		return;
	
	UDynamicMesh* ContainerMesh = GetDynamicMeshComponent()->GetDynamicMesh();
	UDynamicMesh* TubeMesh = NewObject<UDynamicMesh>();
	UDynamicMesh* PlaneMesh = NewObject<UDynamicMesh>();
	//ContainerMesh->Reset();
	TArray<FVector2D> Circle = {FVector2D(10, 0) * VV.CircleScale, FVector2D(-5, 8.66) * VV.CircleScale, FVector2D(-5, -8.66) * VV.CircleScale};
	TArray<FVector2D> Line2D = {FVector2D(-5, 0) * VV.LineScale, FVector2D(5, 0) * VV.LineScale};
	
	TArray<FGeometryScriptPolyPath> TempLines;
	TempLines.Reserve(Lines.Num());
	for (int32 i = 0; i < Lines.Num(); i++)
	{
		FGeometryScriptPolyPath Line = Lines[i];
		TArray<FVector> PathVertices;
		PathVertices.Reserve((*Line.Path).Num());
		for (int32 j = 0; j < (*Line.Path).Num(); j++)
		{
			PathVertices.Add((*Line.Path)[j]);
		}
		FGeometryScriptPolyPath PolyPath;
		PolyPath.Reset();
		*PolyPath.Path = PathVertices;
		TempLines.Add(PolyPath);
	}
	
	//CreateVinesMesh
	int32 SampleRangePointsSumCount = 0;
	TArray<FVector> SampleRangePointsSum;
	FGeometryScriptSpatialQueryOptions Options;
	for (FGeometryScriptPolyPath& Line : TempLines)
	{
		float ArcLength = UE::Geometry::CurveUtil::ArcLength<float, FVector>(*Line.Path, false);
		int32 NumIterations = int32(ArcLength / VV.ResampleLength);
		if (NumIterations < 2)
			continue;
		
		Line = UPolyLine::SmoothLine(Line, 3);
		Line = UPolyLine::ResamppleByLength(Line, VV.ResampleLength);
		int32 VertexCount = (*Line.Path).Num();
		
		//NoiseOffset
		for (int32 n = 0; n < 10; n++)
		{
			VertexCount = (*Line.Path).Num();
			for (int32 i = 0; i < VertexCount; i++)
			{
				FVector& VertexLocation = (*Line.Path)[i];
				FGeometryScriptTrianglePoint NearestPoint;
				if (TryFindNearestPointOnVinePrefixMesh(PrefixMesh, BVH, VertexLocation, Options, NearestPoint, TEXT("VisVine.NoiseProject")))
				{
					VertexLocation = NearestPoint.Position;
				}
				
				UNoise::CurlNoise(VertexLocation, VertexLocation, FVector::ZeroVector, VV.CurlNoiseScale / 10, VV.CurlNoiseFre);
				FRandomStream Random(332);
				const float RandomOffset = 10000.0f * Random.FRand();
				FVector NoisePos = (FVector)(VV.PerlinNoiseFre / 100 * (VertexLocation));
				float OffsetNoise = VV.PerlinNoiseScale * FMath::PerlinNoise3D(NoisePos);
				float PerlinOffset = VV.CurveControl->GetUnadjustedLinearColorValue(i / (VertexCount - 1.0)).R;
				VertexLocation.X += OffsetNoise * PerlinOffset * (1 - float(MainVine));
			}
			Line = UPolyLine::ResamppleByLength(Line, VV.ResampleLength);
		}
		VertexCount = (*Line.Path).Num();
		for (int32 i = 0; i < VertexCount; i++)
		{
			FVector& VertexLocation = (*Line.Path)[i];
			UNoise::CurlNoise(VertexLocation, VertexLocation, FVector::ZeroVector, VV.CurlNoiseScale / 10, VV.CurlNoiseFre);
		}
		
		TArray<FVector> LinePoints = (*Line.Path);
		int32 Test = (*Line.Path).Num();
		TArray<FVector> SampleRangePoints;
		int32 SampleRangeCount = LinePoints.Num();
		SampleRangePoints.Reserve(SampleRangeCount);
		for (int32 i = 0; i < (*Line.Path).Num() * .8; i++)
		{
			SampleRangePoints.Add((*Line.Path)[i]);
		}
		SampleRangePointsSum.Append(SampleRangePoints);
		SampleRangePointsSumCount += (*Line.Path).Num();
	}

	if (SampleRangePointsSum.Num() == 0)
		return;
	
	//Merget Sections of vines
	float SampleInterval = 15;
	SampleRangePointsSum.Sort([](FVector A, FVector B) { return FMath::Rand() > .5; });
	SampleRangePointsSum.SetNum(SampleRangePointsSum.Num() / SampleInterval);
	
	for (FGeometryScriptPolyPath& Line : TempLines)
	{
		if (!MainVine)
		{
			int32 VertexCount = (*Line.Path).Num();
			for (int32 i = 0; i < VertexCount; i++)
			{
				FVector& VertexLocation = (*Line.Path)[i];
				int32 NearPt = UPointFunction::FindNearPointIteration(SampleRangePointsSum, VertexLocation);
				float Dist = FVector::Dist(SampleRangePointsSum[NearPt], VertexLocation);
				if (Dist > VV.ResampleLength * VV.MergeDistMult)
					continue;
				FVector NoisePos = (FVector)(VV.PerlinNoiseFre / 100 * (VertexLocation));
				float OffsetNoise = FMath::Abs(FMath::PerlinNoise3D(NoisePos + FVector::OneVector * 10));
				OffsetNoise = VV.CurveControl->GetUnadjustedLinearColorValue(OffsetNoise).B;
				VertexLocation = FMath::Lerp(VertexLocation, SampleRangePointsSum[NearPt], OffsetNoise);
			}
			Line = UPolyLine::ResamppleByLength(Line, VV.ResampleLength);
			Line = UPolyLine::SmoothLine(Line, 3);
			Line = UPolyLine::ResamppleByLength(Line, VV.ResampleLength);
			VertexCount = (*Line.Path).Num();
			for (int32 i = 0; i < VertexCount; i++)
			{
				FVector& VertexLocation = (*Line.Path)[i];
				FGeometryScriptTrianglePoint NearestPoint;
				if (TryFindNearestPointOnVinePrefixMesh(PrefixMesh, BVH, VertexLocation, Options, NearestPoint, TEXT("VisVine.MergeProject")))
				{
					VertexLocation = NearestPoint.Position;
				}
			}
		}

		int32 VertexCount = (*Line.Path).Num();

		//OffsetVine
		float VineOffset = VV.VinesOffset;
		for (int32 i = 0; i < VertexCount; i++)
		{
			FVector NormalSum = FVector::ZeroVector;
			// for (int32 n = 0; n < 6; n++)
			// {
			// 	float OffsetSerchDist = 50;
			// 	FVector VertexLocation = (*Line.Path)[i];
			// 	FRandomStream Random(123 * VertexLocation.X * i);
			// 	const FVector RandomOffset = Random.VRand();
			// 	FRandomStream RandomDist(.012385 * VertexLocation.X * i);
			// 	const float RandomOffsetDist = RandomDist.FRand() * OffsetSerchDist;
			// 	VertexLocation += RandomOffset * RandomOffsetDist;
			// 	FGeometryScriptTrianglePoint NearestPoint;
			// 	EGeometryScriptSearchOutcomePins Outcome;
			// 	UGeometryScriptLibrary_MeshSpatial::FindNearestPointOnMesh(
			// 	PrefixMesh, BVH, VertexLocation, Options, NearestPoint, Outcome, nullptr);
			// 	PrefixMesh->ProcessMesh([&](const FDynamicMesh3& EditMesh)
			// 	{
			// 		//Sometimes it will Calculates a downward normal it is wrong
			// 		//FVector Normal = EditMesh.GetTriBaryNormal(NearestPoint.TriangleID, NearestPoint.BaryCoords[0], NearestPoint.BaryCoords[1], NearestPoint.BaryCoords[2]);
			// 		FVector Normal = EditMesh.GetTriNormal(NearestPoint.TriangleID);
			// 		NormalSum += Normal * (RandomOffsetDist / OffsetSerchDist);
			// 	});
			// }
			// float OffsetSerchDist = 50;
			FVector VertexLocation = (*Line.Path)[i];
			FRandomStream Random(123 * VertexLocation.X * i);
			const FVector RandomOffset = Random.VRand();
			FRandomStream RandomDist(.012385 * VertexLocation.X * i);
			// const float RandomOffsetDist = RandomDist.FRand() * OffsetSerchDist;
			// VertexLocation += RandomOffset * RandomOffsetDist;
			FGeometryScriptTrianglePoint NearestPoint;
			if (!TryFindNearestPointOnVinePrefixMesh(PrefixMesh, BVH, VertexLocation, Options, NearestPoint, TEXT("VisVine.OffsetNormal")))
			{
				continue;
			}
			FVector Normal;
			PrefixMesh->ProcessMesh([&](const FDynamicMesh3& EditMesh)
			{
				//Sometimes it will Calculates a downward normal it is wrong
				//FVector Normal = EditMesh.GetTriBaryNormal(NearestPoint.TriangleID, NearestPoint.BaryCoords[0], NearestPoint.BaryCoords[1], NearestPoint.BaryCoords[2]);
				Normal = EditMesh.GetTriNormal(NearestPoint.TriangleID);
				//NormalSum += Normal * (RandomOffsetDist / OffsetSerchDist);
			});
			FVector& VertexLocationFix = (*Line.Path)[i];
			VertexLocationFix += Normal * VineOffset;
			VertexLocationFix.Z += FMath::FRandRange(0, 0.1);
		}
		Line = UPolyLine::ResamppleByLength(Line, VV.ResampleLength);
		Line = UPolyLine::SmoothLine(Line, 1);
		
		//CaluclateVineTransforms
		TArray<FVector> LineVectors = *Line.Path;
		int32 LineVertexNum = LineVectors.Num();
		TArray<FTransform> Transforms;
		Transforms.Reserve(LineVertexNum);
		
		for (int32 i = 0; i < LineVertexNum; i++)
		{
			FVector Normal;
			FVector VertexLocation = (*Line.Path)[i];
			FGeometryScriptTrianglePoint NearestPoint;
			if (!TryFindNearestPointOnVinePrefixMesh(PrefixMesh, BVH, VertexLocation, Options, NearestPoint, TEXT("VisVine.TransformNormal")))
			{
				continue;
			}

			FVector TestNormal;
			FVector3f VertexNormal ;
			FVector3f VertexNormal1 ;
			FVector3f VertexNormal2 ;
			FVector3d n;
			PrefixMesh->ProcessMesh([&](const FDynamicMesh3& EditMesh)
			{
				//Sometimes it will Calculates a downward normal it is wrong
				TestNormal = EditMesh.GetTriBaryNormal(NearestPoint.TriangleID, NearestPoint.BaryCoords[0], NearestPoint.BaryCoords[1], NearestPoint.BaryCoords[2]);
				Normal = EditMesh.GetTriNormal(NearestPoint.TriangleID);
				UE::Geometry::FIndex3i id = EditMesh.GetTriangle(NearestPoint.TriangleID);
				VertexNormal = EditMesh.GetVertexNormal(id[0]);
				VertexNormal1 = EditMesh.GetVertexNormal(id[1]);
				VertexNormal2 = EditMesh.GetVertexNormal(id[2]);
				n = FVector3d(NearestPoint.BaryCoords[0] * EditMesh.GetVertexNormal(id[0]) + NearestPoint.BaryCoords[1] * EditMesh.GetVertexNormal(id[1]) + NearestPoint.BaryCoords[2] * EditMesh.GetVertexNormal(id[2]));
				n.Normalize();
			});
			FVector Tangent = UE::Geometry::CurveUtil::Tangent<double, FVector>(LineVectors, i);
			Transforms.Add(FTransform(FRotationMatrix::MakeFromXZ(Tangent, Normal).Rotator(), LineVectors[i], FVector::OneVector));
			//FRotationMatrix::MakeFromXZ()
		}
		
		int32 TransformCount = Transforms.Num();
		if (TransformCount < 3)
			continue;
		
		for (int32 i = 0; i < TransformCount; i++)
		{
			FTransform& Transform = Transforms[i];
			float SweepScale = VV.CurveControl->GetUnadjustedLinearColorValue(i / (TransformCount - 1.0)).G;
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
	
}

bool AVineContainer::VisVineGPU(bool MainVine)
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
	if (!PrepareVineVisualizationLinesCPU(Lines, VV, MainVine, PreparedLines))
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] No valid lines after preprocessing."));
		return false;
	}

	TArray<FVector4f> PathPoints;
	TArray<FIntVector4> PathPointMeta;
	TArray<FIntVector4> SegmentMeta;
	if (!BuildVineVisualizationGPUInput(PreparedLines, VV.CurveControl, PathPoints, PathPointMeta, SegmentMeta))
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
	TArray<uint32> OutIndices;
	if (!DispatchVineVisualizationGPU(
		PathPoints,
		PathPointMeta,
		MeshTriangleVertices,
		SegmentMeta,
		MainVine,
		VV.VinesOffset,
		VV.CircleScale,
		VV.LineScale,
		OutVertices,
		OutIndices))
	{
		UE_LOG(LogTemp, Warning, TEXT("[VisVineGPU] GPU dispatch/readback failed."));
		return false;
	}

	const int32 MaterialID = MainVine ? 0 : 1;
	UDynamicMesh* VineMesh = BuildDynamicMeshFromGPUVineOutput(this, OutVertices, OutIndices, MaterialID, true);
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
	DynamicMeshComponent->GetDynamicMesh()->Reset();
}

UDynamicMesh* AVineContainer::GenerateVines(FSpaceColonizationOptions SC, float ExtrudeScale, bool Result, bool MultThread)
{
	GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.Total"));
	// 1. 收集 Source / Target Transforms
	TArray<FTransform> TubeSourceTransforms;
	TArray<FTransform> PlaneSourceTransforms;
	TArray<FTransform> TargetTransforms;

	if (!GrowTarget || !TubeVineSource || !PlaneVineSource)
	{
		return nullptr;
	}

	const int32 TargetCount = GrowTarget->GetInstanceCount();
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.CollectTargetTransforms"));
		TargetTransforms.Reserve(TargetCount);
		for (int32 i = 0; i < TargetCount; i++)
		{
			FTransform Transform;
			GrowTarget->GetInstanceTransform(i, Transform, true);
			TargetTransforms.Add(Transform);
		}
	}

	const int32 TubeSourceCount = TubeVineSource->GetInstanceCount();
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.CollectTubeSourceTransforms"));
		TubeSourceTransforms.Reserve(TubeSourceCount);
		for (int32 i = 0; i < TubeSourceCount; i++)
		{
			FTransform Transform;
			TubeVineSource->GetInstanceTransform(i, Transform, true);
			TubeSourceTransforms.Add(Transform);
		}
	}

	const int32 PlaneSourceCount = PlaneVineSource->GetInstanceCount();
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.CollectPlaneSourceTransforms"));
		PlaneSourceTransforms.Reserve(PlaneSourceCount);
		for (int32 i = 0; i < PlaneSourceCount; i++)
		{
			FTransform Transform;
			PlaneVineSource->GetInstanceTransform(i, Transform, true);
			PlaneSourceTransforms.Add(Transform);
		}
	}

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

	TArray<TEnumAsByte<EObjectTypeQuery>> ObjectTypes = {
		UEngineTypes::ConvertToObjectType(ECollisionChannel::ECC_WorldStatic)
	};
	TArray<AActor*> OverlapActors;
	TArray<AActor*> ActorsToIgnore;
	TArray<AActor*> MeshActors;
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.BoxOverlapAndFilterActors"));
		UKismetSystemLibrary::BoxOverlapActors(GetWorld(), Center, Extent, ObjectTypes, nullptr, ActorsToIgnore, OverlapActors);

		for (AActor* Actor : OverlapActors)
		{
			if (!Cast<ALandscape>(Actor) && !Cast<ALandscapeProxy>(Actor))
			{
				MeshActors.Add(Actor);
			}
		}
	}

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
	// 缓存 PrefixMesh 和 BVH，用于投影查询
	BVH = LocalBVH;
	PrefixMesh = MeshCombine;
	InstanceBound = Bounds;
	PickActors = MeshActors;

	// 如果只需要输出 Debug Mesh 或不需要最终结果
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.CreateContainerMesh"));
		UDynamicMesh* ContainerMesh = NewObject<UDynamicMesh>(this);
		GetDynamicMeshComponent()->SetDynamicMesh(ContainerMesh);
	}

	// 4. 执行 SpaceColonization
	// Tube Lines
	TArray<FGeometryScriptPolyPath> GeneratedTubeLines;
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.GenerateTubeLines"));
		for (int32 i = 0; i < TubeSourceCount; i++)
		{
			TArray<FTransform> SCSourceTransform;
			SCSourceTransform.Add(TubeSourceTransforms[i]);
			GeneratedTubeLines.Append(UGenerateVines::SpaceColonization(
				SCSourceTransform, TargetTransforms,
				SC.Iteration, SC.Activetime, 5,
				SC.RandGrow, SC.Seed, SC.BackGrowRange, MultThread));
			GeneratedTubeLines.Append(UGenerateVines::SpaceColonizationCS(
		SCSourceTransform, TargetTransforms,
		SC.Iteration, SC.Activetime, 12,
		SC.RandGrow, SC.Seed, SC.BackGrowRange));
		}
	}
	TubeLines = GeneratedTubeLines;

	// Plane Lines (CPU reference + GPU accelerated comparison)
	TArray<FGeometryScriptPolyPath> GeneratedPlaneLinesCPU;
	TArray<FGeometryScriptPolyPath> GeneratedPlaneLines;
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.GeneratePlaneLinesCPUAndCS"));
		for (int32 i = 0; i < PlaneSourceCount; i++)
		{
			TArray<FTransform> SCSourceTransform;
			SCSourceTransform.Add(PlaneSourceTransforms[i]);
			GeneratedPlaneLinesCPU.Append(UGenerateVines::SpaceColonization(
				SCSourceTransform, TargetTransforms,
				SC.Iteration, SC.Activetime, 12,
				SC.RandGrow, SC.Seed, SC.BackGrowRange, MultThread));
			GeneratedPlaneLines.Append(UGenerateVines::SpaceColonizationCS(
				SCSourceTransform, TargetTransforms,
				SC.Iteration, SC.Activetime, 12,
				SC.RandGrow, SC.Seed, SC.BackGrowRange));
		}
	}
	LogSpaceColonizationLineComparison(TEXT("PlaneLines.Total"), GeneratedPlaneLinesCPU, GeneratedPlaneLines);
	PlaneLines = GeneratedPlaneLines;

	// 5. 可视化
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.VisTubeVine"));
		VisVineGPU(true);   // Tube 可视化 (swept polygon)
	}
	{
		GV_ACTOR_TIME_SCOPE(TEXT("AVineContainer.GenerateVines.VisPlaneVine"));
		VisVineGPU(false);  // Plane 可视化 (swept polyline)
	}

	return MeshCombine;
}

void AVineContainer::AddInstance(UFoliageType* InFoliageType)
{
	AddInstanceFromFoliageType(InFoliageType);
}
