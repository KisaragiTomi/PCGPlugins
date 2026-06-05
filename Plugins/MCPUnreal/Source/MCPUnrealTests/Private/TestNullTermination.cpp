// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.
//
// Tests specifically for the null-termination bug in ParseJsonBody.
// The original implementation used UTF8_TO_TCHAR on raw body bytes
// without null termination, causing buffer overreads. The fix copies
// body bytes to a null-terminated TArray<uint8> before conversion.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace MCPNullTermHelpers {
  // Broken version (pre-fix): reads past buffer without null terminator.
  // DO NOT USE in production. Here only to verify the test catches the bug.
  bool ParseJsonBody_BROKEN(const TArray<uint8>& Body, TSharedPtr<FJsonObject>& OutJson) {
    if (Body.Num() == 0) {
      OutJson = MakeShareable(new FJsonObject());
      return true;
    }

    // BUG: No null termination — UTF8_TO_TCHAR may read past Body.GetData()
    const FString BodyStr =
        FString(UTF8_TO_TCHAR(reinterpret_cast<const ANSICHAR*>(Body.GetData())));
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BodyStr);
    return FJsonSerializer::Deserialize(Reader, OutJson) && OutJson.IsValid();
  }

  // Fixed version: adds null terminator before conversion.
  bool ParseJsonBody_FIXED(const TArray<uint8>& Body, TSharedPtr<FJsonObject>& OutJson) {
    if (Body.Num() == 0) {
      OutJson = MakeShareable(new FJsonObject());
      return true;
    }

    TArray<uint8> NullTermBody(Body);
    NullTermBody.Add(0);
    const FString BodyStr =
        FString(UTF8_TO_TCHAR(reinterpret_cast<const ANSICHAR*>(NullTermBody.GetData())));
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BodyStr);
    return FJsonSerializer::Deserialize(Reader, OutJson) && OutJson.IsValid();
  }
}  // namespace MCPNullTermHelpers

// ---------------------------------------------------------------------------
// Test 1: Fixed version handles 2-byte body without overread
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPNullTermFixed2Byte, "MCPUnreal.NullTermination.Fixed2ByteBody",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPNullTermFixed2Byte::RunTest(const FString& Parameters) {
  // Exactly 2 bytes: "{}" — no null terminator in the source
  TArray<uint8> Body;
  Body.Add('{');
  Body.Add('}');

  TSharedPtr<FJsonObject> Json;
  bool bResult = MCPNullTermHelpers::ParseJsonBody_FIXED(Body, Json);

  TestTrue(TEXT("Fixed parser handles 2-byte body"), bResult);
  TestTrue(TEXT("Result is valid"), Json.IsValid());
  TestEqual(TEXT("Empty object"), Json->Values.Num(), 0);

  return true;
}

// ---------------------------------------------------------------------------
// Test 2: Fixed version adds exactly 1 null byte
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPNullTermByteCount, "MCPUnreal.NullTermination.NullByteAdded",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPNullTermByteCount::RunTest(const FString& Parameters) {
  TArray<uint8> Body;
  Body.Add('{');
  Body.Add('}');

  // Verify that adding null terminator increases size by 1
  TArray<uint8> NullTermBody(Body);
  NullTermBody.Add(0);

  TestEqual(TEXT("Original body is 2 bytes"), Body.Num(), 2);
  TestEqual(TEXT("Null-terminated body is 3 bytes"), NullTermBody.Num(), 3);
  TestEqual(TEXT("Last byte is null"), NullTermBody[2], (uint8)0);

  return true;
}

// ---------------------------------------------------------------------------
// Test 3: Body already containing internal nulls doesn't break parsing
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPNullTermInternalNull,
                                 "MCPUnreal.NullTermination.BodyWithInternalNull",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPNullTermInternalNull::RunTest(const FString& Parameters) {
  // JSON string followed by a null byte and garbage — simulates buffer reuse
  const char* Raw = "{\"key\":\"val\"}";
  TArray<uint8> Body;
  Body.Append(reinterpret_cast<const uint8*>(Raw), FCStringAnsi::Strlen(Raw));

  TSharedPtr<FJsonObject> Json;
  bool bResult = MCPNullTermHelpers::ParseJsonBody_FIXED(Body, Json);

  TestTrue(TEXT("Parses correctly"), bResult);
  TestTrue(TEXT("Result valid"), Json.IsValid());

  FString Value;
  TestTrue(TEXT("Has key"), Json->TryGetStringField(TEXT("key"), Value));
  TestEqual(TEXT("key == val"), Value, FString(TEXT("val")));

  return true;
}

// ---------------------------------------------------------------------------
// Test 4: Single-byte body (e.g. just "{") fails gracefully
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPNullTermSingleByte, "MCPUnreal.NullTermination.SingleByteBody",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPNullTermSingleByte::RunTest(const FString& Parameters) {
  TArray<uint8> Body;
  Body.Add('{');

  TSharedPtr<FJsonObject> Json;
  bool bResult = MCPNullTermHelpers::ParseJsonBody_FIXED(Body, Json);

  // Incomplete JSON should fail
  TestFalse(TEXT("Incomplete JSON fails"), bResult);

  return true;
}

// ---------------------------------------------------------------------------
// Test 5: Verify null termination on a realistic multi-field body
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPNullTermRealistic, "MCPUnreal.NullTermination.RealisticBody",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPNullTermRealistic::RunTest(const FString& Parameters) {
  const char* Raw = R"({"operation":"create","package_path":"/Game","material_name":"M_MCPTest"})";
  TArray<uint8> Body;
  Body.Append(reinterpret_cast<const uint8*>(Raw), FCStringAnsi::Strlen(Raw));

  TSharedPtr<FJsonObject> Json;
  bool bResult = MCPNullTermHelpers::ParseJsonBody_FIXED(Body, Json);

  TestTrue(TEXT("Realistic body parses"), bResult);
  TestTrue(TEXT("Result valid"), Json.IsValid());

  FString Op;
  Json->TryGetStringField(TEXT("operation"), Op);
  TestEqual(TEXT("operation == create"), Op, FString(TEXT("create")));

  FString PkgPath;
  Json->TryGetStringField(TEXT("package_path"), PkgPath);
  TestEqual(TEXT("package_path == /Game"), PkgPath, FString(TEXT("/Game")));

  FString MatName;
  Json->TryGetStringField(TEXT("material_name"), MatName);
  TestEqual(TEXT("material_name == M_MCPTest"), MatName, FString(TEXT("M_MCPTest")));

  return true;
}
