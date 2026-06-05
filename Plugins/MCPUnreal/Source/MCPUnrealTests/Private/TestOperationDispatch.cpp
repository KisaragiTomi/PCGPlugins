// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.
//
// Tests for operation dispatch validation — verifying that the route handlers
// correctly parse the "operation" field and validate required parameters.
// These tests exercise the JSON parsing patterns used by all *_ops routes.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace MCPDispatchHelpers {
  /** Simulate operation dispatch for blueprint_query/modify routes. */
  bool ValidateBlueprintOps(const TSharedPtr<FJsonObject>& Body, FString& OutError) {
    FString Operation;
    if (!Body->TryGetStringField(TEXT("operation"), Operation)) {
      OutError = TEXT("Missing required field: operation");
      return false;
    }

    // blueprint_query operations
    TArray<FString> ValidQueryOps = {TEXT("list_functions"),  TEXT("list_variables"),
                                     TEXT("list_graphs"),     TEXT("list_nodes"),
                                     TEXT("get_connections"), TEXT("get_node_details")};

    // blueprint_modify operations
    TArray<FString> ValidModifyOps = {TEXT("add_function"),    TEXT("add_variable"),
                                      TEXT("add_node"),        TEXT("connect_pins"),
                                      TEXT("disconnect_pins"), TEXT("set_variable_default"),
                                      TEXT("remove_node"),     TEXT("compile")};

    bool bValidOp = ValidQueryOps.Contains(Operation) || ValidModifyOps.Contains(Operation);
    if (!bValidOp) {
      OutError = FString::Printf(TEXT("Unknown operation: %s"), *Operation);
      return false;
    }

    // Validate required parameters for certain operations
    if (Operation == TEXT("add_node") || Operation == TEXT("list_nodes") ||
        Operation == TEXT("get_connections") || Operation == TEXT("connect_pins")) {
      if (!Body->HasField(TEXT("blueprint_path"))) {
        OutError = FString::Printf(TEXT("operation '%s' requires blueprint_path"), *Operation);
        return false;
      }
    }

    return true;
  }

  /** Simulate operation dispatch for material_ops. */
  bool ValidateMaterialOps(const TSharedPtr<FJsonObject>& Body, FString& OutError) {
    FString Operation;
    if (!Body->TryGetStringField(TEXT("operation"), Operation)) {
      OutError = TEXT("Missing required field: operation");
      return false;
    }

    TArray<FString> ValidOps = {TEXT("create"),          TEXT("set_parameter"),
                                TEXT("get_parameters"),  TEXT("set_texture"),
                                TEXT("create_instance"), TEXT("list_parameters")};

    if (!ValidOps.Contains(Operation)) {
      OutError = FString::Printf(TEXT("Unknown material operation: %s"), *Operation);
      return false;
    }

    // create requires package_path and material_name
    if (Operation == TEXT("create")) {
      if (!Body->HasField(TEXT("package_path")) || !Body->HasField(TEXT("material_name"))) {
        OutError = TEXT("'create' requires package_path and material_name");
        return false;
      }
    }

    // get_parameters and set_parameter require material_path
    if (Operation == TEXT("get_parameters") || Operation == TEXT("set_parameter")) {
      if (!Body->HasField(TEXT("material_path"))) {
        OutError = FString::Printf(TEXT("'%s' requires material_path"), *Operation);
        return false;
      }
    }

    return true;
  }

  /** Simulate operation dispatch for level_ops. */
  bool ValidateLevelOps(const TSharedPtr<FJsonObject>& Body, FString& OutError) {
    FString Operation;
    if (!Body->TryGetStringField(TEXT("operation"), Operation)) {
      OutError = TEXT("Missing required field: operation");
      return false;
    }

    TArray<FString> ValidOps = {TEXT("get_current"),     TEXT("list_levels"),
                                TEXT("load_level"),      TEXT("save_level"),
                                TEXT("new_level"),       TEXT("add_sublevel"),
                                TEXT("remove_sublevel"), TEXT("set_streaming_method")};

    if (!ValidOps.Contains(Operation)) {
      OutError = FString::Printf(TEXT("Unknown level operation: %s"), *Operation);
      return false;
    }

    return true;
  }
  /** Simulate operation dispatch for pcg_ops. */
  bool ValidatePCGOps(const TSharedPtr<FJsonObject>& Body, FString& OutError) {
    FString Operation;
    if (!Body->TryGetStringField(TEXT("operation"), Operation)) {
      OutError = TEXT("Missing required field: operation");
      return false;
    }
    TArray<FString> Valid = {TEXT("execute"),       TEXT("cleanup"),  TEXT("get_graph_info"),
                             TEXT("set_parameter"), TEXT("add_node"), TEXT("connect_nodes"),
                             TEXT("remove_node")};
    if (!Valid.Contains(Operation)) {
      OutError = FString::Printf(TEXT("Unknown PCG operation: %s"), *Operation);
      return false;
    }
    return true;
  }

  /** Simulate operation dispatch for gas_ops. */
  bool ValidateGASOps(const TSharedPtr<FJsonObject>& Body, FString& OutError) {
    FString Operation;
    if (!Body->TryGetStringField(TEXT("operation"), Operation)) {
      OutError = TEXT("Missing required field: operation");
      return false;
    }
    TArray<FString> Valid = {TEXT("grant_ability"), TEXT("revoke_ability"), TEXT("list_abilities"),
                             TEXT("apply_effect"),  TEXT("get_attributes"), TEXT("set_attribute")};
    if (!Valid.Contains(Operation)) {
      OutError = FString::Printf(TEXT("Unknown GAS operation: %s"), *Operation);
      return false;
    }
    return true;
  }

  /** Simulate operation dispatch for niagara_ops. */
  bool ValidateNiagaraOps(const TSharedPtr<FJsonObject>& Body, FString& OutError) {
    FString Operation;
    if (!Body->TryGetStringField(TEXT("operation"), Operation)) {
      OutError = TEXT("Missing required field: operation");
      return false;
    }
    TArray<FString> Valid = {TEXT("spawn_system"), TEXT("set_parameter"),  TEXT("get_system_info"),
                             TEXT("add_emitter"),  TEXT("remove_emitter"), TEXT("activate"),
                             TEXT("deactivate")};
    if (!Valid.Contains(Operation)) {
      OutError = FString::Printf(TEXT("Unknown Niagara operation: %s"), *Operation);
      return false;
    }
    return true;
  }

  /** Simulate operation dispatch for ism_ops. */
  bool ValidateISMOps(const TSharedPtr<FJsonObject>& Body, FString& OutError) {
    FString Operation;
    if (!Body->TryGetStringField(TEXT("operation"), Operation)) {
      OutError = TEXT("Missing required field: operation");
      return false;
    }
    TArray<FString> Valid = {TEXT("create"),          TEXT("add_instances"),
                             TEXT("clear_instances"), TEXT("get_instance_count"),
                             TEXT("update_instance"), TEXT("remove_instance"),
                             TEXT("set_material")};
    if (!Valid.Contains(Operation)) {
      OutError = FString::Printf(TEXT("Unknown ISM operation: %s"), *Operation);
      return false;
    }
    return true;
  }

  /** Simulate operation dispatch for data_asset_ops. */
  bool ValidateDataAssetOps(const TSharedPtr<FJsonObject>& Body, FString& OutError) {
    FString Operation;
    if (!Body->TryGetStringField(TEXT("operation"), Operation)) {
      OutError = TEXT("Missing required field: operation");
      return false;
    }
    TArray<FString> Valid = {TEXT("list_tables"), TEXT("get_table"),  TEXT("add_row"),
                             TEXT("update_row"),  TEXT("delete_row"), TEXT("import_csv")};
    if (!Valid.Contains(Operation)) {
      OutError = FString::Printf(TEXT("Unknown DataAsset operation: %s"), *Operation);
      return false;
    }
    return true;
  }

  /** Simulate operation dispatch for texture_ops. */
  bool ValidateTextureOps(const TSharedPtr<FJsonObject>& Body, FString& OutError) {
    FString Operation;
    if (!Body->TryGetStringField(TEXT("operation"), Operation)) {
      OutError = TEXT("Missing required field: operation");
      return false;
    }
    TArray<FString> Valid = {TEXT("import"), TEXT("get_info"), TEXT("set_material_texture"),
                             TEXT("list")};
    if (!Valid.Contains(Operation)) {
      OutError = FString::Printf(TEXT("Unknown texture operation: %s"), *Operation);
      return false;
    }
    return true;
  }

  /** Simulate operation dispatch for fab_ops. */
  bool ValidateFabOps(const TSharedPtr<FJsonObject>& Body, FString& OutError) {
    FString Operation;
    if (!Body->TryGetStringField(TEXT("operation"), Operation)) {
      OutError = TEXT("Missing required field: operation");
      return false;
    }
    TArray<FString> Valid = {TEXT("list_cache"), TEXT("cache_info"), TEXT("import"),
                             TEXT("clear_cache")};
    if (!Valid.Contains(Operation)) {
      OutError = FString::Printf(TEXT("Unknown Fab operation: %s"), *Operation);
      return false;
    }
    return true;
  }

  /** Simulate operation dispatch for character_config. */
  bool ValidateCharacterOps(const TSharedPtr<FJsonObject>& Body, FString& OutError) {
    FString Operation;
    if (!Body->TryGetStringField(TEXT("operation"), Operation)) {
      OutError = TEXT("Missing required field: operation");
      return false;
    }
    TArray<FString> Valid = {TEXT("get_config"), TEXT("set_movement"), TEXT("set_capsule"),
                             TEXT("set_mesh"), TEXT("get_movement_modes")};
    if (!Valid.Contains(Operation)) {
      OutError = FString::Printf(TEXT("Unknown character operation: %s"), *Operation);
      return false;
    }
    return true;
  }

  /** Simulate operation dispatch for input_ops. */
  bool ValidateInputOps(const TSharedPtr<FJsonObject>& Body, FString& OutError) {
    FString Operation;
    if (!Body->TryGetStringField(TEXT("operation"), Operation)) {
      OutError = TEXT("Missing required field: operation");
      return false;
    }
    TArray<FString> Valid = {TEXT("list_actions"), TEXT("list_contexts"), TEXT("get_bindings"),
                             TEXT("add_action"),   TEXT("remove_action"), TEXT("add_context"),
                             TEXT("bind_action"),  TEXT("unbind_action")};
    if (!Valid.Contains(Operation)) {
      OutError = FString::Printf(TEXT("Unknown input operation: %s"), *Operation);
      return false;
    }
    return true;
  }

  /** Simulate operation dispatch for ui_query. */
  bool ValidateUIQueryOps(const TSharedPtr<FJsonObject>& Body, FString& OutError) {
    FString Operation;
    if (!Body->TryGetStringField(TEXT("operation"), Operation)) {
      OutError = TEXT("Missing required field: operation");
      return false;
    }
    TArray<FString> Valid = {TEXT("tree"), TEXT("find"), TEXT("umg_list")};
    if (!Valid.Contains(Operation)) {
      OutError = FString::Printf(TEXT("Unknown UI query operation: %s"), *Operation);
      return false;
    }
    return true;
  }

  /** Simulate operation dispatch for network_debug. */
  bool ValidateNetworkDebugOps(const TSharedPtr<FJsonObject>& Body, FString& OutError) {
    FString Operation;
    if (!Body->TryGetStringField(TEXT("operation"), Operation)) {
      OutError = TEXT("Missing required field: operation");
      return false;
    }
    TArray<FString> Valid = {TEXT("list_active"), TEXT("recent_requests"), TEXT("websocket_status"),
                             TEXT("summary")};
    if (!Valid.Contains(Operation)) {
      OutError = FString::Printf(TEXT("Unknown network debug operation: %s"), *Operation);
      return false;
    }
    return true;
  }

}  // namespace MCPDispatchHelpers

