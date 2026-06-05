// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Materials/MaterialInterface.h"
#include "MaterialOverrideTool.generated.h"

class UWorld;
class UMeshComponent;
class UMaterial;
class FEditorViewportClient;

/**
 * 操作统计
 */
USTRUCT(BlueprintType)
struct FMaterialOverrideStats
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "Asset Inspection")
	int32 TotalComponents = 0;

	UPROPERTY(BlueprintReadWrite, Category = "Asset Inspection")
	int32 AppliedCount = 0;

	UPROPERTY(BlueprintReadWrite, Category = "Asset Inspection")
	bool bIsActive = false;
};

/**
 * 单个 Component 的材质替换记录（用于恢复原始材质）
 */
USTRUCT()
struct FComponentMaterialRecord
{
	GENERATED_BODY()

	UPROPERTY()
	FString ComponentPath;

	UPROPERTY()
	TMap<int32, UMaterialInterface*> IndexToMaterial;
};

/**
 * 视口材质覆写工具 — 在编辑器视口渲染阶段临时替换全场景材质。
 */
UCLASS()
class ASSETHANDLERPLUGIN_API UMaterialOverrideTool : public UObject
{
	GENERATED_BODY()

public:
	virtual void BeginDestroy() override;

	/**
	 * 启用编辑器视口渲染阶段的临时材质覆写。
	 */
	UFUNCTION(BlueprintCallable, Category = "Asset Inspection|Material Override")
	FMaterialOverrideStats ApplyViewportOverrideMaterial(
		UWorld* World,
		UMaterial* ViewportMaterial,
		FLinearColor TintColor = FLinearColor::White);

protected:
	bool TryApplyViewportOverride(UWorld* World, UMaterialInterface* OverrideMaterial, const FLinearColor& TintColor);
	void UpdateViewportOverrideMaterial(UMaterialInterface* OverrideMaterial, const FLinearColor& TintColor);
	void RestoreViewportOverride();
	void RestoreAllComponents();
	void CollectMeshComponents(UWorld* World, TArray<UMeshComponent*>& OutComponents);
	UMeshComponent* ResolveComponent(const FString& Path);
	void FlushEditorMaterials();

private:
	UPROPERTY()
	bool bActive = false;

	UPROPERTY()
	TWeakObjectPtr<UWorld> CurrentWorld;

	UPROPERTY()
	TArray<TWeakObjectPtr<UMeshComponent>> ActiveComponents;

	UPROPERTY()
	TArray<FComponentMaterialRecord> Records;

	UPROPERTY()
	FMaterialOverrideStats Stats;

	bool bUsingViewportOverride = false;
	FLinearColor CurrentViewportOverrideTint = FLinearColor::White;
	TArray<FEditorViewportClient*> OverriddenViewportClients;

	UMaterial* SavedLevelColorationLitMaterial = nullptr;
	UMaterial* SavedLevelColorationUnlitMaterial = nullptr;
	UMaterial* SavedShadedLevelColorationLitMaterial = nullptr;
	UMaterial* SavedShadedLevelColorationUnlitMaterial = nullptr;
	UMaterialInterface* SavedClayMaterial = nullptr;
	FLinearColor SavedLightingOnlyBrightness = FLinearColor::White;
};
