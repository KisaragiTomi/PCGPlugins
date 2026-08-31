#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/InputChord.h"
#include "InputCoreTypes.h"
#include "EditorShortcutTypes.generated.h"

/**
 * One key chord and the function it calls.
 *
 * A binding carries no data of its own beyond which function to call. Whatever a
 * shortcut needs to know is written into that function, so the only parameters a
 * bound function may declare are the ones the selection fills.
 */
USTRUCT(BlueprintType)
struct EDITORSHORTCUTS_API FEditorShortcutBinding
{
	GENERATED_BODY()

	/** Shown in logs and notifications. Falls back to the function name when empty. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shortcut")
	FString Label;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shortcut")
	bool bEnabled = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shortcut")
	FInputChord Chord;

	/**
	 * Class that owns the function: a Blueprint function library, a Blueprint class,
	 * or any native class.
	 *
	 * Leave it empty to call FunctionName on each selected actor instead.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Action")
	TSoftClassPtr<UObject> TargetClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Action")
	FName FunctionName;

	/** Wrap the call in an undo transaction. Calls that change nothing leave no undo entry. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Behaviour")
	bool bTransactional = true;

	/** Do nothing when no actor is selected. Turn off for shortcuts that ignore the selection. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Behaviour")
	bool bRequiresSelection = true;

	/** Show a toast in the bottom right corner after the call. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Behaviour")
	bool bShowNotification = true;

	/** Label, or a generated one when Label is empty. */
	FString GetDisplayLabel() const;

	/** Checks what can be checked without loading the target class. */
	bool Validate(FString& OutError) const;
};