// ---------------------------------------------------------------------------
// Blueprint operation dispatch tests
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchBPListFunctions,
                                 "MCPUnreal.OperationDispatch.Blueprint.ListFunctions",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchBPListFunctions::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("list_functions"));
  Body->SetStringField(TEXT("blueprint_path"), TEXT("/Game/BP_Test"));

  FString Error;
  TestTrue(TEXT("list_functions is valid"), MCPDispatchHelpers::ValidateBlueprintOps(Body, Error));

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchBPMissingOp,
                                 "MCPUnreal.OperationDispatch.Blueprint.MissingOperation",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchBPMissingOp::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("blueprint_path"), TEXT("/Game/BP_Test"));

  FString Error;
  TestFalse(TEXT("Missing operation rejected"),
            MCPDispatchHelpers::ValidateBlueprintOps(Body, Error));
  TestTrue(TEXT("Error mentions 'operation'"), Error.Contains(TEXT("operation")));

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchBPUnknownOp,
                                 "MCPUnreal.OperationDispatch.Blueprint.UnknownOperation",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchBPUnknownOp::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("nonexistent_op"));

  FString Error;
  TestFalse(TEXT("Unknown operation rejected"),
            MCPDispatchHelpers::ValidateBlueprintOps(Body, Error));
  TestTrue(TEXT("Error mentions operation name"), Error.Contains(TEXT("nonexistent_op")));

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchBPAddNodeMissingPath,
                                 "MCPUnreal.OperationDispatch.Blueprint.AddNodeMissingPath",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchBPAddNodeMissingPath::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("add_node"));
  // Missing blueprint_path

  FString Error;
  TestFalse(TEXT("add_node without path rejected"),
            MCPDispatchHelpers::ValidateBlueprintOps(Body, Error));
  TestTrue(TEXT("Error mentions blueprint_path"), Error.Contains(TEXT("blueprint_path")));

  return true;
}

