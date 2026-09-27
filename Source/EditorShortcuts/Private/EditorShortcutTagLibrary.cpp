#include "EditorShortcutTagLibrary.h"

#include "Components/TextRenderComponent.h"
#include "Containers/Ticker.h"
#include "EditorShortcutSettings.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "LevelEditorViewport.h"

namespace
{
/**
 * The "+Tag" / "-Tag" label standing on top of an actor that just changed.
 *
 * Ported back from the pre-2026-08-31 `PCGEditorProcess/ActorTagShortcut`. The corner toast
 * says *that* something happened; this says **which actors** it happened to, which is the part
 * that matters when a dozen props are selected and only some of them had the tag.
 *
 * Placement and size follow the actor's bounds (2026-09-23, user: "大小为目标 actor bound",
 * then "文字需要在物体 bound 的最上方"): the label stands **on the top face of the bounding box**,
 * centred on it, its baseline on the top, and is **as wide as the box is across the view**, so
 * a doorknob and a house each get a label their own size. Only the width is matched: standing
 * above the actor, the label has no height to fit into, and matching the box's height as well
 * would shrink the label on anything flat (a floor would get a word a few centimetres tall).
 * Sitting above the bounds also keeps it clear of the actor's own geometry, which would
 * otherwise hide it (TextRender is depth-tested like any mesh).
 *
 * ⚠️ Both the spawn and the destroy are hidden from the undo transaction (`TGuardValue` on
 * `GUndo`). A binding runs inside `FScopedTransaction` by default, and `UWorld::SpawnActor`
 * does `if (GUndo) ModifyLevel(...)` (`LevelActor.cpp:735`) — so without this, undoing the tag
 * change would restore a level whose actor list still holds a label that destroyed itself two
 * seconds ago, and the spawn alone would dirty the map. The destroy passes
 * `bShouldModifyLevel = false` for the same reason: `RemoveActor` would otherwise `Modify()`
 * the level, and `Modify` dirties the package whether or not a transaction is open.
 */
void SpawnTagIndicator(AActor* TargetActor, FName Tag, bool bAdded, float Seconds)
{
	UWorld* World = TargetActor ? TargetActor->GetWorld() : nullptr;
	if (!World) return;

	FVector Origin;
	FVector Extent;
	TargetActor->GetActorBounds(/*bOnlyCollidingComponents=*/false, Origin, Extent);
	// Nothing to bound (an empty actor, a bare scene component): stand a half-metre box on its pivot
	// so there is still something to see.
	if (Extent.GetMax() < 1.0)
	{
		Origin = TargetActor->GetActorLocation();
		Extent = FVector(25.0);
	}

	// Horizontal direction from the viewport being worked in to the actor. The label is turned to
	// face back along it (yaw only, so it stays upright): a text actor left at zero rotation is
	// edge-on or back-facing from most of the angles you actually look from. No viewport (a script)
	// = as if looking along +X.
	FVector Toward = FVector::ForwardVector;
	if (const FEditorViewportClient* Viewport = GCurrentLevelEditingViewportClient)
	{
		const FVector Flat(Origin.X - Viewport->GetViewLocation().X, Origin.Y - Viewport->GetViewLocation().Y, 0.0);
		if (!Flat.IsNearlyZero()) Toward = Flat.GetSafeNormal();
	}
	const FVector Across(-Toward.Y, Toward.X, 0.0);   // the label's width direction

	// How wide the bounding box is as the viewer sees it, across the view.
	const double BoxWidth = 2.0 * (Extent.X * FMath::Abs(Across.X) + Extent.Y * FMath::Abs(Across.Y));

	// Centre of the box's top face, a hair above it so the label never z-fights a flat top.
	// ⚠️ The label's +X must point **at the viewer**: TextRender builds its quads with the normal
	// on local +X (`TangentZ = (1,0,0)`, TextRenderComponent.cpp:1126) and the default text material
	// is one-sided, so a label turned the other way is back-face culled — present, but invisible.
	const FVector Location(Origin.X, Origin.Y, Origin.Z + Extent.Z + 1.0);
	const FRotator Rotation(0.0, (-Toward).Rotation().Yaw, 0.0);

	FActorSpawnParameters Params;
	// Transient: never saved, and `MarkPackageDirty` early-outs on transient objects, so the
	// label cannot dirty the map on its own.
	Params.ObjectFlags = RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
#if WITH_EDITOR
	// Two seconds of flicker in the outliner is just noise. (`AActor::bListedInSceneOutliner` is
	// protected; this spawn flag is the public way in and has to be set before the spawn.)
	Params.bHideFromSceneOutliner = true;
#endif

	AActor* Indicator = nullptr;
	{
		TGuardValue<ITransaction*> NoTransaction(GUndo, nullptr);
		Indicator = World->SpawnActor<AActor>(AActor::StaticClass(), Location, Rotation, Params);
	}
	if (!Indicator) return;

#if WITH_EDITOR
	Indicator->bIsEditorOnlyActor = true;
#endif

	UTextRenderComponent* Text = NewObject<UTextRenderComponent>(Indicator, TEXT("TagText"), RF_Transient);
	Indicator->SetRootComponent(Text);
	// ⚠️ A bare AActor has no root component when it spawns, so the transform passed to SpawnActor
	// went nowhere: the new root starts at identity and the label would stand at the world origin.
	// (The original ActorTagShortcut did SetActorLocation after making its root for this reason;
	// the first port dropped it and the label turned up at (0,0,0).)
	Text->SetWorldLocationAndRotation(Location, Rotation);
	Text->SetText(FText::FromString(FString::Printf(TEXT("%s%s"), bAdded ? TEXT("+") : TEXT("-"), *Tag.ToString())));
	Text->SetTextRenderColor(bAdded ? FColor::Green : FColor::Red);
	// Centred across the box, growing upward from its top face.
	Text->SetHorizontalAlignment(EHTA_Center);
	Text->SetVerticalAlignment(EVRTA_TextBottom);
	// As wide as the box. The string's measured size (local Y = width, Z = height) is linear in
	// WorldSize, so measure it at 1 and scale.
	Text->SetWorldSize(1.0f);
	const FVector Unit = Text->GetTextLocalSize();
	Text->SetWorldSize(float(BoxWidth / FMath::Max(Unit.Y, UE_KINDA_SMALL_NUMBER)));
	Text->RegisterComponent();

	// The world's timer manager only runs while the editor world ticks; the core ticker always
	// does, so a label cannot get stuck on screen when ticking is throttled.
	TWeakObjectPtr<AActor> WeakIndicator(Indicator);
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakIndicator](float)
	{
		if (AActor* Alive = WeakIndicator.Get())
		{
			TGuardValue<ITransaction*> NoTransaction(GUndo, nullptr);
			if (UWorld* AliveWorld = Alive->GetWorld()) AliveWorld->EditorDestroyActor(Alive, /*bShouldModifyLevel=*/false);
		}
		return false;   // one shot
	}), FMath::Max(Seconds, 0.1f));
}

