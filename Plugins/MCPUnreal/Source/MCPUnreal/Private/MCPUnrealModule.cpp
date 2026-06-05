// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.
//
// MCPUnrealModule.cpp — HTTP server startup, route registration, and
// status endpoint implementation.
//
// See IMPLEMENTATION.md §5 for the plugin architecture.

#include "MCPUnrealModule.h"
#include "MCPUnrealUtils.h"

#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/App.h"

DEFINE_LOG_CATEGORY(LogMCPUnreal);

// Console variable for the HTTP server port.
static TAutoConsoleVariable<int32> CVarMCPPort(
    TEXT("mcp.Port"), 8090, TEXT("HTTP server port for the MCPUnreal editor plugin. Default 8090."),
    ECVF_Default);

// ---------------------------------------------------------------------------
// Module lifecycle
// ---------------------------------------------------------------------------

void FMCPUnrealModule::StartupModule() {
  UE_LOG(LogMCPUnreal, Log, TEXT("MCPUnreal plugin starting (version %s)"), PluginVersion);
  StartHttpServer();
}

void FMCPUnrealModule::ShutdownModule() {
  UE_LOG(LogMCPUnreal, Log, TEXT("MCPUnreal plugin shutting down"));
  StopHttpServer();
}

// ---------------------------------------------------------------------------
// HTTP server
// ---------------------------------------------------------------------------

void FMCPUnrealModule::StartHttpServer() {
  if (bServerStarted) {
    return;
  }

  ServerPort = CVarMCPPort.GetValueOnGameThread();

  FHttpServerModule& HttpModule =
      FModuleManager::LoadModuleChecked<FHttpServerModule>("HTTPServer");
  TSharedPtr<IHttpRouter> Router = HttpModule.GetHttpRouter(ServerPort);

  if (!Router.IsValid()) {
    UE_LOG(LogMCPUnreal, Error, TEXT("Failed to create HTTP router on port %d"), ServerPort);
    return;
  }

  // Register routes. Each handler validates its input JSON.
  // POST /api/status — server health and capabilities.
  RouteHandles.Add(
      Router->BindRoute(FHttpPath(TEXT("/api/status")),
                        EHttpServerRequestVerbs::VERB_POST | EHttpServerRequestVerbs::VERB_GET,
                        FHttpRequestHandler::CreateRaw(this, &FMCPUnrealModule::HandleStatus)));

  // Phase 5 routes — each Register* function binds its own endpoints.
  MCPUnreal::RegisterActorRoutes(Router, RouteHandles);
  MCPUnreal::RegisterBlueprintRoutes(Router, RouteHandles);
  MCPUnreal::RegisterAnimBPRoutes(Router, RouteHandles);
  MCPUnreal::RegisterEditorRoutes(Router, RouteHandles);
  MCPUnreal::RegisterAssetRoutes(Router, RouteHandles);
  MCPUnreal::RegisterMaterialRoutes(Router, RouteHandles);
  MCPUnreal::RegisterCharacterRoutes(Router, RouteHandles);
  MCPUnreal::RegisterInputRoutes(Router, RouteHandles);
  MCPUnreal::RegisterLevelRoutes(Router, RouteHandles);
  MCPUnreal::RegisterMeshRoutes(Router, RouteHandles);
  MCPUnreal::RegisterPCGRoutes(Router, RouteHandles);
  MCPUnreal::RegisterGASRoutes(Router, RouteHandles);
  MCPUnreal::RegisterNiagaraRoutes(Router, RouteHandles);
  MCPUnreal::RegisterComponentRoutes(Router, RouteHandles);
  MCPUnreal::RegisterISMRoutes(Router, RouteHandles);
  MCPUnreal::RegisterFabRoutes(Router, RouteHandles);
  MCPUnreal::RegisterTextureRoutes(Router, RouteHandles);
  MCPUnreal::RegisterSubsystemRoutes(Router, RouteHandles);
  MCPUnreal::RegisterDataAssetRoutes(Router, RouteHandles);
  MCPUnreal::RegisterUIQueryRoutes(Router, RouteHandles);
  MCPUnreal::RegisterNetworkDebugRoutes(Router, RouteHandles);

  HttpModule.StartAllListeners();
  bServerStarted = true;

  UE_LOG(LogMCPUnreal, Log, TEXT("MCPUnreal HTTP server started on 127.0.0.1:%d (routes: %d)"),
         ServerPort, RouteHandles.Num());
}

void FMCPUnrealModule::StopHttpServer() {
  if (!bServerStarted) {
    return;
  }

  // Unbind all routes.
  if (FModuleManager::Get().IsModuleLoaded("HTTPServer")) {
    FHttpServerModule& HttpModule =
        FModuleManager::GetModuleChecked<FHttpServerModule>("HTTPServer");
    TSharedPtr<IHttpRouter> Router = HttpModule.GetHttpRouter(ServerPort);
    if (Router.IsValid()) {
      for (const FHttpRouteHandle& Handle : RouteHandles) {
        Router->UnbindRoute(Handle);
      }
    }
  }

  RouteHandles.Empty();
  bServerStarted = false;

  UE_LOG(LogMCPUnreal, Log, TEXT("MCPUnreal HTTP server stopped"));
}

