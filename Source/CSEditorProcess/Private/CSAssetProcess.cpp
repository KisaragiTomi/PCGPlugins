// Fill out your copyright notice in the Description page of Project Settings.


#include "CSAssetProcess.h"

#include "ClearQuad.h"
#include "GlobalShader.h"
#include "MaterialShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderGraphBuilder.h"
#include "RenderTargetPool.h"
#include "ComputeShaderGenerateHepler.h"
#include "EngineUtils.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/StaticMeshActor.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "ComputeShaderGeneral.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryHelpers.h"
#include "Engine/SceneCapture.h"
#include "Engine/SceneCapture2D.h"
#include "SceneRendererInterface.h"
#include "Kismet/KismetSystemLibrary.h"
#include "ComputeShaderSceneCapture.h"
#include "EditorAssetLibrary.h"
#include "Materials/MaterialInstanceConstant.h"


#include "Engine/StaticMeshActor.h"
// #include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "ComputeShaderBasicFunction.h"
#include "MaterialUtilities.h"
#include "ModuleDescriptor.h"
#include "Selection.h"
#include "Editor/MaterialEditor/Public/MaterialEditingLibrary.h"
#include "Engine/DecalActor.h"
#include "Kismet/KismetMathLibrary.h"
#include "Runtime/Experimental/Voronoi/Private/voro++/src/container.hh"
#include "Subsystems/EditorAssetSubsystem.h"


class UEditorAssetSubsystem;
class FAssetRegistryModule;
class ISceneRenderer;

void UCSAssetProcess::CaptureMeshHeight(AStaticMeshActor* InMeshActor, UTextureRenderTarget2D*& OutRenderTarget2D, TArray<AActor*>& OutActors, int32 InTextureSize)
{
	if (InMeshActor == nullptr) return;
	UStaticMesh* ContainerMesh = InMeshActor->GetStaticMeshComponent()->GetStaticMesh();
	if (ContainerMesh == nullptr) return;

	FBoxSphereBounds Bounds = ContainerMesh->GetBounds();

	float MaxHeight = 10000;

	UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	FActorSpawnParameters SpawnParameters;
	AStaticMeshActor *SpawnMeshTargetMesh = GWorld->SpawnActor<AStaticMeshActor>(SpawnParameters);
	AStaticMeshActor *SpawnMeshPlane = GWorld->SpawnActor<AStaticMeshActor>(SpawnParameters);
	SpawnMeshTargetMesh->GetStaticMeshComponent()->SetStaticMesh(ContainerMesh);
	SpawnMeshPlane->GetStaticMeshComponent()->SetStaticMesh(PlaneMesh);
	SpawnMeshPlane->SetActorScale3D(FVector(9999, 9999, 1));
	TArray<AActor*> TargetActors;
	TargetActors.Add(SpawnMeshPlane);
	TargetActors.Add(SpawnMeshTargetMesh);
	
	
	UTextureRenderTarget2D* NewRenderTarget2D = NewObject<UTextureRenderTarget2D>();
	check(NewRenderTarget2D);
	NewRenderTarget2D->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA16f;
	NewRenderTarget2D->ClearColor = FLinearColor::Black;
	NewRenderTarget2D->bAutoGenerateMips = false;
	NewRenderTarget2D->bCanCreateUAV = true;
	NewRenderTarget2D->InitAutoFormat(InTextureSize, InTextureSize);
	
	ASceneCapture2D * CaptureTarget = GWorld->SpawnActor<ASceneCapture2D>(SpawnParameters);
	FTransform CaptureTransform(FRotator(0, -90, -90), FVector(Bounds.Origin.X, Bounds.Origin.Y, MaxHeight), FVector::OneVector);
	
	
	CaptureTarget->SetActorTransform(CaptureTransform);
	CaptureTarget->GetCaptureComponent2D()->TextureTarget = NewRenderTarget2D;
	CaptureTarget->GetCaptureComponent2D()->OrthoWidth = FMath::Max(Bounds.BoxExtent.X, Bounds.BoxExtent.Y) * 2;
	CaptureTarget->GetCaptureComponent2D()->ShowOnlyActors = TargetActors;
	CaptureTarget->GetCaptureComponent2D()->ProjectionType = ECameraProjectionMode::Orthographic;
	CaptureTarget->GetCaptureComponent2D()->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	CaptureTarget->GetCaptureComponent2D()->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;

	OutActors.Add(SpawnMeshPlane);
	OutActors.Add(SpawnMeshTargetMesh);
	OutActors.Add(CaptureTarget);
}

