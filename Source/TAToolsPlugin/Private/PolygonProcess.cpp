// Fill out your copyright notice in the Description page of Project Settings.


#include "PolygonProcess.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "Engine/StaticMeshActor.h"
#include "ToolBuilderUtil.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "CoreMinimal.h"


#include "MinVolumeSphere3.h"
#include "MinVolumeBox3.h"
#include "FitCapsule3.h"
#include "MeshDescription.h"


using namespace UE::Geometry;



void UPolygonProcess::CalculateRoatation(TArray<UStaticMesh*> StaticMeshs, bool DebugBox)
{

}
void UPolygonProcess::GetHoleVerticesPosition(UStaticMesh* InMesh, int32 LODIndex, int32 SectionIndex, TArray<FVector>& InVertices)
{
	 //create mesh to operate on
	 TSharedPtr<FDynamicMesh3> OriginalMesh = MakeShared<FDynamicMesh3>();
	  FMeshDescriptionToDynamicMesh Converter;
	  Converter.Convert(InMesh->GetMeshDescription(LODIndex), *OriginalMesh);
	 
	  //initialize topology
	  TUniquePtr<FBasicTopologyFindPosition> Topology = MakeUnique<FBasicTopologyFindPosition>(OriginalMesh.Get(), false);
	  Topology->GetVerticesPosition(InVertices);
}

void UPolygonProcess::FixOpenAssetAsync(TArray<UStaticMesh*> StaticMeshs, int32 NumberOfCheck, bool DebugBox, bool OpenVertexDir, bool MultiThread)
{
	ATransformManager* TransformManager = nullptr;
	TArray<AActor*> FindOutActors;
	UGameplayStatics::GetAllActorsOfClass(GWorld, ATransformManager::StaticClass(), FindOutActors);
	if (FindOutActors.Num() == 0)
	{
		FActorSpawnParameters Params;
		TransformManager = GWorld->SpawnActor<ATransformManager>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
	}
	else
	{
		TransformManager = Cast<ATransformManager>(FindOutActors[0]);
	}
	TArray<FVector> OpenVertexs;

	TMap<UStaticMesh*, TArray<FVector>> MeshOpenVertex;
	MeshOpenVertex = TransformManager->MeshOpenVertex;
	for (UStaticMesh* StaticMesh : StaticMeshs)
	{
		if(!StaticMesh)
		{
			continue;
		}
		TArray<FVector> Vertices;
		TSharedPtr<FDynamicMesh3> OriginalMesh = MakeShared<FDynamicMesh3>();
		FMeshDescriptionToDynamicMesh Converter;
		Converter.Convert(StaticMesh->GetMeshDescription(0), *OriginalMesh);

		//initialize topology
		TUniquePtr<FBasicTopologyFindPosition> Topology = MakeUnique<FBasicTopologyFindPosition>(OriginalMesh.Get(), false);
		Topology->GetVerticesPosition(Vertices);
		MeshOpenVertex.Add(StaticMesh, Vertices);
	}
	TransformManager->MeshOpenVertex = MeshOpenVertex;
	TArray<AActor*> OutActors;
	UGameplayStatics::GetAllActorsOfClass(GWorld, AStaticMeshActor::StaticClass(), OutActors);


	for (AActor* OutActor : OutActors)
	{
		AStaticMeshActor* StaticActor = Cast<AStaticMeshActor>(OutActor);
		UStaticMesh* StaticMesh = StaticActor->GetStaticMeshComponent()->GetStaticMesh();
		if (StaticMeshs.Find(StaticMesh) < 0)
		{
			continue;
		}
		FTransform Transform = StaticActor->GetActorTransform();
		TSharedPtr<OpenAssetProcess, ESPMode::ThreadSafe> OpenAsset = MakeShareable(new OpenAssetProcess());
		OpenAsset->FAsyncCalDelegate.AddThreadSafeSP(OpenAsset.ToSharedRef(), &OpenAssetProcess::CalculateResult);
		//由于某些情况, Actor是还未创建的, 所以不能直接从Actor获得StaticMesh
		OpenAsset->StaticActor = StaticActor;
		OpenAsset->StaticMesh = StaticMesh;
		OpenAsset->DebugBox = DebugBox;
		OpenAsset->TransformManager = TransformManager;
		OpenAsset->IgOutActors = OutActors;
		OpenAsset->Vertices = *MeshOpenVertex.Find(StaticActor->GetStaticMeshComponent()->GetStaticMesh());
		OpenAsset->NumberOfCheck = NumberOfCheck;
		OpenAsset->FixedTransform = Transform;
		if(OpenVertexDir)
		{
			OpenAsset->CalculateOpenVerticesDir();
		}
		if(MultiThread)
		{
			auto AsyncTask = new FAutoDeleteAsyncTask<FAsyncTasksTemplate<OpenAssetProcess>>(OpenAsset);
			AsyncTask->StartBackgroundTask();
		}
		else
		{
			OpenAsset->CalculateOpenAssetTransform();
			OpenAsset->CalculateSafeTransform();
			StaticActor->SetActorTransform(OpenAsset->FixedTransform);
			if (DebugBox)
			{
				float Duration = 5;
				UKismetSystemLibrary::DrawDebugBox(GWorld, OpenAsset->OutCenter, OpenAsset->OutExtent, FLinearColor(1, 0, 0, 0), OpenAsset->OutRotator, Duration, 11);
				UKismetSystemLibrary::DrawDebugArrow(GWorld, OpenAsset->OutCenter, OpenAsset->OutCenter + OpenAsset->BoxUpDir * 400, 5, FLinearColor(0, 0, 1, 0), Duration, 5);
				for (FVector DrawPos : OpenAsset->OrientSpaceVertices)
				{
					UKismetSystemLibrary::DrawDebugPoint(GWorld, DrawPos, 3, FLinearColor(0, 1, 0, 0), Duration);
				}
			}
		}
	}
}


