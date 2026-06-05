// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// ---------------------------------------------------------------------------
// Test 1: Parse empty body — parse "{}", verify valid empty object
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPParseEmptyBody, "MCPUnreal.JsonParsing.ParseEmptyBody",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPParseEmptyBody::RunTest(const FString& Parameters) {
  FString EmptyJson = TEXT("{}");
  TSharedPtr<FJsonObject> JsonObject;
  TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(EmptyJson);

  TestTrue(TEXT("Parse empty JSON object"), FJsonSerializer::Deserialize(Reader, JsonObject));
  TestTrue(TEXT("JsonObject is valid"), JsonObject.IsValid());
  TestEqual(TEXT("No fields"), JsonObject->Values.Num(), 0);

  return true;
}

// ---------------------------------------------------------------------------
// Test 2: Parse valid body — verify field values from a realistic request
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPParseValidBody, "MCPUnreal.JsonParsing.ParseValidBody",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPParseValidBody::RunTest(const FString& Parameters) {
  FString Json = TEXT("{\"operation\":\"list\",\"blueprint_path\":\"/Game/BP_Test\"}");
  TSharedPtr<FJsonObject> JsonObject;
  TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);

  TestTrue(TEXT("Deserialize succeeds"), FJsonSerializer::Deserialize(Reader, JsonObject));
  TestTrue(TEXT("JsonObject is valid"), JsonObject.IsValid());

  FString Operation;
  TestTrue(TEXT("Has operation field"),
           JsonObject->TryGetStringField(TEXT("operation"), Operation));
  TestEqual(TEXT("operation == list"), Operation, FString(TEXT("list")));

  FString BlueprintPath;
  TestTrue(TEXT("Has blueprint_path field"),
           JsonObject->TryGetStringField(TEXT("blueprint_path"), BlueprintPath));
  TestEqual(TEXT("blueprint_path value"), BlueprintPath, FString(TEXT("/Game/BP_Test")));

  return true;
}

// ---------------------------------------------------------------------------
// Test 3: Parse invalid body — malformed JSON must fail
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPParseInvalidBody, "MCPUnreal.JsonParsing.ParseInvalidBody",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPParseInvalidBody::RunTest(const FString& Parameters) {
  FString MalformedJson = TEXT("{\"operation\": \"list\", broken}");
  TSharedPtr<FJsonObject> JsonObject;
  TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(MalformedJson);

  bool bSuccess = FJsonSerializer::Deserialize(Reader, JsonObject);
  TestFalse(TEXT("Malformed JSON should fail to parse"), bSuccess);

  return true;
}

// ---------------------------------------------------------------------------
// Test 4: JSON to string — serialize a JsonObject, verify output
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPJsonToString, "MCPUnreal.JsonParsing.JsonToString",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPJsonToString::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> JsonObject = MakeShared<FJsonObject>();
  JsonObject->SetStringField(TEXT("name"), TEXT("TestActor"));
  JsonObject->SetNumberField(TEXT("count"), 42);

  FString OutputString;
  TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
      TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutputString);
  FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);

  TestTrue(TEXT("Output contains name field"), OutputString.Contains(TEXT("\"name\"")));
  TestTrue(TEXT("Output contains TestActor value"), OutputString.Contains(TEXT("\"TestActor\"")));
  TestTrue(TEXT("Output contains count field"), OutputString.Contains(TEXT("\"count\"")));
  TestTrue(TEXT("Output contains 42"), OutputString.Contains(TEXT("42")));

  return true;
}

// ---------------------------------------------------------------------------
// Test 5: JSON array to string — serialize a JsonArray, verify output
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPJsonArrayToString, "MCPUnreal.JsonParsing.JsonArrayToString",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPJsonArrayToString::RunTest(const FString& Parameters) {
  TArray<TSharedPtr<FJsonValue>> JsonArray;
  JsonArray.Add(MakeShared<FJsonValueString>(TEXT("alpha")));
  JsonArray.Add(MakeShared<FJsonValueString>(TEXT("beta")));
  JsonArray.Add(MakeShared<FJsonValueString>(TEXT("gamma")));

  FString OutputString;
  TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
      TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutputString);
  FJsonSerializer::Serialize(JsonArray, Writer);

  TestTrue(TEXT("Output contains alpha"), OutputString.Contains(TEXT("\"alpha\"")));
  TestTrue(TEXT("Output contains beta"), OutputString.Contains(TEXT("\"beta\"")));
  TestTrue(TEXT("Output contains gamma"), OutputString.Contains(TEXT("\"gamma\"")));
  TestTrue(TEXT("Output starts with ["), OutputString.StartsWith(TEXT("[")));
  TestTrue(TEXT("Output ends with ]"), OutputString.EndsWith(TEXT("]")));

  return true;
}
