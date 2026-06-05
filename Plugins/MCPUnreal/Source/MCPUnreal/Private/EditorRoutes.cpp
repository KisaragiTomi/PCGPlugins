// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.
//
// EditorRoutes.cpp — HTTP routes for editor utilities: output log, viewport
// capture, script execution, and console commands.
//
// See IMPLEMENTATION.md §3.11 and §5.1.
// Security: execute_script logs all scripts before execution (CLAUDE.md §4).

#include "MCPUnrealUtils.h"

#include "Misc/OutputDeviceRedirector.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Internationalization/Regex.h"
#include "HAL/PlatformProcess.h"
#include "Engine/GameViewportClient.h"
#include "UnrealClient.h"
#include "ImageUtils.h"
#include "Misc/Base64.h"
#if WITH_LIVE_CODING
#include "ILiveCodingModule.h"
#endif
#include "Features/IModularFeatures.h"
#include "Containers/Ticker.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Camera/PlayerCameraManager.h"
#include "LevelEditorViewport.h"

namespace MCPUnreal {

  // ---------------------------------------------------------------------------
  // Output log capture — ring buffer that captures recent log output.
  // ---------------------------------------------------------------------------

  class FMCPLogCapture : public FOutputDevice {
   public:
    static constexpr int32 MaxEntries = 10000;

    struct FLogEntry {
      FString Category;
      FString Message;
      ELogVerbosity::Type Verbosity;
      double Timestamp;  // FPlatformTime::Seconds() at capture time.
    };

    void Install() {
      if (!bInstalled) {
        GLog->AddOutputDevice(this);
        bInstalled = true;
      }
    }

    void Uninstall() {
      if (bInstalled) {
        GLog->RemoveOutputDevice(this);
        bInstalled = false;
      }
    }

    TArray<FLogEntry> GetEntries(const FString& CategoryFilter, ELogVerbosity::Type MinVerbosity,
                                 int32 MaxLines, const FString& Pattern = FString(),
                                 double SinceSeconds = 0.0) const {
      FScopeLock Lock(&Mutex);
      TArray<FLogEntry> Result;

      // Pre-compile regex if pattern provided.
      TUniquePtr<FRegexPattern> RegexPattern;
      if (!Pattern.IsEmpty()) {
        RegexPattern = MakeUnique<FRegexPattern>(Pattern);
      }

      const double Now = FPlatformTime::Seconds();

      for (int32 i = Entries.Num() - 1; i >= 0 && Result.Num() < MaxLines; --i) {
        const FLogEntry& Entry = Entries[i];
        if (Entry.Verbosity > MinVerbosity) {
          continue;
        }
        if (!CategoryFilter.IsEmpty() && !Entry.Category.Contains(CategoryFilter)) {
          continue;
        }
        // Time-based filter: skip entries older than SinceSeconds.
        if (SinceSeconds > 0.0 && (Now - Entry.Timestamp) > SinceSeconds) {
          continue;
        }
        // Regex pattern filter on message text.
        if (RegexPattern.IsValid()) {
          FRegexMatcher Matcher(*RegexPattern, Entry.Message);
          if (!Matcher.FindNext()) {
            continue;
          }
        }
        Result.Insert(Entry, 0);
      }
      return Result;
    }

    virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity,
                           const FName& Category) override {
      FScopeLock Lock(&Mutex);
      FLogEntry Entry;
      Entry.Category = Category.ToString();
      Entry.Message = Message;
      Entry.Verbosity = Verbosity;
      Entry.Timestamp = FPlatformTime::Seconds();
      Entries.Add(MoveTemp(Entry));

      if (Entries.Num() > MaxEntries) {
        Entries.RemoveAt(0, Entries.Num() - MaxEntries);
      }
    }

