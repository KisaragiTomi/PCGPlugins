// Fill out your copyright notice in the Description page of Project Settings.
#include "GeometryEditorActor.h"

#include "EngineUtils.h"
#include "PointFunction.h"
#include "GeometryScript/MeshBasicEditFunctions.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "Spatial/MeshAABBTree3.h"



AVineContainer::AVineContainer(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)	
{
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	TubeFoliageInstanceContainer = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("TubePoints"));
	TubeFoliageInstanceContainer->SetStaticMesh(Mesh);
	TubeFoliageInstanceContainer->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Overlap);
	TubeFoliageInstanceContainer->SetCollisionObjectType(ECollisionChannel::ECC_WorldDynamic);
	TubeFoliageInstanceContainer->SetVisibility(true, false);
	TubeFoliageInstanceContainer->SetHiddenInGame(true);
	TubeFoliageInstanceContainer->SetupAttachment(GetRootComponent(), TEXT("TubePoints"));
	
	PlaneFoliageInstanceContainer = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("PlanePoints"));
	PlaneFoliageInstanceContainer->SetStaticMesh(Mesh);
	PlaneFoliageInstanceContainer->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Overlap);
	PlaneFoliageInstanceContainer->SetCollisionObjectType(ECollisionChannel::ECC_WorldDynamic);
	PlaneFoliageInstanceContainer->SetVisibility(true, false);
	PlaneFoliageInstanceContainer->SetHiddenInGame(true);
	PlaneFoliageInstanceContainer->SetupAttachment(GetRootComponent(), TEXT("PlanePoints"));
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
	UFoliageConverter::GetAllInstancesTransfrom(InFoliageType, Transforms);
	FString FoliageTypeName = InFoliageType->GetName();
	if (FoliageTypeName == TEXT("SMF_TubeVine_FoliageType"))
	{
		TubeFoliageInstanceContainer->AddInstances(Transforms, false, true, false);

	}
	if (FoliageTypeName == TEXT("SMF_PlaneVine_FoliageType"))
	{
		PlaneFoliageInstanceContainer->AddInstances(Transforms, false, true, false);

	}
	if (FoliageTypeName == TEXT("SMF_Target_FoliageType"))
	{
		FoliageInstanceContainer->AddInstances(Transforms, false, true, false);
	}
	for (TActorIterator<AInstancedFoliageActor> It(GWorld); It; ++It)
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
		InstanceContainerTemp = TubeFoliageInstanceContainer;
	
	if ( FoliageTypeName == TEXT("SMF_PlaneVine_FoliageType"))
		InstanceContainerTemp = PlaneFoliageInstanceContainer;
	
	if ( FoliageTypeName == TEXT("SMF_Target_FoliageType"))
		InstanceContainerTemp = FoliageInstanceContainer;

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
	UFoliageConverter::RefreshFoliage(InFoliageType);
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
				EGeometryScriptSearchOutcomePins Outcome;
				UGeometryScriptLibrary_MeshSpatial::FindNearestPointOnMesh(
				PrefixMesh, BVH, VertexLocation, Options, NearestPoint, Outcome, nullptr);
				VertexLocation = NearestPoint.Position;
				
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
				EGeometryScriptSearchOutcomePins Outcome;
				UGeometryScriptLibrary_MeshSpatial::FindNearestPointOnMesh(
				PrefixMesh, BVH, VertexLocation, Options, NearestPoint, Outcome, nullptr);
				VertexLocation = NearestPoint.Position;
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
			EGeometryScriptSearchOutcomePins Outcome;
			UGeometryScriptLibrary_MeshSpatial::FindNearestPointOnMesh(
			PrefixMesh, BVH, VertexLocation, Options, NearestPoint, Outcome, nullptr);
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
			EGeometryScriptSearchOutcomePins Outcome;
			UGeometryScriptLibrary_MeshSpatial::FindNearestPointOnMesh(
			PrefixMesh, BVH, VertexLocation, Options, NearestPoint, Outcome, nullptr);

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

inline void AVineContainer::Clean()
{
	TubeLines.Empty();
	PlaneLines.Empty();
	DynamicMeshComponent->GetDynamicMesh()->Reset();
}

void AVineContainer::AddInstance(UFoliageType* InFoliageType)
{
	AddInstanceFromFoliageType(InFoliageType);
}
