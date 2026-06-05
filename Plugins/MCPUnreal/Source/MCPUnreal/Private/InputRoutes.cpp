// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.
//
// InputRoutes.cpp — HTTP routes for Enhanced Input system management:
// Input Actions, Mapping Contexts, and key bindings.
//
// See IMPLEMENTATION.md §3.10 and §5.1.

#include "MCPUnrealUtils.h"

#include "InputAction.h"
#include "InputMappingContext.h"
#include "EnhancedInputComponent.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Factories/DataAssetFactory.h"
#include "ObjectTools.h"
#include "FileHelpers.h"

namespace MCPUnreal {

  // ---------------------------------------------------------------------------
  // Helpers
  // ---------------------------------------------------------------------------

  /** Map string to EInputActionValueType. */
  static EInputActionValueType ParseValueType(const FString& Str) {
    if (Str.Equals(TEXT("bool"), ESearchCase::IgnoreCase)) return EInputActionValueType::Boolean;
    if (Str.Equals(TEXT("float"), ESearchCase::IgnoreCase)) return EInputActionValueType::Axis1D;
    if (Str.Equals(TEXT("Axis1D"), ESearchCase::IgnoreCase)) return EInputActionValueType::Axis1D;
    if (Str.Equals(TEXT("Vector2D"), ESearchCase::IgnoreCase)) return EInputActionValueType::Axis2D;
    if (Str.Equals(TEXT("Axis2D"), ESearchCase::IgnoreCase)) return EInputActionValueType::Axis2D;
    if (Str.Equals(TEXT("Vector3D"), ESearchCase::IgnoreCase)) return EInputActionValueType::Axis3D;
    if (Str.Equals(TEXT("Axis3D"), ESearchCase::IgnoreCase)) return EInputActionValueType::Axis3D;
    return EInputActionValueType::Boolean;  // Default.
  }

  // ---------------------------------------------------------------------------
  // POST /api/input/ops
  // ---------------------------------------------------------------------------

