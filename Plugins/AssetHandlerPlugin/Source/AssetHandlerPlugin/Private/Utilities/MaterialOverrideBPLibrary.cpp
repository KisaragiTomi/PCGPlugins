// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/MaterialOverrideBPLibrary.h"

#include "Editor/EditorEngine.h"
#include "Logging/AssetHandlerLogging.h"

static TWeakObjectPtr<UMaterialOverrideTool> GSharedToolInstance;

static UMaterialOverrideTool* GetOrCreateTool()
{
	if (GSharedToolInstance.IsValid())
	{
		return GSharedToolInstance.Get();
	}

	UMaterialOverrideTool* NewTool = NewObject<UMaterialOverrideTool>(
		GetTransientPackage(),
		UMaterialOverrideTool::StaticClass(),
		FName(TEXT("MaterialOverrideTool_Shared")),
		RF_Transient | RF_Public);
	NewTool->AddToRoot();

	GSharedToolInstance = NewTool;
	return NewTool;
}

static UWorld* GetEditorWorld()
{
	if (GEditor)
	{
		return GEditor->GetEditorWorldContext().World();
	}

	return nullptr;
}

int32 UMaterialOverrideBPLibrary::K2_ApplyViewportOverrideMaterial(
	UMaterial* ViewportMaterial,
	FLinearColor TintColor)
{
	if (!ViewportMaterial)
	{
		UE_LOG(LogAssetHandler, Error, TEXT("K2_ApplyViewportOverrideMaterial: ViewportMaterial is null"));
		return -1;
	}

	UWorld* World = GetEditorWorld();
	if (!World)
	{
		UE_LOG(LogAssetHandler, Error, TEXT("K2_ApplyViewportOverrideMaterial: No EditorWorld found"));
		return -1;
	}

	UMaterialOverrideTool* Tool = GetOrCreateTool();
	if (!Tool)
	{
		return -1;
	}

	const FMaterialOverrideStats Stats = Tool->ApplyViewportOverrideMaterial(World, ViewportMaterial, TintColor);
	return Stats.bIsActive ? Stats.AppliedCount : -1;
}
