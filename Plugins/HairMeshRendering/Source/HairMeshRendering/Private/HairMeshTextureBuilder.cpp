#include "HairMeshTextureBuilder.h"
#include "HairMeshAsset.h"

// ============================================================================
// Public entry
// ============================================================================

FHairMeshTextureSet FHairMeshTextureBuilder::Build(const UHairMeshAsset* Asset)
{
	FHairMeshTextureSet Result;
	if (!Asset || Asset->Bundles.Num() == 0 || Asset->Vertices.Num() == 0)
	{
		return Result;
	}

	// 1. Compute dimensions
	ComputeTextureDimensions(Asset,
		Result.SliceWidth, Result.SliceHeight, Result.Num3DSlices,
		Result.BundleMappings);

	const int32 W = Result.SliceWidth;
	const int32 H = Result.SliceHeight;
	const int32 D = Result.Num3DSlices;

	// 2. Allocate CPU buffers
	Result.HairMeshVolume.Allocate(W, H, D);
	Result.UVSlice.Allocate(W, H);
	Result.WVolume.Allocate(W, H, D);
	Result.UDirVolume.Allocate(W, H, D);
	Result.VDirVolume.Allocate(W, H, D);

	// 3. Fill root slice (z=0) of HairMeshVolume + UV slice
	FillRootSlice(Asset, Result.BundleMappings, Result.HairMeshVolume, Result.UVSlice);

	// 4. Fill layer and intermediate slices for HairMeshVolume + W volume
	FillLayerAndIntermediateSlices(Asset, Result.BundleMappings, Result.HairMeshVolume, Result.WVolume);

	// 5. Compute styling directions û, v̂
	ComputeStylingDirections(Asset, Result.BundleMappings, Result.UDirVolume, Result.VDirVolume);

	return Result;
}

// ============================================================================
// Slice Layout
// ============================================================================

void FHairMeshTextureBuilder::ComputeRootSliceLayout(
	int32 NumBundles,
	int32& OutBlocksPerRow,
	int32& OutSliceWidth,
	int32& OutSliceHeight)
{
	OutBlocksPerRow = FMath::CeilToInt32(FMath::Sqrt(static_cast<float>(NumBundles)));
	if (OutBlocksPerRow < 1) OutBlocksPerRow = 1;

	int32 NumRows = FMath::DivideAndRoundUp(NumBundles, OutBlocksPerRow);

	// Each block is 2x2 texels
	OutSliceWidth = OutBlocksPerRow * 2;
	OutSliceHeight = NumRows * 2;
}

void FHairMeshTextureBuilder::ComputeTextureDimensions(
	const UHairMeshAsset* Asset,
	int32& OutSliceW, int32& OutSliceH, int32& OutNumSlices,
	TArray<FBundleTexelMapping>& OutMappings)
{
	const int32 NumBundles = Asset->Bundles.Num();
	const int32 NumLayers = Asset->NumExtrusionLayers;

	int32 BlocksPerRow;
	ComputeRootSliceLayout(NumBundles, BlocksPerRow, OutSliceW, OutSliceH);

	// 3D depth: root slice + N segments * (3 intermediate + 1 layer)
	// = 1 + 4 * NumLayers
	OutNumSlices = 1 + 4 * NumLayers;

	OutMappings.SetNum(NumBundles);
	for (int32 i = 0; i < NumBundles; ++i)
	{
		int32 Col = i % BlocksPerRow;
		int32 Row = i / BlocksPerRow;
		OutMappings[i].BlockX = Col * 2;
		OutMappings[i].BlockY = Row * 2;
	}
}

// ============================================================================
// Bézier Conversion
// ============================================================================

void FHairMeshTextureBuilder::CatmullRomToCubicBezier(
	const FVector& P_prev, const FVector& P0, const FVector& P1, const FVector& P_next,
	FVector& OutB0, FVector& OutB1, FVector& OutB2, FVector& OutB3)
{
	// Standard Catmull-Rom to cubic Bézier conversion (uniform parameterization)
	// B0 = P0
	// B1 = P0 + (P1 - P_prev) / 6
	// B2 = P1 - (P_next - P0) / 6
	// B3 = P1
	OutB0 = P0;
	OutB1 = P0 + (P1 - P_prev) / 6.0;
	OutB2 = P1 - (P_next - P0) / 6.0;
	OutB3 = P1;
}

