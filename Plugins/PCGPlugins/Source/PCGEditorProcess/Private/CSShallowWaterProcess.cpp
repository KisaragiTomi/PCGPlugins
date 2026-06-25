#include "CSShallowWaterProcess.h"
#include "CSAssetProcess.h"

#include "EngineUtils.h"
#include "Selection.h"
#include "EditorAssetLibrary.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialUtilities.h"
#include "Editor/MaterialEditor/Public/MaterialEditingLibrary.h"
#include "AssetToolsModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "TimerManager.h"


void UCSShallowWaterProcess::SaveSWData(ACSShallowWaterCapture* InCSSWActor)
{
	if (InCSSWActor == nullptr) return;

	UStaticMesh* SourceMesh = nullptr;
	TArray<FTransform> InstanceTransforms;
	const bool bUseISM = InCSSWActor->SimVisHISM
		&& InCSSWActor->SimVisHISM->GetStaticMesh()
		&& InCSSWActor->SimVisHISM->GetInstanceCount() > 0;

	if (bUseISM)
	{
		SourceMesh = InCSSWActor->SimVisHISM->GetStaticMesh();
		const int32 NumInstances = InCSSWActor->SimVisHISM->GetInstanceCount();
		InstanceTransforms.Reserve(NumInstances);
		for (int32 i = 0; i < NumInstances; i++)
		{
			FTransform T;
			InCSSWActor->SimVisHISM->GetInstanceTransform(i, T, /*bWorldSpace=*/false);
			InstanceTransforms.Add(T);
		}
	}
	else
	{
		SourceMesh = InCSSWActor->ReusltMesh->GetStaticMesh();
		if (!SourceMesh) return;
		InstanceTransforms.Add(FTransform::Identity);
	}

	if (!SourceMesh) return;

	ULevel* CurrentLevel = InCSSWActor->GetLevel();
	FString LevelPathName = GetPathNameSafe(CurrentLevel);
	FString FileName;
	FString LevelPath;
	FString Extension;
	FPaths::Split(LevelPathName, LevelPath, FileName, Extension);
	FString AssetFolderPath = LevelPath.Append("/CSSWData");

	if (InCSSWActor->SWUniqueID < 0)
	{
		FDateTime Time = FDateTime::Now();
		InCSSWActor->SWUniqueID = Time.GetDay() * 1e6 + Time.GetHour() * 1e4 + Time.GetMinute() * 1e2 + Time.GetSecond();
	}
	int32 ActorId = InCSSWActor->SWUniqueID;

	TArray<FFloat16Color> Pixels;
	int32 RTWidth = 0, RTHeight = 0;
	if (InCSSWActor->RT_ResultVelHeight)
	{
		FTextureRenderTargetResource* VHResource = InCSSWActor->RT_ResultVelHeight->GameThread_GetRenderTargetResource();
		if (VHResource)
		{
			FIntPoint VHSize = VHResource->GetSizeXY();
			RTWidth = VHSize.X;
			RTHeight = VHSize.Y;
			VHResource->ReadFloat16Pixels(Pixels);
		}
	}
	if (RTWidth <= 0 || RTHeight <= 0 || Pixels.Num() != RTWidth * RTHeight)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] RT_ResultVelHeight not available for bake. Run simulation first."));
		return;
	}

	TArray<FFloat16Color> DepthWetPixels;
	int32 DWWidth = 0, DWHeight = 0;
	if (InCSSWActor->RT_ResultDepthWet)
	{
		FTextureRenderTargetResource* DWResource = InCSSWActor->RT_ResultDepthWet->GameThread_GetRenderTargetResource();
		if (DWResource)
		{
			FIntPoint DWSize = DWResource->GetSizeXY();
			DWWidth = DWSize.X;
			DWHeight = DWSize.Y;
			DWResource->ReadFloat16Pixels(DepthWetPixels);
		}
	}

	FString MeshAssetName = FString::Printf(TEXT("SM_CSSW_Water_%d"), ActorId);
	FString DuplicateMeshPath = AssetFolderPath / MeshAssetName;

	if (UEditorAssetLibrary::DoesAssetExist(DuplicateMeshPath))
	{
		UEditorAssetLibrary::DeleteAsset(DuplicateMeshPath);
	}

	const FStaticMeshRenderData* RenderData = SourceMesh->GetRenderData();
	if (!RenderData || RenderData->LODResources.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] Source mesh has no render data: %s"), *GetPathNameSafe(SourceMesh));
		return;
	}
	const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
	const int32 SrcNumVerts = LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();
	const int32 SrcNumIndices = LOD.IndexBuffer.GetNumIndices();
	if (SrcNumVerts == 0 || SrcNumIndices < 3)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] Source mesh LOD0 empty: Verts=%d Indices=%d"), SrcNumVerts, SrcNumIndices);
		return;
	}

	TArray<FVector3f> SrcPositions;
	TArray<FVector3f> SrcNormals;
	TArray<FVector2f> SrcUVs;
	SrcPositions.SetNum(SrcNumVerts);
	SrcNormals.SetNum(SrcNumVerts);
	SrcUVs.SetNum(SrcNumVerts);
	for (int32 i = 0; i < SrcNumVerts; i++)
	{
		SrcPositions[i] = LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(i);
		SrcNormals[i] = FVector3f(LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(i));
		SrcUVs[i] = LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(i, 0);
	}
	TArray<uint32> SrcIndices;
	LOD.IndexBuffer.GetCopy(SrcIndices);
	const int32 SrcNumTris = SrcIndices.Num() / 3;

	UE_LOG(LogTemp, Log, TEXT("[CSSW] SrcMesh RenderData: Verts=%d, Tris=%d, Instances=%d"),
		SrcNumVerts, SrcNumTris, InstanceTransforms.Num());

	FString OriginalMeshPath = GetPathNameSafe(SourceMesh);
	UObject* DupObj = UEditorAssetLibrary::DuplicateAsset(OriginalMeshPath, DuplicateMeshPath);
	UStaticMesh* NewMesh = Cast<UStaticMesh>(DupObj);
	if (!NewMesh) return;

	FMeshDescription MergedDesc;
	FStaticMeshAttributes MergedAttrs(MergedDesc);
	MergedAttrs.Register();

	TVertexAttributesRef<FVector3f> MergedPositions = MergedAttrs.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> MergedNormals = MergedAttrs.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector2f> MergedUVs = MergedAttrs.GetVertexInstanceUVs();
	TVertexInstanceAttributesRef<FVector4f> MergedColors = MergedAttrs.GetVertexInstanceColors();

	const float HalfCapture = InCSSWActor->CaptureSize * 0.5f;
	const float InvCapture = InCSSWActor->CaptureSize > 0.f ? 1.f / InCSSWActor->CaptureSize : 0.f;
	const float ActorLocationZ = InCSSWActor->GetActorLocation().Z;
	constexpr float DryThreshold = -9000.0f;
	constexpr float VelocityMax = (float)CSSW_VELOCITY_CLAMP;

	auto SRGBToLinear = [](float S) -> float
	{
		return S <= 0.04045f ? S / 12.92f : FMath::Pow((S + 0.055f) / 1.055f, 2.4f);
	};

	int32 DbgTotalTris = 0, DbgDrySkipped = 0, DbgDegenerateSkipped = 0, DbgCreated = 0;
	FPolygonGroupID DefaultGroup = MergedDesc.CreatePolygonGroup();

	constexpr float WeldThreshold = 0.01f;
	TMap<FIntVector, FVertexID> WeldMap;

	auto TrySampleBakedLocalHeight = [&](const FVector3f& LocalPos3f, float& OutLocalHeight) -> bool
	{
		float U = (LocalPos3f.X + HalfCapture) * InvCapture;
		float V = (LocalPos3f.Y + HalfCapture) * InvCapture;
		int32 PX = FMath::Clamp(FMath::FloorToInt32(U * RTWidth), 0, RTWidth - 1);
		int32 PY = FMath::Clamp(FMath::FloorToInt32(V * RTHeight), 0, RTHeight - 1);
		float ResultWorldHeight = Pixels[PY * RTWidth + PX].B.GetFloat();
		if (ResultWorldHeight <= DryThreshold)
		{
			return false;
		}

		OutLocalHeight = ResultWorldHeight;
		return true;
	};

	auto GetOrCreateWeldedVertex = [&](const FVector3f& LocalPos3f, float LocalHeight) -> FVertexID
	{
		FIntVector Key(
			FMath::RoundToInt32(LocalPos3f.X / WeldThreshold),
			FMath::RoundToInt32(LocalPos3f.Y / WeldThreshold),
			0);
		if (FVertexID* Found = WeldMap.Find(Key))
		{
			return *Found;
		}

		FVertexID NewVID = MergedDesc.CreateVertex();
		MergedPositions[NewVID] = FVector3f(LocalPos3f.X, LocalPos3f.Y, LocalHeight);
		WeldMap.Add(Key, NewVID);
		return NewVID;
	};

	int32 DbgDryInstancesSkipped = 0;

	// Precompute the source mesh XY bounds so we can map each instance's footprint into the
	// result texture and skip instances that contain no wet content at all.
	FBox2f SrcBoundsXY(ForceInit);
	for (int32 i = 0; i < SrcNumVerts; i++)
	{
		SrcBoundsXY += FVector2f(SrcPositions[i].X, SrcPositions[i].Y);
	}

	auto InstanceHasWetContent = [&](const FTransform& InstT) -> bool
	{
		// Build the instance's world-space XY footprint from the source mesh bounds corners.
		const FVector2f Corners[4] = {
			FVector2f(SrcBoundsXY.Min.X, SrcBoundsXY.Min.Y),
			FVector2f(SrcBoundsXY.Max.X, SrcBoundsXY.Min.Y),
			FVector2f(SrcBoundsXY.Min.X, SrcBoundsXY.Max.Y),
			FVector2f(SrcBoundsXY.Max.X, SrcBoundsXY.Max.Y),
		};
		float MinU = 1.f, MinV = 1.f, MaxU = 0.f, MaxV = 0.f;
		for (const FVector2f& C : Corners)
		{
			const FVector W = InstT.TransformPosition(FVector(C.X, C.Y, 0.f));
			const float U = ((float)W.X + HalfCapture) * InvCapture;
			const float V = ((float)W.Y + HalfCapture) * InvCapture;
			MinU = FMath::Min(MinU, U); MaxU = FMath::Max(MaxU, U);
			MinV = FMath::Min(MinV, V); MaxV = FMath::Max(MaxV, V);
		}
		const int32 PX0 = FMath::Clamp(FMath::FloorToInt32(MinU * RTWidth), 0, RTWidth - 1);
		const int32 PX1 = FMath::Clamp(FMath::FloorToInt32(MaxU * RTWidth), 0, RTWidth - 1);
		const int32 PY0 = FMath::Clamp(FMath::FloorToInt32(MinV * RTHeight), 0, RTHeight - 1);
		const int32 PY1 = FMath::Clamp(FMath::FloorToInt32(MaxV * RTHeight), 0, RTHeight - 1);
		for (int32 PY = PY0; PY <= PY1; PY++)
		{
			for (int32 PX = PX0; PX <= PX1; PX++)
			{
				if (Pixels[PY * RTWidth + PX].B.GetFloat() > DryThreshold) return true;
			}
		}
		return false;
	};

	for (const FTransform& InstT : InstanceTransforms)
	{
		// Skip whole instances that have no wet content; only baked tiles with water survive.
		if (!InstanceHasWetContent(InstT)) { DbgDryInstancesSkipped++; continue; }

		for (int32 Tri = 0; Tri < SrcNumTris; Tri++)
		{
			DbgTotalTris++;
			const uint32 I0 = SrcIndices[Tri * 3 + 0];
			const uint32 I1 = SrcIndices[Tri * 3 + 1];
			const uint32 I2 = SrcIndices[Tri * 3 + 2];

			FVector3f WP0 = FVector3f(InstT.TransformPosition(FVector(SrcPositions[I0])));
			FVector3f WP1 = FVector3f(InstT.TransformPosition(FVector(SrcPositions[I1])));
			FVector3f WP2 = FVector3f(InstT.TransformPosition(FVector(SrcPositions[I2])));

			FVector3f WorldVerts[3] = { WP0, WP1, WP2 };
			float LocalHeights[3] = {};
			bool bWetVerts[3] = {};
			int32 WetVertCount = 0;
			float WetHeightSum = 0.0f;
			for (int32 v = 0; v < 3; v++)
			{
				if (TrySampleBakedLocalHeight(WorldVerts[v], LocalHeights[v]))
				{
					bWetVerts[v] = true;
					WetVertCount++;
					WetHeightSum += LocalHeights[v];
				}
			}

			const FVector3f TriCenter = (WorldVerts[0] + WorldVerts[1] + WorldVerts[2]) / 3.0f;
			float CenterLocalHeight = 0.0f;
			const bool bCenterWet = TrySampleBakedLocalHeight(TriCenter, CenterLocalHeight);
			if (WetVertCount == 0 && !bCenterWet)
			{
				DbgDrySkipped++;
				continue;
			}

			const float FallbackLocalHeight = bCenterWet ? CenterLocalHeight : WetHeightSum / FMath::Max(WetVertCount, 1);
			for (int32 v = 0; v < 3; v++)
			{
				if (!bWetVerts[v])
					LocalHeights[v] = FallbackLocalHeight;
			}

			if (DepthWetPixels.Num() > 0 && DWWidth > 0 && DWHeight > 0)
			{
				bool bAllTransparent = true;
				for (int32 v = 0; v < 3; v++)
				{
					float U = (WorldVerts[v].X + HalfCapture) * InvCapture;
					float V = (WorldVerts[v].Y + HalfCapture) * InvCapture;
					int32 DPX = FMath::Clamp(FMath::FloorToInt32(U * DWWidth), 0, DWWidth - 1);
					int32 DPY = FMath::Clamp(FMath::FloorToInt32(V * DWHeight), 0, DWHeight - 1);
					if (DepthWetPixels[DPY * DWWidth + DPX].A.GetFloat() > 0.f)
					{
						bAllTransparent = false;
						break;
					}
				}
				if (bAllTransparent)
				{
					DbgDrySkipped++;
					continue;
				}
			}

			uint32 SrcIdx[3] = { I0, I1, I2 };
			FVertexID NewVIDs[3];
			TArray<FVertexInstanceID> NewPolyVerts;
			NewPolyVerts.Reserve(3);
			TSet<FVertexID> UniqueVerts;

			for (int32 v = 0; v < 3; v++)
			{
				FVertexID NewVID = GetOrCreateWeldedVertex(WorldVerts[v], LocalHeights[v]);
				NewVIDs[v] = NewVID;
				UniqueVerts.Add(NewVID);

				FVertexInstanceID NewVIID = MergedDesc.CreateVertexInstance(NewVID);
				MergedNormals.Set(NewVIID, SrcNormals[SrcIdx[v]]);
				MergedUVs.Set(NewVIID, 0, SrcUVs[SrcIdx[v]]);

				float U = (WorldVerts[v].X + HalfCapture) * InvCapture;
				float V = (WorldVerts[v].Y + HalfCapture) * InvCapture;
				int32 PX = FMath::Clamp(FMath::FloorToInt32(U * RTWidth), 0, RTWidth - 1);
				int32 PY = FMath::Clamp(FMath::FloorToInt32(V * RTHeight), 0, RTHeight - 1);
				const FFloat16Color& Px = Pixels[PY * RTWidth + PX];

				float NormVelX = FMath::Clamp(Px.R.GetFloat() / VelocityMax * 0.5f + 0.5f, 0.f, 1.f);
				float NormVelY = FMath::Clamp(Px.G.GetFloat() / VelocityMax * 0.5f + 0.5f, 0.f, 1.f);
				float Foam = FMath::Clamp(Px.A.GetFloat(), 0.f, 1.f);

				float DepthWetA = 0.f;
				if (DepthWetPixels.Num() > 0 && DWWidth > 0 && DWHeight > 0)
				{
					int32 DPX = FMath::Clamp(FMath::FloorToInt32(U * DWWidth), 0, DWWidth - 1);
					int32 DPY = FMath::Clamp(FMath::FloorToInt32(V * DWHeight), 0, DWHeight - 1);
					DepthWetA = FMath::Clamp(DepthWetPixels[DPY * DWWidth + DPX].A.GetFloat(), 0.f, 1.f);
				}
				MergedColors.Set(NewVIID, FVector4f(
					SRGBToLinear(NormVelX),
					SRGBToLinear(NormVelY),
					SRGBToLinear(DepthWetA),
					Foam));

				NewPolyVerts.Add(NewVIID);
			}

			if (UniqueVerts.Num() >= 3)
			{
				MergedDesc.CreatePolygon(DefaultGroup, NewPolyVerts);
				DbgCreated++;
			}
			else
			{
				DbgDegenerateSkipped++;
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[CSSW] Bake: Total=%d, DryInstSkip=%d, DrySkip=%d, DegenSkip=%d, Created=%d, Instances=%d"),
		DbgTotalTris, DbgDryInstancesSkipped, DbgDrySkipped, DbgDegenerateSkipped, DbgCreated, InstanceTransforms.Num());

	if (DbgCreated == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] No triangles created, skipping mesh output."));
		NewMesh->ConditionalBeginDestroy();
		UEditorAssetLibrary::DeleteAsset(DuplicateMeshPath);
		return;
	}

	FStaticMeshOperations::ComputeTriangleTangentsAndNormals(MergedDesc, 0.0f, *MeshAssetName);
	FStaticMeshOperations::ComputeTangentsAndNormals(
		MergedDesc,
		EComputeNTBsFlags::Normals
		| EComputeNTBsFlags::Tangents
		| EComputeNTBsFlags::UseMikkTSpace
		| EComputeNTBsFlags::BlendOverlappingNormals
		| EComputeNTBsFlags::WeightedNTBs
		| EComputeNTBsFlags::IgnoreDegenerateTriangles);

	FMeshDescription* TargetDesc = NewMesh->GetMeshDescription(0);
	*TargetDesc = MoveTemp(MergedDesc);

	NewMesh->CommitMeshDescription(0);

	UMaterialInterface* BakeMaterial = InCSSWActor->WaterMaterial;
	if (BakeMaterial)
	{
		FString VelHeightTexName = FString::Printf(TEXT("T_CSSW_VelHeight_%d"), ActorId);
		UTexture2D* VelHeightTex = InCSSWActor->RT_ResultVelHeight
			? UCSAssetProcess::ConveretAndSaveRTAsset(VelHeightTexName, AssetFolderPath, InCSSWActor->RT_ResultVelHeight)
			: nullptr;

		FString DepthWetTexName = FString::Printf(TEXT("T_CSSW_DepthWet_%d"), ActorId);
		UTexture2D* DepthWetTex = InCSSWActor->RT_ResultDepthWet
			? UCSAssetProcess::ConveretAndSaveRTAsset(DepthWetTexName, AssetFolderPath, InCSSWActor->RT_ResultDepthWet)
			: nullptr;

		FString WaterMIName = FString::Printf(TEXT("MI_CSSW_Water_%d"), ActorId);
		UMaterialInstance* WaterMI = UCSAssetProcess::FindOrCreateMaterialInstanceAsset(WaterMIName, AssetFolderPath, BakeMaterial);
		UMaterialInstanceConstant* WaterMIC = Cast<UMaterialInstanceConstant>(WaterMI);
		if (WaterMIC)
		{
			if (VelHeightTex) UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(WaterMIC, FName("CSSW_VelHeight"), VelHeightTex);
			if (DepthWetTex) UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(WaterMIC, FName("CSSW_DepthWet"), DepthWetTex);
			UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(WaterMIC, FName("CSSW_SimCenter"),
				FLinearColor(InCSSWActor->SimUVCenter.X, InCSSWActor->SimUVCenter.Y, 0, 0));
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(WaterMIC, FName("CSSW_SimInvSize"), InCSSWActor->SimUVInvSize);
			UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(WaterMIC, FName("SwithSim"), true);
			WaterMIC->PostEditChange();
			UEditorAssetLibrary::SaveLoadedAsset(WaterMIC, false);
			BakeMaterial = WaterMIC;
		}

		TArray<FStaticMaterial>& Materials = NewMesh->GetStaticMaterials();
		if (Materials.Num() > 0)
			Materials[0].MaterialInterface = BakeMaterial;
		else
			Materials.Add(FStaticMaterial(BakeMaterial, TEXT("Water")));
	}

	NewMesh->Build(false);
	NewMesh->PostEditChange();
	UEditorAssetLibrary::SaveLoadedAsset(NewMesh, false);
	NewMesh->MarkPackageDirty();

	if (InCSSWActor->ReusltMesh)
	{
		InCSSWActor->Modify();
		InCSSWActor->ReusltMesh->Modify();

		if (!InCSSWActor->SimulationPreviewMesh)
			InCSSWActor->SimulationPreviewMesh = InCSSWActor->ReusltMesh->GetStaticMesh();

		InCSSWActor->BakedResultMesh = NewMesh;
		InCSSWActor->bUseBakedResultMesh = true;

		InCSSWActor->ReusltMesh->SetStaticMesh(NewMesh);
		InCSSWActor->ReusltMesh->SetRelativeScale3D(FVector::OneVector);
		if (BakeMaterial)
			InCSSWActor->ReusltMesh->SetMaterial(0, BakeMaterial);
		InCSSWActor->ReusltMesh->MarkRenderStateDirty();
	}
}

