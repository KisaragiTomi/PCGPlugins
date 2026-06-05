// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.
//
// MCPUnrealUtils.h — Shared utilities for JSON parsing, HTTP responses,
// and editor world access across all route handlers.

#pragma once

#include "CoreMinimal.h"
#include "MCPUnrealModule.h"  // Picks up DECLARE_LOG_CATEGORY_EXTERN(LogMCPUnreal)
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "HttpResultCallback.h"
#include "IHttpRouter.h"
#include "HttpRouteHandle.h"
#include "HttpPath.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/GameViewportClient.h"
#include "Slate/SceneViewport.h"

namespace MCPUnreal {

  // ---------------------------------------------------------------------------
  // JSON helpers
  // ---------------------------------------------------------------------------

  /** Parse the HTTP request body as JSON. Returns false on failure. */
  inline bool ParseJsonBody(const FHttpServerRequest& Request, TSharedPtr<FJsonObject>& OutJson) {
    if (Request.Body.Num() == 0) {
      // Empty body is valid for some endpoints — return empty object.
      OutJson = MakeShareable(new FJsonObject());
      return true;
    }

    // Null-terminate the body bytes for safe UTF-8 → TCHAR conversion.
    TArray<uint8> NullTermBody(Request.Body);
    NullTermBody.Add(0);
    const FString BodyStr =
        FString(UTF8_TO_TCHAR(reinterpret_cast<const ANSICHAR*>(NullTermBody.GetData())));
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BodyStr);
    return FJsonSerializer::Deserialize(Reader, OutJson) && OutJson.IsValid();
  }

  /** Serialize a JSON object to an FString. */
  inline FString JsonToString(const TSharedPtr<FJsonObject>& Json) {
    FString Result;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Result);
    FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);
    return Result;
  }

  /** Serialize a JSON value array to an FString. */
  inline FString JsonArrayToString(const TArray<TSharedPtr<FJsonValue>>& Array) {
    FString Result;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Result);
    Writer->WriteArrayStart();
    for (const auto& Value : Array) {
      FJsonSerializer::Serialize(Value, FString(), Writer);
    }
    Writer->WriteArrayEnd();
    Writer->Close();
    return Result;
  }

  // ---------------------------------------------------------------------------
  // Response helpers
  // ---------------------------------------------------------------------------

  /** Send a JSON object response. */
  inline void SendJson(const FHttpResultCallback& OnComplete, const TSharedPtr<FJsonObject>& Json) {
    auto Response = FHttpServerResponse::Create(JsonToString(Json), TEXT("application/json"));
    OnComplete(MoveTemp(Response));
  }

  /** Send a raw JSON string response. */
  inline void SendJsonString(const FHttpResultCallback& OnComplete, const FString& JsonStr) {
    auto Response = FHttpServerResponse::Create(JsonStr, TEXT("application/json"));
    OnComplete(MoveTemp(Response));
  }

  /** Send a JSON array response. */
  inline void SendJsonArray(const FHttpResultCallback& OnComplete,
                            const TArray<TSharedPtr<FJsonValue>>& Array) {
    auto Response = FHttpServerResponse::Create(JsonArrayToString(Array), TEXT("application/json"));
    OnComplete(MoveTemp(Response));
  }

  /** Send an error response with a message. */
  inline void SendError(const FHttpResultCallback& OnComplete, const FString& Message,
                        int32 StatusCode = 400) {
    TSharedPtr<FJsonObject> ErrorJson = MakeShareable(new FJsonObject());
    ErrorJson->SetStringField(TEXT("error"), Message);
    auto Response = FHttpServerResponse::Create(JsonToString(ErrorJson), TEXT("application/json"));
    OnComplete(MoveTemp(Response));
  }

  // ---------------------------------------------------------------------------
  // Editor world access
  // ---------------------------------------------------------------------------

  /** Get the current editor world. Returns nullptr if editor is not available. */
  inline UWorld* GetEditorWorld() {
    if (GEditor) {
      return GEditor->GetEditorWorldContext().World();
    }
    return nullptr;
  }

  /** Check if Play In Editor is currently active. */
  inline bool IsPIEActive() { return GEditor && GEditor->IsPlayingSessionInEditor(); }

  /**
   * Get the appropriate world based on the "world" JSON field in the request body.
   * Values: "auto" (default -- PIE if active, else editor), "pie" (error if not running),
   * "editor" (always editor). Missing or empty is treated as "auto".
   */
  inline UWorld* GetWorld(const TSharedPtr<FJsonObject>& Body) {
    FString WorldParam = Body->GetStringField(TEXT("world"));

    if (WorldParam == TEXT("pie")) {
      if (!IsPIEActive() || !GEditor->PlayWorld) {
        return nullptr;  // Caller should return an error.
      }
      return GEditor->PlayWorld;
    }

    if (WorldParam == TEXT("editor")) {
      return GetEditorWorld();
    }

    // "auto" or empty: prefer PIE world if active, else editor.
    if (IsPIEActive() && GEditor->PlayWorld) {
      return GEditor->PlayWorld;
    }
    return GetEditorWorld();
  }

  /**
   * Get the appropriate viewport based on the "world" JSON field.
   * For PIE (or auto when PIE is active): returns the game viewport.
   * For editor: returns GEditor->GetActiveViewport().
   */
  inline FViewport* GetViewport(const TSharedPtr<FJsonObject>& Body) {
    FString WorldParam = Body->GetStringField(TEXT("world"));

    bool bUsePIE = false;
    if (WorldParam == TEXT("pie")) {
      bUsePIE = true;
    } else if (WorldParam == TEXT("editor")) {
      bUsePIE = false;
    } else {
      // "auto": prefer PIE if active.
      bUsePIE = IsPIEActive();
    }

    if (bUsePIE && GEngine && GEngine->GameViewport) {
      return GEngine->GameViewport->GetGameViewport();
    }
    // If PIE was explicitly requested but unavailable, return nullptr
    // so the caller can report an error instead of silently capturing
    // the editor viewport.
    if (WorldParam == TEXT("pie")) {
      return nullptr;
    }
    return GEditor ? GEditor->GetActiveViewport() : nullptr;
  }

  // ---------------------------------------------------------------------------
  // Route registration function declarations
  // ---------------------------------------------------------------------------

  /** Register actor management routes (list, spawn, delete). Issue #18. */
  void RegisterActorRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles);

  /** Register Blueprint editing routes (query + modify). Issue #19. */
  void RegisterBlueprintRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles);

  /** Register Animation Blueprint routes (query + modify). Issue #20. */
  void RegisterAnimBPRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles);

  /** Register editor utility routes (output log, viewport, script, console). Issue #21. */
  void RegisterEditorRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles);

  /** Register asset info and dependency routes. Issue #22. */
  void RegisterAssetRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles);

  /** Register material creation and parameter routes. Issue #26. */
  void RegisterMaterialRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles);

  /** Register character configuration routes. Issue #27. */
  void RegisterCharacterRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles);

  /** Register Enhanced Input system routes. Issue #28. */
  void RegisterInputRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles);

  /** Register level management routes. Issue #29. */
  void RegisterLevelRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles);

  /** Register procedural mesh and RealtimeMesh routes. Issue #32. */
  void RegisterMeshRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles);

  /** Register Procedural Content Generation (PCG) routes. */
  void RegisterPCGRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles);

  /** Register Gameplay Ability System (GAS) routes. */
  void RegisterGASRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles);

  /** Register Niagara VFX system routes. */
  void RegisterNiagaraRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles);

  /** Register actor component introspection routes. Issue #40. */
  void RegisterComponentRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles);

  /** Register InstancedStaticMesh (ISM/HISM) management routes. Issue #41. */
  void RegisterISMRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles);

  /** Register Fab marketplace cache and import routes. Issue #42. */
  void RegisterFabRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles);

  /** Register texture management routes. Issue #44. */
  void RegisterTextureRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles);

  /** Register subsystem introspection routes. Issue #45. */
  void RegisterSubsystemRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles);

  /** Register DataTable/DataAsset management routes. Issue #46. */
  void RegisterDataAssetRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles);

  /** Register Slate/UMG widget introspection routes. Issue #47. */
  void RegisterUIQueryRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles);

  /** Register network debug (HTTP/WebSocket) introspection routes. Issue #48. */
  void RegisterNetworkDebugRoutes(TSharedPtr<IHttpRouter> Router,
                                  TArray<FHttpRouteHandle>& Handles);

}  // namespace MCPUnreal
