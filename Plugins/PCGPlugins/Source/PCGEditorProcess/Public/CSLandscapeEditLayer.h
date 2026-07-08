#pragma once

#include "CoreMinimal.h"
#include "LandscapeEditLayer.h"
#include "LandscapeEditLayerRenderer.h"
#include "LandscapeEditTypes.h" // full definition of ELandscapeToolTargetType (LandscapeEditLayer.h only forward-declares it)
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
	// ULandscapeEditLayerProcedural leaves these ULandscapeEditLayerBase methods PURE_VIRTUAL.
	// They must be implemented here (unconditionally — the base declares them outside WITH_EDITOR),
	// otherwise the CDO hits the pure-virtual and the editor fatal-errors. e.g. SupportsMultiple()
	// is called from FLandscapeEditorCustomNodeBuilder_Layers::CreateLayer when adding the layer.
	virtual bool SupportsMultiple() const override { return true; }
	virtual bool SupportsTargetType(ELandscapeToolTargetType InType) const override { return InType == ELandscapeToolTargetType::Heightmap; }
	virtual bool NeedsPersistentTextures() const override { return false; } // purely procedural: rendered via the merge pipeline, no backing textures
	virtual FString GetDefaultName() const override { return TEXT("CS_Procedural"); }

#if WITH_EDITOR
	virtual TArray<UE::Landscape::EditLayers::FEditLayerRendererState> GetEditLayerRendererStates(
		const UE::Landscape::EditLayers::FMergeContext* InMergeContext) override;
#endif
};
