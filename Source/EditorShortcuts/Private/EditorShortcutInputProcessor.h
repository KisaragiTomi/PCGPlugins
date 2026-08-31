#pragma once

#include "CoreMinimal.h"
#include "Framework/Application/IInputProcessor.h"

/**
 * Watches key presses across the whole editor and dispatches matching bindings.
 *
 * A pre-processor sees keys before any widget does, which is what lets a
 * shortcut work while the mouse is anywhere over the level editor. The cost is
 * that focus has to be checked by hand, see IsTextEntryFocused in the cpp.
 */
class FEditorShortcutInputProcessor : public IInputProcessor
{
public:
	static void Register();
	static void Unregister();

	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}
	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override;
	virtual const TCHAR* GetDebugName() const override { return TEXT("EditorShortcuts"); }
};
