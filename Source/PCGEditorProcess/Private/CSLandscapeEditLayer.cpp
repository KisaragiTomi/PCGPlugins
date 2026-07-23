#include "CSLandscapeEditLayer.h"
#include "CSLandscapeEditLayerBase.h"
#include "EngineUtils.h"
#include "Landscape.h"

#if WITH_EDITOR
TArray<UE::Landscape::EditLayers::FEditLayerRendererState> UCSLandscapeEditLayer::GetEditLayerRendererStates(
	const UE::Landscape::EditLayers::FMergeContext* InMergeContext)
{
	using namespace UE::Landscape::EditLayers;

	TArray<FEditLayerRendererState> States;

	ALandscape* Landscape = GetTypedOuter<ALandscape>();
	if (!Landscape) return States;

	UWorld* World = Landscape->GetWorld();
	if (!World) return States;

	const FGuid MyGuid = GetGuid();

	// Only return the Actor that owns this specific Edit Layer
	for (TActorIterator<ACSLandscapeEditLayerBase> It(World); It; ++It)
	{
		ACSLandscapeEditLayerBase* Actor = *It;
		if (!IsValid(Actor)) continue;
		if (Actor->OwnedEditLayerGuid != MyGuid) continue;
		TScriptInterface<ILandscapeEditLayerRenderer> Renderer(Actor);
		if (Renderer) States.Emplace(InMergeContext, Renderer);
	}

	return States;
}
#endif