void UCSAssetProcess::CalculateMeshHeight(AStaticMeshActor* InMeshActor,  UTextureRenderTarget2D* NewRenderTarget2D)
{
	
	float MaxHeight = 10000;
	if (InMeshActor == nullptr || NewRenderTarget2D == nullptr) return;
	UStaticMesh* ContainerMesh = InMeshActor->GetStaticMeshComponent()->GetStaticMesh(); 
	if (ContainerMesh == nullptr) return;

	FBoxSphereBounds Bounds = ContainerMesh->GetBounds();
	ULevel* CurrentLevel = InMeshActor->GetLevel();
	FString LevelPathName = GetPathNameSafe(CurrentLevel);
	FString FileName;
	FString LevelPath;
	FString Extension;
	FPaths::Split(LevelPathName, LevelPath, FileName, Extension);
	FString AssetPath = LevelPath.Append("/MeshHeightData");
	IAssetRegistry &AssetRegistry = FModuleManager::GetModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TArray<FAssetData> OutAssetData;
	if (!AssetRegistry.GetAssetsByPath(FName(AssetPath), OutAssetData)) return;
	
	FTextureRenderTargetResource* NewTextureTarget = NewRenderTarget2D->GameThread_GetRenderTargetResource();
	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[NewTextureTarget, MaxHeight](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			TShaderMapRef<FGeneralFunctionShader> ComputeShader = FGeneralFunctionShader::CreateShaderPermutation(FGeneralFunctionShader::EGeneralShader::GTS_ProcessMeshHeightTexture);

			FGeneralFunctionShader::FParameters* PassParameters = GraphBuilder.AllocParameters<FGeneralFunctionShader::FParameters>();
			auto GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(NewTextureTarget->GetSizeXY().X, NewTextureTarget->GetSizeXY().Y, 1), FComputeShaderUtils::kGolden2DGroupSize);
			
			FRDGTextureRef TmpTexture_ProcssTexture = CSHepler::ConvertToUVATexture(NewTextureTarget, GraphBuilder);
			FRDGTextureRef ProcssTexture = RegisterExternalTexture(GraphBuilder, NewTextureTarget->GetRenderTargetTexture(), TEXT("Input_RT"));
			
			PassParameters->T_ProcssTexture0 = ProcssTexture;
			PassParameters->RW_ProcssTexture0 = GraphBuilder.CreateUAV(TmpTexture_ProcssTexture);
			PassParameters->InputData0 = MaxHeight;
			PassParameters->Sampler	= TStaticSamplerState<SF_Bilinear>::GetRHI();

			
			GraphBuilder.AddPass(
				RDG_EVENT_NAME("ExecuteExampleComputeShader"),
				PassParameters,
				ERDGPassFlags::AsyncCompute,
				[&PassParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, GroupCount);
				});
			
			AddCopyTexturePass(GraphBuilder, TmpTexture_ProcssTexture, ProcssTexture, FRHICopyTextureInfo());
		}
		GraphBuilder.Execute();

	});
	
	FlushRenderingCommands();
	
	
	TArray<FAssetData> AssetDatas;
	TArray<UTexture2D*> AssetTextures;
	for (int32 i = 0; i < OutAssetData.Num(); i++)
	{
		UCSMeshAsset* MeshAsset = Cast<UCSMeshAsset>(OutAssetData[i].GetAsset());
		if (MeshAsset != nullptr)
		{
			AssetDatas.Add(MeshAsset);
			continue;
		}

		UTexture2D* Texture = Cast<UTexture2D>(OutAssetData[i].GetAsset());
		if (Texture != nullptr)
		{
			AssetTextures.Add(Texture);
		}
	}

	

	
	FString ContainerMeshPathName = GetPathNameSafe(ContainerMesh);
	FString ContainerMeshFileName;
	FString ContainerMeshPath;
	FString ContainerMeshExtension;
	FPaths::Split(ContainerMeshPathName, ContainerMeshPath, ContainerMeshFileName, ContainerMeshExtension);


	IAssetTools &AssetTools = FModuleManager::GetModuleChecked<FAssetToolsModule>("AssetTools").Get();
	// TScriptInterface<IAssetTools> AssetTools =  UAssetToolsHelpers::GetAssetTools();

	FString TextureAssetName = "T_";
	
	TextureAssetName.Append(ContainerMeshFileName);
	TextureAssetName.Append("_Height");
	
	FString TextureAssetPath = AssetPath.Append("/");
	TextureAssetPath.Append(TextureAssetName);

	UTexture2D* CurrentTextureObject = nullptr;
	bool FindTexture = false;
	if (UEditorAssetLibrary::DoesAssetExist(TextureAssetPath))
	{
		UObject* LoadObject = UEditorAssetLibrary::LoadAsset(TextureAssetPath);
		UTexture2D* Texture = Cast<UTexture2D>(LoadObject);
		if (Texture != nullptr)
		{
			CurrentTextureObject = Texture;
			FindTexture = true;
		}
	}
	if (!FindTexture)
	{
		UObject* NewTextureObject = AssetTools.CreateAsset(TextureAssetName, AssetPath, UTexture2D::StaticClass(), nullptr);
		UTexture2D* NewTexture = Cast<UTexture2D>(NewTextureObject);
		if (NewTexture != nullptr)
		{
			CurrentTextureObject = NewTexture;
		}
	}


	FString MeshAssetName = "MA_";
	
	MeshAssetName.Append(ContainerMeshFileName);
	MeshAssetName.Append("_MeshData");
	
	FString MeshAssetPath = AssetPath.Append("/");
	MeshAssetPath.Append(MeshAssetName);
	
	UCSMeshAsset* CurrentMeshAssetObject = nullptr;
	bool FindMeshAsset = false;
	if (UEditorAssetLibrary::DoesAssetExist(MeshAssetPath))
	{
		UObject* LoadObject = UEditorAssetLibrary::LoadAsset(MeshAssetPath);
		UCSMeshAsset* MeshAsset = Cast<UCSMeshAsset>(LoadObject);
		if (MeshAsset != nullptr)
		{
			CurrentMeshAssetObject = MeshAsset;
			FindMeshAsset = true;
		}
	}
	if (!FindMeshAsset)
	{
		
		UObject* NewMeshAssetObject = AssetTools.CreateAsset(MeshAssetName, AssetPath, UCSMeshAsset::StaticClass(), nullptr);
		UCSMeshAsset* NewMeshAsset = Cast<UCSMeshAsset>(NewMeshAssetObject);
		if (NewMeshAsset != nullptr)
		{
			CurrentMeshAssetObject = NewMeshAsset;
		}
	}
	
	CurrentTextureObject->CompressionNoAlpha = true;
	CurrentTextureObject->MipGenSettings = TMGS_NoMipmaps;
	CurrentTextureObject->SRGB = false;
	CurrentTextureObject->DeferCompression = true;
	CurrentTextureObject->AddressX = TextureAddress::TA_Clamp;
	CurrentTextureObject->AddressY = TextureAddress::TA_Clamp;
	
	UKismetRenderingLibrary::ConvertRenderTargetToTexture2DEditorOnly(GWorld, NewRenderTarget2D, CurrentTextureObject);
	
	CurrentTextureObject->CompressionSettings = TextureCompressionSettings::TC_HDR;
	
	CurrentMeshAssetObject->CSStaticMesh = ContainerMesh;
	CurrentMeshAssetObject->CSMeshHeightTexture = CurrentTextureObject;
	CurrentMeshAssetObject->CSMeshPivot = FVector(-Bounds.Origin.X, -Bounds.Origin.Y, Bounds.BoxExtent.Z - Bounds.Origin.Z);
	CurrentMeshAssetObject->CSMeshMaxHeight = Bounds.BoxExtent.Z * 2;
	CurrentMeshAssetObject->CSMeshSize =  FMath::Max(ContainerMesh->GetBounds().BoxExtent.X, ContainerMesh->GetBounds().BoxExtent.Y) * 2;

	UEditorAssetLibrary::SaveLoadedAsset(CurrentTextureObject, false);
	UEditorAssetLibrary::SaveLoadedAsset(CurrentMeshAssetObject, false);

	
	NewRenderTarget2D = nullptr;
}

