// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.
//
// ActorRoutes.cpp — HTTP routes for actor management: list, spawn, delete.
// See IMPLEMENTATION.md §3.3 and §5.1 for the endpoint specification.

#include "MCPUnrealUtils.h"

#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "UObject/UObjectIterator.h"

namespace MCPUnreal {

  // ---------------------------------------------------------------------------
  // POST /api/actors/list
  // ---------------------------------------------------------------------------

  static bool HandleActorsList(const FHttpServerRequest& Request,
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

    const FString ClassFilter = Body->GetStringField(TEXT("class_filter"));
    const FString NameFilter = Body->GetStringField(TEXT("name_filter"));
    const FString TagFilter = Body->GetStringField(TEXT("tag_filter"));

    TArray<TSharedPtr<FJsonValue>> ActorArray;

    for (TActorIterator<AActor> It(World); It; ++It) {
      AActor* Actor = *It;
      if (!Actor || Actor->IsPendingKillPending()) {
        continue;
      }

      // Apply class filter.
      if (!ClassFilter.IsEmpty()) {
        FString ClassName = Actor->GetClass()->GetName();
        if (!ClassName.Contains(ClassFilter)) {
          continue;
        }
      }

      // Apply name filter.
      if (!NameFilter.IsEmpty()) {
        FString ActorLabel = Actor->GetActorNameOrLabel();
        if (!ActorLabel.Contains(NameFilter)) {
          continue;
        }
      }

      // Apply tag filter.
      if (!TagFilter.IsEmpty()) {
        bool bHasTag = false;
        for (const FName& Tag : Actor->Tags) {
          if (Tag.ToString().Contains(TagFilter)) {
            bHasTag = true;
            break;
          }
        }
        if (!bHasTag) {
          continue;
        }
      }

      FVector Location = Actor->GetActorLocation();
      FRotator Rotation = Actor->GetActorRotation();
      FVector Scale = Actor->GetActorScale3D();

      TSharedPtr<FJsonObject> ActorJson = MakeShareable(new FJsonObject());
      ActorJson->SetStringField(TEXT("name"), Actor->GetActorNameOrLabel());
      ActorJson->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
      ActorJson->SetStringField(TEXT("path"), Actor->GetPathName());

      // Location array [X, Y, Z].
      TArray<TSharedPtr<FJsonValue>> LocArray;
      LocArray.Add(MakeShareable(new FJsonValueNumber(Location.X)));
      LocArray.Add(MakeShareable(new FJsonValueNumber(Location.Y)));
      LocArray.Add(MakeShareable(new FJsonValueNumber(Location.Z)));
      ActorJson->SetArrayField(TEXT("location"), LocArray);

      // Rotation array [Pitch, Yaw, Roll].
      TArray<TSharedPtr<FJsonValue>> RotArray;
      RotArray.Add(MakeShareable(new FJsonValueNumber(Rotation.Pitch)));
      RotArray.Add(MakeShareable(new FJsonValueNumber(Rotation.Yaw)));
      RotArray.Add(MakeShareable(new FJsonValueNumber(Rotation.Roll)));
      ActorJson->SetArrayField(TEXT("rotation"), RotArray);

      // Scale array [X, Y, Z].
      TArray<TSharedPtr<FJsonValue>> ScaleArray;
      ScaleArray.Add(MakeShareable(new FJsonValueNumber(Scale.X)));
      ScaleArray.Add(MakeShareable(new FJsonValueNumber(Scale.Y)));
      ScaleArray.Add(MakeShareable(new FJsonValueNumber(Scale.Z)));
      ActorJson->SetArrayField(TEXT("scale"), ScaleArray);

      ActorArray.Add(MakeShareable(new FJsonValueObject(ActorJson)));
    }

    SendJsonArray(OnComplete, ActorArray);
    return true;
  }

  // ---------------------------------------------------------------------------
  // POST /api/actors/spawn
  // ---------------------------------------------------------------------------

