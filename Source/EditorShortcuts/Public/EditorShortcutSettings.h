#pragma once

#include "CoreMinimal.h"
#include "EditorShortcutTypes.h"
#include "Engine/DeveloperSettings.h"
#include "EditorShortcutSettings.generated.h"

/** Project settings for the editor shortcut system. Editor > Project Settings > Plugins > Editor Shortcuts. */
UCLASS(config = EditorPerProjectUserSettings, defaultconfig, BlueprintType, meta = (DisplayName = "Editor Shortcuts"))
class EDITORSHORTCUTS_API UEditorShortcutSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** Master switch for the whole system. */
	UPROPERTY(config, EditAnywhere, Category = "Editor Shortcuts")
	bool bEnabled = true;

	/**
	 * Skip dispatch while a text field owns keyboard focus, so shortcuts cannot fire
	 * mid-rename in the outliner or while typing into a details field.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Editor Shortcuts")
	bool bIgnoreWhileTypingInTextField = true;

	/** Log every dispatch, including chords that matched nothing. */
	UPROPERTY(config, EditAnywhere, Category = "Editor Shortcuts", meta = (DisplayName = "Verbose Logging"))
	bool bVerboseLogging = false;

	UPROPERTY(config, EditAnywhere, BlueprintReadOnly, Category = "Editor Shortcuts", meta = (TitleProperty = "Label"))
	TArray<FEditorShortcutBinding> Bindings;

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	virtual FName GetSectionName() const override { return TEXT("EditorShortcuts"); }
};
