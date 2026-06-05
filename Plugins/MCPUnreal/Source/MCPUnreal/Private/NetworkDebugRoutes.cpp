// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.
//
// NetworkDebugRoutes.cpp — HTTP route for network introspection:
// active HTTP requests, recent request history, WebSocket status.
// See issue #48 and #50.

#include "MCPUnrealUtils.h"

#include "HttpManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/DateTime.h"
#include "Containers/Ticker.h"

namespace MCPUnreal {

  // ---------------------------------------------------------------------------
  // HTTP request tracking ring buffer.
  // ---------------------------------------------------------------------------

  struct FHTTPRequestRecord {
    FString URL;
    FString Method;
    int32 StatusCode = 0;
    double DurationMs = 0.0;
    FString Error;
    FDateTime Timestamp;
    bool bActive = false;
  };

  class FNetworkTracker {
   public:
    static constexpr int32 MaxRecords = 200;

    void RecordRequest(const FString& URL, const FString& Method) {
      FScopeLock Lock(&Mutex);
      FHTTPRequestRecord Record;
      Record.URL = URL;
      Record.Method = Method;
      Record.Timestamp = FDateTime::Now();
      Record.bActive = true;
      Records.Add(MoveTemp(Record));

      if (Records.Num() > MaxRecords) {
        Records.RemoveAt(0, Records.Num() - MaxRecords);
      }
    }

    void CompleteRequest(const FString& URL, int32 StatusCode, double DurationMs,
                         const FString& Error) {
      FScopeLock Lock(&Mutex);
      for (int32 i = Records.Num() - 1; i >= 0; --i) {
        if (Records[i].URL == URL && Records[i].bActive) {
          Records[i].StatusCode = StatusCode;
          Records[i].DurationMs = DurationMs;
          Records[i].Error = Error;
          Records[i].bActive = false;
          break;
        }
      }
    }

    TArray<FHTTPRequestRecord> GetActive() const {
      FScopeLock Lock(&Mutex);
      TArray<FHTTPRequestRecord> Result;
      for (const auto& R : Records) {
        if (R.bActive) Result.Add(R);
      }
      return Result;
    }

    TArray<FHTTPRequestRecord> GetRecent(int32 N) const {
      FScopeLock Lock(&Mutex);
      int32 Start = FMath::Max(0, Records.Num() - N);
      TArray<FHTTPRequestRecord> Result;
      for (int32 i = Start; i < Records.Num(); ++i) {
        Result.Add(Records[i]);
      }
      return Result;
    }

    int32 GetTotalCount() const {
      FScopeLock Lock(&Mutex);
      return Records.Num();
    }

    int32 GetActiveCount() const {
      FScopeLock Lock(&Mutex);
      int32 Count = 0;
      for (const auto& R : Records) {
        if (R.bActive) ++Count;
      }
      return Count;
    }

   private:
    mutable FCriticalSection Mutex;
    TArray<FHTTPRequestRecord> Records;
  };

  static FNetworkTracker& GetTracker() {
    static FNetworkTracker Instance;
    return Instance;
  }

