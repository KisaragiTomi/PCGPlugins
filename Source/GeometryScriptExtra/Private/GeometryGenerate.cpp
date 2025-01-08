#include "GeometryGenerate.h"

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
#include "PointFunction.h"
#include "PolyLine.h"
#include "Curve/CurveUtil.h"
#include "Selection/GeometrySelector.h"
#include "GeometryMath/Public/Noise.h"
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
#include "DynamicMesh/MeshNormals.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "GeometryScript/MeshAssetFunctions.h"


using namespace UE::Geometry;


void UGeometryGenerate::GenerateVines(FSpaceColonizationOptions SC, bool Result, bool OutDebugMesh)
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
	
	UEditorActorSubsystem* EditorActorSubsystem = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
	TArray<AActor*> SelectedActors = EditorActorSubsystem->GetSelectedLevelActors();
	AVineContainer* Container = nullptr;
	for (AActor* Actor : SelectedActors)
	{
		if (Cast<AVineContainer>(Actor))
		{
			Container = Cast<AVineContainer>(Actor);
			break;
		}
	}
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
	if (Container->BVH.Spatial.IsValid() == false || Container->PrefixMesh == nullptr || !Container->
		CheckActors(MeshActors) || Container->InstanceBound != Bounds)
	{
		MeshCombine = VDBMeshFromActors(MeshActors, Bounds, (OutDebugMesh?false:true), SC.ExtentPlus, SC.VoxelSize);
		MeshCombine = UGeometryScriptLibrary_MeshSpatial::BuildBVHForMesh(MeshCombine, BVH, nullptr);
		Container->BVH = BVH;
		Container->PrefixMesh = MeshCombine;
		Container->InstanceBound = Bounds;
		Container->PickActors = MeshActors;
	}
	else
	{
		BVH = Container->BVH;
		MeshCombine = Container->PrefixMesh;
	}
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
		Lines.Append(SpaceColonization(SCSourceTransform, TargetTransforms, SC.Iteration, SC.Activetime, 5, SC.RandGrow, SC.Seed, SC.BackGrowRange));
	}
	UDynamicMesh* OutMesh = Container->GetDynamicMeshComponent()->GetDynamicMesh();
	OutMesh->Reset();
	
	Container->TubeLines.Reset();
	Container->TubeLines = Lines;
	Container->VisVine(true);

	//PlaneLines
	for (int32 i = 0; i < PlaneSourceCount; i++)
	{
		TArray<FTransform> SCSourceTransform;
		SCSourceTransform.Add(PlaneSourceTransforms[i]);
		Lines.Append(SpaceColonization(SCSourceTransform, TargetTransforms, SC.Iteration, SC.Activetime, 12, SC.RandGrow, SC.Seed, SC.BackGrowRange));
	}
	Container->PlaneLines.Reset();
	Container->PlaneLines = Lines;
	Container->VisVine(false);
}

