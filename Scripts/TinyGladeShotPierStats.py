# -*- coding: utf-8 -*-
"""`TinyGladeShotPier.py` 出的图的**可量化判据**。编辑器外跑（PIL + numpy）。

问的是一句话：**开了墩样式之后，连拱之间那截灰泥真的没了吗？**

判据 = 同机位下 `before`（墩样式关）与 `after`（墩样式开）之间**通道差 > 8/255 的像素占比**。
差异集中在两处墩上，所以它就是"那截灰泥在不在"的直接度量。

**阈值先量真值再取一半**（实测 2026-08-30，`L_HouseGroundDemo`，`PierWidth = FrameBrickDepth = 20`）：

    wall  before-vs-after 0.869%   ⇒ 门限 0.43%
    pier  before-vs-after 3.676%   ⇒ 门限 1.83%

**门必须是活的**，所以还要第三组 `sabotage`：墩样式**开着**，但 `PierStyleMaxWidth` 按到 0
（跨度再窄也判不成墩）—— 世界侧被故意破坏成"这个特性不发生"。它与 `before` 的差异实测是

    wall  0.032%     pier  0.010%

也就是渲染噪声底（预热 32 帧之后 Lumen 仍有一点抖动），比门限低 13～180 倍。
⇒ 如果哪天墩不生效了，这两组数会掉到同一个量级，门当场报红。没有这一步的话，
"before 和 after 不一样"这条判据完全可能只是在量噪声。

-------------------------------------------------------------------------------
第二组门（2026-08-30 加）：**门框砖解析推导之后，墩上那条竖缝没了**

⚠️ **这一组是一次性的历史判据，输入图已经不能再生**：拍 `legacyframe` / `legacyoff` 要把
   `csh.FrameLegacy` 打开走门框旧路，而旧路与那个 CVar 已随裁决一第二步删除。四张图若还留在
   `Saved/TinyGladeShots/`（`Saved/` 不进 git，clean checkout 上没有）就照常判，否则打一行
   "缺图，跳过"。**它不再是活门** —— "新旧两条路把砖摆在同一个地方"现在由单测
   `House.FrameAnalyticMatchesLegacy` 守着，那条带自己的 CPU 镜像，不依赖任何产线旧路。
-------------------------------------------------------------------------------

旧路让相邻两拱各出一条门樘砖脚、各伸进跨度一半 ⇒ 两列砖在墩正中共面对接，从地面一直贯着
一条竖缝（`pier_legacyframe_pier.png` 里看得一清二楚）。解析推导把墩升格成一条自己的砖路，
墩上只剩**一列**。

判据同样是同机位像素差，**阈值 = 实测值的一半**（实测 2026-08-30，`L_HouseGroundDemo`，
`PierWidth = FrameBrickDepth = 20`，砖数 120 → 96）::

    pier  legacyframe-vs-after 16.709%   ⇒ 门限 8.35%

**门必须是活的**，对照组是 `legacyoff` vs `before`：**墩样式关掉**之后两条路退回同一个拓扑
（每个拱各出两条门樘），实测差异塌到 **4.204%**，正好是门限的一半、live 的 1/4。

⚠️ 这个对照组**不是噪声底**，别照第一组的口径去要求它接近 0：解析路把每一块砖都从"外接
多边形 + B 样条"挪回到 clip 场自己的边上，位移 0.2~0.4 cm，在这个 40° 贴脸机位上就是
1~2 px，整面墙的砖边都会记进像素差。所以这一组的判据写成**两条**：
live 过门限，且 live ≥ 3×对照（实测 3.97×）。三条数一起才说得清"变化来自并列，不是来自
到处都画得不一样"。

用法::

    python TinyGladeShotPierStats.py                    # 默认 before / after / sabotage + 门框那一组
    python TinyGladeShotPierStats.py <before> <after> [<sabotage>]
"""
import os
import sys

import numpy as np
from PIL import Image

SHOTS = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__))))), "Saved", "TinyGladeShots")

# 通道差超过这个值才算"这个像素变了"。8/255 是量化 + 轻微时间抖动的常规容差。
CHANNEL_TOL = 8

# 机位 → (实测 before-vs-after 差异占比, 门限)。门限一律 = 实测值的一半。
#
# ⚠️ 2026-08-30 门框砖改解析推导之后**重量了一次**：墩样式打开现在还会把两列门樘砖并成一列
# （旧路只是把灰泥裁掉、砖照旧两列），画面差异因此从 0.869 / 3.676 涨到 3.301 / 16.435。
# 沿用旧门限只是"更松"，不是更安全 —— 判据要跟着被测对象走，所以按新实测值重新取一半。
GATES = {
    "wall": (3.301, 1.65),
    "pier": (16.435, 8.22),
}
# sabotage 与 before 的差异上限：取门限的四分之一 —— 实测噪声底比这还低一个量级。
NOISE_CEIL = {view: gate * 0.25 for view, (_, gate) in GATES.items()}