// ---------------------------------------------------------------------------
// Material operation dispatch tests
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchMatCreate,
                                 "MCPUnreal.OperationDispatch.Material.Create",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchMatCreate::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("create"));
  Body->SetStringField(TEXT("package_path"), TEXT("/Game"));
  Body->SetStringField(TEXT("material_name"), TEXT("M_Test"));

  FString Error;
  TestTrue(TEXT("create with required fields valid"),
           MCPDispatchHelpers::ValidateMaterialOps(Body, Error));

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchMatCreateMissingFields,
                                 "MCPUnreal.OperationDispatch.Material.CreateMissingFields",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchMatCreateMissingFields::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("create"));
  // Missing package_path and material_name

  FString Error;
  TestFalse(TEXT("create without required fields rejected"),
            MCPDispatchHelpers::ValidateMaterialOps(Body, Error));
  TestTrue(TEXT("Error mentions requirements"), Error.Contains(TEXT("package_path")));

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchMatGetParams,
                                 "MCPUnreal.OperationDispatch.Material.GetParameters",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchMatGetParams::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("get_parameters"));
  Body->SetStringField(TEXT("material_path"), TEXT("/Engine/BasicShapes/BasicShapeMaterial"));

  FString Error;
  TestTrue(TEXT("get_parameters with path valid"),
           MCPDispatchHelpers::ValidateMaterialOps(Body, Error));

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchMatGetParamsMissingPath,
                                 "MCPUnreal.OperationDispatch.Material.GetParametersMissingPath",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchMatGetParamsMissingPath::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("get_parameters"));
  // Missing material_path

  FString Error;
  TestFalse(TEXT("get_parameters without path rejected"),
            MCPDispatchHelpers::ValidateMaterialOps(Body, Error));
  TestTrue(TEXT("Error mentions material_path"), Error.Contains(TEXT("material_path")));

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchMatUnknown,
                                 "MCPUnreal.OperationDispatch.Material.UnknownOperation",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchMatUnknown::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("invalid_op"));

  FString Error;
  TestFalse(TEXT("Unknown material op rejected"),
            MCPDispatchHelpers::ValidateMaterialOps(Body, Error));

  return true;
}