void OpenAssetProcess::DoCalculate()
{
	FAsyncCalDelegate.Broadcast();
}

void OpenAssetProcess::CalculateResult()
{
	
	CalculateOpenAssetTransform();
	CalculateSafeTransform();
	
	//这是为了要把
	TransformManager->TransformMap.Add(StaticActor, FixedTransform);
	AsyncTask(ENamedThreads::GameThread, [&]()
	{

		TArray<AActor*> ManagerActors;
		UGameplayStatics::GetAllActorsOfClass(GWorld, ATransformManager::StaticClass(), ManagerActors);
		//ATransformManager* TransformManager = nullptr;
		//bool StopFixOpenAsset = true;
		if (ManagerActors.Num() > 0)
		{
			TransformManager = Cast<ATransformManager>(ManagerActors[0]);
			TArray<AActor*> StaticMeshActors;
			UGameplayStatics::GetAllActorsOfClass(GWorld, AStaticMeshActor::StaticClass(), StaticMeshActors);
			for (AActor* Actor : StaticMeshActors)
			{
				AStaticMeshActor* StaticMeshActor = Cast<AStaticMeshActor>(Actor);
				FTransform* Transform = TransformManager->TransformMap.Find(StaticMeshActor);
				if (Transform)
				{
					StaticMeshActor->SetActorTransform(*Transform);
				}
			}
		}
	});
}