/** Modify() is what makes the change undoable; MarkPackageDirty() is what makes it savable. */
int32 ApplyTag(const TArray<AActor*>& Actors, FName Tag, bool bAddWhereMissing, bool bRemoveWherePresent)
{
	if (Tag.IsNone()) return 0;

	const UEditorShortcutSettings* Settings = GetDefault<UEditorShortcutSettings>();
	const bool bIndicator = !Settings || Settings->bShowTagIndicator;
	const float IndicatorSeconds = Settings ? Settings->TagIndicatorSeconds : 2.0f;

	int32 Changed = 0;
	for (AActor* Actor : Actors)
	{
		if (!Actor) continue;

		const bool bHasTag = Actor->Tags.Contains(Tag);
		const bool bShouldAdd = bAddWhereMissing && !bHasTag;
		const bool bShouldRemove = bRemoveWherePresent && bHasTag;
		if (!bShouldAdd && !bShouldRemove) continue;

		Actor->Modify();
		if (bShouldAdd) Actor->Tags.Add(Tag);
		else Actor->Tags.Remove(Tag);
		Actor->MarkPackageDirty();
		// Only actors that actually changed get a label, so the screen tells you the same thing
		// the return value does: what this press did, not what was selected.
		if (bIndicator) SpawnTagIndicator(Actor, Tag, bShouldAdd, IndicatorSeconds);
		++Changed;
	}

	return Changed;
}
} // namespace

int32 UEditorShortcutTagLibrary::AddTagToActors(const TArray<AActor*>& Actors, FName Tag)
{
	return ApplyTag(Actors, Tag, /*bAddWhereMissing=*/true, /*bRemoveWherePresent=*/false);
}

int32 UEditorShortcutTagLibrary::RemoveTagFromActors(const TArray<AActor*>& Actors, FName Tag)
{
	return ApplyTag(Actors, Tag, /*bAddWhereMissing=*/false, /*bRemoveWherePresent=*/true);
}

int32 UEditorShortcutTagLibrary::ToggleTagOnActors(const TArray<AActor*>& Actors, FName Tag)
{
	return ApplyTag(Actors, Tag, /*bAddWhereMissing=*/true, /*bRemoveWherePresent=*/true);
}
