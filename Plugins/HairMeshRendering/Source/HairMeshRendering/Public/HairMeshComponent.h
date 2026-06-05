#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "HairMeshStylingParams.h"
#include "HairMeshTextureBuilder.h"
#include "HairMeshComponent.generated.h"

class UHairMeshAsset;

/**
 * Scene component that attaches a hair-mesh model to an actor.
 * Owns the CPU-side texture data; the scene proxy creates GPU resources.
 */
UCLASS(ClassGroup=(Rendering), meta=(BlueprintSpawnableComponent))
class HAIRMESHRENDERING_API UHairMeshComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UHairMeshComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HairMesh")
	TObjectPtr<UHairMeshAsset> HairMeshAsset;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HairMesh|Styling")
	FHairMeshStylingParams StylingParams;

	// --- LOD ---

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HairMesh|LOD", meta=(ClampMin="0.01"))
	float StrandLODAlpha = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HairMesh|LOD", meta=(ClampMin="0.01"))
	float VertexLODAlpha = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HairMesh|LOD", meta=(ClampMin="0", ClampMax="1"))
	float LODTransitionLambda = 0.3f;

	/** Read-only: built texture set, accessible by scene proxy on creation */
	const FHairMeshTextureSet& GetTextureSet() const { return TextureSet; }

	UFUNCTION(BlueprintCallable, Category="HairMesh")
	void RebuildTextures();

	// --- UPrimitiveComponent interface ---
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;

protected:
	virtual void OnRegister() override;
	virtual void OnUnregister() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	FHairMeshTextureSet TextureSet;
};
