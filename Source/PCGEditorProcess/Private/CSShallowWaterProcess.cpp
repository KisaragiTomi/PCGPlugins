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

	FName CSSWTag = InCSSWActor->SWTag;

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
	FString ResultVelHeightName = FString::Printf(TEXT("T_CSSW_VelHeight_%d"), ActorId);
	FString ResultDepthWetName = FString::Printf(TEXT("T_CSSW_DepthWet_%d"), ActorId);
	FString ResultWaterMaterialName = FString::Printf(TEXT("MI_CSSW_Water_%d"), ActorId);
	FString ResultDecalMaterialName = FString::Printf(TEXT("MI_CSSW_Decal_%d"), ActorId);
	FString ResultWaterMaterialPathFull = AssetFolderPath + "/" + ResultWaterMaterialName + "." + ResultWaterMaterialName;
	FString ResultDecalMaterialPathFull = AssetFolderPath + "/" + ResultDecalMaterialName + "." + ResultDecalMaterialName;

	UTexture2D* VelHeightTexture = UCSAssetProcess::ConveretAndSaveRTAsset(ResultVelHeightName, AssetFolderPath, InCSSWActor->RT_ResultVelHeight);
	UTexture2D* DepthWetTexture = UCSAssetProcess::ConveretAndSaveRTAsset(ResultDepthWetName, AssetFolderPath, InCSSWActor->RT_ResultDepthWet);

	auto FindOrCreateChildMIC = [&](UMaterialInterface* ParentMat, const FString& MIName) -> UMaterialInstanceConstant*
	{
		if (!ParentMat) return nullptr;
		FString MIPath = AssetFolderPath / MIName;
		UMaterialInstanceConstant* MIC = nullptr;
		if (UEditorAssetLibrary::DoesAssetExist(MIPath))
		{
			MIC = Cast<UMaterialInstanceConstant>(UEditorAssetLibrary::LoadAsset(MIPath));
		}
		if (!MIC)
		{
			UPackage* Pkg = CreatePackage(*(MIPath));
			Pkg->FullyLoad();
			MIC = NewObject<UMaterialInstanceConstant>(Pkg, *MIName, RF_Public | RF_Standalone);
			FAssetRegistryModule::AssetCreated(MIC);
		}
		UMaterialEditingLibrary::SetMaterialInstanceParent(MIC, ParentMat);
		return MIC;
	};

	UMaterialInstanceConstant* WaterMIC = FindOrCreateChildMIC(InCSSWActor->WaterMaterial, ResultWaterMaterialName);
	UMaterialInstanceConstant* DecalMIC = FindOrCreateChildMIC(InCSSWActor->DecalMaterial, ResultDecalMaterialName);

	if (WaterMIC)
	{
		if (VelHeightTexture)
			UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(WaterMIC, TEXT("CSSW_VelHeight"), VelHeightTexture);
		if (DepthWetTexture)
			UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(WaterMIC, TEXT("CSSW_DepthWet"), DepthWetTexture);
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(WaterMIC, TEXT("SwitchSim"), true);
		UEditorAssetLibrary::SaveLoadedAsset(WaterMIC, false);
		WaterMIC->MarkPackageDirty();
	}
	if (DecalMIC)
	{
		if (VelHeightTexture)
			UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(DecalMIC, TEXT("CSSW_VelHeight"), VelHeightTexture);
		if (DepthWetTexture)
			UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(DecalMIC, TEXT("CSSW_DepthWet"), DepthWetTexture);
		UEditorAssetLibrary::SaveLoadedAsset(DecalMIC, false);
		DecalMIC->MarkPackageDirty();
	}
	if (VelHeightTexture) { UEditorAssetLibrary::SaveLoadedAsset(VelHeightTexture, false); VelHeightTexture->MarkPackageDirty(); }
	if (DepthWetTexture) { UEditorAssetLibrary::SaveLoadedAsset(DepthWetTexture, false); DepthWetTexture->MarkPackageDirty(); }

	const int32 RTWidth  = InCSSWActor->CachedResultWidth;
	const int32 RTHeight = InCSSWActor->CachedResultHeight;
	if (RTWidth <= 0 || RTHeight <= 0 || InCSSWActor->CachedResultPixels.Num() != RTWidth * RTHeight)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] No cached result data available for bake. Run simulation first."));
		return;
	}
	const TArray<FFloat16Color>& Pixels = InCSSWActor->CachedResultPixels;

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

	auto GetOrCreateWeldedVertex = [&](const FVector3f& WorldPos3f) -> FVertexID
	{
		FIntVector Key(
			FMath::RoundToInt32(WorldPos3f.X / WeldThreshold),
			FMath::RoundToInt32(WorldPos3f.Y / WeldThreshold),
			0);
		if (FVertexID* Found = WeldMap.Find(Key))
		{
			return *Found;
		}

		float U = (WorldPos3f.X + HalfCapture) * InvCapture;
		float V = (WorldPos3f.Y + HalfCapture) * InvCapture;
		int32 PX = FMath::Clamp(FMath::FloorToInt32(U * RTWidth), 0, RTWidth - 1);
		int32 PY = FMath::Clamp(FMath::FloorToInt32(V * RTHeight), 0, RTHeight - 1);
		float HeightOffset = Pixels[PY * RTWidth + PX].B.GetFloat();

		FVertexID NewVID = MergedDesc.CreateVertex();
		MergedPositions[NewVID] = FVector3f(WorldPos3f.X, WorldPos3f.Y, WorldPos3f.Z + HeightOffset);
		WeldMap.Add(Key, NewVID);
		return NewVID;
	};

	for (const FTransform& InstT : InstanceTransforms)
	{
		for (int32 Tri = 0; Tri < SrcNumTris; Tri++)
		{
			DbgTotalTris++;
			const uint32 I0 = SrcIndices[Tri * 3 + 0];
			const uint32 I1 = SrcIndices[Tri * 3 + 1];
			const uint32 I2 = SrcIndices[Tri * 3 + 2];

			FVector3f WP0 = FVector3f(InstT.TransformPosition(FVector(SrcPositions[I0])));
			FVector3f WP1 = FVector3f(InstT.TransformPosition(FVector(SrcPositions[I1])));
			FVector3f WP2 = FVector3f(InstT.TransformPosition(FVector(SrcPositions[I2])));

			bool bAllDry = true;
			FVector3f WorldVerts[3] = { WP0, WP1, WP2 };
			for (int32 v = 0; v < 3; v++)
			{
				float U = (WorldVerts[v].X + HalfCapture) * InvCapture;
				float V = (WorldVerts[v].Y + HalfCapture) * InvCapture;
				int32 PX = FMath::Clamp(FMath::FloorToInt32(U * RTWidth), 0, RTWidth - 1);
				int32 PY = FMath::Clamp(FMath::FloorToInt32(V * RTHeight), 0, RTHeight - 1);
				float H = Pixels[PY * RTWidth + PX].B.GetFloat();
				if (H > DryThreshold) { bAllDry = false; break; }
			}
			if (bAllDry) { DbgDrySkipped++; continue; }

			uint32 SrcIdx[3] = { I0, I1, I2 };
			FVertexID NewVIDs[3];
			TArray<FVertexInstanceID> NewPolyVerts;
			NewPolyVerts.Reserve(3);
			TSet<FVertexID> UniqueVerts;

			for (int32 v = 0; v < 3; v++)
			{
				FVertexID NewVID = GetOrCreateWeldedVertex(WorldVerts[v]);
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

	UE_LOG(LogTemp, Log, TEXT("[CSSW] Bake: Total=%d, DrySkip=%d, DegenSkip=%d, Created=%d, Instances=%d"),
		DbgTotalTris, DbgDrySkipped, DbgDegenerateSkipped, DbgCreated, InstanceTransforms.Num());

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

	if (WaterMIC)
	{
		TArray<FStaticMaterial>& Materials = NewMesh->GetStaticMaterials();
		if (Materials.Num() > 0)
			Materials[0].MaterialInterface = WaterMIC;
		else
			Materials.Add(FStaticMaterial(WaterMIC, TEXT("Water")));
	}

	NewMesh->Build(false);
	NewMesh->PostEditChange();
	UEditorAssetLibrary::SaveLoadedAsset(NewMesh, false);
	NewMesh->MarkPackageDirty();

	InCSSWActor->Modify();
	if (InCSSWActor->ReusltMesh)
	{
		InCSSWActor->ReusltMesh->Modify();
		if (!InCSSWActor->SimulationPreviewMesh)
		{
			InCSSWActor->SimulationPreviewMesh = InCSSWActor->ReusltMesh->GetStaticMesh();
		}
		InCSSWActor->ReusltMesh->SetStaticMesh(NewMesh);
		InCSSWActor->ReusltMesh->SetRelativeScale3D(FVector::OneVector);
		if (WaterMIC)
		{
			InCSSWActor->ReusltMesh->SetMaterial(0, WaterMIC);
		}
		InCSSWActor->ReusltMesh->SetVisibility(true);
		InCSSWActor->ReusltMesh->MarkRenderStateDirty();
	}
	if (!InCSSWActor->SimulationWaterMaterial)
	{
		InCSSWActor->SimulationWaterMaterial = InCSSWActor->WaterMaterial;
	}
	if (!InCSSWActor->SimulationDecalMaterial)
	{
		InCSSWActor->SimulationDecalMaterial = InCSSWActor->DecalMaterial;
	}
	InCSSWActor->BakedResultMesh = NewMesh;
	InCSSWActor->bUseBakedResultMesh = true;
	InCSSWActor->WaterMaterial = WaterMIC ? WaterMIC : InCSSWActor->WaterMaterial;
	InCSSWActor->DecalMaterial = DecalMIC ? DecalMIC : InCSSWActor->DecalMaterial;
	InCSSWActor->StopSolver();
	InCSSWActor->bSimVisActive = false;
	if (InCSSWActor->SimVisHISM)
	{
		InCSSWActor->SimVisHISM->ClearInstances();
		InCSSWActor->SimVisHISM->SetVisibility(false);
	}
	InCSSWActor->MarkPackageDirty();
	if (InCSSWActor->ReusltMesh)
	{
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
		{ InCSSWActor->RT_SceneDepth,       TEXT("Debug_SceneDepth") },
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

	OutCSSWActor->Iteration = Iteration;
	OutCSSWActor->StartSolver(TimerRate);

	return true;
}

void UCSShallowWaterProcess::StopSWSolver(ACSShallowWaterCapture* InCSSWActor)
{
	if (!InCSSWActor) return;

	InCSSWActor->StopSolver();
}