ACSShallowWaterCapture* UCSShallowWaterProcess::CSSW_GetSelectActor()
{
	ACSShallowWaterCapture* SelectedCapture = nullptr;
	USelection* SelectedActors = GEditor->GetSelectedActors();
	if (SelectedActors == nullptr || SelectedActors->Num() == 0) return SelectedCapture;
	AActor* SelectedActor = Cast<AActor>(SelectedActors->GetSelectedObject(0));

	if (SelectedActor == nullptr) return SelectedCapture;
	
	SelectedCapture = Cast<ACSShallowWaterCapture>(SelectedActor);
	if (SelectedCapture != nullptr) return SelectedCapture;

	AActor* ParentActor = SelectedActor->GetAttachParentActor();
	SelectedCapture = Cast<ACSShallowWaterCapture>(ParentActor);
	if (SelectedCapture != nullptr) return SelectedCapture;

	ACSSHallowWaterSource* SelectedSource = Cast<ACSSHallowWaterSource>(SelectedActor);
	if (SelectedSource == nullptr) return SelectedCapture;

	TArray<TEnumAsByte<EObjectTypeQuery>> ObjectTypes = {
		UEngineTypes::ConvertToObjectType(ECollisionChannel::ECC_WorldStatic),
		UEngineTypes::ConvertToObjectType(ECollisionChannel::ECC_WorldDynamic)
	};
	FVector BoxExtent = FVector(5, 5, 100000);
	UWorld* World = SelectedSource->GetWorld();
	if (!World) return SelectedCapture;

	TArray<AActor*> OverlapActors;
	TArray<AActor*> ActorsToIgnore;
	UKismetSystemLibrary::BoxOverlapActors(World, SelectedSource->GetActorLocation(), BoxExtent,
	                                       ObjectTypes, ACSShallowWaterCapture::StaticClass(), ActorsToIgnore,
	                                       OverlapActors);
	DrawDebugBox(World, SelectedSource->GetActorLocation(), BoxExtent, FColor(1, 1, 1, 1), false, 1, 0, 1);
	if (OverlapActors.Num() == 0) return SelectedCapture;
	SelectedCapture = Cast<ACSShallowWaterCapture>(OverlapActors[0]);

	return SelectedCapture;
}