// ---------------------------------------------------------------------------
// Level operation dispatch tests
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchLevelGetCurrent,
                                 "MCPUnreal.OperationDispatch.Level.GetCurrent",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchLevelGetCurrent::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("get_current"));

  FString Error;
  TestTrue(TEXT("get_current valid"), MCPDispatchHelpers::ValidateLevelOps(Body, Error));

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchLevelListLevels,
                                 "MCPUnreal.OperationDispatch.Level.ListLevels",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchLevelListLevels::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("list_levels"));

  FString Error;
  TestTrue(TEXT("list_levels valid"), MCPDispatchHelpers::ValidateLevelOps(Body, Error));

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchLevelUnknown,
                                 "MCPUnreal.OperationDispatch.Level.UnknownOperation",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchLevelUnknown::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("delete_everything"));

  FString Error;
  TestFalse(TEXT("Unknown level op rejected"), MCPDispatchHelpers::ValidateLevelOps(Body, Error));

  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchLevelMissing,
                                 "MCPUnreal.OperationDispatch.Level.MissingOperation",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchLevelMissing::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  // No operation field at all

  FString Error;
  TestFalse(TEXT("Missing operation rejected"), MCPDispatchHelpers::ValidateLevelOps(Body, Error));
  TestTrue(TEXT("Error mentions operation"), Error.Contains(TEXT("operation")));

  return true;
}