void UPolygonProcess::StopFixOpenAssetAsync(TArray<UStaticMesh*> StaticMeshs, FVector Dir)
{
	TMap<UStaticMesh*, TArray<FVector>> MeshOpenVertex;
	for (UStaticMesh* StaticMesh : StaticMeshs)
	{
		if(!StaticMesh)
		{
			continue;
		}
		TArray<FVector> Vertices;
		TSharedPtr<FDynamicMesh3> OriginalMesh = MakeShared<FDynamicMesh3>();
		FMeshDescriptionToDynamicMesh Converter;
		Converter.Convert(StaticMesh->GetMeshDescription(0), *OriginalMesh);
		
		TUniquePtr<FBasicTopologyFindPosition> Topology = MakeUnique<FBasicTopologyFindPosition>(OriginalMesh.Get(), false);
		Topology->GetVerticesPosition(Vertices);
		MeshOpenVertex.Add(StaticMesh, Vertices);
	}
	TArray<AActor*> OutActors;
	UGameplayStatics::GetAllActorsOfClass(GWorld, AStaticMeshActor::StaticClass(), OutActors);
	FVector UpVector = FVector(0, 0, 1);
	for (AActor* OutActor : OutActors)
	{
		AStaticMeshActor* StaticActor = Cast<AStaticMeshActor>(OutActor);
		FTransform Transform = StaticActor->GetActorTransform();
		TArray<FVector> Vertices;
		if(!MeshOpenVertex.Find(StaticActor->GetStaticMeshComponent()->GetStaticMesh()))
		{
			continue;
		}
		TSharedPtr<OpenAssetProcess, ESPMode::ThreadSafe> OpenAsset = MakeShareable(new OpenAssetProcess());
		OpenAsset->IgOutActors = OutActors;
		OpenAsset->Vertices = *MeshOpenVertex.Find(StaticActor->GetStaticMeshComponent()->GetStaticMesh());;
		OpenAsset->StaticMesh = StaticActor->GetStaticMeshComponent()->GetStaticMesh();
		OpenAsset->FixedTransform = Transform;
		OpenAsset->CalculateSafeTransform();
		StaticActor->SetActorTransform(OpenAsset->FixedTransform);
	}
}


void OpenAssetProcess::CalculateEigenVector()
{
	FVector ObjectUpVector = Transform.GetRotation().GetUpVector();
	FOrientedBox3d Box;
	FMinVolumeBox3d MinBoxCalc;
	bool bMinBoxOK = MinBoxCalc.Solve(OrientSpaceVertices.Num(),
		[&](int32 Index) { return OrientSpaceVertices[Index]; });
	if (bMinBoxOK && MinBoxCalc.IsSolutionAvailable())
	{
		MinBoxCalc.GetResult(Box);
	}
	else
	{
		return;

	}
	FVector3d Extents = Box.Extents;
	FVector3d Center = Box.Frame.Origin;
	FQuaterniond Quat = Box.Frame.Rotation;
	OutCenter = FVector(Center.X, Center.Y, Center.Z);
	OutExtent = FVector(Extents.X, Extents.Y, Extents.Z);
	OutRotator = FQuat(Quat.X, Quat.Y, Quat.Z, Quat.W).Rotator();

	float DotObjectCheck = -1;
	float DotUpCheck = -1;
	float FAxisDot = 0;
	float UpDot = 0;
	FVector ObjectUpDir;
	FVector UpVectorUpDir;

	FVector3d Corner0 = Box.GetCorner(0);
	FVector3d Corner1 = Box.GetCorner(1);
	FVector3d Corner3 = Box.GetCorner(3);
	FVector3d Corner4 = Box.GetCorner(4);
	FVector3d Axis0 = Corner1 - Corner0;
	FVector3d Axis1 = Corner3 - Corner0;
	FVector3d Axis2 = Corner4 - Corner0;
	FVector3d BoxUpDird;

	if (Axis0.Length() < Axis1.Length())
	{
		BoxUpDird = Axis0;
	}
	else
	{
		BoxUpDird = Axis1;
	}
	if (Axis2.Length() < BoxUpDird.Length())
	{
		BoxUpDird = Axis2;
	}
	
	BoxUpDird = BoxUpDird.GetSafeNormal();
	float Dotd = BoxUpDird.Dot(ObjectUpVector);
	if (Dotd < 0)
	{
		BoxUpDird *= -1;
	}
	BoxUpDir = FVector(BoxUpDird.X, BoxUpDird.Y, BoxUpDird.Z);

	for (int32 i = 0; i < 3; i++)
	{
		FVector3d Axis = Box.GetAxis(i);
		FVector FAxis = FVector(Axis.X, Axis.Y, Axis.Z);

		FAxisDot = FVector::DotProduct(ObjectUpVector, FAxis);
		UpDot = FVector::DotProduct(UpVector, FAxis);
		if (FAxisDot > DotObjectCheck)
		{
			ObjectUpDir = FAxis;
			DotObjectCheck = FAxisDot;
		}
		if (UpDot > DotUpCheck)
		{
			UpVectorUpDir = FAxis;
			DotUpCheck = UpDot;
		}
		FAxisDot = FVector::DotProduct(ObjectUpVector, -FAxis);
		UpDot = FVector::DotProduct(UpVector, -FAxis);
		if (FAxisDot > DotObjectCheck)
		{
			ObjectUpDir = -FAxis;
			DotObjectCheck = FAxisDot;
		}
		if (UpDot > DotUpCheck)
		{
			UpVectorUpDir = FAxis;
			DotUpCheck = UpDot;
		}
	}
	float QuadrateThreshold = 11;
	if (FMath::Abs(Axis0.Length() - Axis1.Length()) < QuadrateThreshold || FMath::Abs(Axis0.Length() - Axis2.Length()) < QuadrateThreshold || FMath::Abs(Axis2.Length() - Axis1.Length()) < QuadrateThreshold)
	{
		BoxUpDir = UpVectorUpDir;
	}
	if (FVector::DotProduct(ObjectUpVector, UpVector) < .1)
	{
		BoxUpDir = ObjectUpDir;
	}
}

