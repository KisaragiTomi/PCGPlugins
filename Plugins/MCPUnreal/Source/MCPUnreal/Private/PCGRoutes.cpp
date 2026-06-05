// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.
//
// PCGRoutes.cpp — HTTP routes for Procedural Content Generation (PCG)
// graph editing and component execution.
//
// Guarded by WITH_PCG — returns 501 when PCG module is unavailable.

#include "MCPUnrealUtils.h"

#if WITH_PCG
#include "PCGComponent.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGSettings.h"
#include "PCGPin.h"
#include "PCGEdge.h"
#include "EngineUtils.h"
#include "UObject/UObjectIterator.h"
#endif

namespace MCPUnreal {

  // ---------------------------------------------------------------------------
  // POST /api/pcg/ops
  // ---------------------------------------------------------------------------

  static bool HandlePCGOps(const FHttpServerRequest& Request,
                           const FHttpResultCallback& OnComplete) {
#if WITH_PCG
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

    UWorld* World = GetWorld(Body);

    // --- execute ---
    if (Operation == TEXT("execute")) {
      const FString ActorPath = Body->GetStringField(TEXT("actor_path"));
      if (ActorPath.IsEmpty()) {
        SendError(OnComplete, TEXT("actor_path is required for execute"));
        return true;
      }

      AActor* Actor = FindObject<AActor>(nullptr, *ActorPath);
      if (!Actor) {
        SendError(OnComplete, FString::Printf(TEXT("Actor not found: '%s'"), *ActorPath));
        return true;
      }

      UPCGComponent* PCGComp = Actor->FindComponentByClass<UPCGComponent>();
      if (!PCGComp) {
        SendError(OnComplete, TEXT("Actor does not have a UPCGComponent"));
        return true;
      }

      PCGComp->Generate(true);

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetStringField(TEXT("actor_path"), Actor->GetPathName());
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- cleanup ---
    if (Operation == TEXT("cleanup")) {
      const FString ActorPath = Body->GetStringField(TEXT("actor_path"));
      if (ActorPath.IsEmpty()) {
        SendError(OnComplete, TEXT("actor_path is required for cleanup"));
        return true;
      }

      AActor* Actor = FindObject<AActor>(nullptr, *ActorPath);
      if (!Actor) {
        SendError(OnComplete, FString::Printf(TEXT("Actor not found: '%s'"), *ActorPath));
        return true;
      }

      UPCGComponent* PCGComp = Actor->FindComponentByClass<UPCGComponent>();
      if (!PCGComp) {
        SendError(OnComplete, TEXT("Actor does not have a UPCGComponent"));
        return true;
      }

      PCGComp->Cleanup(true);

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- get_graph_info ---
    if (Operation == TEXT("get_graph_info")) {
      const FString GraphPath = Body->GetStringField(TEXT("graph_path"));
      if (GraphPath.IsEmpty()) {
        SendError(OnComplete, TEXT("graph_path is required for get_graph_info"));
        return true;
      }

      UPCGGraph* Graph = LoadObject<UPCGGraph>(nullptr, *GraphPath);
      if (!Graph) {
        SendError(OnComplete, FString::Printf(TEXT("PCG graph not found: '%s'"), *GraphPath));
        return true;
      }

      // Serialize nodes.
      TArray<TSharedPtr<FJsonValue>> NodesArray;
      for (UPCGNode* Node : Graph->GetNodes()) {
        if (!Node) continue;

        TSharedPtr<FJsonObject> NodeJson = MakeShareable(new FJsonObject());
        NodeJson->SetStringField(TEXT("id"), Node->GetFName().ToString());
        NodeJson->SetStringField(TEXT("title"),
                                 Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString());

        if (Node->GetSettings()) {
          NodeJson->SetStringField(TEXT("settings_class"),
                                   Node->GetSettings()->GetClass()->GetName());
        }

        // List pins.
        TArray<TSharedPtr<FJsonValue>> InputPins;
        for (UPCGPin* Pin : Node->GetInputPins()) {
          if (!Pin) continue;
          TSharedPtr<FJsonObject> PinJson = MakeShareable(new FJsonObject());
          PinJson->SetStringField(TEXT("label"), Pin->Properties.Label.ToString());
          InputPins.Add(MakeShareable(new FJsonValueObject(PinJson)));
        }
        NodeJson->SetArrayField(TEXT("input_pins"), InputPins);

        TArray<TSharedPtr<FJsonValue>> OutputPins;
        for (UPCGPin* Pin : Node->GetOutputPins()) {
          if (!Pin) continue;
          TSharedPtr<FJsonObject> PinJson = MakeShareable(new FJsonObject());
          PinJson->SetStringField(TEXT("label"), Pin->Properties.Label.ToString());
          OutputPins.Add(MakeShareable(new FJsonValueObject(PinJson)));
        }
        NodeJson->SetArrayField(TEXT("output_pins"), OutputPins);

        NodesArray.Add(MakeShareable(new FJsonValueObject(NodeJson)));
      }

      // Serialize edges.
      TArray<TSharedPtr<FJsonValue>> EdgesArray;
      for (UPCGNode* Node : Graph->GetNodes()) {
        if (!Node) continue;
        for (UPCGPin* OutputPin : Node->GetOutputPins()) {
          if (!OutputPin) continue;
          for (const UPCGEdge* Edge : OutputPin->Edges) {
            if (!Edge || !Edge->InputPin || !Edge->InputPin->Node) continue;

            TSharedPtr<FJsonObject> EdgeJson = MakeShareable(new FJsonObject());
            EdgeJson->SetStringField(TEXT("source_node"), Node->GetFName().ToString());
            EdgeJson->SetStringField(TEXT("source_pin"), OutputPin->Properties.Label.ToString());
            EdgeJson->SetStringField(TEXT("target_node"),
                                     Edge->InputPin->Node->GetFName().ToString());
            EdgeJson->SetStringField(TEXT("target_pin"),
                                     Edge->InputPin->Properties.Label.ToString());
            EdgesArray.Add(MakeShareable(new FJsonValueObject(EdgeJson)));
          }
        }
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetArrayField(TEXT("nodes"), NodesArray);
      ResponseJson->SetArrayField(TEXT("edges"), EdgesArray);
      ResponseJson->SetNumberField(TEXT("node_count"), NodesArray.Num());
      ResponseJson->SetNumberField(TEXT("edge_count"), EdgesArray.Num());
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- set_parameter ---
    if (Operation == TEXT("set_parameter")) {
      const FString ActorPath = Body->GetStringField(TEXT("actor_path"));
      const FString ParamName = Body->GetStringField(TEXT("parameter_name"));
      if (ActorPath.IsEmpty() || ParamName.IsEmpty()) {
        SendError(OnComplete, TEXT("actor_path and parameter_name are required for set_parameter"));
        return true;
      }

      AActor* Actor = FindObject<AActor>(nullptr, *ActorPath);
      if (!Actor) {
        SendError(OnComplete, FString::Printf(TEXT("Actor not found: '%s'"), *ActorPath));
        return true;
      }

      UPCGComponent* PCGComp = Actor->FindComponentByClass<UPCGComponent>();
      if (!PCGComp) {
        SendError(OnComplete, TEXT("Actor does not have a UPCGComponent"));
        return true;
      }

      // Attempt to set the parameter as different types.
      bool bSet = false;
      if (Body->HasField(TEXT("parameter_value"))) {
        // Try numeric first.
        double NumValue;
        if (Body->TryGetNumberField(TEXT("parameter_value"), NumValue)) {
          // Set as numeric — PCG parameters accept FPCGMetadataAttribute.
          // Use property system for user parameters.
          FProperty* Prop = PCGComp->GetClass()->FindPropertyByName(FName(*ParamName));
          if (Prop) {
            if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop)) {
              FloatProp->SetPropertyValue_InContainer(PCGComp, static_cast<float>(NumValue));
              bSet = true;
            } else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop)) {
              DoubleProp->SetPropertyValue_InContainer(PCGComp, NumValue);
              bSet = true;
            } else if (FIntProperty* IntProp = CastField<FIntProperty>(Prop)) {
              IntProp->SetPropertyValue_InContainer(PCGComp, static_cast<int32>(NumValue));
              bSet = true;
            }
          }
        }

        if (!bSet) {
          // Try string.
          FString StrValue;
          if (Body->TryGetStringField(TEXT("parameter_value"), StrValue)) {
            FProperty* Prop = PCGComp->GetClass()->FindPropertyByName(FName(*ParamName));
            if (Prop) {
              if (FStrProperty* StrProp = CastField<FStrProperty>(Prop)) {
                StrProp->SetPropertyValue_InContainer(PCGComp, StrValue);
                bSet = true;
              }
            }
          }
        }
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), bSet);
      if (!bSet) {
        ResponseJson->SetStringField(TEXT("warning"), TEXT("Parameter not found or type mismatch"));
      }
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- add_node ---
    if (Operation == TEXT("add_node")) {
      const FString GraphPath = Body->GetStringField(TEXT("graph_path"));
      const FString NodeType = Body->GetStringField(TEXT("node_type"));
      if (GraphPath.IsEmpty() || NodeType.IsEmpty()) {
        SendError(OnComplete, TEXT("graph_path and node_type are required for add_node"));
        return true;
      }

      UPCGGraph* Graph = LoadObject<UPCGGraph>(nullptr, *GraphPath);
      if (!Graph) {
        SendError(OnComplete, FString::Printf(TEXT("PCG graph not found: '%s'"), *GraphPath));
        return true;
      }

      // Find the settings class by name.
      UClass* SettingsClass =
          FindFirstObject<UClass>(*NodeType, EFindFirstObjectOptions::ExactClass);
      if (!SettingsClass || !SettingsClass->IsChildOf(UPCGSettings::StaticClass())) {
        SendError(OnComplete,
                  FString::Printf(TEXT("PCG settings class not found: '%s'"), *NodeType));
        return true;
      }

      UPCGSettings* DefaultSettings = NewObject<UPCGSettings>(GetTransientPackage(), SettingsClass);
      UPCGNode* NewNode = Graph->AddNode(DefaultSettings);

      if (!NewNode) {
        SendError(OnComplete, TEXT("Failed to add node to graph"), 500);
        return true;
      }

      // Set optional label.
      FString NodeLabel = Body->GetStringField(TEXT("node_label"));
      if (!NodeLabel.IsEmpty()) {
        NewNode->NodeTitle = FName(*NodeLabel);
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetStringField(TEXT("node_id"), NewNode->GetFName().ToString());
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- connect_nodes ---
    if (Operation == TEXT("connect_nodes")) {
      const FString GraphPath = Body->GetStringField(TEXT("graph_path"));
      const FString SourceNodeID = Body->GetStringField(TEXT("node_id"));
      const FString TargetNodeID = Body->GetStringField(TEXT("target_node_id"));
      if (GraphPath.IsEmpty() || SourceNodeID.IsEmpty() || TargetNodeID.IsEmpty()) {
        SendError(OnComplete,
                  TEXT("graph_path, node_id, and target_node_id are required for connect_nodes"));
        return true;
      }

      UPCGGraph* Graph = LoadObject<UPCGGraph>(nullptr, *GraphPath);
      if (!Graph) {
        SendError(OnComplete, FString::Printf(TEXT("PCG graph not found: '%s'"), *GraphPath));
        return true;
      }

      // Find source and target nodes by FName.
      UPCGNode* SourceNode = nullptr;
      UPCGNode* TargetNode = nullptr;
      for (UPCGNode* Node : Graph->GetNodes()) {
        if (Node && Node->GetFName().ToString() == SourceNodeID) {
          SourceNode = Node;
        }
        if (Node && Node->GetFName().ToString() == TargetNodeID) {
          TargetNode = Node;
        }
      }

      if (!SourceNode) {
        SendError(OnComplete, FString::Printf(TEXT("Source node not found: '%s'"), *SourceNodeID));
        return true;
      }
      if (!TargetNode) {
        SendError(OnComplete, FString::Printf(TEXT("Target node not found: '%s'"), *TargetNodeID));
        return true;
      }

      // Find pins by label (or use first available).
      const FString SourcePinLabel = Body->GetStringField(TEXT("source_pin_label"));
      const FString TargetPinLabel = Body->GetStringField(TEXT("target_pin_label"));

      UPCGPin* SourcePin = nullptr;
      for (UPCGPin* Pin : SourceNode->GetOutputPins()) {
        if (Pin &&
            (SourcePinLabel.IsEmpty() || Pin->Properties.Label.ToString() == SourcePinLabel)) {
          SourcePin = Pin;
          break;
        }
      }

      UPCGPin* TargetPin = nullptr;
      for (UPCGPin* Pin : TargetNode->GetInputPins()) {
        if (Pin &&
            (TargetPinLabel.IsEmpty() || Pin->Properties.Label.ToString() == TargetPinLabel)) {
          TargetPin = Pin;
          break;
        }
      }

      if (!SourcePin) {
        SendError(OnComplete, TEXT("Source output pin not found"));
        return true;
      }
      if (!TargetPin) {
        SendError(OnComplete, TEXT("Target input pin not found"));
        return true;
      }

      Graph->AddEdge(SourceNode, SourcePin->Properties.Label, TargetNode,
                     TargetPin->Properties.Label);

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- remove_node ---
    if (Operation == TEXT("remove_node")) {
      const FString GraphPath = Body->GetStringField(TEXT("graph_path"));
      const FString NodeID = Body->GetStringField(TEXT("node_id"));
      if (GraphPath.IsEmpty() || NodeID.IsEmpty()) {
        SendError(OnComplete, TEXT("graph_path and node_id are required for remove_node"));
        return true;
      }

      UPCGGraph* Graph = LoadObject<UPCGGraph>(nullptr, *GraphPath);
      if (!Graph) {
        SendError(OnComplete, FString::Printf(TEXT("PCG graph not found: '%s'"), *GraphPath));
        return true;
      }

      UPCGNode* TargetNode = nullptr;
      for (UPCGNode* Node : Graph->GetNodes()) {
        if (Node && Node->GetFName().ToString() == NodeID) {
          TargetNode = Node;
          break;
        }
      }

      if (!TargetNode) {
        SendError(OnComplete, FString::Printf(TEXT("Node not found: '%s'"), *NodeID));
        return true;
      }

      Graph->RemoveNode(TargetNode);

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    SendError(OnComplete, FString::Printf(TEXT("Unknown PCG operation: '%s'"), *Operation));
    return true;
#else
    SendError(
        OnComplete,
        TEXT("PCG module is not available. Enable the PCG plugin in your project to use pcg_ops."),
        501);
    return true;
#endif
  }

  // ---------------------------------------------------------------------------
  // Registration
  // ---------------------------------------------------------------------------

  void RegisterPCGRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles) {
    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/pcg/ops")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandlePCGOps)));

    UE_LOG(LogMCPUnreal, Verbose, TEXT("Registered PCG routes (1 endpoint)"));
  }

}  // namespace MCPUnreal
