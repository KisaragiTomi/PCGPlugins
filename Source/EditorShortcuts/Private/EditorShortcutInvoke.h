#pragma once

#include "CoreMinimal.h"

class AActor;
class UClass;
class UFunction;
class UObject;

/**
 * The one calling convention the shortcut system understands.
 *
 * A bound function may declare only parameters the selection can fill:
 *   TArray<AActor*> (or an array of any AActor subclass) - the level editor selection
 *   AActor* (or any AActor subclass)                     - the actor currently being processed
 *
 * Anything else is refused, because a binding has no way to supply it. A function
 * that needs a value takes it from its own body, or from a wrapper function that
 * hard-codes the value and forwards the selection.
 */
namespace EditorShortcutInvoke
{
	/** Finds FunctionName on Target's class, naming plausible alternatives when it is not there. */
	UFunction* FindCallableFunction(UObject* Target, FName FunctionName, FString& OutError);

	/**
	 * True when every parameter of Function can be filled from the selection.
	 * OutError names the parameters that cannot, so a binding can be checked without running it.
	 */
	bool IsBindable(const UFunction* Function, FString& OutError);

	/** Calls Function on Target. Returns false, with OutError set, when its signature is not bindable. */
	bool CallFunction(UObject* Target, UFunction* Function, const TArray<AActor*>& SelectedActors, AActor* SingleActor, FString& OutError);
}
