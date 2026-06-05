"""
NKSR point cloud surface reconstruction (inference only).
Called by UE Blueprint via FPlatformProcess.

Usage:
  python reconstruct.py --input <file> --output <file.obj>
                        [--detail-level 1.0] [--device cuda]
                        [--nksr-package <path>]

Exit code 0 = success, non-zero = failure.
Outputs JSON status to stdout on the last line:
  {"status":"ok","vertices":N,"faces":N,"output":"path"}
  {"status":"error","message":"..."}
"""
import argparse
import json
import os
import sys
import numpy as np


def load_point_cloud(path: str):
    ext = os.path.splitext(path)[1].lower()

    if ext == '.ply':
        try:
            import open3d as o3d
            pcd = o3d.io.read_point_cloud(path)
            pts = np.asarray(pcd.points, dtype=np.float32)
            nrm = np.asarray(pcd.normals, dtype=np.float32) if pcd.has_normals() else None
            return pts, nrm
        except ImportError:
            from plyfile import PlyData
            ply = PlyData.read(path)
            v = ply['vertex']
            pts = np.stack([v['x'], v['y'], v['z']], axis=-1).astype(np.float32)
            nrm = np.stack([v['nx'], v['ny'], v['nz']], axis=-1).astype(np.float32) if 'nx' in v else None
            return pts, nrm

    elif ext in ('.obj', '.stl', '.off'):
        import trimesh
        mesh = trimesh.load(path, process=False)
        if isinstance(mesh, trimesh.PointCloud):
            return np.asarray(mesh.vertices, dtype=np.float32), None
        pts = np.asarray(mesh.vertices, dtype=np.float32)
        nrm = np.asarray(mesh.vertex_normals, dtype=np.float32) if hasattr(mesh, 'vertex_normals') else None
        return pts, nrm

    elif ext in ('.fbx', '.dae', '.gltf', '.glb'):
        import assimp_py
        scene = assimp_py.import_file(path,
            assimp_py.Process_Triangulate | assimp_py.Process_GenSmoothNormals)
        all_pts, all_nrm = [], []
        for m in scene.meshes:
            all_pts.append(np.array(m.vertices, dtype=np.float32).reshape(-1, 3))
            if len(m.normals) > 0:
                all_nrm.append(np.array(m.normals, dtype=np.float32).reshape(-1, 3))
        pts = np.concatenate(all_pts, axis=0)
        nrm = np.concatenate(all_nrm, axis=0) if all_nrm else None
        return pts, nrm

    elif ext in ('.xyz', '.txt', '.csv'):
        data = np.loadtxt(path, dtype=np.float32)
        return data[:, :3], data[:, 3:6] if data.shape[1] >= 6 else None

    elif ext == '.npy':
        data = np.load(path).astype(np.float32)
        return data[:, :3], data[:, 3:6] if data.shape[1] >= 6 else None

    raise ValueError(f"Unsupported format: {ext}")


def estimate_normals(points: np.ndarray, k: int = 30) -> np.ndarray:
    try:
        import open3d as o3d
        pcd = o3d.geometry.PointCloud()
        pcd.points = o3d.utility.Vector3dVector(points)
        pcd.estimate_normals(o3d.geometry.KDTreeSearchParamKNN(knn=k))
        pcd.orient_normals_consistent_tangent_plane(k=k)
        return np.asarray(pcd.normals, dtype=np.float32)
    except ImportError:
        from scipy.spatial import cKDTree
        tree = cKDTree(points)
        _, idx = tree.query(points, k=k)
        normals = np.zeros_like(points)
        for i in range(len(points)):
            neighbors = points[idx[i]]
            centered = neighbors - neighbors.mean(axis=0)
            cov = centered.T @ centered
            _, _, vh = np.linalg.svd(cov)
            normals[i] = vh[-1]
        return normals


def result_json(status, **kwargs):
    d = {"status": status}
    d.update(kwargs)
    print(json.dumps(d))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--input', '-i', required=True)
    parser.add_argument('--output', '-o', default=None)
    parser.add_argument('--detail-level', type=float, default=1.0)
    parser.add_argument('--device', default='cuda')
    parser.add_argument('--nksr-package', default=None,
                        help='Path to NKSR package dir (added to PYTHONPATH)')
    args = parser.parse_args()

    if args.nksr_package:
        sys.path.insert(0, args.nksr_package)

    if args.output is None:
        base = os.path.splitext(args.input)[0]
        args.output = f"{base}_nksr.obj"

    try:
        import torch
        device = torch.device(args.device if torch.cuda.is_available() else 'cpu')

        points, normals = load_point_cloud(args.input)
        if normals is None:
            normals = estimate_normals(points)

        import nksr
        reconstructor = nksr.Reconstructor(device)
        input_xyz = torch.from_numpy(points).float().to(device)
        input_normal = torch.from_numpy(normals).float().to(device)

        field = reconstructor.reconstruct(input_xyz, input_normal,
                                          detail_level=args.detail_level)
        mesh = field.extract_dual_mesh(mise_iter=1)

        verts = mesh.v.cpu().numpy()
        faces = mesh.f.cpu().numpy()

        os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)

        import trimesh
        trimesh.Trimesh(vertices=verts, faces=faces, process=False).export(args.output)

        result_json("ok", vertices=int(len(verts)), faces=int(len(faces)),
                    output=os.path.abspath(args.output))

    except Exception as e:
        result_json("error", message=str(e))
        sys.exit(1)


if __name__ == '__main__':
    main()
