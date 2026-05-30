#pragma once

#include "CoreMinimal.h"
#include "LandscapeEditLayer.h"
#include "LandscapeEditLayerRenderer.h"
#include "CSLandscapeEditLayer.generated.h"

class ACSLandscapeEditor;

/**
 * Custom procedural Edit Layer that delegates rendering to ACSLandscapeEditor actors in the scene.
 * Each instance of this layer collects associated CS actors and feeds them into the Merge pipeline.
 */
UCLASS(MinimalAPI, meta = (DisplayName = "CS Procedural Edit Layer"))
class UCSLandscapeEditLayer : public ULandscapeEditLayerProcedural
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	virtual FString GetDefaultName() const override { return TEXT("CS_Procedural"); }

	virtual TArray<UE::Landscape::EditLayers::FEditLayerRendererState> GetEditLayerRendererStates(
		const UE::Landscape::EditLayers::FMergeContext* InMergeContext) override;
#endif
};