void UCSAssetProcess::SaveSWData(ACSShallowWaterCapture* InCSSWActor)
{
	if (InCSSWActor == nullptr) return;
	
	UStaticMesh* VisMesh = InCSSWActor->VisualizeMesh->GetStaticMesh(); 
	if (VisMesh == nullptr) return;

	// InCSSWActor->VisualizeMesh->SetVisibility(false);
	// InCSSWActor->CausticsDecal->SetVisibility(false);

	FName CSSWTag = InCSSWActor->SWTag;

	
	ULevel* CurrentLevel = InCSSWActor->GetLevel();
	FString LevelPathName = GetPathNameSafe(CurrentLevel);
	FString FileName;
	FString LevelPath;
	FString Extension;
	FPaths::Split(LevelPathName, LevelPath, FileName, Extension);
	FString AssetFloderPath = LevelPath.Append("/CSSWData");

	

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
	FString RestultWaterMaterialPathFull = AssetFloderPath + "/" + ResultWaterMaterialName + "." + ResultWaterMaterialName;
	FString RestultDecalMaterialPathFull = AssetFloderPath + "/" + ResultDecalMaterialName + "." + ResultDecalMaterialName;


	UTexture2D* VelHeightTexture = UCSAssetProcess::ConveretAndSaveRTAsset(ResultVelHeightName, AssetFloderPath, InCSSWActor->RT_ResultVelHeight);
	UTexture2D* DepthWetTexture = UCSAssetProcess::ConveretAndSaveRTAsset(ResultDepthWetName, AssetFloderPath, InCSSWActor->RT_ResultDepthWet);
	UMaterialInstance* WaterMaterial = UCSAssetProcess::FindOrDuplicateMaterialInstanceAsset( GetPathNameSafe(InCSSWActor->WaterMaterial), RestultWaterMaterialPathFull);
	UMaterialInstance* DecalMaterial = UCSAssetProcess::FindOrDuplicateMaterialInstanceAsset( GetPathNameSafe(InCSSWActor->DecalMaterial), RestultDecalMaterialPathFull);
	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Cast<UMaterialInstanceConstant>(WaterMaterial), TEXT("CSSW_VelHeight"), VelHeightTexture);
	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Cast<UMaterialInstanceConstant>(WaterMaterial), TEXT("CSSW_DepthWet"), DepthWetTexture);
	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Cast<UMaterialInstanceConstant>(DecalMaterial), TEXT("CSSW_VelHeight"), VelHeightTexture);
	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Cast<UMaterialInstanceConstant>(DecalMaterial), TEXT("CSSW_DepthWet"), DepthWetTexture);
	
	// UEditorAssetLibrary::SaveLoadedAsset(WaterMaterial, false);
	// UEditorAssetLibrary::SaveLoadedAsset(DecalMaterial, false);
	
	// WaterMaterial->GetPackage()->MarkPackageDirty();
	// DecalMaterial->GetPackage()->MarkPackageDirty();
	
	// WaterMaterial->GetPackage()->SetDirtyFlag(true);
	// WaterMaterial->GetPackage()->MarkPackageDirty();

	// bool test = WaterMaterial->GetPackage()->IsDirty();
	// GEditor.broadcasta
	// GEditor->GetEditorSubsystem<UEditorAssetSubsystem>()->SetDirtyFlag(WaterMaterial, true);
	
	// UPackage aa;
	// aa.SetDirtyFlag();
}

