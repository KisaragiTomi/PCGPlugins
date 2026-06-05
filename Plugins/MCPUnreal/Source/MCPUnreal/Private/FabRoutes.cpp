// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.
//
// FabRoutes.cpp — HTTP routes for Fab marketplace asset cache management
// and import. Only operates on already-downloaded/cached assets.
// See issue #42.

#include "MCPUnrealUtils.h"

#if WITH_FAB
#include "Utilities/FabAssetsCache.h"
#include "Utilities/AssetUtils.h"
#endif

namespace MCPUnreal {

  // ---------------------------------------------------------------------------
  // POST /api/fab/ops
  // ---------------------------------------------------------------------------

  static bool HandleFabOps(const FHttpServerRequest& Request,
                           const FHttpResultCallback& OnComplete) {
#if !WITH_FAB
    SendError(OnComplete, TEXT("Fab plugin is not available in this build"), 501);
    return true;
#else
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

    // --- list_cache ---
    if (Operation == TEXT("list_cache")) {
      TArray<FString> CachedAssets = FFabAssetsCache::GetCachedAssets();

      TArray<TSharedPtr<FJsonValue> > AssetsArray;
      for (const FString& AssetId : CachedAssets) {
        TSharedPtr<FJsonObject> AssetJson = MakeShareable(new FJsonObject());
        AssetJson->SetStringField(TEXT("asset_id"), AssetId);

        FString CachedFile = FFabAssetsCache::GetCachedFile(AssetId);
        if (!CachedFile.IsEmpty()) {
          AssetJson->SetStringField(TEXT("file_path"), CachedFile);
        }

        AssetsArray.Add(MakeShareable(new FJsonValueObject(AssetJson)));
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetNumberField(TEXT("asset_count"), CachedAssets.Num());
      ResponseJson->SetArrayField(TEXT("assets"), AssetsArray);

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- cache_info ---
    if (Operation == TEXT("cache_info")) {
      TArray<FString> CachedAssets = FFabAssetsCache::GetCachedAssets();

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetStringField(TEXT("cache_location"), FFabAssetsCache::GetCacheLocation());
      ResponseJson->SetStringField(TEXT("cache_size"),
                                   FFabAssetsCache::GetCacheSizeString().ToString());
      ResponseJson->SetNumberField(TEXT("cache_size_bytes"),
                                   static_cast<double>(FFabAssetsCache::GetCacheSize()));
      ResponseJson->SetNumberField(TEXT("asset_count"), CachedAssets.Num());

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- import ---
    if (Operation == TEXT("import")) {
      const FString AssetId = Body->GetStringField(TEXT("asset_id"));
      const FString Destination = Body->GetStringField(TEXT("destination"));

      if (AssetId.IsEmpty()) {
        SendError(OnComplete, TEXT("asset_id is required for import"));
        return true;
      }
      if (Destination.IsEmpty()) {
        SendError(OnComplete, TEXT("destination is required for import (e.g. /Game/Assets/)"));
        return true;
      }

      // Get the cached file path.
      FString CachedFile = FFabAssetsCache::GetCachedFile(AssetId);
      if (CachedFile.IsEmpty()) {
        SendError(OnComplete, FString::Printf(TEXT("Asset '%s' not found in Fab cache"), *AssetId));
        return true;
      }

      // Determine extraction path.
      FString ExtractPath = FPaths::GetPath(CachedFile) / AssetId;

      // Unzip if needed (cached file is typically a .zip).
      if (CachedFile.EndsWith(TEXT(".zip"))) {
        if (!FAssetUtils::Unzip(CachedFile, ExtractPath)) {
          SendError(OnComplete,
                    FString::Printf(TEXT("Failed to extract cached asset '%s'"), *AssetId), 500);
          return true;
        }
      } else {
        // Single file — just use the cached path directly.
        ExtractPath = FPaths::GetPath(CachedFile);
      }

      // Scan for assets and import them.
      FAssetUtils::ScanForAssets(Destination);

      // Sync content browser to the destination folder.
      FAssetUtils::SyncContentBrowserToFolder(Destination, /*bFocusContentBrowser=*/true);

      UE_LOG(LogMCPUnreal, Log, TEXT("Imported Fab asset '%s' to '%s' from cache '%s'"), *AssetId,
             *Destination, *CachedFile);

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetStringField(TEXT("imported_path"), Destination);
      ResponseJson->SetStringField(
          TEXT("message"),
          FString::Printf(TEXT("Imported asset '%s' to '%s'"), *AssetId, *Destination));

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- clear_cache ---
    if (Operation == TEXT("clear_cache")) {
      FFabAssetsCache::ClearCache();

      UE_LOG(LogMCPUnreal, Log, TEXT("Fab cache cleared"));

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetStringField(TEXT("message"), TEXT("Cache cleared"));

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    SendError(OnComplete, FString::Printf(TEXT("Unknown Fab operation: %s"), *Operation));
    return true;
#endif  // WITH_FAB
  }

  // ---------------------------------------------------------------------------
  // Registration
  // ---------------------------------------------------------------------------

  void RegisterFabRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles) {
    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/fab/ops")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleFabOps)));

    UE_LOG(LogMCPUnreal, Verbose, TEXT("Registered Fab routes (1 endpoint)"));
  }

}  // namespace MCPUnreal
