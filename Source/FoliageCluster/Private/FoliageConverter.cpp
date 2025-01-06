//// Fill out your copyright notice in the Description page of Project Settings.
//
#include "FoliageConverter.h"
#include "Uobject/Object.h"
#include "Engine/StaticMesh.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Camera/CameraTypes.h"
#include "Engine/SceneCapture2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "ComponentReregisterContext.h"
#include "FoliageType.h"
#include "InstancedFoliageActor.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "Runtime/Experimental/Voronoi/Private/voro++/src/container.hh"
#include "Subsystems/EditorActorSubsystem.h"

#if WITH_EDITOR

void UFoliageConverter::ConvertFoliageToInstanceComponent(UFoliageType* InFoliageType)
{
	UEditorActorSubsystem* EditorActorSubsystem = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
	TArray<AActor*> SelectedActors = EditorActorSubsystem->GetSelectedLevelActors();
	AConvertFoliageInstanceContainer* Container = nullptr;
	for (AActor* Actor : SelectedActors)
	{
		if (Cast<AConvertFoliageInstanceContainer>(Actor))
		{
			Container = Cast<AConvertFoliageInstanceContainer>(Actor);
			break;
		}
	}
	if (!Container)
		return;
	
	Container->AddInstanceFromFoliageType(InFoliageType);
}

void UFoliageConverter::ConvertInstanceComponentToFoliage(UFoliageType* InFoliageType, UClass* ContainerClass)
{
	UEditorActorSubsystem* EditorActorSubsystem = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
	TArray<AActor*> SelectedActors = EditorActorSubsystem->GetSelectedLevelActors();
	AConvertFoliageInstanceContainer* Container = nullptr;
	for (AActor* Actor : SelectedActors)
	{
		if (Cast<AConvertFoliageInstanceContainer>(Actor))
		{
			Container = Cast<AConvertFoliageInstanceContainer>(Actor);
			break;
		}
	}
	if (!Container)
		return;
	Container->ConvertInstanceToFoliage(InFoliageType);
}

#endif

UStaticMesh* UFoliageConverter::GetStaticMesh(UFoliageType* InFoliageType)
{
	for (TActorIterator<AInstancedFoliageActor> It(GWorld); It; ++It)
	{
		AInstancedFoliageActor* IFA = (*It);
		FFoliageInfo* FoliageInfo = It->FindInfo(InFoliageType);
		if (!FoliageInfo)
		{
			continue;
		}
		UStaticMesh* SMesh = FoliageInfo->GetComponent()->GetStaticMesh();
		if (!SMesh)
		{
			continue;
		}
		return SMesh;
	}
	return nullptr;
}

const UFoliageType* UFoliageConverter::GetFoliageType(UStaticMesh* StaticMesh)
{
	AInstancedFoliageActor* IFA = nullptr;
	for (TActorIterator<AInstancedFoliageActor> It(GWorld); It; ++It)
	{
		IFA = (*It);
		break;
	}
	if (!IFA)
		return nullptr;

	TArray<const UFoliageType*> FoliageTypes;
	IFA->GetAllFoliageTypesForSource(StaticMesh, FoliageTypes);
	if (FoliageTypes.Num() > 0)
	{
		return FoliageTypes[0];
	}
	
	return nullptr;
}

void UFoliageConverter::GetAllInstancesTransfrom(UFoliageType* InFoliageType, TArray<FTransform>& OutTransforms)
{
	
	for (TActorIterator<AInstancedFoliageActor> It(GWorld); It; ++It)
	{
		AInstancedFoliageActor* IFA = (*It);
		FFoliageInfo* FoliageInfo = It->FindInfo(InFoliageType);
		if (!FoliageInfo)
			continue;
		
		if (!FoliageInfo->GetComponent() || FoliageInfo->Instances.Num() == 0)
			continue;
		int32 InstanceCount = FoliageInfo->GetComponent()->GetInstanceCount();
		TArray<FTransform> TempTransforms;
		TempTransforms.SetNum(InstanceCount);

		for (int32 i = 0; i < InstanceCount; i++)
		{
			FoliageInfo->GetComponent()->GetInstanceTransform(i, TempTransforms[i], true);
		}
		OutTransforms.Append(TempTransforms);
	}
	
}

