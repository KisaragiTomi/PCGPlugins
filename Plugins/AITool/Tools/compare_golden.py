"""Compare python golden dumps against C++ commandlet dumps, aligning per-voxel data by ijk.

Voxel iteration order differs by design (nanovdb tree order vs C++ lexicographic), so every
per-voxel tensor is sorted by its own coords before comparison.

Usage: python compare_golden.py --golden <dir> --cpp <dir> [--rtol 1e-3] [--atol 1e-4]
Exit code 0 = pass, 1 = mismatch.
"""
import argparse
import os
import sys

import numpy as np

DEPTHS = range(4)


def load(d, name):
    p = os.path.join(d, name)
    return np.load(p) if os.path.exists(p) else None


def sort_key(coords):
    return np.lexsort((coords[:, 2], coords[:, 1], coords[:, 0]))


class Report:
    def __init__(self):
        self.failures = []

    def check(self, ok, msg):
        print(("  PASS  " if ok else "  FAIL  ") + msg)
        if not ok:
            self.failures.append(msg)


def cmp_coords(rep, name, a, b):
    if a is None or b is None:
        rep.check(a is None and b is None, f"{name}: presence (golden={a is not None}, cpp={b is not None})")
        return None, None
    if a.shape != b.shape:
        rep.check(False, f"{name}: shape {a.shape} vs {b.shape}")
        return None, None
    ia, ib = sort_key(a), sort_key(b)
    ok = np.array_equal(a[ia], b[ib])
    rep.check(ok, f"{name}: {a.shape[0]} voxels, set equality")
    return (ia, ib) if ok else (None, None)


def cmp_feat(rep, name, a, b, ia, ib, rtol, atol):
    if a is None or b is None:
        rep.check(a is None and b is None, f"{name}: presence")
        return
    if a.size == 0 and b.size == 0:
        rep.check(True, f"{name}: both empty")
        return
    if a.shape != b.shape:
        rep.check(False, f"{name}: shape {a.shape} vs {b.shape}")
        return
    aa = a[ia] if ia is not None else a
    bb = b[ib] if ib is not None else b
    diff = np.abs(aa - bb)
    denom = np.maximum(np.abs(aa), np.abs(bb))
    ok = np.all(diff <= atol + rtol * denom)
    rep.check(bool(ok), f"{name}: max_abs={diff.max():.3e} max_rel={(diff / np.maximum(denom, 1e-12)).max():.3e}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--golden", required=True)
    ap.add_argument("--cpp", required=True)
    ap.add_argument("--rtol", type=float, default=1e-3)
    ap.add_argument("--atol", type=float, default=1e-4)
    ap.add_argument("--sol-atol", type=float, default=5e-3)
    args = ap.parse_args()
    rep = Report()

    g_scale, c_scale = load(args.golden, "global_scale.npy"), load(args.cpp, "global_scale.npy")
    if g_scale is not None and c_scale is not None:
        rep.check(abs(float(g_scale[0]) - float(c_scale[0])) < 1e-6 * max(1.0, abs(float(g_scale[0]))),
                  f"global_scale: {float(g_scale[0]):.6f} vs {float(c_scale[0]):.6f}")

    print("== encoder svh ==")
    enc_perm = {}
    for d in DEPTHS:
        n = f"svh_enc_d{d}.npy"
        enc_perm[d] = cmp_coords(rep, n, load(args.golden, n), load(args.cpp, n))
    print("== encoder features ==")
    ia, ib = enc_perm.get(0, (None, None))
    cmp_feat(rep, "encoder_feat.npy", load(args.golden, "encoder_feat.npy"), load(args.cpp, "encoder_feat.npy"),
             ia, ib, args.rtol, args.atol)

    print("== decoder / tmp svh ==")
    dec_perm, tmp_perm = {}, {}
    for d in DEPTHS:
        n = f"svh_dec_d{d}.npy"
        dec_perm[d] = cmp_coords(rep, n, load(args.golden, n), load(args.cpp, n))
        n = f"svh_tmp_d{d}.npy"
        tmp_perm[d] = cmp_coords(rep, n, load(args.golden, n), load(args.cpp, n))

    print("== unet features ==")
    for d in DEPTHS:
        ta, tb = tmp_perm.get(d, (None, None))
        da, db = dec_perm.get(d, (None, None))
        for prefix, (pa, pb) in (("feat_struct", (ta, tb)), ("feat_udf", (ta, tb)),
                                 ("feat_basis", (da, db)), ("feat_normal", (da, db))):
            n = f"{prefix}_d{d}.npy"
            ga, ca = load(args.golden, n), load(args.cpp, n)
            if ga is not None and ga.size and (pa is None):
                if ga is not None and ca is not None and ga.size and ca.size:
                    rep.check(False, f"{n}: coords mismatched upstream, skipping value check")
                continue
            cmp_feat(rep, n, ga, ca, pa, pb, args.rtol, args.atol)

    print("== solutions ==")
    for d in DEPTHS:
        da, db = dec_perm.get(d, (None, None))
        n = f"solution_d{d}.npy"
        ga, ca = load(args.golden, n), load(args.cpp, n)
        if ga is not None and ca is not None and ga.ndim == 1:
            ga, ca = ga[:, None], ca[:, None]
        cmp_feat(rep, n, ga, ca, da, db, args.rtol, args.sol_atol)

    print("== mesh ==")
    gv, cv = load(args.golden, "mesh_v.npy"), load(args.cpp, "mesh_v.npy")
    gf, cf = load(args.golden, "mesh_f.npy"), load(args.cpp, "mesh_f.npy")
    if gv is not None and cv is not None:
        rep.check(abs(gv.shape[0] - cv.shape[0]) <= max(4, int(0.01 * gv.shape[0])),
                  f"mesh verts: {gv.shape[0]} vs {cv.shape[0]}")
        rep.check(gf is not None and cf is not None and abs(gf.shape[0] - cf.shape[0]) <= max(4, int(0.01 * gf.shape[0])),
                  f"mesh faces: {None if gf is None else gf.shape[0]} vs {None if cf is None else cf.shape[0]}")
        try:
            from scipy.spatial import cKDTree
            d1 = cKDTree(gv).query(cv)[0]
            d2 = cKDTree(cv).query(gv)[0]
            chamfer = 0.5 * (d1.mean() + d2.mean())
            rep.check(chamfer < 0.05, f"mesh chamfer: {chamfer:.5f} (hausdorff≈{max(d1.max(), d2.max()):.5f})")
        except ImportError:
            print("  WARN  scipy unavailable, skipped chamfer")

    print()
    if rep.failures:
        print(f"RESULT: FAIL ({len(rep.failures)} mismatches)")
        sys.exit(1)
    print("RESULT: PASS")
    sys.exit(0)


if __name__ == "__main__":
    main()
