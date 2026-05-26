#include "CSLandscapeEditLayer.h"
#include "ComputeShaderLandscape.h"
#include "ComputeShaderLandscapeLayer.h"
#include "ComputeShaderLandscapeTempLayer.h"
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
	auto TryAdd = [&](auto* Actor)
	{
		if (!IsValid(Actor)) return;
		if (Actor->OwnedEditLayerGuid != MyGuid) return;
		TScriptInterface<ILandscapeEditLayerRenderer> Renderer(Actor);
		if (Renderer) States.Emplace(InMergeContext, Renderer);
	};

	for (TActorIterator<ACSLandscape> It(World); It; ++It) TryAdd(*It);
	for (TActorIterator<ACSLandscapeLayer> It(World); It; ++It) TryAdd(*It);
	for (TActorIterator<ACSLandscapeTempLayer> It(World); It; ++It) TryAdd(*It);

	return States;
}
#endif
