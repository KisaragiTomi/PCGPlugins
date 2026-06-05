#include "EnhancedHairCardsActor.h"

#include "Components/SceneComponent.h"
#include "EnhancedHairCardsAsset.h"
#include "EnhancedHairCardsComponent.h"
#include "GroomComponent.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"

namespace EnhancedHairCardsActor
{
	static const FName GeneratedComponentTag(TEXT("EnhancedHairCardsGeneratedComponent"));
	static const FName SourceGroomSimulationTag(TEXT("EnhancedHairCardsSourceGroomSimulationComponent"));
	static const FName NiagaraSimulationTag(TEXT("EnhancedHairCardsNiagaraSimulationComponent"));
}

AEnhancedHairCardsActor::AEnhancedHairCardsActor(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	RootComponent = SceneRoot;
}

void AEnhancedHairCardsActor::SetHairCardsAsset(UEnhancedHairCardsAsset* InHairCardsAsset)
{
	if (HairCardsAsset == InHairCardsAsset)
	{
		return;
	}

	Modify();
	HairCardsAsset = InHairCardsAsset;
	RebuildComponentsFromAsset();
}

void AEnhancedHairCardsActor::RebuildComponentsFromAsset()
{
	TArray<UEnhancedHairCardsComponent*> ExistingComponents;
	GetComponents(ExistingComponents);

	for (UEnhancedHairCardsComponent* Component : ExistingComponents)
	{
		if (Component && Component->ComponentTags.Contains(EnhancedHairCardsActor::GeneratedComponentTag))
		{
			Component->DestroyComponent();
		}
	}

	TArray<UNiagaraComponent*> ExistingNiagaraComponents;
	GetComponents(ExistingNiagaraComponents);
	for (UNiagaraComponent* Component : ExistingNiagaraComponents)
	{
		if (Component && Component->ComponentTags.Contains(EnhancedHairCardsActor::NiagaraSimulationTag))
		{
			Component->DestroyComponent();
		}
	}

	TArray<UGroomComponent*> ExistingGroomComponents;
	GetComponents(ExistingGroomComponents);
	for (UGroomComponent* Component : ExistingGroomComponents)
	{
		if (Component && Component->ComponentTags.Contains(EnhancedHairCardsActor::SourceGroomSimulationTag))
		{
			Component->DestroyComponent();
		}
	}

	HairCardsComponents.Reset();
	NiagaraSimulationComponents.Reset();
	SourceGroomSimulationComponent = nullptr;
	BuiltHairCardsAsset = nullptr;
	BuiltPartCount = INDEX_NONE;
	BuiltSimulationGroupCount = INDEX_NONE;

	if (!HairCardsAsset)
	{
		return;
	}

	if (HairCardsAsset->SourceGroom)
	{
		const FName GroomComponentName = MakeUniqueObjectName(
			this,
			UGroomComponent::StaticClass(),
			TEXT("EnhancedHairCardsSourceGroomSimulation"));

		SourceGroomSimulationComponent = NewObject<UGroomComponent>(
			this,
			UGroomComponent::StaticClass(),
			GroomComponentName,
			RF_Transient);

		SourceGroomSimulationComponent->CreationMethod = EComponentCreationMethod::Native;
		SourceGroomSimulationComponent->ComponentTags.AddUnique(EnhancedHairCardsActor::SourceGroomSimulationTag);
		SourceGroomSimulationComponent->SetupAttachment(RootComponent);
		SourceGroomSimulationComponent->SetUseCards(true);
		SourceGroomSimulationComponent->SetGroomAsset(HairCardsAsset->SourceGroom);

		AddOwnedComponent(SourceGroomSimulationComponent);
		SourceGroomSimulationComponent->OnComponentCreated();
		SourceGroomSimulationComponent->RegisterComponent();
	}

	for (int32 PartIndex = 0; PartIndex < HairCardsAsset->Parts.Num(); ++PartIndex)
	{
		const FEnhancedHairCardsPart& Part = HairCardsAsset->Parts[PartIndex];
		const FName ComponentName = MakeUniqueObjectName(
			this,
			UEnhancedHairCardsComponent::StaticClass(),
			*FString::Printf(TEXT("EnhancedHairCardsPart_%02d"), PartIndex));

		UEnhancedHairCardsComponent* Component = NewObject<UEnhancedHairCardsComponent>(
			this,
			UEnhancedHairCardsComponent::StaticClass(),
			ComponentName,
			RF_Transient);

		Component->CreationMethod = EComponentCreationMethod::Native;
		Component->ComponentTags.AddUnique(EnhancedHairCardsActor::GeneratedComponentTag);
		Component->SetupAttachment(RootComponent);
		Component->ApplyHairCardsPart(Part);
		Component->SetSourceGroomComponent(SourceGroomSimulationComponent);

		AddOwnedComponent(Component);
		Component->OnComponentCreated();
		Component->RegisterComponent();

		HairCardsComponents.Add(Component);
	}

	if (SourceGroomSimulationComponent)
	{
		if (HairCardsAsset->SimulationSettings.bUseSourceGroomNiagara
			&& HairCardsAsset->SimulationSettings.HasEnabledSimulation())
		{
			NiagaraSimulationComponents.Reset();
			for (int32 SimulationIndex = 0; SimulationIndex < HairCardsAsset->SimulationSettings.Groups.Num(); ++SimulationIndex)
			{
				const FEnhancedHairCardsNiagaraSimulationGroup& SimulationGroup = HairCardsAsset->SimulationSettings.Groups[SimulationIndex];
				if (!SimulationGroup.bEnableSimulation
					|| !SimulationGroup.NiagaraSystem
					|| !SourceGroomSimulationComponent->NiagaraComponents.IsValidIndex(SimulationGroup.SourceGroupIndex))
				{
					continue;
				}

				UNiagaraComponent* NiagaraComponent = SourceGroomSimulationComponent->NiagaraComponents[SimulationGroup.SourceGroupIndex];
				if (!NiagaraComponent)
				{
					continue;
				}

				NiagaraComponent->ComponentTags.AddUnique(EnhancedHairCardsActor::NiagaraSimulationTag);
				NiagaraComponent->SetAsset(SimulationGroup.NiagaraSystem);
				NiagaraComponent->bUseAttachParentBound = true;
				NiagaraComponent->ReinitializeSystem();
				NiagaraComponent->Activate(true);
				NiagaraSimulationComponents.Add(NiagaraComponent);
			}
		}
	}

	BuiltHairCardsAsset = HairCardsAsset;
	BuiltPartCount = HairCardsAsset->Parts.Num();
	BuiltSimulationGroupCount = HairCardsAsset->SimulationSettings.Groups.Num();
	ApplyAssetPreviewSettings();
}

