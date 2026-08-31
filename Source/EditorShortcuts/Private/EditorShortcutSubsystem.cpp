#include "EditorShortcutSubsystem.h"

#include "EditorShortcutInvoke.h"
#include "EditorShortcutSettings.h"
#include "EditorShortcutsLog.h"

#include "Editor.h"
#include "Engine/Selection.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "EditorShortcuts"

namespace
{
void GetSelectedActors(TArray<AActor*>& OutActors)
{
	OutActors.Reset();
	if (!GEditor) return;

	USelection* Selection = GEditor->GetSelectedActors();
	if (!Selection) return;

	Selection->GetSelectedObjects<AActor>(OutActors);
}

void ShowToast(const FString& Message, bool bIsError)
{
	if (Message.IsEmpty()) return;

	// Commandlets and headless runs have no Slate application to hang a toast on.
	if (!FSlateApplication::IsInitialized()) return;

	FNotificationInfo Info(FText::FromString(Message));
	Info.bFireAndForget = true;
	Info.bUseSuccessFailIcons = true;
	Info.ExpireDuration = bIsError ? 5.0f : 2.0f;
	Info.FadeOutDuration = 0.5f;

	TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
	if (Item.IsValid()) Item->SetCompletionState(bIsError ? SNotificationItem::CS_Fail : SNotificationItem::CS_Success);
}
} // namespace

UEditorShortcutSubsystem* UEditorShortcutSubsystem::Get()
{
	return GEditor ? GEditor->GetEditorSubsystem<UEditorShortcutSubsystem>() : nullptr;
}

void UEditorShortcutSubsystem::Deinitialize()
{
	InstanceCache.Empty();
	Super::Deinitialize();
}

bool UEditorShortcutSubsystem::ExecuteBindingByIndex(int32 BindingIndex)
{
	const UEditorShortcutSettings* Settings = GetDefault<UEditorShortcutSettings>();
	if (!Settings || !Settings->Bindings.IsValidIndex(BindingIndex)) return false;

	return ExecuteBinding(Settings->Bindings[BindingIndex]).bHandled;
}

bool UEditorShortcutSubsystem::ExecuteBindingByLabel(const FString& Label)
{
	const UEditorShortcutSettings* Settings = GetDefault<UEditorShortcutSettings>();
	if (!Settings) return false;

	for (const FEditorShortcutBinding& Binding : Settings->Bindings)
	{
		if (Binding.bEnabled && Binding.GetDisplayLabel().Equals(Label, ESearchCase::IgnoreCase)) return ExecuteBinding(Binding).bHandled;
	}

	UE_LOG(LogEditorShortcuts, Warning, TEXT("No enabled binding labelled %s"), *Label);
	return false;
}

bool UEditorShortcutSubsystem::RunBinding(const FEditorShortcutBinding& Binding)
{
	const FEditorShortcutResult Result = ExecuteBinding(Binding);
	return Result.bHandled && !Result.HasError();
}

void UEditorShortcutSubsystem::FlushTargetInstances()
{
	InstanceCache.Empty();
}

TArray<FString> UEditorShortcutSubsystem::ValidateAllBindings()
{
	TArray<FString> Problems;

	const UEditorShortcutSettings* Settings = GetDefault<UEditorShortcutSettings>();
	if (!Settings) return Problems;

	for (int32 Index = 0; Index < Settings->Bindings.Num(); ++Index)
	{
		const FEditorShortcutBinding& Binding = Settings->Bindings[Index];
		if (!Binding.bEnabled) continue;

		const FString Prefix = FString::Printf(TEXT("Binding %d '%s'"), Index, *Binding.GetDisplayLabel());

		FString Error;
		if (!Binding.Validate(Error))
		{
			Problems.Add(FString::Printf(TEXT("%s: %s"), *Prefix, *Error));
			continue;
		}

		if (!Binding.Chord.IsValidChord()) Problems.Add(FString::Printf(TEXT("%s: no key, so only script can run it"), *Prefix));

		// Without a target class the function is looked up per selected actor, which
		// depends on what is selected right now and cannot be checked ahead of time.
		if (Binding.TargetClass.IsNull()) continue;

		UObject* Target = ResolveTarget(Binding, Error);
		if (!Target)
		{
			Problems.Add(FString::Printf(TEXT("%s: %s"), *Prefix, *Error));
			continue;
		}

		UFunction* Function = EditorShortcutInvoke::FindCallableFunction(Target, Binding.FunctionName, Error);
		if (!Function)
		{
			Problems.Add(FString::Printf(TEXT("%s: %s"), *Prefix, *Error));
			continue;
		}

		if (!EditorShortcutInvoke::IsBindable(Function, Error)) Problems.Add(FString::Printf(TEXT("%s: %s"), *Prefix, *Error));
	}

	for (const FString& Problem : Problems)
	{
		UE_LOG(LogEditorShortcuts, Warning, TEXT("%s"), *Problem);
	}

	return Problems;
}

