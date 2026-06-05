// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// ---------------------------------------------------------------------------
// Test 1: Missing required field — body without "operation"
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPMissingRequiredField,
                                 "MCPUnreal.InputValidation.MissingRequiredField",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPMissingRequiredField::RunTest(const FString& Parameters) {
  FString Json = TEXT("{\"blueprint_path\":\"/Game/BP_Test\"}");
  TSharedPtr<FJsonObject> JsonObject;
  TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);

  TestTrue(TEXT("Deserialize succeeds"), FJsonSerializer::Deserialize(Reader, JsonObject));
  TestTrue(TEXT("JsonObject is valid"), JsonObject.IsValid());
  TestFalse(TEXT("operation field is missing"), JsonObject->HasField(TEXT("operation")));

  return true;
}

// ---------------------------------------------------------------------------
// Test 2: Array parsing — parse nested numeric arrays
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPArrayParsing, "MCPUnreal.InputValidation.ArrayParsing",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPArrayParsing::RunTest(const FString& Parameters) {
  FString Json = TEXT("{\"vertices\":[[1,2,3],[4,5,6]]}");
  TSharedPtr<FJsonObject> JsonObject;
  TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);

  TestTrue(TEXT("Deserialize succeeds"), FJsonSerializer::Deserialize(Reader, JsonObject));
  TestTrue(TEXT("JsonObject is valid"), JsonObject.IsValid());
  TestTrue(TEXT("Has vertices field"), JsonObject->HasField(TEXT("vertices")));

  const TArray<TSharedPtr<FJsonValue>>* VerticesArray;
  TestTrue(TEXT("vertices is array"),
           JsonObject->TryGetArrayField(TEXT("vertices"), VerticesArray));
  TestEqual(TEXT("Two vertices"), VerticesArray->Num(), 2);

  // Verify first vertex [1,2,3]
  const TArray<TSharedPtr<FJsonValue>>& FirstVertex = (*VerticesArray)[0]->AsArray();
  TestEqual(TEXT("First vertex has 3 components"), FirstVertex.Num(), 3);
  TestEqual(TEXT("First vertex X"), FirstVertex[0]->AsNumber(), 1.0);
  TestEqual(TEXT("First vertex Y"), FirstVertex[1]->AsNumber(), 2.0);
  TestEqual(TEXT("First vertex Z"), FirstVertex[2]->AsNumber(), 3.0);

  // Verify second vertex [4,5,6]
  const TArray<TSharedPtr<FJsonValue>>& SecondVertex = (*VerticesArray)[1]->AsArray();
  TestEqual(TEXT("Second vertex has 3 components"), SecondVertex.Num(), 3);
  TestEqual(TEXT("Second vertex X"), SecondVertex[0]->AsNumber(), 4.0);
  TestEqual(TEXT("Second vertex Y"), SecondVertex[1]->AsNumber(), 5.0);
  TestEqual(TEXT("Second vertex Z"), SecondVertex[2]->AsNumber(), 6.0);

  return true;
}

// ---------------------------------------------------------------------------
// Test 3: Short array handling — array with fewer elements than expected
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPShortArrayHandling,
                                 "MCPUnreal.InputValidation.ShortArrayHandling",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPShortArrayHandling::RunTest(const FString& Parameters) {
  // Location array with only 2 elements instead of the expected 3
  FString Json = TEXT("{\"location\":[10,20]}");
  TSharedPtr<FJsonObject> JsonObject;
  TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);

  TestTrue(TEXT("Deserialize succeeds"), FJsonSerializer::Deserialize(Reader, JsonObject));
  TestTrue(TEXT("JsonObject is valid"), JsonObject.IsValid());

  const TArray<TSharedPtr<FJsonValue>>* LocationArray;
  TestTrue(TEXT("location is array"),
           JsonObject->TryGetArrayField(TEXT("location"), LocationArray));
  TestEqual(TEXT("Array has only 2 elements"), LocationArray->Num(), 2);

  // Code should check array length before accessing elements
  bool bHasEnoughElements = LocationArray->Num() >= 3;
  TestFalse(TEXT("Does not have 3 elements"), bHasEnoughElements);

  return true;
}

// ---------------------------------------------------------------------------
// Test 4: String array parsing — parse array of modifier strings
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPStringArrayParsing,
                                 "MCPUnreal.InputValidation.StringArrayParsing",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPStringArrayParsing::RunTest(const FString& Parameters) {
  FString Json = TEXT("{\"modifiers\":[\"Negate\",\"Swizzle\"]}");
  TSharedPtr<FJsonObject> JsonObject;
  TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);

  TestTrue(TEXT("Deserialize succeeds"), FJsonSerializer::Deserialize(Reader, JsonObject));
  TestTrue(TEXT("JsonObject is valid"), JsonObject.IsValid());

  const TArray<TSharedPtr<FJsonValue>>* ModifiersArray;
  TestTrue(TEXT("modifiers is array"),
           JsonObject->TryGetArrayField(TEXT("modifiers"), ModifiersArray));
  TestEqual(TEXT("Two modifiers"), ModifiersArray->Num(), 2);
  TestEqual(TEXT("First modifier is Negate"), (*ModifiersArray)[0]->AsString(),
            FString(TEXT("Negate")));
  TestEqual(TEXT("Second modifier is Swizzle"), (*ModifiersArray)[1]->AsString(),
            FString(TEXT("Swizzle")));

  return true;
}
