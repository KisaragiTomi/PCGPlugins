#pragma once

// Point-cloud / mesh file IO. All functions return false + OutError instead of asserting.
// Formats: PLY (ascii + binary_little_endian), OBJ (v/vn), XYZ/TXT/CSV (whitespace or comma),
// NPY (v1.0/2.0, float32/float64, C-order, (N,3) or (N,>=6)). NPY read/write also serves golden dumps.

#include "NKSRCommon.h"

struct FNKSRMeshBuffers
{
	TArray<FVector3f> Vertices;
	TArray<FIntVector> Triangles;
};

/** Dispatches on extension (.ply/.obj/.xyz/.txt/.csv/.npy). OutNormals empty when the file has none. */
bool NKSRLoadPointCloudFile(const FString& Path, TArray<FVector3f>& OutPoints, TArray<FVector3f>& OutNormals, FString& OutError);

/** Wavefront OBJ with v / f (1-based) lines. Creates the directory if needed. */
bool NKSRSaveObj(const FString& Path, const FNKSRMeshBuffers& Mesh, FString& OutError);

// --- NPY helpers (v1.0 header, little-endian, C-order) ---

bool NKSRLoadNpyFloat(const FString& Path, FNKSRMatrix& Out, FString& OutError);
bool NKSRSaveNpyFloat(const FString& Path, const FNKSRMatrix& In, FString& OutError);
bool NKSRSaveNpyInt64(const FString& Path, TConstArrayView<int64> Data, int32 Rows, int32 Cols, FString& OutError);
bool NKSRSaveNpyInt32(const FString& Path, TConstArrayView<int32> Data, int32 Rows, int32 Cols, FString& OutError);