UTexture2D* UCSAssetProcess::SaveTextureData(UTextureRenderTarget2D* RenderTarget, FString AssetName)
{
	if (RenderTarget == nullptr) return nullptr;
	
	ULevel* CurrentLevel = GWorld->GetLevel(0);
	FString LevelPathName = GetPathNameSafe(CurrentLevel);
	FString FileName;
	FString LevelPath;
	FString Extension;
	FPaths::Split(LevelPathName, LevelPath, FileName, Extension);
	FString AssetFloderPath = LevelPath.Append("/ResultTexture");

	

	if (AssetName == "")
	{
		FDateTime Time = FDateTime::Now();
		int32 UniqueID = Time.GetDay() * 1e6 + Time.GetHour() * 1e4 + Time.GetMinute() * 1e2 + Time.GetSecond();
		AssetName = FString::Printf(TEXT("T_%d"), UniqueID);
	}
	else
	{
		AssetName = "T_" + AssetName;
	}

	UTexture2D* ResultTexture = UCSAssetProcess::ConveretAndSaveRTAsset(AssetName, AssetFloderPath, RenderTarget);
	return  ResultTexture;
}


template<typename T>
T* UCSAssetProcess::FindOrCreateAsset(FString AssetName, FString AssetFolderPath, TFunction<T*()> Func)
{

	FString AssetPath = AssetFolderPath + "/" + AssetName;
	T* ResultObject = nullptr;
	if (UEditorAssetLibrary::DoesAssetExist(AssetPath))
	{
		ResultObject = Cast<T>(UEditorAssetLibrary::LoadAsset(AssetPath));
		if (ResultObject != nullptr) return ResultObject;
	}
	ResultObject = Func();
	
	return ResultObject;
}

