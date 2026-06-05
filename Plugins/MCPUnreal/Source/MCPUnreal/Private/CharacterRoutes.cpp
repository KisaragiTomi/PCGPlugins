// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.
//
// CharacterRoutes.cpp — HTTP routes for character configuration: movement,
// capsule, mesh, and camera settings.
//
// See IMPLEMENTATION.md §3.7 and §5.1.

#include "MCPUnrealUtils.h"

#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"

namespace MCPUnreal {

  // ---------------------------------------------------------------------------
  // Helpers
  // ---------------------------------------------------------------------------

  /** Load a Character Blueprint's CDO for property access. */
  static ACharacter* GetCharacterCDO(const FString& BlueprintPath,
                                     const FHttpResultCallback& OnComplete) {
    UObject* Obj = StaticLoadObject(UBlueprint::StaticClass(), nullptr, *BlueprintPath);
    UBlueprint* BP = Cast<UBlueprint>(Obj);
    if (!BP) {
      SendError(OnComplete, FString::Printf(TEXT("Blueprint not found: '%s'"), *BlueprintPath));
      return nullptr;
    }

    ACharacter* CDO = Cast<ACharacter>(BP->GeneratedClass->GetDefaultObject());
    if (!CDO) {
      SendError(OnComplete, TEXT("Blueprint is not a Character class"));
      return nullptr;
    }
    return CDO;
  }

  // ---------------------------------------------------------------------------
  // POST /api/characters/config
  // ---------------------------------------------------------------------------

