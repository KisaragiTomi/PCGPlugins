#include "GeometryGenerate.h"

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

		if (false)
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
			PathTexParamV, true, 1, 1, 0, nullptr);

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
		//MergedMeshesDescription.Vertices().Num();

		FDynamicMesh3 ConvertlMesh;
		FMeshDescriptionToDynamicMesh Converter;
		Converter.Convert(&MergedMeshesDescription, ConvertlMesh);
		
		EditMesh.Copy(ConvertlMesh);
		
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	
	return TargetMesh;
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