void UCSShallowWaterProcess::MaterialTest(ACSShallowWaterCapture* InCSSWActor)
{
	if (InCSSWActor == nullptr) return;
	
	UStaticMesh* VisMesh = InCSSWActor->ReusltMesh->GetStaticMesh(); 
	if (VisMesh == nullptr) return;

	FName CSSWTag = FName("CSSW_Bake");
	TArray<AActor*> AttachedActors; 
	InCSSWActor->GetAttachedActors(AttachedActors);
	for (AActor* AttachedActor : AttachedActors)
	{
		if (AttachedActor->Tags.Contains(CSSWTag)) AttachedActor->Destroy();
	}
	
	ULevel* CurrentLevel = InCSSWActor->GetLevel();
	FString LevelPathName = GetPathNameSafe(CurrentLevel);
	FString FileName;
	FString LevelPath;
	FString Extension;
	FPaths::Split(LevelPathName, LevelPath, FileName, Extension);
	FString AssetFolderPath = LevelPath.Append("/CSSWData");

	FDateTime Time = FDateTime::Now();
	FString ResultVelHeightName = FString::Printf(TEXT("T_CSSW_VelHeight_%d%d%d"), Time.GetHour(), Time.GetMinute(), Time.GetSecond());
	FString ResultWaterMaterialName = FString::Printf(TEXT("MI_CSSW_Water_%d%d%d"), Time.GetHour(), Time.GetMinute(), Time.GetSecond());
	FString VelHeightAssetPathFull = AssetFolderPath + "/" + ResultVelHeightName + "." + ResultVelHeightName;
	
	FString WaterMaterialPath = GetPathNameSafe(InCSSWActor->WaterMaterial);
	UCSAssetProcess::FindOrDuplicateMaterialInstanceAsset(WaterMaterialPath, VelHeightAssetPathFull, true);
}