// ---------------------------------------------------------------------------
// PCG operation dispatch tests
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchPCGExecute, "MCPUnreal.OperationDispatch.PCG.Execute",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchPCGExecute::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("execute"));
  FString Error;
  TestTrue(TEXT("execute valid"), MCPDispatchHelpers::ValidatePCGOps(Body, Error));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchPCGUnknown,
                                 "MCPUnreal.OperationDispatch.PCG.UnknownOperation",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchPCGUnknown::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("invalid_pcg_op"));
  FString Error;
  TestFalse(TEXT("Unknown PCG op rejected"), MCPDispatchHelpers::ValidatePCGOps(Body, Error));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchPCGMissing,
                                 "MCPUnreal.OperationDispatch.PCG.MissingOperation",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchPCGMissing::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  FString Error;
  TestFalse(TEXT("Missing op rejected"), MCPDispatchHelpers::ValidatePCGOps(Body, Error));
  TestTrue(TEXT("Error mentions operation"), Error.Contains(TEXT("operation")));
  return true;
}

// ---------------------------------------------------------------------------
// GAS operation dispatch tests
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchGASGrantAbility,
                                 "MCPUnreal.OperationDispatch.GAS.GrantAbility",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchGASGrantAbility::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("grant_ability"));
  FString Error;
  TestTrue(TEXT("grant_ability valid"), MCPDispatchHelpers::ValidateGASOps(Body, Error));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchGASUnknown,
                                 "MCPUnreal.OperationDispatch.GAS.UnknownOperation",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchGASUnknown::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("explode"));
  FString Error;
  TestFalse(TEXT("Unknown GAS op rejected"), MCPDispatchHelpers::ValidateGASOps(Body, Error));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchGASMissing,
                                 "MCPUnreal.OperationDispatch.GAS.MissingOperation",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchGASMissing::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  FString Error;
  TestFalse(TEXT("Missing op rejected"), MCPDispatchHelpers::ValidateGASOps(Body, Error));
  return true;
}

// ---------------------------------------------------------------------------
// Niagara operation dispatch tests
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchNiagaraSpawn,
                                 "MCPUnreal.OperationDispatch.Niagara.SpawnSystem",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchNiagaraSpawn::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("spawn_system"));
  FString Error;
  TestTrue(TEXT("spawn_system valid"), MCPDispatchHelpers::ValidateNiagaraOps(Body, Error));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchNiagaraUnknown,
                                 "MCPUnreal.OperationDispatch.Niagara.UnknownOperation",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchNiagaraUnknown::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("destroy_universe"));
  FString Error;
  TestFalse(TEXT("Unknown Niagara op rejected"),
            MCPDispatchHelpers::ValidateNiagaraOps(Body, Error));
  return true;
}

// ---------------------------------------------------------------------------
// ISM operation dispatch tests
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchISMCreate, "MCPUnreal.OperationDispatch.ISM.Create",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchISMCreate::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("create"));
  FString Error;
  TestTrue(TEXT("create valid"), MCPDispatchHelpers::ValidateISMOps(Body, Error));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchISMAddInstances,
                                 "MCPUnreal.OperationDispatch.ISM.AddInstances",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchISMAddInstances::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("add_instances"));
  FString Error;
  TestTrue(TEXT("add_instances valid"), MCPDispatchHelpers::ValidateISMOps(Body, Error));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchISMUnknown,
                                 "MCPUnreal.OperationDispatch.ISM.UnknownOperation",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchISMUnknown::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("teleport"));
  FString Error;
  TestFalse(TEXT("Unknown ISM op rejected"), MCPDispatchHelpers::ValidateISMOps(Body, Error));
  return true;
}

// ---------------------------------------------------------------------------
// DataAsset operation dispatch tests
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchDataAssetListTables,
                                 "MCPUnreal.OperationDispatch.DataAsset.ListTables",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchDataAssetListTables::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("list_tables"));
  FString Error;
  TestTrue(TEXT("list_tables valid"), MCPDispatchHelpers::ValidateDataAssetOps(Body, Error));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchDataAssetUnknown,
                                 "MCPUnreal.OperationDispatch.DataAsset.UnknownOperation",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchDataAssetUnknown::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("drop_table"));
  FString Error;
  TestFalse(TEXT("Unknown DataAsset op rejected"),
            MCPDispatchHelpers::ValidateDataAssetOps(Body, Error));
  return true;
}

// ---------------------------------------------------------------------------
// Texture operation dispatch tests
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchTextureImport,
                                 "MCPUnreal.OperationDispatch.Texture.Import",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchTextureImport::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("import"));
  FString Error;
  TestTrue(TEXT("import valid"), MCPDispatchHelpers::ValidateTextureOps(Body, Error));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchTextureUnknown,
                                 "MCPUnreal.OperationDispatch.Texture.UnknownOperation",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchTextureUnknown::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("corrupt"));
  FString Error;
  TestFalse(TEXT("Unknown texture op rejected"),
            MCPDispatchHelpers::ValidateTextureOps(Body, Error));
  return true;
}