# 门框砖解析推导那一组（见文件头第二节）。只看贴脸机位 —— 整面墙那个机位上"到处都挪了
# 1~2 px"的成分占比太大，比值分不开（实测 3.073% vs 1.440%，只有 2.1×）。
FRAME_MEASURED = 16.709   # pier 机位：legacyframe vs after
FRAME_GATE = 8.35         # = 实测值的一半
FRAME_CONTROL = 4.204     # pier 机位：legacyoff vs before（墩样式关 ⇒ 两条路同拓扑）
FRAME_MIN_RATIO = 3.0     # live / control 实测 3.97


def changed_fraction(path_a, path_b):
    a = np.asarray(Image.open(path_a).convert("RGB")).astype(np.int16)
    b = np.asarray(Image.open(path_b).convert("RGB")).astype(np.int16)
    if a.shape != b.shape:
        raise ValueError("两张图尺寸不同：%s vs %s" % (a.shape, b.shape))
    return float((np.abs(a - b).max(axis=2) > CHANNEL_TOL).mean()) * 100.0


def shot(tag, view):
    return os.path.join(SHOTS, "pier_%s_%s.png" % (tag, view))


def main():
    args = sys.argv[1:]
    before = args[0] if len(args) > 0 else "before"
    after = args[1] if len(args) > 1 else "after"
    sabotage = args[2] if len(args) > 2 else "sabotage"

    failed = []
    print("%-6s %-22s %-22s %s" % ("view", "before-vs-after", "before-vs-sabotage", "verdict"))
    for view, (measured, gate) in GATES.items():
        for tag in (before, after, sabotage):
            if not os.path.exists(shot(tag, view)):
                print("%-6s 缺图：%s" % (view, shot(tag, view)))
                failed.append(view)
                break
        else:
            live = changed_fraction(shot(before, view), shot(after, view))
            noise = changed_fraction(shot(before, view), shot(sabotage, view))
            ok_live = live >= gate
            ok_noise = noise <= NOISE_CEIL[view]
            if not ok_live:
                failed.append("%s: 墩样式没让画面变（%.3f%% < 门限 %.3f%%）" % (view, live, gate))
            if not ok_noise:
                failed.append("%s: 故意不发生的那一组也变了（%.3f%% > 噪声上限 %.3f%%）—— 判据在量别的东西"
                              % (view, noise, NOISE_CEIL[view]))
            print("%-6s %7.3f%% (门限 %5.3f%%)  %7.3f%% (上限 %5.3f%%)  %s   [实测基准 %.3f%%]"
                  % (view, live, gate, noise, NOISE_CEIL[view],
                     "PASS" if (ok_live and ok_noise) else "FAIL", measured))

    # ---- 第二组：门框砖解析推导之后墩上那条竖缝没了 ----
    frame_shots = [shot(t, "pier") for t in ("legacyframe", "after", "legacyoff", "before")]
    if all(os.path.exists(p) for p in frame_shots):
        live = changed_fraction(shot("legacyframe", "pier"), shot("after", "pier"))
        control = changed_fraction(shot("legacyoff", "pier"), shot("before", "pier"))
        ratio = live / control if control > 1.0e-6 else float("inf")
        ok = live >= FRAME_GATE and control <= FRAME_GATE and ratio >= FRAME_MIN_RATIO
        print("frame  %7.3f%% (门限 %5.3f%%)  control %7.3f%%  ratio %5.2f (>= %.1f)  %s   [实测基准 %.3f%% / %.3f%%]"
              % (live, FRAME_GATE, control, ratio, FRAME_MIN_RATIO,
                 "PASS" if ok else "FAIL", FRAME_MEASURED, FRAME_CONTROL))
        if live < FRAME_GATE:
            failed.append("frame: 并列没让画面变（%.3f%% < 门限 %.3f%%）" % (live, FRAME_GATE))
        if control > FRAME_GATE:
            failed.append("frame: 墩样式关掉的那一组也过了门限（%.3f%%）—— 判据在量别的东西" % control)
        if ratio < FRAME_MIN_RATIO:
            failed.append("frame: 并列的贡献没压过「到处都挪了 1~2 px」那一份（%.2f× < %.1f×）"
                          % (ratio, FRAME_MIN_RATIO))
    else:
        print("frame  缺图（历史判据，legacyframe / legacyoff 两个 tag 已随门框旧路删除，不能再生），跳过")

    if failed:
        for line in failed:
            print("PIERSTATS FAIL: %s" % line)
        print("PIERSTATS FAILED")
        return 1
    print("PIERSTATS OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
