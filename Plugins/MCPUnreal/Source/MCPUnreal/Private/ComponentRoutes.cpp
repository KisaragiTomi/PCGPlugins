// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.
//
// ComponentRoutes.cpp — HTTP route for actor component introspection.
// Returns the full component hierarchy for a given actor.
// See issue #40.

#include "MCPUnrealUtils.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"

namespace MCPUnreal {

  // Build JSON for a single component, recursively including children.
  static TSharedPtr<FJsonObject> ComponentToJson(USceneComponent* Component,
                                                 bool bIncludeTransforms) {
    if (!Component) {
      return nullptr;
    }

    TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject());
    Json->SetStringField(TEXT("name"), Component->GetName());
    Json->SetStringField(TEXT("class"), Component->GetClass()->GetName());
    Json->SetBoolField(TEXT("visible"), Component->IsVisible());

    // Include instance count for ISM components.
    if (const UInstancedStaticMeshComponent* ISM = Cast<UInstancedStaticMeshComponent>(Component)) {
      Json->SetNumberField(TEXT("instance_count"), ISM->GetInstanceCount());
    }

    // Include mesh asset reference for mesh components.
    if (const UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(Component)) {
      if (SMC->GetStaticMesh()) {
        Json->SetStringField(TEXT("mesh"), SMC->GetStaticMesh()->GetPathName());
      }
    } else if (const USkeletalMeshComponent* SkMC = Cast<USkeletalMeshComponent>(Component)) {
      if (SkMC->GetSkeletalMeshAsset()) {
        Json->SetStringField(TEXT("mesh"), SkMC->GetSkeletalMeshAsset()->GetPathName());
      }
    }

    // Include transform if requested.
    if (bIncludeTransforms) {
      FVector Location = Component->GetRelativeLocation();
      FRotator Rotation = Component->GetRelativeRotation();
      FVector Scale = Component->GetRelativeScale3D();

      TSharedPtr<FJsonObject> TransformJson = MakeShareable(new FJsonObject());

      TArray<TSharedPtr<FJsonValue>> LocArray;
      LocArray.Add(MakeShareable(new FJsonValueNumber(Location.X)));
      LocArray.Add(MakeShareable(new FJsonValueNumber(Location.Y)));
      LocArray.Add(MakeShareable(new FJsonValueNumber(Location.Z)));
      TransformJson->SetArrayField(TEXT("location"), LocArray);

      TArray<TSharedPtr<FJsonValue>> RotArray;
      RotArray.Add(MakeShareable(new FJsonValueNumber(Rotation.Pitch)));
      RotArray.Add(MakeShareable(new FJsonValueNumber(Rotation.Yaw)));
      RotArray.Add(MakeShareable(new FJsonValueNumber(Rotation.Roll)));
      TransformJson->SetArrayField(TEXT("rotation"), RotArray);

      TArray<TSharedPtr<FJsonValue>> ScaleArray;
      ScaleArray.Add(MakeShareable(new FJsonValueNumber(Scale.X)));
      ScaleArray.Add(MakeShareable(new FJsonValueNumber(Scale.Y)));
      ScaleArray.Add(MakeShareable(new FJsonValueNumber(Scale.Z)));
      TransformJson->SetArrayField(TEXT("scale"), ScaleArray);

      Json->SetObjectField(TEXT("transform"), TransformJson);
    }

    // Recurse into children.
    TArray<USceneComponent*> Children;
    Component->GetChildrenComponents(false, Children);

    if (Children.Num() > 0) {
      TArray<TSharedPtr<FJsonValue>> ChildArray;
      for (USceneComponent* Child : Children) {
        if (Child) {
          TSharedPtr<FJsonObject> ChildJson = ComponentToJson(Child, bIncludeTransforms);
          if (ChildJson) {
            ChildArray.Add(MakeShareable(new FJsonValueObject(ChildJson)));
          }
        }
      }
      Json->SetArrayField(TEXT("children"), ChildArray);
    }

