// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.
//
// Tests for MCPUnreal::ParseJsonBody covering the null-termination fix
// and various body payloads discovered during manual integration testing.

#include "Misc/AutomationTest.h"
#include "HttpServerRequest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Re-implement ParseJsonBody logic for unit testing (MCPUnrealUtils.h is
// editor-only; the test module can't include it directly because of the
// GEditor dependency). This mirrors the production code exactly.
namespace MCPTestHelpers {
  bool ParseJsonBody(const TArray<uint8>& Body, TSharedPtr<FJsonObject>& OutJson) {
    if (Body.Num() == 0) {
      OutJson = MakeShareable(new FJsonObject());
      return true;
    }

    // Null-terminate the body bytes for safe UTF-8 -> TCHAR conversion.
    TArray<uint8> NullTermBody(Body);
    NullTermBody.Add(0);
    const FString BodyStr =
        FString(UTF8_TO_TCHAR(reinterpret_cast<const ANSICHAR*>(NullTermBody.GetData())));
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BodyStr);
    return FJsonSerializer::Deserialize(Reader, OutJson) && OutJson.IsValid();
  }
}  // namespace MCPTestHelpers

// ---------------------------------------------------------------------------
// Test 1: Empty body returns valid empty FJsonObject
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPParseBodyEmpty, "MCPUnreal.ParseJsonBody.EmptyBody",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPParseBodyEmpty::RunTest(const FString& Parameters) {
  TArray<uint8> Body;
  TSharedPtr<FJsonObject> Json;

  TestTrue(TEXT("Empty body parses successfully"), MCPTestHelpers::ParseJsonBody(Body, Json));
  TestTrue(TEXT("Result is valid"), Json.IsValid());
  TestEqual(TEXT("No fields in empty body"), Json->Values.Num(), 0);

  return true;
}

// ---------------------------------------------------------------------------
// Test 2: Short body "{}" — the exact payload that triggered the original
// null-termination buffer overread bug.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPParseBodyShort, "MCPUnreal.ParseJsonBody.ShortBody",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPParseBodyShort::RunTest(const FString& Parameters) {
  const char* Raw = "{}";
  TArray<uint8> Body;
  Body.Append(reinterpret_cast<const uint8*>(Raw), 2);  // exactly 2 bytes, no null

  TSharedPtr<FJsonObject> Json;
  TestTrue(TEXT("Short body '{}' parses successfully"), MCPTestHelpers::ParseJsonBody(Body, Json));
  TestTrue(TEXT("Result is valid"), Json.IsValid());
  TestEqual(TEXT("No fields"), Json->Values.Num(), 0);

  return true;
}

// ---------------------------------------------------------------------------
// Test 3: Realistic actor spawn request body
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPParseBodySpawnActor, "MCPUnreal.ParseJsonBody.SpawnActorBody",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPParseBodySpawnActor::RunTest(const FString& Parameters) {
  const char* Raw =
      R"({"class_name":"PointLight","name":"MCP_TestLight","location":[0,0,300],"scale":[1,1,1]})";
  TArray<uint8> Body;
  Body.Append(reinterpret_cast<const uint8*>(Raw), FCStringAnsi::Strlen(Raw));

  TSharedPtr<FJsonObject> Json;
  TestTrue(TEXT("Spawn body parses"), MCPTestHelpers::ParseJsonBody(Body, Json));
  TestTrue(TEXT("Result is valid"), Json.IsValid());

  FString ClassName;
  TestTrue(TEXT("Has class_name"), Json->TryGetStringField(TEXT("class_name"), ClassName));
  TestEqual(TEXT("class_name == PointLight"), ClassName, FString(TEXT("PointLight")));

  FString Name;
  TestTrue(TEXT("Has name"), Json->TryGetStringField(TEXT("name"), Name));
  TestEqual(TEXT("name == MCP_TestLight"), Name, FString(TEXT("MCP_TestLight")));

  const TArray<TSharedPtr<FJsonValue>>* Location;
  TestTrue(TEXT("Has location array"), Json->TryGetArrayField(TEXT("location"), Location));
  TestEqual(TEXT("Location has 3 elements"), Location->Num(), 3);
  TestEqual(TEXT("Location Z == 300"), (*Location)[2]->AsNumber(), 300.0);

  return true;
}

// ---------------------------------------------------------------------------
// Test 4: Material ops request body with operation dispatch
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPParseBodyMaterialOps,
                                 "MCPUnreal.ParseJsonBody.MaterialOpsBody",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPParseBodyMaterialOps::RunTest(const FString& Parameters) {
  const char* Raw =
      R"({"operation":"get_parameters","material_path":"/Engine/BasicShapes/BasicShapeMaterial"})";
  TArray<uint8> Body;
  Body.Append(reinterpret_cast<const uint8*>(Raw), FCStringAnsi::Strlen(Raw));

  TSharedPtr<FJsonObject> Json;
  TestTrue(TEXT("Material ops body parses"), MCPTestHelpers::ParseJsonBody(Body, Json));

  FString Operation;
  TestTrue(TEXT("Has operation"), Json->TryGetStringField(TEXT("operation"), Operation));
  TestEqual(TEXT("operation == get_parameters"), Operation, FString(TEXT("get_parameters")));

  FString MaterialPath;
  TestTrue(TEXT("Has material_path"), Json->TryGetStringField(TEXT("material_path"), MaterialPath));
  TestTrue(TEXT("material_path starts with /Engine"), MaterialPath.StartsWith(TEXT("/Engine")));

  return true;
}