   private:
    mutable FCriticalSection Mutex;
    TArray<FLogEntry> Entries;
    bool bInstalled = false;
  };

  /** Global log capture instance — installed on first use. */
  static FMCPLogCapture& GetLogCapture() {
    static FMCPLogCapture Instance;
    return Instance;
  }

  static FString VerbosityToString(ELogVerbosity::Type V) {
    switch (V) {
      case ELogVerbosity::Fatal:
        return TEXT("fatal");
      case ELogVerbosity::Error:
        return TEXT("error");
      case ELogVerbosity::Warning:
        return TEXT("warning");
      case ELogVerbosity::Display:
        return TEXT("display");
      case ELogVerbosity::Log:
        return TEXT("log");
      case ELogVerbosity::Verbose:
        return TEXT("verbose");
      default:
        return TEXT("unknown");
    }
  }

  static ELogVerbosity::Type StringToVerbosity(const FString& S) {
    if (S == TEXT("fatal")) return ELogVerbosity::Fatal;
    if (S == TEXT("error")) return ELogVerbosity::Error;
    if (S == TEXT("warning")) return ELogVerbosity::Warning;
    if (S == TEXT("display")) return ELogVerbosity::Display;
    if (S == TEXT("log")) return ELogVerbosity::Log;
    if (S == TEXT("verbose")) return ELogVerbosity::Verbose;
    return ELogVerbosity::All;  // Default: return everything.
  }

  // ---------------------------------------------------------------------------
  // POST /api/editor/output_log
  // ---------------------------------------------------------------------------

  static bool HandleOutputLog(const FHttpServerRequest& Request,
                              const FHttpResultCallback& OnComplete) {
    // Ensure log capture is running.
    GetLogCapture().Install();

    TSharedPtr<FJsonObject> Body;
    if (!ParseJsonBody(Request, Body)) {
      SendError(OnComplete, TEXT("Invalid JSON in request body"));
      return true;
    }

    const FString Category = Body->GetStringField(TEXT("category"));
    const FString VerbosityStr = Body->GetStringField(TEXT("verbosity"));
    const FString Pattern = Body->GetStringField(TEXT("pattern"));
    const double SinceSeconds = Body->GetNumberField(TEXT("since_seconds"));
    int32 MaxLines = static_cast<int32>(Body->GetNumberField(TEXT("max_lines")));
    if (MaxLines <= 0) MaxLines = 100;

    ELogVerbosity::Type MinVerbosity = StringToVerbosity(VerbosityStr);
    TArray<FMCPLogCapture::FLogEntry> Entries =
        GetLogCapture().GetEntries(Category, MinVerbosity, MaxLines, Pattern, SinceSeconds);

    TArray<TSharedPtr<FJsonValue>> EntriesArray;
    for (const auto& Entry : Entries) {
      TSharedPtr<FJsonObject> EntryJson = MakeShareable(new FJsonObject());
      EntryJson->SetStringField(TEXT("category"), Entry.Category);
      EntryJson->SetStringField(TEXT("verbosity"), VerbosityToString(Entry.Verbosity));
      EntryJson->SetStringField(TEXT("message"), Entry.Message);
      EntriesArray.Add(MakeShareable(new FJsonValueObject(EntryJson)));
    }

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    ResponseJson->SetArrayField(TEXT("entries"), EntriesArray);
    ResponseJson->SetNumberField(TEXT("count"), EntriesArray.Num());
    SendJson(OnComplete, ResponseJson);
    return true;
  }

  // ---------------------------------------------------------------------------
  // POST /api/editor/capture_viewport
  // ---------------------------------------------------------------------------

  /** Build and send the viewport capture HTTP response from a bitmap. */
  static void SendCaptureResponse(const FHttpResultCallback& OnComplete, const FString& OutputPath,
                                  int32 Width, int32 Height, TArray<FColor>& Bitmap) {
    // Fix alpha channel: Metal on macOS returns A=0 (fully transparent) from
    // ReadPixels, which makes the PNG appear blank in viewers. Force opaque.
    for (FColor& Pixel : Bitmap) {
      Pixel.A = 255;
    }

    TArray64<uint8> PNGData;
    FImageUtils::PNGCompressImageArray(Width, Height, Bitmap, PNGData);

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    ResponseJson->SetBoolField(TEXT("success"), true);
    ResponseJson->SetNumberField(TEXT("width"), Width);
    ResponseJson->SetNumberField(TEXT("height"), Height);

    if (!OutputPath.IsEmpty()) {
      FFileHelper::SaveArrayToFile(PNGData, *OutputPath);
      ResponseJson->SetStringField(TEXT("file_path"), OutputPath);
    } else {
      ResponseJson->SetStringField(
          TEXT("image_base64"),
          FBase64::Encode(PNGData.GetData(), static_cast<uint32>(PNGData.Num())));
      ResponseJson->SetStringField(TEXT("format"), TEXT("png"));
    }
    SendJson(OnComplete, ResponseJson);
  }

  static bool HandleCaptureViewport(const FHttpServerRequest& Request,
                                    const FHttpResultCallback& OnComplete) {
    TSharedPtr<FJsonObject> Body;
    if (!ParseJsonBody(Request, Body)) {
      SendError(OnComplete, TEXT("Invalid JSON in request body"));
      return true;
    }

    const FString OutputPath = Body->GetStringField(TEXT("output_path"));
    bool bIncludeUI = false;
    Body->TryGetBoolField(TEXT("include_ui"), bIncludeUI);

    if (bIncludeUI) {
      // Composited capture (3D scene + Slate/UMG widgets) via FScreenshotRequest.
      // This is async: the engine captures on the next rendered frame and fires
      // UGameViewportClient::OnScreenshotCaptured with the final bitmap.
      if (!GEngine || !GEngine->GameViewport) {
        SendError(OnComplete, TEXT("include_ui requires a game viewport — start PIE first"));
        return true;
      }

      // Shared state for the deferred HTTP response.
      struct FCaptureCtx {
        FHttpResultCallback Callback;
        FString OutputPath;
        FDelegateHandle ScreenshotHandle;
        FTSTicker::FDelegateHandle TimeoutHandle;
        bool bDone = false;
      };
      TSharedPtr<FCaptureCtx> Ctx = MakeShared<FCaptureCtx>();
      Ctx->Callback = OnComplete;
      Ctx->OutputPath = OutputPath;

      // Listen for the composited screenshot from the game viewport.
      Ctx->ScreenshotHandle = UGameViewportClient::OnScreenshotCaptured().AddLambda(
          [Ctx](int32 W, int32 H, const TArray<FColor>& InBitmap) {
            if (Ctx->bDone) return;
            Ctx->bDone = true;
            UGameViewportClient::OnScreenshotCaptured().Remove(Ctx->ScreenshotHandle);
            FTSTicker::GetCoreTicker().RemoveTicker(Ctx->TimeoutHandle);

            TArray<FColor> Bitmap = InBitmap;
            SendCaptureResponse(Ctx->Callback, Ctx->OutputPath, W, H, Bitmap);
          });

      // Timeout after 5 seconds to avoid hanging the HTTP connection.
      auto Elapsed = MakeShared<float>(0.0f);
      Ctx->TimeoutHandle = FTSTicker::GetCoreTicker().AddTicker(
          FTickerDelegate::CreateLambda([Ctx, Elapsed](float Dt) -> bool {
            *Elapsed += Dt;
            if (*Elapsed > 5.0f && !Ctx->bDone) {
              Ctx->bDone = true;
              UGameViewportClient::OnScreenshotCaptured().Remove(Ctx->ScreenshotHandle);
              SendError(Ctx->Callback, TEXT("Screenshot capture timed out (5s)"), 500);
              return false;
            }
            return !Ctx->bDone;
          }));

      // Request screenshot on the next frame. bShowUI=true composites Slate.
      FScreenshotRequest::RequestScreenshot(true);
      return true;  // Response sent from delegate callback.
    }

    // Direct ReadPixels path (3D scene only, no Slate UI overlay).
    FViewport* Viewport = GetViewport(Body);
    if (!Viewport) {
      SendError(OnComplete, TEXT("No active viewport available"), 500);
      return true;
    }

    TArray<FColor> Bitmap;
    if (!Viewport->ReadPixels(Bitmap)) {
      SendError(OnComplete, TEXT("Failed to read viewport pixels"), 500);
      return true;
    }

    const int32 Width = Viewport->GetSizeXY().X;
    const int32 Height = Viewport->GetSizeXY().Y;
    SendCaptureResponse(OnComplete, OutputPath, Width, Height, Bitmap);
    return true;
  }

  // ---------------------------------------------------------------------------
  // POST /api/editor/execute_script
  // ---------------------------------------------------------------------------

  static bool HandleExecuteScript(const FHttpServerRequest& Request,
                                  const FHttpResultCallback& OnComplete) {
    TSharedPtr<FJsonObject> Body;
    if (!ParseJsonBody(Request, Body)) {
      SendError(OnComplete, TEXT("Invalid JSON in request body"));
      return true;
    }

    const FString Script = Body->GetStringField(TEXT("script"));
    if (Script.IsEmpty()) {
      SendError(OnComplete, TEXT("script is required"));
      return true;
    }

    // SECURITY: Log the full script before execution (CLAUDE.md Security §4).
    UE_LOG(LogMCPUnreal, Warning, TEXT("=== SCRIPT EXECUTION REQUEST ==="));
    UE_LOG(LogMCPUnreal, Warning, TEXT("%s"), *Script);
    UE_LOG(LogMCPUnreal, Warning, TEXT("=== END SCRIPT ==="));

    // Execute via editor Python scripting (requires Python Editor Script Plugin).
    UWorld* World = GetWorld(Body);
    FString Output;
    bool bSuccess = false;

    if (GEngine && World) {
      FMCPLogCapture& LogCapture = GetLogCapture();
      LogCapture.Install();
      const double PreExecTime = FPlatformTime::Seconds();

      FStringOutputDevice ExecOutput;
      FString Command = FString::Printf(TEXT("py %s"), *Script);
      bSuccess = GEngine->Exec(World, *Command, ExecOutput);

      const double Elapsed = FPlatformTime::Seconds() - PreExecTime + 0.5;
      TArray<FMCPLogCapture::FLogEntry> PythonLogs = LogCapture.GetEntries(
          TEXT("Python"), ELogVerbosity::All, 500, FString(), Elapsed);

      TArray<FString> Lines;
      if (!ExecOutput.IsEmpty()) { Lines.Add(ExecOutput); }
      for (const auto& Entry : PythonLogs) {
        Lines.Add(Entry.Message);
      }
      Output = FString::Join(Lines, TEXT("\n"));
    }

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    ResponseJson->SetBoolField(TEXT("success"), bSuccess);
    ResponseJson->SetStringField(TEXT("output"), Output);
    SendJson(OnComplete, ResponseJson);
    return true;
  }

  // ---------------------------------------------------------------------------
  // POST /api/editor/console_command
  // ---------------------------------------------------------------------------

  static bool HandleConsoleCommand(const FHttpServerRequest& Request,
                                   const FHttpResultCallback& OnComplete) {
    TSharedPtr<FJsonObject> Body;
    if (!ParseJsonBody(Request, Body)) {
      SendError(OnComplete, TEXT("Invalid JSON in request body"));
      return true;
    }

    const FString Command = Body->GetStringField(TEXT("command"));
    if (Command.IsEmpty()) {
      SendError(OnComplete, TEXT("command is required"));
      return true;
    }

    UE_LOG(LogMCPUnreal, Log, TEXT("Executing console command: %s"), *Command);

    UWorld* World = GetWorld(Body);
    bool bSuccess = false;

    if (GEngine && World) {
      bSuccess = GEngine->Exec(World, *Command);
    }

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    ResponseJson->SetBoolField(TEXT("success"), bSuccess);
    ResponseJson->SetStringField(TEXT("command"), Command);
    SendJson(OnComplete, ResponseJson);
    return true;
  }

  // ---------------------------------------------------------------------------
  // POST /api/editor/live_compile
  // ---------------------------------------------------------------------------

  static bool HandleLiveCompile(const FHttpServerRequest& Request,
                                const FHttpResultCallback& OnComplete) {
    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());

