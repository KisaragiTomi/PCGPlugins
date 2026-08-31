#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "EditorShortcutSetupLibrary.generated.h"

/**
 * Generates the Blueprint function library a tag shortcut binds to.
 *
 * A binding cannot pass a tag, so every tag needs its own no-argument function.
 * Writing those by hand is tedious and easy to get subtly wrong, so this builds
 * them: one function per tag, each forwarding the selection to
 * UEditorShortcutTagLibrary::ToggleTagOnActors with the tag baked in.
 *
 * The asset is an Editor Utility Blueprint parented on EditorFunctionLibrary, which
 * is the only Blueprint shape this system uses. It is exactly what the editor makes
 * for Editor Utilities > Editor Utility Blueprint with that parent picked, so a
 * generated library and a hand-made one behave identically.
 *
 * Run it again with a longer tag list to add more; existing functions with the
 * same names are rebuilt, anything else in the asset is left alone.
 */
UCLASS()
class EDITORSHORTCUTS_API UEditorShortcutSetupLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Creates or updates a Blueprint function library holding one Toggle<Tag> function per tag.
	 *
	 * @param PackageName  where to put it, e.g. /PCGPlugins/EditorShortcuts/BPFL_ShortcutActions
	 * @param Tags         tags to generate functions for
	 * @param bSave        write the package to disk
	 * @return             the generated class path to put in a binding's TargetClass, empty on failure
	 */
	UFUNCTION(BlueprintCallable, Category = "Editor Shortcuts|Setup")
	static FString CreateTagToggleLibrary(const FString& PackageName, const TArray<FName>& Tags, bool bSave = true);
};