void FHairMeshTextureBuilder::CubicToTwoQuadraticBezier(
	const FVector& B0, const FVector& B1, const FVector& B2, const FVector& B3,
	FVector& OutQ0, FVector& OutQ1, FVector& OutQ2,
	FVector& OutQ3, FVector& OutQ4)
{
	// Truong et al. 2020: approximate one cubic Bézier as two quadratic Bézier curves
	// Midpoint of the cubic curve
	FVector Mid = (B0 + 3.0 * B1 + 3.0 * B2 + B3) / 8.0;

	// First quadratic: Q0 = B0, Q1 = (3*B1 - B0)/2, Q2 = Mid
	OutQ0 = B0;
	OutQ1 = (3.0 * B1 - B0) / 2.0;
	OutQ2 = Mid;

	// Second quadratic: Q3 = (3*B2 - B3)/2, Q4 = B3
	OutQ3 = (3.0 * B2 - B3) / 2.0;
	OutQ4 = B3;
}

// ============================================================================
// Fill Root Slice
// ============================================================================

static FLinearColor Vec3ToColor(const FVector& V)
{
	return FLinearColor(V.X, V.Y, V.Z, 1.0f);
}

void FHairMeshTextureBuilder::FillRootSlice(
	const UHairMeshAsset* Asset,
	const TArray<FBundleTexelMapping>& Mappings,
	FHairMeshVolumeData& HairMeshVol,
	FHairMeshSliceData& UVSlice)
{
	const int32 RootSliceZ = 0;

	for (int32 BundleIdx = 0; BundleIdx < Asset->Bundles.Num(); ++BundleIdx)
	{
		const FHairMeshBundle& Bundle = Asset->Bundles[BundleIdx];
		const FHairMeshFace& Face = Bundle.RootFace;
		const FBundleTexelMapping& Map = Mappings[BundleIdx];

		if (Face.VertexIndices.Num() < 3) continue;

		// Get the 4 vertex indices (for triangle: 4th = duplicate of one vertex)
		int32 Idx[4];
		if (Face.bIsTriangle && Face.VertexIndices.Num() == 3)
		{
			Idx[0] = Face.VertexIndices[0];
			Idx[1] = Face.VertexIndices[1];
			Idx[2] = Face.VertexIndices[2];
			Idx[3] = Face.VertexIndices[2]; // duplicate last
		}
		else
		{
			for (int32 i = 0; i < 4; ++i)
			{
				Idx[i] = (i < Face.VertexIndices.Num()) ? Face.VertexIndices[i] : Face.VertexIndices[0];
			}
		}

		// 2x2 block layout:
		// (BlockX,   BlockY)   = Idx[0]    (BlockX+1, BlockY)   = Idx[1]
		// (BlockX,   BlockY+1) = Idx[2]    (BlockX+1, BlockY+1) = Idx[3]
		const int32 BX = Map.BlockX;
		const int32 BY = Map.BlockY;

		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			int32 LX = BX + (Corner % 2);
			int32 LY = BY + (Corner / 2);
			const FHairMeshVertex& V = Asset->Vertices[Idx[Corner]];

			// Hair mesh volume: root slice stores positions
			HairMeshVol.At(LX, LY, RootSliceZ) = Vec3ToColor(V.Position);

			// UV slice: stores uv coords (w is 0 at root)
			UVSlice.At(LX, LY) = FLinearColor(V.UVW.X, V.UVW.Y, 0.0f, 1.0f);
		}
	}
}

// ============================================================================
// Fill Layer + Intermediate Slices
// ============================================================================

