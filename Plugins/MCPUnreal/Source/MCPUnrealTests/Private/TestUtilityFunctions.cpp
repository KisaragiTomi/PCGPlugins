// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.
//
// Tests for MCPUnreal utility functions in MCPUnrealUtils.h:
// JsonToString, JsonArrayToString, SendJson, SendError, SendJsonArray.

#include "MCPUnrealUtils.h"
#include "Misc/AutomationTest.h"

// ---------------------------------------------------------------------------
// JsonToString tests
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPUtilJsonToStringBasic,
                                 "MCPUnreal.Utilities.JsonToString.Basic",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPUtilJsonToStringBasic::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
  Obj->SetBoolField(TEXT("success"), true);
  Obj->SetStringField(TEXT("name"), TEXT("TestActor"));
  Obj->SetNumberField(TEXT("count"), 42);

  FString Result = MCPUnreal::JsonToString(Obj);

  TestTrue(TEXT("Contains success"), Result.Contains(TEXT("\"success\"")));
  TestTrue(TEXT("Contains name"), Result.Contains(TEXT("\"TestActor\"")));
  TestTrue(TEXT("Contains count"), Result.Contains(TEXT("42")));
  TestFalse(TEXT("Not empty"), Result.IsEmpty());

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPUtilJsonToStringEmpty,
                                 "MCPUnreal.Utilities.JsonToString.EmptyObject",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPUtilJsonToStringEmpty::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
  FString Result = MCPUnreal::JsonToString(Obj);

  TestFalse(TEXT("Not empty string"), Result.IsEmpty());
  // Empty object should produce something like "{}" or "{\n}"
  TestTrue(TEXT("Contains opening brace"), Result.Contains(TEXT("{")));
  TestTrue(TEXT("Contains closing brace"), Result.Contains(TEXT("}")));

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPUtilJsonToStringNested,
                                 "MCPUnreal.Utilities.JsonToString.NestedObject",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPUtilJsonToStringNested::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Inner = MakeShared<FJsonObject>();
  Inner->SetNumberField(TEXT("x"), 100.0);
  Inner->SetNumberField(TEXT("y"), 200.0);
  Inner->SetNumberField(TEXT("z"), 300.0);

  TSharedPtr<FJsonObject> Outer = MakeShared<FJsonObject>();
  Outer->SetObjectField(TEXT("location"), Inner);
  Outer->SetStringField(TEXT("actor"), TEXT("Cube_1"));

  FString Result = MCPUnreal::JsonToString(Outer);

  TestTrue(TEXT("Contains location"), Result.Contains(TEXT("\"location\"")));
  TestTrue(TEXT("Contains x"), Result.Contains(TEXT("100")));
  TestTrue(TEXT("Contains actor"), Result.Contains(TEXT("\"Cube_1\"")));

  return true;
}

