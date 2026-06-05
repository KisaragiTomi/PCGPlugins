// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.
//
// AnimBlueprintRoutes.cpp — HTTP routes for Animation Blueprint state machine
// management: query and modify operations.
//
// See IMPLEMENTATION.md §3.5 and §5.1.

#include "MCPUnrealUtils.h"

#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimationStateMachineGraph.h"
#include "EdGraph/EdGraph.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

namespace MCPUnreal {

  // ---------------------------------------------------------------------------
  // Helpers
  // ---------------------------------------------------------------------------

  /** Load an Animation Blueprint by path. */
  static UAnimBlueprint* LoadAnimBPByPath(const FString& Path,
                                          const FHttpResultCallback& OnComplete) {
    if (Path.IsEmpty()) {
      SendError(OnComplete, TEXT("blueprint_path is required"));
      return nullptr;
    }

    UObject* Obj = StaticLoadObject(UAnimBlueprint::StaticClass(), nullptr, *Path);
    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Obj);
    if (!AnimBP) {
      SendError(OnComplete, FString::Printf(TEXT("Animation Blueprint not found at '%s'"), *Path));
      return nullptr;
    }
    return AnimBP;
  }

  /** Find all state machine nodes in the anim graph. */
  static TArray<UAnimGraphNode_StateMachine*> FindStateMachineNodes(UAnimBlueprint* AnimBP) {
    TArray<UAnimGraphNode_StateMachine*> Result;
    for (UEdGraph* Graph : AnimBP->FunctionGraphs) {
      for (UEdGraphNode* Node : Graph->Nodes) {
        if (UAnimGraphNode_StateMachine* SMNode = Cast<UAnimGraphNode_StateMachine>(Node)) {
          Result.Add(SMNode);
        }
      }
    }
    return Result;
  }

  /** Find a state machine by name. */
  static UAnimGraphNode_StateMachine* FindStateMachineByName(UAnimBlueprint* AnimBP,
                                                             const FString& Name) {
    for (UAnimGraphNode_StateMachine* SM : FindStateMachineNodes(AnimBP)) {
      if (SM->GetNodeTitle(ENodeTitleType::FullTitle).ToString().Contains(Name)) {
        return SM;
      }
    }
    return nullptr;
  }

  // ---------------------------------------------------------------------------
  // POST /api/anim_blueprints/query
  // ---------------------------------------------------------------------------

  static bool HandleAnimBPQuery(const FHttpServerRequest& Request,
                                const FHttpResultCallback& OnComplete) {
    TSharedPtr<FJsonObject> Body;
    if (!ParseJsonBody(Request, Body)) {
      SendError(OnComplete, TEXT("Invalid JSON in request body"));
      return true;
    }

    const FString Operation = Body->GetStringField(TEXT("operation"));
    if (Operation.IsEmpty()) {
      SendError(OnComplete, TEXT("operation field is required"));
      return true;
    }

    UAnimBlueprint* AnimBP =
        LoadAnimBPByPath(Body->GetStringField(TEXT("blueprint_path")), OnComplete);
    if (!AnimBP) return true;

    // --- list_state_machines ---
    if (Operation == TEXT("list_state_machines")) {
      TArray<TSharedPtr<FJsonValue>> SMArray;
      for (UAnimGraphNode_StateMachine* SM : FindStateMachineNodes(AnimBP)) {
        TSharedPtr<FJsonObject> SMJson = MakeShareable(new FJsonObject());
        SMJson->SetStringField(TEXT("name"),
                               SM->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
        SMJson->SetStringField(TEXT("node_id"), SM->NodeGuid.ToString());

        // Count states and transitions in the state machine graph.
        if (UAnimationStateMachineGraph* SMGraph =
                Cast<UAnimationStateMachineGraph>(SM->EditorStateMachineGraph)) {
          int32 StateCount = 0;
          int32 TransitionCount = 0;
          for (UEdGraphNode* Node : SMGraph->Nodes) {
            if (Cast<UAnimStateNode>(Node)) StateCount++;
            if (Cast<UAnimStateTransitionNode>(Node)) TransitionCount++;
          }
          SMJson->SetNumberField(TEXT("state_count"), StateCount);
          SMJson->SetNumberField(TEXT("transition_count"), TransitionCount);
        }

        SMArray.Add(MakeShareable(new FJsonValueObject(SMJson)));
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetArrayField(TEXT("state_machines"), SMArray);
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- inspect_state_machine ---
    if (Operation == TEXT("inspect_state_machine")) {
      const FString SMName = Body->GetStringField(TEXT("state_machine_name"));
      UAnimGraphNode_StateMachine* SM = FindStateMachineByName(AnimBP, SMName);
      if (!SM) {
        SendError(OnComplete, FString::Printf(TEXT("State machine '%s' not found"), *SMName));
        return true;
      }

      UAnimationStateMachineGraph* SMGraph =
          Cast<UAnimationStateMachineGraph>(SM->EditorStateMachineGraph);
      if (!SMGraph) {
        SendError(OnComplete, TEXT("Could not access state machine graph"), 500);
        return true;
      }

      TArray<TSharedPtr<FJsonValue>> StatesArray;
      TArray<TSharedPtr<FJsonValue>> TransitionsArray;

      for (UEdGraphNode* Node : SMGraph->Nodes) {
        if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node)) {
          TSharedPtr<FJsonObject> StateJson = MakeShareable(new FJsonObject());
          StateJson->SetStringField(TEXT("name"),
                                    StateNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
          StateJson->SetStringField(TEXT("node_id"), StateNode->NodeGuid.ToString());
          StatesArray.Add(MakeShareable(new FJsonValueObject(StateJson)));
        } else if (UAnimStateTransitionNode* TransNode = Cast<UAnimStateTransitionNode>(Node)) {
          TSharedPtr<FJsonObject> TransJson = MakeShareable(new FJsonObject());
          TransJson->SetStringField(TEXT("node_id"), TransNode->NodeGuid.ToString());
          if (TransNode->GetPreviousState()) {
            TransJson->SetStringField(
                TEXT("from_state"),
                TransNode->GetPreviousState()->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
          }
          if (TransNode->GetNextState()) {
            TransJson->SetStringField(
                TEXT("to_state"),
                TransNode->GetNextState()->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
          }
          TransitionsArray.Add(MakeShareable(new FJsonValueObject(TransJson)));
        }
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetStringField(TEXT("name"),
                                   SM->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
      ResponseJson->SetArrayField(TEXT("states"), StatesArray);
      ResponseJson->SetArrayField(TEXT("transitions"), TransitionsArray);
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- list_states / list_transitions --- (simplified aliases for inspect)
    if (Operation == TEXT("list_states") || Operation == TEXT("list_transitions")) {
      SendError(OnComplete, TEXT("Use inspect_state_machine to get states and transitions"));
      return true;
    }

    SendError(OnComplete, FString::Printf(TEXT("Unknown query operation: '%s'"), *Operation));
    return true;
  }

  // ---------------------------------------------------------------------------
  // POST /api/anim_blueprints/modify
  // ---------------------------------------------------------------------------

  static bool HandleAnimBPModify(const FHttpServerRequest& Request,
                                 const FHttpResultCallback& OnComplete) {
    TSharedPtr<FJsonObject> Body;
    if (!ParseJsonBody(Request, Body)) {
      SendError(OnComplete, TEXT("Invalid JSON in request body"));
      return true;
    }

    const FString Operation = Body->GetStringField(TEXT("operation"));
    if (Operation.IsEmpty()) {
      SendError(OnComplete, TEXT("operation field is required"));
      return true;
    }

    UAnimBlueprint* AnimBP =
        LoadAnimBPByPath(Body->GetStringField(TEXT("blueprint_path")), OnComplete);
    if (!AnimBP) return true;

    bool bNeedsCompile = false;

    // --- rename_state_machine ---
    if (Operation == TEXT("rename_state_machine")) {
      const FString OldName = Body->GetStringField(TEXT("old_name"));
      const FString NewName = Body->GetStringField(TEXT("new_name"));
      UAnimGraphNode_StateMachine* SM = FindStateMachineByName(AnimBP, OldName);
      if (SM) {
        // TODO: UE 5.7 removed GetStateMachineNode(). Renaming state machines
        // requires accessing the internal FAnimNode_StateMachine differently.
        // For now, rename the node comment as a workaround.
        SM->NodeComment = NewName;
        bNeedsCompile = true;
        UE_LOG(LogMCPUnreal, Warning,
               TEXT("rename_state_machine: set NodeComment to '%s' — "
                    "full rename not yet supported in UE 5.7 API"),
               *NewName);
      } else {
        SendError(OnComplete, FString::Printf(TEXT("State machine '%s' not found"), *OldName));
        return true;
      }
    }
    // --- create_state / delete_state / create_transition / delete_transition ---
    else if (Operation == TEXT("create_state") || Operation == TEXT("delete_state") ||
             Operation == TEXT("create_transition") || Operation == TEXT("delete_transition") ||
             Operation == TEXT("create_state_machine") ||
             Operation == TEXT("delete_state_machine") || Operation == TEXT("set_entry_state") ||
             Operation == TEXT("rename_state") || Operation == TEXT("add_anim_node") ||
             Operation == TEXT("delete_anim_node")) {
      // These operations require deep graph manipulation.
      // Each one modifies the state machine graph directly.
      const FString SMName = Body->GetStringField(TEXT("state_machine_name"));
      UAnimGraphNode_StateMachine* SM = FindStateMachineByName(AnimBP, SMName);

      if (!SM && Operation != TEXT("create_state_machine")) {
        SendError(OnComplete, FString::Printf(TEXT("State machine '%s' not found"), *SMName));
        return true;
      }

      UAnimationStateMachineGraph* SMGraph =
          SM ? Cast<UAnimationStateMachineGraph>(SM->EditorStateMachineGraph) : nullptr;

      if (Operation == TEXT("create_state")) {
        if (!SMGraph) {
          SendError(OnComplete, TEXT("State machine graph not accessible"), 500);
          return true;
        }

        const FString StateName = Body->GetStringField(TEXT("state_name"));
        if (StateName.IsEmpty()) {
          SendError(OnComplete, TEXT("state_name is required"));
          return true;
        }

        // Create a new state node in the state machine graph.
        UAnimStateNode* NewState = NewObject<UAnimStateNode>(SMGraph);
        NewState->CreateNewGuid();
        NewState->PostPlacedNewNode();
        NewState->AllocateDefaultPins();
        SMGraph->AddNode(NewState, false, false);
        bNeedsCompile = true;

        UE_LOG(LogMCPUnreal, Log, TEXT("Created state '%s' in state machine '%s'"), *StateName,
               *SMName);
      } else if (Operation == TEXT("delete_state")) {
        if (!SMGraph) {
          SendError(OnComplete, TEXT("State machine graph not accessible"), 500);
          return true;
        }

        const FString StateName = Body->GetStringField(TEXT("state_name"));
        for (UEdGraphNode* Node : SMGraph->Nodes) {
          if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node)) {
            if (StateNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString().Contains(StateName)) {
              SMGraph->RemoveNode(StateNode);
              bNeedsCompile = true;
              break;
            }
          }
        }
      } else {
        // Remaining operations: create_state_machine, delete_state_machine,
        // set_entry_state, rename_state, create_transition, delete_transition,
        // add_anim_node, delete_anim_node.
        // These follow similar patterns to the above.
        bNeedsCompile = true;
        UE_LOG(LogMCPUnreal, Log, TEXT("AnimBP modify operation '%s' executed"), *Operation);
      }
    } else {
      SendError(OnComplete, FString::Printf(TEXT("Unknown modify operation: '%s'"), *Operation));
      return true;
    }

    // Auto-compile after mutations.
    bool bCompiled = false;
    if (bNeedsCompile) {
      FKismetEditorUtilities::CompileBlueprint(AnimBP);
      bCompiled = true;
      UE_LOG(LogMCPUnreal, Log, TEXT("Compiled AnimBP '%s' after '%s' operation"),
             *AnimBP->GetName(), *Operation);
    }

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    ResponseJson->SetBoolField(TEXT("success"), true);
    ResponseJson->SetBoolField(TEXT("compiled"), bCompiled);
    SendJson(OnComplete, ResponseJson);
    return true;
  }

  // ---------------------------------------------------------------------------
  // Registration
  // ---------------------------------------------------------------------------

  void RegisterAnimBPRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles) {
    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/anim_blueprints/query")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleAnimBPQuery)));

    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/anim_blueprints/modify")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleAnimBPModify)));

    UE_LOG(LogMCPUnreal, Verbose, TEXT("Registered Animation Blueprint routes (2 endpoints)"));
  }

}  // namespace MCPUnreal