UTexture2D* UCSAssetProcess::ConveretAndSaveRTAsset(FString AssetName, FString AssetFolderPath, UTextureRenderTarget2D* RenderTarget)
{
	TextureCompressionSettings InCompressionSettings = TC_Default;
	UTexture2D* OutTexture = FindOrCreateAsset<UTexture2D>(AssetName, AssetFolderPath,[&]()
	{
		UTexture2D* NewTexture = nullptr;
		EPixelFormat Format = RenderTarget->GetFormat();
		switch (Format)
		{
			//Convert Texture will fail when format is PF_A32B32G32R32F
			case EPixelFormat::PF_A32B32G32R32F:
				InCompressionSettings = TC_HDR_F32;
				break;
			case EPixelFormat::PF_A16B16G16R16:
				InCompressionSettings = TC_HDR;
				break;
			default:
				break;
		}
		
		NewTexture = UKismetRenderingLibrary::RenderTargetCreateStaticTexture2DEditorOnly(RenderTarget, AssetFolderPath + "/" + AssetName, InCompressionSettings, TextureMipGenSettings::TMGS_FromTextureGroup);
		return NewTexture;
	});


	//
	// // OutTexture->CompressionNoAlpha = true;
	// OutTexture->MipGenSettings = TMGS_NoMipmaps;
	// OutTexture->SRGB = false;
	// // OutTexture->DeferCompression = true;
	// OutTexture->AddressX = TextureAddress::TA_Clamp;
	// OutTexture->AddressY = TextureAddress::TA_Clamp;

	UKismetRenderingLibrary::ConvertRenderTargetToTexture2DEditorOnly(GWorld, RenderTarget, OutTexture);
	// UEditorAssetLibrary::SaveLoadedAsset(OutTexture, false);
	
	return OutTexture;
}

