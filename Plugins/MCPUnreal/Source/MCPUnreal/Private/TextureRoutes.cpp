// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.
//
// TextureRoutes.cpp — HTTP routes for texture management: import, info,
// material texture assignment, and listing.
// See issue #44.

#include "MCPUnrealUtils.h"

#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInstanceConstant.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Factories/TextureFactory.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace MCPUnreal {

  // Map compression string to enum.
  static TextureCompressionSettings ParseCompression(const FString& Str) {
    if (Str == TEXT("TC_Normalmap")) return TC_Normalmap;
    if (Str == TEXT("TC_Masks")) return TC_Masks;
    if (Str == TEXT("TC_HDR")) return TC_HDR;
    if (Str == TEXT("TC_VectorDisplacementmap")) return TC_VectorDisplacementmap;
    return TC_Default;
  }

  static FString CompressionToString(TextureCompressionSettings S) {
    switch (S) {
      case TC_Normalmap:
        return TEXT("TC_Normalmap");
      case TC_Masks:
        return TEXT("TC_Masks");
      case TC_HDR:
        return TEXT("TC_HDR");
      case TC_VectorDisplacementmap:
        return TEXT("TC_VectorDisplacementmap");
      default:
        return TEXT("TC_Default");
    }
  }

  // Build texture info JSON from a UTexture2D.
  static TSharedPtr<FJsonObject> TextureToJson(UTexture2D* Texture) {
    TSharedPtr<FJsonObject> Info = MakeShareable(new FJsonObject());
    Info->SetStringField(TEXT("asset"), Texture->GetPathName());
    Info->SetStringField(TEXT("name"), Texture->GetName());
    Info->SetNumberField(TEXT("width"), Texture->GetSizeX());
    Info->SetNumberField(TEXT("height"), Texture->GetSizeY());
    Info->SetStringField(TEXT("format"), GetPixelFormatString(Texture->GetPixelFormat()));
    Info->SetNumberField(TEXT("mip_count"), Texture->GetNumMips());
    Info->SetStringField(TEXT("compression"), CompressionToString(Texture->CompressionSettings));
    Info->SetNumberField(
        TEXT("size_kb"),
        static_cast<int32>(Texture->GetResourceSizeBytes(EResourceSizeMode::EstimatedTotal) /
                           1024));
    return Info;
  }

  // ---------------------------------------------------------------------------
  // POST /api/textures/ops
  // ---------------------------------------------------------------------------

  static bool HandleTextureOps(const FHttpServerRequest& Request,
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

    // --- import ---
    if (Operation == TEXT("import")) {
      const FString SourcePath = Body->GetStringField(TEXT("source_path"));
      const FString Destination = Body->GetStringField(TEXT("destination"));
      const FString CompressionStr = Body->GetStringField(TEXT("compression"));

      if (SourcePath.IsEmpty()) {
        SendError(OnComplete, TEXT("source_path is required for import"));
        return true;
      }
      if (Destination.IsEmpty()) {
        SendError(OnComplete, TEXT("destination is required for import"));
        return true;
      }

      // Read the source file.
      TArray<uint8> FileData;
      if (!FFileHelper::LoadFileToArray(FileData, *SourcePath)) {
        SendError(OnComplete, FString::Printf(TEXT("Failed to read source file: %s"), *SourcePath));
        return true;
      }

      // Import using TextureFactory.
      UTextureFactory* Factory = NewObject<UTextureFactory>();
      Factory->AddToRoot();  // prevent GC during import

      const FString PackagePath = FPaths::GetPath(Destination);
      const FString AssetName = FPaths::GetBaseFilename(Destination);
      UPackage* Package = CreatePackage(*Destination);

      const uint8* DataPtr = FileData.GetData();
      UTexture2D* ImportedTexture = Cast<UTexture2D>(Factory->FactoryCreateBinary(
          UTexture2D::StaticClass(), Package, *AssetName, RF_Public | RF_Standalone, nullptr,
          *FPaths::GetExtension(SourcePath), DataPtr, DataPtr + FileData.Num(), GWarn));

      Factory->RemoveFromRoot();

      if (!ImportedTexture) {
        SendError(OnComplete, TEXT("Failed to import texture"), 500);
        return true;
      }

      // Apply compression settings.
      if (!CompressionStr.IsEmpty()) {
        ImportedTexture->CompressionSettings = ParseCompression(CompressionStr);
        ImportedTexture->UpdateResource();
      }

      // Save the package.
      Package->MarkPackageDirty();

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetStringField(TEXT("asset"), ImportedTexture->GetPathName());
      ResponseJson->SetStringField(TEXT("message"), TEXT("Imported texture"));

      UE_LOG(LogMCPUnreal, Log, TEXT("Imported texture '%s' from '%s'"),
             *ImportedTexture->GetPathName(), *SourcePath);
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- get_info ---
    if (Operation == TEXT("get_info")) {
      const FString AssetPath = Body->GetStringField(TEXT("asset"));
      if (AssetPath.IsEmpty()) {
        SendError(OnComplete, TEXT("asset is required for get_info"));
        return true;
      }

      UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *AssetPath);
      if (!Texture) {
        SendError(OnComplete, FString::Printf(TEXT("Texture not found: %s"), *AssetPath));
        return true;
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetObjectField(TEXT("info"), TextureToJson(Texture));

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- set_material_texture ---
    if (Operation == TEXT("set_material_texture")) {
      const FString MaterialPath = Body->GetStringField(TEXT("material_instance"));
      const FString ParamName = Body->GetStringField(TEXT("param_name"));
      const FString TexturePath = Body->GetStringField(TEXT("texture"));

      if (MaterialPath.IsEmpty() || ParamName.IsEmpty() || TexturePath.IsEmpty()) {
        SendError(OnComplete, TEXT("material_instance, param_name, and texture are all required "
                                   "for set_material_texture"));
        return true;
      }

      UTexture* Texture = LoadObject<UTexture>(nullptr, *TexturePath);
      if (!Texture) {
        SendError(OnComplete, FString::Printf(TEXT("Texture not found: %s"), *TexturePath));
        return true;
      }

      // Try as MaterialInstanceConstant (editor asset).
      UMaterialInstanceConstant* MIC =
          LoadObject<UMaterialInstanceConstant>(nullptr, *MaterialPath);
      if (MIC) {
        MIC->SetTextureParameterValueEditorOnly(FName(*ParamName), Texture);
        MIC->MarkPackageDirty();

        TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
        ResponseJson->SetBoolField(TEXT("success"), true);
        ResponseJson->SetStringField(TEXT("message"), TEXT("Texture parameter set"));
        SendJson(OnComplete, ResponseJson);
        return true;
      }

      SendError(OnComplete,
                FString::Printf(TEXT("Material instance not found: %s"), *MaterialPath));
      return true;
    }

    // --- list ---
    if (Operation == TEXT("list")) {
      const FString Path = Body->GetStringField(TEXT("path"));
      if (Path.IsEmpty()) {
        SendError(OnComplete, TEXT("path is required for list"));
        return true;
      }

      FAssetRegistryModule& ARM =
          FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
      TArray<FAssetData> Assets;
      ARM.Get().GetAssetsByPath(FName(*Path), Assets, /*bRecursive=*/true);

      TArray<TSharedPtr<FJsonValue>> TexturesArray;
      for (const FAssetData& AssetData : Assets) {
        if (AssetData.AssetClassPath.GetAssetName() != TEXT("Texture2D")) {
          continue;
        }
        UTexture2D* Tex = Cast<UTexture2D>(AssetData.GetAsset());
        if (Tex) {
          TexturesArray.Add(MakeShareable(new FJsonValueObject(TextureToJson(Tex))));
        }
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetNumberField(TEXT("count"), TexturesArray.Num());
      ResponseJson->SetArrayField(TEXT("textures"), TexturesArray);

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    SendError(OnComplete, FString::Printf(TEXT("Unknown texture operation: %s"), *Operation));
    return true;
  }

  // ---------------------------------------------------------------------------
  // Registration
  // ---------------------------------------------------------------------------

  void RegisterTextureRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles) {
    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/textures/ops")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleTextureOps)));

    UE_LOG(LogMCPUnreal, Verbose, TEXT("Registered texture routes (1 endpoint)"));
  }

}  // namespace MCPUnreal
