#include "EditorShortcutInputProcessor.h"
#include "EditorShortcutSettings.h"
#include "EditorShortcutsLog.h"

#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogEditorShortcuts);

namespace
{
/**
 * Reports the configuration mistakes a global input pre-processor cannot catch on its own:
 * an unreachable binding, a malformed action, or two bindings racing for the same chord.
 */
void AuditBindings()
{
	const UEditorShortcutSettings* Settings = GetDefault<UEditorShortcutSettings>();
	if (!Settings) return;

	for (int32 Index = 0; Index < Settings->Bindings.Num(); ++Index)
	{
		const FEditorShortcutBinding& Binding = Settings->Bindings[Index];
		if (!Binding.bEnabled) continue;

		const FString Label = Binding.GetDisplayLabel();

		FString ValidationError;
		if (!Binding.Validate(ValidationError)) UE_LOG(LogEditorShortcuts, Warning, TEXT("Binding %d '%s' is misconfigured: %s"), Index, *Label, *ValidationError);

		if (!Binding.Chord.IsValidChord())
		{
			UE_LOG(LogEditorShortcuts, Warning, TEXT("Binding %d '%s' has no key, so only script can run it"), Index, *Label);
			continue;
		}

		for (int32 Earlier = 0; Earlier < Index; ++Earlier)
		{
			const FEditorShortcutBinding& Other = Settings->Bindings[Earlier];
			if (!Other.bEnabled || Other.Chord != Binding.Chord) continue;

			UE_LOG(LogEditorShortcuts, Warning, TEXT("Binding %d '%s' shares %s with binding %d '%s'; both will run"),
				Index, *Label, *Binding.Chord.GetInputText().ToString(), Earlier, *Other.GetDisplayLabel());
		}
	}
}

void OnPostEngineInit()
{
	AuditBindings();
	FEditorShortcutInputProcessor::Register();
}
} // namespace

class FEditorShortcutsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Slate is not up yet when an editor module starts, so wait for it.
		PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddStatic(&OnPostEngineInit);
	}

	virtual void ShutdownModule() override
	{
		if (PostEngineInitHandle.IsValid())
		{
			FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
			PostEngineInitHandle.Reset();
		}

		FEditorShortcutInputProcessor::Unregister();
	}

private:
	FDelegateHandle PostEngineInitHandle;
};

IMPLEMENT_MODULE(FEditorShortcutsModule, EditorShortcuts)