void OpenAssetProcess::CalculateOpenAssetTransform()
{
	for (int32 c = 0; c < NumberOfCheck; c++)
	{
		Transform = FixedTransform;
		//FVector RightVector = Transform.GetRotation().GetRightVector();
		//FVector FixedRightVector = FVector::CrossProduct(FVector::CrossProduct(UpVector, RightVector), UpVector);
		//TArray<FVector> Vertices = *MeshOpenVertex.Find(StaticMesh);
		OrientSpaceVertices.Empty();
		int32 AboveGround = 0;
		float TransformDir = -1;
		float MinTraceDistance = 9999;
		float MinUpTraceDistance = 9999;
		float MaxTraceDistance = -9999;
		float MaxUpTraceDistance = -9999;
		FVector StartTest;
		for (FVector Vertice : Vertices)
		{
			FVector WorldLocation = Transform.TransformPosition(Vertice);
			OrientSpaceVertices.Add(WorldLocation * (FVector::OneVector - UpVector));
		}
		for (int32 i = 0; i < Vertices.Num(); i++)
		{
			
			FVector Start = Transform.TransformPosition(Vertices[i]);
			StartTest = Start;
			FVector End = Start - UpVector * 100000;
			TArray<AActor*> ActorsToIgnore;
			ActorsToIgnore = IgOutActors;
			FHitResult OutHit;
			//bool Hit;
			if (UKismetSystemLibrary::LineTraceSingleForObjects(GWorld, Start, End, ObjectTypes, true, ActorsToIgnore, EDrawDebugTrace::None, OutHit, true))
			{
				if(OutHit.Normal.Z<0)
				{
					OutHit.Normal *= -1;
				}
				if (FVector::DotProduct(OutHit.Normal, UpVector) > 0)
				{
					AboveGround += 1;
					OrientSpaceVertices[i] = OrientSpaceVertices[i] + UpVector * OutHit.Distance;
					if (OutHit.Distance < MinTraceDistance)
					{
						MinTraceDistance = OutHit.Distance;
					}
					if (OutHit.Distance > MaxTraceDistance)
					{
						MaxTraceDistance = OutHit.Distance;
					}
				}
			}
			FVector UpEnd = Start + UpVector * 100000;
			if (UKismetSystemLibrary::LineTraceSingleForObjects(GWorld, Start, UpEnd, ObjectTypes, true, ActorsToIgnore, EDrawDebugTrace::None, OutHit, true))
			{
				if(OutHit.Normal.Z<0)
				{
					OutHit.Normal *= -1;
				}
				if (FVector::DotProduct(OutHit.Normal, UpVector) > 0)
				{
					OrientSpaceVertices[i] = OrientSpaceVertices[i] - UpVector * OutHit.Distance;
					if (OutHit.Distance < MinUpTraceDistance)
					{
						MinUpTraceDistance = OutHit.Distance;
					}
					if (OutHit.Distance > MaxUpTraceDistance)
					{
						MaxUpTraceDistance = OutHit.Distance;
					}
				}
			}
		}
		StartTest = StartTest;
		MaxTraceDistance = FMath::Max(float(20), float(MaxTraceDistance * .2));
		//如果所有点都在地面以下了, 那就网上移把.
		if (AboveGround == 0)
		{
			TransformDir = 1;
			MinTraceDistance = MinUpTraceDistance;
			MaxTraceDistance = 20;
		}

		CalculateEigenVector();

		//UE_LOG(LogTemp, Warning, TEXT(" %s"), *BoxUpDir.ToString());
		UE_LOG(LogTemp, Warning, TEXT("ThreadDone"));
		FVector TransformLocation = FixedTransform.GetLocation() + UpVector * TransformDir * FMath::Max(MinTraceDistance, MaxTraceDistance);
		float DotUpVector = FVector::DotProduct(UpVector, BoxUpDir);
		FRotator ActorRotator = Transform.Rotator();
		if (FMath::Abs(DotUpVector) < .95)
		{
			FRotator FixedRotator = FQuat(FVector::CrossProduct(UpVector, BoxUpDir), FMath::Clamp(FMath::Acos(DotUpVector), float(0), float(3.14 / 4))).Rotator();
			FTransform RotatorTransform = FTransform(FixedRotator, FVector::ZeroVector).Inverse();
			ActorRotator = RotatorTransform.TransformRotation(Transform.Rotator().Quaternion()).Rotator();

		}
		FixedTransform = FTransform(ActorRotator, TransformLocation, FixedTransform.GetScale3D());
	}
}

