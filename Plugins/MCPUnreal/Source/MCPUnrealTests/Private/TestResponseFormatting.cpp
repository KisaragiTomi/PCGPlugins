// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

// ---------------------------------------------------------------------------
// Test 1: JSON object format — create response with success/message fields
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPJsonObjectFormat,
                                 "MCPUnreal.ResponseFormatting.JsonObjectFormat",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPJsonObjectFormat::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
  Response->SetBoolField(TEXT("success"), true);
  Response->SetStringField(TEXT("message"), TEXT("Actor spawned successfully"));

  FString OutputString;
  TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
      TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutputString);
  FJsonSerializer::Serialize(Response.ToSharedRef(), Writer);

  TestTrue(TEXT("Contains success field"), OutputString.Contains(TEXT("\"success\"")));
  TestTrue(TEXT("Contains true value"), OutputString.Contains(TEXT("true")));
  TestTrue(TEXT("Contains message field"), OutputString.Contains(TEXT("\"message\"")));
  TestTrue(TEXT("Contains message value"),
           OutputString.Contains(TEXT("Actor spawned successfully")));
  TestTrue(TEXT("Starts with {"), OutputString.StartsWith(TEXT("{")));
  TestTrue(TEXT("Ends with }"), OutputString.EndsWith(TEXT("}")));

  return true;
}

// ---------------------------------------------------------------------------
// Test 2: JSON array format — create array response and verify
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPJsonArrayFormat,
                                 "MCPUnreal.ResponseFormatting.JsonArrayFormat",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPJsonArrayFormat::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Actor1 = MakeShared<FJsonObject>();
  Actor1->SetStringField(TEXT("name"), TEXT("StaticMeshActor_0"));
  Actor1->SetStringField(TEXT("class"), TEXT("StaticMeshActor"));

  TSharedPtr<FJsonObject> Actor2 = MakeShared<FJsonObject>();
  Actor2->SetStringField(TEXT("name"), TEXT("PointLight_0"));
  Actor2->SetStringField(TEXT("class"), TEXT("PointLight"));

  TArray<TSharedPtr<FJsonValue>> ActorArray;
  ActorArray.Add(MakeShared<FJsonValueObject>(Actor1));
  ActorArray.Add(MakeShared<FJsonValueObject>(Actor2));

  FString OutputString;
  TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
      TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutputString);
  FJsonSerializer::Serialize(ActorArray, Writer);

  TestTrue(TEXT("Contains StaticMeshActor_0"), OutputString.Contains(TEXT("StaticMeshActor_0")));
  TestTrue(TEXT("Contains PointLight_0"), OutputString.Contains(TEXT("PointLight_0")));
  TestTrue(TEXT("Starts with ["), OutputString.StartsWith(TEXT("[")));
  TestTrue(TEXT("Ends with ]"), OutputString.EndsWith(TEXT("]")));

  return true;
}

// ---------------------------------------------------------------------------
// Test 3: Empty array format — serialize empty array to "[]"
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPEmptyArrayFormat,
                                 "MCPUnreal.ResponseFormatting.EmptyArrayFormat",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPEmptyArrayFormat::RunTest(const FString& Parameters) {
  TArray<TSharedPtr<FJsonValue>> EmptyArray;

  FString OutputString;
  TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
      TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutputString);
  FJsonSerializer::Serialize(EmptyArray, Writer);

  TestEqual(TEXT("Empty array serializes to []"), OutputString, FString(TEXT("[]")));

  return true;
}

// ---------------------------------------------------------------------------
// Test 4: Error format — create error response, verify error message
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPErrorFormat, "MCPUnreal.ResponseFormatting.ErrorFormat",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPErrorFormat::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> ErrorResponse = MakeShared<FJsonObject>();
  ErrorResponse->SetBoolField(TEXT("success"), false);
  ErrorResponse->SetStringField(
      TEXT("error"),
      TEXT("Actor not found: /Game/Maps/TestMap.TestMap:PersistentLevel.MissingActor"));

  FString OutputString;
  TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
      TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutputString);
  FJsonSerializer::Serialize(ErrorResponse.ToSharedRef(), Writer);

  TestTrue(TEXT("Contains success false"), OutputString.Contains(TEXT("\"success\":false")));
  TestTrue(TEXT("Contains error field"), OutputString.Contains(TEXT("\"error\"")));
  TestTrue(TEXT("Contains error message"), OutputString.Contains(TEXT("Actor not found")));

  return true;
}