TArray<FGeometryScriptPolyPath> UGeometryGenerate::SpaceColonization(TArray<FTransform> TubeSourceTransforms, TArray<FTransform> TargetTransforms, int32 Iteration, int32 Activetime, int32 BackGrowCount, float RandGrow, float Seed, float BackGrowRange)
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
	float Infrad = 200;
	int32 PtNum = TargetLocations.Num();
	for (int32 i = 0; i < Iteration; i++ )
	{
		for (int32 p = 0; p < PtNum; p++ )
		{
			SCAttributes[p].Associates.Reset();
		}
		for (int32 p = 0; p < PtNum; p++ )
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
		
		for (int32 p = 0; p < PtNum; p++ )
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

	//CreateLineArray
	for (int32 p = 0; p < TargetTransforms.Num(); p++)
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

UDynamicMesh* UGeometryGenerate::VDBMeshFromActors(TArray<AActor*> In_Actors, FBox Bounds, bool Result, int32 ExtentPlus, float VoxelSize)
{
	float LandscapeMeshExtrude = 100;
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
	if (AppendMeshComponents.Num() == 0)
	{
		//return nullptr;
	}
	//CollectSceneMeshComponennt
	FTransform TransformCenter = FTransform::Identity;
	for (UStaticMeshComponent* AppendMeshComponent : AppendMeshComponents)
	{
		//FTransform ComponentTransform = AppendMeshComponent->GetComponentToWorld();
		if (Cast<UInstancedStaticMeshComponent>(AppendMeshComponent))
		{
			UInstancedStaticMeshComponent* Instances = Cast<UInstancedStaticMeshComponent>(AppendMeshComponent);
			int32 InstanceCount = Instances->GetInstanceCount();
			UStaticMesh* InstanceMesh = Instances->GetStaticMesh();
			for (int32 i = 0; i < InstanceCount; i++)
			{
				UDynamicMesh* InstanceDynamicMesh = NewObject<UDynamicMesh>();
				FTransform InstanceTransform = FTransform::Identity;
				Instances->GetInstanceTransform(i, InstanceTransform);
				FBox StaticMeshBound = InstanceMesh->GetBoundingBox();
				StaticMeshBound = StaticMeshBound.TransformBy(InstanceTransform);
				
				if (!StaticMeshBound.Intersect(Bounds))
					continue;
				
				FGeometryScriptCopyMeshFromAssetOptions AssetOptions;
				FGeometryScriptMeshReadLOD RequestedLOD;
				RequestedLOD.LODIndex = FMath::Min(InstanceMesh->GetNumLODs() - 1, 3);
				RequestedLOD.LODType = EGeometryScriptLODType::RenderData;
				EGeometryScriptOutcomePins Outcome;
				UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMesh(InstanceMesh, InstanceDynamicMesh, AssetOptions, RequestedLOD, Outcome);
				
				InstanceDynamicMesh->EditMesh([&](FDynamicMesh3& EditMesh) 
				{
					MeshTransforms::ApplyTransform(EditMesh, (FTransformSRT3d)InstanceTransform, true);
				
				}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
				OutMesh->EditMesh([&](FDynamicMesh3& AppendToMesh)
				{
					InstanceDynamicMesh->EditMesh([&](FDynamicMesh3& EditMesh)
					{
						FTransformSRT3d XForm(TransformCenter);
						FMeshIndexMappings TmpMappings;
						FDynamicMeshEditor Editor(&AppendToMesh);
						const FDynamicMesh3* UseOtherMesh = &EditMesh;
						Editor.AppendMesh(UseOtherMesh, TmpMappings,
							[&](int, const FVector3d& Position) { return XForm.TransformPosition(Position); },
							[&](int, const FVector3d& Normal) { return XForm.TransformNormal(Normal); });
					});
				}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
			}
			continue;
		}
		//Bounds.Intersect()
		
		UE::Conversion::FToMeshOptions ToMeshOptions;
		ToMeshOptions.bUseClosestLOD = false;
		ToMeshOptions.LODIndex = 0;
		
		ToMeshOptions.LODType = UE::Conversion::EMeshLODType::MaxAvailable;
		ToMeshOptions.bWantNormals = true;
		ToMeshOptions.bWantTangents = true;
		FDynamicMesh3 CopyMesh;
		FText ErrorMessage;
		FTransform LocalToWorld;
		bool bSuccess = UE::Conversion::SceneComponentToDynamicMesh(AppendMeshComponent, ToMeshOptions, true, CopyMesh, LocalToWorld, ErrorMessage);
		if (!bSuccess)
		{
			continue;
		}
		
		OutMesh->EditMesh([&](FDynamicMesh3& AppendToMesh)
		{
			FTransformSRT3d XForm(TransformCenter);
			FMeshIndexMappings TmpMappings;
			FDynamicMeshEditor Editor(&AppendToMesh);
			const FDynamicMesh3* UseOtherMesh = &CopyMesh;
			Editor.AppendMesh(UseOtherMesh, TmpMappings,
				[&](int, const FVector3d& Position) { return XForm.TransformPosition(Position); },
				[&](int, const FVector3d& Normal) { return XForm.TransformNormal(Normal); });

		}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	}
	
	//CreateBoundaryMesh
	UGeometryGenerate::ExtrudeUnclosedBoundary(OutMesh, LandscapeMeshExtrude);

	//TODO Adapting to situations where objects are very far from the Landscape
	//CreateLandscapeMesh
	UDynamicMesh* LandscapePlaneMesh = NewObject<UDynamicMesh>(); 
	ULandscapeExtra::CreateProjectPlane(LandscapePlaneMesh, Center, Extent * 1.1, ExtentPlus);
	UDynamicMesh* BoundaryMesh = FixUnclosedBoundary(LandscapePlaneMesh, LandscapeMeshExtrude, false, false);
	FGeometryScriptAppendMeshOptions AppendOptions;
	UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(OutMesh, LandscapePlaneMesh, FTransform(FVector(0, 0, -LandscapeMeshExtrude)));
	UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(OutMesh, LandscapePlaneMesh, FTransform::Identity);
	UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(OutMesh, BoundaryMesh, FTransform::Identity);
	
	if (!Result)
	{
		return OutMesh;
	}
	
	OutMesh = VoxelMergeMeshs(OutMesh , VoxelSize);
	
	//BlurNormals
	//OutMesh = BlurVertexNormals(OutMesh);
	
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
			PathTexParamV, true, 1, 1, 0, nullptr);

		UDynamicMesh* FillFanMesh = NewObject<UDynamicMesh>();
		
		FixMesh->EditMesh([&](FDynamicMesh3& FixEditMesh)
		{
			BoundaryMesh->EditMesh([&](FDynamicMesh3& EditMesh)
			{
				int32 VertexCount = EditMesh.VertexCount() / 2;
				FMeshNormals Normals(&FixEditMesh);
				Normals.ComputeVertexNormals();
				TArray<FVector> NormalArray = Normals.GetNormals();
				
				for (int i = 0; i < VertexCount; i++)
				{
					int32 TarId = i * 2 + 1;
					if (EditMesh.IsVertex(TarId))
					{
						FVector Normal = NormalArray[LoopVertexNumber[i]];
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
		
		UGeometryGenerate::FillLine(FillFanMesh, Vertices);
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

UDynamicMesh* UGeometryGenerate::FillLine(UDynamicMesh* TargetMesh, TArray<FVector> VertexLoop)
{
	bool Success = true;
	UDynamicMesh* BoundaryMeshCombineOut = NewObject<UDynamicMesh>();
	BoundaryMeshCombineOut->Reset();
	FDynamicMesh3 CopyMesh;
	BoundaryMeshCombineOut->EditMesh([&](FDynamicMesh3& EditMesh)
	{
		int32 VertexCount = VertexLoop.Num();
		for (int32 i = 0; i < VertexCount; i++)
		{
			EditMesh.AppendVertex(VertexLoop[i]);
		}
		FVector3d c = FVector3d::Zero();
		for (int i = 0; i < VertexLoop.Num(); ++i)
		{
			c += VertexLoop[i];
		}
		c *= 1.0 / VertexLoop.Num();
		// add centroid vtx
		int32 NewVertex = EditMesh.AppendVertex(c);
		
		//FDynamicMeshEditor Editor(EditMesh);
		FDynamicMeshEditResult AddFanResult;
		int N = VertexLoop.Num();
		AddFanResult.NewTriangles.Reserve(N);

		int i = 0;
		for (i = 0; i < N; ++i)
		{
			int A = i;
			int B = (i + 1) % N;

			FIndex3i NewT(NewVertex, B, A);
			int NewTID = EditMesh.AppendTriangle(NewT, 0);
			if (NewTID < 0)
			{
				Success = false;
			}

			AddFanResult.NewTriangles.Add(NewTID);
		}
		CopyMesh = EditMesh;
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);

	if (Success)
	{
		TargetMesh->SetMesh(MoveTemp(CopyMesh));
		return TargetMesh;
	}
	return nullptr;
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
			PathTexParamV, true, 1, 1, 0, nullptr);
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
		MergedMeshesDescription.Vertices().Num();

		FDynamicMesh3 ConvertlMesh;
		FMeshDescriptionToDynamicMesh Converter;
		Converter.Convert(&MergedMeshesDescription, ConvertlMesh);
		
		EditMesh.Copy(ConvertlMesh);
		
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	
	return TargetMesh;
}

UDynamicMesh* UGeometryGenerate::BlurVertexNormals(UDynamicMesh* TargetMesh, int32 Iteration, bool RecomputeNormals)
{
	FGeometryScriptCalculateNormalsOptions CalculateOptions;
	if (RecomputeNormals)
	{
		TargetMesh = UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(TargetMesh, CalculateOptions);
	}
	
	FVector3f normaltestpre;
	FVector3f normaltestaffter;
	TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh) 
	{
		FDynamicMeshNormalOverlay* Normals = EditMesh.Attributes()->PrimaryNormals();
	
		if (Normals->ElementCount() > 0)
		{
			FSmoothDynamicMeshAttributes BlurOp(EditMesh);
			BlurOp.bUseParallel = true;
			BlurOp.NumIterations = Iteration;
			BlurOp.Strength = .5;
			BlurOp.EdgeWeightMethod = static_cast<FSmoothDynamicMeshAttributes::EEdgeWeights>(0);
		
			TArray<bool> NormalsToSmooth;
			NormalsToSmooth.Reserve(3);
			NormalsToSmooth.Add(true);
			NormalsToSmooth.Add(true);
			NormalsToSmooth.Add(true);
		
			FGeometryScriptMeshSelection Selection;
			if (!Selection.IsEmpty())
			{
				Selection.ProcessByVertexID(EditMesh, [&](int32 VertexID)
				{
					BlurOp.Selection.Add(VertexID);
				});
			}
			
			BlurOp.SmoothOverlay(Normals, NormalsToSmooth);
		}
		
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	
	UGeometryGenerate::CreateVertexNormals(TargetMesh);
	
	return TargetMesh;
}

UDynamicMesh* UGeometryGenerate::GetVertexNormalOverlay(UDynamicMesh* TargetMesh)
{
	return TargetMesh;
}

UDynamicMesh* UGeometryGenerate::CreateVertexNormals(UDynamicMesh* TargetMesh)
{
	TArray<int32> Test;
	TArray<FVector> NormalsTemp;
	FVector3f Test0;
	FVector3f Test1;
	FVector3f Test2;
	FVector3f Test3;
	TArray<FVector3f> NormalsTemp2;
	TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh) 
	{
		if (EditMesh.VertexCount() > 0)
		{
			if (EditMesh.HasVertexNormals() == false)
			{
				EditMesh.EnableVertexNormals(FVector3f::UpVector);
			}
			FMeshNormals Normals(&EditMesh);
			Normals.ComputeVertexNormals();
			Normals.CopyToVertexNormals(&EditMesh, false);
			NormalsTemp = Normals.GetNormals();
			for (int32 i = 0; i < NormalsTemp.Num(); i++)
			{
				FVector3f Normal = FVector3f(NormalsTemp[i].X, NormalsTemp[i].Y, NormalsTemp[i].Z);
				EditMesh.SetVertexNormal(i, Normal);
				NormalsTemp2.Add(EditMesh.GetVertexNormal(i));
			}
			Test0 = EditMesh.GetVertexNormal(0);
			Test1 = EditMesh.GetVertexNormal(1);
			// for (int32 i = 0; i < EditMesh.VertexCount(); i++)
			// {
			// 	Test.Add(i);
			// }
			// FDynamicMeshNormalOverlay* Normals = EditMesh.Attributes()->PrimaryNormals();
			// int32 TriCount = EditMesh.TriangleCount();
			// for (int32 TriIndex = 0; TriIndex < TriCount; ++TriIndex)
			// {
			// 	FIndex3i TVid =  Normals->GetTriangle(TriIndex);
			// 	for (int32 i = 0; i < 3; ++i)
			// 	{
			// 		
			// 		FVector3f Normal = Normals->GetElement(TVid[i]);
			// 		EditMesh.SetVertexNormal(TVid[i], Normal);
			// 	}
			
			Test2 = EditMesh.GetVertexNormal(2);
			Test3 = EditMesh.GetVertexNormal(3);
		}

	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	
	return TargetMesh;
}

UStaticMesh* UGeometryGenerate::CreateStaticMeshAsset(UDynamicMesh* TargetMesh, FString AssetPathAndName, TArray<UMaterialInterface*> Materials)
{
	if (TargetMesh->GetTriangleCount() == 0) return nullptr;
	
	UE::AssetUtils::FStaticMeshAssetOptions AssetOptions;
	AssetPathAndName = UPackageTools::SanitizePackageName(AssetPathAndName);
	AssetOptions.NewAssetPath = AssetPathAndName;
	AssetOptions.NumSourceModels = 1;
	AssetOptions.AssetMaterials = Materials;
	AssetOptions.bEnableRecomputeNormals = false;
	AssetOptions.bEnableRecomputeTangents = true;
	AssetOptions.CollisionType = ECollisionTraceFlag::CTF_UseComplexAsSimple;
	AssetOptions.NumMaterialSlots = Materials.Num();
	
	FDynamicMesh3 CopyMesh;
	TargetMesh->ProcessMesh([&](const FDynamicMesh3& ReadMesh)
	{
		CopyMesh = ReadMesh;
	});
	AssetOptions.SourceMeshes.DynamicMeshes.Add(&CopyMesh);

	UE::AssetUtils::FStaticMeshResults ResultData;
	UE::AssetUtils::ECreateStaticMeshResult AssetResult = UE::AssetUtils::CreateStaticMeshAsset(AssetOptions, ResultData);
	
	UStaticMesh* NewStaticMesh = ResultData.StaticMesh;
	NewStaticMesh->PostEditChange();
	GEditor->EndTransaction();
	FAssetRegistryModule::AssetCreated(NewStaticMesh);
	return NewStaticMesh;
}

FVector UGeometryGenerate::TestViewPosition()
{
	FLevelEditorViewportClient* SelectedViewport = NULL;

	for(FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
	{
		if (!ViewportClient->IsOrtho())
		{
			SelectedViewport = ViewportClient;
		}
	}
	FViewport* Viewport =  SelectedViewport->Viewport;
	

	return FVector::ZeroVector;
}
