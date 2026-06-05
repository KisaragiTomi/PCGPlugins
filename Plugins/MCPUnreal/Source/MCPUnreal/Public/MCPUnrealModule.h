// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.
//
// MCPUnrealModule.h — Editor plugin module that runs a local HTTP server
// for the mcp-unreal Go binary to communicate with the UE editor.
//
// See IMPLEMENTATION.md §5 for the plugin architecture.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "HttpRouteHandle.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMCPUnreal, Log, All);

/**
 * MCPUnreal editor plugin module.
 *
 * Starts an HTTP server on localhost:8090 (configurable via mcp.Port console
 * variable) that exposes editor internals to the mcp-unreal Go MCP server.
 *
 * The server binds to 127.0.0.1 only. All route handlers validate input
 * JSON before acting on it. See CLAUDE.md Security §3 and §4.
 */
class FMCPUnrealModule : public IModuleInterface {
 public:
  /** IModuleInterface */
  virtual void StartupModule() override;
  virtual void ShutdownModule() override;
  virtual bool IsGameModule() const override { return false; }

  /** Plugin version reported by the /api/status endpoint. */
  static constexpr const TCHAR* PluginVersion = TEXT("0.2.0");

 private:
  /** HTTP server lifecycle. */
  void StartHttpServer();
  void StopHttpServer();

  /** Route handlers — each returns true if the request was handled. */
  bool HandleStatus(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

  /** Active route handles for cleanup. */
  TArray<FHttpRouteHandle> RouteHandles;

  /** Server state. */
  bool bServerStarted = false;
  int32 ServerPort = 8090;

  /** Send a JSON success response. */
  static void SendJsonResponse(const FHttpResultCallback& OnComplete, const FString& JsonBody,
                               int32 StatusCode = 200);

  /** Send a JSON error response. */
  static void SendErrorResponse(const FHttpResultCallback& OnComplete, const FString& ErrorMessage,
                                int32 StatusCode = 400);
};
