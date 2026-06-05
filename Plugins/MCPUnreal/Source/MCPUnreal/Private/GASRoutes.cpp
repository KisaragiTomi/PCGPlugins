// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.
//
// GASRoutes.cpp — HTTP routes for Gameplay Ability System (GAS) operations
// including ability granting, effects, and attribute management.
//
// Guarded by WITH_GAMEPLAY_ABILITIES — returns 501 when GAS modules are unavailable.

#include "MCPUnrealUtils.h"

#if WITH_GAMEPLAY_ABILITIES
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "GameplayAbilitySpec.h"
#include "GameplayEffect.h"
#include "AttributeSet.h"
#include "EngineUtils.h"
#include "UObject/UObjectIterator.h"
#endif

namespace MCPUnreal {

#if WITH_GAMEPLAY_ABILITIES

  // ---------------------------------------------------------------------------
  // Helper — find AbilitySystemComponent on an actor
  // ---------------------------------------------------------------------------

  static UAbilitySystemComponent* GetASC(AActor* Actor) {
    if (!Actor) {
      return nullptr;
    }

    // Try IAbilitySystemInterface first (standard pattern).
    if (IAbilitySystemInterface* ASI = Cast<IAbilitySystemInterface>(Actor)) {
      return ASI->GetAbilitySystemComponent();
    }

    // Fall back to component search.
    return Actor->FindComponentByClass<UAbilitySystemComponent>();
  }

#endif  // WITH_GAMEPLAY_ABILITIES

  // ---------------------------------------------------------------------------
  // POST /api/gas/ops
  // ---------------------------------------------------------------------------

