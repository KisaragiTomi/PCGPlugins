# -*- coding: utf-8 -*-
"""
草朝向验收：证明**朝向是成簇的**，而不是逐叶随机。

这条**只能量、不能看** —— 肉眼分不清"逐叶随机"和"簇权重 0.15"，而两者的画面差别恰恰是
这一轮从 TG 复刻过来的东西（`_generate_grass:270-300` / `:391-416`）。

三条判据：

  A. **近处相关、远处无关**：随机取实例对，按距离分箱，算两株朝向的点积均值。
     同簇（< 1 m）的均值必须显著为正；跨簇（> 2×簇距）的必须趋近 0。
     只看"近处为正"是不够的 —— 如果远处也为正，那说明全场被某个方向整体拉偏了（bug），
     而不是成簇。

  B. **关掉成簇就塌回 0**：把 `ClumpAlignment` 设成 0 再测一遍，A 里的近处相关必须消失。
     这条排除"近处相关其实来自别的原因"（比如朝向被地形梯度带跑了）。

  C. **确定性**：连散两趟，朝向必须**逐位相同**。整簇共用的量由簇身份哈希给，
     换成 InterlockedAdd 的槽位也能跑出"看起来成簇"的画面，但每次重散全场重掷 ——
     而本 pass 每一笔落笔都重扫（石阶 S1 栽过同一枪，且没有任何断言会报红）。

⚠️ **不存盘**，跑完关卡是脏的。
"""
import math
import random

import unreal

PKG = "/PCGPlugins/HouseTest"
unreal.EditorLoadingAndSavingUtils.load_map("%s/L_HouseGroundDemo" % PKG)
A = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
g = next(a for a in A.get_all_level_actors()
         if "Ground" in a.get_class().get_name() and "Shaper" not in a.get_class().get_name())


def read_facings():
    n, origins, facings = g.call_method("DebugReadGroundCoverFacingsSync", (0,))
    return n, list(origins), list(facings)


