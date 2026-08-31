#include "EditorShortcutTypes.h"

FString FEditorShortcutBinding::GetDisplayLabel() const
{
	if (!Label.IsEmpty()) return Label;
	if (FunctionName.IsNone()) return TEXT("Editor Shortcut");
	if (TargetClass.IsNull()) return FString::Printf(TEXT("%s on selection"), *FunctionName.ToString());

	return FString::Printf(TEXT("%s.%s"), *TargetClass.GetAssetName(), *FunctionName.ToString());
}

bool FEditorShortcutBinding::Validate(FString& OutError) const
{
	if (FunctionName.IsNone())
	{
		OutError = TEXT("FunctionName is empty");
		return false;
	}

	if (TargetClass.IsNull() && !bRequiresSelection)
	{
		OutError = TEXT("no TargetClass and selection not required, so there is nothing to call the function on");
		return false;
	}

	return true;
}
