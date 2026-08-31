#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "EditorShortcutTagLibrary.generated.h"

class AActor;

/**
 * Actor tag operations for Blueprints to call.
 *
 * These take a tag, so a shortcut cannot bind to them directly; bind to a function in
 * an Editor Utility Blueprint parented on EditorFunctionLibrary, which hard-codes the
 * tag and forwards the selection:
 *
 *     ToggleCSSW(Actors) -> ToggleTagOnActors(Actors, "CSSW")
 *
 * They exist because Modify() is not reachable from Blueprint, and without it a tag
 * change made in Blueprint would not be undoable.
 *
 * Each returns how many actors it changed.
 */
UCLASS()
class EDITORSHORTCUTS_API UEditorShortcutTagLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Editor Shortcuts|Tags")
	static int32 AddTagToActors(const TArray<AActor*>& Actors, FName Tag);

	UFUNCTION(BlueprintCallable, Category = "Editor Shortcuts|Tags")
	static int32 RemoveTagFromActors(const TArray<AActor*>& Actors, FName Tag);

	/** Adds the tag where it is missing and removes it where it is present. */
	UFUNCTION(BlueprintCallable, Category = "Editor Shortcuts|Tags")
	static int32 ToggleTagOnActors(const TArray<AActor*>& Actors, FName Tag);
};
