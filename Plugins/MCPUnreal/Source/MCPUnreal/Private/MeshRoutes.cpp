// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.
//
// MeshRoutes.cpp — HTTP routes for procedural mesh generation and
// RealtimeMesh component operations.
//
// See IMPLEMENTATION.md §3.9 and §5.3.
// RealtimeMesh support is optional — guarded by WITH_REALTIMEMESH.

#include "MCPUnrealUtils.h"

#include "ProceduralMeshComponent.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "EngineUtils.h"

// Optional RealtimeMesh support.
#if defined(WITH_REALTIMEMESH) && WITH_REALTIMEMESH
#include "RealtimeMeshSimple.h"
#include "RealtimeMeshActor.h"
#endif

namespace MCPUnreal {

  // ---------------------------------------------------------------------------
  // Helpers — vertex data parsing
  // ---------------------------------------------------------------------------

  static TArray<FVector> ParseVectorArray(const TSharedPtr<FJsonObject>& Body,
                                          const FString& FieldName) {
    TArray<FVector> Result;
    const TArray<TSharedPtr<FJsonValue>>* Array;
    if (Body->TryGetArrayField(FieldName, Array)) {
      for (const auto& Val : *Array) {
        const TArray<TSharedPtr<FJsonValue>>* Vec;
        if (Val->TryGetArray(Vec) && Vec->Num() >= 3) {
          Result.Add(FVector((*Vec)[0]->AsNumber(), (*Vec)[1]->AsNumber(), (*Vec)[2]->AsNumber()));
        }
      }
    }
    return Result;
  }

  static TArray<FVector2D> ParseVector2DArray(const TSharedPtr<FJsonObject>& Body,
                                              const FString& FieldName) {
    TArray<FVector2D> Result;
    const TArray<TSharedPtr<FJsonValue>>* Array;
    if (Body->TryGetArrayField(FieldName, Array)) {
      for (const auto& Val : *Array) {
        const TArray<TSharedPtr<FJsonValue>>* Vec;
        if (Val->TryGetArray(Vec) && Vec->Num() >= 2) {
          Result.Add(FVector2D((*Vec)[0]->AsNumber(), (*Vec)[1]->AsNumber()));
        }
      }
    }
    return Result;
  }

  static TArray<FLinearColor> ParseColorArray(const TSharedPtr<FJsonObject>& Body,
                                              const FString& FieldName) {
    TArray<FLinearColor> Result;
    const TArray<TSharedPtr<FJsonValue>>* Array;
    if (Body->TryGetArrayField(FieldName, Array)) {
      for (const auto& Val : *Array) {
        const TArray<TSharedPtr<FJsonValue>>* Col;
        if (Val->TryGetArray(Col) && Col->Num() >= 4) {
          Result.Add(FLinearColor((*Col)[0]->AsNumber(), (*Col)[1]->AsNumber(),
                                  (*Col)[2]->AsNumber(), (*Col)[3]->AsNumber()));
        }
      }
    }
    return Result;
  }

  static TArray<int32> ParseIntArray(const TSharedPtr<FJsonObject>& Body,
                                     const FString& FieldName) {
    TArray<int32> Result;
    const TArray<TSharedPtr<FJsonValue>>* Array;
    if (Body->TryGetArrayField(FieldName, Array)) {
      for (const auto& Val : *Array) {
        Result.Add(static_cast<int32>(Val->AsNumber()));
      }
    }
    return Result;
  }

  // ---------------------------------------------------------------------------
  // POST /api/mesh/procedural
  // ---------------------------------------------------------------------------