void OpenAssetProcess::CalculateSafeTransform()
{
	Transform = FixedTransform;
	
	int32 AboveGround = 0;
	//float TransformDir = -1;
	float MinTraceDistance = 9999;
	//float MinUpTraceDistance = 9999;
	float MaxTraceDistance = -9999;
	//float MaxUpTraceDistance = -9999;
	for (FVector Vertice : Vertices)
	{
		FVector WorldLocation = Transform.TransformPosition(Vertice);
		OrientSpaceVertices.Add(WorldLocation * (FVector::OneVector - UpVector));
	}

	for (int32 i = 0; i < Vertices.Num(); i++)
	{
		FVector Start = Transform.TransformPosition(Vertices[i]);
		FVector End = Start - UpVector * 100000;
		TArray<AActor*> ActorsToIgnore;
		ActorsToIgnore = IgOutActors;
		FHitResult OutHit;
		if (UKismetSystemLibrary::LineTraceSingleForObjects(GWorld, Start, End, ObjectTypes, true, ActorsToIgnore, EDrawDebugTrace::None, OutHit, true))
		{
			if(OutHit.Normal.Z<0)
			{
				OutHit.Normal *= -1;
			}
			if (FVector::DotProduct(OutHit.Normal, UpVector) > 0)
			{
				AboveGround += 1;
				OrientSpaceVertices[i] += UpVector * OutHit.Distance;
				if (OutHit.Distance < MinTraceDistance)
				{
					MinTraceDistance = OutHit.Distance;
				}
				if (OutHit.Distance > MaxTraceDistance)
				{
					MaxTraceDistance = OutHit.Distance;
				}
			}
		}
	}
	if (MaxTraceDistance > 0)
	{
		FVector Location = Transform.GetLocation();
		Location += UpVector * -1 * MaxTraceDistance;
		FixedTransform.SetLocation(Location);
	}
}

void OpenAssetProcess::CalculateOpenVerticesDir()
{
	TArray<FVector> VerticesTemp = Vertices;
	for (FVector &Vertice : VerticesTemp)
	{
		FVector WorldLocation = FixedTransform.TransformPosition(Vertice);
		Vertice = WorldLocation;
	}
	FBox Box = FBox(VerticesTemp);
	FVector OpenVertexsCenter = (Box.Max + Box.Min) / 2;
	FVector ObjectCenter, ObjectExtent;
	ObjectCenter = FixedTransform.TransformPosition(StaticMesh->GetBounds().Origin);
	UpVector = (ObjectCenter - OpenVertexsCenter).GetSafeNormal();
	if (FVector::DotProduct(UpVector, FVector(0, 0, -1)) > 0)
	{
		UpVector = FVector(0, 0, 1);
	}
	
}


// void OpenAssetProcess::CalculateTransform()
// {
// 	
// }
//
