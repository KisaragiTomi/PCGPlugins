// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.
//
// AssetRoutes.cpp — HTTP routes for asset info, dependencies, and referencers.
//
// See IMPLEMENTATION.md §3.6 and §5.1.

#include "MCPUnrealUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "UObject/UObjectIterator.h"

namespace MCPUnreal {

  // ---------------------------------------------------------------------------
  // POST /api/assets/info
  // ---------------------------------------------------------------------------

  static bool HandleAssetInfo(const FHttpServerRequest& Request,
                              const FHttpResultCallback& OnComplete) {
    TSharedPtr<FJsonObject> Body;
    if (!ParseJsonBody(Request, Body)) {
      SendError(OnComplete, TEXT("Invalid JSON in request body"));
      return true;
    }

    const FString AssetPath = Body->GetStringField(TEXT("asset_path"));
    if (AssetPath.IsEmpty()) {
      SendError(OnComplete, TEXT("asset_path is required"));
      return true;
    }

    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
    if (!AssetData.IsValid()) {
      SendError(OnComplete, FString::Printf(TEXT("Asset not found at path '%s'"), *AssetPath));
      return true;
    }

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    ResponseJson->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
    ResponseJson->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
    ResponseJson->SetStringField(TEXT("class"), AssetData.AssetClassPath.ToString());
    ResponseJson->SetStringField(TEXT("package"), AssetData.PackageName.ToString());

    // Disk size.
    ResponseJson->SetNumberField(TEXT("package_flags"),
                                 static_cast<double>(AssetData.PackageFlags));

    // Tags — include all asset registry tags as metadata.
    TSharedPtr<FJsonObject> TagsJson = MakeShareable(new FJsonObject());
    for (const auto& Pair : AssetData.TagsAndValues) {
      TagsJson->SetStringField(Pair.Key.ToString(), Pair.Value.AsString());
    }
    ResponseJson->SetObjectField(TEXT("tags"), TagsJson);

    SendJson(OnComplete, ResponseJson);
    return true;
  }

  // ---------------------------------------------------------------------------
  // POST /api/assets/dependencies
  // ---------------------------------------------------------------------------

  static bool HandleAssetDependencies(const FHttpServerRequest& Request,
                                      const FHttpResultCallback& OnComplete) {
    TSharedPtr<FJsonObject> Body;
    if (!ParseJsonBody(Request, Body)) {
      SendError(OnComplete, TEXT("Invalid JSON in request body"));
      return true;
    }

    const FString AssetPath = Body->GetStringField(TEXT("asset_path"));
    if (AssetPath.IsEmpty()) {
      SendError(OnComplete, TEXT("asset_path is required"));
      return true;
    }

    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    // Extract package name from asset path.
    FString PackageName;
    FString AssetName;
    AssetPath.Split(TEXT("."), &PackageName, &AssetName);
    if (PackageName.IsEmpty()) {
      PackageName = AssetPath;
    }

    TArray<FName> Dependencies;
    AssetRegistry.GetDependencies(FName(*PackageName), Dependencies);

    TArray<TSharedPtr<FJsonValue>> DepsArray;
    for (const FName& Dep : Dependencies) {
      DepsArray.Add(MakeShareable(new FJsonValueString(Dep.ToString())));
    }

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    ResponseJson->SetStringField(TEXT("asset_path"), AssetPath);
    ResponseJson->SetArrayField(TEXT("dependencies"), DepsArray);
    ResponseJson->SetNumberField(TEXT("count"), DepsArray.Num());
    SendJson(OnComplete, ResponseJson);
    return true;
  }

  // ---------------------------------------------------------------------------
  // POST /api/assets/referencers
  // ---------------------------------------------------------------------------

