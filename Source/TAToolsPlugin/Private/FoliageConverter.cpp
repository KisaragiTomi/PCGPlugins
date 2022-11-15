//// Fill out your copyright notice in the Description page of Project Settings.
//
#include "FoliageConverter.h"
#include "Uobject/Object.h"
#include "Engine/StaticMesh.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetRenderingLibrary.h"
//#include "Engine/TextureRenderTarget2D.h"
//#include "KismetProceduralMeshLibrary.h"
#include "Camera/CameraTypes.h"
//#include "KismetMathLibrary.h"
#include "Engine/SceneCapture2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "ComponentReregisterContext.h"
#include "FoliageType.h"
#include "InstancedFoliageActor.h"
#include "Engine/StaticMeshActor.h"

//#include "MeshRegionBoundaryLoops.h"
//#if WITH_OPENCV
//OPENCV_INCLUDES_START
//#undef check // the check macro causes problems with opencv headers
//#include "opencv2/calib3d.hpp"
//#include "opencv2/imgproc.hpp"

////using namespace cv;
//OPENCV_INCLUDES_END
//#endif




bool UFoliageConverter::RemoveFoliageInstance(UStaticMesh* StaticMesh, const int32 InstanceIndex)
{
	ULevel* DesiredLevel = GWorld->GetCurrentLevel();

	AInstancedFoliageActor* InstancedFoliageActor = AInstancedFoliageActor::GetInstancedFoliageActorForLevel(DesiredLevel, true);
	if (!InstancedFoliageActor || InstancedFoliageActor->IsPendingKill())
		return false;

	TArray<const UFoliageType*> FoliageTypes;
	InstancedFoliageActor->GetAllFoliageTypesForSource(StaticMesh, FoliageTypes);
	if(FoliageTypes.Num() == 0)
	{
		return false;
	}
	const UFoliageType* FoliageType = FoliageTypes[0];
	
	if (!FoliageType || FoliageType->IsPendingKill())
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
	if (!InstancedFoliageActor || InstancedFoliageActor->IsPendingKill())
		return false;

	TArray<const UFoliageType*> FoliageTypes;
	InstancedFoliageActor->GetAllFoliageTypesForSource(StaticMesh, FoliageTypes);
	if(FoliageTypes.Num() == 0)
	{
		return false;
	}
	const UFoliageType* FoliageType = FoliageTypes[0];
	if (!FoliageType || FoliageType->IsPendingKill())
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
	if (!InstancedFoliageActor || InstancedFoliageActor->IsPendingKill())
		return false;

	TArray<const UFoliageType*> FoliageTypes;
	InstancedFoliageActor->GetAllFoliageTypesForSource(StaticMesh, FoliageTypes);
	if(FoliageTypes.Num() == 0)
	{
		return false;
	}
	const UFoliageType* FoliageType = FoliageTypes[0];
	if (!FoliageType || FoliageType->IsPendingKill())
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
	if (!InstancedFoliageActor || InstancedFoliageActor->IsPendingKill())
		return false;

	TArray<const UFoliageType*> FoliageTypes;
	InstancedFoliageActor->GetAllFoliageTypesForSource(StaticMesh, FoliageTypes);
	if(FoliageTypes.Num() == 0)
	{
		return false;
	}
	const UFoliageType* FoliageType = FoliageTypes[0];
	if (!FoliageType || FoliageType->IsPendingKill())
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
	InstanceStaticMesh->InstanceUpdateCmdBuffer.NumEdits++;
	InstanceStaticMesh->MarkRenderStateDirty();

	return true;
}


bool UFoliageConverter::AddFoliageInstance(UStaticMesh* StaticMesh, UPrimitiveComponent* AttachComponent, const FTransform& Transform)
{
	ULevel* DesiredLevel = GWorld->GetCurrentLevel();

	AInstancedFoliageActor* InstancedFoliageActor = AInstancedFoliageActor::GetInstancedFoliageActorForLevel(DesiredLevel, true);
	if (!InstancedFoliageActor || InstancedFoliageActor->IsPendingKill())
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
	if (!InstancedFoliageActor || InstancedFoliageActor->IsPendingKill())
		return nullptr;

	TArray<const UFoliageType*> FoliageTypes;
	InstancedFoliageActor->GetAllFoliageTypesForSource(StaticMesh, FoliageTypes);
	if(FoliageTypes.Num() == 0)
	{
		return nullptr;
	}
	const UFoliageType* FoliageType = FoliageTypes[0];
	if (!FoliageType || FoliageType->IsPendingKill())
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