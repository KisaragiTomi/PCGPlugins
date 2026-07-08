"""Run the ORIGINAL python NKSR (CPU) stage by stage and dump golden reference data.

Requires the reference environment (torch 2.7.0+cu128 + nksr package deps) — CPU device is used
so float32 math matches the C++ port (no TF32).

Dump names mirror the C++ commandlet -DumpDir output:
  global_scale.npy, svh_enc_d{d}.npy, encoder_feat.npy,
  svh_dec_d{d}.npy, svh_tmp_d{d}.npy,
  feat_struct_d{d}.npy, feat_basis_d{d}.npy, feat_normal_d{d}.npy, feat_udf_d{d}.npy,
  solution_d{d}.npy, mesh_v.npy, mesh_f.npy
Coords dumps are int32 (N,3) in the grid's own iteration order; comparison aligns by ijk.

Usage: python gen_golden.py --input <golden_input.npy> --out <dir> [--detail-level 1.0]
"""
import argparse
import math
import os
import sys

import numpy as np

NKSR_PKG = r"D:\MyProject\AITest\ConvertToSurface\ConvertToSurface\NKSR\package"
if NKSR_PKG not in sys.path:
    sys.path.insert(0, NKSR_PKG)

import torch  # noqa: E402
import nksr  # noqa: E402
from nksr.svh import SparseFeatureHierarchy  # noqa: E402
from nksr.fields import KernelField, NeuralField  # noqa: E402


def save(out_dir, name, arr):
    np.save(os.path.join(out_dir, name), arr)


def dump_svh(out_dir, prefix, svh):
    for d in range(svh.depth):
        grid = svh.grids[d]
        if grid is None:
            coords = np.zeros((0, 3), np.int32)
        else:
            coords = grid.active_grid_coords().cpu().numpy().astype(np.int32)
        save(out_dir, f"{prefix}_d{d}.npy", coords)


def dump_feats(out_dir, prefix, feats, depth):
    for d in range(depth):
        if d in feats:
            arr = feats[d].detach().cpu().numpy().astype(np.float32)
        else:
            arr = np.zeros((0, 0), np.float32)
        save(out_dir, f"{prefix}_d{d}.npy", arr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--detail-level", type=float, default=1.0)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    device = torch.device("cpu")
    torch.set_num_threads(max(1, os.cpu_count() - 2))

    data = np.load(args.input).astype(np.float32)
    assert data.shape[1] >= 6, "expected (N,6) xyz+normal"
    xyz = torch.from_numpy(data[:, :3]).to(device)
    normal = torch.from_numpy(data[:, 3:6]).to(device)

    reconstructor = nksr.Reconstructor(device)
    hp = reconstructor.hparams
    net = reconstructor.network

    # --- detail_level -> global_scale (mirror of Reconstructor.reconstruct) ---
    global_scale = 1.0
    if args.detail_level is not None and hp.density_range is not None:
        vox_ijk = torch.unique(torch.div(xyz, hp.voxel_size, rounding_mode="floor").long(), dim=0)
        cur_density = xyz.size(0) / vox_ijk.size(0)
        min_density, max_density = hp.density_range
        target_density = min_density + (max_density - min_density) * (1.0 - args.detail_level)
        target_density = max(target_density, 0.01)
        global_scale = math.sqrt(target_density / cur_density)
    save(args.out, "global_scale.npy", np.array([global_scale], np.float32))
    if global_scale != 1.0:
        xyz = xyz / global_scale

    # --- SVH ---
    svh = SparseFeatureHierarchy(voxel_size=hp.voxel_size, depth=hp.tree_depth, device=device)
    svh.build_point_splatting(xyz)
    dump_svh(args.out, "svh_enc", svh)

    # --- encoder ---
    feat = net.encoder(xyz, normal, svh, 0)
    save(args.out, "encoder_feat.npy", feat.detach().cpu().numpy().astype(np.float32))

    # --- unet ---
    feat_set, dec_svh, tmp_svh = net.unet(feat, svh, adaptive_depth=hp.adaptive_depth)
    dump_svh(args.out, "svh_dec", dec_svh)
    dump_svh(args.out, "svh_tmp", tmp_svh)
    dump_feats(args.out, "feat_struct", feat_set.structure_features, hp.tree_depth)
    dump_feats(args.out, "feat_basis", feat_set.basis_features, hp.tree_depth)
    dump_feats(args.out, "feat_normal", feat_set.normal_features, hp.tree_depth)
    dump_feats(args.out, "feat_udf", feat_set.udf_features, hp.tree_depth)

    # --- kernel field solve (fused, defaults) ---
    field = KernelField(
        svh=dec_svh,
        interpolator=net.interpolators,
        features=feat_set.basis_features,
        approx_kernel_grad=False,
        solver_max_iter=2000,
        solver_tol=1.0e-5,
    )
    normal_xyz = torch.cat([dec_svh.get_voxel_centers(d) for d in range(hp.adaptive_depth)])
    normal_value = torch.cat([feat_set.normal_features[d] for d in range(hp.adaptive_depth)])
    normal_weight = hp.solver.normal_weight / normal_xyz.size(0) * (hp.voxel_size ** 2)
    field.solve(
        pos_xyz=xyz, normal_xyz=normal_xyz, normal_value=-normal_value,
        pos_weight=hp.solver.pos_weight / xyz.size(0),
        normal_weight=normal_weight,
        reg_weight=1.0,
        nystrom_min_depth=100,
    )
    for d in range(hp.tree_depth):
        sol = field.solutions.get(d)
        arr = sol.detach().cpu().numpy().astype(np.float32) if sol is not None else np.zeros((0,), np.float32)
        save(args.out, f"solution_d{d}.npy", arr)

    # --- mask + mesh ---
    mask_field = NeuralField(svh=tmp_svh, decoder=net.udf_decoder, features=feat_set.udf_features)
    mask_field.set_level_set(2 * hp.voxel_size)
    field.set_mask_field(mask_field)
    field.set_scale(global_scale)

    mesh = field.extract_dual_mesh(mise_iter=1)
    save(args.out, "mesh_v.npy", mesh.v.detach().cpu().numpy().astype(np.float32))
    save(args.out, "mesh_f.npy", mesh.f.detach().cpu().numpy().astype(np.int64))
    print(f"golden dump complete: {args.out}")
    print(f"  mesh: {mesh.v.shape[0]} verts, {mesh.f.shape[0]} faces, scale={global_scale:.6f}")


if __name__ == "__main__":
    main()