void AEnhancedHairCardsActor::RefreshComponentsFromAsset(bool bForceRebuild)
{
	if (bForceRebuild || !IsBuiltFromCurrentAsset())
	{
		RebuildComponentsFromAsset();
		return;
	}

	if (!HairCardsAsset)
	{
		return;
	}

	for (int32 PartIndex = 0; PartIndex < HairCardsComponents.Num(); ++PartIndex)
	{
		UEnhancedHairCardsComponent* Component = HairCardsComponents[PartIndex];
		if (!Component || !HairCardsAsset->Parts.IsValidIndex(PartIndex))
		{
			continue;
		}

		Component->ApplyCardSettings(HairCardsAsset->Parts[PartIndex].CardSettings, false);
		Component->SetSourceGroomComponent(SourceGroomSimulationComponent);
		Component->SetMaterial(0, HairCardsAsset->Parts[PartIndex].Material);
	}

	ApplyAssetPreviewSettings();
}

void AEnhancedHairCardsActor::SetGuideSimulationEnabled(bool bEnabled)
{
	for (UEnhancedHairCardsComponent* Component : HairCardsComponents)
	{
		if (Component)
		{
			Component->SetGuideSimulationEnabled(bEnabled);
		}
	}
}

void AEnhancedHairCardsActor::ResetGuideSimulation()
{
	for (UEnhancedHairCardsComponent* Component : HairCardsComponents)
	{
		if (Component)
		{
			Component->ResetGuideSimulation();
		}
	}
}

