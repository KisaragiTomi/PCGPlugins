#include "ActorTagShortcut.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "Components/TextRenderComponent.h"
#include "Engine/World.h"
#include "TimerManager.h"

UActorTagShortcutSettings::UActorTagShortcutSettings()
{
	CategoryName = TEXT("Plugins");
}

static TSharedPtr<FActorTagInputProcessor> GTagInputProcessor;

static void SpawnTagIndicator(AActor* TargetActor, const FName& Tag, bool bAdded)
{
	UWorld* World = TargetActor->GetWorld();
	if (!World)
	{
		return;
	}

	FVector Origin;
	FVector BoxExtent;
	TargetActor->GetActorBounds(false, Origin, BoxExtent);

	FVector SpawnLocation = Origin + FVector(0, 0, BoxExtent.Z + 50.f);

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Indicator = World->SpawnActor<AActor>(AActor::StaticClass(), SpawnLocation, FRotator::ZeroRotator, Params);
	if (!Indicator)
	{
		return;
	}

	Indicator->SetFlags(RF_Transient);
#if WITH_EDITOR
	Indicator->SetIsTemporarilyHiddenInEditor(false);
	Indicator->bIsEditorOnlyActor = true;
#endif

	USceneComponent* Root = NewObject<USceneComponent>(Indicator, TEXT("Root"));
	Indicator->SetRootComponent(Root);
	Root->RegisterComponent();
	Indicator->SetActorLocation(SpawnLocation);

	UTextRenderComponent* Text = NewObject<UTextRenderComponent>(Indicator, TEXT("TagText"));
	Text->SetupAttachment(Root);

	FString DisplayText = FString::Printf(TEXT("%s: %s"),
		bAdded ? TEXT("+Tag") : TEXT("-Tag"),
		*Tag.ToString());
	Text->SetText(FText::FromString(DisplayText));
	Text->SetTextRenderColor(bAdded ? FColor::Green : FColor::Red);
	Text->SetHorizontalAlignment(EHTA_Center);
	Text->SetVerticalAlignment(EVRTA_TextCenter);
	float TextSize = FMath::Max(BoxExtent.X, BoxExtent.Y);
	Text->SetWorldSize(FMath::Max(TextSize, 10.f));
	Text->RegisterComponent();

	FTimerHandle Handle;
	TWeakObjectPtr<AActor> WeakIndicator = Indicator;
	World->GetTimerManager().SetTimer(Handle, [WeakIndicator]()
	{
		if (WeakIndicator.IsValid())
		{
			WeakIndicator->Destroy();
		}
	}, 2.0f, false);
}

void FActorTagInputProcessor::Register()
{
	if (!GTagInputProcessor.IsValid())
	{
		GTagInputProcessor = MakeShared<FActorTagInputProcessor>();
		FSlateApplication::Get().RegisterInputPreProcessor(GTagInputProcessor);
	}
}

void FActorTagInputProcessor::Unregister()
{
	if (GTagInputProcessor.IsValid())
	{
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().UnregisterInputPreProcessor(GTagInputProcessor);
		}
		GTagInputProcessor.Reset();
	}
}

bool FActorTagInputProcessor::HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent)
{
	const UActorTagShortcutSettings* Settings = GetDefault<UActorTagShortcutSettings>();
	if (!Settings || Settings->Bindings.IsEmpty())
	{
		return false;
	}

	const FKey PressedKey = InKeyEvent.GetKey();
	const bool bCtrl = InKeyEvent.IsControlDown();
	const bool bAlt = InKeyEvent.IsAltDown();
	const bool bShift = InKeyEvent.IsShiftDown();

	for (const FActorTagBinding& Binding : Settings->Bindings)
	{
		if (!Binding.Key.IsValid() || Binding.Tag.IsNone())
		{
			continue;
		}

		if (PressedKey == Binding.Key
			&& bCtrl == Binding.bCtrl
			&& bAlt == Binding.bAlt
			&& bShift == Binding.bShift)
		{
			if (!GEditor)
			{
				return false;
			}

			USelection* Selection = GEditor->GetSelectedActors();
			if (!Selection || Selection->Num() == 0)
			{
				return false;
			}

			TArray<AActor*> SelectedActors;
			Selection->GetSelectedObjects<AActor>(SelectedActors);

			for (AActor* Actor : SelectedActors)
			{
				if (!Actor)
				{
					continue;
				}

				if (Actor->Tags.Contains(Binding.Tag))
				{
					Actor->Tags.Remove(Binding.Tag);
					SpawnTagIndicator(Actor, Binding.Tag, false);
					UE_LOG(LogTemp, Log, TEXT("[TagShortcut] Removed tag '%s' from %s"),
						*Binding.Tag.ToString(), *Actor->GetActorNameOrLabel());
				}
				else
				{
					Actor->Tags.Add(Binding.Tag);
					SpawnTagIndicator(Actor, Binding.Tag, true);
					UE_LOG(LogTemp, Log, TEXT("[TagShortcut] Added tag '%s' to %s"),
						*Binding.Tag.ToString(), *Actor->GetActorNameOrLabel());
				}
				Actor->MarkPackageDirty();
			}

			return true;
		}
	}

	return false;
}
