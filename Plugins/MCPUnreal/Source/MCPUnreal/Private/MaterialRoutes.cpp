// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.
//
// MaterialRoutes.cpp — HTTP routes for material creation and parameter editing.
//
// See IMPLEMENTATION.md §3.8 and §5.1.

#include "MCPUnrealUtils.h"

#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"

namespace MCPUnreal {

  // ---------------------------------------------------------------------------
  // POST /api/materials/ops
  // ---------------------------------------------------------------------------

  static bool HandleMaterialOps(const FHttpServerRequest& Request,
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

    // --- create ---
    if (Operation == TEXT("create")) {
      const FString PackagePath = Body->GetStringField(TEXT("package_path"));
      const FString MaterialName = Body->GetStringField(TEXT("material_name"));
      if (PackagePath.IsEmpty() || MaterialName.IsEmpty()) {
        SendError(OnComplete, TEXT("package_path and material_name are required"));
        return true;
      }

      IAssetTools& AssetTools =
          FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
      UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
      UObject* NewAsset =
          AssetTools.CreateAsset(MaterialName, PackagePath, UMaterial::StaticClass(), Factory);

      if (!NewAsset) {
        SendError(OnComplete, TEXT("Failed to create material"), 500);
        return true;
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetStringField(TEXT("path"), NewAsset->GetPathName());
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- create_instance ---
    if (Operation == TEXT("create_instance")) {
      const FString ParentPath = Body->GetStringField(TEXT("parent_path"));
      const FString PackagePath = Body->GetStringField(TEXT("package_path"));
      const FString MaterialName = Body->GetStringField(TEXT("material_name"));
      if (ParentPath.IsEmpty() || PackagePath.IsEmpty() || MaterialName.IsEmpty()) {
        SendError(OnComplete, TEXT("parent_path, package_path, and material_name are required"));
        return true;
      }

      UMaterial* Parent = LoadObject<UMaterial>(nullptr, *ParentPath);
      if (!Parent) {
        SendError(OnComplete,
                  FString::Printf(TEXT("Parent material not found: '%s'"), *ParentPath));
        return true;
      }

      IAssetTools& AssetTools =
          FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
      UMaterialInstanceConstantFactoryNew* Factory =
          NewObject<UMaterialInstanceConstantFactoryNew>();
      Factory->InitialParent = Parent;
      UObject* NewAsset = AssetTools.CreateAsset(MaterialName, PackagePath,
                                                 UMaterialInstanceConstant::StaticClass(), Factory);

      if (!NewAsset) {
        SendError(OnComplete, TEXT("Failed to create material instance"), 500);
        return true;
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetStringField(TEXT("path"), NewAsset->GetPathName());
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- get_parameters / list_parameters ---
    if (Operation == TEXT("get_parameters") || Operation == TEXT("list_parameters")) {
      const FString MaterialPath = Body->GetStringField(TEXT("material_path"));
      if (MaterialPath.IsEmpty()) {
        SendError(OnComplete, TEXT("material_path is required"));
        return true;
      }

      UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
      if (!Mat) {
        SendError(OnComplete, FString::Printf(TEXT("Material not found: '%s'"), *MaterialPath));
        return true;
      }

      TArray<TSharedPtr<FJsonValue>> ParamsArray;

      // Scalar parameters.
      TArray<FMaterialParameterInfo> ScalarParams;
      TArray<FGuid> ScalarGuids;
      Mat->GetAllScalarParameterInfo(ScalarParams, ScalarGuids);
      for (const auto& Info : ScalarParams) {
        TSharedPtr<FJsonObject> ParamJson = MakeShareable(new FJsonObject());
        ParamJson->SetStringField(TEXT("name"), Info.Name.ToString());
        ParamJson->SetStringField(TEXT("type"), TEXT("scalar"));
        if (Operation == TEXT("get_parameters")) {
          float Value;
          if (Mat->GetScalarParameterValue(Info, Value)) {
            ParamJson->SetNumberField(TEXT("value"), Value);
          }
        }
        ParamsArray.Add(MakeShareable(new FJsonValueObject(ParamJson)));
      }

      // Vector parameters.
      TArray<FMaterialParameterInfo> VectorParams;
      TArray<FGuid> VectorGuids;
      Mat->GetAllVectorParameterInfo(VectorParams, VectorGuids);
      for (const auto& Info : VectorParams) {
        TSharedPtr<FJsonObject> ParamJson = MakeShareable(new FJsonObject());
        ParamJson->SetStringField(TEXT("name"), Info.Name.ToString());
        ParamJson->SetStringField(TEXT("type"), TEXT("vector"));
        if (Operation == TEXT("get_parameters")) {
          FLinearColor Value;
          if (Mat->GetVectorParameterValue(Info, Value)) {
            TArray<TSharedPtr<FJsonValue>> ColorArray;
            ColorArray.Add(MakeShareable(new FJsonValueNumber(Value.R)));
            ColorArray.Add(MakeShareable(new FJsonValueNumber(Value.G)));
            ColorArray.Add(MakeShareable(new FJsonValueNumber(Value.B)));
            ColorArray.Add(MakeShareable(new FJsonValueNumber(Value.A)));
            ParamJson->SetArrayField(TEXT("value"), ColorArray);
          }
        }
        ParamsArray.Add(MakeShareable(new FJsonValueObject(ParamJson)));
      }

      // Texture parameters.
      TArray<FMaterialParameterInfo> TextureParams;
      TArray<FGuid> TextureGuids;
      Mat->GetAllTextureParameterInfo(TextureParams, TextureGuids);
      for (const auto& Info : TextureParams) {
        TSharedPtr<FJsonObject> ParamJson = MakeShareable(new FJsonObject());
        ParamJson->SetStringField(TEXT("name"), Info.Name.ToString());
        ParamJson->SetStringField(TEXT("type"), TEXT("texture"));
        if (Operation == TEXT("get_parameters")) {
          UTexture* Texture = nullptr;
          if (Mat->GetTextureParameterValue(Info, Texture) && Texture) {
            ParamJson->SetStringField(TEXT("value"), Texture->GetPathName());
          }
        }
        ParamsArray.Add(MakeShareable(new FJsonValueObject(ParamJson)));
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetArrayField(TEXT("parameters"), ParamsArray);
      ResponseJson->SetNumberField(TEXT("count"), ParamsArray.Num());
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- set_parameter ---
    if (Operation == TEXT("set_parameter")) {
      const FString MaterialPath = Body->GetStringField(TEXT("material_path"));
      const FString ParamName = Body->GetStringField(TEXT("parameter_name"));
      if (MaterialPath.IsEmpty() || ParamName.IsEmpty()) {
        SendError(OnComplete, TEXT("material_path and parameter_name are required"));
        return true;
      }

      UMaterialInstanceConstant* MIC =
          LoadObject<UMaterialInstanceConstant>(nullptr, *MaterialPath);
      if (!MIC) {
        SendError(OnComplete,
                  TEXT("Material instance not found (set_parameter works on material instances)"));
        return true;
      }

      // Try scalar parameter.
      if (Body->HasField(TEXT("parameter_value"))) {
        float Value = static_cast<float>(Body->GetNumberField(TEXT("parameter_value")));
        MIC->SetScalarParameterValueEditorOnly(FName(*ParamName), Value);
      }

      // Try color/vector parameter.
      const TArray<TSharedPtr<FJsonValue>>* ColorArray;
      if (Body->TryGetArrayField(TEXT("color"), ColorArray) && ColorArray->Num() >= 4) {
        FLinearColor Color((*ColorArray)[0]->AsNumber(), (*ColorArray)[1]->AsNumber(),
                           (*ColorArray)[2]->AsNumber(), (*ColorArray)[3]->AsNumber());
        MIC->SetVectorParameterValueEditorOnly(FName(*ParamName), Color);
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- set_texture ---
    if (Operation == TEXT("set_texture")) {
      const FString MaterialPath = Body->GetStringField(TEXT("material_path"));
      const FString ParamName = Body->GetStringField(TEXT("parameter_name"));
      const FString TexturePath = Body->GetStringField(TEXT("texture_path"));
      if (MaterialPath.IsEmpty() || ParamName.IsEmpty() || TexturePath.IsEmpty()) {
        SendError(OnComplete, TEXT("material_path, parameter_name, and texture_path are required"));
        return true;
      }

      UMaterialInstanceConstant* MIC =
          LoadObject<UMaterialInstanceConstant>(nullptr, *MaterialPath);
      if (!MIC) {
        SendError(OnComplete, TEXT("Material instance not found"));
        return true;
      }

      UTexture* Texture = LoadObject<UTexture>(nullptr, *TexturePath);
      if (!Texture) {
        SendError(OnComplete, FString::Printf(TEXT("Texture not found: '%s'"), *TexturePath));
        return true;
      }

      MIC->SetTextureParameterValueEditorOnly(FName(*ParamName), Texture);

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    SendError(OnComplete, FString::Printf(TEXT("Unknown material operation: '%s'"), *Operation));
    return true;
  }

  // ---------------------------------------------------------------------------
  // Registration
  // ---------------------------------------------------------------------------

  void RegisterMaterialRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles) {
    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/materials/ops")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleMaterialOps)));

    UE_LOG(LogMCPUnreal, Verbose, TEXT("Registered material routes (1 endpoint)"));
  }

}  // namespace MCPUnreal