#if WITH_LIVE_CODING
    ILiveCodingModule* LiveCoding =
        FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
    if (!LiveCoding) {
      ResponseJson->SetBoolField(TEXT("success"), false);
      ResponseJson->SetStringField(TEXT("status"), TEXT("Unavailable"));
      ResponseJson->SetStringField(
          TEXT("errors"),
          TEXT("Live Coding module is not loaded. Enable Live Coding in Editor Preferences."));
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    if (!LiveCoding->IsEnabledByDefault()) {
      ResponseJson->SetBoolField(TEXT("success"), false);
      ResponseJson->SetStringField(TEXT("status"), TEXT("Disabled"));
      ResponseJson->SetStringField(TEXT("errors"),
                                   TEXT("Live Coding is disabled in Editor Preferences. Enable it "
                                        "under Edit > Editor Preferences > Live Coding."));
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    if (LiveCoding->IsCompiling()) {
      ResponseJson->SetBoolField(TEXT("success"), false);
      ResponseJson->SetStringField(TEXT("status"), TEXT("Compiling"));
      ResponseJson->SetStringField(TEXT("errors"), TEXT("A compilation is already in progress."));
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    UE_LOG(LogMCPUnreal, Log, TEXT("Triggering Live Coding recompile via MCP"));
    LiveCoding->EnableByDefault(true);
    LiveCoding->Compile();

    ResponseJson->SetBoolField(TEXT("success"), true);
    ResponseJson->SetStringField(TEXT("status"), TEXT("Compiling"));
#else
    ResponseJson->SetBoolField(TEXT("success"), false);
    ResponseJson->SetStringField(TEXT("status"), TEXT("Unavailable"));
    ResponseJson->SetStringField(TEXT("errors"), TEXT("Live Coding is only available on Windows."));
#endif

    SendJson(OnComplete, ResponseJson);
    return true;
  }

  // ---------------------------------------------------------------------------
  // POST /api/editor/pie_control
  // ---------------------------------------------------------------------------

  static bool HandlePIEControl(const FHttpServerRequest& Request,
                               const FHttpResultCallback& OnComplete) {
    TSharedPtr<FJsonObject> Body;
    if (!ParseJsonBody(Request, Body)) {
      SendError(OnComplete, TEXT("Invalid JSON in request body"));
      return true;
    }

    FString Operation = Body->GetStringField(TEXT("operation"));

    if (Operation == TEXT("start")) {
      if (IsPIEActive()) {
        SendError(OnComplete, TEXT("PIE is already running"));
        return true;
      }

      FRequestPlaySessionParams Params;
      Params.WorldType = EPlaySessionWorldType::PlayInEditor;
      Params.SessionDestination = EPlaySessionDestinationType::InProcess;

      FString MapPath = Body->GetStringField(TEXT("map_path"));
      if (!MapPath.IsEmpty()) {
        Params.GlobalMapOverride = MapPath;
      }

      bool bSimulate = false;
      if (Body->TryGetBoolField(TEXT("simulate"), bSimulate) && bSimulate) {
        Params.WorldType = EPlaySessionWorldType::SimulateInEditor;
      }

      GEditor->RequestPlaySession(Params);

      TSharedPtr<FJsonObject> Resp = MakeShareable(new FJsonObject());
      Resp->SetBoolField(TEXT("success"), true);
      Resp->SetStringField(TEXT("message"),
                           TEXT("PIE start requested (async — use status to verify)"));
      SendJson(OnComplete, Resp);

    } else if (Operation == TEXT("stop")) {
      if (!IsPIEActive()) {
        SendError(OnComplete, TEXT("PIE is not running"));
        return true;
      }

      GEditor->RequestEndPlayMap();

      TSharedPtr<FJsonObject> Resp = MakeShareable(new FJsonObject());
      Resp->SetBoolField(TEXT("success"), true);
      Resp->SetStringField(TEXT("message"),
                           TEXT("PIE stop requested (async — use status to verify)"));
      SendJson(OnComplete, Resp);

    } else if (Operation == TEXT("status")) {
      TSharedPtr<FJsonObject> Resp = MakeShareable(new FJsonObject());
      Resp->SetBoolField(TEXT("pie_active"), IsPIEActive());
      if (IsPIEActive() && GEditor && GEditor->PlayWorld) {
        Resp->SetStringField(TEXT("pie_map"), GEditor->PlayWorld->GetMapName());
      }
      SendJson(OnComplete, Resp);

    } else {
      SendError(OnComplete, TEXT("Unknown operation. Valid: start, stop, status"));
    }

    return true;
  }

  // ---------------------------------------------------------------------------
  // POST /api/editor/player_control
  // ---------------------------------------------------------------------------

  static bool HandlePlayerControl(const FHttpServerRequest& Request,
                                  const FHttpResultCallback& OnComplete) {
    TSharedPtr<FJsonObject> Body;
    if (!ParseJsonBody(Request, Body)) {
      SendError(OnComplete, TEXT("Invalid JSON in request body"));
      return true;
    }

    FString Operation = Body->GetStringField(TEXT("operation"));

    // --- get_camera / set_camera: work without PIE ---
    if (Operation == TEXT("get_camera")) {
      if (!GEditor || !GEditor->GetActiveViewport()) {
        SendError(OnComplete, TEXT("No active editor viewport"));
        return true;
      }
      FEditorViewportClient* ViewClient =
          static_cast<FEditorViewportClient*>(GEditor->GetActiveViewport()->GetClient());
      if (!ViewClient) {
        SendError(OnComplete, TEXT("No editor viewport client available"));
        return true;
      }

      FVector Loc = ViewClient->GetViewLocation();
      FRotator Rot = ViewClient->GetViewRotation();

      TArray<TSharedPtr<FJsonValue>> LocArr;
      LocArr.Add(MakeShareable(new FJsonValueNumber(Loc.X)));
      LocArr.Add(MakeShareable(new FJsonValueNumber(Loc.Y)));
      LocArr.Add(MakeShareable(new FJsonValueNumber(Loc.Z)));

      TArray<TSharedPtr<FJsonValue>> RotArr;
      RotArr.Add(MakeShareable(new FJsonValueNumber(Rot.Pitch)));
      RotArr.Add(MakeShareable(new FJsonValueNumber(Rot.Yaw)));
      RotArr.Add(MakeShareable(new FJsonValueNumber(Rot.Roll)));

      TSharedPtr<FJsonObject> Resp = MakeShareable(new FJsonObject());
      Resp->SetArrayField(TEXT("camera_location"), LocArr);
      Resp->SetArrayField(TEXT("camera_rotation"), RotArr);
      Resp->SetBoolField(TEXT("success"), true);
      SendJson(OnComplete, Resp);
      return true;
    }

    if (Operation == TEXT("set_camera")) {
      if (!GEditor || !GEditor->GetActiveViewport()) {
        SendError(OnComplete, TEXT("No active editor viewport"));
        return true;
      }
      FEditorViewportClient* ViewClient =
          static_cast<FEditorViewportClient*>(GEditor->GetActiveViewport()->GetClient());
      if (!ViewClient) {
        SendError(OnComplete, TEXT("No editor viewport client available"));
        return true;
      }

      const TArray<TSharedPtr<FJsonValue>>* LocArr;
      if (Body->TryGetArrayField(TEXT("location"), LocArr) && LocArr->Num() >= 3) {
        FVector NewLoc((*LocArr)[0]->AsNumber(), (*LocArr)[1]->AsNumber(),
                       (*LocArr)[2]->AsNumber());
        ViewClient->SetViewLocation(NewLoc);
      }

      const TArray<TSharedPtr<FJsonValue>>* RotArr;
      if (Body->TryGetArrayField(TEXT("rotation"), RotArr) && RotArr->Num() >= 3) {
        FRotator NewRot((*RotArr)[0]->AsNumber(), (*RotArr)[1]->AsNumber(),
                        (*RotArr)[2]->AsNumber());
        ViewClient->SetViewRotation(NewRot);
      }

      ViewClient->Invalidate();

      TSharedPtr<FJsonObject> Resp = MakeShareable(new FJsonObject());
      Resp->SetBoolField(TEXT("success"), true);
      Resp->SetStringField(TEXT("message"), TEXT("Editor viewport camera updated"));
      SendJson(OnComplete, Resp);
      return true;
    }

    // --- Player operations: require PIE ---
    UWorld* World = GetWorld(Body);
    if (!World) {
      SendError(OnComplete,
                TEXT("PIE not running — player operations require an active PIE session"));
      return true;
    }

    APlayerController* PC = World->GetFirstPlayerController();
    if (!PC) {
      SendError(OnComplete, TEXT("No player controller found in the current world"));
      return true;
    }

    if (Operation == TEXT("get_info")) {
      APawn* Pawn = PC->GetPawn();

      TSharedPtr<FJsonObject> Resp = MakeShareable(new FJsonObject());
      Resp->SetStringField(TEXT("controller_path"), PC->GetPathName());

      if (Pawn) {
        Resp->SetStringField(TEXT("pawn_path"), Pawn->GetPathName());
        Resp->SetStringField(TEXT("pawn_class"), Pawn->GetClass()->GetName());

        FVector PawnLoc = Pawn->GetActorLocation();
        TArray<TSharedPtr<FJsonValue>> LocArr;
        LocArr.Add(MakeShareable(new FJsonValueNumber(PawnLoc.X)));
        LocArr.Add(MakeShareable(new FJsonValueNumber(PawnLoc.Y)));
        LocArr.Add(MakeShareable(new FJsonValueNumber(PawnLoc.Z)));
        Resp->SetArrayField(TEXT("location"), LocArr);

        FRotator PawnRot = Pawn->GetActorRotation();
        TArray<TSharedPtr<FJsonValue>> RotArr;
        RotArr.Add(MakeShareable(new FJsonValueNumber(PawnRot.Pitch)));
        RotArr.Add(MakeShareable(new FJsonValueNumber(PawnRot.Yaw)));
        RotArr.Add(MakeShareable(new FJsonValueNumber(PawnRot.Roll)));
        Resp->SetArrayField(TEXT("rotation"), RotArr);
      }

      FRotator ControlRot = PC->GetControlRotation();
      TArray<TSharedPtr<FJsonValue>> CtrlRotArr;
      CtrlRotArr.Add(MakeShareable(new FJsonValueNumber(ControlRot.Pitch)));
      CtrlRotArr.Add(MakeShareable(new FJsonValueNumber(ControlRot.Yaw)));
      CtrlRotArr.Add(MakeShareable(new FJsonValueNumber(ControlRot.Roll)));
      Resp->SetArrayField(TEXT("control_rotation"), CtrlRotArr);

      // Camera info from the player camera manager.
      if (PC->PlayerCameraManager) {
        FVector CamLoc = PC->PlayerCameraManager->GetCameraLocation();
        FRotator CamRot = PC->PlayerCameraManager->GetCameraRotation();

        TArray<TSharedPtr<FJsonValue>> CamLocArr;
        CamLocArr.Add(MakeShareable(new FJsonValueNumber(CamLoc.X)));
        CamLocArr.Add(MakeShareable(new FJsonValueNumber(CamLoc.Y)));
        CamLocArr.Add(MakeShareable(new FJsonValueNumber(CamLoc.Z)));
        Resp->SetArrayField(TEXT("camera_location"), CamLocArr);

        TArray<TSharedPtr<FJsonValue>> CamRotArr;
        CamRotArr.Add(MakeShareable(new FJsonValueNumber(CamRot.Pitch)));
        CamRotArr.Add(MakeShareable(new FJsonValueNumber(CamRot.Yaw)));
        CamRotArr.Add(MakeShareable(new FJsonValueNumber(CamRot.Roll)));
        Resp->SetArrayField(TEXT("camera_rotation"), CamRotArr);
      }

      Resp->SetBoolField(TEXT("success"), true);
      SendJson(OnComplete, Resp);
      return true;
    }

    if (Operation == TEXT("teleport")) {
      APawn* Pawn = PC->GetPawn();
      if (!Pawn) {
        SendError(OnComplete, TEXT("Player controller has no possessed pawn"));
        return true;
      }

      const TArray<TSharedPtr<FJsonValue>>* LocArr;
      if (!Body->TryGetArrayField(TEXT("location"), LocArr) || LocArr->Num() < 3) {
        SendError(OnComplete, TEXT("location [X,Y,Z] array is required for teleport"));
        return true;
      }

      FVector NewLoc((*LocArr)[0]->AsNumber(), (*LocArr)[1]->AsNumber(), (*LocArr)[2]->AsNumber());

      FRotator NewRot = Pawn->GetActorRotation();
      const TArray<TSharedPtr<FJsonValue>>* RotArr;
      if (Body->TryGetArrayField(TEXT("rotation"), RotArr) && RotArr->Num() >= 3) {
        NewRot =
            FRotator((*RotArr)[0]->AsNumber(), (*RotArr)[1]->AsNumber(), (*RotArr)[2]->AsNumber());
      }

      bool bTeleported = Pawn->TeleportTo(NewLoc, NewRot);

      TSharedPtr<FJsonObject> Resp = MakeShareable(new FJsonObject());
      Resp->SetBoolField(TEXT("success"), bTeleported);
      if (bTeleported) {
        FVector ResultLoc = Pawn->GetActorLocation();
        TArray<TSharedPtr<FJsonValue>> ResultLocArr;
        ResultLocArr.Add(MakeShareable(new FJsonValueNumber(ResultLoc.X)));
        ResultLocArr.Add(MakeShareable(new FJsonValueNumber(ResultLoc.Y)));
        ResultLocArr.Add(MakeShareable(new FJsonValueNumber(ResultLoc.Z)));
        Resp->SetArrayField(TEXT("location"), ResultLocArr);

        FRotator ResultRot = Pawn->GetActorRotation();
        TArray<TSharedPtr<FJsonValue>> ResultRotArr;
        ResultRotArr.Add(MakeShareable(new FJsonValueNumber(ResultRot.Pitch)));
        ResultRotArr.Add(MakeShareable(new FJsonValueNumber(ResultRot.Yaw)));
        ResultRotArr.Add(MakeShareable(new FJsonValueNumber(ResultRot.Roll)));
        Resp->SetArrayField(TEXT("rotation"), ResultRotArr);

        Resp->SetStringField(TEXT("message"), TEXT("Pawn teleported successfully"));
      } else {
        Resp->SetStringField(TEXT("message"), TEXT("Teleport failed — destination may be blocked"));
      }
      SendJson(OnComplete, Resp);
      return true;
    }

    if (Operation == TEXT("set_rotation")) {
      const TArray<TSharedPtr<FJsonValue>>* RotArr;
      if (!Body->TryGetArrayField(TEXT("rotation"), RotArr) || RotArr->Num() < 3) {
        SendError(OnComplete, TEXT("rotation [Pitch,Yaw,Roll] array is required for set_rotation"));
        return true;
      }

      FRotator NewRot((*RotArr)[0]->AsNumber(), (*RotArr)[1]->AsNumber(), (*RotArr)[2]->AsNumber());
      PC->SetControlRotation(NewRot);

      TSharedPtr<FJsonObject> Resp = MakeShareable(new FJsonObject());
      Resp->SetBoolField(TEXT("success"), true);
      Resp->SetStringField(TEXT("message"), TEXT("Control rotation updated"));

      TArray<TSharedPtr<FJsonValue>> ResultRotArr;
      ResultRotArr.Add(MakeShareable(new FJsonValueNumber(NewRot.Pitch)));
      ResultRotArr.Add(MakeShareable(new FJsonValueNumber(NewRot.Yaw)));
      ResultRotArr.Add(MakeShareable(new FJsonValueNumber(NewRot.Roll)));
      Resp->SetArrayField(TEXT("control_rotation"), ResultRotArr);
      SendJson(OnComplete, Resp);
      return true;
    }

    if (Operation == TEXT("set_view_target")) {
      FString ActorPath = Body->GetStringField(TEXT("actor_path"));
      if (ActorPath.IsEmpty()) {
        SendError(OnComplete, TEXT("actor_path is required for set_view_target"));
        return true;
      }

      AActor* Target = nullptr;
      for (TActorIterator<AActor> It(World); It; ++It) {
        if (It->GetPathName() == ActorPath || It->GetActorLabel() == ActorPath) {
          Target = *It;
          break;
        }
      }

      if (!Target) {
        SendError(OnComplete, FString::Printf(TEXT("Actor not found: %s"), *ActorPath));
        return true;
      }

      PC->SetViewTarget(Target);

      TSharedPtr<FJsonObject> Resp = MakeShareable(new FJsonObject());
      Resp->SetBoolField(TEXT("success"), true);
      Resp->SetStringField(TEXT("message"), FString::Printf(TEXT("View target set to %s"),
                                                            *Target->GetActorLabel()));
      Resp->SetStringField(TEXT("target_path"), Target->GetPathName());
      SendJson(OnComplete, Resp);
      return true;
    }

    SendError(OnComplete,
              TEXT("Unknown operation. Valid: get_info, teleport, set_rotation, set_view_target, "
                   "get_camera, set_camera"));
    return true;
  }

  // ---------------------------------------------------------------------------
  // Registration
  // ---------------------------------------------------------------------------

  void RegisterEditorRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles) {
    // Install log capture on route registration.
    GetLogCapture().Install();

    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/editor/output_log")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleOutputLog)));

    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/editor/capture_viewport")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleCaptureViewport)));

    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/editor/execute_script")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleExecuteScript)));

    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/editor/console_command")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleConsoleCommand)));

    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/editor/live_compile")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleLiveCompile)));

    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/editor/pie_control")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandlePIEControl)));

    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/editor/player_control")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandlePlayerControl)));

    UE_LOG(LogMCPUnreal, Verbose, TEXT("Registered editor utility routes (7 endpoints)"));
  }

}  // namespace MCPUnreal