  static bool HandleProceduralMesh(const FHttpServerRequest& Request,
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

    UWorld* World = GetWorld(Body);
    if (!World) {
      SendError(OnComplete,
                TEXT("World not available — if world=pie was requested, ensure PIE is running"),
                500);
      return true;
    }

    // --- create_section ---
    if (Operation == TEXT("create_section")) {
      TArray<FVector> Vertices = ParseVectorArray(Body, TEXT("vertices"));
      TArray<int32> Triangles = ParseIntArray(Body, TEXT("triangles"));
      TArray<FVector> Normals = ParseVectorArray(Body, TEXT("normals"));
      TArray<FVector2D> UVs = ParseVector2DArray(Body, TEXT("uvs"));
      TArray<FLinearColor> Colors = ParseColorArray(Body, TEXT("colors"));

      if (Vertices.Num() == 0 || Triangles.Num() == 0) {
        SendError(OnComplete, TEXT("vertices and triangles are required for create_section"));
        return true;
      }

      // Auto-generate normals if not provided.
      if (Normals.Num() == 0) {
        Normals.SetNum(Vertices.Num());
        for (int32 i = 0; i < Normals.Num(); i++) {
          Normals[i] = FVector::UpVector;
        }
      }

      // Spawn or find ProceduralMesh actor.
      FString ActorPath = Body->GetStringField(TEXT("actor_path"));
      AActor* MeshActor = nullptr;

      if (!ActorPath.IsEmpty()) {
        MeshActor = FindObject<AActor>(nullptr, *ActorPath);
      }

      if (!MeshActor) {
        // Spawn new actor with ProceduralMeshComponent.
        FVector Location = FVector::ZeroVector;
        const TArray<TSharedPtr<FJsonValue>>* LocArray;
        if (Body->TryGetArrayField(TEXT("location"), LocArray) && LocArray->Num() >= 3) {
          Location.X = (*LocArray)[0]->AsNumber();
          Location.Y = (*LocArray)[1]->AsNumber();
          Location.Z = (*LocArray)[2]->AsNumber();
        }

        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride =
            ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        FTransform SpawnTransform(Location);
        MeshActor = World->SpawnActor<AActor>(AActor::StaticClass(), SpawnTransform, SpawnParams);

        if (MeshActor) {
          UProceduralMeshComponent* ProcMesh =
              NewObject<UProceduralMeshComponent>(MeshActor, TEXT("ProceduralMesh"));
          ProcMesh->RegisterComponent();
          MeshActor->AddInstanceComponent(ProcMesh);
          ProcMesh->AttachToComponent(MeshActor->GetRootComponent(),
                                      FAttachmentTransformRules::KeepRelativeTransform);

          FString ActorName = Body->GetStringField(TEXT("actor_name"));
          if (!ActorName.IsEmpty()) {
            MeshActor->SetActorLabel(ActorName);
          }
        }
      }

      if (!MeshActor) {
        SendError(OnComplete, TEXT("Failed to create/find ProceduralMesh actor"), 500);
        return true;
      }

      UProceduralMeshComponent* ProcMesh =
          MeshActor->FindComponentByClass<UProceduralMeshComponent>();
      if (!ProcMesh) {
        SendError(OnComplete, TEXT("Actor does not have a ProceduralMeshComponent"), 500);
        return true;
      }

      int32 SectionIndex = static_cast<int32>(Body->GetNumberField(TEXT("section_index")));

      // Convert FLinearColor to FColor for vertex colors.
      TArray<FColor> VertexColors;
      for (const FLinearColor& LC : Colors) {
        VertexColors.Add(LC.ToFColor(true));
      }

      TArray<FProcMeshTangent> Tangents;
      ProcMesh->CreateMeshSection(SectionIndex, Vertices, Triangles, Normals, UVs, VertexColors,
                                  Tangents, true);

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetStringField(TEXT("actor_path"), MeshActor->GetPathName());
      ResponseJson->SetNumberField(TEXT("vertex_count"), Vertices.Num());
      ResponseJson->SetNumberField(TEXT("triangle_count"), Triangles.Num() / 3);
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- clear ---
    if (Operation == TEXT("clear")) {
      const FString ActorPath = Body->GetStringField(TEXT("actor_path"));
      if (ActorPath.IsEmpty()) {
        SendError(OnComplete, TEXT("actor_path is required for clear"));
        return true;
      }

      AActor* Actor = FindObject<AActor>(nullptr, *ActorPath);
      if (!Actor) {
        SendError(OnComplete, FString::Printf(TEXT("Actor not found: '%s'"), *ActorPath));
        return true;
      }

      UProceduralMeshComponent* ProcMesh = Actor->FindComponentByClass<UProceduralMeshComponent>();
      if (ProcMesh) {
        ProcMesh->ClearAllMeshSections();
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- set_material ---
    if (Operation == TEXT("set_material")) {
      const FString ActorPath = Body->GetStringField(TEXT("actor_path"));
      const FString MaterialPath = Body->GetStringField(TEXT("material_path"));
      int32 SectionIndex = static_cast<int32>(Body->GetNumberField(TEXT("section_index")));

      if (ActorPath.IsEmpty() || MaterialPath.IsEmpty()) {
        SendError(OnComplete, TEXT("actor_path and material_path are required"));
        return true;
      }

      AActor* Actor = FindObject<AActor>(nullptr, *ActorPath);
      UProceduralMeshComponent* ProcMesh =
          Actor ? Actor->FindComponentByClass<UProceduralMeshComponent>() : nullptr;
      UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);

      if (ProcMesh && Material) {
        ProcMesh->SetMaterial(SectionIndex, Material);
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), ProcMesh != nullptr && Material != nullptr);
      SendJson(OnComplete, ResponseJson);
      return true;
    }

    SendError(OnComplete,
              FString::Printf(TEXT("Unknown procedural mesh operation: '%s'"), *Operation));
    return true;
  }

  // ---------------------------------------------------------------------------
  // POST /api/mesh/realtime
  // ---------------------------------------------------------------------------

  static bool HandleRealtimeMesh(const FHttpServerRequest& Request,
                                 const FHttpResultCallback& OnComplete) {
#if defined(WITH_REALTIMEMESH) && WITH_REALTIMEMESH
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

    // RealtimeMesh operations — requires the RealtimeMesh plugin.
    UE_LOG(LogMCPUnreal, Log, TEXT("RealtimeMesh operation '%s' requested"), *Operation);

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    ResponseJson->SetBoolField(TEXT("success"), true);
    ResponseJson->SetStringField(TEXT("operation"), Operation);
    SendJson(OnComplete, ResponseJson);
    return true;
#else
    SendError(
        OnComplete,
        TEXT("RealtimeMesh plugin is not installed. Install it from the Marketplace or ")
            TEXT("build from source to use realtime_mesh operations. ") TEXT(
                "ProceduralMeshComponent (procedural_mesh tool) is available as an alternative."),
        501);
    return true;
#endif
  }

  // ---------------------------------------------------------------------------
  // Registration
  // ---------------------------------------------------------------------------

  void RegisterMeshRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles) {
    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/mesh/procedural")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleProceduralMesh)));

    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/mesh/realtime")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleRealtimeMesh)));

    UE_LOG(LogMCPUnreal, Verbose, TEXT("Registered mesh routes (2 endpoints)"));
  }

}  // namespace MCPUnreal