  static TSharedPtr<FJsonObject> RequestToJson(const FHTTPRequestRecord& Record) {
    TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject());
    Json->SetStringField(TEXT("url"), Record.URL);
    Json->SetStringField(TEXT("method"), Record.Method);
    if (Record.StatusCode > 0) {
      Json->SetNumberField(TEXT("status_code"), Record.StatusCode);
    }
    if (Record.DurationMs > 0) {
      Json->SetNumberField(TEXT("duration_ms"), Record.DurationMs);
    }
    if (!Record.Error.IsEmpty()) {
      Json->SetStringField(TEXT("error"), Record.Error);
    }
    Json->SetBoolField(TEXT("active"), Record.bActive);
    Json->SetStringField(TEXT("timestamp"), Record.Timestamp.ToIso8601());
    return Json;
  }

  // ---------------------------------------------------------------------------
  // Auto-tracking stub. Route registration keeps this disabled by default.
  // ---------------------------------------------------------------------------

  class FHTTPAutoTracker {
   public:
    void Start() {
      if (bStarted) return;
      bStarted = true;

      // Ticker runs every 0.5 seconds on the game thread.
      TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
          FTickerDelegate::CreateRaw(this, &FHTTPAutoTracker::OnTick), 0.5f);

      UE_LOG(LogMCPUnreal, Log, TEXT("Network auto-tracker started (polling every 0.5s)"));
    }

    void Stop() {
      if (!bStarted) return;
      if (TickerHandle.IsValid()) {
        FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
      }
      bStarted = false;
    }

    bool IsStarted() const { return bStarted; }

   private:
    bool OnTick(float DeltaTime) {
      static_cast<void>(DeltaTime);
      // Do not call FHttpManager::Flush() here. Flush can block the game thread
      // while outstanding editor connectivity checks time out.
      return true;  // Keep ticking.
    }

    FTSTicker::FDelegateHandle TickerHandle;
    bool bStarted = false;
  };

  static FHTTPAutoTracker& GetAutoTracker() {
    static FHTTPAutoTracker Instance;
    return Instance;
  }

  // ---------------------------------------------------------------------------
  // POST /api/network/debug
  // ---------------------------------------------------------------------------

  static bool HandleNetworkDebug(const FHttpServerRequest& Request,
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

    // --- list_active ---
    if (Operation == TEXT("list_active")) {
      TArray<FHTTPRequestRecord> Active = GetTracker().GetActive();

      TArray<TSharedPtr<FJsonValue>> ActiveArray;
      for (const auto& R : Active) {
        ActiveArray.Add(MakeShareable(new FJsonValueObject(RequestToJson(R))));
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetArrayField(TEXT("active_requests"), ActiveArray);
      ResponseJson->SetNumberField(TEXT("count"), ActiveArray.Num());

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- recent_requests ---
    if (Operation == TEXT("recent_requests")) {
      int32 LastN = static_cast<int32>(Body->GetNumberField(TEXT("last_n")));
      if (LastN <= 0) LastN = 20;

      TArray<FHTTPRequestRecord> Recent = GetTracker().GetRecent(LastN);

      TArray<TSharedPtr<FJsonValue>> RecentArray;
      for (const auto& R : Recent) {
        RecentArray.Add(MakeShareable(new FJsonValueObject(RequestToJson(R))));
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetArrayField(TEXT("recent_requests"), RecentArray);
      ResponseJson->SetNumberField(TEXT("count"), RecentArray.Num());

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- websocket_status ---
    if (Operation == TEXT("websocket_status")) {
      // Report summary — actual WebSocket tracking needs per-connection hooks
      // but we can report the overall module status and tracker stats.
      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetArrayField(TEXT("websockets"), TArray<TSharedPtr<FJsonValue>>());
      ResponseJson->SetNumberField(TEXT("count"), 0);
      ResponseJson->SetStringField(
          TEXT("note"),
          TEXT("WebSocket tracking requires per-connection instrumentation. ") TEXT(
              "Use get_output_log with pattern 'WebSocket' to find WebSocket activity in logs."));

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- summary ---
    if (Operation == TEXT("summary")) {
      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetNumberField(TEXT("total_tracked"), GetTracker().GetTotalCount());
      ResponseJson->SetNumberField(TEXT("active_count"), GetTracker().GetActiveCount());
      ResponseJson->SetBoolField(TEXT("auto_tracking_enabled"), GetAutoTracker().IsStarted());

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    SendError(OnComplete, FString::Printf(TEXT("Unknown network debug operation: %s"), *Operation));
    return true;
  }

  // ---------------------------------------------------------------------------
  // Registration
  // ---------------------------------------------------------------------------

  void RegisterNetworkDebugRoutes(TSharedPtr<IHttpRouter> Router,
                                  TArray<FHttpRouteHandle>& Handles) {
    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/network/debug")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleNetworkDebug)));

    UE_LOG(LogMCPUnreal, Verbose,
           TEXT("Registered network debug routes (1 endpoint, auto-tracking disabled)"));
  }

}  // namespace MCPUnreal
