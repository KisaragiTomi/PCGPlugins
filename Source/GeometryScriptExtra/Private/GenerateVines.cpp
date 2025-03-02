// Fill out your copyright notice in the Description page of Project Settings.


#include "GenerateVines.h"

#include "Landscape.h"
#include "PointFunction.h"
#include "GeometryAsync.h"
#include "Kismet/KismetSystemLibrary.h"

using namespace UE::Geometry;

void UGenerateVines::GenerateVines(AVineContainer* Container, FSpaceColonizationOptions SC, float ExtrudeScale, bool Result, bool OutDebugMesh, bool MultThread)
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
	if (!Container)
		return;
	
	UDynamicMesh* ContainerMesh = Container->GetDynamicMeshComponent()->GetDynamicMesh();
	ContainerMesh->Reset();
	
	TArray<FTransform> TubeSourceTransforms;
	TArray<FTransform> PlaneSourceTransforms;
	TArray<FTransform> TargetTransforms;
//	Container->FoliageInstanceContainer->GetInstanceTransform(0, TubeSourceTransforms[0], true);
	
	int32 TargetCount = Container->FoliageInstanceContainer->GetInstanceCount();
	TargetTransforms.Reserve(TargetCount);
	
	for (int32 i = 0; i < TargetCount; i++)
	{
		FTransform Transform;
		Container->FoliageInstanceContainer->GetInstanceTransform(i, Transform, true);
		TargetTransforms.Add(Transform);
	}

	int32 TubeSourceCount = Container->TubeFoliageInstanceContainer->GetInstanceCount();
	TubeSourceTransforms.Reserve(TubeSourceCount);
	
	for (int32 i = 0; i < TubeSourceCount; i++)
	{
		FTransform Transform;
		Container->TubeFoliageInstanceContainer->GetInstanceTransform(i, Transform, true);
		TubeSourceTransforms.Add(Transform);
	}

	int32 PlaneSourceCount = Container->PlaneFoliageInstanceContainer->GetInstanceCount();
	PlaneSourceTransforms.Reserve(PlaneSourceCount);
	
	for (int32 i = 0; i < PlaneSourceCount; i++)
	{
		FTransform Transform;
		Container->PlaneFoliageInstanceContainer->GetInstanceTransform(i, Transform, true);
		PlaneSourceTransforms.Add(Transform);
	}
	if (TargetCount == 0 || (TubeSourceCount == 0 && PlaneSourceCount == 0))
		return;
	
	TArray<FTransform> BBoxTransforms;
	TArray<FVector> BBoxVectors;
	BBoxTransforms.Append(TubeSourceTransforms);
	BBoxTransforms.Append(PlaneSourceTransforms);
	BBoxTransforms.Append(TargetTransforms);
	BBoxVectors.Reserve(BBoxTransforms.Num());
	for (FTransform Transform : BBoxTransforms)
	{
		BBoxVectors.Add(Transform.GetLocation());
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
	UKismetSystemLibrary::BoxOverlapActors(GWorld, Center, Extent, ObjectTypes, nullptr, ActorsToIgnore, OverlapActors);

	TArray<AActor*> MeshActors;
	for (AActor* Actor : OverlapActors)
	{
		if (!Cast<ALandscape>(Actor) && !Cast<ALandscapeProxy>(Actor))
		{
			MeshActors.Add(Actor);
		}
	}
	FGeometryScriptDynamicMeshBVH BVH;
	UDynamicMesh* MeshCombine = nullptr;
	// if (Container->BVH.Spatial.IsValid() == false || Container->PrefixMesh == nullptr || !Container->
	// 	CheckActors(MeshActors) || Container->InstanceBound != Bounds)
	// {
	MeshCombine = UGeometryGenerate::VDBMeshFromActors(MeshActors, BBoxVectors, (OutDebugMesh?false:true), SC.ExtentPlus, SC.VoxelSize, ExtrudeScale, MultThread);
	MeshCombine = UGeometryScriptLibrary_MeshSpatial::BuildBVHForMesh(MeshCombine, BVH, nullptr);
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
		MeshCombine->ProcessMesh([&](const FDynamicMesh3& EditMesh)
		{
			MeshCopy = EditMesh;
		});

		ContainerMesh->SetMesh(MoveTemp(MeshCopy));
		return;
	}

	TArray<FGeometryScriptPolyPath> Lines;
	//TubeLines
	for (int32 i = 0; i < TubeSourceCount; i++)
	{
		TArray<FTransform> SCSourceTransform;
		SCSourceTransform.Add(TubeSourceTransforms[i]);
		Lines.Append(SpaceColonization(SCSourceTransform, TargetTransforms, SC.Iteration, SC.Activetime, 5, SC.RandGrow, SC.Seed, SC.BackGrowRange, MultThread));
	}
	UDynamicMesh* OutMesh = Container->GetDynamicMeshComponent()->GetDynamicMesh();
	OutMesh->Reset();
	
	Container->TubeLines.Reset();
	Container->TubeLines = Lines;
	Container->VisVine(true);

	//PlaneLines
	Lines.Reset();
	for (int32 i = 0; i < PlaneSourceCount; i++)
	{
		TArray<FTransform> SCSourceTransform;
		SCSourceTransform.Add(PlaneSourceTransforms[i]);
		Lines.Append(SpaceColonization(SCSourceTransform, TargetTransforms, SC.Iteration, SC.Activetime, 12, SC.RandGrow, SC.Seed, SC.BackGrowRange, MultThread));
	}
	Container->PlaneLines.Reset();
	Container->PlaneLines = Lines;
	Container->VisVine(false);
}