void UFoliageConverter::RefreshFoliage(UFoliageType* InFoliageType)
{
	for (TActorIterator<AInstancedFoliageActor> It(GWorld); It; ++It)
	{
		AInstancedFoliageActor* IFA = (*It);
		FFoliageInfo* FoliageInfo = It->FindInfo(InFoliageType);
		if (!FoliageInfo)
		{
			continue;
		}

		FoliageInfo->Refresh(true, true);
	}
}

const UFoliageType* UFoliageConverter::AddFoliageType(const UFoliageType* InType)
{
	AInstancedFoliageActor* IFA = nullptr;
	for (TActorIterator<AInstancedFoliageActor> It(GWorld); It; ++It)
	{
		IFA = (*It);
		break;
	}
	if (!IFA)
		return nullptr;
	
	FFoliageInfo* Info = nullptr;
	UFoliageType* FoliageType = IFA->AddFoliageType(InType, &Info);
	
	return nullptr;
}

bool UFoliageConverter::RemoveFoliageInstance(UStaticMesh* StaticMesh, const int32 InstanceIndex)
{
	ULevel* DesiredLevel = GWorld->GetCurrentLevel();

	AInstancedFoliageActor* InstancedFoliageActor = AInstancedFoliageActor::GetInstancedFoliageActorForLevel(DesiredLevel, true);
	if (!InstancedFoliageActor)
		return false;

	TArray<const UFoliageType*> FoliageTypes;
	InstancedFoliageActor->GetAllFoliageTypesForSource(StaticMesh, FoliageTypes);
	if(FoliageTypes.Num() == 0)
	{
		return false;
	}
	const UFoliageType* FoliageType = FoliageTypes[0];
	
	if (!FoliageType)
		return false;

	FFoliageInfo* FoliageInfo = InstancedFoliageActor->FindInfo(FoliageType);
	//if (FoliageInfo && InstanceArray.Max() < FoliageInfo->Instances.Num())
	if (FoliageInfo)
	{
		TArray<int32> temparray;
		temparray.Add(InstanceIndex);
		FoliageInfo->RemoveInstances(temparray, true);
		FoliageInfo->Refresh(true, true);
		return true;
		}
	
	return false;

}

bool UFoliageConverter::SetFoliageInstanceTransform(UStaticMesh* StaticMesh, const int32 InstanceIndex, const FTransform& Transform)
{
	ULevel* DesiredLevel = GWorld->GetCurrentLevel();

	AInstancedFoliageActor* InstancedFoliageActor = AInstancedFoliageActor::GetInstancedFoliageActorForLevel(DesiredLevel, true);
	if (!InstancedFoliageActor)
		return false;

	TArray<const UFoliageType*> FoliageTypes;
	InstancedFoliageActor->GetAllFoliageTypesForSource(StaticMesh, FoliageTypes);
	if(FoliageTypes.Num() == 0)
	{
		return false;
	}
	const UFoliageType* FoliageType = FoliageTypes[0];
	if (!FoliageType)
		return false;

	else
	{
		FFoliageInfo* FoliageInfo = InstancedFoliageActor->FindInfo(FoliageType);
		if (FoliageInfo)
		{	
			if (InstanceIndex +1 >= FoliageInfo->Instances.Num())
				return false;
			TArray<int32> temparray;
			temparray.Add(InstanceIndex);
			FoliageInfo->PreMoveInstances(temparray);

			FFoliageInstance& Instance = FoliageInfo->Instances[InstanceIndex];
			Instance.Location = Transform.GetLocation();
			Instance.Rotation = Transform.GetRotation().Rotator();
			FVector Scale = Transform.GetScale3D();
			Instance.DrawScale3D = FVector3f(Scale.X,Scale.Y,Scale.Z);
			
			FoliageInfo->PostMoveInstances(temparray, false);
			return true;
			//}

		}
		return false;
	}
}

