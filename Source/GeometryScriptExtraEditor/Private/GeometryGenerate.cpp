#include "GeometryGenerate.h"

#include "CollisionQueryParams.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "StaticMeshRenderDataPointSampler.h"
#include "DetailLayoutBuilder.h"
#include "UDynamicMesh.h"
#include "LandscapeExtra.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/MeshTransforms.h"
#include "DynamicMeshEditor.h"
#include "DynamicMeshToMeshDescription.h"
#include "TransformSequence.h"
#include "Kismet/GameplayStatics.h"
#include "ConversionUtils/SceneComponentToDynamicMesh.h"
#include "GeometryScript/MeshVoxelFunctions.h"
#include "GeometryScript/MeshSelectionFunctions.h"
#include "GeometryScript/MeshSelectionQueryFunctions.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryScript/MeshBasicEditFunctions.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "GeometryScript/PolygonFunctions.h"
#include "GeometryScript/MeshSpatialFunctions.h"
#include "MeshDescription.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "DynamicMeshToMeshDescription.h"
#include "Landscape.h"
#include "ProxyLODVolume.h"
#include "StaticMeshAttributes.h"
#include "GeometryMathUtils.h"
#include "Curve/CurveUtil.h"
#include "Selection/GeometrySelector.h"
#include "EngineUtils.h"
#include "GeometryScript/MeshVertexColorFunctions.h"
#include "DynamicMesh/DynamicMeshOverlay.h"
#include "Operations/SmoothDynamicMeshAttributes.h"
#include "Spatial/MeshAABBTree3.h"
#include "LevelEditorViewport.h"
#include "AssetUtils/CreateStaticMeshUtil.h"
#include "AssetUtils/CreateSkeletalMeshUtil.h"
#include "AssetUtils/CreateTexture2DUtil.h"
#include "PackageTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CleaningOps/HoleFillOp.h"
#include "DataWrappers/ChaosVDParticleDataWrapper.h"
#include "DynamicMesh/MeshNormals.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "GeometryScript/MeshRemeshFunctions.h"
#include "GeometryScript/MeshSamplingFunctions.h"
#include "Interfaces/IHttpResponse.h"
#include "Properties/UVLayoutProperties.h"
#include "VDBExtra.h"
#include "GeometryAsync.h"
#include "GeometryGeneral.h"


using namespace UE::Geometry;

namespace
{
bool PointInsideBox(const FBox& Box, const FVector& Point, double Tolerance = KINDA_SMALL_NUMBER)
{
	return Box.IsValid
		&& Point.X >= Box.Min.X - Tolerance && Point.X <= Box.Max.X + Tolerance
		&& Point.Y >= Box.Min.Y - Tolerance && Point.Y <= Box.Max.Y + Tolerance
		&& Point.Z >= Box.Min.Z - Tolerance && Point.Z <= Box.Max.Z + Tolerance;
}

FBox IntersectBoxes(const FBox& A, const FBox& B)
{
	if (!A.IsValid || !B.IsValid || !A.Intersect(B))
	{
		return FBox(ForceInit);
	}

	const FVector Min(
		FMath::Max(A.Min.X, B.Min.X),
		FMath::Max(A.Min.Y, B.Min.Y),
		FMath::Max(A.Min.Z, B.Min.Z));
	const FVector Max(
		FMath::Min(A.Max.X, B.Max.X),
		FMath::Min(A.Max.Y, B.Max.Y),
		FMath::Min(A.Max.Z, B.Max.Z));

	TArray<FVector> Corners;
	Corners.Reserve(2);
	Corners.Add(Min);
	Corners.Add(Max);
	return FBox(Corners);
}

float GetPointSamplingSpacing(const FBox& Box, float DesiredSpacing, int32 MaxPoints)
{
	float Spacing = FMath::Max(DesiredSpacing, 1.0f);
	if (!Box.IsValid || MaxPoints <= 0)
	{
		return Spacing;
	}

	const FVector Size = Box.GetSize();
	const double EstimatedSurfacePoints = 2.0 * (
		FMath::Max(1.0, Size.X / Spacing) * FMath::Max(1.0, Size.Y / Spacing)
		+ FMath::Max(1.0, Size.X / Spacing) * FMath::Max(1.0, Size.Z / Spacing)
		+ FMath::Max(1.0, Size.Y / Spacing) * FMath::Max(1.0, Size.Z / Spacing));

	if (EstimatedSurfacePoints > MaxPoints)
	{
		Spacing *= FMath::Sqrt(EstimatedSurfacePoints / MaxPoints);
	}

	return Spacing;
}

int32 GridSteps(double Length, float Spacing)
{
	return FMath::Max(1, FMath::CeilToInt(Length / Spacing));
}

double GridValue(double Min, double Max, int32 Index, int32 Steps)
{
	return Steps <= 0 ? (Min + Max) * 0.5 : FMath::Lerp(Min, Max, double(Index) / double(Steps));
}

bool AddUniqueActorPoint(const FVector& Point, const FBox& Bounds, float VoxelSize, TSet<FIntVector>& UniqueKeys, TArray<FVector>& OutPoints)
{
	if (!PointInsideBox(Bounds, Point))
	{
		return false;
	}

	const float CellSize = FMath::Max(VoxelSize * 0.25f, 1.0f);
	const FIntVector Key(
		FMath::RoundToInt(Point.X / CellSize),
		FMath::RoundToInt(Point.Y / CellSize),
		FMath::RoundToInt(Point.Z / CellSize));
	if (UniqueKeys.Contains(Key))
	{
		return false;
	}

	UniqueKeys.Add(Key);
	OutPoints.Add(Point);
	return true;
}

bool TraceComponentPoint(UPrimitiveComponent* Component, const FVector& Start, const FVector& End, const FBox& SampleBox,
                         const FBox& Bounds, float VoxelSize, TSet<FIntVector>& UniqueKeys, TArray<FVector>& OutPoints)
{
	if (!Component)
	{
		return false;
	}

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(VDBMeshFromActorPoints), true);
	QueryParams.bReturnFaceIndex = false;
	QueryParams.bReturnPhysicalMaterial = false;

	FHitResult Hit;
	if (!Component->LineTraceComponent(Hit, Start, End, QueryParams))
	{
		return false;
	}
	if (!PointInsideBox(SampleBox, Hit.ImpactPoint))
	{
		return false;
	}

	return AddUniqueActorPoint(Hit.ImpactPoint, Bounds, VoxelSize, UniqueKeys, OutPoints);
}

int32 AddComponentTracePoints(UPrimitiveComponent* Component, const FBox& SampleBox, const FBox& Bounds, float Spacing,
                              int32 MaxPoints, float VoxelSize, TSet<FIntVector>& UniqueKeys, TArray<FVector>& OutPoints)
{
	if (!Component || !SampleBox.IsValid)
	{
		return 0;
	}

	const int32 StartPointCount = OutPoints.Num();
	const FVector Min = SampleBox.Min;
	const FVector Max = SampleBox.Max;
	const float Padding = FMath::Max(Spacing, VoxelSize);
	const int32 XSteps = GridSteps(Max.X - Min.X, Spacing);
	const int32 YSteps = GridSteps(Max.Y - Min.Y, Spacing);
	const int32 ZSteps = GridSteps(Max.Z - Min.Z, Spacing);

	auto CanAddMore = [&]()
	{
		return MaxPoints <= 0 || (OutPoints.Num() - StartPointCount) < MaxPoints;
	};

	auto TraceBothWays = [&](const FVector& Start, const FVector& End)
	{
		if (!CanAddMore())
		{
			return;
		}
		TraceComponentPoint(Component, Start, End, SampleBox, Bounds, VoxelSize, UniqueKeys, OutPoints);
		if (CanAddMore())
		{
			TraceComponentPoint(Component, End, Start, SampleBox, Bounds, VoxelSize, UniqueKeys, OutPoints);
		}
	};

	for (int32 YIndex = 0; YIndex <= YSteps && CanAddMore(); ++YIndex)
	{
		const double Y = GridValue(Min.Y, Max.Y, YIndex, YSteps);
		for (int32 ZIndex = 0; ZIndex <= ZSteps && CanAddMore(); ++ZIndex)
		{
			const double Z = GridValue(Min.Z, Max.Z, ZIndex, ZSteps);
			TraceBothWays(FVector(Min.X - Padding, Y, Z), FVector(Max.X + Padding, Y, Z));
		}
	}

	for (int32 XIndex = 0; XIndex <= XSteps && CanAddMore(); ++XIndex)
	{
		const double X = GridValue(Min.X, Max.X, XIndex, XSteps);
		for (int32 ZIndex = 0; ZIndex <= ZSteps && CanAddMore(); ++ZIndex)
		{
			const double Z = GridValue(Min.Z, Max.Z, ZIndex, ZSteps);
			TraceBothWays(FVector(X, Min.Y - Padding, Z), FVector(X, Max.Y + Padding, Z));
		}
	}

	for (int32 XIndex = 0; XIndex <= XSteps && CanAddMore(); ++XIndex)
	{
		const double X = GridValue(Min.X, Max.X, XIndex, XSteps);
		for (int32 YIndex = 0; YIndex <= YSteps && CanAddMore(); ++YIndex)
		{
			const double Y = GridValue(Min.Y, Max.Y, YIndex, YSteps);
			TraceBothWays(FVector(X, Y, Min.Z - Padding), FVector(X, Y, Max.Z + Padding));
		}
	}

	return OutPoints.Num() - StartPointCount;
}