// ---------------------------------------------------------------------------
// JsonArrayToString tests
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPUtilJsonArrayToStringBasic,
                                 "MCPUnreal.Utilities.JsonArrayToString.Basic",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPUtilJsonArrayToStringBasic::RunTest(const FString& Parameters) {
  TArray<TSharedPtr<FJsonValue>> Array;
  Array.Add(MakeShared<FJsonValueString>(TEXT("alpha")));
  Array.Add(MakeShared<FJsonValueString>(TEXT("beta")));

  FString Result = MCPUnreal::JsonArrayToString(Array);

  TestTrue(TEXT("Contains alpha"), Result.Contains(TEXT("alpha")));
  TestTrue(TEXT("Contains beta"), Result.Contains(TEXT("beta")));
  TestTrue(TEXT("Starts with ["), Result.TrimStartAndEnd().StartsWith(TEXT("[")));
  TestTrue(TEXT("Ends with ]"), Result.TrimStartAndEnd().EndsWith(TEXT("]")));

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPUtilJsonArrayToStringEmpty,
                                 "MCPUnreal.Utilities.JsonArrayToString.Empty",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPUtilJsonArrayToStringEmpty::RunTest(const FString& Parameters) {
  TArray<TSharedPtr<FJsonValue>> Empty;

  FString Result = MCPUnreal::JsonArrayToString(Empty);

  TestFalse(TEXT("Not empty string"), Result.IsEmpty());
  TestTrue(TEXT("Starts with ["), Result.TrimStartAndEnd().StartsWith(TEXT("[")));
  TestTrue(TEXT("Ends with ]"), Result.TrimStartAndEnd().EndsWith(TEXT("]")));

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPUtilJsonArrayToStringObjects,
                                 "MCPUnreal.Utilities.JsonArrayToString.Objects",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPUtilJsonArrayToStringObjects::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
  A->SetStringField(TEXT("name"), TEXT("Actor_A"));

  TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>();
  B->SetStringField(TEXT("name"), TEXT("Actor_B"));

  TArray<TSharedPtr<FJsonValue>> Array;
  Array.Add(MakeShared<FJsonValueObject>(A));
  Array.Add(MakeShared<FJsonValueObject>(B));

  FString Result = MCPUnreal::JsonArrayToString(Array);

  TestTrue(TEXT("Contains Actor_A"), Result.Contains(TEXT("Actor_A")));
  TestTrue(TEXT("Contains Actor_B"), Result.Contains(TEXT("Actor_B")));

  return true;
}

// ---------------------------------------------------------------------------
// SendJson / SendError / SendJsonArray via callback capture
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPUtilSendJsonCallback,
                                 "MCPUnreal.Utilities.SendJson.CallbackCapture",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPUtilSendJsonCallback::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
  Obj->SetBoolField(TEXT("success"), true);
  Obj->SetStringField(TEXT("actor_path"), TEXT("/Game/Maps/Test.Test:PersistentLevel.Cube_0"));

  bool bCallbackInvoked = false;
  FHttpResultCallback Callback = [&bCallbackInvoked](TUniquePtr<FHttpServerResponse> Response) {
    bCallbackInvoked = true;
    // Response was created — if we get here, SendJson works.
  };

  MCPUnreal::SendJson(Callback, Obj);
  TestTrue(TEXT("Callback was invoked"), bCallbackInvoked);

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPUtilSendErrorCallback,
                                 "MCPUnreal.Utilities.SendError.CallbackCapture",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPUtilSendErrorCallback::RunTest(const FString& Parameters) {
  bool bCallbackInvoked = false;
  FHttpResultCallback Callback = [&bCallbackInvoked](TUniquePtr<FHttpServerResponse> Response) {
    bCallbackInvoked = true;
  };

  MCPUnreal::SendError(Callback, TEXT("Actor not found"), 404);
  TestTrue(TEXT("Error callback was invoked"), bCallbackInvoked);

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPUtilSendErrorDefaultStatus,
                                 "MCPUnreal.Utilities.SendError.DefaultStatus",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPUtilSendErrorDefaultStatus::RunTest(const FString& Parameters) {
  bool bCallbackInvoked = false;
  FHttpResultCallback Callback = [&bCallbackInvoked](TUniquePtr<FHttpServerResponse> Response) {
    bCallbackInvoked = true;
  };

  // Default status code (400)
  MCPUnreal::SendError(Callback, TEXT("Missing required field: operation"));
  TestTrue(TEXT("Error callback was invoked with default status"), bCallbackInvoked);

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPUtilSendJsonArrayCallback,
                                 "MCPUnreal.Utilities.SendJsonArray.CallbackCapture",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPUtilSendJsonArrayCallback::RunTest(const FString& Parameters) {
  TArray<TSharedPtr<FJsonValue>> Array;

  TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
  Item->SetStringField(TEXT("name"), TEXT("PointLight_0"));
  Array.Add(MakeShared<FJsonValueObject>(Item));

  bool bCallbackInvoked = false;
  FHttpResultCallback Callback = [&bCallbackInvoked](TUniquePtr<FHttpServerResponse> Response) {
    bCallbackInvoked = true;
  };

  MCPUnreal::SendJsonArray(Callback, Array);
  TestTrue(TEXT("Array callback was invoked"), bCallbackInvoked);

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPUtilSendJsonStringCallback,
                                 "MCPUnreal.Utilities.SendJsonString.CallbackCapture",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPUtilSendJsonStringCallback::RunTest(const FString& Parameters) {
  bool bCallbackInvoked = false;
  FHttpResultCallback Callback = [&bCallbackInvoked](TUniquePtr<FHttpServerResponse> Response) {
    bCallbackInvoked = true;
  };

  MCPUnreal::SendJsonString(Callback, TEXT("{\"raw\":true}"));
  TestTrue(TEXT("JsonString callback was invoked"), bCallbackInvoked);

  return true;
}