  static bool HandleCharacterConfig(const FHttpServerRequest& Request,
                                    const FHttpResultCallback& OnComplete) {
    TSharedPtr<FJsonObject> Body;
    if (!ParseJsonBody(Request, Body)) {
      SendError(OnComplete, TEXT("Invalid JSON in request body"));
      return true;
    }

    const FString Operation = Body->GetStringField(TEXT("operation"));
    const FString BlueprintPath = Body->GetStringField(TEXT("blueprint_path"));
    if (Operation.IsEmpty()) {
      SendError(OnComplete, TEXT("operation is required"));
      return true;
    }
    if (BlueprintPath.IsEmpty()) {
      SendError(OnComplete, TEXT("blueprint_path is required"));
      return true;
    }

    ACharacter* CDO = GetCharacterCDO(BlueprintPath, OnComplete);
    if (!CDO) return true;

    UCharacterMovementComponent* MovementComp = CDO->GetCharacterMovement();
    UCapsuleComponent* Capsule = CDO->GetCapsuleComponent();

    // --- get_config ---
    if (Operation == TEXT("get_config")) {
      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());

      if (MovementComp) {
        TSharedPtr<FJsonObject> MovJson = MakeShareable(new FJsonObject());
        MovJson->SetNumberField(TEXT("max_walk_speed"), MovementComp->MaxWalkSpeed);
        MovJson->SetNumberField(TEXT("max_acceleration"), MovementComp->MaxAcceleration);
        MovJson->SetNumberField(TEXT("jump_z_velocity"), CDO->JumpMaxHoldTime > 0
                                                             ? MovementComp->JumpZVelocity
                                                             : MovementComp->JumpZVelocity);
        MovJson->SetNumberField(TEXT("gravity_scale"), MovementComp->GravityScale);
        MovJson->SetNumberField(TEXT("air_control"), MovementComp->AirControl);
        MovJson->SetNumberField(TEXT("braking_deceleration"),
                                MovementComp->BrakingDecelerationWalking);
        ResponseJson->SetObjectField(TEXT("movement"), MovJson);
      }

      if (Capsule) {
        TSharedPtr<FJsonObject> CapJson = MakeShareable(new FJsonObject());
        CapJson->SetNumberField(TEXT("radius"), Capsule->GetUnscaledCapsuleRadius());
        CapJson->SetNumberField(TEXT("half_height"), Capsule->GetUnscaledCapsuleHalfHeight());
        ResponseJson->SetObjectField(TEXT("capsule"), CapJson);
      }

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- set_movement ---
    if (Operation == TEXT("set_movement")) {
      if (!MovementComp) {
        SendError(OnComplete, TEXT("CharacterMovementComponent not found"), 500);
        return true;
      }

      if (Body->HasField(TEXT("max_walk_speed")))
        MovementComp->MaxWalkSpeed =
            static_cast<float>(Body->GetNumberField(TEXT("max_walk_speed")));
      if (Body->HasField(TEXT("max_acceleration")))
        MovementComp->MaxAcceleration =
            static_cast<float>(Body->GetNumberField(TEXT("max_acceleration")));
      if (Body->HasField(TEXT("jump_z_velocity")))
        MovementComp->JumpZVelocity =
            static_cast<float>(Body->GetNumberField(TEXT("jump_z_velocity")));
      if (Body->HasField(TEXT("gravity_scale")))
        MovementComp->GravityScale =
            static_cast<float>(Body->GetNumberField(TEXT("gravity_scale")));
      if (Body->HasField(TEXT("air_control")))
        MovementComp->AirControl = static_cast<float>(Body->GetNumberField(TEXT("air_control")));
      if (Body->HasField(TEXT("braking_deceleration")))
        MovementComp->BrakingDecelerationWalking =
            static_cast<float>(Body->GetNumberField(TEXT("braking_deceleration")));

      CDO->MarkPackageDirty();

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- set_capsule ---
    if (Operation == TEXT("set_capsule")) {
      if (!Capsule) {
        SendError(OnComplete, TEXT("CapsuleComponent not found"), 500);
        return true;
      }

      float Radius = Capsule->GetUnscaledCapsuleRadius();
      float HalfHeight = Capsule->GetUnscaledCapsuleHalfHeight();

      if (Body->HasField(TEXT("capsule_radius")))
        Radius = static_cast<float>(Body->GetNumberField(TEXT("capsule_radius")));
      if (Body->HasField(TEXT("capsule_half_height")))
        HalfHeight = static_cast<float>(Body->GetNumberField(TEXT("capsule_half_height")));

      Capsule->SetCapsuleSize(Radius, HalfHeight);
      CDO->MarkPackageDirty();

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- set_mesh ---
    if (Operation == TEXT("set_mesh")) {
      const FString MeshPath = Body->GetStringField(TEXT("skeletal_mesh_path"));
      if (!MeshPath.IsEmpty()) {
        USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *MeshPath);
        if (Mesh && CDO->GetMesh()) {
          CDO->GetMesh()->SetSkeletalMesh(Mesh);
        }
      }

      const FString AnimBPPath = Body->GetStringField(TEXT("anim_blueprint_path"));
      if (!AnimBPPath.IsEmpty() && CDO->GetMesh()) {
        UClass* AnimClass = LoadClass<UAnimInstance>(nullptr, *AnimBPPath);
        if (AnimClass) {
          CDO->GetMesh()->SetAnimInstanceClass(AnimClass);
        }
      }

      CDO->MarkPackageDirty();

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- get_movement_modes ---
    if (Operation == TEXT("get_movement_modes")) {
      TArray<TSharedPtr<FJsonValue>> Modes;
      Modes.Add(MakeShareable(new FJsonValueString(TEXT("Walking"))));
      Modes.Add(MakeShareable(new FJsonValueString(TEXT("NavWalking"))));
      Modes.Add(MakeShareable(new FJsonValueString(TEXT("Falling"))));
      Modes.Add(MakeShareable(new FJsonValueString(TEXT("Swimming"))));
      Modes.Add(MakeShareable(new FJsonValueString(TEXT("Flying"))));
      Modes.Add(MakeShareable(new FJsonValueString(TEXT("Custom"))));

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetArrayField(TEXT("modes"), Modes);
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    SendError(OnComplete, FString::Printf(TEXT("Unknown character operation: '%s'"), *Operation));
    return true;
  }

  // ---------------------------------------------------------------------------
  // Registration
  // ---------------------------------------------------------------------------

  void RegisterCharacterRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles) {
    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/characters/config")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleCharacterConfig)));

    UE_LOG(LogMCPUnreal, Verbose, TEXT("Registered character routes (1 endpoint)"));
  }

}  // namespace MCPUnreal