  static bool HandleAssetReferencers(const FHttpServerRequest& Request,
                                     const FHttpResultCallback& OnComplete) {
    TSharedPtr<FJsonObject> Body;
    if (!ParseJsonBody(Request, Body)) {
      SendError(OnComplete, TEXT("Invalid JSON in request body"));
      return true;
    }

    const FString AssetPath = Body->GetStringField(TEXT("asset_path"));
    if (AssetPath.IsEmpty()) {
      SendError(OnComplete, TEXT("asset_path is required"));
      return true;
    }

    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    FString PackageName;
    FString AssetName;
    AssetPath.Split(TEXT("."), &PackageName, &AssetName);
    if (PackageName.IsEmpty()) {
      PackageName = AssetPath;
    }

    TArray<FName> Referencers;
    AssetRegistry.GetReferencers(FName(*PackageName), Referencers);

    TArray<TSharedPtr<FJsonValue>> RefsArray;
    for (const FName& Ref : Referencers) {
      RefsArray.Add(MakeShareable(new FJsonValueString(Ref.ToString())));
    }

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    ResponseJson->SetStringField(TEXT("asset_path"), AssetPath);
    ResponseJson->SetArrayField(TEXT("referencers"), RefsArray);
    ResponseJson->SetNumberField(TEXT("count"), RefsArray.Num());
    SendJson(OnComplete, ResponseJson);
    return true;
  }

  // ---------------------------------------------------------------------------
  // POST /api/assets/search
  // ---------------------------------------------------------------------------

  static bool HandleAssetSearch(const FHttpServerRequest& Request,
                                const FHttpResultCallback& OnComplete) {
    TSharedPtr<FJsonObject> Body;
    if (!ParseJsonBody(Request, Body)) {
      SendError(OnComplete, TEXT("Invalid JSON in request body"));
      return true;
    }

    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    const FString ClassFilter = Body->GetStringField(TEXT("class_filter"));
    const FString PathFilter = Body->GetStringField(TEXT("path_filter"));
    const FString NameFilter = Body->GetStringField(TEXT("name_filter"));
    bool bRecursivePath = true;
    if (Body->HasField(TEXT("recursive_path"))) {
      bRecursivePath = Body->GetBoolField(TEXT("recursive_path"));
    }

    FARFilter Filter;
    Filter.bRecursivePaths = bRecursivePath;
    Filter.bRecursiveClasses = true;

    // Class filter — resolve class name to FTopLevelAssetPath.
    if (!ClassFilter.IsEmpty()) {
      // Try as a full path first (e.g. "/Script/Engine.StaticMesh").
      FTopLevelAssetPath ClassPath(ClassFilter);
      if (!ClassPath.IsValid()) {
        // Short name — search known engine/script packages.
        ClassPath = FTopLevelAssetPath(TEXT("/Script/Engine"), *ClassFilter);
      }
      Filter.ClassPaths.Add(ClassPath);
    }

    // Path filter — restrict to a specific content path.
    if (!PathFilter.IsEmpty()) {
      Filter.PackagePaths.Add(FName(*PathFilter));
    }

    TArray<FAssetData> Assets;
    AssetRegistry.GetAssets(Filter, Assets);

    // Apply name filter (substring match) client-side since FARFilter
    // does not support partial name matching.
    TArray<TSharedPtr<FJsonValue>> ResultArray;
    for (const FAssetData& Asset : Assets) {
      if (!NameFilter.IsEmpty()) {
        if (!Asset.AssetName.ToString().Contains(NameFilter)) {
          continue;
        }
      }

      TSharedPtr<FJsonObject> AssetJson = MakeShareable(new FJsonObject());
      AssetJson->SetStringField(TEXT("name"), Asset.AssetName.ToString());
      AssetJson->SetStringField(TEXT("path"), Asset.GetObjectPathString());
      AssetJson->SetStringField(TEXT("class"), Asset.AssetClassPath.ToString());
      AssetJson->SetStringField(TEXT("package"), Asset.PackageName.ToString());
      ResultArray.Add(MakeShareable(new FJsonValueObject(AssetJson)));
    }

    SendJsonArray(OnComplete, ResultArray);
    return true;
  }

  // ---------------------------------------------------------------------------
  // Registration
  // ---------------------------------------------------------------------------

  void RegisterAssetRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles) {
    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/assets/info")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleAssetInfo)));

    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/assets/dependencies")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleAssetDependencies)));

    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/assets/referencers")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleAssetReferencers)));

    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/assets/search")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleAssetSearch)));

    UE_LOG(LogMCPUnreal, Verbose, TEXT("Registered asset routes (4 endpoints)"));
  }

}  // namespace MCPUnreal