UMaterialInstance* UCSAssetProcess::FindOrDuplicateMaterialInstanceAsset(FString SourcePath, FString TargetPath, bool Overwirte)
{
	FString TargetFolderName;
	FString TargetFileName;
	FString TargetExtension;
	FPaths::Split(TargetPath, TargetFolderName, TargetFileName, TargetExtension);
	if (Overwirte == true && UEditorAssetLibrary::DoesAssetExist(TargetFolderName / TargetFileName)) UEditorAssetLibrary::DeleteAsset(TargetFolderName / TargetFileName);

	if (!UEditorAssetLibrary::DoesAssetExist(SourcePath)) return nullptr;
	FString SourceFolderName;
	FString SourceFileName;
	FString SourceExtension;
	FPaths::Split(SourcePath, SourceFolderName, SourceFileName, SourceExtension);
	UMaterialInstance* Parent = Cast<UMaterialInstance>(UEditorAssetLibrary::LoadAsset(SourceFolderName / SourceFileName));
	if (Parent == nullptr) return nullptr;

	UMaterialInstance* OutMaterialInstance = nullptr;
	
	OutMaterialInstance = FindOrCreateAsset<UMaterialInstance>(TargetFileName, TargetFolderName,[&]()
	{
		
		UObject* NewObject = UEditorAssetLibrary::DuplicateAsset(SourcePath, TargetPath);
		UMaterialInstance* NewMaterialInstance = Cast<UMaterialInstance>(NewObject);
		return NewMaterialInstance;
	});
	
	UMaterialInstanceConstant* Constant = Cast<UMaterialInstanceConstant>(OutMaterialInstance);

	FStaticSwitchParameter SwitchParameter;
	// SwitchParameter.ParameterInfo.Name = TEXT("UseCustomUV");
	// SwitchParameter.Value = true;
	// SwitchParameter.bOverride = true;
	// NewStaticParameterSet.StaticSwitchParameters.Add(SwitchParameter);
	//
	// SwitchParameter.ParameterInfo.Name = *(TEXT("UseUV") + FString::FromInt(MeshData.TextureCoordinateIndex));
	// NewStaticParameterSet.StaticSwitchParameters.Add(SwitchParameter);
	OutMaterialInstance->InitStaticPermutation();

	UMaterialEditingLibrary::SetMaterialInstanceParent(Constant, Parent);
	// UEditorAssetLibrary::SaveLoadedAsset(OutMaterialInstance, false);
	
	
	return OutMaterialInstance;
}

//This function has a bug
UMaterialInstance* UCSAssetProcess::FindOrCreateMaterialInstanceAsset(FString AssetName, FString AssetFolderPath, UMaterialInterface* Parent)
{
	FString AssetPath = AssetFolderPath + "/" + AssetName;
	UMaterialInstance* OutMaterialInstance = FindOrCreateAsset<UMaterialInstance>(AssetName, AssetFolderPath,[&]()
	{
		IAssetTools &AssetTools = FModuleManager::GetModuleChecked<FAssetToolsModule>("AssetTools").Get();
		UObject* NewObject = AssetTools.CreateAsset(AssetName, AssetFolderPath, UMaterialInstance::StaticClass(), nullptr);
		UMaterialInstance* NewMaterialInstance = Cast<UMaterialInstance>(NewObject);
		// UMaterialInstanceConstant* NewMaterialInstance = FMaterialUtilities::CreateInstancedMaterial(Parent, CreatePackage( *AssetPath), AssetName, RF_Public | RF_Standalone);
		NewMaterialInstance->InitStaticPermutation();

		NewMaterialInstance->PostEditChange();

		return NewMaterialInstance;
	});
	UMaterialInterface* NewInterface =  OutMaterialInstance;
	
	UMaterialInstanceConstant* Constant = Cast<UMaterialInstanceConstant>(OutMaterialInstance);
	UMaterialEditingLibrary::SetMaterialInstanceParent(Constant, Parent);
	
	return OutMaterialInstance;;
}