  static bool HandleInputOps(const FHttpServerRequest& Request,
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

    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    // --- list_actions ---
    if (Operation == TEXT("list_actions")) {
      TArray<FAssetData> Assets;
      AssetRegistry.GetAssetsByClass(
          FTopLevelAssetPath(TEXT("/Script/EnhancedInput"), TEXT("InputAction")), Assets);

      TArray<TSharedPtr<FJsonValue>> ActionsArray;
      for (const FAssetData& Asset : Assets) {
        TSharedPtr<FJsonObject> ActionJson = MakeShareable(new FJsonObject());
        ActionJson->SetStringField(TEXT("name"), Asset.AssetName.ToString());
        ActionJson->SetStringField(TEXT("path"), Asset.GetObjectPathString());
        ActionsArray.Add(MakeShareable(new FJsonValueObject(ActionJson)));
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetArrayField(TEXT("actions"), ActionsArray);
      ResponseJson->SetNumberField(TEXT("count"), ActionsArray.Num());
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- list_contexts ---
    if (Operation == TEXT("list_contexts")) {
      TArray<FAssetData> Assets;
      AssetRegistry.GetAssetsByClass(
          FTopLevelAssetPath(TEXT("/Script/EnhancedInput"), TEXT("InputMappingContext")), Assets);

      TArray<TSharedPtr<FJsonValue>> ContextsArray;
      for (const FAssetData& Asset : Assets) {
        TSharedPtr<FJsonObject> CtxJson = MakeShareable(new FJsonObject());
        CtxJson->SetStringField(TEXT("name"), Asset.AssetName.ToString());
        CtxJson->SetStringField(TEXT("path"), Asset.GetObjectPathString());
        ContextsArray.Add(MakeShareable(new FJsonValueObject(CtxJson)));
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetArrayField(TEXT("contexts"), ContextsArray);
      ResponseJson->SetNumberField(TEXT("count"), ContextsArray.Num());
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- get_bindings ---
    if (Operation == TEXT("get_bindings")) {
      const FString AssetPath = Body->GetStringField(TEXT("asset_path"));
      if (AssetPath.IsEmpty()) {
        SendError(OnComplete, TEXT("asset_path is required"));
        return true;
      }

      UInputMappingContext* Context = LoadObject<UInputMappingContext>(nullptr, *AssetPath);
      if (!Context) {
        SendError(OnComplete, FString::Printf(TEXT("Mapping Context not found: '%s'"), *AssetPath));
        return true;
      }

      TArray<TSharedPtr<FJsonValue>> BindingsArray;
      for (const FEnhancedActionKeyMapping& Mapping : Context->GetMappings()) {
        TSharedPtr<FJsonObject> BindJson = MakeShareable(new FJsonObject());
        if (Mapping.Action) {
          BindJson->SetStringField(TEXT("action"), Mapping.Action->GetName());
          BindJson->SetStringField(TEXT("action_path"), Mapping.Action->GetPathName());
        }
        BindJson->SetStringField(TEXT("key"), Mapping.Key.GetFName().ToString());
        BindingsArray.Add(MakeShareable(new FJsonValueObject(BindJson)));
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetArrayField(TEXT("bindings"), BindingsArray);
      ResponseJson->SetNumberField(TEXT("count"), BindingsArray.Num());
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- add_action ---
    if (Operation == TEXT("add_action")) {
      const FString ActionName = Body->GetStringField(TEXT("action_name"));
      if (ActionName.IsEmpty()) {
        SendError(OnComplete, TEXT("action_name is required for add_action"));
        return true;
      }

      FString PackagePath = Body->GetStringField(TEXT("package_path"));
      if (PackagePath.IsEmpty()) {
        PackagePath = TEXT("/Game/Input");
      }

      const FString ValueTypeStr = Body->GetStringField(TEXT("value_type"));

      IAssetTools& AssetTools =
          FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
      UDataAssetFactory* Factory = NewObject<UDataAssetFactory>();

      UObject* NewAsset =
          AssetTools.CreateAsset(ActionName, PackagePath, UInputAction::StaticClass(), Factory);
      if (!NewAsset) {
        SendError(OnComplete, FString::Printf(TEXT("Failed to create InputAction '%s' in %s"),
                                              *ActionName, *PackagePath));
        return true;
      }

      UInputAction* InputAction = Cast<UInputAction>(NewAsset);
      if (InputAction && !ValueTypeStr.IsEmpty()) {
        InputAction->ValueType = ParseValueType(ValueTypeStr);
        InputAction->MarkPackageDirty();
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetStringField(TEXT("path"), NewAsset->GetPathName());
      ResponseJson->SetStringField(TEXT("name"), ActionName);
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- remove_action ---
    if (Operation == TEXT("remove_action")) {
      const FString AssetPath = Body->GetStringField(TEXT("asset_path"));
      if (AssetPath.IsEmpty()) {
        SendError(OnComplete, TEXT("asset_path is required for remove_action"));
        return true;
      }

      UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
      if (!Asset) {
        SendError(OnComplete, FString::Printf(TEXT("Asset not found: '%s'"), *AssetPath));
        return true;
      }

      TArray<UObject*> ObjectsToDelete;
      ObjectsToDelete.Add(Asset);
      int32 Deleted = ObjectTools::DeleteObjects(ObjectsToDelete, /*bShowConfirmation=*/false);

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), Deleted > 0);
      ResponseJson->SetNumberField(TEXT("deleted_count"), Deleted);
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- add_context ---
    if (Operation == TEXT("add_context")) {
      const FString ContextName = Body->GetStringField(TEXT("context_name"));
      if (ContextName.IsEmpty()) {
        SendError(OnComplete, TEXT("context_name is required for add_context"));
        return true;
      }

      FString PackagePath = Body->GetStringField(TEXT("package_path"));
      if (PackagePath.IsEmpty()) {
        PackagePath = TEXT("/Game/Input");
      }

      IAssetTools& AssetTools =
          FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
      UDataAssetFactory* Factory = NewObject<UDataAssetFactory>();

      UObject* NewAsset = AssetTools.CreateAsset(ContextName, PackagePath,
                                                 UInputMappingContext::StaticClass(), Factory);
      if (!NewAsset) {
        SendError(OnComplete,
                  FString::Printf(TEXT("Failed to create InputMappingContext '%s' in %s"),
                                  *ContextName, *PackagePath));
        return true;
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetStringField(TEXT("path"), NewAsset->GetPathName());
      ResponseJson->SetStringField(TEXT("name"), ContextName);
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- bind_action ---
    if (Operation == TEXT("bind_action")) {
      const FString AssetPath = Body->GetStringField(TEXT("asset_path"));
      const FString ActionName = Body->GetStringField(TEXT("action_name"));
      const FString KeyName = Body->GetStringField(TEXT("key"));

      if (AssetPath.IsEmpty() || ActionName.IsEmpty() || KeyName.IsEmpty()) {
        SendError(OnComplete,
                  TEXT("asset_path, action_name, and key are all required for bind_action"));
        return true;
      }

      UInputMappingContext* Context = LoadObject<UInputMappingContext>(nullptr, *AssetPath);
      if (!Context) {
        SendError(OnComplete, FString::Printf(TEXT("Mapping Context not found: '%s'"), *AssetPath));
        return true;
      }

      // Find the Input Action — try loading by path, then search by name.
      UInputAction* Action = LoadObject<UInputAction>(nullptr, *ActionName);
      if (!Action) {
        // Search asset registry by name.
        TArray<FAssetData> Assets;
        AssetRegistry.GetAssetsByClass(
            FTopLevelAssetPath(TEXT("/Script/EnhancedInput"), TEXT("InputAction")), Assets);
        for (const FAssetData& Asset : Assets) {
          if (Asset.AssetName.ToString() == ActionName) {
            Action = Cast<UInputAction>(Asset.GetAsset());
            break;
          }
        }
      }

      if (!Action) {
        SendError(OnComplete, FString::Printf(TEXT("InputAction not found: '%s'"), *ActionName));
        return true;
      }

      // Resolve the key.
      FKey Key(*KeyName);
      if (!Key.IsValid()) {
        SendError(OnComplete, FString::Printf(TEXT("Invalid key name: '%s'"), *KeyName));
        return true;
      }

      // Add the mapping.
      FEnhancedActionKeyMapping& Mapping = Context->MapKey(Action, Key);

      Context->MarkPackageDirty();

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetStringField(TEXT("action"), Action->GetName());
      ResponseJson->SetStringField(TEXT("key"), KeyName);
      ResponseJson->SetStringField(TEXT("context"), Context->GetName());
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- unbind_action ---
    if (Operation == TEXT("unbind_action")) {
      const FString AssetPath = Body->GetStringField(TEXT("asset_path"));
      const FString ActionName = Body->GetStringField(TEXT("action_name"));

      if (AssetPath.IsEmpty() || ActionName.IsEmpty()) {
        SendError(OnComplete, TEXT("asset_path and action_name are required for unbind_action"));
        return true;
      }

      UInputMappingContext* Context = LoadObject<UInputMappingContext>(nullptr, *AssetPath);
      if (!Context) {
        SendError(OnComplete, FString::Printf(TEXT("Mapping Context not found: '%s'"), *AssetPath));
        return true;
      }

      // Find the action.
      UInputAction* Action = LoadObject<UInputAction>(nullptr, *ActionName);
      if (!Action) {
        TArray<FAssetData> Assets;
        AssetRegistry.GetAssetsByClass(
            FTopLevelAssetPath(TEXT("/Script/EnhancedInput"), TEXT("InputAction")), Assets);
        for (const FAssetData& Asset : Assets) {
          if (Asset.AssetName.ToString() == ActionName) {
            Action = Cast<UInputAction>(Asset.GetAsset());
            break;
          }
        }
      }

      if (!Action) {
        SendError(OnComplete, FString::Printf(TEXT("InputAction not found: '%s'"), *ActionName));
        return true;
      }

      // Remove all mappings for this action.
      Context->UnmapAllKeysFromAction(Action);
      Context->MarkPackageDirty();

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetStringField(TEXT("action"), Action->GetName());
      ResponseJson->SetStringField(TEXT("context"), Context->GetName());
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    SendError(OnComplete, FString::Printf(TEXT("Unknown input operation: %s"), *Operation));
    return true;
  }

  // ---------------------------------------------------------------------------
  // Registration
  // ---------------------------------------------------------------------------

  void RegisterInputRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles) {
    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/input/ops")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleInputOps)));

    UE_LOG(LogMCPUnreal, Verbose, TEXT("Registered input routes (1 endpoint)"));
  }

}  // namespace MCPUnreal