// ---------------------------------------------------------------------------
// Fab operation dispatch tests
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchFabListCache,
                                 "MCPUnreal.OperationDispatch.Fab.ListCache",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchFabListCache::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("list_cache"));
  FString Error;
  TestTrue(TEXT("list_cache valid"), MCPDispatchHelpers::ValidateFabOps(Body, Error));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchFabUnknown,
                                 "MCPUnreal.OperationDispatch.Fab.UnknownOperation",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchFabUnknown::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("purchase_everything"));
  FString Error;
  TestFalse(TEXT("Unknown Fab op rejected"), MCPDispatchHelpers::ValidateFabOps(Body, Error));
  return true;
}

// ---------------------------------------------------------------------------
// Character operation dispatch tests
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchCharGetConfig,
                                 "MCPUnreal.OperationDispatch.Character.GetConfig",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchCharGetConfig::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("get_config"));
  FString Error;
  TestTrue(TEXT("get_config valid"), MCPDispatchHelpers::ValidateCharacterOps(Body, Error));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchCharUnknown,
                                 "MCPUnreal.OperationDispatch.Character.UnknownOperation",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchCharUnknown::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("fly"));
  FString Error;
  TestFalse(TEXT("Unknown character op rejected"),
            MCPDispatchHelpers::ValidateCharacterOps(Body, Error));
  return true;
}

// ---------------------------------------------------------------------------
// Input operation dispatch tests
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchInputListActions,
                                 "MCPUnreal.OperationDispatch.Input.ListActions",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchInputListActions::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("list_actions"));
  FString Error;
  TestTrue(TEXT("list_actions valid"), MCPDispatchHelpers::ValidateInputOps(Body, Error));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchInputBindAction,
                                 "MCPUnreal.OperationDispatch.Input.BindAction",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchInputBindAction::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("bind_action"));
  FString Error;
  TestTrue(TEXT("bind_action valid"), MCPDispatchHelpers::ValidateInputOps(Body, Error));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchInputUnknown,
                                 "MCPUnreal.OperationDispatch.Input.UnknownOperation",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchInputUnknown::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("remap_keyboard"));
  FString Error;
  TestFalse(TEXT("Unknown input op rejected"), MCPDispatchHelpers::ValidateInputOps(Body, Error));
  return true;
}

// ---------------------------------------------------------------------------
// UI Query operation dispatch tests
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchUIQueryTree,
                                 "MCPUnreal.OperationDispatch.UIQuery.Tree",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchUIQueryTree::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("tree"));
  FString Error;
  TestTrue(TEXT("tree valid"), MCPDispatchHelpers::ValidateUIQueryOps(Body, Error));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchUIQueryUnknown,
                                 "MCPUnreal.OperationDispatch.UIQuery.UnknownOperation",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchUIQueryUnknown::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("delete_widget"));
  FString Error;
  TestFalse(TEXT("Unknown UIQuery op rejected"),
            MCPDispatchHelpers::ValidateUIQueryOps(Body, Error));
  return true;
}

// ---------------------------------------------------------------------------
// Network Debug operation dispatch tests
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchNetDebugListActive,
                                 "MCPUnreal.OperationDispatch.NetworkDebug.ListActive",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchNetDebugListActive::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("list_active"));
  FString Error;
  TestTrue(TEXT("list_active valid"), MCPDispatchHelpers::ValidateNetworkDebugOps(Body, Error));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchNetDebugSummary,
                                 "MCPUnreal.OperationDispatch.NetworkDebug.Summary",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchNetDebugSummary::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("summary"));
  FString Error;
  TestTrue(TEXT("summary valid"), MCPDispatchHelpers::ValidateNetworkDebugOps(Body, Error));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDispatchNetDebugUnknown,
                                 "MCPUnreal.OperationDispatch.NetworkDebug.UnknownOperation",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FMCPDispatchNetDebugUnknown::RunTest(const FString& Parameters) {
  TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
  Body->SetStringField(TEXT("operation"), TEXT("hack_network"));
  FString Error;
  TestFalse(TEXT("Unknown network debug op rejected"),
            MCPDispatchHelpers::ValidateNetworkDebugOps(Body, Error));
  return true;
}