void FHairMeshTextureBuilder::FillLayerAndIntermediateSlices(
	const UHairMeshAsset* Asset,
	const TArray<FBundleTexelMapping>& Mappings,
	FHairMeshVolumeData& HairMeshVol,
	FHairMeshVolumeData& WVol)
{
	const int32 NumLayers = Asset->NumExtrusionLayers;

	for (int32 BundleIdx = 0; BundleIdx < Asset->Bundles.Num(); ++BundleIdx)
	{
		const FHairMeshBundle& Bundle = Asset->Bundles[BundleIdx];
		const FHairMeshFace& Face = Bundle.RootFace;
		const FBundleTexelMapping& Map = Mappings[BundleIdx];

		if (Face.VertexIndices.Num() < 3) continue;

		const int32 NumFaceVerts = Face.bIsTriangle ? 3 : 4;
		const int32 StoredVerts = 4; // always 4 corners in 2x2 block

		// For each corner of the 2x2 block, gather positions across all layers
		for (int32 Corner = 0; Corner < StoredVerts; ++Corner)
		{
			int32 LX = Map.BlockX + (Corner % 2);
			int32 LY = Map.BlockY + (Corner / 2);

			// Collect positions for this corner across layers: P[0]=root, P[1..N]=extrusion layers
			TArray<FVector> Positions;
			TArray<float> WValues;
			Positions.SetNum(NumLayers + 1);
			WValues.SetNum(NumLayers + 1);

			// Root layer (already written, but we need it for spline computation)
			{
				int32 VertIdx;
				if (Face.bIsTriangle && Corner == 3)
					VertIdx = Face.VertexIndices[2]; // duplicate
				else if (Corner < Face.VertexIndices.Num())
					VertIdx = Face.VertexIndices[Corner];
				else
					VertIdx = Face.VertexIndices[0];

				Positions[0] = Asset->Vertices[VertIdx].Position;
				WValues[0] = Asset->Vertices[VertIdx].UVW.Z;
			}

			// Extrusion layers
			for (int32 Layer = 1; Layer <= NumLayers; ++Layer)
			{
				int32 CornerInLayer = FMath::Min(Corner, NumFaceVerts - 1);
				int32 LayerVertIndexInArray = Layer * NumFaceVerts + CornerInLayer;

				if (LayerVertIndexInArray < Bundle.LayerVertexIndices.Num())
				{
					int32 GlobalVertIdx = Bundle.LayerVertexIndices[LayerVertIndexInArray];
					if (GlobalVertIdx >= 0 && GlobalVertIdx < Asset->Vertices.Num())
					{
						Positions[Layer] = Asset->Vertices[GlobalVertIdx].Position;
						WValues[Layer] = Asset->Vertices[GlobalVertIdx].UVW.Z;
					}
				}
			}

			// For each segment between consecutive layers, compute Bézier control points
			for (int32 Seg = 0; Seg < NumLayers; ++Seg)
			{
				// Catmull-Rom needs 4 points: P[Seg-1], P[Seg], P[Seg+1], P[Seg+2]
				FVector P_prev = (Seg > 0) ? Positions[Seg - 1] : (2.0 * Positions[0] - Positions[1]);
				FVector P0 = Positions[Seg];
				FVector P1 = Positions[Seg + 1];
				FVector P_next = (Seg + 2 <= NumLayers) ? Positions[Seg + 2] : (2.0 * Positions[NumLayers] - Positions[NumLayers - 1]);

				// Catmull-Rom → cubic Bézier
				FVector CB0, CB1, CB2, CB3;
				CatmullRomToCubicBezier(P_prev, P0, P1, P_next, CB0, CB1, CB2, CB3);

				// Cubic → 2 quadratic Bézier
				FVector Q0, Q1, Q2, Q3, Q4;
				CubicToTwoQuadraticBezier(CB0, CB1, CB2, CB3, Q0, Q1, Q2, Q3, Q4);

				// 3D slice layout:
				// Slice 0                     = root layer
				// Slice 1 + Seg*4 + 0         = Q1 (first quadratic control point)
				// Slice 1 + Seg*4 + 1         = Q2 = Mid (shared endpoint)
				// Slice 1 + Seg*4 + 2         = Q3 (second quadratic control point)
				// Slice 1 + Seg*4 + 3         = layer Seg+1 position (Q4)
				int32 BaseSlice = 1 + Seg * 4;

				HairMeshVol.At(LX, LY, BaseSlice + 0) = Vec3ToColor(Q1);
				HairMeshVol.At(LX, LY, BaseSlice + 1) = Vec3ToColor(Q2);
				HairMeshVol.At(LX, LY, BaseSlice + 2) = Vec3ToColor(Q3);
				HairMeshVol.At(LX, LY, BaseSlice + 3) = Vec3ToColor(Q4);

				// W values: interpolate linearly for intermediate slices
				float W0 = WValues[Seg];
				float W1 = WValues[Seg + 1];

				WVol.At(LX, LY, BaseSlice + 0) = FLinearColor(FMath::Lerp(W0, W1, 0.25f), 0, 0, 1);
				WVol.At(LX, LY, BaseSlice + 1) = FLinearColor(FMath::Lerp(W0, W1, 0.50f), 0, 0, 1);
				WVol.At(LX, LY, BaseSlice + 2) = FLinearColor(FMath::Lerp(W0, W1, 0.75f), 0, 0, 1);
				WVol.At(LX, LY, BaseSlice + 3) = FLinearColor(W1, 0, 0, 1);
			}

			// W for root slice
			WVol.At(LX, LY, 0) = FLinearColor(WValues[0], 0, 0, 1);
		}
	}
}