void UCSAssetProcess::GetDistanceToNearestSurface(UTextureRenderTarget2D* InDebugView)
{
	FEditorViewportClient* EditorViewportClient = StaticCast<FEditorViewportClient*>(GEditor->GetActiveViewport()->GetClient());
	FViewport* Viewport = EditorViewportClient->Viewport;
	FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(Viewport, EditorViewportClient->GetScene(), EditorViewportClient->EngineShowFlags));
	// ViewFamily.viewinf
	// FSceneRenderer
	FSceneView* SceneView = EditorViewportClient->CalcSceneView(&ViewFamily);
	SceneView->Family->GetSceneRenderer()->GetSceneUniforms();
	// FScene* Scene;
	// // FDistanceFieldSceneData& DFData = Scene->DistanceFieldSceneData;
	// Scene->GetRenderScene();
	
	// SceneView
	// SceneView->bIsViewInfo
	UComputeShaderBasicFunction::CalDistanceToNearestSurface(SceneView, InDebugView);
}

void UCSAssetProcess::CreateDebugTexture(AActor* TargetActor, UTextureRenderTarget2D* InDebugView, FString DebugName)
{
	ULevel* CurrentLevel =TargetActor ->GetLevel();
	FString LevelPathName = GetPathNameSafe(CurrentLevel);
	FString FileName;
	FString LevelPath;
	FString Extension;
	FPaths::Split(LevelPathName, LevelPath, FileName, Extension);
	FString AssetPath = LevelPath.Append("/" + DebugName + "Debug");
	UCSAssetProcess::ConveretAndSaveRTAsset(TEXT("Debug"), AssetPath, InDebugView);
}

ACSShallowWaterCapture* UCSAssetProcess::CSSW_GetSelectActor()
{

	ACSShallowWaterCapture* SelectedCapture =nullptr;
	USelection* SelectedActors = GEditor->GetSelectedActors();
	if ( SelectedActors == nullptr && SelectedActors->Num() == 0 ) return SelectedCapture;	//Return nullptr
	int32 test = SelectedActors->Num();
	AActor* SelectedActor = Cast<AActor>( SelectedActors->GetSelectedObject(0) );

	if ( SelectedActor == nullptr ) return SelectedCapture; //Return nullptr
	
	SelectedCapture = Cast<ACSShallowWaterCapture>( SelectedActor );
	if ( SelectedCapture != nullptr ) return SelectedCapture;	//Return vaild data

	AActor* ParentActor = SelectedActor->GetAttachParentActor();
	SelectedCapture = Cast<ACSShallowWaterCapture>( ParentActor );
	if ( SelectedCapture != nullptr ) return SelectedCapture;	//Return vaild data

	ACSSHallowWaterSource* SelectedSource = Cast<ACSSHallowWaterSource>(SelectedActor);
	if (SelectedSource == nullptr) return SelectedCapture;	//Return nullptr

	TArray<TEnumAsByte<EObjectTypeQuery>> ObjectTypes = {
		UEngineTypes::ConvertToObjectType(ECollisionChannel::ECC_WorldStatic),
		UEngineTypes::ConvertToObjectType(ECollisionChannel::ECC_WorldDynamic)
	};
	FVector BoxExtent = FVector(5, 5, 100000);
	TArray<AActor*> OverlapActors;
	TArray<AActor*> ActorsToIgnore;
	UKismetSystemLibrary::BoxOverlapActors(GWorld, SelectedSource->GetActorLocation(), BoxExtent,
	                                       ObjectTypes, ACSShallowWaterCapture::StaticClass(), ActorsToIgnore,
	                                       OverlapActors);
	DrawDebugBox(GWorld, SelectedSource->GetActorLocation(), BoxExtent, FColor(1, 1, 1, 1), false, 1, 0, 1);
	if (OverlapActors.Num() == 0) return SelectedCapture; //Return nullptr
	SelectedCapture = Cast<ACSShallowWaterCapture>( OverlapActors[0] );

	return SelectedCapture; //Return nullptr or vaild data
	
	// UKismetSystemLibrary::PrintString(GWorld, TEXT("NoSelected"));
	
	
}

