// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.
//
// UIQueryRoutes.cpp — HTTP route for Slate/UMG widget introspection.
// See issue #47.

#include "MCPUnrealUtils.h"

#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"
#include "Components/WidgetComponent.h"
#include "Blueprint/UserWidget.h"
#include "EngineUtils.h"

namespace MCPUnreal {

  // Recursively build JSON for a Slate widget tree.
  static TSharedPtr<FJsonObject> WidgetToJson(const TSharedRef<SWidget>& Widget, int32 Depth,
                                              int32 MaxDepth) {
    TSharedPtr<FJsonObject> Info = MakeShareable(new FJsonObject());
    Info->SetStringField(TEXT("type"), Widget->GetType().ToString());

    FString WidgetName = Widget->ToString();
    if (WidgetName.Len() > 100) WidgetName = WidgetName.Left(100);
    Info->SetStringField(TEXT("name"), WidgetName);

    Info->SetBoolField(TEXT("visible"), Widget->GetVisibility().IsVisible());
    Info->SetBoolField(TEXT("enabled"), Widget->IsEnabled());

    // Bounds.
    FGeometry Geo = Widget->GetCachedGeometry();
    TSharedPtr<FJsonObject> Bounds = MakeShareable(new FJsonObject());
    Bounds->SetNumberField(TEXT("x"), Geo.GetAbsolutePosition().X);
    Bounds->SetNumberField(TEXT("y"), Geo.GetAbsolutePosition().Y);
    Bounds->SetNumberField(TEXT("width"), Geo.GetAbsoluteSize().X);
    Bounds->SetNumberField(TEXT("height"), Geo.GetAbsoluteSize().Y);
    Info->SetObjectField(TEXT("bounds"), Bounds);

    // Children (respect depth limit).
    if (MaxDepth <= 0 || Depth < MaxDepth) {
      FChildren* Children = const_cast<FChildren*>(Widget->GetChildren());
      if (Children && Children->Num() > 0) {
        TArray<TSharedPtr<FJsonValue>> ChildArray;
        for (int32 i = 0; i < Children->Num(); ++i) {
          TSharedRef<SWidget> Child = Children->GetChildAt(i);
          ChildArray.Add(
              MakeShareable(new FJsonValueObject(WidgetToJson(Child, Depth + 1, MaxDepth))));
        }
        Info->SetArrayField(TEXT("children"), ChildArray);
      }
    }

    return Info;
  }

  // Search widget tree for widgets matching a class name.
  static void FindWidgetsByClass(const TSharedRef<SWidget>& Widget, const FString& ClassName,
                                 TArray<TSharedPtr<FJsonValue>>& OutResults,
                                 int32 MaxResults = 100) {
    if (OutResults.Num() >= MaxResults) return;

    if (Widget->GetType().ToString().Contains(ClassName)) {
      OutResults.Add(MakeShareable(new FJsonValueObject(WidgetToJson(Widget, 0, 1))));
    }

    FChildren* Children = const_cast<FChildren*>(Widget->GetChildren());
    if (Children) {
      for (int32 i = 0; i < Children->Num(); ++i) {
        FindWidgetsByClass(Children->GetChildAt(i), ClassName, OutResults, MaxResults);
      }
    }
  }

  // ---------------------------------------------------------------------------
  // POST /api/ui/query
  // ---------------------------------------------------------------------------

  static bool HandleUIQuery(const FHttpServerRequest& Request,
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

    // --- tree ---
    if (Operation == TEXT("tree")) {
      int32 MaxDepth = static_cast<int32>(Body->GetNumberField(TEXT("max_depth")));

      TArray<TSharedPtr<FJsonValue>> WidgetsArray;
      TArray<TSharedRef<SWindow>> Windows;
      FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);

      int32 TotalCount = 0;
      for (const TSharedRef<SWindow>& Window : Windows) {
        WidgetsArray.Add(MakeShareable(new FJsonValueObject(WidgetToJson(Window, 0, MaxDepth))));
        TotalCount++;
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetArrayField(TEXT("widgets"), WidgetsArray);
      ResponseJson->SetNumberField(TEXT("count"), TotalCount);

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- find ---
    if (Operation == TEXT("find")) {
      const FString ClassName = Body->GetStringField(TEXT("class"));
      if (ClassName.IsEmpty()) {
        SendError(OnComplete, TEXT("class is required for find"));
        return true;
      }

      TArray<TSharedPtr<FJsonValue>> Results;
      TArray<TSharedRef<SWindow>> Windows;
      FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);

      for (const TSharedRef<SWindow>& Window : Windows) {
        FindWidgetsByClass(Window, ClassName, Results);
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetArrayField(TEXT("widgets"), Results);
      ResponseJson->SetNumberField(TEXT("count"), Results.Num());

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- umg_list ---
    if (Operation == TEXT("umg_list")) {
      UWorld* World = GetWorld(Body);
      TArray<TSharedPtr<FJsonValue>> WidgetsArray;

      if (World) {
        for (TActorIterator<AActor> It(World); It; ++It) {
          AActor* Actor = *It;
          if (!Actor || Actor->IsPendingKillPending()) continue;

          TArray<UWidgetComponent*> WidgetComps;
          Actor->GetComponents(WidgetComps);

          for (UWidgetComponent* WC : WidgetComps) {
            if (!WC) continue;
            TSharedPtr<FJsonObject> Info = MakeShareable(new FJsonObject());
            Info->SetStringField(TEXT("type"), TEXT("UWidgetComponent"));
            Info->SetStringField(TEXT("name"), WC->GetName());
            Info->SetBoolField(TEXT("visible"), WC->IsVisible());
            Info->SetBoolField(TEXT("enabled"), WC->IsActive());

            UUserWidget* UserWidget = WC->GetWidget();
            if (UserWidget) {
              Info->SetStringField(TEXT("widget_class"), UserWidget->GetClass()->GetName());
            }

            WidgetsArray.Add(MakeShareable(new FJsonValueObject(Info)));
          }
        }
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetArrayField(TEXT("widgets"), WidgetsArray);
      ResponseJson->SetNumberField(TEXT("count"), WidgetsArray.Num());

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    SendError(OnComplete, FString::Printf(TEXT("Unknown UI query operation: %s"), *Operation));
    return true;
  }

  // ---------------------------------------------------------------------------
  // Registration
  // ---------------------------------------------------------------------------

  void RegisterUIQueryRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles) {
    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/ui/query")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleUIQuery)));

    UE_LOG(LogMCPUnreal, Verbose, TEXT("Registered UI query routes (1 endpoint)"));
  }

}  // namespace MCPUnreal
