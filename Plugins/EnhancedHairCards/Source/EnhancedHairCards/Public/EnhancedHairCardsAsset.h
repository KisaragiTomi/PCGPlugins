#pragma once

#include "CoreMinimal.h"
#include "EnhancedHairCardsDatas.h"
#include "UObject/Object.h"
#include "EnhancedHairCardsAsset.generated.h"

class UGroomAsset;
class UMaterialInterface;
class UNiagaraSystem;
class UStaticMesh;

USTRUCT(BlueprintType)
struct FEnhancedHairCardsPart
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Source")
	int32 SourceGroupIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Source")
	int32 SourceLODIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Source")
	int32 SourceCardsDescriptionIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Source")
	FName SourceMaterialSlotName = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Source")
	TObjectPtr<UStaticMesh> SourceMesh = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Material")
	TObjectPtr<UMaterialInterface> Material = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Cards")
	FEnhancedHairCardsSettings CardSettings;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Guides")
	TArray<FEnhancedHairCardsGuideCurve> GuideCurves;
};

USTRUCT(BlueprintType)
struct FEnhancedHairCardsPreviewSettings
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Visibility")
	bool bShowOriginalGroom = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Visibility",
		meta=(ToolTip="Keep the source Groom component visible when it is used as the cards simulation/deformation driver. Turning this off lets preview visibility hide the Groom, but the original Groom cards deformation buffer may stop updating."))
	bool bKeepSourceGroomVisibleForSimulation = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Visibility")
	bool bShowEnhancedCards = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Visibility")
	bool bShowGuides = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Visibility")
	bool bRenderEnhancedCardsMesh = false;
};

USTRUCT(BlueprintType)
struct FEnhancedHairCardsNiagaraSimulationGroup
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category="Source")
	int32 SourceGroupIndex = INDEX_NONE;

	UPROPERTY(BlueprintReadWrite, Category="Source")
	int32 SourceLODIndex = INDEX_NONE;

	UPROPERTY(BlueprintReadWrite, Category="Simulation")
	bool bEnableSimulation = false;

	UPROPERTY(BlueprintReadWrite, Category="Simulation")
	FName SourceNiagaraSolver = NAME_None;

	UPROPERTY(BlueprintReadWrite, Category="Simulation")
	TObjectPtr<UNiagaraSystem> NiagaraSystem = nullptr;
};

USTRUCT(BlueprintType)
struct FEnhancedHairCardsSimulationSettings
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category="Simulation")
	bool bUseSourceGroomNiagara = true;

	UPROPERTY(BlueprintReadWrite, Category="Simulation")
	bool bShowSourceGroomSimulation = true;

	UPROPERTY(BlueprintReadWrite, Category="Simulation")
	TArray<FEnhancedHairCardsNiagaraSimulationGroup> Groups;

	bool HasEnabledSimulation() const
	{
		return Groups.ContainsByPredicate([](const FEnhancedHairCardsNiagaraSimulationGroup& Group)
		{
			return Group.bEnableSimulation && Group.NiagaraSystem != nullptr;
		});
	}
};

UCLASS(BlueprintType)
class ENHANCEDHAIRCARDS_API UEnhancedHairCardsAsset : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Source")
	TObjectPtr<UGroomAsset> SourceGroom = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Cards")
	TArray<FEnhancedHairCardsPart> Parts;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Preview")
	FEnhancedHairCardsPreviewSettings PreviewSettings;

	UPROPERTY(BlueprintReadWrite, Category="Simulation")
	FEnhancedHairCardsSimulationSettings SimulationSettings;

	UFUNCTION(BlueprintPure, Category="Enhanced Hair Cards")
	int32 GetNumParts() const { return Parts.Num(); }

	UFUNCTION(BlueprintPure, Category="Enhanced Hair Cards")
	int32 GetTotalGuideCurveCount() const;

	UFUNCTION(BlueprintPure, Category="Enhanced Hair Cards|Simulation")
	int32 GetNumSimulationGroups() const { return SimulationSettings.Groups.Num(); }

	UFUNCTION(BlueprintPure, Category="Enhanced Hair Cards|Simulation")
	int32 GetNumGuideSimulationParts() const;

	UFUNCTION(BlueprintPure, Category="Enhanced Hair Cards|Simulation")
	bool HasGuideSimulationEnabled() const;

	UFUNCTION(BlueprintCallable, Category="Enhanced Hair Cards|Simulation")
	void SetAllGuideSimulationEnabled(bool bEnabled);
};
