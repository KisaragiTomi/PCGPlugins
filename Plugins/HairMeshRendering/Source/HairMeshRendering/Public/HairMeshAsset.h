#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "HairMeshAsset.generated.h"

/**
 * A single layer vertex: position + styling metadata.
 * Hair mesh is built from a root layer (scalp) extruded outward into
 * successive layers; each vertex sits on one layer of one bundle.
 */
USTRUCT(BlueprintType)
struct FHairMeshVertex
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FVector Position = FVector::ZeroVector;

	/** Styling texture-space coordinate (u,v on root layer, w along extrusion) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FVector UVW = FVector::ZeroVector;
};

/**
 * A face on the scalp mesh (root layer).
 * Quads store 4 indices; triangles duplicate one vertex to fill a 2x2 texel block.
 */
USTRUCT(BlueprintType)
struct FHairMeshFace
{
	GENERATED_BODY()

	/** 4 vertex indices (for triangles, last index duplicates one vertex) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<int32> VertexIndices;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bIsTriangle = false;
};

/**
 * One bundle of the hair mesh — corresponds to a single root-layer face
 * and all its extrusion layers.
 */
USTRUCT(BlueprintType)
struct FHairMeshBundle
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FHairMeshFace RootFace;

	/** Number of extrusion layers (excluding the root layer) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 NumLayers = 0;

	/**
	 * Vertex indices per layer, stored row-major:
	 * Layer k -> Indices[k * NumFaceVerts .. (k+1) * NumFaceVerts - 1]
	 * Layer 0 = root layer (same as RootFace.VertexIndices).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<int32> LayerVertexIndices;

	/** Target strand count at maximum detail */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="1"))
	int32 MaxStrandsPerBundle = 256;
};

/**
 * Core asset that stores the volumetric hair-mesh topology.
 * At build time this is converted into 5 GPU textures by FHairMeshTextureBuilder.
 */
UCLASS(BlueprintType)
class HAIRMESHRENDERING_API UHairMeshAsset : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="HairMesh")
	TArray<FHairMeshVertex> Vertices;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="HairMesh")
	TArray<FHairMeshBundle> Bundles;

	/** Total extrusion layers (uniform across all bundles for simplicity) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="HairMesh", meta=(ClampMin="1"))
	int32 NumExtrusionLayers = 3;

	/** Maximum strands per bundle (can be overridden per bundle) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="HairMesh", meta=(ClampMin="1"))
	int32 DefaultMaxStrands = 256;

	/** Vertices per strand at highest detail (must be 2^e + 1) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="HairMesh", meta=(ClampMin="3"))
	int32 MaxVerticesPerStrand = 33;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
