# -*- coding: utf-8 -*-
"""逐实例 custom data 验收：证明**数据写对了、而且和 packed 行逐实例配得上**。

地被这一家的语义：`[0]` = 弯曲幅度（TG `_945`/`_997`），**`[1]` 保留、恒 0**
（它曾经存"该株世界高度"，2026-09-07 拆掉 —— 材质能从逐实例 Local→World 自己推出来，
见 `CSGroundCover.usf` 里那条 ⚠️）。

四条判据：

  A. **配对**：弯曲幅度的公式末尾有 `× (1 − 遮罩)`，所以每一株都必须满足
     `BendAmp ≤ BendRange.Max × (1 − 遮罩(该株原点)) + eps`。
     这条才是关键 —— custom data 与 packed 行是**两块并列的缓冲**，下标写错的症状是
     "数值张冠李戴、剔除一变就换一批"，而光看数值范围完全看不出来，
     必须有一条把两块缓冲**绑在一起**的判据。
     （原来那条是 `高度cm / 高度缩放 == 网格高度`，随 `[1]` 一起拆了。）

  A2. **`[1]` 恒 0**：槽位是"保留"而不是"留着旧值" —— 恒写 0 才能把这件事做成可断言的。

  B. **弯曲幅度的分布带辐射簇的签名**：TG 的公式是 `均匀[0.5,2] × (辐射簇 ? 0.5 : 1) × (1−遮罩)`，
     30% 的簇取 0.5 倍 ⇒ 混合分布的均值应当 ≈ 0.7×1.25 + 0.3×0.625 = **1.06**，
     显著低于纯均匀的 1.25，且下界应当到 ~0.25。
     只查"范围在 [0,2] 内"是不够的：把辐射倍率写成 1.0 也照样落在范围里。

  C. **关掉辐射倍率就回到纯均匀**：`RadialBendScale = 1.0` 时均值必须回到 ≈1.25。
     这条排除"均值偏低其实来自别的原因"。
     ⚠️ **只能查均值，不能查下界**：公式末尾还有 `× (1 − 遮罩)`，关卡里画过的路上那些草的
     弯曲被按比例压低，min 会掉到 0.5 以下 —— 那是**正确行为**。第一版在这里加了
     `min > 0.45` 的断言，实测 0.315 被判成 FAIL，其实是判据漏了遮罩那一项。

⚠️ **不存盘**，跑完关卡是脏的。
"""
import unreal

PKG = "/PCGPlugins/HouseTest"
unreal.EditorLoadingAndSavingUtils.load_map("%s/L_HouseGroundDemo" % PKG)
A = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
g = next(a for a in A.get_all_level_actors()
         if "Ground" in a.get_class().get_name() and "Shaper" not in a.get_class().get_name())

grass = g.get_editor_property("Grass")
bend_max = grass.get_editor_property("BendRange").y
unreal.log("CUSTOM BendRange=%s RadialBendScale=%.2f"
           % (grass.get_editor_property("BendRange"), grass.get_editor_property("RadialBendScale")))


def read():
    n, bend, reserved, origins = g.call_method("DebugReadGroundCoverCustomDataSync", (0,))
    return n, list(bend), list(reserved), list(origins)


def stats(v):
    n = len(v)
    if n == 0:
        return 0.0, 0.0, 0.0
    return min(v), sum(v) / n, max(v)


n, bend, reserved, origins = read()
unreal.log("CUSTOM 读回 %d 株" % n)

# ---- A. 配对：逐实例上界 ----
# 遮罩走 `SampleRoadWeight`（镜像双线性）—— 与 kernel 读的 GPU 色流是同一份权威的两侧投影，
# 8 bit 量化也相同，所以能直接比。eps 给 float 往返和两侧插值的微差留余量。
bad, worst_over, worst_i, on_road = 0, 0.0, -1, 0
for i in range(n):
    o = origins[i]
    mask = g.call_method("SampleRoadWeight", (unreal.Vector2D(o.x, o.y),))
    if mask > 0.05:
        on_road += 1
    bound = bend_max * (1.0 - mask)
    over = bend[i] - bound
    if over > worst_over:
        worst_over, worst_i = over, i
    if over > 0.02:
        bad += 1
unreal.log("CUSTOM A 配对：BendAmp ≤ %.2f×(1−遮罩) —— 越界 %d 株，最大超出 %.4f（第 %d 株）；"
           "遮罩>0.05 的有 %d 株（判据的有效样本）"
           % (bend_max, bad, worst_over, worst_i, on_road))
if on_road < 50:
    unreal.log_warning("CUSTOM A 关卡里几乎没有画过的路，这条判据没有有效样本 —— 先画一条路再测")
unreal.log("CUSTOM A %s" % ("PASS" if bad == 0 and on_road >= 50 else "**FAIL**（custom data 与 packed 行下标错位）"))

# ---- A2. [1] 恒 0 ----
nz = sum(1 for v in reserved if abs(v) > 1e-6)
unreal.log("CUSTOM A2 保留槽 [1]：非零 %d 株  %s" % (nz, "PASS" if nz == 0 else "**FAIL**"))

# ---- B. 分布签名 ----
lo, mean, hi = stats(bend)
unreal.log("CUSTOM B 弯曲幅度：min=%.3f mean=%.3f max=%.3f（期望 min≈0.25 mean≈1.06 max≈2.0）"
           % (lo, mean, hi))
ok_b = 0.98 < mean < 1.15 and lo < 0.45 and hi > 1.8
unreal.log("CUSTOM B %s" % ("PASS" if ok_b else "**FAIL**（分布没有辐射簇的签名）"))

# ---- C. 关掉辐射倍率 ----
sp = g.get_editor_property("Grass")
sp.set_editor_property("RadialBendScale", 1.0)
g.set_editor_property("Grass", sp)
g.call_method("RebuildGroundCover")
n2, bend2, _, _ = read()
lo2, mean2, hi2 = stats(bend2)
unreal.log("CUSTOM C RadialBendScale=1.0：min=%.3f mean=%.3f max=%.3f（期望 mean≈1.25）"
           % (lo2, mean2, hi2))
# 只查均值：下界被遮罩项合法地拉到 0.5 以下（见文件头 C 的 ⚠️）。
ok_c = 1.18 < mean2 < 1.32
unreal.log("CUSTOM C %s" % ("PASS" if ok_c else "**FAIL**（关掉倍率后没回到纯均匀）"))

unreal.log("CUSTOM DONE")
