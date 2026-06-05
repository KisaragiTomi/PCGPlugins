// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.
//
// NiagaraRoutes.cpp — HTTP routes for Niagara VFX system management
// including spawning systems, parameter control, and emitter editing.
//
// Guarded by WITH_NIAGARA — returns 501 when Niagara modules are unavailable.

#include "MCPUnrealUtils.h"

#if WITH_NIAGARA
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraFunctionLibrary.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h"
#endif

namespace MCPUnreal {

  // ---------------------------------------------------------------------------
  // POST /api/niagara/ops
  // ---------------------------------------------------------------------------

  static bool HandleNiagaraOps(const FHttpServerRequest& Request,
                               const FHttpResultCallback& OnComplete) {
#if WITH_NIAGARA
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

    UWorld* World = GetWorld(Body);

    // --- spawn_system ---
    if (Operation == TEXT("spawn_system")) {
      const FString SystemPath = Body->GetStringField(TEXT("system_path"));
      if (SystemPath.IsEmpty()) {
        SendError(OnComplete, TEXT("system_path is required for spawn_system"));
        return true;
      }

      if (!World) {
        SendError(OnComplete,
                  TEXT("World not available — if world=pie was requested, ensure PIE is running"),
                  500);
        return true;
      }

      UNiagaraSystem* NiagaraSystem = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
      if (!NiagaraSystem) {
        SendError(OnComplete, FString::Printf(TEXT("Niagara system not found: '%s'"), *SystemPath));
        return true;
      }

      // Parse transform.
      FVector Location = FVector::ZeroVector;
      FRotator Rotation = FRotator::ZeroRotator;
      FVector Scale = FVector::OneVector;

      const TArray<TSharedPtr<FJsonValue>>* LocArray;
      if (Body->TryGetArrayField(TEXT("location"), LocArray) && LocArray->Num() >= 3) {
        Location.X = (*LocArray)[0]->AsNumber();
        Location.Y = (*LocArray)[1]->AsNumber();
        Location.Z = (*LocArray)[2]->AsNumber();
      }

      const TArray<TSharedPtr<FJsonValue>>* RotArray;
      if (Body->TryGetArrayField(TEXT("rotation"), RotArray) && RotArray->Num() >= 3) {
        Rotation.Pitch = (*RotArray)[0]->AsNumber();
        Rotation.Yaw = (*RotArray)[1]->AsNumber();
        Rotation.Roll = (*RotArray)[2]->AsNumber();
      }

      const TArray<TSharedPtr<FJsonValue>>* ScaleArray;
      if (Body->TryGetArrayField(TEXT("scale"), ScaleArray) && ScaleArray->Num() >= 3) {
        Scale.X = (*ScaleArray)[0]->AsNumber();
        Scale.Y = (*ScaleArray)[1]->AsNumber();
        Scale.Z = (*ScaleArray)[2]->AsNumber();
      }

      // Spawn actor with NiagaraComponent.
      FActorSpawnParameters SpawnParams;
      SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
      FTransform Transform(Rotation, Location, Scale);

      AActor* NewActor = World->SpawnActor<AActor>(AActor::StaticClass(), Transform, SpawnParams);
      if (!NewActor) {
        SendError(OnComplete, TEXT("Failed to spawn actor"), 500);
        return true;
      }

      UNiagaraComponent* NiagaraComp =
          NewObject<UNiagaraComponent>(NewActor, TEXT("NiagaraComponent"));
      NiagaraComp->SetAsset(NiagaraSystem);
      NiagaraComp->RegisterComponent();
      NewActor->AddInstanceComponent(NiagaraComp);
      NiagaraComp->AttachToComponent(NewActor->GetRootComponent(),
                                     FAttachmentTransformRules::KeepRelativeTransform);

      // Set auto-activate.
      bool bAutoActivate = true;
      if (Body->HasField(TEXT("auto_activate"))) {
        bAutoActivate = Body->GetBoolField(TEXT("auto_activate"));
      }
      NiagaraComp->SetAutoActivate(bAutoActivate);

      if (bAutoActivate) {
        NiagaraComp->Activate(true);
      }

      // Set actor label.
      FString ActorName = Body->GetStringField(TEXT("actor_name"));
      if (!ActorName.IsEmpty()) {
        NewActor->SetActorLabel(ActorName);
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetStringField(TEXT("actor_path"), NewActor->GetPathName());
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- set_parameter ---
    if (Operation == TEXT("set_parameter")) {
      const FString ActorPath = Body->GetStringField(TEXT("actor_path"));
      const FString ParamName = Body->GetStringField(TEXT("parameter_name"));
      const FString ParamType = Body->GetStringField(TEXT("parameter_type"));
      if (ActorPath.IsEmpty() || ParamName.IsEmpty()) {
        SendError(OnComplete, TEXT("actor_path and parameter_name are required for set_parameter"));
        return true;
      }

      AActor* Actor = FindObject<AActor>(nullptr, *ActorPath);
      if (!Actor) {
        SendError(OnComplete, FString::Printf(TEXT("Actor not found: '%s'"), *ActorPath));
        return true;
      }

      UNiagaraComponent* NiagaraComp = Actor->FindComponentByClass<UNiagaraComponent>();
      if (!NiagaraComp) {
        SendError(OnComplete, TEXT("Actor does not have a NiagaraComponent"));
        return true;
      }

      FName VarName(*ParamName);
      bool bSet = false;

      if (ParamType == TEXT("float")) {
        double Value;
        if (Body->TryGetNumberField(TEXT("parameter_value"), Value)) {
          NiagaraComp->SetVariableFloat(VarName, static_cast<float>(Value));
          bSet = true;
        }
      } else if (ParamType == TEXT("int")) {
        double Value;
        if (Body->TryGetNumberField(TEXT("parameter_value"), Value)) {
          NiagaraComp->SetVariableInt(VarName, static_cast<int32>(Value));
          bSet = true;
        }
      } else if (ParamType == TEXT("bool")) {
        bool Value;
        if (Body->TryGetBoolField(TEXT("parameter_value"), Value)) {
          NiagaraComp->SetVariableBool(VarName, Value);
          bSet = true;
        }
      } else if (ParamType == TEXT("vector")) {
        const TArray<TSharedPtr<FJsonValue>>* VecArray;
        if (Body->TryGetArrayField(TEXT("parameter_value"), VecArray) && VecArray->Num() >= 3) {
          FVector Vec((*VecArray)[0]->AsNumber(), (*VecArray)[1]->AsNumber(),
                      (*VecArray)[2]->AsNumber());
          NiagaraComp->SetVariableVec3(VarName, Vec);
          bSet = true;
        }
      } else if (ParamType == TEXT("color")) {
        const TArray<TSharedPtr<FJsonValue>>* ColArray;
        if (Body->TryGetArrayField(TEXT("parameter_value"), ColArray) && ColArray->Num() >= 4) {
          FLinearColor Color((*ColArray)[0]->AsNumber(), (*ColArray)[1]->AsNumber(),
                             (*ColArray)[2]->AsNumber(), (*ColArray)[3]->AsNumber());
          NiagaraComp->SetVariableLinearColor(VarName, Color);
          bSet = true;
        }
      } else {
        SendError(
            OnComplete,
            FString::Printf(
                TEXT("Unknown parameter_type: '%s'. Expected: float, int, bool, vector, color"),
                *ParamType));
        return true;
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), bSet);
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- get_system_info ---
    if (Operation == TEXT("get_system_info")) {
      const FString SystemPath = Body->GetStringField(TEXT("system_path"));
      if (SystemPath.IsEmpty()) {
        SendError(OnComplete, TEXT("system_path is required for get_system_info"));
        return true;
      }

      UNiagaraSystem* NiagaraSystem = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
      if (!NiagaraSystem) {
        SendError(OnComplete, FString::Printf(TEXT("Niagara system not found: '%s'"), *SystemPath));
        return true;
      }

      // List emitter handles.
      TArray<TSharedPtr<FJsonValue>> EmittersArray;
      for (const FNiagaraEmitterHandle& Handle : NiagaraSystem->GetEmitterHandles()) {
        TSharedPtr<FJsonObject> EmitterJson = MakeShareable(new FJsonObject());
        EmitterJson->SetStringField(TEXT("name"), Handle.GetName().ToString());
        EmitterJson->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
        EmittersArray.Add(MakeShareable(new FJsonValueObject(EmitterJson)));
      }

      // List exposed user parameters.
      TArray<TSharedPtr<FJsonValue>> ParamsArray;
      for (const FNiagaraVariableWithOffset& VarWithOffset :
           NiagaraSystem->GetExposedParameters().ReadParameterVariables()) {
        TSharedPtr<FJsonObject> ParamJson = MakeShareable(new FJsonObject());
        ParamJson->SetStringField(TEXT("name"), VarWithOffset.GetName().ToString());
        ParamJson->SetStringField(TEXT("type"), VarWithOffset.GetType().GetName());
        ParamsArray.Add(MakeShareable(new FJsonValueObject(ParamJson)));
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetArrayField(TEXT("emitters"), EmittersArray);
      ResponseJson->SetArrayField(TEXT("parameters"), ParamsArray);
      ResponseJson->SetNumberField(TEXT("emitter_count"), EmittersArray.Num());
      ResponseJson->SetNumberField(TEXT("parameter_count"), ParamsArray.Num());
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- add_emitter ---
    if (Operation == TEXT("add_emitter")) {
      const FString SystemPath = Body->GetStringField(TEXT("system_path"));
      const FString EmitterPath = Body->GetStringField(TEXT("emitter_path"));
      if (SystemPath.IsEmpty() || EmitterPath.IsEmpty()) {
        SendError(OnComplete, TEXT("system_path and emitter_path are required for add_emitter"));
        return true;
      }

      UNiagaraSystem* NiagaraSystem = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
      if (!NiagaraSystem) {
        SendError(OnComplete, FString::Printf(TEXT("Niagara system not found: '%s'"), *SystemPath));
        return true;
      }

      UNiagaraEmitter* Emitter = LoadObject<UNiagaraEmitter>(nullptr, *EmitterPath);
      if (!Emitter) {
        SendError(OnComplete,
                  FString::Printf(TEXT("Niagara emitter not found: '%s'"), *EmitterPath));
        return true;
      }

      FNiagaraEmitterHandle NewHandle =
          NiagaraSystem->AddEmitterHandle(*Emitter, Emitter->GetFName(), FGuid::NewGuid());

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetStringField(TEXT("emitter_name"), NewHandle.GetName().ToString());
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- remove_emitter ---
    if (Operation == TEXT("remove_emitter")) {
      const FString SystemPath = Body->GetStringField(TEXT("system_path"));
      const FString EmitterName = Body->GetStringField(TEXT("emitter_name"));
      if (SystemPath.IsEmpty() || EmitterName.IsEmpty()) {
        SendError(OnComplete, TEXT("system_path and emitter_name are required for remove_emitter"));
        return true;
      }

      UNiagaraSystem* NiagaraSystem = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
      if (!NiagaraSystem) {
        SendError(OnComplete, FString::Printf(TEXT("Niagara system not found: '%s'"), *SystemPath));
        return true;
      }

      bool bRemoved = false;
      const TArray<FNiagaraEmitterHandle>& Handles = NiagaraSystem->GetEmitterHandles();
      for (int32 i = 0; i < Handles.Num(); i++) {
        if (Handles[i].GetName().ToString() == EmitterName) {
          NiagaraSystem->RemoveEmitterHandle(Handles[i]);
          bRemoved = true;
          break;
        }
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), bRemoved);
      if (!bRemoved) {
        ResponseJson->SetStringField(
            TEXT("error"), FString::Printf(TEXT("Emitter '%s' not found in system"), *EmitterName));
      }
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- activate ---
    if (Operation == TEXT("activate")) {
      const FString ActorPath = Body->GetStringField(TEXT("actor_path"));
      if (ActorPath.IsEmpty()) {
        SendError(OnComplete, TEXT("actor_path is required for activate"));
        return true;
      }

      AActor* Actor = FindObject<AActor>(nullptr, *ActorPath);
      if (!Actor) {
        SendError(OnComplete, FString::Printf(TEXT("Actor not found: '%s'"), *ActorPath));
        return true;
      }

      UNiagaraComponent* NiagaraComp = Actor->FindComponentByClass<UNiagaraComponent>();
      if (!NiagaraComp) {
        SendError(OnComplete, TEXT("Actor does not have a NiagaraComponent"));
        return true;
      }

      NiagaraComp->Activate(true);

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- deactivate ---
    if (Operation == TEXT("deactivate")) {
      const FString ActorPath = Body->GetStringField(TEXT("actor_path"));
      if (ActorPath.IsEmpty()) {
        SendError(OnComplete, TEXT("actor_path is required for deactivate"));
        return true;
      }

      AActor* Actor = FindObject<AActor>(nullptr, *ActorPath);
      if (!Actor) {
        SendError(OnComplete, FString::Printf(TEXT("Actor not found: '%s'"), *ActorPath));
        return true;
      }

      UNiagaraComponent* NiagaraComp = Actor->FindComponentByClass<UNiagaraComponent>();
      if (!NiagaraComp) {
        SendError(OnComplete, TEXT("Actor does not have a NiagaraComponent"));
        return true;
      }

      NiagaraComp->Deactivate();

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    SendError(OnComplete, FString::Printf(TEXT("Unknown Niagara operation: '%s'"), *Operation));
    return true;
#else
    SendError(OnComplete,
              TEXT("Niagara module is not available. Enable the Niagara plugin in your project to "
                   "use niagara_ops."),
              501);
    return true;
#endif
  }

  // ---------------------------------------------------------------------------
  // Registration
  // ---------------------------------------------------------------------------

  void RegisterNiagaraRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles) {
    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/niagara/ops")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleNiagaraOps)));

    UE_LOG(LogMCPUnreal, Verbose, TEXT("Registered Niagara routes (1 endpoint)"));
  }

}  // namespace MCPUnreal