bool UFoliageConverter::SetFoliageInstanceID(UStaticMesh* StaticMesh, int32 InstanceArray, int32 BaseID)
{
	ULevel* DesiredLevel = GWorld->GetCurrentLevel();

	AInstancedFoliageActor* InstancedFoliageActor = AInstancedFoliageActor::GetInstancedFoliageActorForLevel(DesiredLevel, true);
	if (!InstancedFoliageActor)
		return false;

	TArray<const UFoliageType*> FoliageTypes;
	InstancedFoliageActor->GetAllFoliageTypesForSource(StaticMesh, FoliageTypes);
	if(FoliageTypes.Num() == 0)
	{
		return false;
	}
	const UFoliageType* FoliageType = FoliageTypes[0];
	if (!FoliageType)
		return false;

	else
	{
		FFoliageInfo* FoliageInfo = InstancedFoliageActor->FindInfo(FoliageType);
		if (FoliageInfo)
		{
			//FoliageInfo->PreMoveInstances(InstancedFoliageActor, InstanceArray);
			FFoliageInstance& Instance = FoliageInfo->Instances[InstanceArray];
			Instance.BaseId = BaseID;
			//FoliageInfo->PostMoveInstances(InstancedFoliageActor, InstanceArray, false);
			return true;
		}
		return false;
	}
}

bool UFoliageConverter::GetFoliageInstanceID(UStaticMesh* StaticMesh, int32 InstanceArray, int32& BaseID)
{
	BaseID = -1;
	ULevel* DesiredLevel = GWorld->GetCurrentLevel();

	AInstancedFoliageActor* InstancedFoliageActor = AInstancedFoliageActor::GetInstancedFoliageActorForLevel(DesiredLevel, true);
	if (!InstancedFoliageActor)
		return false;

	TArray<const UFoliageType*> FoliageTypes;
	InstancedFoliageActor->GetAllFoliageTypesForSource(StaticMesh, FoliageTypes);
	if(FoliageTypes.Num() == 0)
	{
		return false;
	}
	const UFoliageType* FoliageType = FoliageTypes[0];
	if (!FoliageType)
		return false;

	else
	{
		FFoliageInfo* FoliageInfo = InstancedFoliageActor->FindInfo(FoliageType);
		if (FoliageInfo)
		{
				FFoliageInstance& Instance = FoliageInfo->Instances[InstanceArray];
				BaseID = Instance.BaseId;
			return true;
		}
		return false;
	}
}

bool UFoliageConverter::GetCustomDataValue(UInstancedStaticMeshComponent* InstanceStaticMesh, int32 InstanceIndex, int32 CustomDataIndex, float& CustomDataValue)
{
	if (!InstanceStaticMesh)
	{
		return false;
	}
	//不知道为什么原本的函数居然不起作用所以只能重新做一个了
	TArray<FInstancedStaticMeshInstanceData> PerInstanceSMData = InstanceStaticMesh->PerInstanceSMData;
	int32 NumCustomDataFloats = InstanceStaticMesh->NumCustomDataFloats;
	if (!PerInstanceSMData.IsValidIndex(InstanceIndex) || CustomDataIndex < 0 || CustomDataIndex >= NumCustomDataFloats)
	{
		return false;
	}
	TArray<float> PerInstanceSMCustomData = InstanceStaticMesh->PerInstanceSMCustomData;
	CustomDataValue = PerInstanceSMCustomData[InstanceIndex * NumCustomDataFloats + CustomDataIndex];
	return true;
}