// ============================================================================
// Styling Directions (Eq. 1-4)
// ============================================================================

void FHairMeshTextureBuilder::ComputeStylingDirections(
	const UHairMeshAsset* Asset,
	const TArray<FBundleTexelMapping>& Mappings,
	FHairMeshVolumeData& UDirVol,
	FHairMeshVolumeData& VDirVol)
{
	const int32 NumLayers = Asset->NumExtrusionLayers;

	// Build adjacency: vertex index → list of bundle indices that use it
	TMap<int32, TArray<int32>> VertexToBundles;
	for (int32 BundleIdx = 0; BundleIdx < Asset->Bundles.Num(); ++BundleIdx)
	{
		const FHairMeshBundle& Bundle = Asset->Bundles[BundleIdx];
		for (int32 Idx : Bundle.RootFace.VertexIndices)
		{
			VertexToBundles.FindOrAdd(Idx).AddUnique(BundleIdx);
		}
		for (int32 Idx : Bundle.LayerVertexIndices)
		{
			VertexToBundles.FindOrAdd(Idx).AddUnique(BundleIdx);
		}
	}

	// For each bundle, fill the U/V direction at each corner across all layers
	for (int32 BundleIdx = 0; BundleIdx < Asset->Bundles.Num(); ++BundleIdx)
	{
		const FHairMeshBundle& Bundle = Asset->Bundles[BundleIdx];
		const FHairMeshFace& Face = Bundle.RootFace;
		const FBundleTexelMapping& Map = Mappings[BundleIdx];

		if (Face.VertexIndices.Num() < 3) continue;
		const int32 NumFaceVerts = Face.bIsTriangle ? 3 : 4;

		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			int32 LX = Map.BlockX + (Corner % 2);
			int32 LY = Map.BlockY + (Corner / 2);

			for (int32 Layer = 0; Layer <= NumLayers; ++Layer)
			{
				int32 VertIdx;
				if (Layer == 0)
				{
					int32 C = (Face.bIsTriangle && Corner == 3) ? 2 : FMath::Min(Corner, Face.VertexIndices.Num() - 1);
					VertIdx = Face.VertexIndices[C];
				}
				else
				{
					int32 C = FMath::Min(Corner, NumFaceVerts - 1);
					int32 ArrayIdx = Layer * NumFaceVerts + C;
					VertIdx = (ArrayIdx < Bundle.LayerVertexIndices.Num()) ? Bundle.LayerVertexIndices[ArrayIdx] : -1;
				}

				FVector UDir = FVector::ForwardVector;
				FVector VDir = FVector::RightVector;

				if (VertIdx >= 0 && VertIdx < Asset->Vertices.Num())
				{
					UDir = ComputeVertexUDirection(Asset, VertIdx, Layer, VertexToBundles);
					VDir = ComputeVertexVDirection(Asset, VertIdx, Layer, VertexToBundles);
				}

				// Map layer to 3D slices: root=0, layer k = 1 + (k-1)*4 + 3 (for k>=1)
				int32 SliceZ;
				if (Layer == 0) SliceZ = 0;
				else SliceZ = 1 + (Layer - 1) * 4 + 3;

				UDirVol.At(LX, LY, SliceZ) = Vec3ToColor(UDir);
				VDirVol.At(LX, LY, SliceZ) = Vec3ToColor(VDir);
			}
		}
	}
}