def correlation(origins, facings, near_cm, far_cm, far_samples=200000, seed=12345):
    """近处点积均值（空间分桶枚举）+ 远处点积均值（随机抽样）。

    ⚠️ **近处不能靠随机取对**：第一版就是这么写的，48k 株铺在 1024 m² 上，随机一对同时落在
       1 m 内的概率只有 ~0.3%，20 万次抽样只换来 623 对，标准误 ≈ 0.028 —— 于是"成簇关掉后
       近处相关 +0.037"被判成 FAIL，其实那是 1.3σ，跟 0 分不开。**欠采样的测试会稳定地
       给出假 FAIL**，而且看起来和真 bug 一模一样。
       改成按 `near_cm` 边长分桶、枚举同桶与相邻桶内的对，近处样本量直接上万。
    远处仍用随机抽样：远对本来就占绝大多数，随机抽 20 万次能拿到十几万对，够稳。
    """
    near2, far2 = near_cm * near_cm, far_cm * far_cm

    buckets = {}
    for i, o in enumerate(origins):
        buckets.setdefault((int(o.x // near_cm), int(o.y // near_cm)), []).append(i)

    near_sum, near_cnt = 0.0, 0
    for (bx, by), members in buckets.items():
        cand = list(members)
        # 只并右邻与上邻，避免每一对被数两次。
        for nb in ((bx + 1, by), (bx, by + 1), (bx + 1, by + 1), (bx + 1, by - 1)):
            cand.extend(buckets.get(nb, ()))
        for ii in range(len(members)):
            i = members[ii]
            for j in cand:
                if j <= i:
                    continue
                dx = origins[i].x - origins[j].x
                dy = origins[i].y - origins[j].y
                if dx * dx + dy * dy >= near2:
                    continue
                near_sum += facings[i].x * facings[j].x + facings[i].y * facings[j].y
                near_cnt += 1

    rng = random.Random(seed)
    n = len(origins)
    far_sum, far_cnt = 0.0, 0
    for _ in range(far_samples):
        i = rng.randrange(n)
        j = rng.randrange(n)
        if i == j:
            continue
        dx = origins[i].x - origins[j].x
        dy = origins[i].y - origins[j].y
        if dx * dx + dy * dy < far2:
            continue
        far_sum += facings[i].x * facings[j].x + facings[i].y * facings[j].y
        far_cnt += 1

    return (near_sum / max(near_cnt, 1), near_cnt,
            far_sum / max(far_cnt, 1), far_cnt)


def set_grass(prop, value):
    sp = g.get_editor_property("Grass")
    sp.set_editor_property(prop, value)
    g.set_editor_property("Grass", sp)
    g.call_method("RebuildGroundCover")


grass = g.get_editor_property("Grass")
clump = grass.get_editor_property("ClumpSize")
unreal.log("ORIENT 簇距=%.0f cm  辐射概率=%.2f  簇权重上限=%.2f  倾倒角=%.0f°"
           % (clump, grass.get_editor_property("ClumpRadialChance"),
              grass.get_editor_property("ClumpAlignment"),
              grass.get_editor_property("LeanDegrees")))

# ---- A. 成簇开着 ----
n, origins, facings = read_facings()
unreal.log("ORIENT 读回 %d 株" % n)
near, nc, far, fc = correlation(origins, facings, 100.0, 2.0 * clump)
unreal.log("ORIENT A 成簇开：近(<1m) 点积均值 = %+.3f (%d 对)   远(>%.0fm) = %+.3f (%d 对)"
           % (near, nc, 2.0 * clump / 100.0, far, fc))
ok_a = near > 0.08 and abs(far) < 0.03
unreal.log("ORIENT A %s" % ("PASS" if ok_a else "**FAIL**（近处要显著为正、远处要趋近 0）"))

# ---- B. 关掉成簇 ----
set_grass("ClumpAlignment", 0.0)
n0, o0, f0 = read_facings()
near0, nc0, far0, fc0 = correlation(o0, f0, 100.0, 2.0 * clump)
unreal.log("ORIENT B 成簇关：近(<1m) = %+.3f (%d 对)   远 = %+.3f (%d 对)" % (near0, nc0, far0, fc0))
# 分桶之后近处样本量上万 ⇒ 标准误 < 0.01，0.03 才是个有意义的判据（第一版只有 623 对，
# 标准误 0.028，那个阈值等于在噪声上划线）。
ok_b = abs(near0) < 0.03
unreal.log("ORIENT B %s" % ("PASS" if ok_b else "**FAIL**（关掉后近处相关必须消失）"))
unreal.log("ORIENT   ⇒ 成簇带来的近处相关增量 = %+.3f" % (near - near0))

# ---- C. 确定性 ----
#
# ⚠️ **不能按下标比**：槽位来自 `InterlockedAdd`，两趟之间**本来就不保证同序**
#    （这正是本工程"槽位不是稳定身份"那条纪律的另一面）。第一版按 `f1[i]` vs `f2[i]` 比，
#    比的是两株不同的草，必然 FAIL。要比就得按**位置**排序后比 —— 位置由格身份决定，
#    是稳定的，所以排完序两趟应当逐项对齐。
def by_position(origins, facings):
    order = sorted(range(len(origins)),
                   key=lambda i: (round(origins[i].x, 2), round(origins[i].y, 2)))
    return ([(round(origins[i].x, 2), round(origins[i].y, 2)) for i in order],
            [(facings[i].x, facings[i].y) for i in order])


restore = grass.get_editor_property("ClumpAlignment")
set_grass("ClumpAlignment", restore)
n1, o1, f1 = read_facings()
# 幂等哈希会把重复调用吃掉，所以先扰动一下再改回来，强迫真的重散一趟。
set_grass("ClumpAlignment", 0.25)
set_grass("ClumpAlignment", restore)
n2, o2, f2 = read_facings()

p1, d1 = by_position(o1, f1)
p2, d2 = by_position(o2, f2)
same = (n1 == n2) and p1 == p2 and all(
    abs(d1[i][0] - d2[i][0]) < 1e-5 and abs(d1[i][1] - d2[i][1]) < 1e-5 for i in range(len(d1)))
unreal.log("ORIENT C 两趟散布：%d vs %d 株，按位置排序后朝向一致 = %s  %s"
           % (n1, n2, same, "PASS" if same else "**FAIL**（随机源漂了 —— 查是不是用了槽位当种子）"))

unreal.log("ORIENT DONE")