bool UFoliageConverter::SetCustomDataValue(UInstancedStaticMeshComponent* InstanceStaticMesh, int32 InstanceIndex, int32 CustomDataIndex, float CustomDataValue)
{
	if (!InstanceStaticMesh)
	{
		return false;
	}

	TArray<float> PerInstanceSMCustomData = InstanceStaticMesh->PerInstanceSMCustomData;
	int32 NumCustomDataFloats = InstanceStaticMesh->NumCustomDataFloats;
	TArray<FInstancedStaticMeshInstanceData> PerInstanceSMData = InstanceStaticMesh->PerInstanceSMData;
	if (!PerInstanceSMData.IsValidIndex(InstanceIndex) || CustomDataIndex < 0)
	{
		return false;
	}
	if (NumCustomDataFloats <= CustomDataIndex)
	{
		InstanceStaticMesh->NumCustomDataFloats = CustomDataIndex + 1;
		InstanceStaticMesh->PerInstanceSMCustomData.Empty(PerInstanceSMData.Num() * InstanceStaticMesh->NumCustomDataFloats);
		InstanceStaticMesh->PerInstanceSMCustomData.SetNumZeroed(PerInstanceSMData.Num() * InstanceStaticMesh->NumCustomDataFloats);

	}
	InstanceStaticMesh->MarkRenderStateDirty();
	InstanceStaticMesh->Modify();

	InstanceStaticMesh->PerInstanceSMCustomData[InstanceIndex * InstanceStaticMesh->NumCustomDataFloats + CustomDataIndex] = CustomDataValue;

	// Force recreation of the render data when proxy is created
	//这里不知道为什么更新材质不能用自带的函数, 必须直接写++ 错误提示貌似是没有include
	// InstanceStaticMesh->InstanceUpdateCmdBuffer.NumEdits++;
	InstanceStaticMesh->MarkRenderStateDirty();

	return true;
}


bool UFoliageConverter::AddFoliageInstance(UStaticMesh* StaticMesh, UPrimitiveComponent* AttachComponent, const FTransform& Transform)
{
	ULevel* DesiredLevel = GWorld->GetCurrentLevel();

	AInstancedFoliageActor* InstancedFoliageActor = AInstancedFoliageActor::GetInstancedFoliageActorForLevel(DesiredLevel, true);
	if (!InstancedFoliageActor)
		return false;

	TArray<const UFoliageType*> FoliageTypes;
	InstancedFoliageActor->GetAllFoliageTypesForSource(StaticMesh, FoliageTypes);
	if(FoliageTypes.Num() == 0)
	{
		return false;
	}
	const UFoliageType* FoliageType = FoliageTypes[0];
	
	// Get the FoliageMeshInfo for this Foliage type so we can add the instance to it
	FFoliageInfo* FoliageInfo = InstancedFoliageActor->FindInfo(FoliageType);
	if (!FoliageInfo)
		return false;

	FFoliageInstance FoliageInstance;
	FoliageInfo->ReserveAdditionalInstances(FoliageType, 1);

	if (AttachComponent)
	{
		FoliageInstance.BaseComponent = AttachComponent;
	}

	FoliageInstance.Location = Transform.GetLocation();
	FoliageInstance.Rotation = Transform.GetRotation().Rotator();
	FVector Scale = Transform.GetScale3D();
	FoliageInstance.DrawScale3D = FVector3f(Scale.X,Scale.Y,Scale.Z);
	FoliageInfo->AddInstance(FoliageType, FoliageInstance);
	
	FoliageInfo->Refresh(true, true);
	return true;
}

void UFoliageConverter::DistanceSort(TMap<float, int32> InMap, TMap<float, int32>& OutMap)
{
	InMap.KeySort([](float A, float B) { return A > B; });
	OutMap = InMap;
}

UHierarchicalInstancedStaticMeshComponent* UFoliageConverter::FindFoliageInstanceComponent(UStaticMesh* StaticMesh)
{
	ULevel* DesiredLevel = GWorld->GetCurrentLevel();

	AInstancedFoliageActor* InstancedFoliageActor = AInstancedFoliageActor::GetInstancedFoliageActorForLevel(DesiredLevel, true);
	if (!InstancedFoliageActor)
		return nullptr;

	TArray<const UFoliageType*> FoliageTypes;
	InstancedFoliageActor->GetAllFoliageTypesForSource(StaticMesh, FoliageTypes);
	if(FoliageTypes.Num() == 0)
	{
		return nullptr;
	}
	const UFoliageType* FoliageType = FoliageTypes[0];
	if (!FoliageType )
	{
		return nullptr;
	}

	FFoliageInfo* FoliageInfo = InstancedFoliageActor->FindInfo(FoliageType);
	if (!FoliageInfo)
		return nullptr;

	return FoliageInfo->GetComponent();
}