TArray<FGeometryScriptPolyPath> UGenerateVines::SpaceColonization(TArray<FTransform> TubeSourceTransforms, TArray<FTransform> TargetTransforms, int32 Iteration, int32 Activetime, int32 BackGrowCount, float RandGrow, float Seed, float BackGrowRange, bool MultThread)
{
	TArray<FGeometryScriptPolyPath> Lines;
	if (TubeSourceTransforms.Num() == 0 || TargetTransforms.Num() == 0)
		return Lines;
	
	TArray<FVector> SourceLocations;
	SourceLocations.Reserve(TubeSourceTransforms.Num());
	TArray<FVector> TargetLocations;
	TargetLocations.Reserve(TargetTransforms.Num());
	TArray<FSpaceColonizationAttribute> SCAttributes;
	SCAttributes.SetNum(TargetTransforms.Num());
	for (FTransform Transform : TubeSourceTransforms)
	{
		SourceLocations.Add(Transform.GetLocation());
	}
	for (FTransform Transform : TargetTransforms)
	{
		TargetLocations.Add(Transform.GetLocation());
	}
	for (FVector SourceLocation : SourceLocations)
	{
		int32 Nearpt = UPointFunction::FindNearPointIteration(TargetLocations, SourceLocation);
		SCAttributes[Nearpt].Attractor = false;
		SCAttributes[Nearpt].Startpt = true;
		SCAttributes[Nearpt].Startid = Nearpt;
	}
	
	//TArray<TArray<FVector>> Lines;
	
	//bool MultThread = false;
	float Infrad = 200;
	int32 ThreadPointNum = 1;
	int32 NumPt = TargetLocations.Num();
	int32 NumThreads = FMath::Min(NumPt / ThreadPointNum + 1, FPlatformMisc::NumberOfCoresIncludingHyperthreads() - 1LL);
	TArray<TFuture<void>> Threads;
	Threads.Reserve(NumThreads);
	const int64 Batch = NumPt / NumThreads + 1;
	for (int32 i = 0; i < Iteration; i++ )
	{
		
		for (int32 p = 0; p < NumPt; p++ )
		{
			SCAttributes[p].Associates.Reset();
		}
		if (MultThread)
		{
			SCOPE_CYCLE_COUNTER(STAT_SpaceColonizationMultThread)
			TArray<TTuple<int32, int32>> AssociatesCollection =  ProcessAsync::ProcessAsync<TTuple<int32, int32>>(NumPt, ThreadPointNum, [&](const int32 p)
			{
				TTuple<int32, int32> FindPt;
				FindPt.Key = -1;
				bool Attractor = SCAttributes[p].Attractor;
				if (!Attractor)
					return FindPt;
				FVector FindLocation = TargetLocations[p];
				int32 NearPt = UPointFunction::FindNearPointIteration(TargetLocations, FindLocation, [SCAttributes](int32 CurrentIteration)
				{
					return SCAttributes[CurrentIteration].Attractor == false;
				});
				if (NearPt == -1)
					return FindPt;
							
				float NearestDist = FVector::Dist(FindLocation, TargetLocations[NearPt]);
				if (NearestDist * 1.1 < Infrad )
				{
					FindPt.Key = NearPt;
					FindPt.Value = p;
				}
				return FindPt;
			});
			for (int32 j = 0; j < AssociatesCollection.Num(); j++)
			{
				if (AssociatesCollection[j].Key == -1)
					continue;
				SCAttributes[AssociatesCollection[j].Key].Associates.Add(AssociatesCollection[j].Value);
			}

			TArray<TTuple<FIndex3i, FVector>> ProcessResult = ProcessAsync::ProcessAsync<TTuple<FIndex3i, FVector>>(NumPt, ThreadPointNum, [&](const int32 p)
			{
				TTuple<FIndex3i, FVector> ThreadCalculate;
				ThreadCalculate.Key = FIndex3i(-1, -1, 0);
				float Grow = FMath::RandRange(0, 1);
				if (SCAttributes[p].Attractor == true
					|| SCAttributes[p].SpawnCount < i - Activetime
					|| (Grow < RandGrow && i > 10)
					|| SCAttributes[p].Associates.Num() == 0
					|| SCAttributes[p].BranchCount > 2)
					return ThreadCalculate;

				
				FVector FindLocation = TargetLocations[p];
				int32 NearPt = UPointFunction::FindNearPointIteration(TargetLocations, FindLocation, [SCAttributes](int32 CurrentIteration)
				{
					return SCAttributes[CurrentIteration].Attractor == true;
				});
				if (NearPt == -1)
					return ThreadCalculate;
							
				float NearestDist = FVector::Dist(FindLocation, TargetLocations[NearPt]);

				FVector DirSum;
				for (int32 Index : SCAttributes[p].Associates)
				{
					FVector Dir = (TargetLocations[Index] - TargetLocations[p]);
					//Dir.Normalize();
					DirSum += Dir;
				}
				DirSum.Normalize();
				
				ThreadCalculate.Key = FIndex3i(p, NearPt, 1);
				ThreadCalculate.Value = TargetLocations[p]+ DirSum * NearestDist;
				
				return ThreadCalculate;
			});
			for (int32 j = 0; j < ProcessResult.Num(); j++)
			{
				if (ProcessResult[j].Key.C == 0)
					continue;
				int32 p = ProcessResult[j].Key.A;
				int32 NearPt = ProcessResult[j].Key.B;
				SCAttributes[p].SpawnCount += 1;
				TargetLocations[NearPt] = ProcessResult[j].Value;
				SCAttributes[NearPt].Startid = SCAttributes[p].Startid;
				SCAttributes[NearPt].SpawnCount = SCAttributes[p].SpawnCount;
				SCAttributes[NearPt].Attractor = false;
				SCAttributes[NearPt].End = true;
				SCAttributes[NearPt].BranchCount = 1;
				SCAttributes[p].End = false;
				
				SCAttributes[NearPt].PrePt = p;
				SCAttributes[p].NextPt = NearPt;
				SCAttributes[p].BranchCount += 1;
			}
		}
		else
		{
			SCOPE_CYCLE_COUNTER(STAT_SpaceColonization)
			for (int32 p = 0; p < NumPt; p++ )
			{
				bool Attractor = SCAttributes[p].Attractor;
				if (!Attractor)
					continue;
				FVector FindLocation = TargetLocations[p];
				int32 NearPt = UPointFunction::FindNearPointIteration(TargetLocations, FindLocation, [SCAttributes](int32 CurrentIteration)
				{
					return SCAttributes[CurrentIteration].Attractor == false;
				});
				if (NearPt == -1)
					continue;
				
				float NearestDist = FVector::Dist(FindLocation, TargetLocations[NearPt]);
				if (NearestDist * 1.1 < Infrad )
				{
					//Infrad = NearestDist * 1.1;
					SCAttributes[NearPt].Associates.Add(p);
				}
			}
			
			for (int32 p = 0; p < NumPt; p++ )
			{
				float Grow = FMath::RandRange(0, 1);
				if (SCAttributes[p].Attractor == true
					|| SCAttributes[p].SpawnCount < i - Activetime
					|| (Grow < RandGrow && i > 10)
					|| SCAttributes[p].Associates.Num() == 0
					|| SCAttributes[p].BranchCount > 2)
					continue;
				
				FVector FindLocation = TargetLocations[p];
				int32 NearPt = UPointFunction::FindNearPointIteration(TargetLocations, FindLocation, [SCAttributes](int32 CurrentIteration)
				{
					return SCAttributes[CurrentIteration].Attractor == true;
				});
				if (NearPt == -1)
					continue;
				
				float NearestDist = FVector::Dist(FindLocation, TargetLocations[NearPt]);

				FVector DirSum;
				for (int32 Index : SCAttributes[p].Associates)
				{
					FVector Dir = (TargetLocations[Index] - TargetLocations[p]);
					//Dir.Normalize();
					DirSum += Dir;
				}
				DirSum.Normalize();
				
				SCAttributes[p].SpawnCount += 1;
				TargetLocations[NearPt] = TargetLocations[p]+ DirSum * NearestDist;
				SCAttributes[NearPt].Startid = SCAttributes[p].Startid;
				SCAttributes[NearPt].SpawnCount = SCAttributes[p].SpawnCount;
				SCAttributes[NearPt].Attractor = false;
				SCAttributes[NearPt].End = true;
				SCAttributes[NearPt].BranchCount = 1;
				SCAttributes[p].End = false;
				
				SCAttributes[NearPt].PrePt = p;
				SCAttributes[p].NextPt = NearPt;
				SCAttributes[p].BranchCount += 1;

			}
		}

	}
	//CreateLineArray
	for (int32 p = 0; p < NumPt; p++)
	{
		if (SCAttributes[p].End != true)
			continue;
		
		TArray<FVector> Line;
		int32 LineCount = 0;
		int32 CurrentIndex = p;

		Line.Add(TargetLocations[CurrentIndex]);
		//float LineLength = 0;
		while (SCAttributes[CurrentIndex].PrePt != -1 || LineCount < 100 )
		{
			int32 PreIndex = SCAttributes[CurrentIndex].PrePt;
			if (PreIndex == -1)
				break;
			
			float DistancePre = FVector::Dist(TargetLocations[PreIndex], TargetLocations[CurrentIndex]);
			Line.Add(TargetLocations[PreIndex]);
			CurrentIndex = PreIndex;
			LineCount += 1;
			FRandomStream Random(332 + DistancePre);
			const float RandomOffset = Random.FRand();
			SCAttributes[CurrentIndex].BackCount += 1;
			if (SCAttributes[CurrentIndex].BackCount > BackGrowCount)
			 	break;
			//LineLength += DistancePre;
		}
		if (Line.Num() == 0)
			continue;
		
		FGeometryScriptPolyPath PolyPath;
		PolyPath.Reset();
		*PolyPath.Path = Line;
		
		Lines.Add(PolyPath);
	}
	return Lines;
}