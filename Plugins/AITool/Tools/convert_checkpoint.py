#!/usr/bin/env python3
"""Convert ks_weights.npz (extracted from NKSR ks.pth) into the .nkw bundle
consumed by FNKSRWeightStore (NKSRWeights.h).

.nkw format (all little-endian):
  header: u32 magic 'NKSW' (bytes b'NKSW'), u32 version=1, u32 count, u32 reserved=0
  per tensor: u16 nameLen, utf8 name, u8 dtype (0 = float32), u8 ndim,
              u32 dims[ndim], f32 data[prod(dims)]

Keys are written in lexicographic (sorted) order for determinism.
After writing, the file is re-parsed with an independent reader and validated
against the npz (count, per-key shape, sampled values).
"""

import struct
import sys
import numpy as np

NPZ_PATH = r"D:/MyWork/UnrealProject/UETest574_2/Plugins/AITool/Intermediate/PortWork/ks_weights.npz"
OUT_PATH = r"D:/MyWork/UnrealProject/UETest574_2/Plugins/AITool/Resources/nksr_ks.nkw"

MAGIC = b"NKSW"
VERSION = 1
DTYPE_F32 = 0


def write_nkw(npz, out_path):
    keys = sorted(npz.files)
    with open(out_path, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<III", VERSION, len(keys), 0))
        for key in keys:
            arr = npz[key]
            if arr.dtype != np.float32:
                raise ValueError(f"{key}: unexpected dtype {arr.dtype}, expected float32")
            arr = np.ascontiguousarray(arr, dtype="<f4")
            name = key.encode("utf-8")
            if len(name) > 0xFFFF:
                raise ValueError(f"{key}: name too long")
            f.write(struct.pack("<H", len(name)))
            f.write(name)
            f.write(struct.pack("<BB", DTYPE_F32, arr.ndim))
            for d in arr.shape:
                f.write(struct.pack("<I", d))
            f.write(arr.tobytes(order="C"))
    return keys


def read_nkw(path):
    """Independent parser used only for verification."""
    with open(path, "rb") as f:
        data = f.read()
    off = 0

    def take(n):
        nonlocal off
        if off + n > len(data):
            raise ValueError(f"truncated file at offset {off} (+{n} > {len(data)})")
        chunk = data[off:off + n]
        off += n
        return chunk

    if take(4) != MAGIC:
        raise ValueError("bad magic")
    version, count, reserved = struct.unpack("<III", take(12))
    if version != VERSION:
        raise ValueError(f"bad version {version}")
    tensors = {}
    for _ in range(count):
        (name_len,) = struct.unpack("<H", take(2))
        name = take(name_len).decode("utf-8")
        dtype, ndim = struct.unpack("<BB", take(2))
        if dtype != DTYPE_F32:
            raise ValueError(f"{name}: unsupported dtype {dtype}")
        dims = struct.unpack(f"<{ndim}I", take(4 * ndim))
        numel = int(np.prod(dims, dtype=np.int64)) if ndim > 0 else 1
        arr = np.frombuffer(take(4 * numel), dtype="<f4").reshape(dims)
        if name in tensors:
            raise ValueError(f"duplicate key {name}")
        tensors[name] = arr
    if off != len(data):
        raise ValueError(f"{len(data) - off} trailing bytes")
    return tensors


def verify(npz, tensors):
    npz_keys = sorted(npz.files)
    nkw_keys = sorted(tensors.keys())
    if npz_keys != nkw_keys:
        missing = set(npz_keys) - set(nkw_keys)
        extra = set(nkw_keys) - set(npz_keys)
        raise ValueError(f"key mismatch: missing={missing}, extra={extra}")
    total_numel = 0
    for key in npz_keys:
        a = npz[key]
        b = tensors[key]
        if a.shape != b.shape:
            raise ValueError(f"{key}: shape {b.shape} != npz {a.shape}")
        total_numel += a.size
        # exact byte-for-byte value check (both are float32 LE)
        if not np.array_equal(a.view(np.uint32), b.view(np.uint32)):
            raise ValueError(f"{key}: values differ")
        if np.isnan(b).any() or np.isinf(b).any():
            raise ValueError(f"{key}: NaN/Inf in output")
    # spot-print a few samples for eyeballing
    for key in (npz_keys[0], npz_keys[len(npz_keys) // 2], npz_keys[-1]):
        flat = tensors[key].ravel()
        print(f"  sample {key} shape={tensors[key].shape} first3={flat[:3].tolist()}")
    return total_numel


def main():
    npz = np.load(NPZ_PATH)
    print(f"loaded npz: {len(npz.files)} arrays")
    keys = write_nkw(npz, OUT_PATH)
    import os
    size = os.path.getsize(OUT_PATH)
    print(f"wrote {OUT_PATH}: {len(keys)} tensors, {size} bytes ({size / 1024 / 1024:.2f} MB)")
    tensors = read_nkw(OUT_PATH)
    total = verify(npz, tensors)
    print(f"verify OK: {len(tensors)} tensors, {total} float32 values, all bit-exact vs npz")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print(f"FAILED: {e}", file=sys.stderr)
        sys.exit(1)