  static bool HandleActorsSpawn(const FHttpServerRequest& Request,
                                const FHttpResultCallback& OnComplete) {
    TSharedPtr<FJsonObject> Body;
    if (!ParseJsonBody(Request, Body)) {
      SendError(OnComplete, TEXT("Invalid JSON in request body"));
      return true;
    }

    const FString ClassName = Body->GetStringField(TEXT("class_name"));
    if (ClassName.IsEmpty()) {
      SendError(OnComplete, TEXT("class_name is required"));
      return true;
    }

    UWorld* World = GetWorld(Body);
    if (!World) {
      SendError(OnComplete,
                TEXT("World not available — if world=pie was requested, ensure PIE is running"),
                500);
      return true;
    }

    // Resolve the class. Try direct lookup first, then with common prefixes.
    UClass* ActorClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::None);
    if (!ActorClass) {
      // Try with 'A' prefix (e.g. "PointLight" -> "APointLight").
      ActorClass = FindFirstObject<UClass>(*(TEXT("A") + ClassName), EFindFirstObjectOptions::None);
    }
    if (!ActorClass) {
      // Try loading by full path.
      ActorClass = LoadClass<AActor>(nullptr, *ClassName);
    }
    if (!ActorClass || !ActorClass->IsChildOf(AActor::StaticClass())) {
      SendError(
          OnComplete,
          FString::Printf(TEXT("Actor class '%s' not found or is not an Actor class"), *ClassName));
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

    // Spawn the actor.
    FTransform SpawnTransform(Rotation, Location, Scale);
    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    const FString RequestedName = Body->GetStringField(TEXT("name"));
    if (!RequestedName.IsEmpty()) {
      SpawnParams.Name = FName(*RequestedName);
    }

    AActor* NewActor = World->SpawnActor<AActor>(ActorClass, SpawnTransform, SpawnParams);
    if (!NewActor) {
      SendError(OnComplete,
                FString::Printf(TEXT("Failed to spawn actor of class '%s'"), *ClassName), 500);
      return true;
    }

    if (!RequestedName.IsEmpty()) {
      NewActor->SetActorLabel(RequestedName);
    }

    UE_LOG(LogMCPUnreal, Log, TEXT("Spawned actor '%s' (%s) at (%s)"),
           *NewActor->GetActorNameOrLabel(), *ClassName, *Location.ToString());

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    ResponseJson->SetStringField(TEXT("actor_path"), NewActor->GetPathName());
    ResponseJson->SetStringField(TEXT("actor_name"), NewActor->GetActorNameOrLabel());
    ResponseJson->SetStringField(TEXT("class"), ActorClass->GetName());

    SendJson(OnComplete, ResponseJson);
    return true;
  }

  // ---------------------------------------------------------------------------
  // POST /api/actors/delete
  // ---------------------------------------------------------------------------

  static bool HandleActorsDelete(const FHttpServerRequest& Request,
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

    // Collect actors to delete by path or name.
    TSet<FString> PathsToDelete;
    TSet<FString> NamesToDelete;

    const TArray<TSharedPtr<FJsonValue>>* PathsArray;
    if (Body->TryGetArrayField(TEXT("actor_paths"), PathsArray)) {
      for (const auto& Val : *PathsArray) {
        PathsToDelete.Add(Val->AsString());
      }
    }

    const TArray<TSharedPtr<FJsonValue>>* NamesArray;
    if (Body->TryGetArrayField(TEXT("actor_names"), NamesArray)) {
      for (const auto& Val : *NamesArray) {
        NamesToDelete.Add(Val->AsString());
      }
    }

    if (PathsToDelete.Num() == 0 && NamesToDelete.Num() == 0) {
      SendError(OnComplete, TEXT("At least one of actor_paths or actor_names is required"));
      return true;
    }

    TArray<TSharedPtr<FJsonValue>> DeletedNames;
    int32 DeletedCount = 0;

    for (TActorIterator<AActor> It(World); It; ++It) {
      AActor* Actor = *It;
      if (!Actor || Actor->IsPendingKillPending()) {
        continue;
      }

      bool bShouldDelete = false;
      if (PathsToDelete.Contains(Actor->GetPathName())) {
        bShouldDelete = true;
      }
      if (NamesToDelete.Contains(Actor->GetActorNameOrLabel())) {
        bShouldDelete = true;
      }

      if (bShouldDelete) {
        FString ActorName = Actor->GetActorNameOrLabel();
        UE_LOG(LogMCPUnreal, Log, TEXT("Deleting actor '%s'"), *ActorName);
        World->DestroyActor(Actor);
        DeletedNames.Add(MakeShareable(new FJsonValueString(ActorName)));
        DeletedCount++;
      }
    }

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    ResponseJson->SetNumberField(TEXT("deleted_count"), DeletedCount);
    ResponseJson->SetArrayField(TEXT("deleted"), DeletedNames);

    SendJson(OnComplete, ResponseJson);
    return true;
  }

  // ---------------------------------------------------------------------------
  // Registration
  // ---------------------------------------------------------------------------

  void RegisterActorRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles) {
    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/actors/list")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleActorsList)));

    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/actors/spawn")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleActorsSpawn)));

    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/actors/delete")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleActorsDelete)));

    UE_LOG(LogMCPUnreal, Verbose, TEXT("Registered actor routes (3 endpoints)"));
  }

}  // namespace MCPUnreal
