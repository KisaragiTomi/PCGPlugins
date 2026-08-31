#include "EditorShortcutInvoke.h"

#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/Class.h"
#include "UObject/Script.h"
#include "UObject/UnrealType.h"

namespace EditorShortcutInvoke
{
namespace
{
/** True for TArray<AActor*> and arrays of any AActor subclass. */
bool IsActorArrayParam(const FProperty* Property)
{
	const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property);
	if (!ArrayProperty) return false;

	const FObjectPropertyBase* Inner = CastField<FObjectPropertyBase>(ArrayProperty->Inner);
	return Inner && Inner->PropertyClass && Inner->PropertyClass->IsChildOf(AActor::StaticClass());
}

/** True for AActor* and any AActor subclass pointer. */
bool IsActorParam(const FProperty* Property)
{
	const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);
	return ObjectProperty && ObjectProperty->PropertyClass && ObjectProperty->PropertyClass->IsChildOf(AActor::StaticClass());
}

/** A declared default makes a parameter optional, so leaving it alone is what the author intended. */
bool HasDeclaredDefault(const UFunction* Function, const FProperty* Property)
{
	return Function->HasMetaData(*(TEXT("CPP_Default_") + Property->GetName()));
}

/**
 * The world context parameter, which the author never sees.
 *
 * Compiling a Blueprint function library adds a hidden __WorldContext to every
 * function, so this is not an edge case: without it, no Blueprint function library
 * would be bindable at all.
 */
bool IsWorldContextParam(const UFunction* Function, const FProperty* Property)
{
	if (Property->GetName() == TEXT("__WorldContext")) return true;

	const FString* DeclaredName = Function->FindMetaData(TEXT("WorldContext"));
	return DeclaredName && *DeclaredName == Property->GetName();
}

/** True for the parameters the caller fills rather than the binding. */
bool IsSuppliedBySystem(const UFunction* Function, const FProperty* Property)
{
	return IsActorArrayParam(Property) || IsActorParam(Property) || IsWorldContextParam(Function, Property);
}

/** True when the selection reaches this function at all, used to rank suggestions. */
bool TakesSelection(const UFunction* Function)
{
	for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		if (IsActorArrayParam(*It) || IsActorParam(*It)) return true;
	}
	return false;
}

/** Names the functions on Class that a shortcut could plausibly have meant. */
FString DescribeCandidates(const UClass* Class)
{
	TArray<FString> Names;
	for (TFieldIterator<UFunction> It(Class); It && Names.Num() < 8; ++It)
	{
		if (It->HasAnyFunctionFlags(FUNC_Delegate)) continue;
		if (!It->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintEvent)) continue;
		if (!TakesSelection(*It)) continue;

		FString Unused;
		if (!IsBindable(*It, Unused)) continue;

		Names.Add(It->GetName());
	}

	if (Names.IsEmpty()) return FString();

	return FString::Printf(TEXT(" Bindable functions there: %s"), *FString::Join(Names, TEXT(", ")));
}

/** Releases the parameter frame built by CallFunction. */
void DestroyFrame(UFunction* Function, uint8* Frame)
{
	for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		It->DestroyValue_InContainer(Frame);
	}
	FMemory::Free(Frame);
}
} // namespace

bool IsBindable(const UFunction* Function, FString& OutError)
{
	if (!Function)
	{
		OutError = TEXT("no function");
		return false;
	}

	TArray<FString> Unfillable;
	for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_ReturnParm)) continue;
		if (IsSuppliedBySystem(Function, *It)) continue;
		if (HasDeclaredDefault(Function, *It)) continue;

		Unfillable.Add(FString::Printf(TEXT("%s (%s)"), *It->GetName(), *It->GetCPPType()));
	}

	if (Unfillable.IsEmpty()) return true;

	OutError = FString::Printf(
		TEXT("'%s' declares %s, which a shortcut cannot supply. Bind to a function that takes only the selection, ")
		TEXT("or wrap this one in a Blueprint function that hard-codes the value."),
		*Function->GetName(), *FString::Join(Unfillable, TEXT(", ")));
	return false;
}

