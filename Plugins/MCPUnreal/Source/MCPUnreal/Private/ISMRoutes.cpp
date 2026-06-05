// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.
//
// ISMRoutes.cpp — HTTP routes for InstancedStaticMesh (ISM) management.
// Supports create, add_instances, clear, count, update, remove, set_material.
// See issue #41.

#include "MCPUnrealUtils.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"

namespace MCPUnreal {

  // Find an actor by path or name.
  static AActor* FindActorByPathOrName(UWorld* World, const FString& ActorPath,
                                       const FString& ActorName) {
    for (TActorIterator<AActor> It(World); It; ++It) {
      AActor* Actor = *It;
      if (!Actor || Actor->IsPendingKillPending()) {
        continue;
      }
      if (!ActorPath.IsEmpty() && Actor->GetPathName() == ActorPath) {
        return Actor;
      }
      if (!ActorName.IsEmpty() && Actor->GetActorNameOrLabel() == ActorName) {
        return Actor;
      }
    }
    return nullptr;
  }

  // Find an ISM component on an actor by name.
  static UInstancedStaticMeshComponent* FindISMComponent(AActor* Actor,
                                                         const FString& ComponentName) {
    TArray<UActorComponent*> Components;
    Actor->GetComponents(Components);
    for (UActorComponent* Comp : Components) {
      UInstancedStaticMeshComponent* ISM = Cast<UInstancedStaticMeshComponent>(Comp);
      if (ISM && ISM->GetName() == ComponentName) {
        return ISM;
      }
    }
    return nullptr;
  }

  // Parse a transform from a JSON object.
  static FTransform ParseTransformFromJson(const TSharedPtr<FJsonObject>& Json) {
    FVector Location = FVector::ZeroVector;
    FRotator Rotation = FRotator::ZeroRotator;
    FVector Scale = FVector::OneVector;

    const TArray<TSharedPtr<FJsonValue>>* LocArray;
    if (Json->TryGetArrayField(TEXT("location"), LocArray) && LocArray->Num() >= 3) {
      Location.X = (*LocArray)[0]->AsNumber();
      Location.Y = (*LocArray)[1]->AsNumber();
      Location.Z = (*LocArray)[2]->AsNumber();
    }

    const TArray<TSharedPtr<FJsonValue>>* RotArray;
    if (Json->TryGetArrayField(TEXT("rotation"), RotArray) && RotArray->Num() >= 3) {
      Rotation.Pitch = (*RotArray)[0]->AsNumber();
      Rotation.Yaw = (*RotArray)[1]->AsNumber();
      Rotation.Roll = (*RotArray)[2]->AsNumber();
    }

    const TArray<TSharedPtr<FJsonValue>>* ScaleArray;
    if (Json->TryGetArrayField(TEXT("scale"), ScaleArray) && ScaleArray->Num() >= 3) {
      Scale.X = (*ScaleArray)[0]->AsNumber();
      Scale.Y = (*ScaleArray)[1]->AsNumber();
      Scale.Z = (*ScaleArray)[2]->AsNumber();
    }

    return FTransform(Rotation, Location, Scale);
  }

  // ---------------------------------------------------------------------------
  // POST /api/ism/ops
  // ---------------------------------------------------------------------------

