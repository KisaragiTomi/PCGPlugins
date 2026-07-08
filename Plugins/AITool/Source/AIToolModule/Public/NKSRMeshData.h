#pragma once

#include "CoreMinimal.h"
#include "NKSRMeshData.generated.h"

/** Reconstruction output mesh, Blueprint-friendly. Triangles = flattened index triples. */
USTRUCT(BlueprintType)
struct AITOOLMODULE_API FNKSRMeshData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "NKSR")
	TArray<FVector> Vertices;

	UPROPERTY(BlueprintReadOnly, Category = "NKSR")
	TArray<int32> Triangles;
};
