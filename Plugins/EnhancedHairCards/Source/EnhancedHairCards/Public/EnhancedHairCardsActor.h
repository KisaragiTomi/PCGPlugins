#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "EnhancedHairCardsActor.generated.h"

class UEnhancedHairCardsAsset;
class UEnhancedHairCardsComponent;
class UGroomComponent;
class UNiagaraComponent;
class USceneComponent;

UCLASS(BlueprintType, HideCategories=(Input, Replication), showcategories=("Input|MouseInput", "Input|TouchInput"))
class ENHANCEDHAIRCARDS_API AEnhancedHairCardsActor : public AActor
{
	GENERATED_BODY()

public:
	AEnhancedHairCardsActor(const FObjectInitializer& ObjectInitializer);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Enhanced Hair Cards")
	TObjectPtr<USceneComponent> SceneRoot = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Enhanced Hair Cards")
	TObjectPtr<UEnhancedHairCardsAsset> HairCardsAsset = nullptr;

	UPROPERTY(Transient, BlueprintReadOnly, Category="Enhanced Hair Cards")
	TArray<TObjectPtr<UEnhancedHairCardsComponent>> HairCardsComponents;

	UPROPERTY(Transient, BlueprintReadOnly, Category="Enhanced Hair Cards|Simulation")
	TObjectPtr<UGroomComponent> SourceGroomSimulationComponent = nullptr;

	UPROPERTY(Transient, BlueprintReadOnly, Category="Enhanced Hair Cards|Simulation")
	TArray<TObjectPtr<UNiagaraComponent>> NiagaraSimulationComponents;

	UFUNCTION(BlueprintCallable, Category="Enhanced Hair Cards")
	void SetHairCardsAsset(UEnhancedHairCardsAsset* InHairCardsAsset);

	UFUNCTION(BlueprintCallable, Category="Enhanced Hair Cards")
	void RebuildComponentsFromAsset();

	UFUNCTION(BlueprintCallable, Category="Enhanced Hair Cards")
	void RefreshComponentsFromAsset(bool bForceRebuild = false);

	UFUNCTION(BlueprintCallable, Category="Enhanced Hair Cards|Simulation")
	void SetGuideSimulationEnabled(bool bEnabled);

	UFUNCTION(BlueprintCallable, Category="Enhanced Hair Cards|Simulation")
	void ResetGuideSimulation();

	UFUNCTION(BlueprintCallable, Category="Enhanced Hair Cards|Simulation")
	void SetDynamicsStrength(float Strength);

	UFUNCTION(BlueprintCallable, Category="Enhanced Hair Cards|Simulation")
	void SetDynamicsWind(FVector LocalDirection, float WindStrength);

	const TArray<TObjectPtr<UEnhancedHairCardsComponent>>& GetHairCardsComponents() const
	{
		return HairCardsComponents;
	}

	virtual void OnConstruction(const FTransform& Transform) override;

#if WITH_EDITOR
	virtual bool GetReferencedContentObjects(TArray<UObject*>& Objects) const override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	UPROPERTY(Transient)
	TObjectPtr<UEnhancedHairCardsAsset> BuiltHairCardsAsset = nullptr;

	UPROPERTY(Transient)
	int32 BuiltPartCount = INDEX_NONE;

	UPROPERTY(Transient)
	int32 BuiltSimulationGroupCount = INDEX_NONE;

	void ApplyAssetPreviewSettings();
	bool IsBuiltFromCurrentAsset() const;
};