void UCSAssetProcess::MaterialTest(ACSShallowWaterCapture* InCSSWActor)
{
	if (InCSSWActor == nullptr) return;
	
	UStaticMesh* VisMesh = InCSSWActor->VisualizeMesh->GetStaticMesh(); 
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
	FString AssetFloderPath = LevelPath.Append("/CSSWData");

	FDateTime Time = FDateTime::Now();
	FString ResultVelHeightName = FString::Printf(TEXT("T_CSSW_VelHeight_%d%d%d"), Time.GetHour(), Time.GetMinute(), Time.GetSecond());
	FString ResultDepthWetName = FString::Printf(TEXT("T_CSSW_DepthWet_%d%d%d"), Time.GetHour(), Time.GetMinute(), Time.GetSecond());
	FString ResultWaterMaterialName = FString::Printf(TEXT("MI_CSSW_Water_%d%d%d"), Time.GetHour(), Time.GetMinute(), Time.GetSecond());
	FString ResultDecalMaterialName = FString::Printf(TEXT("MI_CSSW_Decal_%d%d%d"), Time.GetHour(), Time.GetMinute(), Time.GetSecond());
	// FString VelHeightAssetPath = AssetPath.Append("/" + ResultVelHeightName);
	// FString DepthWetAssetPath = AssetPath.Append("/" + ResultDepthWetName);
	FString  VelHeightAssetPath = AssetFloderPath / ResultWaterMaterialName;
	FString  VelHeightAssetPathFull = AssetFloderPath + "/" + ResultVelHeightName + "." + ResultVelHeightName;
	
	FString WaterMaterialPath = GetPathNameSafe(InCSSWActor->WaterMaterial);
	// UMaterialInstance* WaterMaterial = UCSAssetProcess::FindOrCreateMaterialInstanceAsset(ResultWaterMaterialName, AssetFloderPath, InCSSWActor->WaterMaterial);
	UMaterialInstance* WaterMaterial = UCSAssetProcess::FindOrDuplicateMaterialInstanceAsset(WaterMaterialPath, VelHeightAssetPathFull, true);
	
	UPackage* InOuter = CreatePackage( *VelHeightAssetPath);
	UMaterialInstanceConstant* NewMaterialInstance = FMaterialUtilities::CreateInstancedMaterial(InCSSWActor->WaterMaterial, InOuter, ResultVelHeightName, RF_Public | RF_Standalone);
	//
	
	FName PathName = FName(*WaterMaterialPath);
	// TCHAR* NameChar = PathName
	// UMaterialInstanceConstant* Test = Cast<UMaterialInstanceConstant>(UEditorAssetLibrary::LoadAsset(VelHeightAssetPathFull));
	// UMaterialInstanceConstant* Test2 = Cast<UMaterialInstanceConstant>(UEditorAssetLibrary::LoadAsset(VelHeightAssetPath));

	TStringBuilder<512> InName;
	InName = *VelHeightAssetPath;
	FPathViews::GetMountPointNameFromPath(InName);
	FStringView PackageMountPoint = FPathViews::GetMountPointNameFromPath(InName);
	// uint32 DefaultPackageFlags = GCreatePackageDefaultFlagsMap.Find(PackageMountPoint);
	
}
