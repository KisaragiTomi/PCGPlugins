#pragma once

#include "CoreMinimal.h"
#include "RHI.h"
#include "RHIResources.h"

class UHairMeshAsset;

/**
 * CPU-side data for one 3D texture volume.
 * Stored as RGBA float16 (FFloat16Color), linearised row-major: [z][y][x].
 */
struct FHairMeshVolumeData
{
	int32 SizeX = 0;
	int32 SizeY = 0;
	int32 SizeZ = 0;

	TArray<FLinearColor> Pixels;

	void Allocate(int32 X, int32 Y, int32 Z)
	{
		SizeX = X; SizeY = Y; SizeZ = Z;
		Pixels.SetNumZeroed(X * Y * Z);
	}

	int32 Index(int32 X, int32 Y, int32 Z) const
	{
		return Z * SizeX * SizeY + Y * SizeX + X;
	}

	FLinearColor& At(int32 X, int32 Y, int32 Z) { return Pixels[Index(X, Y, Z)]; }
	const FLinearColor& At(int32 X, int32 Y, int32 Z) const { return Pixels[Index(X, Y, Z)]; }
};

/** CPU-side data for one 2D texture. */
struct FHairMeshSliceData
{
	int32 SizeX = 0;
	int32 SizeY = 0;

	TArray<FLinearColor> Pixels;

	void Allocate(int32 X, int32 Y)
	{
		SizeX = X; SizeY = Y;
		Pixels.SetNumZeroed(X * Y);
	}

	int32 Index(int32 X, int32 Y) const { return Y * SizeX + X; }

	FLinearColor& At(int32 X, int32 Y) { return Pixels[Index(X, Y)]; }
	const FLinearColor& At(int32 X, int32 Y) const { return Pixels[Index(X, Y)]; }
};

/**
 * Mapping from a bundle index to its 2×2 texel block on the root slice.
 */
struct FBundleTexelMapping
{
	int32 BlockX = 0;  // top-left texel X of the 2x2 block
	int32 BlockY = 0;  // top-left texel Y of the 2x2 block
};

/**
 * Complete CPU-side result of building the 5 hair mesh textures.
 * The scene proxy uploads these to GPU on the render thread.
 */
struct FHairMeshTextureSet
{
	/** 3D — object-space vertex positions with quadratic Bézier intermediate slices */
	FHairMeshVolumeData HairMeshVolume;

	/** 2D — root-layer uv styling coordinates */
	FHairMeshSliceData UVSlice;

	/** 3D — per-vertex w styling coordinate */
	FHairMeshVolumeData WVolume;

	/** 3D — object-space styling direction û */
	FHairMeshVolumeData UDirVolume;

	/** 3D — object-space styling direction v̂ */
	FHairMeshVolumeData VDirVolume;

	/** Per-bundle mapping to root slice texel blocks */
	TArray<FBundleTexelMapping> BundleMappings;

	int32 SliceWidth = 0;
	int32 SliceHeight = 0;
	int32 Num3DSlices = 0;

	bool IsValid() const { return SliceWidth > 0 && SliceHeight > 0 && Num3DSlices > 0; }
};

/**
 * Builds the 5 GPU textures required for hair-mesh rendering.
 *
 * Algorithm overview (per the SIGGRAPH 2024 paper):
 * 1. Layout root slice: each scalp face (quad/tri) → 2×2 texel block
 * 2. For each pair of successive layers: Catmull-Rom → cubic Bézier → 2 quadratic Bézier
 * 3. Place quadratic control points on 3 intermediate slices between layer slices
 * 4. Fill UV texture (root uv) and W texture (per-layer w)
 * 5. Compute û/v̂ per vertex via area-weighted average (Eq.1-4)
 */
class HAIRMESHRENDERING_API FHairMeshTextureBuilder
{
public:
	static FHairMeshTextureSet Build(const UHairMeshAsset* Asset);

private:
	// --- Slice Layout ---
	static void ComputeRootSliceLayout(
		int32 NumBundles,
		int32& OutBlocksPerRow,
		int32& OutSliceWidth,
		int32& OutSliceHeight);

	static void ComputeTextureDimensions(
		const UHairMeshAsset* Asset,
		int32& OutSliceW, int32& OutSliceH, int32& OutNumSlices,
		TArray<FBundleTexelMapping>& OutMappings);

	// --- Bézier Conversion ---
	static void CatmullRomToCubicBezier(
		const FVector& P_prev, const FVector& P0, const FVector& P1, const FVector& P_next,
		FVector& OutB0, FVector& OutB1, FVector& OutB2, FVector& OutB3);

	static void CubicToTwoQuadraticBezier(
		const FVector& B0, const FVector& B1, const FVector& B2, const FVector& B3,
		FVector& OutQ0, FVector& OutQ1, FVector& OutQ2,
		FVector& OutQ3, FVector& OutQ4);

	// --- Fill Slices ---
	static void FillRootSlice(
		const UHairMeshAsset* Asset,
		const TArray<FBundleTexelMapping>& Mappings,
		FHairMeshVolumeData& HairMeshVol,
		FHairMeshSliceData& UVSlice);

	static void FillLayerAndIntermediateSlices(
		const UHairMeshAsset* Asset,
		const TArray<FBundleTexelMapping>& Mappings,
		FHairMeshVolumeData& HairMeshVol,
		FHairMeshVolumeData& WVol);

	// --- Styling Directions ---
	static void ComputeStylingDirections(
		const UHairMeshAsset* Asset,
		const TArray<FBundleTexelMapping>& Mappings,
		FHairMeshVolumeData& UDirVol,
		FHairMeshVolumeData& VDirVol);

	/**
	 * Compute û for vertex at given index on given layer.
	 * Uses area-weighted average of surrounding triangles (Eq.1-3).
	 */
	static FVector ComputeVertexUDirection(
		const UHairMeshAsset* Asset,
		int32 VertexIndex, int32 LayerIndex,
		const TMap<int32, TArray<int32>>& VertexToFaces);

	static FVector ComputeVertexVDirection(
		const UHairMeshAsset* Asset,
		int32 VertexIndex, int32 LayerIndex,
		const TMap<int32, TArray<int32>>& VertexToFaces);
};