    return Json;
  }

  // Build JSON for a non-scene component (no hierarchy, no transform).
  static TSharedPtr<FJsonObject> NonSceneComponentToJson(UActorComponent* Component) {
    if (!Component) {
      return nullptr;
    }

    TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject());
    Json->SetStringField(TEXT("name"), Component->GetName());
    Json->SetStringField(TEXT("class"), Component->GetClass()->GetName());
    Json->SetBoolField(TEXT("is_active"), Component->IsActive());

    return Json;
  }

  // ---------------------------------------------------------------------------
  // POST /api/actors/components
  // ---------------------------------------------------------------------------

  static bool HandleActorsComponents(const FHttpServerRequest& Request,
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

    const FString ActorPath = Body->GetStringField(TEXT("actor_path"));
    const FString ActorName = Body->GetStringField(TEXT("actor_name"));
    const bool bIncludeTransforms = Body->GetBoolField(TEXT("include_transforms"));

    if (ActorPath.IsEmpty() && ActorName.IsEmpty()) {
      SendError(OnComplete, TEXT("Either actor_path or actor_name is required"));
      return true;
    }

    // Find the actor.
    AActor* FoundActor = nullptr;
    for (TActorIterator<AActor> It(World); It; ++It) {
      AActor* Actor = *It;
      if (!Actor || Actor->IsPendingKillPending()) {
        continue;
      }

      if (!ActorPath.IsEmpty() && Actor->GetPathName() == ActorPath) {
        FoundActor = Actor;
        break;
      }
      if (!ActorName.IsEmpty() && Actor->GetActorNameOrLabel() == ActorName) {
        FoundActor = Actor;
        break;
      }
    }

    if (!FoundActor) {
      FString SearchKey = ActorPath.IsEmpty() ? ActorName : ActorPath;
      SendError(OnComplete, FString::Printf(TEXT("Actor not found: %s"), *SearchKey));
      return true;
    }

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    ResponseJson->SetStringField(TEXT("actor"), FoundActor->GetActorNameOrLabel());
    ResponseJson->SetStringField(TEXT("class"), FoundActor->GetClass()->GetName());
    ResponseJson->SetStringField(TEXT("path"), FoundActor->GetPathName());

    // Build the scene component tree starting from the root.
    USceneComponent* RootComponent = FoundActor->GetRootComponent();
    if (RootComponent) {
      TSharedPtr<FJsonObject> RootJson = ComponentToJson(RootComponent, bIncludeTransforms);
      if (RootJson) {
        // Flatten: put the root's children as the top-level "components" array,
        // and include the root itself as the first element.
        TArray<TSharedPtr<FJsonValue>> ComponentArray;
        ComponentArray.Add(MakeShareable(new FJsonValueObject(RootJson)));
        ResponseJson->SetArrayField(TEXT("components"), ComponentArray);
      }
    }

    // Also include non-scene components (gameplay components without transforms).
    TArray<UActorComponent*> AllComponents;
    FoundActor->GetComponents(AllComponents);

    TArray<TSharedPtr<FJsonValue>> NonSceneArray;
    for (UActorComponent* Comp : AllComponents) {
      if (Comp && !Cast<USceneComponent>(Comp)) {
        TSharedPtr<FJsonObject> CompJson = NonSceneComponentToJson(Comp);
        if (CompJson) {
          NonSceneArray.Add(MakeShareable(new FJsonValueObject(CompJson)));
        }
      }
    }
    if (NonSceneArray.Num() > 0) {
      ResponseJson->SetArrayField(TEXT("non_scene_components"), NonSceneArray);
    }

    // Total component count.
    ResponseJson->SetNumberField(TEXT("total_components"), AllComponents.Num());

    SendJson(OnComplete, ResponseJson);
    return true;
  }

  // ---------------------------------------------------------------------------
  // Registration
  // ---------------------------------------------------------------------------

  void RegisterComponentRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles) {
    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/actors/components")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleActorsComponents)));

    UE_LOG(LogMCPUnreal, Verbose, TEXT("Registered component routes (1 endpoint)"));
  }

}  // namespace MCPUnreal