void UFoliageConverter::AddFoliage()
{
	// UWorld* World = GWorld;
	// UClass* ReferenceBP = UEditorAssetLibrary::LoadBlueprintClass(TEXT("Blueprint'/Game/Test/AsynTest/FoliageCommunity_Child.FoliageCommunity_Child'"));
	// UObject* object = UEditorAssetLibrary::LoadAsset(TEXT("Blueprint'/Game/Test/AsynTest/FoliageCommunity_Child.FoliageCommunity_Child'"));
	// UClass* ReferenceContainer = UEditorAssetLibrary::LoadBlueprintClass(TEXT("Blueprint'/Game/Test/AsynTest/FoliageRegistrar.FoliageRegistrar'"));
	// TArray<AActor*> Actors;
	// TArray<UObject*> subobjects;
	// object->GetDefaultSubobjects(subobjects);
	// UGameplayStatics::GetAllActorsOfClass(World, ReferenceBP, Actors);
	// if (Actors.Num() > 0)
	// {
		//AActor* ReferenceActor = Actors[0];

		//TArray<UActorComponent*> ChildComponents = ReferenceActor->GetComponentsByClass(ReferenceContainer);

		//for (auto ChildComponent : ChildComponents)
		//{
		//	ChildComponent
		//}

	// }


}



#define LOCTEXT_NAMESPACE "AConvertFoliageInstanceContainer"

AConvertFoliageInstanceContainer::AConvertFoliageInstanceContainer(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	FoliageInstanceContainer = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("FoliageInstanceContainer"));
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	FoliageInstanceContainer->SetStaticMesh(Mesh);
	FoliageInstanceContainer->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Overlap);
	FoliageInstanceContainer->SetCollisionObjectType(ECollisionChannel::ECC_WorldDynamic);
	FoliageInstanceContainer->SetHiddenInGame(true);
	FoliageInstanceContainer->SetupAttachment(GetRootComponent(), TEXT("FoliageInstanceContainer"));
}

void AConvertFoliageInstanceContainer::AddInstanceFromFoliageType(UFoliageType* InFoliageType)
{
	TArray<FTransform> Transforms;
	UFoliageConverter::GetAllInstancesTransfrom(InFoliageType, Transforms);
	UMaterial* Material = LoadObject<UMaterial>(
	nullptr, TEXT("/Engine/EngineMaterials/EditorBrushMaterial.EditorBrushMaterial"));
	FoliageInstanceContainer->SetStaticMesh(UFoliageConverter::GetStaticMesh(InFoliageType));
	FoliageInstanceContainer->AddInstances(Transforms, false, true, false);
	FoliageInstanceContainer->SetMaterial(0, Material);

	for (TActorIterator<AInstancedFoliageActor> It(GWorld); It; ++It)
	{
		AInstancedFoliageActor* IFA = (*It);
		IFA->RemoveFoliageType(&InFoliageType, 1);
	}
}

void AConvertFoliageInstanceContainer::ConvertInstanceToFoliage(UFoliageType* InFoliageType)
{
	int32 InstanceCount = FoliageInstanceContainer->GetInstanceCount();
	if (InstanceCount == 0)
		return;
	
	TArray<FTransform> Transforms;
	Transforms.Reserve(InstanceCount);
	for (int32 i = 0; i < InstanceCount; i++)
	{
		FTransform Transform;
		FoliageInstanceContainer->GetInstanceTransform(i, Transform, true);
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
	
	FoliageInstanceContainer->ClearInstances();
	UFoliageConverter::RefreshFoliage(InFoliageType);
}

