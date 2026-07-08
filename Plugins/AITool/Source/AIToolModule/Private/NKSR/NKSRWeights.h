#pragma once

// .nkw weight bundle loader (converted from ks.pth by Tools/convert_checkpoint.py).
// Format: header {u32 magic 'NKSW', u32 version=1, u32 count, u32 reserved}
// then per tensor {u16 nameLen, utf8 name, u8 dtype(0=f32), u8 ndim, u32 dims[ndim], f32 data[]} — all little-endian.
// Tensor inventory & shapes: Docs/PortSpecs/F_checkpoint_report.md / F_checkpoint_inventory.json.

#include "NKSRCommon.h"

class FNKSRWeightStore
{
public:
	/**
	 * Loads Resources/nksr_ks.nkw once (thread-safe); returns nullptr with OutError on failure
	 * (missing file, bad magic, malformed entry). Never asserts.
	 */
	static const FNKSRWeightStore* Get(FString& OutError);

	/** nullptr if the key is absent. Multi-dim tensors are flattened: Rows = dim0, Cols = product of the rest. */
	const FNKSRMatrix* Find(const FString& Key) const;

	/**
	 * Find + shape check. ExpectedDims matched against the original ndim dims (not the flattened 2D view).
	 * On mismatch returns nullptr and appends a message to OutError.
	 */
	const FNKSRMatrix* FindChecked(const FString& Key, TConstArrayView<int32> ExpectedDims, FString& OutError) const;

	const TMap<FString, TArray<int32>>& AllShapes() const { return Shapes; }

private:
	bool LoadFromFile(const FString& Path, FString& OutError);

	TMap<FString, FNKSRMatrix> Tensors;
	TMap<FString, TArray<int32>> Shapes;
};
