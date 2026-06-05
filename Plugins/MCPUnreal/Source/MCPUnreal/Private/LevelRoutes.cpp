// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.
//
// LevelRoutes.cpp — HTTP routes for level management: load, save, create,
// streaming sublevel management.
//
// See IMPLEMENTATION.md §3.9 and §5.1.

#include "MCPUnrealUtils.h"

#include "EditorLevelUtils.h"
#include "FileHelpers.h"
#include "LevelEditorSubsystem.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Misc/PackageName.h"

namespace MCPUnreal {

  // ---------------------------------------------------------------------------
  // POST /api/levels/ops
  // ---------------------------------------------------------------------------

  static bool HandleLevelOps(const FHttpServerRequest& Request,
                             const FHttpResultCallback& OnComplete) {
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

    // --- get_current ---
    if (Operation == TEXT("get_current")) {
      UWorld* World = GetWorld(Body);
      if (!World) {
        SendError(OnComplete,
                  TEXT("World not available — if world=pie was requested, ensure PIE is running"),
                  500);
        return true;
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetStringField(TEXT("level_name"), World->GetMapName());
      ResponseJson->SetStringField(TEXT("package_name"), World->GetOutermost()->GetName());

      // List streaming levels.
      TArray<TSharedPtr<FJsonValue>> StreamingArray;
      for (ULevelStreaming* StreamingLevel : World->GetStreamingLevels()) {
        if (StreamingLevel) {
          TSharedPtr<FJsonObject> SLJson = MakeShareable(new FJsonObject());
          SLJson->SetStringField(TEXT("package_name"), StreamingLevel->GetWorldAssetPackageName());
          SLJson->SetBoolField(TEXT("loaded"), StreamingLevel->HasLoadedLevel());
          SLJson->SetBoolField(TEXT("visible"), StreamingLevel->GetShouldBeVisibleFlag());
          StreamingArray.Add(MakeShareable(new FJsonValueObject(SLJson)));
        }
      }
      ResponseJson->SetArrayField(TEXT("streaming_levels"), StreamingArray);

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- list_levels ---
    if (Operation == TEXT("list_levels")) {
      FAssetRegistryModule& AssetRegistryModule =
          FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
      IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

      TArray<FAssetData> Assets;
      AssetRegistry.GetAssetsByClass(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("World")),
                                     Assets);

      TArray<TSharedPtr<FJsonValue>> LevelsArray;
      for (const FAssetData& Asset : Assets) {
        TSharedPtr<FJsonObject> LevelJson = MakeShareable(new FJsonObject());
        LevelJson->SetStringField(TEXT("name"), Asset.AssetName.ToString());
        LevelJson->SetStringField(TEXT("path"), Asset.GetObjectPathString());
        LevelJson->SetStringField(TEXT("package"), Asset.PackageName.ToString());
        LevelsArray.Add(MakeShareable(new FJsonValueObject(LevelJson)));
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetArrayField(TEXT("levels"), LevelsArray);
      ResponseJson->SetNumberField(TEXT("count"), LevelsArray.Num());
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- load_level ---
    if (Operation == TEXT("load_level")) {
      const FString LevelPath = Body->GetStringField(TEXT("level_path"));
      if (LevelPath.IsEmpty()) {
        SendError(OnComplete, TEXT("level_path is required"));
        return true;
      }

      ULevelEditorSubsystem* LevelEditorSub = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
      if (LevelEditorSub) {
        bool bSuccess = LevelEditorSub->LoadLevel(LevelPath);
        TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
        ResponseJson->SetBoolField(TEXT("success"), bSuccess);
        ResponseJson->SetStringField(TEXT("level_path"), LevelPath);
        SendJson(OnComplete, ResponseJson);
      } else {
        SendError(OnComplete, TEXT("Level editor subsystem not available"), 500);
      }
      return true;
    }

    // --- save_level ---
    if (Operation == TEXT("save_level")) {
      UWorld* World = GetWorld(Body);
      if (!World) {
        SendError(OnComplete,
                  TEXT("World not available — if world=pie was requested, ensure PIE is running"),
                  500);
        return true;
      }

      bool bSuccess = FEditorFileUtils::SaveCurrentLevel();

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), bSuccess);
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- new_level ---
    if (Operation == TEXT("new_level")) {
      const FString LevelName = Body->GetStringField(TEXT("level_name"));
      const FString PackagePath = Body->GetStringField(TEXT("package_path"));
      if (LevelName.IsEmpty() || PackagePath.IsEmpty()) {
        SendError(OnComplete, TEXT("level_name and package_path are required"));
        return true;
      }

      ULevelEditorSubsystem* LevelEditorSub = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
      if (LevelEditorSub) {
        FString FullPath = PackagePath / LevelName;
        bool bSuccess = LevelEditorSub->NewLevel(FullPath);
        TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
        ResponseJson->SetBoolField(TEXT("success"), bSuccess);
        ResponseJson->SetStringField(TEXT("level_path"), FullPath);
        SendJson(OnComplete, ResponseJson);
      } else {
        SendError(OnComplete, TEXT("Level editor subsystem not available"), 500);
      }
      return true;
    }

    // --- add_sublevel ---
    if (Operation == TEXT("add_sublevel")) {
      const FString LevelPath = Body->GetStringField(TEXT("level_path"));
      if (LevelPath.IsEmpty()) {
        SendError(OnComplete, TEXT("level_path is required"));
        return true;
      }

      UWorld* World = GetWorld(Body);
      if (!World) {
        SendError(OnComplete,
                  TEXT("World not available — if world=pie was requested, ensure PIE is running"),
                  500);
        return true;
      }

      ULevelStreaming* NewLevel = EditorLevelUtils::AddLevelToWorld(
          World, *LevelPath, ULevelStreamingDynamic::StaticClass());

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), NewLevel != nullptr);
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- remove_sublevel ---
    if (Operation == TEXT("remove_sublevel")) {
      const FString LevelPath = Body->GetStringField(TEXT("level_path"));
      if (LevelPath.IsEmpty()) {
        SendError(OnComplete, TEXT("level_path is required"));
        return true;
      }

      UWorld* World = GetWorld(Body);
      if (!World) {
        SendError(OnComplete,
                  TEXT("World not available — if world=pie was requested, ensure PIE is running"),
                  500);
        return true;
      }

      bool bRemoved = false;
      for (ULevelStreaming* StreamingLevel : World->GetStreamingLevels()) {
        if (StreamingLevel && StreamingLevel->GetWorldAssetPackageName().Contains(LevelPath)) {
          EditorLevelUtils::RemoveLevelFromWorld(StreamingLevel->GetLoadedLevel());
          bRemoved = true;
          break;
        }
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), bRemoved);
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    SendError(OnComplete, FString::Printf(TEXT("Unknown level operation: '%s'"), *Operation));
    return true;
  }

  // ---------------------------------------------------------------------------
  // Registration
  // ---------------------------------------------------------------------------

  void RegisterLevelRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles) {
    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/levels/ops")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleLevelOps)));

    UE_LOG(LogMCPUnreal, Verbose, TEXT("Registered level routes (1 endpoint)"));
  }

}  // namespace MCPUnreal