// ---------------------------------------------------------------------------
// Test 5: Level ops request body
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPParseBodyLevelOps, "MCPUnreal.ParseJsonBody.LevelOpsBody",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPParseBodyLevelOps::RunTest(const FString& Parameters) {
  const char* Raw = R"({"operation":"get_current"})";
  TArray<uint8> Body;
  Body.Append(reinterpret_cast<const uint8*>(Raw), FCStringAnsi::Strlen(Raw));

  TSharedPtr<FJsonObject> Json;
  TestTrue(TEXT("Level ops body parses"), MCPTestHelpers::ParseJsonBody(Body, Json));

  FString Operation;
  TestTrue(TEXT("Has operation"), Json->TryGetStringField(TEXT("operation"), Operation));
  TestEqual(TEXT("operation == get_current"), Operation, FString(TEXT("get_current")));

  return true;
}

// ---------------------------------------------------------------------------
// Test 6: Delete actors body with actor_names array
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPParseBodyDeleteActors,
                                 "MCPUnreal.ParseJsonBody.DeleteActorsBody",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPParseBodyDeleteActors::RunTest(const FString& Parameters) {
  const char* Raw = R"({"actor_names":["MCP_TestLight","MCP_MoveTest"]})";
  TArray<uint8> Body;
  Body.Append(reinterpret_cast<const uint8*>(Raw), FCStringAnsi::Strlen(Raw));

  TSharedPtr<FJsonObject> Json;
  TestTrue(TEXT("Delete body parses"), MCPTestHelpers::ParseJsonBody(Body, Json));

  const TArray<TSharedPtr<FJsonValue>>* Names;
  TestTrue(TEXT("Has actor_names"), Json->TryGetArrayField(TEXT("actor_names"), Names));
  TestEqual(TEXT("Two names"), Names->Num(), 2);
  TestEqual(TEXT("First name"), (*Names)[0]->AsString(), FString(TEXT("MCP_TestLight")));
  TestEqual(TEXT("Second name"), (*Names)[1]->AsString(), FString(TEXT("MCP_MoveTest")));

  return true;
}

// ---------------------------------------------------------------------------
// Test 7: Malformed JSON body returns false
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPParseBodyMalformed, "MCPUnreal.ParseJsonBody.MalformedBody",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPParseBodyMalformed::RunTest(const FString& Parameters) {
  const char* Raw = "{broken json content!}";
  TArray<uint8> Body;
  Body.Append(reinterpret_cast<const uint8*>(Raw), FCStringAnsi::Strlen(Raw));

  TSharedPtr<FJsonObject> Json;
  TestFalse(TEXT("Malformed body fails to parse"), MCPTestHelpers::ParseJsonBody(Body, Json));

  return true;
}

// ---------------------------------------------------------------------------
// Test 8: Body with Unicode characters (Blueprint paths can contain these)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPParseBodyUnicode, "MCPUnreal.ParseJsonBody.UnicodeBody",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPParseBodyUnicode::RunTest(const FString& Parameters) {
  const char* Raw = R"({"name":"Test_\u00e9\u00e0\u00fc","value":42})";
  TArray<uint8> Body;
  Body.Append(reinterpret_cast<const uint8*>(Raw), FCStringAnsi::Strlen(Raw));

  TSharedPtr<FJsonObject> Json;
  TestTrue(TEXT("Unicode body parses"), MCPTestHelpers::ParseJsonBody(Body, Json));
  TestTrue(TEXT("Has name field"), Json->HasField(TEXT("name")));
  TestEqual(TEXT("value == 42"), Json->GetNumberField(TEXT("value")), 42.0);

  return true;
}

// ---------------------------------------------------------------------------
// Test 9: Large body (simulate a Blueprint modify with many nodes)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPParseBodyLarge, "MCPUnreal.ParseJsonBody.LargeBody",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPParseBodyLarge::RunTest(const FString& Parameters) {
  // Build a JSON body with 100 vertex entries.
  FString JsonStr = TEXT("{\"vertices\":[");
  for (int32 i = 0; i < 100; i++) {
    if (i > 0) JsonStr += TEXT(",");
    JsonStr += FString::Printf(TEXT("[%d,%d,%d]"), i, i * 2, i * 3);
  }
  JsonStr += TEXT("]}");

  FTCHARToUTF8 Utf8(*JsonStr);
  TArray<uint8> Body;
  Body.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());

  TSharedPtr<FJsonObject> Json;
  TestTrue(TEXT("Large body parses"), MCPTestHelpers::ParseJsonBody(Body, Json));

  const TArray<TSharedPtr<FJsonValue>>* Vertices;
  TestTrue(TEXT("Has vertices"), Json->TryGetArrayField(TEXT("vertices"), Vertices));
  TestEqual(TEXT("100 vertices"), Vertices->Num(), 100);

  return true;
}