FVector FHairMeshTextureBuilder::ComputeVertexUDirection(
	const UHairMeshAsset* Asset,
	int32 VertexIndex, int32 LayerIndex,
	const TMap<int32, TArray<int32>>& VertexToBundles)
{
	// Eq.1-3: area-weighted average of u directions from surrounding triangles
	// For each bundle containing this vertex, construct the triangle(s) at this layer
	// that include this vertex, compute the linear mapping T_i, and accumulate.

	const TArray<int32>* Bundles = VertexToBundles.Find(VertexIndex);
	if (!Bundles || Bundles->Num() == 0)
	{
		return FVector::ForwardVector;
	}

	FVector Accumulated = FVector::ZeroVector;
	float TotalArea = 0.0f;

	for (int32 BundleIdx : *Bundles)
	{
		const FHairMeshBundle& Bundle = Asset->Bundles[BundleIdx];
		const FHairMeshFace& Face = Bundle.RootFace;
		const int32 NumFaceVerts = Face.bIsTriangle ? 3 : 4;

		// Get vertex indices for this layer
		TArray<int32, TInlineAllocator<4>> LayerVerts;
		TArray<FVector, TInlineAllocator<4>> LayerPositions;
		TArray<FVector2D, TInlineAllocator<4>> LayerUVs;

		if (LayerIndex == 0)
		{
			for (int32 i = 0; i < NumFaceVerts; ++i)
			{
				int32 VI = Face.VertexIndices[i];
				LayerVerts.Add(VI);
				LayerPositions.Add(Asset->Vertices[VI].Position);
				LayerUVs.Add(FVector2D(Asset->Vertices[VI].UVW.X, Asset->Vertices[VI].UVW.Y));
			}
		}
		else
		{
			for (int32 i = 0; i < NumFaceVerts; ++i)
			{
				int32 ArrayIdx = LayerIndex * NumFaceVerts + i;
				if (ArrayIdx < Bundle.LayerVertexIndices.Num())
				{
					int32 VI = Bundle.LayerVertexIndices[ArrayIdx];
					LayerVerts.Add(VI);
					if (VI >= 0 && VI < Asset->Vertices.Num())
					{
						LayerPositions.Add(Asset->Vertices[VI].Position);
						LayerUVs.Add(FVector2D(Asset->Vertices[VI].UVW.X, Asset->Vertices[VI].UVW.Y));
					}
				}
			}
		}

		if (LayerPositions.Num() < 3) continue;

		// Triangulate face and process each triangle containing VertexIndex
		auto ProcessTriangle = [&](int32 I0, int32 I1, int32 I2)
		{
			// Check if VertexIndex is in this triangle
			bool bContains = (LayerVerts[I0] == VertexIndex) ||
							 (LayerVerts[I1] == VertexIndex) ||
							 (LayerVerts[I2] == VertexIndex);
			if (!bContains) return;

			const FVector& P0 = LayerPositions[I0];
			const FVector& P1 = LayerPositions[I1];
			const FVector& P2 = LayerPositions[I2];

			const FVector2D& UV0 = LayerUVs[I0];
			const FVector2D& UV1 = LayerUVs[I1];
			const FVector2D& UV2 = LayerUVs[I2];

			// Texture-space area (Eq.2 denominator)
			float TextureArea = FMath::Abs(
				(UV1.X - UV0.X) * (UV2.Y - UV0.Y) - (UV2.X - UV0.X) * (UV1.Y - UV0.Y)) * 0.5f;

			if (TextureArea < SMALL_NUMBER) return;

			// T_i^{-1} (Eq.3): map texture-space u-hat to object-space
			// u_i = (P1-P0, P2-P0) * T_i^{-1} * [1, 0]^T
			float du1 = UV1.X - UV0.X;
			float dv1 = UV1.Y - UV0.Y;
			float du2 = UV2.X - UV0.X;
			float dv2 = UV2.Y - UV0.Y;

			float Det = du1 * dv2 - du2 * dv1;
			if (FMath::Abs(Det) < SMALL_NUMBER) return;

			float InvDet = 1.0f / Det;

			// T_i^{-1} columns: first column maps [1,0] (u direction)
			float InvT00 = dv2 * InvDet;
			float InvT10 = -dv1 * InvDet;

			FVector Edge1 = P1 - P0;
			FVector Edge2 = P2 - P0;

			FVector UDir_i = Edge1 * InvT00 + Edge2 * InvT10;

			Accumulated += TextureArea * UDir_i;
			TotalArea += TextureArea;
		};

		// First triangle: 0,1,2
		ProcessTriangle(0, 1, 2);
		// Second triangle (if quad): 0,2,3
		if (NumFaceVerts >= 4 && LayerPositions.Num() >= 4)
		{
			ProcessTriangle(0, 2, 3);
		}
	}

	if (TotalArea > SMALL_NUMBER)
	{
		FVector Result = Accumulated / TotalArea;
		if (!Result.Normalize())
		{
			return FVector::ForwardVector;
		}
		return Result;
	}

	return FVector::ForwardVector;
}

