// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/MaterialOverrideTool.h"

#include "Components/MeshComponent.h"
#include "Editor/EditorEngine.h"
#include "EditorViewportClient.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Logging/AssetHandlerLogging.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "ShowFlags.h"

namespace
{
int32 CountMaterialSlots(const TArray<UMeshComponent*>& Components)
{
	int32 SlotCount = 0;
	for (const UMeshComponent* MeshComponent : Components)
	{
		SlotCount += MeshComponent ? MeshComponent->GetNumMaterials() : 0;
	}
	return SlotCount;
}
} // namespace

void UMaterialOverrideTool::BeginDestroy()
{
	RestoreViewportOverride();
	RestoreAllComponents();
	Super::BeginDestroy();
}

FMaterialOverrideStats UMaterialOverrideTool::ApplyViewportOverrideMaterial(
	UWorld* World,
	UMaterial* ViewportMaterial,
	FLinearColor TintColor)
{
	if (!World || !ViewportMaterial)
	{
		UE_LOG(LogAssetHandler, Error, TEXT("ApplyViewportOverrideMaterial: Invalid parameters"));
		Stats = FMaterialOverrideStats();
		return FMaterialOverrideStats();
	}

	if (bActive)
	{
		RestoreViewportOverride();
		RestoreAllComponents();
	}

	CurrentWorld = World;
	CurrentViewportOverrideTint = TintColor;
	Records.Reset();
	ActiveComponents.Reset();

	TArray<UMeshComponent*> Components;
	CollectMeshComponents(World, Components);

	if (!TryApplyViewportOverride(World, ViewportMaterial, TintColor))
	{
		bActive = false;
		Stats = FMaterialOverrideStats();
		UE_LOG(LogAssetHandler, Error,
			TEXT("ApplyViewportOverrideMaterial: Failed to acquire a compatible editor viewport"));
		return FMaterialOverrideStats();
	}

	bActive = true;
	Stats.TotalComponents = Components.Num();
	Stats.AppliedCount = CountMaterialSlots(Components);
	Stats.bIsActive = true;
	return Stats;
}

bool UMaterialOverrideTool::TryApplyViewportOverride(
	UWorld* World,
	UMaterialInterface* OverrideMaterial,
	const FLinearColor& TintColor)
{
#if WITH_EDITOR
	if (!GEditor || !GEngine || !World || !OverrideMaterial)
	{
		return false;
	}

	RestoreViewportOverride();
	OverriddenViewportClients.Reset();

	for (FEditorViewportClient* ViewportClient : GEditor->GetAllViewportClients())
	{
		if (!ViewportClient ||
			!ViewportClient->IsLevelEditorClient() ||
			!ViewportClient->IsPerspective() ||
			!ViewportClient->IsVisible() ||
			ViewportClient->GetWorld() != World)
		{
			continue;
		}

		if (ViewportClient->IsEngineShowFlagsOverrideEnabled())
		{
			continue;
		}

		const bool bPerspective = ViewportClient->IsPerspective();
		ViewportClient->EnableOverrideEngineShowFlags(
			[bPerspective](FEngineShowFlags& ShowFlags)
			{
				ApplyViewMode(EViewModeIndex::VMI_Clay, bPerspective, ShowFlags);
			});
		OverriddenViewportClients.Add(ViewportClient);
	}

	if (OverriddenViewportClients.Num() == 0)
	{
		return false;
	}

	SavedLevelColorationLitMaterial = GEngine->LevelColorationLitMaterial.Get();
	SavedLevelColorationUnlitMaterial = GEngine->LevelColorationUnlitMaterial.Get();
	SavedShadedLevelColorationLitMaterial = GEngine->ShadedLevelColorationLitMaterial.Get();
	SavedShadedLevelColorationUnlitMaterial = GEngine->ShadedLevelColorationUnlitMaterial.Get();
	SavedClayMaterial = GEngine->ClayMaterial.Get();
	SavedLightingOnlyBrightness = GEngine->LightingOnlyBrightness;

	bUsingViewportOverride = true;
	UpdateViewportOverrideMaterial(OverrideMaterial, TintColor);
	return true;
#else
	return false;
#endif
}

void UMaterialOverrideTool::UpdateViewportOverrideMaterial(
	UMaterialInterface* OverrideMaterial,
	const FLinearColor& TintColor)
{
#if WITH_EDITOR
	if (!bUsingViewportOverride || !GEngine || !OverrideMaterial)
	{
		return;
	}

	GEngine->ClayMaterial = OverrideMaterial;
	CurrentViewportOverrideTint = TintColor;
	FlushEditorMaterials();
#endif
}

void UMaterialOverrideTool::RestoreViewportOverride()
{
#if WITH_EDITOR
	if (!bUsingViewportOverride)
	{
		return;
	}

	if (GEngine)
	{
		GEngine->LevelColorationLitMaterial = SavedLevelColorationLitMaterial;
		GEngine->LevelColorationUnlitMaterial = SavedLevelColorationUnlitMaterial;
		GEngine->ShadedLevelColorationLitMaterial = SavedShadedLevelColorationLitMaterial;
		GEngine->ShadedLevelColorationUnlitMaterial = SavedShadedLevelColorationUnlitMaterial;
		GEngine->ClayMaterial = SavedClayMaterial;
		GEngine->LightingOnlyBrightness = SavedLightingOnlyBrightness;
	}

	for (FEditorViewportClient* ViewportClient : OverriddenViewportClients)
	{
		if (!ViewportClient)
		{
			continue;
		}

		ViewportClient->DisableOverrideEngineShowFlags();
		ViewportClient->RequestRealTimeFrames(2);
		ViewportClient->Invalidate();
	}

	OverriddenViewportClients.Reset();
	bUsingViewportOverride = false;
#endif
}

void UMaterialOverrideTool::RestoreAllComponents()
{
	for (const FComponentMaterialRecord& Record : Records)
	{
		UMeshComponent* MeshComponent = ResolveComponent(Record.ComponentPath);
		if (!MeshComponent)
		{
			continue;
		}

		for (const TPair<int32, UMaterialInterface*>& Pair : Record.IndexToMaterial)
		{
			MeshComponent->SetMaterial(Pair.Key, Pair.Value);
		}
	}

	Records.Reset();
	ActiveComponents.Reset();
	FlushEditorMaterials();
}

void UMaterialOverrideTool::CollectMeshComponents(
	UWorld* World,
	TArray<UMeshComponent*>& OutComponents)
{
	OutComponents.Reset();
	if (!World)
	{
		return;
	}

	for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
	{
		AActor* Actor = *ActorIt;
		if (!Actor)
		{
			continue;
		}

		TInlineComponentArray<UMeshComponent*> MeshComponents(Actor);
		for (UMeshComponent* MeshComponent : MeshComponents)
		{
			if (MeshComponent)
			{
				OutComponents.Add(MeshComponent);
			}
		}
	}
}

UMeshComponent* UMaterialOverrideTool::ResolveComponent(const FString& Path)
{
	return FindObject<UMeshComponent>(nullptr, *Path);
}

void UMaterialOverrideTool::FlushEditorMaterials()
{
	for (FEditorViewportClient* ViewportClient : OverriddenViewportClients)
	{
		if (ViewportClient)
		{
			ViewportClient->RequestRealTimeFrames(2);
			ViewportClient->Invalidate();
		}
	}

	for (const TWeakObjectPtr<UMeshComponent>& MeshComponent : ActiveComponents)
	{
		if (MeshComponent.IsValid())
		{
			MeshComponent->MarkRenderStateDirty();
		}
	}
}
