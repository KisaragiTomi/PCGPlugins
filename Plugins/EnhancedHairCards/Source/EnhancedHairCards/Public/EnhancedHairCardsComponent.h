#pragma once

#include "CoreMinimal.h"
#include "Components/MeshComponent.h"
#include "EnhancedHairCardsVertexFactory.h"
#include "EnhancedHairCardsDatas.h"
#include "EnhancedHairCardsComponent.generated.h"

struct FEnhancedHairCardsPart;
class UGroomComponent;

UCLASS(ClassGroup=(Rendering), meta=(BlueprintSpawnableComponent))
class ENHANCEDHAIRCARDS_API UEnhancedHairCardsComponent : public UMeshComponent
{
	GENERATED_BODY()

public:
	UEnhancedHairCardsComponent(const FObjectInitializer& ObjectInitializer);

	// ---- Data source ----

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hair Cards")
	UStaticMesh* SourceMesh = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hair Cards|Source")
	int32 SourceGroupIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hair Cards|Source")
	int32 SourceLODIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hair Cards|Source")
	int32 SourceCardsDescriptionIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hair Cards|Source",
		meta=(ToolTip="Optional source Groom component. When valid, EnhancedHairCards renders from the Groom card deformed buffers so the original Groom simulation/deformation pipeline drives the cards."))
	TObjectPtr<UGroomComponent> SourceGroomComponent = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hair Cards|Source",
		meta=(ToolTip="Prefer the source Groom component's original card deformation buffers over the local preview guide simulation. Rendering still uses EnhancedHairCards."))
	bool bUseSourceGroomSimulation = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hair Cards")
	FEnhancedHairCardsSettings CardSettings;

	UPROPERTY(BlueprintReadOnly, Category = "Hair Cards|Guides",
		meta=(ToolTip="Rest guide curves copied from the source Groom card LOD. Stored in component local space for future simulation/interpolation work."))
	TArray<FEnhancedHairCardsGuideCurve> GuideCurves;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hair Cards|Custom Data",
		meta=(ToolTip="Per-primitive float data accessible in material via GetCustomPrimitiveData node"))
	TArray<float> CustomPrimitiveFloats;

	// ---- Blueprint helpers ----

	UFUNCTION(BlueprintCallable, Category = "Hair Cards")
	void SetCustomDataFloat(int32 Index, float Value);

	UFUNCTION(BlueprintCallable, Category = "Hair Cards|Dynamics")
	void SetDynamicsEnabled(bool bEnabled);

	UFUNCTION(BlueprintCallable, Category = "Hair Cards|Dynamics")
	void SetDynamicsStrength(float Strength);

	UFUNCTION(BlueprintCallable, Category = "Hair Cards|Dynamics")
	void SetDynamicsWind(FVector LocalDirection, float WindStrength);

	UFUNCTION(BlueprintCallable, Category = "Hair Cards|Dynamics")
	void SetGuideSimulationEnabled(bool bEnabled);

	UFUNCTION(BlueprintCallable, Category = "Hair Cards|Dynamics")
	void ResetGuideSimulation();

	UFUNCTION(BlueprintCallable, Category = "Hair Cards|Source")
	void SetSourceGroomComponent(UGroomComponent* InSourceGroomComponent);

	UFUNCTION(BlueprintCallable, Category = "Hair Cards|Debug")
	void SetGuideDebugEnabled(bool bEnabled);

	void ApplyCardSettings(const FEnhancedHairCardsSettings& InCardSettings, bool bResetGuideSimulation);
	void ApplyHairCardsPart(const FEnhancedHairCardsPart& Part);

	UFUNCTION(BlueprintCallable, Category = "Hair Cards")
	void RebuildRenderData();

	// ---- UMeshComponent interface ----

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual int32 GetNumMaterials() const override;
	virtual UMaterialInterface* GetMaterial(int32 ElementIndex) const override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void SendRenderDynamicData_Concurrent() override;

protected:
	virtual void OnRegister() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	friend class FEnhancedHairCardsSceneProxy;

	void InvalidateCachedLocalBounds();
	FBoxSphereBounds GetCachedLocalBounds() const;
	bool ShouldUseSourceGroomSimulation() const;
	bool HasUsableSourceGroomCardsResource() const;
	bool ShouldRunGuideSimulation() const;
	void UpdateGuideSimulationTickState();
	void RebuildGuideSimulationState(bool bForceReset);
	bool SimulateGuideCurves(float DeltaTime);

	mutable bool bCachedLocalBoundsValid = false;
	mutable FBoxSphereBounds CachedLocalBounds;
	bool bLastSourceGroomCardsResourceUsable = false;

	struct FGuideSimulationCurveState
	{
		int32 PointOffset = 0;
		int32 PointCount = 0;
	};

	bool bGuideSimulationInitialized = false;
	TArray<FGuideSimulationCurveState> GuideSimulationCurveStates;
	TArray<FEnhancedHairCardsGuideCurve> RestSimulationGuideCurves;
	TArray<FEnhancedHairCardsGuideCurve> SimulatedGuideCurves;
	TArray<FVector> SimulatedPreviousPoints;
	FTransform LastGuideSimulationComponentTransform = FTransform::Identity;
};