FVector FHairMeshTextureBuilder::ComputeVertexVDirection(
	const UHairMeshAsset* Asset,
	int32 VertexIndex, int32 LayerIndex,
	const TMap<int32, TArray<int32>>& VertexToBundles)
{
	// Same structure as ComputeVertexUDirection but extracts v direction

	const TArray<int32>* Bundles = VertexToBundles.Find(VertexIndex);
	if (!Bundles || Bundles->Num() == 0)
	{
		return FVector::RightVector;
	}

	FVector Accumulated = FVector::ZeroVector;
	float TotalArea = 0.0f;

	for (int32 BundleIdx : *Bundles)
	{
		const FHairMeshBundle& Bundle = Asset->Bundles[BundleIdx];
		const FHairMeshFace& Face = Bundle.RootFace;
		const int32 NumFaceVerts = Face.bIsTriangle ? 3 : 4;

		TArray<int32, TInlineAllocator<4>> LayerVerts;
		TArray<FVector, TInlineAllocator<4>> LayerPositions;
		TArray<FVector2D, TInlineAllocator<4>> LayerUVs;

		if (LayerIndex == 0)
		{
			for (int32 i = 0; i < NumFaceVerts; ++i)
			{
				int32 VI = Face.VertexIndices[i];
				LayerVerts.Add(VI);
				LayerPositions.Add(Asset->Vertices[VI].Position);
				LayerUVs.Add(FVector2D(Asset->Vertices[VI].UVW.X, Asset->Vertices[VI].UVW.Y));
			}
		}
		else
		{
			for (int32 i = 0; i < NumFaceVerts; ++i)
			{
				int32 ArrayIdx = LayerIndex * NumFaceVerts + i;
				if (ArrayIdx < Bundle.LayerVertexIndices.Num())
				{
					int32 VI = Bundle.LayerVertexIndices[ArrayIdx];
					LayerVerts.Add(VI);
					if (VI >= 0 && VI < Asset->Vertices.Num())
					{
						LayerPositions.Add(Asset->Vertices[VI].Position);
						LayerUVs.Add(FVector2D(Asset->Vertices[VI].UVW.X, Asset->Vertices[VI].UVW.Y));
					}
				}
			}
		}

		if (LayerPositions.Num() < 3) continue;

		auto ProcessTriangle = [&](int32 I0, int32 I1, int32 I2)
		{
			bool bContains = (LayerVerts[I0] == VertexIndex) ||
							 (LayerVerts[I1] == VertexIndex) ||
							 (LayerVerts[I2] == VertexIndex);
			if (!bContains) return;

			const FVector& P0 = LayerPositions[I0];
			const FVector& P1 = LayerPositions[I1];
			const FVector& P2 = LayerPositions[I2];

			const FVector2D& UV0 = LayerUVs[I0];
			const FVector2D& UV1 = LayerUVs[I1];
			const FVector2D& UV2 = LayerUVs[I2];

			float TextureArea = FMath::Abs(
				(UV1.X - UV0.X) * (UV2.Y - UV0.Y) - (UV2.X - UV0.X) * (UV1.Y - UV0.Y)) * 0.5f;

			if (TextureArea < SMALL_NUMBER) return;

			float du1 = UV1.X - UV0.X;
			float dv1 = UV1.Y - UV0.Y;
			float du2 = UV2.X - UV0.X;
			float dv2 = UV2.Y - UV0.Y;

			float Det = du1 * dv2 - du2 * dv1;
			if (FMath::Abs(Det) < SMALL_NUMBER) return;

			float InvDet = 1.0f / Det;

			// T_i^{-1} second column maps [0,1] (v direction)
			float InvT01 = -du2 * InvDet;
			float InvT11 = du1 * InvDet;

			FVector Edge1 = P1 - P0;
			FVector Edge2 = P2 - P0;

			FVector VDir_i = Edge1 * InvT01 + Edge2 * InvT11;

			Accumulated += TextureArea * VDir_i;
			TotalArea += TextureArea;
		};

		ProcessTriangle(0, 1, 2);
		if (NumFaceVerts >= 4 && LayerPositions.Num() >= 4)
		{
			ProcessTriangle(0, 2, 3);
		}
	}

	if (TotalArea > SMALL_NUMBER)
	{
		FVector Result = Accumulated / TotalArea;
		if (!Result.Normalize())
		{
			return FVector::RightVector;
		}
		return Result;
	}

	return FVector::RightVector;
}
