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

	/**
	 * Stand a short-lived "+Tag" / "-Tag" label on top of every actor a tag call changed, as wide as its bounds.
	 *
	 * The corner toast says *that* something happened; this says **which actors** it happened
	 * to, which is the part that matters when a dozen props are selected.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Editor Shortcuts|Tag Indicator")
	bool bShowTagIndicator = true;

	/** How long a label stays up. */
	UPROPERTY(config, EditAnywhere, Category = "Editor Shortcuts|Tag Indicator", meta = (ClampMin = "0.1", ClampMax = "30.0", EditCondition = "bShowTagIndicator"))
	float TagIndicatorSeconds = 2.0f;

	UPROPERTY(config, EditAnywhere, BlueprintReadOnly, Category = "Editor Shortcuts", meta = (TitleProperty = "Label"))
	TArray<FEditorShortcutBinding> Bindings;

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	virtual FName GetSectionName() const override { return TEXT("EditorShortcuts"); }
};