  static bool HandleGASOps(const FHttpServerRequest& Request,
                           const FHttpResultCallback& OnComplete) {
#if WITH_GAMEPLAY_ABILITIES
    TSharedPtr<FJsonObject> Body;
    if (!ParseJsonBody(Request, Body)) {
      SendError(OnComplete, TEXT("Invalid JSON in request body"));
      return true;
    }

    const FString Operation = Body->GetStringField(TEXT("operation"));
    if (Operation.IsEmpty()) {
      SendError(OnComplete, TEXT("operation is required"));
      return true;
    }

    // --- grant_ability ---
    if (Operation == TEXT("grant_ability")) {
      const FString ActorPath = Body->GetStringField(TEXT("actor_path"));
      const FString AbilityClassName = Body->GetStringField(TEXT("ability_class"));
      if (ActorPath.IsEmpty() || AbilityClassName.IsEmpty()) {
        SendError(OnComplete, TEXT("actor_path and ability_class are required for grant_ability"));
        return true;
      }

      AActor* Actor = FindObject<AActor>(nullptr, *ActorPath);
      if (!Actor) {
        SendError(OnComplete, FString::Printf(TEXT("Actor not found: '%s'"), *ActorPath));
        return true;
      }

      UAbilitySystemComponent* ASC = GetASC(Actor);
      if (!ASC) {
        SendError(OnComplete, TEXT("Actor does not have an AbilitySystemComponent"));
        return true;
      }

      UClass* AbilityClass = LoadClass<UGameplayAbility>(nullptr, *AbilityClassName);
      if (!AbilityClass) {
        SendError(OnComplete,
                  FString::Printf(TEXT("Ability class not found: '%s'"), *AbilityClassName));
        return true;
      }

      FGameplayAbilitySpec Spec(AbilityClass, 1, INDEX_NONE, Actor);
      FGameplayAbilitySpecHandle Handle = ASC->GiveAbility(Spec);

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), Handle.IsValid());
      ResponseJson->SetStringField(TEXT("ability_spec_handle"), Handle.ToString());
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- revoke_ability ---
    if (Operation == TEXT("revoke_ability")) {
      const FString ActorPath = Body->GetStringField(TEXT("actor_path"));
      if (ActorPath.IsEmpty()) {
        SendError(OnComplete, TEXT("actor_path is required for revoke_ability"));
        return true;
      }

      AActor* Actor = FindObject<AActor>(nullptr, *ActorPath);
      if (!Actor) {
        SendError(OnComplete, FString::Printf(TEXT("Actor not found: '%s'"), *ActorPath));
        return true;
      }

      UAbilitySystemComponent* ASC = GetASC(Actor);
      if (!ASC) {
        SendError(OnComplete, TEXT("Actor does not have an AbilitySystemComponent"));
        return true;
      }

      int32 RevokedCount = 0;
      const FString AbilityClassName = Body->GetStringField(TEXT("ability_class"));
      const FString AbilityTagStr = Body->GetStringField(TEXT("ability_tag"));

      if (!AbilityClassName.IsEmpty()) {
        // Revoke by class.
        UClass* AbilityClass = LoadClass<UGameplayAbility>(nullptr, *AbilityClassName);
        if (AbilityClass) {
          for (const FGameplayAbilitySpec& Spec : ASC->GetActivatableAbilities()) {
            if (Spec.Ability && Spec.Ability->GetClass() == AbilityClass) {
              ASC->ClearAbility(Spec.Handle);
              RevokedCount++;
            }
          }
        }
      } else if (!AbilityTagStr.IsEmpty()) {
        // Revoke by tag.
        FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*AbilityTagStr), false);
        if (Tag.IsValid()) {
          for (const FGameplayAbilitySpec& Spec : ASC->GetActivatableAbilities()) {
            if (Spec.Ability && Spec.Ability->GetAssetTags().HasTag(Tag)) {
              ASC->ClearAbility(Spec.Handle);
              RevokedCount++;
            }
          }
        }
      } else {
        SendError(OnComplete, TEXT("ability_class or ability_tag is required for revoke_ability"));
        return true;
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetNumberField(TEXT("revoked_count"), RevokedCount);
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- list_abilities ---
    if (Operation == TEXT("list_abilities")) {
      const FString ActorPath = Body->GetStringField(TEXT("actor_path"));
      if (ActorPath.IsEmpty()) {
        SendError(OnComplete, TEXT("actor_path is required for list_abilities"));
        return true;
      }

      AActor* Actor = FindObject<AActor>(nullptr, *ActorPath);
      if (!Actor) {
        SendError(OnComplete, FString::Printf(TEXT("Actor not found: '%s'"), *ActorPath));
        return true;
      }

      UAbilitySystemComponent* ASC = GetASC(Actor);
      if (!ASC) {
        SendError(OnComplete, TEXT("Actor does not have an AbilitySystemComponent"));
        return true;
      }

      TArray<TSharedPtr<FJsonValue>> AbilitiesArray;
      for (const FGameplayAbilitySpec& Spec : ASC->GetActivatableAbilities()) {
        TSharedPtr<FJsonObject> AbilityJson = MakeShareable(new FJsonObject());
        if (Spec.Ability) {
          AbilityJson->SetStringField(TEXT("class"), Spec.Ability->GetClass()->GetPathName());
          AbilityJson->SetNumberField(TEXT("level"), Spec.Level);
          AbilityJson->SetBoolField(TEXT("active"), Spec.IsActive());

          // Serialize ability tags.
          TArray<TSharedPtr<FJsonValue>> Tags;
          for (const FGameplayTag& Tag : Spec.Ability->GetAssetTags()) {
            Tags.Add(MakeShareable(new FJsonValueString(Tag.ToString())));
          }
          AbilityJson->SetArrayField(TEXT("tags"), Tags);
        }
        AbilityJson->SetStringField(TEXT("handle"), Spec.Handle.ToString());
        AbilitiesArray.Add(MakeShareable(new FJsonValueObject(AbilityJson)));
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetArrayField(TEXT("abilities"), AbilitiesArray);
      ResponseJson->SetNumberField(TEXT("count"), AbilitiesArray.Num());
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- apply_effect ---
    if (Operation == TEXT("apply_effect")) {
      const FString ActorPath = Body->GetStringField(TEXT("actor_path"));
      const FString EffectClassName = Body->GetStringField(TEXT("effect_class"));
      if (ActorPath.IsEmpty() || EffectClassName.IsEmpty()) {
        SendError(OnComplete, TEXT("actor_path and effect_class are required for apply_effect"));
        return true;
      }

      AActor* Actor = FindObject<AActor>(nullptr, *ActorPath);
      if (!Actor) {
        SendError(OnComplete, FString::Printf(TEXT("Actor not found: '%s'"), *ActorPath));
        return true;
      }

      UAbilitySystemComponent* ASC = GetASC(Actor);
      if (!ASC) {
        SendError(OnComplete, TEXT("Actor does not have an AbilitySystemComponent"));
        return true;
      }

      UClass* EffectClass = LoadClass<UGameplayEffect>(nullptr, *EffectClassName);
      if (!EffectClass) {
        SendError(OnComplete,
                  FString::Printf(TEXT("Effect class not found: '%s'"), *EffectClassName));
        return true;
      }

      float EffectLevel = 1.0f;
      double LevelValue;
      if (Body->TryGetNumberField(TEXT("effect_level"), LevelValue)) {
        EffectLevel = static_cast<float>(LevelValue);
      }

      FGameplayEffectContextHandle Context = ASC->MakeEffectContext();
      Context.AddSourceObject(Actor);
      FGameplayEffectSpecHandle SpecHandle =
          ASC->MakeOutgoingSpec(EffectClass, EffectLevel, Context);

      if (SpecHandle.IsValid()) {
        ASC->ApplyGameplayEffectSpecToSelf(*SpecHandle.Data.Get());
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), SpecHandle.IsValid());
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- get_attributes ---
    if (Operation == TEXT("get_attributes")) {
      const FString ActorPath = Body->GetStringField(TEXT("actor_path"));
      if (ActorPath.IsEmpty()) {
        SendError(OnComplete, TEXT("actor_path is required for get_attributes"));
        return true;
      }

      AActor* Actor = FindObject<AActor>(nullptr, *ActorPath);
      if (!Actor) {
        SendError(OnComplete, FString::Printf(TEXT("Actor not found: '%s'"), *ActorPath));
        return true;
      }

      UAbilitySystemComponent* ASC = GetASC(Actor);
      if (!ASC) {
        SendError(OnComplete, TEXT("Actor does not have an AbilitySystemComponent"));
        return true;
      }

      TArray<TSharedPtr<FJsonValue>> AttributeSets;
      for (const UAttributeSet* AttrSet : ASC->GetSpawnedAttributes()) {
        if (!AttrSet) continue;

        TSharedPtr<FJsonObject> SetJson = MakeShareable(new FJsonObject());
        SetJson->SetStringField(TEXT("name"), AttrSet->GetClass()->GetName());

        TArray<TSharedPtr<FJsonValue>> Attributes;
        for (TFieldIterator<FProperty> It(AttrSet->GetClass()); It; ++It) {
          FProperty* Prop = *It;
          if (!Prop) continue;

          // Only include FGameplayAttributeData properties.
          FStructProperty* StructProp = CastField<FStructProperty>(Prop);
          if (!StructProp || StructProp->Struct != FGameplayAttributeData::StaticStruct()) {
            continue;
          }

          const FGameplayAttributeData* Data =
              StructProp->ContainerPtrToValuePtr<FGameplayAttributeData>(AttrSet);
          if (!Data) continue;

          TSharedPtr<FJsonObject> AttrJson = MakeShareable(new FJsonObject());
          AttrJson->SetStringField(TEXT("name"), Prop->GetName());
          AttrJson->SetNumberField(TEXT("base"), Data->GetBaseValue());
          AttrJson->SetNumberField(TEXT("current"), Data->GetCurrentValue());
          Attributes.Add(MakeShareable(new FJsonValueObject(AttrJson)));
        }

        SetJson->SetArrayField(TEXT("attributes"), Attributes);
        AttributeSets.Add(MakeShareable(new FJsonValueObject(SetJson)));
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetArrayField(TEXT("attribute_sets"), AttributeSets);
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- set_attribute ---
    if (Operation == TEXT("set_attribute")) {
      const FString ActorPath = Body->GetStringField(TEXT("actor_path"));
      const FString AttributeName = Body->GetStringField(TEXT("attribute_name"));
      if (ActorPath.IsEmpty() || AttributeName.IsEmpty()) {
        SendError(OnComplete, TEXT("actor_path and attribute_name are required for set_attribute"));
        return true;
      }

      double AttributeValue = 0.0;
      if (!Body->TryGetNumberField(TEXT("attribute_value"), AttributeValue)) {
        SendError(OnComplete, TEXT("attribute_value is required for set_attribute"));
        return true;
      }

      AActor* Actor = FindObject<AActor>(nullptr, *ActorPath);
      if (!Actor) {
        SendError(OnComplete, FString::Printf(TEXT("Actor not found: '%s'"), *ActorPath));
        return true;
      }

      UAbilitySystemComponent* ASC = GetASC(Actor);
      if (!ASC) {
        SendError(OnComplete, TEXT("Actor does not have an AbilitySystemComponent"));
        return true;
      }

      // Find the attribute by name across all spawned attribute sets.
      bool bFound = false;
      for (UAttributeSet* AttrSet : ASC->GetSpawnedAttributes()) {
        if (!AttrSet) continue;

        FProperty* Prop = AttrSet->GetClass()->FindPropertyByName(FName(*AttributeName));
        FStructProperty* StructProp = CastField<FStructProperty>(Prop);
        if (StructProp && StructProp->Struct == FGameplayAttributeData::StaticStruct()) {
          FGameplayAttribute Attribute(Prop);
          ASC->SetNumericAttributeBase(Attribute, static_cast<float>(AttributeValue));
          bFound = true;
          break;
        }
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), bFound);
      if (!bFound) {
        ResponseJson->SetStringField(
            TEXT("error"), FString::Printf(TEXT("Attribute '%s' not found"), *AttributeName));
      }
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    SendError(OnComplete, FString::Printf(TEXT("Unknown GAS operation: '%s'"), *Operation));
    return true;
#else
    SendError(OnComplete,
              TEXT("GameplayAbilities module is not available. Enable the GameplayAbilities plugin "
                   "in your project to use gas_ops."),
              501);
    return true;
#endif
  }

  // ---------------------------------------------------------------------------
  // Registration
  // ---------------------------------------------------------------------------

  void RegisterGASRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles) {
    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/gas/ops")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleGASOps)));

    UE_LOG(LogMCPUnreal, Verbose, TEXT("Registered GAS routes (1 endpoint)"));
  }

}  // namespace MCPUnreal