int32 AddBoxSurfacePoints(const FBox& SampleBox, const FBox& Bounds, float Spacing, int32 MaxPoints,
                          float VoxelSize, TSet<FIntVector>& UniqueKeys, TArray<FVector>& OutPoints)
{
	if (!SampleBox.IsValid)
	{
		return 0;
	}

	const int32 StartPointCount = OutPoints.Num();
	const FVector Min = SampleBox.Min;
	const FVector Max = SampleBox.Max;
	const int32 XSteps = GridSteps(Max.X - Min.X, Spacing);
	const int32 YSteps = GridSteps(Max.Y - Min.Y, Spacing);
	const int32 ZSteps = GridSteps(Max.Z - Min.Z, Spacing);

	auto CanAddMore = [&]()
	{
		return MaxPoints <= 0 || (OutPoints.Num() - StartPointCount) < MaxPoints;
	};

	auto AddPoint = [&](const FVector& Point)
	{
		if (CanAddMore())
		{
			AddUniqueActorPoint(Point, Bounds, VoxelSize, UniqueKeys, OutPoints);
		}
	};

	for (int32 XIndex = 0; XIndex <= XSteps && CanAddMore(); ++XIndex)
	{
		const double X = GridValue(Min.X, Max.X, XIndex, XSteps);
		for (int32 YIndex = 0; YIndex <= YSteps && CanAddMore(); ++YIndex)
		{
			const double Y = GridValue(Min.Y, Max.Y, YIndex, YSteps);
			AddPoint(FVector(X, Y, Min.Z));
			AddPoint(FVector(X, Y, Max.Z));
		}
	}

	for (int32 XIndex = 0; XIndex <= XSteps && CanAddMore(); ++XIndex)
	{
		const double X = GridValue(Min.X, Max.X, XIndex, XSteps);
		for (int32 ZIndex = 0; ZIndex <= ZSteps && CanAddMore(); ++ZIndex)
		{
			const double Z = GridValue(Min.Z, Max.Z, ZIndex, ZSteps);
			AddPoint(FVector(X, Min.Y, Z));
			AddPoint(FVector(X, Max.Y, Z));
		}
	}

	for (int32 YIndex = 0; YIndex <= YSteps && CanAddMore(); ++YIndex)
	{
		const double Y = GridValue(Min.Y, Max.Y, YIndex, YSteps);
		for (int32 ZIndex = 0; ZIndex <= ZSteps && CanAddMore(); ++ZIndex)
		{
			const double Z = GridValue(Min.Z, Max.Z, ZIndex, ZSteps);
			AddPoint(FVector(Min.X, Y, Z));
			AddPoint(FVector(Max.X, Y, Z));
		}
	}

	return OutPoints.Num() - StartPointCount;
}

bool ShouldSampleBounds(const FBox& ActorBounds, const FBox& Bounds, const TArray<FVector>& BBoxVectors)
{
	if (!ActorBounds.IsValid || !ActorBounds.Intersect(Bounds))
	{
		return false;
	}

	if (BBoxVectors.Num() == 0)
	{
		return true;
	}

	for (const FVector& BBoxVector : BBoxVectors)
	{
		if (PointInsideBox(ActorBounds, BBoxVector))
		{
			return true;
		}
	}

	return true;
}

int32 EstimateGpuPointCount(const FBox& SampleBox, float DesiredSpacing, int32 MaxPoints)
{
	if (!SampleBox.IsValid || MaxPoints == 0)
	{
		return 0;
	}

	const FVector Size = SampleBox.GetSize();
	const float Spacing = FMath::Max(DesiredSpacing, 1.0f);
	const double EstimatedSurfacePoints = 2.0 * (
		FMath::Max(1.0, Size.X / Spacing) * FMath::Max(1.0, Size.Y / Spacing)
		+ FMath::Max(1.0, Size.X / Spacing) * FMath::Max(1.0, Size.Z / Spacing)
		+ FMath::Max(1.0, Size.Y / Spacing) * FMath::Max(1.0, Size.Z / Spacing));
	const int32 ClampedEstimate = FMath::Max(1, FMath::CeilToInt(EstimatedSurfacePoints));
	return MaxPoints > 0 ? FMath::Min(ClampedEstimate, MaxPoints) : ClampedEstimate;
}

void AddActorComponentPoints(UStaticMeshComponent* StaticMeshComponent, const FBox& ComponentBounds, const FBox& Bounds,
                             float DesiredSpacing, int32 MaxPointsPerComponent, float VoxelSize,
                             TSet<FIntVector>& UniqueKeys, TArray<FVector>& OutPoints)
{
	const FBox SampleBox = IntersectBoxes(ComponentBounds, Bounds);
	if (!SampleBox.IsValid)
	{
		return;
	}

	const float Spacing = GetPointSamplingSpacing(SampleBox, DesiredSpacing, MaxPointsPerComponent);
	const int32 AddedByTrace = AddComponentTracePoints(StaticMeshComponent, SampleBox, Bounds, Spacing, MaxPointsPerComponent,
	                                                   VoxelSize, UniqueKeys, OutPoints);
	if (AddedByTrace == 0)
	{
		AddBoxSurfacePoints(SampleBox, Bounds, Spacing, MaxPointsPerComponent, VoxelSize, UniqueKeys, OutPoints);
	}
}

bool IsValidCSTriangleIndex(int32 Index, int32 VertexCount)
{
	return Index >= 0 && Index < VertexCount;
}

bool IsFiniteCSVertex(const FVector& Vertex)
{
	return !Vertex.ContainsNaN()
		&& FMath::IsFinite(Vertex.X)
		&& FMath::IsFinite(Vertex.Y)
		&& FMath::IsFinite(Vertex.Z);
}

bool IsDegenerateCSTriangle(const FVector& A, const FVector& B, const FVector& C)
{
	const FVector AB = B - A;
	const FVector AC = C - A;
	const double AreaSq4 = FVector::CrossProduct(AB, AC).SizeSquared();
	return AreaSq4 <= 1.0e-8;
}