FEditorShortcutResult UEditorShortcutSubsystem::ExecuteBinding(const FEditorShortcutBinding& Binding)
{
	const UEditorShortcutSettings* Settings = GetDefault<UEditorShortcutSettings>();
	const FString Label = Binding.GetDisplayLabel();

	FEditorShortcutResult Result;
	Result.bHandled = true;

	FString ValidationError;
	if (!Binding.Validate(ValidationError))
	{
		Result.Error = ValidationError;
	}
	else
	{
		TArray<AActor*> SelectedActors;
		GetSelectedActors(SelectedActors);

		if (Binding.bRequiresSelection && SelectedActors.IsEmpty())
		{
			Result.Message = FString::Printf(TEXT("%s: no actor selected"), *Label);
			if (Settings && Settings->bVerboseLogging) UE_LOG(LogEditorShortcuts, Log, TEXT("%s"), *Result.Message);
			return Result;
		}

		// A transaction that records nothing is dropped by the undo buffer on its own,
		// so there is no need to work out whether the call changed anything.
		TUniquePtr<FScopedTransaction> Transaction;
		if (Binding.bTransactional) Transaction = MakeUnique<FScopedTransaction>(FText::FromString(Label));

		Result = Binding.TargetClass.IsNull()
			? CallOnSelectedActors(Binding, SelectedActors)
			: CallOnTargetObject(Binding, SelectedActors);

		Result.bHandled = true;
		if (Transaction && Result.HasError()) Transaction->Cancel();
	}

	if (Result.HasError())
	{
		const FString Text = FString::Printf(TEXT("%s: %s"), *Label, *Result.Error);
		UE_LOG(LogEditorShortcuts, Warning, TEXT("%s"), *Text);
		if (Binding.bShowNotification) ShowToast(Text, true);
		return Result;
	}

	if (!Result.Message.IsEmpty())
	{
		if (Settings && Settings->bVerboseLogging) UE_LOG(LogEditorShortcuts, Log, TEXT("%s"), *Result.Message);
		if (Binding.bShowNotification) ShowToast(Result.Message, false);
	}

	return Result;
}

FEditorShortcutResult UEditorShortcutSubsystem::CallOnSelectedActors(const FEditorShortcutBinding& Binding, const TArray<AActor*>& SelectedActors)
{
	FEditorShortcutResult Result;
	int32 SkippedWithoutFunction = 0;
	FString LastFindError;

	for (AActor* Actor : SelectedActors)
	{
		if (!Actor) continue;

		UFunction* Function = EditorShortcutInvoke::FindCallableFunction(Actor, Binding.FunctionName, LastFindError);
		if (!Function)
		{
			++SkippedWithoutFunction;
			continue;
		}

		// The call may change anything on the actor, so snapshot it before running.
		if (Binding.bTransactional) Actor->Modify();

		FString CallError;
		if (!EditorShortcutInvoke::CallFunction(Actor, Function, SelectedActors, Actor, CallError))
		{
			// A signature problem applies to every actor of that class, so stop rather than repeat it.
			Result.Error = CallError;
			return Result;
		}

		++Result.AffectedActors;
	}

	if (Result.AffectedActors == 0)
	{
		Result.Error = LastFindError.IsEmpty()
			? FString::Printf(TEXT("nothing to call '%s' on"), *Binding.FunctionName.ToString())
			: LastFindError;
		return Result;
	}

	Result.Message = FString::Printf(TEXT("%s  on %d actor(s)"), *Binding.FunctionName.ToString(), Result.AffectedActors);
	if (SkippedWithoutFunction > 0) Result.Message += FString::Printf(TEXT("  (%d skipped)"), SkippedWithoutFunction);

	return Result;
}

FEditorShortcutResult UEditorShortcutSubsystem::CallOnTargetObject(const FEditorShortcutBinding& Binding, const TArray<AActor*>& SelectedActors)
{
	FEditorShortcutResult Result;

	FString ResolveError;
	UObject* Target = ResolveTarget(Binding, ResolveError);
	if (!Target)
	{
		Result.Error = ResolveError;
		return Result;
	}

	FString FindError;
	UFunction* Function = EditorShortcutInvoke::FindCallableFunction(Target, Binding.FunctionName, FindError);
	if (!Function)
	{
		Result.Error = FindError;
		return Result;
	}

	// Blueprint function library entries are static and belong on the default object.
	if (Function->HasAnyFunctionFlags(FUNC_Static)) Target = Target->GetClass()->GetDefaultObject();

	AActor* FirstActor = SelectedActors.IsEmpty() ? nullptr : SelectedActors[0];

	FString CallError;
	if (!EditorShortcutInvoke::CallFunction(Target, Function, SelectedActors, FirstActor, CallError))
	{
		Result.Error = CallError;
		return Result;
	}

	Result.AffectedActors = SelectedActors.Num();
	Result.Message = FString::Printf(TEXT("%s  on %d actor(s)"), *Binding.FunctionName.ToString(), SelectedActors.Num());
	return Result;
}

UObject* UEditorShortcutSubsystem::ResolveTarget(const FEditorShortcutBinding& Binding, FString& OutError)
{
	UClass* TargetClass = Binding.TargetClass.LoadSynchronous();
	if (!TargetClass)
	{
		OutError = FString::Printf(TEXT("could not load target class %s"), *Binding.TargetClass.ToString());
		return nullptr;
	}

	if (TargetClass->HasAnyClassFlags(CLASS_Abstract))
	{
		// Its static functions still live on the default object, which is enough to call them.
		return TargetClass->GetDefaultObject();
	}

	UObject* Instance = GetOrCreateInstance(TargetClass);
	if (!Instance) OutError = FString::Printf(TEXT("could not instantiate %s"), *TargetClass->GetName());

	return Instance;
}

UObject* UEditorShortcutSubsystem::GetOrCreateInstance(UClass* Class)
{
	if (!Class) return nullptr;

	// Recompiling a Blueprint replaces its generated class, leaving the old key pointing at a dead one.
	for (auto It = InstanceCache.CreateIterator(); It; ++It)
	{
		if (!IsValid(It.Key()) || !IsValid(It.Value()) || It.Key()->HasAnyClassFlags(CLASS_NewerVersionExists)) It.RemoveCurrent();
	}

	if (TObjectPtr<UObject>* Existing = InstanceCache.Find(Class)) return *Existing;

	UObject* Instance = NewObject<UObject>(this, Class, NAME_None, RF_Transient);
	InstanceCache.Add(Class, Instance);
	return Instance;
}

#undef LOCTEXT_NAMESPACE
