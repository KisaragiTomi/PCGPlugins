# -*- coding: utf-8 -*-
"""`TinyGladeShotSeam.py` 出的图的**可量化判据**。编辑器外跑（PIL + numpy）。

回答的只有一个问题：**改前/改后那点差异，真的是接缝吗？**

判据是逐像素差异率 `diff%` —— 某个通道的绝对差超过 `DELTA`（8/255）的像素占比。
8 这个数沿用拉尺寸那一轮的口径（`resize_drag.png` vs `resize_full.png` 用的就是它）。

四组图、三个数，**缺一不可**：

  · signal     `off` vs `on`（邻居**已从捕获里隐藏**）。画面里唯一能变的只有 House_Road
               自己身上的接缝：插进邻居里的那截墙被 clip 掉 + 轮廓交点上立起来的砖柱。
  · sabotage   `off` vs `apart` —— **故意破坏世界侧**。接缝照旧开着、代码一行没变、邻居照旧
               隐藏，只把邻居挪出 footprint ⇒ 两栋房不再相交 ⇒ 这一对必须**塌回噪声底**。
               ⚠️ **这一条才是"门是活的"的证明。** 上一轮 D8 的门看着有差异（35.2% → 33.0%），
               实际在测藤蔓重排 —— "两张图不一样"永远不能证明"不一样的地方是我说的那个东西"。
  · floor      `off` vs `off2` —— **噪声底**：世界、开关、相机一律不动，同一组再拍一遍。
               ⚠️ 没有它，"破坏组塌到 1.4%"这句话没有刻度。实测 `wide` 机位的底就有 2.085%
               （大片天空 + 草地，32 帧预热后 Lumen 仍在收敛），而 `corner` 只有 0.993% ——
               同一个绝对数在两个机位是完全不同的意思，**必须逐机位现量**。

门限：
  · signal ≥ `SIGNAL_MEASURED × 0.5`。照项目口径**先量真值再取一半**。
  · sabotage ≤ `floor × 1.25`。**这条不写死数**，用这一次跑出来的噪声底当刻度 ——
    机位、光照、预热帧数任何一样变了，底就跟着变，写死的数只会给出一个假的绿灯。
  · floor < `SIGNAL_MEASURED × 0.5`。底把信号淹了的话前两条都不成立，这一条是它们的前置。

`pairoff` vs `pairon`（邻居可见）是交付要的那张改前/改后，**只作观感参考不作判据**：
它里面混着邻居自己那一半接缝，隔离性不如上面三条。

用法::

    python TinyGladeShotSeamStats.py
"""
import os
import sys

import numpy as np
from PIL import Image

SHOTS = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__))))), "Saved", "TinyGladeShots")

VIEWS = ["corner", "wide"]
DELTA = 8          # 8/255：滤掉光照收敛噪声，远小于"墙被裁掉一块"的量级

# ---- 实测真值（2026-08-31 本轮出图）。**改了机位 / 关卡 / 预热帧数就要重量一次。** ----
# corner: signal 17.843% · sabotage 0.079% · floor 0.953%   （破坏组比噪声底还低一个数量级）
# wide  : signal  5.571% · sabotage 1.220% · floor 2.192%   （底大是因为满屏天空+草地仍在收敛）
#
# ⚠️ 这组数是**修掉"砖数已经是 0 了、画面上砖还立着"那条之后**量的。修之前 corner 的 signal
# 是 15.904% —— 差的那 1.9 个百分点正是两根本不该出现在 `off` 里的砖柱。**旧的那组数看着一样
# 体面，却是拿一张错的基准图量出来的**：这就是为什么出图那一步不能只看数、必须真的把图打开看。
SIGNAL_MEASURED = {"corner": 17.843, "wide": 5.571}
# 破坏组允许比这一次的噪声底高出的倍数。1.25 = 给底本身的抖动留一档，不是给信号留的。
SABOTAGE_HEADROOM = 1.25


def load(phase, view):
    path = os.path.join(SHOTS, "seam_%s_%s.png" % (phase, view))
    if not os.path.exists(path):
        return None
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


def diff_rate(a, b):
    """某个通道绝对差 > DELTA 的像素占比（%）。"""
    if a is None or b is None:
        return None
    return 100.0 * float(np.mean(np.max(np.abs(a - b), axis=2) > DELTA))


def main():
    missing = []
    rows = []
    for view in VIEWS:
        imgs = {p: load(p, view) for p in ("off", "on", "apart", "off2", "pairoff", "pairon")}
        for phase, img in imgs.items():
            if img is None:
                missing.append("seam_%s_%s.png" % (phase, view))
        rows.append((view,
                     diff_rate(imgs["off"], imgs["on"]),
                     diff_rate(imgs["off"], imgs["apart"]),
                     diff_rate(imgs["off"], imgs["off2"]),
                     diff_rate(imgs["pairoff"], imgs["pairon"])))

    if missing:
        print("SEAMSTATS MISSING: %s" % ", ".join(missing))
    fmt = lambda v: "n/a" if v is None else "%.3f" % v
    print("%-8s %10s %10s %10s %10s" % ("view", "signal", "sabotage", "floor", "visible"))
    for view, signal, sabotage, floor, visible in rows:
        print("%-8s %9s%% %9s%% %9s%% %9s%%"
              % (view, fmt(signal), fmt(sabotage), fmt(floor), fmt(visible)))

    ok = True
    for view, signal, sabotage, floor, _visible in rows:
        if signal is None or sabotage is None or floor is None:
            print("[FAIL] %s: images missing" % view)
            ok = False
            continue
        gate = SIGNAL_MEASURED.get(view, 0.0) * 0.5
        ceiling = floor * SABOTAGE_HEADROOM

        # 前置：底不能把信号淹了，否则下面两条都是空判据。
        if floor >= gate:
            print("[FAIL] %s: the noise floor (%.3f%%) swallows the gate (%.3f%%) -- nothing below is meaningful"
                  % (view, floor, gate))
            ok = False
            continue
        print("[PASS] %s: the noise floor is %.3f%%, well under the %.3f%% gate" % (view, floor, gate))

        if signal < gate:
            print("[FAIL] %s: the seam barely changed the picture (%.3f%% < %.3f%%)" % (view, signal, gate))
            ok = False
        else:
            print("[PASS] %s: turning the seam on changes %.3f%% of the frame (gate %.3f%%)" % (view, signal, gate))

        # 破坏组：**必须**塌回噪声底。没塌 = 这道门量的不是接缝，整组图作废。
        if sabotage > ceiling:
            print("[FAIL] %s: moving the neighbour clear did NOT collapse the diff (%.3f%% > %.3f%%)"
                  " -- the gate is measuring something else" % (view, sabotage, ceiling))
            ok = False
        else:
            print("[PASS] %s: the sabotage collapses to %.3f%%, at or under the %.3f%% noise floor"
                  % (view, sabotage, floor))
    print("SEAMSTATS OK" if ok else "SEAMSTATS FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