UDynamicMesh* BuildDynamicMeshFromCSTriangleData(const TArray<FVector>& Vertices,
	const TArray<int32>& Indices,
	const TArray<FVector>& VertexNormals,
	int32 VertexCount,
	int32 IndexCount,
	bool bReverseOrientation,
	bool bSkipDegenerateTriangles,
	bool bRecomputeNormals)
{
	UDynamicMesh* OutMesh = NewObject<UDynamicMesh>();
	if (!OutMesh)
	{
		return nullptr;
	}
	OutMesh->Reset();

	const int32 EffectiveVertexCount = VertexCount >= 0
		? FMath::Clamp(VertexCount, 0, Vertices.Num())
		: Vertices.Num();
	const int32 EffectiveIndexCount = IndexCount >= 0
		? FMath::Clamp(IndexCount, 0, Indices.Num())
		: Indices.Num();

	if (EffectiveVertexCount < 3)
	{
		return OutMesh;
	}

	const bool bUseTriangleSoup = EffectiveIndexCount == 0;
	const int32 TriangleCount = bUseTriangleSoup ? EffectiveVertexCount / 3 : EffectiveIndexCount / 3;
	if (TriangleCount <= 0)
	{
		return OutMesh;
	}

	const bool bUseInputVertexNormals = !bRecomputeNormals && VertexNormals.Num() >= EffectiveVertexCount;

	FDynamicMesh3 Mesh;
	TArray<int32> VertexIDMap;
	VertexIDMap.Reserve(EffectiveVertexCount);
	for (int32 VertexIndex = 0; VertexIndex < EffectiveVertexCount; ++VertexIndex)
	{
		const FVector& Vertex = Vertices[VertexIndex];
		if (IsFiniteCSVertex(Vertex))
		{
			VertexIDMap.Add(Mesh.AppendVertex(FVector3d(Vertex)));
		}
		else
		{
			VertexIDMap.Add(INDEX_NONE);
		}
	}

	if (bUseInputVertexNormals)
	{
		Mesh.EnableVertexNormals(FVector3f::UpVector);
		for (int32 VertexIndex = 0; VertexIndex < EffectiveVertexCount; ++VertexIndex)
		{
			const int32 MeshVertexID = VertexIDMap[VertexIndex];
			if (MeshVertexID == INDEX_NONE || VertexNormals[VertexIndex].ContainsNaN())
			{
				continue;
			}

			const FVector SafeNormal = VertexNormals[VertexIndex].GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
			Mesh.SetVertexNormal(MeshVertexID, FVector3f(SafeNormal));
		}
	}

	int32 AddedTriangles = 0;
	for (int32 TriangleIndex = 0; TriangleIndex < TriangleCount; ++TriangleIndex)
	{
		int32 A = INDEX_NONE;
		int32 B = INDEX_NONE;
		int32 C = INDEX_NONE;

		if (bUseTriangleSoup)
		{
			A = TriangleIndex * 3 + 0;
			B = TriangleIndex * 3 + 1;
			C = TriangleIndex * 3 + 2;
		}
		else
		{
			A = Indices[TriangleIndex * 3 + 0];
			B = Indices[TriangleIndex * 3 + 1];
			C = Indices[TriangleIndex * 3 + 2];
		}

		if (!IsValidCSTriangleIndex(A, EffectiveVertexCount)
			|| !IsValidCSTriangleIndex(B, EffectiveVertexCount)
			|| !IsValidCSTriangleIndex(C, EffectiveVertexCount)
			|| VertexIDMap[A] == INDEX_NONE
			|| VertexIDMap[B] == INDEX_NONE
			|| VertexIDMap[C] == INDEX_NONE
			|| A == B || B == C || A == C)
		{
			continue;
		}

		if (bSkipDegenerateTriangles && IsDegenerateCSTriangle(Vertices[A], Vertices[B], Vertices[C]))
		{
			continue;
		}

		if (bReverseOrientation)
		{
			Swap(B, C);
		}

		const int32 NewTriangleID = Mesh.AppendTriangle(FIndex3i(VertexIDMap[A], VertexIDMap[B], VertexIDMap[C]), 0);
		if (NewTriangleID >= 0)
		{
			++AddedTriangles;
		}
	}

	if (AddedTriangles == 0)
	{
		return OutMesh;
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





UDynamicMesh* UGeometryGenerate::CSTriangleDataToDynamicMesh(FCSTriangleMeshData CSTriangleData, bool bReverseOrientation, bool bSkipDegenerateTriangles, bool bRecomputeNormals)
{
	return CSTriangleBuffersToDynamicMesh(
		CSTriangleData.Vertices,
		CSTriangleData.Indices,
		CSTriangleData.VertexNormals,
		CSTriangleData.VertexCount,
		CSTriangleData.IndexCount,
		bReverseOrientation,
		bSkipDegenerateTriangles,
		bRecomputeNormals);
}

UDynamicMesh* UGeometryGenerate::CSTriangleBuffersToDynamicMesh(TArray<FVector> Vertices, TArray<int32> Indices, TArray<FVector> VertexNormals, int32 VertexCount, int32 IndexCount, bool bReverseOrientation, bool bSkipDegenerateTriangles, bool bRecomputeNormals)
{
	return BuildDynamicMeshFromCSTriangleData(
		Vertices,
		Indices,
		VertexNormals,
		VertexCount,
		IndexCount,
		bReverseOrientation,
		bSkipDegenerateTriangles,
		bRecomputeNormals);
}

UDynamicMesh* UGeometryGenerate::CSTriangleReadbackToDynamicMesh(const TArray<FVector4f>& CompactVertices, const TArray<uint32>& CompactIndices, const TArray<FVector4f>& CompactVertexNormals, int32 VertexCount, int32 IndexCount, bool bReverseOrientation, bool bSkipDegenerateTriangles, bool bRecomputeNormals)
{
	const int32 EffectiveVertexCount = VertexCount >= 0
		? FMath::Clamp(VertexCount, 0, CompactVertices.Num())
		: CompactVertices.Num();
	const int32 EffectiveIndexCount = IndexCount >= 0
		? FMath::Clamp(IndexCount, 0, CompactIndices.Num())
		: CompactIndices.Num();

	TArray<FVector> Vertices;
	Vertices.Reserve(EffectiveVertexCount);
	for (int32 VertexIndex = 0; VertexIndex < EffectiveVertexCount; ++VertexIndex)
	{
		const FVector4f& Vertex = CompactVertices[VertexIndex];
		Vertices.Add(FVector(Vertex.X, Vertex.Y, Vertex.Z));
	}

	TArray<int32> Indices;
	Indices.Reserve(EffectiveIndexCount);
	for (int32 IndexBufferIndex = 0; IndexBufferIndex < EffectiveIndexCount; ++IndexBufferIndex)
	{
		const uint32 Index = CompactIndices[IndexBufferIndex];
		Indices.Add(Index <= uint32(TNumericLimits<int32>::Max()) ? int32(Index) : INDEX_NONE);
	}

	TArray<FVector> VertexNormals;
	if (CompactVertexNormals.Num() >= EffectiveVertexCount)
	{
		VertexNormals.Reserve(EffectiveVertexCount);
		for (int32 NormalIndex = 0; NormalIndex < EffectiveVertexCount; ++NormalIndex)
		{
			const FVector4f& Normal = CompactVertexNormals[NormalIndex];
			VertexNormals.Add(FVector(Normal.X, Normal.Y, Normal.Z));
		}
	}

	return BuildDynamicMeshFromCSTriangleData(
		Vertices,
		Indices,
		VertexNormals,
		EffectiveVertexCount,
		EffectiveIndexCount,
		bReverseOrientation,
		bSkipDegenerateTriangles,
		bRecomputeNormals);
}

UDynamicMesh* UGeometryGenerate::VDBMeshFromActors(TArray<AActor*> In_Actors, TArray<FVector> BBoxVertors, bool Result, int32 ExtentPlus, float VoxelSize, float LandscapeMeshExtrude, bool MultThread)
{
	//float LandscapeMeshExtrude = 100;
	FBox Bounds(BBoxVertors);
	FVector Center = Bounds.GetCenter();
	FVector Extent = Bounds.GetExtent();
	
	UDynamicMesh* OutMesh = NewObject<UDynamicMesh>();
	TArray<UStaticMeshComponent*> AppendMeshComponents;
	for (AActor* Actor : In_Actors)
	{
		TArray<UStaticMeshComponent*> StaticMeshComponents;
		Actor->GetComponents(UStaticMeshComponent::StaticClass(), StaticMeshComponents);
		AppendMeshComponents.Append(StaticMeshComponents);
	}
	//CollectSceneMeshComponennt
	FTransform TransformCenter = FTransform::Identity;
	TArray<UStaticMesh*> BoundStaticMeshs;
	TArray<FTransform> BoundTransforms;
	TMap<UStaticMesh*, TArray<FTransform>> BoundTransformMap;
	for (UStaticMeshComponent* AppendMeshComponent : AppendMeshComponents)
	{
		if (Cast<UInstancedStaticMeshComponent>(AppendMeshComponent))
		{
			UInstancedStaticMeshComponent* Instances = Cast<UInstancedStaticMeshComponent>(AppendMeshComponent);
			int32 InstanceCount = Instances->GetInstanceCount();
			UStaticMesh* InstanceMesh = Instances->GetStaticMesh();
			
			for (int32 i = 0; i < InstanceCount; i++)
			{
				FTransform InstanceTransform = FTransform::Identity;
				Instances->GetInstanceTransform(i, InstanceTransform);
				FBox StaticMeshBound = InstanceMesh->GetBoundingBox();
				StaticMeshBound = StaticMeshBound.TransformBy(InstanceTransform);
				
				if (!StaticMeshBound.Intersect(Bounds))
					continue;

				bool VectorInBox = false;
				for (FVector BBoxVector : BBoxVertors)
				{
					if (StaticMeshBound.IsInside(BBoxVector))
						VectorInBox = true;
				}
				if (!VectorInBox)
					continue;

				if (BoundTransformMap.Contains(InstanceMesh))
				{
					TArray<FTransform>* Transforms = BoundTransformMap.Find(InstanceMesh);
					Transforms->Add(InstanceTransform);

				}
				else
				{
					BoundTransformMap.Add(InstanceMesh, {InstanceTransform});
					//BoundTransforms.Add(InstanceTransform);
				}
			}
			continue;
		}
		UStaticMesh* StaticMesh = AppendMeshComponent->GetStaticMesh();
		FTransform Transform = AppendMeshComponent->GetComponentToWorld();
		FBox StaticMeshBound = StaticMesh->GetBoundingBox();
		StaticMeshBound = StaticMeshBound.TransformBy(Transform);
		bool VectorInBox = false;
		for (FVector BBoxVector : BBoxVertors)
		{
			if (StaticMeshBound.IsInside(BBoxVector))
				VectorInBox = true;
		}
		if (!VectorInBox)
			continue;

		if (BoundTransformMap.Contains(StaticMesh))
		{
			TArray<FTransform>* Transforms = BoundTransformMap.Find(StaticMesh);
			Transforms->Add(Transform);

		}
		else
		{
			BoundTransformMap.Add(StaticMesh, {Transform});
		}
	}
	//ConvertMeshs
	TArray<UStaticMesh*> BoundTransformMapKeyArray;
	TArray<TArray<FTransform>> BoundTransformMapValueArray;
	BoundTransformMap.GenerateKeyArray(BoundTransformMapKeyArray);
	BoundTransformMap.GenerateValueArray(BoundTransformMapValueArray);

	if (BoundTransformMapKeyArray.Num() > 0)
	{
		UDynamicMesh* DynamicMeshCollection = NewObject<UDynamicMesh>();
		DynamicMeshCollection->Reset();

		if (MultThread)
		{
			SCOPE_CYCLE_COUNTER(STAT_SCConvertMeshMultThread)
			CollectMeshsMultThread(DynamicMeshCollection, BoundTransformMapKeyArray, BoundTransformMapValueArray, Bounds, LandscapeMeshExtrude, VoxelSize);
		}
		else
		{
			SCOPE_CYCLE_COUNTER(STAT_SCConvertMesh)
			CollectMeshs(DynamicMeshCollection, BoundTransformMapKeyArray, BoundTransformMapValueArray, Bounds, LandscapeMeshExtrude);
		}
		SCOPE_CYCLE_COUNTER(STAT_SCConvertMesh)
		//CollectMeshs(DynamicMeshCollection, BoundTransformMapKeyArray, BoundTransformMapValueArray, Bounds, LandscapeMeshExtrude);
		UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(OutMesh, DynamicMeshCollection, FTransform::Identity);
	}
	
	//CreateLandscapeMesh
	UDynamicMesh* LandscapePlaneMesh = NewObject<UDynamicMesh>(); 
	ULandscapeExtra::CreateProjectPlane(LandscapePlaneMesh, Center, Extent * 1.1, ExtentPlus);
	UDynamicMesh* BoundaryMesh = FixUnclosedBoundary(LandscapePlaneMesh, LandscapeMeshExtrude, false, false);
	UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(OutMesh, LandscapePlaneMesh, FTransform(FVector(0, 0, -LandscapeMeshExtrude)));
	UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(OutMesh, LandscapePlaneMesh, FTransform::Identity);
	UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(OutMesh, BoundaryMesh, FTransform::Identity);
	
	if (!Result)
		return OutMesh;

	
	OutMesh = VoxelMergeMeshs(OutMesh , VoxelSize);
	FGeometryScriptCalculateNormalsOptions CalculateOptions;
	UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(OutMesh, CalculateOptions);
	
	//MeshAttributeTest
	// OutMesh->EditMesh([&](FDynamicMesh3& Mesh)
	// 	{
	// 		Mesh.Attributes()->HasAttachedAttribute("TestAttrib");
	// 		Mesh.Attributes()->AttachAttribute("TestAttrib" , new TDynamicMeshVertexAttribute<float, 3>(&Mesh));
	// 		int32 NumLayer = Mesh.Attributes()->NumWeightLayers();
	// 		int32 VCount = Mesh.VertexCount();
	// 		TDynamicMeshVertexAttribute<float, 3>* Weight = static_cast<TDynamicMeshVertexAttribute<float, 3>*>(Mesh.Attributes()->GetAttachedAttribute("TestAttrib"));
	// 		for (int VID : Mesh.VertexIndicesItr())
	// 		{
	// 			FVector Test = FVector::ZeroVector;
	// 			Weight->SetValue(VID, FVector(1, 0, 0));
	// 			//Mesh.vertex
	// 			Weight->GetValue(VID, Test);
	// 			Test = FVector::ZeroVector;
	// 		}
	// 	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	
	return OutMesh;
}

UDynamicMesh* UGeometryGenerate::VDBMeshFromActorPoints(TArray<AActor*> In_Actors, TArray<FVector> BBoxVertors, bool Result, int32 ExtentPlus,
                                                        float VoxelSize, float LandscapeMeshExtrude, float PointSpacing,
                                                        float PointRadiusMult, int32 MaxPointsPerComponent)
{
	FBox Bounds(BBoxVertors);
	if (!Bounds.IsValid)
	{
		return NewObject<UDynamicMesh>();
	}

	const FVector Center = Bounds.GetCenter();
	const FVector Extent = Bounds.GetExtent();
	const float DesiredSpacing = PointSpacing > 0 ? PointSpacing : FMath::Max(VoxelSize, 1.0f);

	TArray<FVector> SamplePoints;
	TSet<FIntVector> UniquePointKeys;
	struct FPointFallbackRequest
	{
		UStaticMeshComponent* Component = nullptr;
		FBox ComponentBounds = FBox(ForceInit);
	};
	TArray<FStaticMeshRenderDataPointSampleRequest> GpuSampleRequests;
	TArray<FPointFallbackRequest> FallbackRequests;

	for (AActor* Actor : In_Actors)
	{
		if (!Actor)
		{
			continue;
		}

		TArray<UStaticMeshComponent*> StaticMeshComponents;
		Actor->GetComponents(UStaticMeshComponent::StaticClass(), StaticMeshComponents);
		for (UStaticMeshComponent* StaticMeshComponent : StaticMeshComponents)
		{
			if (!StaticMeshComponent || !StaticMeshComponent->GetStaticMesh())
			{
				continue;
			}

			if (UInstancedStaticMeshComponent* InstancedComponent = Cast<UInstancedStaticMeshComponent>(StaticMeshComponent))
			{
				const UStaticMesh* InstanceMesh = InstancedComponent->GetStaticMesh();
				if (!InstanceMesh)
				{
					continue;
				}

				const FBox MeshBounds = InstanceMesh->GetBoundingBox();
				for (int32 InstanceIndex = 0; InstanceIndex < InstancedComponent->GetInstanceCount(); ++InstanceIndex)
				{
					FTransform InstanceTransform = FTransform::Identity;
					InstancedComponent->GetInstanceTransform(InstanceIndex, InstanceTransform, true);
					const FBox InstanceBounds = MeshBounds.TransformBy(InstanceTransform);
					if (!ShouldSampleBounds(InstanceBounds, Bounds, BBoxVertors))
					{
						continue;
					}

					const FBox SampleBox = IntersectBoxes(InstanceBounds, Bounds);
					FStaticMeshRenderDataPointSampleRequest& GpuRequest = GpuSampleRequests.AddDefaulted_GetRef();
					GpuRequest.StaticMesh = const_cast<UStaticMesh*>(InstanceMesh);
					GpuRequest.LODIndex = 0;
					GpuRequest.LocalToWorld = InstanceTransform;
					GpuRequest.WorldBounds = SampleBox;
					GpuRequest.MaxPoints = EstimateGpuPointCount(SampleBox, DesiredSpacing, MaxPointsPerComponent);
					GpuRequest.VoxelCellSize = FMath::Max(VoxelSize * 0.25f, 1.0f);

					FPointFallbackRequest& FallbackRequest = FallbackRequests.AddDefaulted_GetRef();
					FallbackRequest.Component = StaticMeshComponent;
					FallbackRequest.ComponentBounds = InstanceBounds;
				}
				continue;
			}

			const FBox ComponentBounds = StaticMeshComponent->Bounds.GetBox();
			if (!ShouldSampleBounds(ComponentBounds, Bounds, BBoxVertors))
			{
				continue;
			}

			const FBox SampleBox = IntersectBoxes(ComponentBounds, Bounds);
			FStaticMeshRenderDataPointSampleRequest& GpuRequest = GpuSampleRequests.AddDefaulted_GetRef();
			GpuRequest.StaticMesh = StaticMeshComponent->GetStaticMesh();
			GpuRequest.LODIndex = 0;
			GpuRequest.LocalToWorld = StaticMeshComponent->GetComponentTransform();
			GpuRequest.WorldBounds = SampleBox;
			GpuRequest.MaxPoints = EstimateGpuPointCount(SampleBox, DesiredSpacing, MaxPointsPerComponent);
			GpuRequest.VoxelCellSize = FMath::Max(VoxelSize * 0.25f, 1.0f);

			FPointFallbackRequest& FallbackRequest = FallbackRequests.AddDefaulted_GetRef();
			FallbackRequest.Component = StaticMeshComponent;
			FallbackRequest.ComponentBounds = ComponentBounds;
		}
	}

	TArray<int32> GpuPointsPerRequest;
	TArray<FVector> GpuPoints;
	FStaticMeshRenderDataPointSampler::SamplePointsSync(GpuSampleRequests, GpuPoints, GpuPointsPerRequest);
	for (const FVector& Point : GpuPoints)
	{
		AddUniqueActorPoint(Point, Bounds, VoxelSize, UniquePointKeys, SamplePoints);
	}

	for (int32 RequestIndex = 0; RequestIndex < FallbackRequests.Num(); ++RequestIndex)
	{
		const bool bGpuProducedPoints = GpuPointsPerRequest.IsValidIndex(RequestIndex) && GpuPointsPerRequest[RequestIndex] > 0;
		if (bGpuProducedPoints)
		{
			continue;
		}

		const FPointFallbackRequest& FallbackRequest = FallbackRequests[RequestIndex];
		AddActorComponentPoints(FallbackRequest.Component, FallbackRequest.ComponentBounds, Bounds, DesiredSpacing, MaxPointsPerComponent,
		                        VoxelSize, UniquePointKeys, SamplePoints);
	}

	UDynamicMesh* OutMesh = NewObject<UDynamicMesh>();
	if (SamplePoints.Num() > 0)
	{
		UVDBExtra::ParticlesToVDBMeshUniform(OutMesh, SamplePoints, PointRadiusMult, VoxelSize);
	}

	UDynamicMesh* LandscapePlaneMesh = NewObject<UDynamicMesh>();
	ULandscapeExtra::CreateProjectPlane(LandscapePlaneMesh, Center, Extent * 1.1, ExtentPlus);
	UDynamicMesh* BoundaryMesh = FixUnclosedBoundary(LandscapePlaneMesh, LandscapeMeshExtrude, false, false);
	UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(OutMesh, LandscapePlaneMesh, FTransform(FVector(0, 0, -LandscapeMeshExtrude)));
	UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(OutMesh, LandscapePlaneMesh, FTransform::Identity);
	UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(OutMesh, BoundaryMesh, FTransform::Identity);

	if (!Result)
	{
		return OutMesh;
	}

	OutMesh = VoxelMergeMeshs(OutMesh, VoxelSize);
	FGeometryScriptCalculateNormalsOptions CalculateOptions;
	UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(OutMesh, CalculateOptions);
	return OutMesh;
}

UDynamicMesh* UGeometryGenerate::ExtrudeUnclosedBoundary(UDynamicMesh* FixMesh, float Offset, bool AppendMesh)
{
	FGeometryScriptMeshSelection Selection;
	UGeometryScriptLibrary_MeshSelectionFunctions::CreateSelectAllMeshSelection(FixMesh, Selection, EGeometryScriptMeshSelectionType::Triangles);

	TArray<FGeometryScriptIndexList> IndexLoops;
	TArray<FGeometryScriptPolyPath> PathLoops;
	int32 NumLoops = 0;
	bool bFoundErrors = false;
	UGeometryScriptLibrary_MeshSelectionQueryFunctions::GetMeshSelectionBoundaryLoops(FixMesh, Selection,IndexLoops, PathLoops, NumLoops, bFoundErrors, nullptr);

	int32 LoopNum = IndexLoops.Num();
	FGeometryScriptPrimitiveOptions PrimitiveOptions;
	FDynamicMesh3 BoundaryMeshCombine;
	UDynamicMesh* FillFanMeshCombine = NewObject<UDynamicMesh>();
	UGeometryGeneral::CreateVertexNormalFromOverlay(FixMesh);
	for (int32 i = 0; i < LoopNum; i++)
	{
		FGeometryScriptPolyPath Pathloop = PathLoops[i];
		FGeometryScriptIndexList IndexLoop = IndexLoops[i];
		TArray<FVector> Vertices = *Pathloop.Path;
		TArray<FVector> Tangents;
		TArray<int32> LoopVertexNumber = *IndexLoop.List;

		Tangents.Reserve(Vertices.Num());
		for (int32 j = 0; j < Vertices.Num(); j++)
		{
			FVector Tangent = UE::Geometry::CurveUtil::Tangent<double, FVector>(Vertices, j);
			Tangents.Add(Tangent);
		}

		UDynamicMesh* BoundaryMesh = NewObject<UDynamicMesh>();

		TArray<FTransform> PathTransforms;
		PathTransforms.Reserve(Vertices.Num());
		TArray<float> PathTexParamV;
		PathTexParamV.Reserve(Vertices.Num());
		for (FVector Vertece : Vertices)
		{
			FTransform Transform(Vertece);
			PathTransforms.Add(Transform);
			PathTexParamV.Add(0);
		}
		PathTexParamV.Add(0);
		TArray<FVector2D> PolylineVertices;
		PolylineVertices.Add(FVector2D(0, 0));
		PolylineVertices.Add(FVector2D(0, 0));
		TArray<float> PolylineTexParamU;
		PolylineTexParamU.Add(0);
		PolylineTexParamU.Add(1);

		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSweepPolyline(
			BoundaryMesh, PrimitiveOptions, FTransform::Identity, PolylineVertices, PathTransforms, PolylineTexParamU,
			PathTexParamV, true, 1, 1, 0);

		UDynamicMesh* FillFanMesh = NewObject<UDynamicMesh>();
		
		FixMesh->EditMesh([&](FDynamicMesh3& FixEditMesh)
		{
			BoundaryMesh->EditMesh([&](FDynamicMesh3& EditMesh)
			{
				int32 VertexCount = EditMesh.VertexCount() / 2;
				for (int i = 0; i < VertexCount; i++)
				{
					int32 TarId = i * 2 + 1;
					if (EditMesh.IsVertex(TarId))
					{
						FVector Normal = (FVector)FixEditMesh.GetVertexNormal(LoopVertexNumber[i]);
						FVector Tangent = Tangents[i];
						FVector Dir = FVector::CrossProduct(Tangent, Normal);
						FVector BoundaryLocation = EditMesh.GetVertex(TarId);
						BoundaryLocation -= Normal * Offset;
						
						FVector& VertexLocation = Vertices[i];
						VertexLocation -= Normal * Offset;
						EditMesh.SetVertex(TarId, BoundaryLocation);
					}
				}

				FMeshIndexMappings TmpMappings;
				FDynamicMeshEditor Editor = nullptr;
				if (AppendMesh)
				{
					Editor = FDynamicMeshEditor(&FixEditMesh);
				}
				else
				{
					Editor = FDynamicMeshEditor(&BoundaryMeshCombine);
				}

				FTransform XForm = FTransform::Identity;

				Editor.AppendMesh(&EditMesh, TmpMappings,
				                  [&](int, const FVector3d& Position) { return XForm.TransformPosition(Position); });
			}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
		}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
		
		UGeometryGeneral::FillLine(FillFanMesh, Vertices);
		UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(FixMesh, FillFanMesh, FTransform::Identity);
		UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(FillFanMeshCombine, FillFanMesh, FTransform::Identity);
	}
	if (!AppendMesh)
	{
		UDynamicMesh* MeshCombineOut = NewObject<UDynamicMesh>();
		MeshCombineOut->SetMesh(MoveTemp(BoundaryMeshCombine));
		UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(MeshCombineOut, FillFanMeshCombine, FTransform::Identity);
		return MeshCombineOut;
	}
	
	return FixMesh;
}



UDynamicMesh* UGeometryGenerate::CollectMeshsMultThread(UDynamicMesh* TargetMesh, TArray<UStaticMesh*> BoundTransformMapKeyArray, TArray<TArray<FTransform>> BoundTransformMapValueArray, FBox Bounds, float MeshExtrude, float VoxelSize)
{
	TArray<UDynamicMesh*> DynamicMeshes;
	DynamicMeshes.SetNum(BoundTransformMapKeyArray.Num());
	TArray<TTuple<int32, FTransform>> DynamicMeshTransforms;
	for (int32 i = 0; i < BoundTransformMapValueArray.Num(); i++)
	{
		TArray<FTransform> Transforms = BoundTransformMapValueArray[i];
		for (int32 j = 0; j < Transforms.Num(); j++)
		{
			TTuple<int32, FTransform> TransformTuple;
			TransformTuple.Key = i;
			TransformTuple.Value = Transforms[j];
			DynamicMeshTransforms.Add(TransformTuple);
		}
	}
	TArray<TTuple<int32, UDynamicMesh*>> MeshConvertTuples = ProcessAsync::ProcessAsync<TTuple<int32, UDynamicMesh*>>(
	BoundTransformMapKeyArray.Num(), 1, [&](const int32 i)
	{
		UDynamicMesh* DynamicMesh = NewObject<UDynamicMesh>();
		DynamicMesh->Reset();

		UStaticMesh* StaticMesh = BoundTransformMapKeyArray[i];
		FGeometryScriptCopyMeshFromAssetOptions AssetOptions;
		FGeometryScriptMeshReadLOD RequestedLOD;
		RequestedLOD.LODIndex = FMath::Min(StaticMesh->GetNumLODs() - 1, 3);
		RequestedLOD.LODType = EGeometryScriptLODType::RenderData;
		EGeometryScriptOutcomePins Outcome;
		UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMesh(
			StaticMesh, DynamicMesh, AssetOptions, RequestedLOD, Outcome);
		TTuple<int32, UDynamicMesh*> DynamicMeshTuple;
		DynamicMeshTuple.Key = i;
		DynamicMeshTuple.Value = DynamicMesh;
		return DynamicMeshTuple;
	});
	for (TTuple<int32, UDynamicMesh*> DynamicMeshTuple : MeshConvertTuples)
	{
		DynamicMeshes[DynamicMeshTuple.Key] = DynamicMeshTuple.Value;
	}
	
	bool UsePointVDB = false;
	if (UsePointVDB)
	{
		UDynamicMesh* CombineMeshs = NewObject<UDynamicMesh>();
		TArray<UDynamicMesh*> Meshs = ProcessAsync::ProcessAsync<UDynamicMesh*>(
		DynamicMeshTransforms.Num(), 1, [&](const int32 i)
		{
			TArray<FVector> SamplePointsPerMesh;
			FTransform Transform = DynamicMeshTransforms[i].Value;
			UDynamicMesh* DynamicMesh = DynamicMeshes[DynamicMeshTransforms[i].Key];
			UDynamicMesh* PerMesh = NewObject<UDynamicMesh>();
			PerMesh->Reset();
			FDynamicMesh3 MeshCopy;
			DynamicMesh->ProcessMesh([&](const FDynamicMesh3& EditMesh)
			{
				MeshCopy = EditMesh;
			});
			PerMesh->SetMesh(MoveTemp(MeshCopy));
			PerMesh->EditMesh([&](FDynamicMesh3& EditMesh)
			{
				MeshTransforms::ApplyTransform(EditMesh, (FTransformSRT3d)Transform, true);
			}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);

			PerMesh->EditMesh([&](FDynamicMesh3& EditMesh)
			{
				int32 TriCount = EditMesh.TriangleCount();
				for (int32 i = 0; i < TriCount; i++)
				{
					FIndex3i VertexIndexs = EditMesh.GetTriangle(i);
					bool IsOutSideTriangle = true;
					TArray<FVector> VertexPositions;
					VertexPositions.Reserve(3);
					for (int32 j = 0; j < 3; j++)
					{
						FVector Vertex = EditMesh.GetVertex(VertexIndexs[j]);
						VertexPositions.Add(Vertex);
					}
					FBox TriBox(VertexPositions);
					if (!Bounds.Intersect(TriBox))
					{
						EditMesh.RemoveTriangle(i);
					}
				}
			}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
			

			return PerMesh;
		});
		for (UDynamicMesh* Mesh : Meshs)
		{
			if (!Mesh || Mesh->GetTriangleCount() == 0)
				continue;
			
			UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(CombineMeshs, Mesh, FTransform::Identity);
		}
		FGeometryScriptDynamicMeshBVH BVH;
		UGeometryScriptLibrary_MeshSpatial::BuildBVHForMesh(CombineMeshs, BVH, nullptr);
		
		TArray<FTransform> Samples;
		TArray<double> SampleRadii;
		FGeometryScriptIndexList TriangleIDs;
		FGeometryScriptMeshPointSamplingOptions Options;
		Options.SamplingRadius = VoxelSize * .4;
		FGeometryScriptNonUniformPointSamplingOptions NonUniformOptions;
		UGeometryScriptLibrary_MeshSamplingFunctions::ComputeNonUniformPointSampling(CombineMeshs, Options, NonUniformOptions, Samples, SampleRadii, TriangleIDs);
		
		TArray<FVector> SamplePoints;
		SamplePoints.Reserve(Samples.Num());
		for (int32 i = 0; i < Samples.Num(); i++)
		{
			FVector Location = Samples[i].GetLocation() - Samples[i].GetRotation().GetUpVector() * VoxelSize / 0.7;
			SamplePoints.Add(Location);
		}


		FDynamicMesh3 MeshCopy;
		CombineMeshs->ProcessMesh([&](const FDynamicMesh3& EditMesh)
		{
			MeshCopy = EditMesh;
		});
		TargetMesh->SetMesh(MoveTemp(MeshCopy));

		TargetMesh = UVDBExtra::ParticlesToVDBMeshUniform(TargetMesh, SamplePoints, 2, VoxelSize);
		// TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh)
		// {
		// 	for (int i = 0; i < EditMesh.VertexCount(); i++)
		// 	{
		// 		if (!EditMesh.IsVertex(i))
		// 			continue;
		// 					
		// 		FVector Vertex = EditMesh.GetVertex(i);
		// 		FVector NearLocation = FVector::ZeroVector;
		// 		FVector VertexNormal = FVector::ZeroVector;
		// 		FGeometryScriptSpatialQueryOptions NearPointOptions;
		// 		FGeometryScriptTrianglePoint NearestPoint;
		// 		EGeometryScriptSearchOutcomePins Outcome;
		// 		UGeometryScriptLibrary_MeshSpatial::FindNearestPointOnMesh(
		// 			CombineMeshs, BVH, Vertex, NearPointOptions, NearestPoint, Outcome, nullptr);
		// 		NearLocation = NearestPoint.Position;
		// 		VertexNormal = EditMesh.GetTriNormal(NearestPoint.TriangleID);
		// 		FVector Dir = Vertex - NearLocation;
		// 		Dir.Normalize();
		// 		if (FVector::DotProduct(VertexNormal, Dir) > 0)
		// 			EditMesh.SetVertex(i, NearLocation);
		// 	}
		// }, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
		
		return TargetMesh;
	}
	else
	{
		TArray<UDynamicMesh*> Meshs = ProcessAsync::ProcessAsync<UDynamicMesh*>(
		DynamicMeshTransforms.Num(), 1, [&](const int32 i)
		{
			FTransform Transform = DynamicMeshTransforms[i].Value;
			UDynamicMesh* DynamicMesh = DynamicMeshes[DynamicMeshTransforms[i].Key];
			UDynamicMesh* PerMesh = NewObject<UDynamicMesh>();
			PerMesh->Reset();
			FDynamicMesh3 MeshCopy;
			DynamicMesh->ProcessMesh([&](const FDynamicMesh3& EditMesh)
			{
				MeshCopy = EditMesh;
			});
			PerMesh->SetMesh(MoveTemp(MeshCopy));
			PerMesh->EditMesh([&](FDynamicMesh3& EditMesh)
			{
				MeshTransforms::ApplyTransform(EditMesh, (FTransformSRT3d)Transform, true);
			}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);

			PerMesh->EditMesh([&](FDynamicMesh3& EditMesh)
			{
				int32 TriCount = EditMesh.TriangleCount();
				for (int32 i = 0; i < TriCount; i++)
				{
					FIndex3i VertexIndexs = EditMesh.GetTriangle(i);
					bool IsOutSideTriangle = true;
					TArray<FVector> VertexPositions;
					VertexPositions.Reserve(3);
					for (int32 j = 0; j < 3; j++)
					{
						FVector Vertex = EditMesh.GetVertex(VertexIndexs[j]);
						VertexPositions.Add(Vertex);
					}
					FBox TriBox(VertexPositions);
					if (!Bounds.Intersect(TriBox))
					{
						EditMesh.RemoveTriangle(i);
					}
				}
			}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
				
			//BlurNormals 
			UGeometryGeneral::BlurVertexNormals(PerMesh);
				
			//CreateBoundaryMesh
			UGeometryGenerate::ExtrudeUnclosedBoundary(PerMesh, MeshExtrude);
			return PerMesh;
		});
		for (UDynamicMesh* Mesh : Meshs)
		{
			if (!Mesh || Mesh->GetTriangleCount() == 0)
				continue;

			UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(TargetMesh, Mesh, FTransform::Identity);
		}
		return TargetMesh;
	}
}



UDynamicMesh* UGeometryGenerate::CollectMeshs(UDynamicMesh* TargetMesh, TArray<UStaticMesh*> BoundTransformMapKeyArray,
                                              TArray<TArray<FTransform>> BoundTransformMapValueArray, FBox Bounds, float MeshExtrude)
{
	for (int32 i = 0; i < BoundTransformMapKeyArray.Num(); i++)
	{
		
		UDynamicMesh* DynamicMesh = NewObject<UDynamicMesh>();
		UStaticMesh* StaticMesh = BoundTransformMapKeyArray[i];
		TArray<FTransform> Transforms = BoundTransformMapValueArray[i];
		FGeometryScriptCopyMeshFromAssetOptions AssetOptions;
		FGeometryScriptMeshReadLOD RequestedLOD;
		RequestedLOD.LODIndex = FMath::Min(StaticMesh->GetNumLODs() - 1, 3);
		RequestedLOD.LODType = EGeometryScriptLODType::RenderData;
		EGeometryScriptOutcomePins Outcome;
		UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMesh(
			StaticMesh, DynamicMesh, AssetOptions, RequestedLOD, Outcome);

		for (int32 j = 0; j < Transforms.Num(); j++)
		{
			UDynamicMesh* PerMesh = NewObject<UDynamicMesh>();
			PerMesh->Reset();
			FDynamicMesh3 MeshCopy;
			DynamicMesh->ProcessMesh([&](const FDynamicMesh3& EditMesh)
			{
				MeshCopy = EditMesh;
			});
			PerMesh->SetMesh(MoveTemp(MeshCopy));
			PerMesh->EditMesh([&](FDynamicMesh3& EditMesh)
			{
				MeshTransforms::ApplyTransform(EditMesh, (FTransformSRT3d)Transforms[j], true);
			}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);

			PerMesh->EditMesh([&](FDynamicMesh3& EditMesh)
			{
				int32 TriCount = EditMesh.TriangleCount();
				for (int32 i = 0; i < TriCount; i++)
				{
					FIndex3i VertexIndexs = EditMesh.GetTriangle(i);
					bool IsOutSideTriangle = true;
					TArray<FVector> VertexPositions;
					VertexPositions.Reserve(3);
					for (int32 j = 0; j < 3; j++)
					{
						FVector Vertex = EditMesh.GetVertex(VertexIndexs[j]);
						VertexPositions.Add(Vertex);
					}
					FBox TriBox(VertexPositions);
					if (!Bounds.Intersect(TriBox))
					{
						EditMesh.RemoveTriangle(i);
					}
				}
			}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
			//BlurNormals 
			UGeometryGeneral::BlurVertexNormals(PerMesh);
			//CreateBoundaryMesh
			UGeometryGenerate::ExtrudeUnclosedBoundary(PerMesh, MeshExtrude);
			UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(TargetMesh, PerMesh, FTransform::Identity);
		}
	}
	return TargetMesh;
}

UDynamicMesh* UGeometryGenerate::FixUnclosedBoundary(UDynamicMesh* FixMesh, float ProjectOffset, bool ProjectToLandscape, bool AppendMesh)
{
	FGeometryScriptMeshSelection Selection;
	UGeometryScriptLibrary_MeshSelectionFunctions::CreateSelectAllMeshSelection(FixMesh, Selection, EGeometryScriptMeshSelectionType::Triangles);

	TArray<FGeometryScriptIndexList> IndexLoops;
	TArray<FGeometryScriptPolyPath> PathLoops;
	int32 NumLoops = 0;
	bool bFoundErrors = false;
	UGeometryScriptLibrary_MeshSelectionQueryFunctions::GetMeshSelectionBoundaryLoops(FixMesh, Selection,IndexLoops, PathLoops, NumLoops, bFoundErrors, nullptr);
	
	FGeometryScriptPrimitiveOptions PrimitiveOptions;
	FDynamicMesh3 BoundaryMeshCombine;
	for (FGeometryScriptPolyPath Pathloop : PathLoops)
	{
		UDynamicMesh* BoundaryMesh = NewObject<UDynamicMesh>();
		TArray<FVector> Vertices = *Pathloop.Path;
		TArray<FTransform> PathTransforms;
		PathTransforms.Reserve(Vertices.Num());
		TArray<float> PathTexParamV;
		PathTexParamV.Reserve(Vertices.Num());
		for (FVector Vertece : Vertices)
		{
			FTransform Transform(Vertece);
			PathTransforms.Add(Transform);
			PathTexParamV.Add(0);
		}
		PathTexParamV.Add(0);
		TArray<FVector2D> PolylineVertices;
		PolylineVertices.Add(FVector2D(0, 0));
		PolylineVertices.Add(FVector2D(0, 0));
		TArray<float> PolylineTexParamU;
		PolylineTexParamU.Add(0);
		PolylineTexParamU.Add(1);
		
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSweepPolyline(
			BoundaryMesh, PrimitiveOptions, FTransform::Identity, PolylineVertices, PathTransforms, PolylineTexParamU,
			PathTexParamV, true, 1, 1, 0);
		FixMesh->EditMesh([&](FDynamicMesh3& FixEditMesh)
		{
			BoundaryMesh->EditMesh([&](FDynamicMesh3& EditMesh)
			{
				int32 VertexCount = EditMesh.VertexCount() / 2;
				for (int i = 0; i < VertexCount; i++)
				{
					int32 TarId = i * 2 + 1;
					if (EditMesh.IsVertex(TarId))
					{
						FVector BoundaryLocation = EditMesh.GetVertex(TarId);
						BoundaryLocation.Z -= ProjectOffset;
						FVector FixedLocation = BoundaryLocation;
						
						if (ProjectToLandscape)
						{
							FVector ProjectLoaction = FVector::ZeroVector;
							FVector ProjectNormal = FVector::ZeroVector;
							if (ULandscapeExtra::ProjectPoint(BoundaryLocation, ProjectLoaction, ProjectNormal))
							{
								FixedLocation = ProjectLoaction;
							}
						}

						if (BoundaryLocation.Z < FixedLocation.Z)
						{
							EditMesh.SetVertex(TarId, BoundaryLocation);
							continue;
						}
						EditMesh.SetVertex(TarId, FixedLocation);
						
					}
				}
				
				FMeshIndexMappings TmpMappings;
				FDynamicMeshEditor Editor = nullptr;
				if (AppendMesh)
				{
					Editor = FDynamicMeshEditor(&FixEditMesh);
				}
				else
				{
					Editor = FDynamicMeshEditor(&BoundaryMeshCombine);
				}

				FTransform XForm = FTransform::Identity;
				
				Editor.AppendMesh(&EditMesh, TmpMappings,
				[&](int, const FVector3d& Position) { return XForm.TransformPosition(Position); });
				

			}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);

		}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	}
	if (!AppendMesh)
	{
		UDynamicMesh* BoundaryMeshCombineOut = NewObject<UDynamicMesh>();
		BoundaryMeshCombineOut->SetMesh(MoveTemp(BoundaryMeshCombine));
		return BoundaryMeshCombineOut;
	}
	
	return FixMesh;
}

UDynamicMesh* UGeometryGenerate::VoxelMergeMeshs(UDynamicMesh* TargetMesh, float VoxelSize)
{
	FProgressCancel *Progress = nullptr;
	struct FVoxelBoolInterrupter : IVoxelBasedCSG::FInterrupter
	{
		FVoxelBoolInterrupter(FProgressCancel* ProgressCancel) : Progress(ProgressCancel) {}
		FProgressCancel* Progress;
		virtual ~FVoxelBoolInterrupter() {}
		virtual bool wasInterrupted(int percent = -1) override final
		{
			bool Cancelled = Progress && Progress->Cancelled();
			return Cancelled;
		}

	} Interrupter(Progress);


	TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh)
	{
	// 	float Size = 0.f;
	// 	auto GrowSize = [&Size, VoxelCount](const FDynamicMesh3& DynamicMesh)
	// 	{
	// 		FAxisAlignedBox3d AABB = DynamicMesh.GetBounds(true);
	// 		FVector Scale = FVector::OneVector;
	// 		FVector Extents = 2. * AABB.Extents();
	// 		// Scale with the local space scale.
	// 		Extents.X = Extents.X * FMath::Abs(Scale.X);
	// 		Extents.Y = Extents.Y * FMath::Abs(Scale.Y);
	// 		Extents.Z = Extents.Z * FMath::Abs(Scale.Z);
	// 		
	// 		float MajorAxisSize = FMath::Max3(Extents.X, Extents.Y, Extents.Z);
	// 		Size = FMath::Max(MajorAxisSize / VoxelCount, Size);
	// 	};
	//
	// 	GrowSize(EditMesh);
	//
	// 	if (Size == 0)
	// 	{
	// 		return;
	// 	}
	//	Size = 10;
		TUniquePtr<IVoxelBasedCSG> VoxelCSGTool = IVoxelBasedCSG::CreateCSGTool(VoxelSize);

		FMeshDescription MeshDescription;
		FStaticMeshAttributes StaticMeshAttributes(MeshDescription);
		StaticMeshAttributes.Register();
		FConversionToMeshDescriptionOptions ToMeshDescriptionOptions;
		ToMeshDescriptionOptions.bSetPolyGroups = false;
		FDynamicMeshToMeshDescription DynamicMeshToMeshDescription(ToMeshDescriptionOptions);
		DynamicMeshToMeshDescription.Convert(&EditMesh, MeshDescription);
		TArray<IVoxelBasedCSG::FPlacedMesh> PlacedMeshs;
		IVoxelBasedCSG::FPlacedMesh PlacedMesh(&MeshDescription, FTransform::Identity);
		PlacedMeshs.Add(PlacedMesh);
		
		const double MaxIsoOffset = 2 * VoxelSize;
		const double CSGIsoSurface = FMath::Clamp(0, 0., MaxIsoOffset); // the interior distance values maybe messed up when doing a union.
		FVector MergedOrigin;
		FMeshDescription MergedMeshesDescription;

		
		bool bSuccess = VoxelCSGTool->ComputeUnion(Interrupter, PlacedMeshs, MergedMeshesDescription, MergedOrigin, 0.001, 0);
		//MergedMeshesDescription.Vertices().Num();

		FDynamicMesh3 ConvertlMesh;
		FMeshDescriptionToDynamicMesh Converter;
		Converter.Convert(&MergedMeshesDescription, ConvertlMesh);
		
		EditMesh.Copy(ConvertlMesh);
		
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	
	return TargetMesh;
}



UDynamicMesh* UGeometryGenerate::VDBMeshFromSurfaceVoxels(TArray<AActor*> In_Actors, TArray<FVector> ValidPositions,
                                                          float VoxelSize, float SurfaceDistance, float PointRadiusMult, bool bProjectToSurface, float InclusionDistance)
{
	FBox Bounds(ValidPositions);
	if (!Bounds.IsValid)
	{
		return NewObject<UDynamicMesh>();
	}

	const float MaxDist = SurfaceDistance > 0 ? SurfaceDistance : VoxelSize;
	const double MaxDistSq = (double)MaxDist * (double)MaxDist;
	const float InclusionDistSq = InclusionDistance * InclusionDistance;

	auto IsCloseToValidPositions = [&](const FBox& MeshBounds) -> bool
	{
		for (const FVector& Pos : ValidPositions)
		{
			if (MeshBounds.ComputeSquaredDistanceToPoint(Pos) <= InclusionDistSq)
			{
				return true;
			}
		}
		return false;
	};

	TMap<UStaticMesh*, TArray<FTransform>> MeshTransformMap;
	for (AActor* Actor : In_Actors)
	{
		if (!Actor)
		{
			continue;
		}

		TArray<UStaticMeshComponent*> Components;
		Actor->GetComponents(Components);
		for (UStaticMeshComponent* Comp : Components)
		{
			if (!Comp || !Comp->GetStaticMesh())
			{
				continue;
			}

			UStaticMesh* SM = Comp->GetStaticMesh();
			if (UInstancedStaticMeshComponent* ISM = Cast<UInstancedStaticMeshComponent>(Comp))
			{
				for (int32 i = 0; i < ISM->GetInstanceCount(); i++)
				{
					FTransform InstanceTransform;
					ISM->GetInstanceTransform(i, InstanceTransform, true);
					FBox MeshBounds = SM->GetBoundingBox().TransformBy(InstanceTransform);
					if (IsCloseToValidPositions(MeshBounds))
					{
						MeshTransformMap.FindOrAdd(SM).Add(InstanceTransform);
					}
				}
			}
			else
			{
				FTransform WorldTransform = Comp->GetComponentTransform();
				FBox MeshBounds = SM->GetBoundingBox().TransformBy(WorldTransform);
				if (IsCloseToValidPositions(MeshBounds))
				{
					MeshTransformMap.FindOrAdd(SM).Add(WorldTransform);
				}
			}
		}
	}

	FDynamicMesh3 CombinedMesh;
	for (auto& Pair : MeshTransformMap)
	{
		UStaticMesh* SM = Pair.Key;
		const TArray<FTransform>& Transforms = Pair.Value;

		UDynamicMesh* TempMesh = NewObject<UDynamicMesh>();
		FGeometryScriptCopyMeshFromAssetOptions AssetOptions;
		FGeometryScriptMeshReadLOD RequestedLOD;
		RequestedLOD.LODIndex = 0;
		RequestedLOD.LODType = EGeometryScriptLODType::RenderData;
		EGeometryScriptOutcomePins Outcome;
		UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMesh(SM, TempMesh, AssetOptions, RequestedLOD, Outcome);

		for (const FTransform& Transform : Transforms)
		{
			FDynamicMesh3 MeshCopy;
			TempMesh->ProcessMesh([&](const FDynamicMesh3& ReadMesh)
			{
				MeshCopy = ReadMesh;
			});
			MeshTransforms::ApplyTransform(MeshCopy, (FTransformSRT3d)Transform, true);

			TArray<int32> TrianglesToRemove;
			for (int32 TID : MeshCopy.TriangleIndicesItr())
			{
				FIndex3i Tri = MeshCopy.GetTriangle(TID);
				FVector V0 = FVector(MeshCopy.GetVertex(Tri[0]));
				FVector V1 = FVector(MeshCopy.GetVertex(Tri[1]));
				FVector V2 = FVector(MeshCopy.GetVertex(Tri[2]));

				FBox TriBox(ForceInit);
				TriBox += V0;
				TriBox += V1;
				TriBox += V2;

				bool bKeep = false;
				for (const FVector& Pos : ValidPositions)
				{
					if (TriBox.ComputeSquaredDistanceToPoint(Pos) <= InclusionDistSq)
					{
						bKeep = true;
						break;
					}
				}
				if (!bKeep)
				{
					TrianglesToRemove.Add(TID);
				}
			}

			for (int32 TID : TrianglesToRemove)
			{
				MeshCopy.RemoveTriangle(TID);
			}

			if (MeshCopy.TriangleCount() > 0)
			{
				FDynamicMeshEditor Editor(&CombinedMesh);
				FMeshIndexMappings Mappings;
				Editor.AppendMesh(&MeshCopy, Mappings);
			}
		}
	}

	if (CombinedMesh.TriangleCount() == 0)
	{
		return NewObject<UDynamicMesh>();
	}

	TMeshAABBTree3<FDynamicMesh3> Spatial(&CombinedMesh, true);

	const int32 NX = FMath::Max(1, FMath::CeilToInt((Bounds.Max.X - Bounds.Min.X) / VoxelSize));
	const int32 NY = FMath::Max(1, FMath::CeilToInt((Bounds.Max.Y - Bounds.Min.Y) / VoxelSize));
	const int32 NZ = FMath::Max(1, FMath::CeilToInt((Bounds.Max.Z - Bounds.Min.Z) / VoxelSize));

	FCriticalSection Mutex;
	TArray<FVector> SurfaceVoxels;

	ParallelFor(NX, [&](int32 IX)
	{
		TArray<FVector> LocalPoints;
		for (int32 IY = 0; IY < NY; IY++)
		{
			for (int32 IZ = 0; IZ < NZ; IZ++)
			{
				FVector3d Center(
					Bounds.Min.X + (IX + 0.5) * VoxelSize,
					Bounds.Min.Y + (IY + 0.5) * VoxelSize,
					Bounds.Min.Z + (IZ + 0.5) * VoxelSize);

				double NearDistSq = TNumericLimits<double>::Max();
				IMeshSpatial::FQueryOptions QueryOptions;
				QueryOptions.MaxDistance = MaxDist;
				int32 NearTri = Spatial.FindNearestTriangle(Center, NearDistSq, QueryOptions);
				if (NearTri >= 0 && NearDistSq <= MaxDistSq)
				{
					LocalPoints.Add(FVector(Center));
				}
			}
		}

		if (LocalPoints.Num() > 0)
		{
			FScopeLock Lock(&Mutex);
			SurfaceVoxels.Append(LocalPoints);
		}
	});

	UDynamicMesh* OutMesh = NewObject<UDynamicMesh>();
	if (SurfaceVoxels.Num() > 0)
	{
		UVDBExtra::ParticlesToVDBMeshUniform(OutMesh, SurfaceVoxels, PointRadiusMult, VoxelSize);
	}

	if (bProjectToSurface && OutMesh->GetTriangleCount() > 0)
	{
		OutMesh->EditMesh([&](FDynamicMesh3& EditMesh)
		{
			ParallelFor(EditMesh.MaxVertexID(), [&](int32 VID)
			{
				if (!EditMesh.IsVertex(VID))
				{
					return;
				}

				FVector3d Vertex = EditMesh.GetVertex(VID);
				double NearDistSq = TNumericLimits<double>::Max();
				int32 NearTri = Spatial.FindNearestTriangle(Vertex, NearDistSq);
				if (NearTri >= 0)
				{
					FIndex3i Tri = CombinedMesh.GetTriangle(NearTri);
					FVector3d V0 = CombinedMesh.GetVertex(Tri[0]);
					FVector3d V1 = CombinedMesh.GetVertex(Tri[1]);
					FVector3d V2 = CombinedMesh.GetVertex(Tri[2]);
					FVector NearestPoint = FMath::ClosestPointOnTriangleToPoint(FVector(Vertex), FVector(V0), FVector(V1), FVector(V2));
					FVector3d TriNormal = VectorUtil::Normal(V0, V1, V2);
					FVector3d Dir = Vertex - FVector3d(NearestPoint);
					if (Dir.SquaredLength() < KINDA_SMALL_NUMBER || Dir.Dot(TriNormal) > 0)
					{
						EditMesh.SetVertex(VID, FVector3d(NearestPoint));
					}
				}
			});
		}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);

		FGeometryScriptCalculateNormalsOptions NormalOptions;
		UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(OutMesh, NormalOptions);
	}

	return OutMesh;
}

FVector UGeometryGenerate::TestViewPosition()
{
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
	//
	//
	return FVector::ZeroVector;
}

UDynamicMesh* UGeometryGenerate::SurfaceVoxelsToVDBMesh(AComputeShaderMeshGenerator* Generator,
	float VoxelSize,
	float RadiusMult,
	bool bRecomputeNormals)
{
	return Generator ? Generator->SurfaceVoxelsToVDBMesh(VoxelSize, RadiusMult, bRecomputeNormals) : nullptr;
}

UDynamicMesh* UGeometryGenerate::VDBVoxelsToOpenDynamicMesh(FCSSurfaceVoxelData SurfaceVoxels,
	float VoxelSize,
	float RadiusMult,
	bool bRecomputeNormals)
{
	const int32 EffectiveVoxelCount = SurfaceVoxels.VoxelCount >= 0
		? FMath::Clamp(SurfaceVoxels.VoxelCount, 0, SurfaceVoxels.Positions.Num())
		: SurfaceVoxels.Positions.Num();
	if (EffectiveVoxelCount <= 0)
	{
		return NewObject<UDynamicMesh>();
	}

	const float SafeVoxelSize = FMath::Max(VoxelSize > 0.0f ? VoxelSize : SurfaceVoxels.VoxelSize, UE_KINDA_SMALL_NUMBER);

	// 收集有效的世界空间位置
	TArray<FVector> WorldPositions;
	WorldPositions.Reserve(EffectiveVoxelCount);
	for (int32 Index = 0; Index < EffectiveVoxelCount; ++Index)
	{
		const FVector& Position = SurfaceVoxels.Positions[Index];
		if (Position.ContainsNaN() || !FMath::IsFinite(Position.X) || !FMath::IsFinite(Position.Y) || !FMath::IsFinite(Position.Z))
		{
			continue;
		}
		WorldPositions.Add(Position);
	}

	if (WorldPositions.IsEmpty())
	{
		return NewObject<UDynamicMesh>();
	}

	// 使用 VDB ParticlesToLevelSet 转 mesh，输入已经是世界空间坐标
	UDynamicMesh* OutMesh = NewObject<UDynamicMesh>();
	UVDBExtra::ParticlesToVDBMeshUniform(OutMesh, WorldPositions, RadiusMult, SafeVoxelSize, false);

	if (bRecomputeNormals)
	{
		FGeometryScriptCalculateNormalsOptions CalculateOptions;
		UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(OutMesh, CalculateOptions);
	}

	return OutMesh;
}