// ---------------------------------------------------------------------------
// POST /api/status
// ---------------------------------------------------------------------------

bool FMCPUnrealModule::HandleStatus(const FHttpServerRequest& Request,
                                    const FHttpResultCallback& OnComplete) {
  TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
  ResponseJson->SetStringField(TEXT("name"), TEXT("MCPUnreal"));
  ResponseJson->SetStringField(TEXT("version"), PluginVersion);
  ResponseJson->SetStringField(TEXT("ue_version"), FApp::GetBuildVersion());
  ResponseJson->SetNumberField(TEXT("port"), ServerPort);

  // Report project name from the loaded project.
  FString ProjectName = FApp::GetProjectName();
  ResponseJson->SetStringField(TEXT("project"), ProjectName);

  // PIE state.
  ResponseJson->SetBoolField(TEXT("pie_active"), MCPUnreal::IsPIEActive());
  if (MCPUnreal::IsPIEActive() && GEditor && GEditor->PlayWorld) {
    ResponseJson->SetStringField(TEXT("pie_map"), GEditor->PlayWorld->GetMapName());
  }

  // Capabilities list — all registered route groups.
  TArray<TSharedPtr<FJsonValue>> Capabilities;
  Capabilities.Add(MakeShareable(new FJsonValueString(TEXT("status"))));
  Capabilities.Add(MakeShareable(new FJsonValueString(TEXT("actors"))));
  Capabilities.Add(MakeShareable(new FJsonValueString(TEXT("blueprints"))));
  Capabilities.Add(MakeShareable(new FJsonValueString(TEXT("anim_blueprints"))));
  Capabilities.Add(MakeShareable(new FJsonValueString(TEXT("editor"))));
  Capabilities.Add(MakeShareable(new FJsonValueString(TEXT("assets"))));
  Capabilities.Add(MakeShareable(new FJsonValueString(TEXT("materials"))));
  Capabilities.Add(MakeShareable(new FJsonValueString(TEXT("characters"))));
  Capabilities.Add(MakeShareable(new FJsonValueString(TEXT("input"))));
  Capabilities.Add(MakeShareable(new FJsonValueString(TEXT("levels"))));
  Capabilities.Add(MakeShareable(new FJsonValueString(TEXT("mesh"))));
  Capabilities.Add(MakeShareable(new FJsonValueString(TEXT("pcg"))));
  Capabilities.Add(MakeShareable(new FJsonValueString(TEXT("gas"))));
  Capabilities.Add(MakeShareable(new FJsonValueString(TEXT("niagara"))));
  Capabilities.Add(MakeShareable(new FJsonValueString(TEXT("components"))));
  Capabilities.Add(MakeShareable(new FJsonValueString(TEXT("ism"))));
  Capabilities.Add(MakeShareable(new FJsonValueString(TEXT("fab"))));
  Capabilities.Add(MakeShareable(new FJsonValueString(TEXT("textures"))));
  Capabilities.Add(MakeShareable(new FJsonValueString(TEXT("subsystems"))));
  Capabilities.Add(MakeShareable(new FJsonValueString(TEXT("data_assets"))));
  Capabilities.Add(MakeShareable(new FJsonValueString(TEXT("ui_query"))));
  Capabilities.Add(MakeShareable(new FJsonValueString(TEXT("network_debug"))));
  ResponseJson->SetArrayField(TEXT("capabilities"), Capabilities);

  FString ResponseStr;
  TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseStr);
  FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);

  SendJsonResponse(OnComplete, ResponseStr);
  return true;
}

// ---------------------------------------------------------------------------
// Response helpers
// ---------------------------------------------------------------------------

void FMCPUnrealModule::SendJsonResponse(const FHttpResultCallback& OnComplete,
                                        const FString& JsonBody, int32 StatusCode) {
  auto Response = FHttpServerResponse::Create(JsonBody, TEXT("application/json"));
  OnComplete(MoveTemp(Response));
}

void FMCPUnrealModule::SendErrorResponse(const FHttpResultCallback& OnComplete,
                                         const FString& ErrorMessage, int32 StatusCode) {
  TSharedPtr<FJsonObject> ErrorJson = MakeShareable(new FJsonObject());
  ErrorJson->SetStringField(TEXT("error"), ErrorMessage);

  FString ErrorStr;
  TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ErrorStr);
  FJsonSerializer::Serialize(ErrorJson.ToSharedRef(), Writer);

  SendJsonResponse(OnComplete, ErrorStr, StatusCode);
}

IMPLEMENT_MODULE(FMCPUnrealModule, MCPUnreal)