void AEnhancedHairCardsActor::SetDynamicsStrength(float Strength)
{
	for (UEnhancedHairCardsComponent* Component : HairCardsComponents)
	{
		if (Component)
		{
			Component->SetDynamicsStrength(Strength);
		}
	}
}

void AEnhancedHairCardsActor::SetDynamicsWind(FVector LocalDirection, float WindStrength)
{
	for (UEnhancedHairCardsComponent* Component : HairCardsComponents)
	{
		if (Component)
		{
			Component->SetDynamicsWind(LocalDirection, WindStrength);
		}
	}
}

void AEnhancedHairCardsActor::ApplyAssetPreviewSettings()
{
	if (!HairCardsAsset)
	{
		return;
	}

	if (SourceGroomSimulationComponent)
	{
		const bool bKeepSourceGroomVisibleForSimulation =
			HairCardsAsset->PreviewSettings.bKeepSourceGroomVisibleForSimulation
			&& HairCardsAsset->SourceGroom
			&& HairCardsComponents.ContainsByPredicate([](const TObjectPtr<UEnhancedHairCardsComponent>& Component)
			{
				return Component && Component->bUseSourceGroomSimulation;
			});
		const bool bShowSourceGroom =
			bKeepSourceGroomVisibleForSimulation
			||
			HairCardsAsset->PreviewSettings.bShowOriginalGroom;
		SourceGroomSimulationComponent->SetVisibility(bShowSourceGroom, true);
		SourceGroomSimulationComponent->SetHiddenInGame(!bShowSourceGroom);
		SourceGroomSimulationComponent->MarkRenderStateDirty();
	}

	for (UEnhancedHairCardsComponent* Component : HairCardsComponents)
	{
		if (!Component)
		{
			continue;
		}

		Component->SetVisibility(HairCardsAsset->PreviewSettings.bShowEnhancedCards, true);
		Component->SetHiddenInGame(!HairCardsAsset->PreviewSettings.bShowEnhancedCards);
		Component->CardSettings.bRenderCardsMesh = HairCardsAsset->PreviewSettings.bRenderEnhancedCardsMesh;
		Component->CardSettings.GuideDebug.bDrawGuides = HairCardsAsset->PreviewSettings.bShowGuides;
		Component->MarkRenderStateDirty();
	}
}

void AEnhancedHairCardsActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	if (!IsBuiltFromCurrentAsset())
	{
		RebuildComponentsFromAsset();
	}
}

bool AEnhancedHairCardsActor::IsBuiltFromCurrentAsset() const
{
	const int32 CurrentPartCount = HairCardsAsset ? HairCardsAsset->Parts.Num() : INDEX_NONE;
	const int32 CurrentSimulationGroupCount = HairCardsAsset ? HairCardsAsset->SimulationSettings.Groups.Num() : INDEX_NONE;
	return BuiltHairCardsAsset == HairCardsAsset
		&& BuiltPartCount == CurrentPartCount
		&& BuiltSimulationGroupCount == CurrentSimulationGroupCount
		&& HairCardsComponents.Num() == FMath::Max(CurrentPartCount, 0);
}

#if WITH_EDITOR
bool AEnhancedHairCardsActor::GetReferencedContentObjects(TArray<UObject*>& Objects) const
{
	Super::GetReferencedContentObjects(Objects);

	if (HairCardsAsset)
	{
		Objects.Add(HairCardsAsset);
	}

	return true;
}

void AEnhancedHairCardsActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.Property
		? PropertyChangedEvent.Property->GetFName()
		: NAME_None;

	if (PropertyName == GET_MEMBER_NAME_CHECKED(AEnhancedHairCardsActor, HairCardsAsset))
	{
		RebuildComponentsFromAsset();
	}
}
#endif
