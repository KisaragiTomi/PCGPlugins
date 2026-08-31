#include "EditorShortcutInputProcessor.h"

#include "EditorShortcutSettings.h"
#include "EditorShortcutSubsystem.h"
#include "EditorShortcutsLog.h"

#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/InputChord.h"
#include "Misc/App.h"

namespace
{
TSharedPtr<FEditorShortcutInputProcessor> GInputProcessor;

/**
 * True while a text field owns keyboard focus.
 *
 * There is no engine API for this, so the widget type name is the available
 * signal. SEditableText and SMultiLineEditableText both carry EditableText.
 */
bool IsTextEntryFocused()
{
	if (!FSlateApplication::IsInitialized()) return false;

	const TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetKeyboardFocusedWidget();
	if (!Focused.IsValid()) return false;

	const FString WidgetType = Focused->GetType().ToString();
	return WidgetType.Contains(TEXT("EditableText"));
}

/** The chord the user actually pressed, in the same shape the bindings are stored in. */
FInputChord MakeChordFromEvent(const FKeyEvent& KeyEvent)
{
	FInputChord Chord;
	Chord.Key = KeyEvent.GetKey();
	Chord.bShift = KeyEvent.IsShiftDown();
	Chord.bCtrl = KeyEvent.IsControlDown();
	Chord.bAlt = KeyEvent.IsAltDown();
	Chord.bCmd = KeyEvent.IsCommandDown();
	return Chord;
}
} // namespace

void FEditorShortcutInputProcessor::Register()
{
	if (GInputProcessor.IsValid()) return;
	if (!FSlateApplication::IsInitialized()) return;

	// Nothing can press a key in a commandlet or a headless run.
	if (IsRunningCommandlet() || !FApp::CanEverRender() || !GEditor) return;

	GInputProcessor = MakeShared<FEditorShortcutInputProcessor>();
	FSlateApplication::Get().RegisterInputPreProcessor(GInputProcessor);
}

void FEditorShortcutInputProcessor::Unregister()
{
	if (!GInputProcessor.IsValid()) return;

	if (FSlateApplication::IsInitialized()) FSlateApplication::Get().UnregisterInputPreProcessor(GInputProcessor);
	GInputProcessor.Reset();
}

bool FEditorShortcutInputProcessor::HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent)
{
	// Auto-repeat would run the action once per frame while the key is held.
	if (InKeyEvent.IsRepeat()) return false;

	const UEditorShortcutSettings* Settings = GetDefault<UEditorShortcutSettings>();
	if (!Settings || !Settings->bEnabled || Settings->Bindings.IsEmpty()) return false;

	// During play the keys belong to the game.
	if (!GEditor || GEditor->PlayWorld) return false;

	if (Settings->bIgnoreWhileTypingInTextField && IsTextEntryFocused()) return false;

	const FInputChord Pressed = MakeChordFromEvent(InKeyEvent);
	if (!Pressed.IsValidChord()) return false;

	UEditorShortcutSubsystem* Subsystem = UEditorShortcutSubsystem::Get();
	if (!Subsystem) return false;

	bool bHandled = false;
	for (const FEditorShortcutBinding& Binding : Settings->Bindings)
	{
		if (!Binding.bEnabled || Binding.Chord != Pressed) continue;

		if (Settings->bVerboseLogging) UE_LOG(LogEditorShortcuts, Log, TEXT("Chord %s matched binding %s"), *Pressed.GetInputText().ToString(), *Binding.GetDisplayLabel());

		bHandled |= Subsystem->ExecuteBinding(Binding).bHandled;
	}

	return bHandled;
}