  static bool HandleISMOps(const FHttpServerRequest& Request,
                           const FHttpResultCallback& OnComplete) {
    TSharedPtr<FJsonObject> Body;
    if (!ParseJsonBody(Request, Body)) {
      SendError(OnComplete, TEXT("Invalid JSON in request body"));
      return true;
    }

    UWorld* World = GetWorld(Body);
    if (!World) {
      SendError(OnComplete,
                TEXT("World not available — if world=pie was requested, ensure PIE is running"),
                500);
      return true;
    }

    const FString Operation = Body->GetStringField(TEXT("operation"));
    if (Operation.IsEmpty()) {
      SendError(OnComplete, TEXT("operation is required"));
      return true;
    }

    // --- create ---
    if (Operation == TEXT("create")) {
      const FString ActorPath = Body->GetStringField(TEXT("actor_path"));
      const FString ActorName = Body->GetStringField(TEXT("actor_name"));
      const FString MeshPath = Body->GetStringField(TEXT("mesh"));
      const FString MaterialPath = Body->GetStringField(TEXT("material"));
      const bool bUseHISM = Body->GetBoolField(TEXT("use_hism"));

      if (ActorPath.IsEmpty() && ActorName.IsEmpty()) {
        SendError(OnComplete, TEXT("actor_path or actor_name is required for create"));
        return true;
      }

      AActor* Actor = FindActorByPathOrName(World, ActorPath, ActorName);
      if (!Actor) {
        SendError(OnComplete, FString::Printf(TEXT("Actor not found: %s"),
                                              *(ActorPath.IsEmpty() ? ActorName : ActorPath)));
        return true;
      }

      // Create the ISM or HISM component.
      UInstancedStaticMeshComponent* NewISM = nullptr;
      if (bUseHISM) {
        NewISM = NewObject<UHierarchicalInstancedStaticMeshComponent>(Actor);
      } else {
        NewISM = NewObject<UInstancedStaticMeshComponent>(Actor);
      }

      NewISM->RegisterComponent();
      NewISM->AttachToComponent(Actor->GetRootComponent(),
                                FAttachmentTransformRules::KeepRelativeTransform);

      // Set mesh if provided.
      if (!MeshPath.IsEmpty()) {
        UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
        if (Mesh) {
          NewISM->SetStaticMesh(Mesh);
        } else {
          UE_LOG(LogMCPUnreal, Warning, TEXT("Static mesh not found: %s"), *MeshPath);
        }
      }

      // Set material if provided.
      if (!MaterialPath.IsEmpty()) {
        UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
        if (Material) {
          NewISM->SetMaterial(0, Material);
        }
      }

      Actor->AddInstanceComponent(NewISM);

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetStringField(TEXT("component_name"), NewISM->GetName());
      ResponseJson->SetNumberField(TEXT("instance_count"), 0);

      UE_LOG(LogMCPUnreal, Log, TEXT("Created %s component '%s' on actor '%s'"),
             bUseHISM ? TEXT("HISM") : TEXT("ISM"), *NewISM->GetName(),
             *Actor->GetActorNameOrLabel());

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // For all other operations, we need component_name and the actor.
    const FString ComponentName = Body->GetStringField(TEXT("component_name"));
    const FString ActorPath = Body->GetStringField(TEXT("actor_path"));
    const FString ActorName = Body->GetStringField(TEXT("actor_name"));

    if (ComponentName.IsEmpty()) {
      SendError(OnComplete, TEXT("component_name is required for this operation"));
      return true;
    }
    if (ActorPath.IsEmpty() && ActorName.IsEmpty()) {
      SendError(OnComplete, TEXT("actor_path or actor_name is required"));
      return true;
    }

    AActor* Actor = FindActorByPathOrName(World, ActorPath, ActorName);
    if (!Actor) {
      SendError(OnComplete, FString::Printf(TEXT("Actor not found: %s"),
                                            *(ActorPath.IsEmpty() ? ActorName : ActorPath)));
      return true;
    }

    UInstancedStaticMeshComponent* ISM = FindISMComponent(Actor, ComponentName);
    if (!ISM) {
      SendError(OnComplete,
                FString::Printf(TEXT("ISM component '%s' not found on actor"), *ComponentName));
      return true;
    }

    // --- add_instances ---
    if (Operation == TEXT("add_instances")) {
      const TArray<TSharedPtr<FJsonValue>>* TransformsArray;
      if (!Body->TryGetArrayField(TEXT("transforms"), TransformsArray) ||
          TransformsArray->Num() == 0) {
        SendError(OnComplete, TEXT("transforms array is required for add_instances"));
        return true;
      }

      int32 AddedCount = 0;
      for (const auto& Val : *TransformsArray) {
        const TSharedPtr<FJsonObject>* TransformObj;
        if (Val->TryGetObject(TransformObj)) {
          FTransform Transform = ParseTransformFromJson(*TransformObj);
          ISM->AddInstance(Transform, /*bWorldSpace=*/true);
          AddedCount++;
        }
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetStringField(TEXT("component_name"), ComponentName);
      ResponseJson->SetNumberField(TEXT("instance_count"), ISM->GetInstanceCount());
      ResponseJson->SetNumberField(TEXT("added_count"), AddedCount);

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- clear_instances ---
    if (Operation == TEXT("clear_instances")) {
      ISM->ClearInstances();

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetStringField(TEXT("component_name"), ComponentName);
      ResponseJson->SetNumberField(TEXT("instance_count"), 0);

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- get_instance_count ---
    if (Operation == TEXT("get_instance_count")) {
      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetStringField(TEXT("component_name"), ComponentName);
      ResponseJson->SetNumberField(TEXT("instance_count"), ISM->GetInstanceCount());

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- update_instance ---
    if (Operation == TEXT("update_instance")) {
      int32 InstanceIndex = static_cast<int32>(Body->GetNumberField(TEXT("instance_index")));
      if (InstanceIndex < 0 || InstanceIndex >= ISM->GetInstanceCount()) {
        SendError(OnComplete, FString::Printf(TEXT("Instance index %d out of range [0, %d)"),
                                              InstanceIndex, ISM->GetInstanceCount()));
        return true;
      }

      const TSharedPtr<FJsonObject>* TransformObj;
      if (!Body->TryGetObjectField(TEXT("transform"), TransformObj)) {
        SendError(OnComplete, TEXT("transform is required for update_instance"));
        return true;
      }

      FTransform NewTransform = ParseTransformFromJson(*TransformObj);
      ISM->UpdateInstanceTransform(InstanceIndex, NewTransform, /*bWorldSpace=*/true,
                                   /*bMarkRenderStateDirty=*/true);

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetStringField(TEXT("component_name"), ComponentName);
      ResponseJson->SetNumberField(TEXT("instance_count"), ISM->GetInstanceCount());

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- remove_instance ---
    if (Operation == TEXT("remove_instance")) {
      int32 InstanceIndex = static_cast<int32>(Body->GetNumberField(TEXT("instance_index")));
      if (InstanceIndex < 0 || InstanceIndex >= ISM->GetInstanceCount()) {
        SendError(OnComplete, FString::Printf(TEXT("Instance index %d out of range [0, %d)"),
                                              InstanceIndex, ISM->GetInstanceCount()));
        return true;
      }

      ISM->RemoveInstance(InstanceIndex);

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetStringField(TEXT("component_name"), ComponentName);
      ResponseJson->SetNumberField(TEXT("instance_count"), ISM->GetInstanceCount());

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- set_material ---
    if (Operation == TEXT("set_material")) {
      const FString MaterialPath = Body->GetStringField(TEXT("material"));
      if (MaterialPath.IsEmpty()) {
        SendError(OnComplete, TEXT("material is required for set_material"));
        return true;
      }

      UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
      if (!Material) {
        SendError(OnComplete, FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
        return true;
      }

      ISM->SetMaterial(0, Material);

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetStringField(TEXT("component_name"), ComponentName);
      ResponseJson->SetNumberField(TEXT("instance_count"), ISM->GetInstanceCount());

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    SendError(OnComplete, FString::Printf(TEXT("Unknown ISM operation: %s"), *Operation));
    return true;
  }

  // ---------------------------------------------------------------------------
  // Registration
  // ---------------------------------------------------------------------------

  void RegisterISMRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles) {
    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/ism/ops")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleISMOps)));

    UE_LOG(LogMCPUnreal, Verbose, TEXT("Registered ISM routes (1 endpoint)"));
  }

}  // namespace MCPUnreal