UFunction* FindCallableFunction(UObject* Target, FName FunctionName, FString& OutError)
{
	if (!Target)
	{
		OutError = TEXT("no target object");
		return nullptr;
	}

	if (FunctionName.IsNone())
	{
		OutError = TEXT("FunctionName is empty");
		return nullptr;
	}

	UFunction* Function = Target->FindFunction(FunctionName);
	if (Function && !Function->HasAnyFunctionFlags(FUNC_Delegate)) return Function;

	if (Function) OutError = FString::Printf(TEXT("'%s' is a delegate, not a callable function"), *FunctionName.ToString());
	else OutError = FString::Printf(TEXT("'%s' has no function named '%s'.%s"), *Target->GetClass()->GetName(), *FunctionName.ToString(), *DescribeCandidates(Target->GetClass()));

	return nullptr;
}

bool CallFunction(UObject* Target, UFunction* Function, const TArray<AActor*>& SelectedActors, AActor* SingleActor, FString& OutError)
{
	if (!Target || !Function)
	{
		OutError = TEXT("no target object or function");
		return false;
	}

	if (!IsBindable(Function, OutError)) return false;

	// ParmsSize covers the return value too, so allocate and initialise the whole frame.
	const int32 FrameSize = FMath::Max<int32>(Function->ParmsSize, 1);
	uint8* Frame = static_cast<uint8*>(FMemory::Malloc(FrameSize));
	FMemory::Memzero(Frame, FrameSize);

	for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		It->InitializeValue_InContainer(Frame);
	}

	for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		FProperty* Param = *It;
		if (Param->HasAnyPropertyFlags(CPF_ReturnParm)) continue;

		if (IsActorArrayParam(Param))
		{
			FArrayProperty* ArrayProperty = CastFieldChecked<FArrayProperty>(Param);
			FObjectPropertyBase* Inner = CastFieldChecked<FObjectPropertyBase>(ArrayProperty->Inner);

			// The declared element class may be narrower than AActor, so drop what it cannot hold.
			TArray<AActor*> Accepted;
			Accepted.Reserve(SelectedActors.Num());
			for (AActor* Actor : SelectedActors)
			{
				if (Actor && Actor->IsA(Inner->PropertyClass)) Accepted.Add(Actor);
			}

			FScriptArrayHelper ArrayHelper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(Frame));
			ArrayHelper.Resize(Accepted.Num());
			for (int32 Index = 0; Index < Accepted.Num(); ++Index)
			{
				Inner->SetObjectPropertyValue(ArrayHelper.GetRawPtr(Index), Accepted[Index]);
			}
			continue;
		}

		if (IsActorParam(Param))
		{
			FObjectPropertyBase* ObjectProperty = CastFieldChecked<FObjectPropertyBase>(Param);
			AActor* Value = SingleActor && SingleActor->IsA(ObjectProperty->PropertyClass) ? SingleActor : nullptr;
			ObjectProperty->SetObjectPropertyValue_InContainer(Frame, Value);
			continue;
		}

		// Hand the hidden context the editor world, so world-context nodes inside the
		// function resolve instead of running against null.
		if (IsWorldContextParam(Function, Param))
		{
			FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Param);
			UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (ObjectProperty && EditorWorld && EditorWorld->IsA(ObjectProperty->PropertyClass)) ObjectProperty->SetObjectPropertyValue_InContainer(Frame, EditorWorld);
		}
	}

	{
		// Blueprint execution is blocked in the editor unless this guard is in scope.
		FEditorScriptExecutionGuard ScriptGuard;
		Target->ProcessEvent(Function, Frame);
	}

	DestroyFrame(Function, Frame);
	return true;
}
} // namespace EditorShortcutInvoke