void UCSShallowWaterProcess::DebugDumpSWPassResults(ACSShallowWaterCapture* InCSSWActor)
{
	if (InCSSWActor == nullptr)
	{
		UE_LOG(LogTemp, Error, TEXT("DebugDumpSWPassResults: InCSSWActor is null."));
		return;
	}

	ULevel* CurrentLevel = InCSSWActor->GetLevel();
	FString LevelPathName = GetPathNameSafe(CurrentLevel);
	FString FileName;
	FString LevelPath;
	FString Extension;
	FPaths::Split(LevelPathName, LevelPath, FileName, Extension);
	FString DebugFolderPath = LevelPath / TEXT("debug");

	struct FRTEntry
	{
		UTextureRenderTarget2D* RT;
		FString Name;
	};

	TArray<FRTEntry> RTsToSave = {
		{ InCSSWActor->RT_VelocityHeight,   TEXT("Debug_VelocityHeight") },
		{ InCSSWActor->RT_ResultVelHeight,   TEXT("Debug_ResultVelHeight") },
		{ InCSSWActor->RT_ResultDepthWet,    TEXT("Debug_ResultDepthWet") },
		{ InCSSWActor->RT_SmoothHeight,      TEXT("Debug_SmoothHeight") },
		{ InCSSWActor->RT_DebugView,         TEXT("Debug_DebugView") },
	};

	int32 SavedCount = 0;
	for (const FRTEntry& Entry : RTsToSave)
	{
		if (Entry.RT == nullptr)
		{
			UE_LOG(LogTemp, Warning, TEXT("DebugDumpSWPassResults: %s is null, skipping."), *Entry.Name);
			continue;
		}
		UTexture2D* SavedTexture = UCSAssetProcess::ConveretAndSaveRTAsset(Entry.Name, DebugFolderPath, Entry.RT);
		if (SavedTexture)
		{
			UEditorAssetLibrary::SaveLoadedAsset(SavedTexture, false);
			SavedCount++;
			UE_LOG(LogTemp, Log, TEXT("DebugDumpSWPassResults: Saved %s -> %s/%s"), *Entry.Name, *DebugFolderPath, *Entry.Name);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("DebugDumpSWPassResults: Failed to save %s"), *Entry.Name);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("DebugDumpSWPassResults: Done. Saved %d/%d textures to %s"), SavedCount, RTsToSave.Num(), *DebugFolderPath);
}

bool UCSShallowWaterProcess::StartSWSolver(ACSShallowWaterCapture*& OutCSSWActor,
	int32 Iteration, float TimerRate)
{
	OutCSSWActor = CSSW_GetSelectActor();
	if (!OutCSSWActor)
	{
		UE_LOG(LogTemp, Warning, TEXT("StartSWSolver: No CSSW actor selected."));
		return false;
	}

	OutCSSWActor->StartSolver(TimerRate, Iteration);

	return true;
}

void UCSShallowWaterProcess::StopSWSolver(ACSShallowWaterCapture* InCSSWActor)
{
	if (!InCSSWActor) return;

	InCSSWActor->StopSolver();
}
