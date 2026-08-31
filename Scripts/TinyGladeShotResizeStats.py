# -*- coding: utf-8 -*-
"""`TinyGladeShotResize.py` 出的图的可量化判据。编辑器外跑（PIL + numpy）。

为什么离线算：判据必须是**任何人拿着那些 png 就能复现**的数，不是"这次跑出来的一个值"。

三条判据：

  A **相邻帧差异率**（`diff%` = 通道差 > 8 的像素占比）。
    禁带**开**时，除跳带那一帧外，逐帧差异应当是连续拉伸该有的那一小段；
    禁带**关**时，屋脊翻面的那一帧会顶出一个明显更大的尖峰。
    ⚠️ 这一条同时就是"门是活的"的证明：翻轴在像素上**看得见**（off 那组顶了出来），
    所以 on 那组"没看见"才有意义。看不见的话这整套判据是空的。

  B **派生物可见性**：`on_no{frame,vine,decor}` 与同尺寸基准帧 `on_f8` 比。
    差异率必须明显不为零 —— 某一类关掉后画面纹丝不动，就说明这组图**根本没拍到它**，
    "它跟得住"是空话（D8 那轮第一版判据量的是藤蔓重排而不是洞，就是这么发现的）。

  D **拖出来的 vs 全量重建**（`on_f{2,5,8}` vs `on_f{2,5,8}_rb`）：同一帧多补一次
    `RebuildHouse` 后重拍。差异率应当只剩光照收敛噪声 —— 回归里那条 readback 断言只能证明
    **计数**相同（砖 121/121、洞 8/8……），证不了摆位；这一条才把"摆位也一样"钉死。

  C **零像素占比** `zero%`：精确 (0,0,0) 的比例。它是渲染路径的故障指纹
    （Lumen 单帧无历史 ⇒ 间接光恒零），不是观感量。预热对了就应该是 0.000%。

用法::

    python TinyGladeShotResizeStats.py [<shots-dir>]
"""
import os
import sys

import numpy as np
from PIL import Image

DEFAULT_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__))))), "Saved", "TinyGladeShots")
CHANNEL_TOL = 8          # 8/255：低于它的差异是光照收敛噪声，不是几何变化（沿用拉尺寸那轮的口径）
FRAMES = 9


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


def diff_pct(a, b):
    return 100.0 * float(np.mean(np.max(np.abs(a - b), axis=2) > CHANNEL_TOL))


def zero_pct(a):
    return 100.0 * float(np.mean(np.all(a == 0, axis=2)))


def main(shots_dir):
    def p(tag):
        return os.path.join(shots_dir, "resize_%s.png" % tag)

    missing = [t for t in (["on_f%d" % i for i in range(FRAMES)]
                           + ["off_f%d" % i for i in range(FRAMES)]
                           + ["on_noframe", "on_novine", "on_nodecor"]) if not os.path.exists(p(t))]
    if missing:
        print("missing: %s" % ", ".join(missing))
        return 1

    # C 先打：它是**故障指纹**，不是观感量。任何一帧不为 0 都说明这组图的渲染路径有问题，
    # 后面两条判据在那种情况下读出来的数没有意义（坑 ⑨ / 坑 ⑪，见出图脚本文件头）。
    print("== C  zero% (精确 (0,0,0)；预热与空转都对了应当接近 0.000) ==")
    worst = 0.0
    for tag in (["on_f%d" % i for i in range(FRAMES)] + ["off_f%d" % i for i in range(FRAMES)]
                + ["on_noframe", "on_novine", "on_nodecor"]):
        z = zero_pct(load(p(tag)))
        worst = max(worst, z)
        if z > 0.20:
            print("  %-11s zero=%.3f%%   <<< 这一帧的渲染路径不对，别读它的差异率" % (tag, z))
    print("  worst zero=%.3f%%%s" % (worst, "   OK" if worst <= 0.20 else ""))

    print("== A  相邻帧差异率（同机位，通道差 > %d）==" % CHANNEL_TOL)
    peaks = {}
    for group in ("on", "off"):
        frames = [load(p("%s_f%d" % (group, i))) for i in range(FRAMES)]
        seq = [diff_pct(frames[i], frames[i + 1]) for i in range(FRAMES - 1)]
        peaks[group] = (max(seq), seq.index(max(seq)))
        print("  band %-3s %s" % (group, "  ".join("f%d→f%d=%5.2f%%" % (i, i + 1, d)
                                                   for i, d in enumerate(seq))))
        print("           peak=%.2f%% at f%d→f%d" % (peaks[group][0], peaks[group][1], peaks[group][1] + 1))

    print("== D  拖出来的 vs 全量重建（同一帧再补一次 RebuildHouse）==")
    for i in (2, 5, 8):
        a, b = p("on_f%d" % i), p("on_f%d_rb" % i)
        if os.path.exists(b):
            print("  on_f%d vs on_f%d_rb  diff=%.2f%%" % (i, i, diff_pct(load(a), load(b))))

    print("== E  拖动中 vs 松手（bFinished=True，不清滞回表）==")
    for i in (2, 5, 8):
        a, b = p("on_f%d" % i), p("on_f%d_rel" % i)
        if os.path.exists(b):
            print("  on_f%d vs on_f%d_rel diff=%.2f%%  (zero: %.3f%% -> %.3f%%)"
                  % (i, i, diff_pct(load(a), load(b)), zero_pct(load(a)), zero_pct(load(b))))

    print("== B  派生物可见性（与同尺寸 on_f8 比）==")
    base = load(p("on_f8"))
    for tag, label in (("on_noframe", "门框砖"), ("on_novine", "藤蔓"), ("on_nodecor", "摆件")):
        d = diff_pct(base, load(p(tag)))
        print("  %-11s (%s) diff=%.2f%%%s" % (tag, label, d, "   ⚠️ 这组图没拍到它" if d < 0.20 else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_DIR))
