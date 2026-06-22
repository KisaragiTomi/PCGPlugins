#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Framework/Application/IInputProcessor.h"
#include "InputCoreTypes.h"
#include "ActorTagShortcut.generated.h"

USTRUCT()
struct FActorTagBinding
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Tag Shortcut")
	FName Tag;

	UPROPERTY(EditAnywhere, Category = "Tag Shortcut")
	FKey Key;

	UPROPERTY(EditAnywhere, Category = "Tag Shortcut")
	bool bCtrl = false;

	UPROPERTY(EditAnywhere, Category = "Tag Shortcut")
	bool bAlt = false;

	UPROPERTY(EditAnywhere, Category = "Tag Shortcut")
	bool bShift = false;
};

UCLASS(config = EditorPerProjectUserSettings, defaultconfig, meta = (DisplayName = "Actor Tag Shortcuts"))
class UActorTagShortcutSettings : public UDeveloperSettings
{
	GENERATED_BODY()
public:
	UActorTagShortcutSettings();

	UPROPERTY(config, EditAnywhere, Category = "Tag Shortcuts", meta = (TitleProperty = "Tag"))
	TArray<FActorTagBinding> Bindings;

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	virtual FName GetSectionName() const override { return TEXT("ActorTagShortcuts"); }
};

class FActorTagInputProcessor : public IInputProcessor
{
public:
	static void Register();
	static void Unregister();

	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}
	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override;
};
