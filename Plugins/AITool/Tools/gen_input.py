"""Generate the deterministic golden-test input point cloud (shared by python & C++ pipelines).

Fibonacci sphere, radius 50, analytic normals, small fixed-seed radial noise.
Output: (N, 6) float32 npy  [x y z nx ny nz]

Usage: python gen_input.py [--out <path>] [--n 8192] [--radius 50] [--noise 0.15]
"""
import argparse
import os

import numpy as np


def fibonacci_sphere(n: int, radius: float) -> np.ndarray:
    i = np.arange(n, dtype=np.float64)
    phi = np.pi * (3.0 - np.sqrt(5.0))
    y = 1.0 - 2.0 * (i + 0.5) / n
    r = np.sqrt(1.0 - y * y)
    theta = phi * i
    pts = np.stack([np.cos(theta) * r, y, np.sin(theta) * r], axis=1)
    return pts * radius


def main():
    ap = argparse.ArgumentParser()
    default_out = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "..", "Intermediate", "PortWork", "golden_input.npy")
    ap.add_argument("--out", default=default_out)
    ap.add_argument("--n", type=int, default=8192)
    ap.add_argument("--radius", type=float, default=50.0)
    ap.add_argument("--noise", type=float, default=0.15)
    args = ap.parse_args()

    pts = fibonacci_sphere(args.n, args.radius)
    normals = pts / np.linalg.norm(pts, axis=1, keepdims=True)

    rng = np.random.RandomState(42)
    pts = pts + normals * rng.randn(args.n, 1) * args.noise

    data = np.concatenate([pts, normals], axis=1).astype(np.float32)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    np.save(args.out, data)
    print(f"wrote {args.out}  shape={data.shape} dtype={data.dtype}")
    print(f"bbox min={pts.min(0)} max={pts.max(0)}")


if __name__ == "__main__":
    main()
