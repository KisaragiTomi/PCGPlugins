#pragma once

#include "CoreMinimal.h"
#include "EditorShortcutTypes.h"
#include "EditorSubsystem.h"
#include "EditorShortcutSubsystem.generated.h"

class AActor;

/** Outcome of running one binding, used for the toast and the log. */
struct FEditorShortcutResult
{
	/** True once the binding was recognised, whether or not it changed anything. Drives key consumption. */
	bool bHandled = false;

	int32 AffectedActors = 0;
	FString Message;
	FString Error;

	bool HasError() const { return !Error.IsEmpty(); }
};

/**
 * Runs shortcut bindings.
 *
 * Every binding does the same thing: call a function, with the selection fed into
 * whichever parameters take actors. Execution is exposed to Blueprint and Python so
 * a binding can be run and checked without pressing its key.
 */
UCLASS()
class EDITORSHORTCUTS_API UEditorShortcutSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	static UEditorShortcutSubsystem* Get();

	virtual void Deinitialize() override;

	/** Runs the binding at BindingIndex in UEditorShortcutSettings::Bindings. */
	UFUNCTION(BlueprintCallable, Category = "Editor Shortcuts")
	bool ExecuteBindingByIndex(int32 BindingIndex);

	/** Runs the first enabled binding whose label matches, case insensitively. */
	UFUNCTION(BlueprintCallable, Category = "Editor Shortcuts")
	bool ExecuteBindingByLabel(const FString& Label);

	/** Runs a binding that is not in the settings list, for one-off calls and tests. */
	UFUNCTION(BlueprintCallable, Category = "Editor Shortcuts")
	bool RunBinding(const FEditorShortcutBinding& Binding);

	/**
	 * Resolves every configured binding without running it and returns one line per problem:
	 * a missing class, a function that is not there, an argument that will not parse.
	 * Empty means every binding is wired up correctly.
	 */
	UFUNCTION(BlueprintCallable, Category = "Editor Shortcuts")
	TArray<FString> ValidateAllBindings();

	/** Runs Binding against the current level editor selection. */
	FEditorShortcutResult ExecuteBinding(const FEditorShortcutBinding& Binding);

	/** Drops cached target instances, so a recompiled Blueprint is picked up on the next press. */
	UFUNCTION(BlueprintCallable, Category = "Editor Shortcuts")
	void FlushTargetInstances();

private:
	/** Loads TargetClass and picks the object the call should run on. */
	UObject* ResolveTarget(const FEditorShortcutBinding& Binding, FString& OutError);

	UObject* GetOrCreateInstance(UClass* Class);

	FEditorShortcutResult CallOnSelectedActors(const FEditorShortcutBinding& Binding, const TArray<AActor*>& SelectedActors);
	FEditorShortcutResult CallOnTargetObject(const FEditorShortcutBinding& Binding, const TArray<AActor*>& SelectedActors);

	UPROPERTY(Transient)
	TMap<TObjectPtr<UClass>, TObjectPtr<UObject>> InstanceCache;
};
