# -*- coding: utf-8 -*-
"""把 `TinyGladeShotRockNoise.py` 两次运行的落盘**逐槽**比对 —— 纯 CPython，不进编辑器。

为什么比对能干净到不需要分带：`bAlive` 只看坡度、**与 `Noise` 无关**，图案与种子两次也都没变
⇒ 两次运行的顶点槽**一一对应**。逐槽相减，得到的正是被改掉的那一项本身::

    ΔPos = Pos_B − Pos_A = −Noise · (1 − Rock) · N        （‖N‖ = 1）

于是 `|ΔPos|` **就是**"这个顶点上有多少噪声是绕过坡度 mask 加进去的"，
一次梯度估计、一次分带都不需要 —— 而单次运行里想靠梯度把外沿环框出来是**死路**：
位移让顶点 XY 跑了几十厘米，而过渡带在半径上只有 1 m 宽（实测把 ±6 cm 的信号涂抹成 ±39 cm）。

用法::

    python TinyGladeShotRockNoiseStats.py <A.json> <B.json>

输出只描述差别，**不下结论** —— 该选哪一版是看图的人的事。
"""
import json
import math
import sys

# Windows 控制台默认 GBK，输出里的 − / · 会直接抛 UnicodeEncodeError
# （报错位置在 print 上，看着像脚本本身坏了）。
try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass


# 单通道差超过它才算"这个采样点变了"，与 TinyGladeShotRockShell.py 的 PIXEL_DELTA 同源：
# 压掉 TAA/曝光抖动，留下真正的几何变化。
PIXEL_DELTA = 12


def load(path):
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def quantiles(vals, qs):
    if not vals:
        return [float("nan")] * len(qs)
    s = sorted(vals)
    return [s[min(len(s) - 1, max(0, int(round(q * (len(s) - 1)))))] for q in qs]


def main(path_a, path_b):
    a, b = load(path_a), load(path_b)
    va, vb = a["verts"], b["verts"]

    # 槽位对不上就整个作废：对不上意味着两次的活三角集合不同，而 bAlive 与 Noise 无关 ⇒
    # 一定是别的东西也被改了（地形、参数、种子），这时逐槽相减量到的是那个东西，不是噪声。
    if va["live_tris"] != vb["live_tris"]:
        sa, sb = set(va["live_tris"]), set(vb["live_tris"])
        print("[FAIL] 两次运行的活三角集合不同：A %d / B %d，只在 A %d、只在 B %d 个。"
              % (len(sa), len(sb), len(sa - sb), len(sb - sa)))
        print("       bAlive 只看坡度、与 Noise 无关 —— 集合变了说明这两次之间还改了别的东西，"
              "逐槽比对无效。")
        return 1

    pa, pb = va["live_pos"], vb["live_pos"]
    if len(pa) != len(pb):
        print("[FAIL] 顶点数不同：A %d / B %d" % (len(pa), len(pb)))
        return 1

    deltas = []
    for (ax, ay, az), (bx, by, bz) in zip(pa, pb):
        deltas.append(math.sqrt((bx - ax) ** 2 + (by - ay) ** 2 + (bz - az) ** 2))

    n = len(deltas)
    q = quantiles(deltas, [0.0, 0.25, 0.5, 0.75, 0.9, 0.99, 1.0])
    moved1 = sum(1 for d in deltas if d > 1.0)
    moved3 = sum(1 for d in deltas if d > 3.0)
    print("A = %s" % path_a)
    print("B = %s" % path_b)
    print("活三角 %d / 活顶点 %d（两次逐三角一致）" % (len(va["live_tris"]), n))
    print("逐顶点位移 |ΔPos| cm："
          "min %.3f / p25 %.3f / 中位 %.3f / p75 %.3f / p90 %.3f / p99 %.3f / max %.3f"
          % tuple(q))
    print("动了 > 1 cm 的顶点：%d（%.1f%%）；> 3 cm：%d（%.1f%%）"
          % (moved1, 100.0 * moved1 / n, moved3, 100.0 * moved3 / n))
    print("含义：|ΔPos| = Noise·(1−Rock)。它**大**的地方就是坡度 mask 还没起来、"
          "而噪声照旧满幅的地方；两版画面上的差别全部集中在这些顶点。")

    print()
    print("同机位逐采样点的画面差异（%d 个点/张，单通道差 > %d/255 才算变了）："
          % (a["grid"][0] * a["grid"][1], PIXEL_DELTA))
    for name in sorted(set(a["pixels"]) & set(b["pixels"])):
        pa2, pb2 = a["pixels"][name], b["pixels"][name]
        changed = sum(1 for u, v in zip(pa2, pb2)
                      if max(abs(u[0] - v[0]), abs(u[1] - v[1]), abs(u[2] - v[2])) > PIXEL_DELTA)
        biggest = max(max(abs(u[0] - v[0]), abs(u[1] - v[1]), abs(u[2] - v[2]))
                      for u, v in zip(pa2, pb2))
        print("  %-12s 变了 %4d/%4d（%5.1f%%），单点最大通道差 %.0f/255"
              % (name, changed, len(pa2), 100.0 * changed / len(pa2), biggest))
    print(u"  ⚠️ 这个数**不是**判据，只是「两张图差在哪、差多少」的一个刻度："
          u"同一台相机第二次拍时已带着上一轮的 Lumen 历史，捕获自身就有不对称"
          u"（裙边噪声那条门就是因此被降级成只出图的）。")

    print()
    print("按半径分箱的法向垂距（各自运行内量的，只作剖面存档）：")
    print("  %-14s %-28s %-28s" % ("r 区间 cm", "A: 中位 / 幅度 / 地面下%", "B: 中位 / 幅度 / 地面下%"))
    keys = sorted(set(va["profile"]) | set(vb["profile"]), key=lambda k: int(k))
    for k in keys:
        da, db = va["profile"].get(k, {"n": 0}), vb["profile"].get(k, {"n": 0})

        def fmt(d):
            if not d.get("n"):
                return "n=0"
            return "n=%4d %+7.2f / %6.2f / %5.1f%%" % (d["n"], d["median"], d["span"], d["below_pct"])
        print("  %-14s %-28s %-28s" % ("%s..%d" % (k, int(k) + 100), fmt(da), fmt(db)))
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
