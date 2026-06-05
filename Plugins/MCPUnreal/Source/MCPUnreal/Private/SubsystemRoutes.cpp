// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.
//
// SubsystemRoutes.cpp — HTTP route for querying active UE subsystems
// (World, GameInstance, Engine, Editor, LocalPlayer).
// See issue #45.

#include "MCPUnrealUtils.h"

#include "Subsystems/WorldSubsystem.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Subsystems/EngineSubsystem.h"
#include "Subsystems/LocalPlayerSubsystem.h"
#include "EditorSubsystem.h"
#include "Kismet/GameplayStatics.h"

namespace MCPUnreal {

  // Add subsystem info to the array.
  static void AddSubsystemInfo(TArray<TSharedPtr<FJsonValue>>& OutArray, const FString& ClassName,
                               const FString& TypeName, bool bInitialized) {
    TSharedPtr<FJsonObject> Info = MakeShareable(new FJsonObject());
    Info->SetStringField(TEXT("class"), ClassName);
    Info->SetStringField(TEXT("type"), TypeName);
    Info->SetBoolField(TEXT("initialized"), bInitialized);
    OutArray.Add(MakeShareable(new FJsonValueObject(Info)));
  }

  // Collect world subsystems.
  static void CollectWorldSubsystems(UWorld* World, TArray<TSharedPtr<FJsonValue>>& OutArray) {
    if (!World) return;

    TArray<UWorldSubsystem*> Subsystems = World->GetSubsystemArrayCopy<UWorldSubsystem>();
    for (const UWorldSubsystem* Sub : Subsystems) {
      if (Sub) {
        AddSubsystemInfo(OutArray, Sub->GetClass()->GetName(), TEXT("world"), true);
      }
    }
  }

  // Collect game instance subsystems.
  static void CollectGameInstanceSubsystems(UWorld* World,
                                            TArray<TSharedPtr<FJsonValue>>& OutArray) {
    if (!World) return;
    UGameInstance* GI = World->GetGameInstance();
    if (!GI) return;

    TArray<UGameInstanceSubsystem*> Subsystems =
        GI->GetSubsystemArrayCopy<UGameInstanceSubsystem>();
    for (const UGameInstanceSubsystem* Sub : Subsystems) {
      if (Sub) {
        AddSubsystemInfo(OutArray, Sub->GetClass()->GetName(), TEXT("game_instance"), true);
      }
    }
  }

  // Collect engine subsystems.
  static void CollectEngineSubsystems(TArray<TSharedPtr<FJsonValue>>& OutArray) {
    if (!GEngine) return;

    TArray<UEngineSubsystem*> Subsystems = GEngine->GetEngineSubsystemArrayCopy<UEngineSubsystem>();
    for (const UEngineSubsystem* Sub : Subsystems) {
      if (Sub) {
        AddSubsystemInfo(OutArray, Sub->GetClass()->GetName(), TEXT("engine"), true);
      }
    }
  }

  // Collect editor subsystems.
  static void CollectEditorSubsystems(TArray<TSharedPtr<FJsonValue>>& OutArray) {
    if (!GEditor) return;

    TArray<UEditorSubsystem*> Subsystems = GEditor->GetEditorSubsystemArrayCopy<UEditorSubsystem>();
    for (const UEditorSubsystem* Sub : Subsystems) {
      if (Sub) {
        AddSubsystemInfo(OutArray, Sub->GetClass()->GetName(), TEXT("editor"), true);
      }
    }
  }

  // Collect local player subsystems.
  static void CollectLocalPlayerSubsystems(UWorld* World,
                                           TArray<TSharedPtr<FJsonValue>>& OutArray) {
    if (!World) return;
    ULocalPlayer* LP = World->GetFirstLocalPlayerFromController();
    if (!LP) return;

    TArray<ULocalPlayerSubsystem*> Subsystems = LP->GetSubsystemArrayCopy<ULocalPlayerSubsystem>();
    for (const ULocalPlayerSubsystem* Sub : Subsystems) {
      if (Sub) {
        AddSubsystemInfo(OutArray, Sub->GetClass()->GetName(), TEXT("local_player"), true);
      }
    }
  }

  // ---------------------------------------------------------------------------
  // POST /api/subsystems/query
  // ---------------------------------------------------------------------------

  static bool HandleSubsystemQuery(const FHttpServerRequest& Request,
                                   const FHttpResultCallback& OnComplete) {
    TSharedPtr<FJsonObject> Body;
    if (!ParseJsonBody(Request, Body)) {
      SendError(OnComplete, TEXT("Invalid JSON in request body"));
      return true;
    }

    const FString Type = Body->GetStringField(TEXT("type"));
    if (Type.IsEmpty()) {
      SendError(
          OnComplete,
          TEXT("type is required (world, game_instance, engine, editor, local_player, or all)"));
      return true;
    }

    UWorld* World = GetWorld(Body);
    TArray<TSharedPtr<FJsonValue>> SubsystemsArray;

    if (Type == TEXT("world") || Type == TEXT("all")) {
      CollectWorldSubsystems(World, SubsystemsArray);
    }
    if (Type == TEXT("game_instance") || Type == TEXT("all")) {
      CollectGameInstanceSubsystems(World, SubsystemsArray);
    }
    if (Type == TEXT("engine") || Type == TEXT("all")) {
      CollectEngineSubsystems(SubsystemsArray);
    }
    if (Type == TEXT("editor") || Type == TEXT("all")) {
      CollectEditorSubsystems(SubsystemsArray);
    }
    if (Type == TEXT("local_player") || Type == TEXT("all")) {
      CollectLocalPlayerSubsystems(World, SubsystemsArray);
    }

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    ResponseJson->SetArrayField(TEXT("subsystems"), SubsystemsArray);
    ResponseJson->SetNumberField(TEXT("count"), SubsystemsArray.Num());

    SendJson(OnComplete, ResponseJson);
    return true;
  }

  // ---------------------------------------------------------------------------
  // Registration
  // ---------------------------------------------------------------------------

  void RegisterSubsystemRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles) {
    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/subsystems/query")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleSubsystemQuery)));

    UE_LOG(LogMCPUnreal, Verbose, TEXT("Registered subsystem routes (1 endpoint)"));
  }

}  // namespace MCPUnreal
