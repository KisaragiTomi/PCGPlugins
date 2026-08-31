# -*- coding: utf-8 -*-
"""
TinyGlade 演示关卡的无头回归。

按项目惯例以**日志断言**为准、不信退出码：每条断言打一行 [PASS]/[FAIL]，末尾打总计与
REGRESS OK / REGRESS FAILED。

两条实测陷阱（写进注释，别再踩）：
 ① 顶点色笔刷是 **3D 球**，脚本落笔必须自己 SampleHeight 贴地 —— 固定 Z=0 会让抬高的坡面
    整段落在球外，看起来像"路没画上/镜像没保存"。
 ② 必须用真 RHI：-nullrhi 下全局着色器没编译，AddSetCountersPass 的 TShaderMapRef 会断言失败。
"""
import math

import unreal

FAILS = []
PASSES = [0]


def check(label, ok, detail=""):
    if ok:
        PASSES[0] += 1
        unreal.log("[PASS] %s %s" % (label, detail))
    else:
        FAILS.append(label)
        unreal.log_error("[FAIL] %s %s" % (label, detail))


def near(label, got, want, tol, unit=""):
    check(label, abs(got - want) <= tol, "got=%.3f%s want=%.3f%s tol=%.3f" % (got, unit, want, unit, tol))


def actors():
    return unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()


def find(label):
    for a in actors():
        if a.get_actor_label() == label:
            return a
    return None


def settle_tris(mesh, tries=8):
    """等在途的异步编辑落地后再读三角形数。

    UCSMesh::EditMeshAsync 的收尾是一个 GameThread AsyncTask，而
    -ExecutePythonScript 跟本没有 tick 去泵它 —— 刚拖完就读会读到
    “上一次真正上传完的那一版”，最后一帧还在 PendingBodySnapshot 里。
    GetTriangleCountSync 自己会 flush，顺带把队列泵一次，所以连读到
    两次相同即已收敛。

    ⚠️ 这**不是**被测代码的毛病（编辑器里每帧都有 tick）。之所以以前看不见，
    是因为拖动期每帧都有阻塞刷新，无意中替这条尾巴做了同步 ——
    把阻塞消掉之后它才露出来。
    """
    if not mesh:
        return -1
    last = mesh.get_triangle_count_sync()
    for _ in range(tries):
        cur = mesh.get_triangle_count_sync()
        if cur == last:
            return cur
        last = cur
    return last


def paint_line(ground, x0, y0, x1, y1, steps):
    """沿直线落笔。每一笔都自己贴地 —— 笔刷是 3D 球，固定 Z 会在坡面上整段落空。"""
    ground.begin_paint_stroke()
    for i in range(steps + 1):
        t = float(i) / steps
        x = x0 + (x1 - x0) * t
        y = y0 + (y1 - y0) * t
        z = ground.sample_height(unreal.Vector2D(x, y))
        ground.apply_paint_stroke(unreal.Vector(x, y, z))
    ground.end_paint_stroke()


# =============================================================================
# L_HouseGroundDemo：门拱（道路推导）+ 承重柱（落座）
# =============================================================================
def demo_house_ground():
    unreal.log("========== L_HouseGroundDemo ==========")
    unreal.EditorLoadingAndSavingUtils.load_map("/PCGPlugins/HouseTest/L_HouseGroundDemo")

    ground = find("Ground_Demo")
    road_house = find("House_Road")
    pillar_house = find("House_Pillar")
    check("actors present", ground and road_house and pillar_house)
    if not (ground and road_house and pillar_house):
        return

    # ---- 初始：没画路就不该有门 ----
    ground.reset_paint()
    road_house.rebuild_house()
    check("no road -> no doors", road_house.get_open_door_count() == 0,
          "doors=%d" % road_house.get_open_door_count())

    # ---- 画一条南北向的路穿过 House_Road（局部 X 是长轴，路沿 Y 走横穿两面长墙） ----
    loc = road_house.get_actor_location()
    paint_line(ground, loc.x, loc.y - 800.0, loc.x, loc.y + 800.0, 16)

    doors = road_house.get_open_door_count()
    # 长墙 600、护角 60 ⇒ 可用 480，目标段距 150 ⇒ round(3.2) = 3 段/墙，两面长墙共 6 拱。
    check("road across the house opens 6 arches", doors == 6, "doors=%d" % doors)

    # ---- 门框砖：clip 的配套件。没有它，洞缘的厚度断口会一眼看穿墙 ----
    bricks = road_house.get_frame_brick_count()
    check("open arches grow frame bricks", bricks > 0, "bricks=%d" % bricks)
    # 每个洞两条曲线（上边界含门樘 / 下边界），门贴地所以只有上边界那条：
    # 6 个拱 × 一圈砖，砖长 26cm、拱周长约 (2×165 + π×55) ≈ 503cm ⇒ 每拱约 18 块。
    check("frame brick count is in the right ballpark", 60 <= bricks <= 200, "bricks=%d" % bricks)

    # ---- GPU 侧真值：CPU 记的砖数与 GPU 上那个被 indirect draw 消费的计数器必须相等 ----
    #
    # 这条本身不是重点，重点是它让下面那条"擦掉之后必须归零"不是空判据：如果回读通路
    # 恒返回 0，这一条会先红。**成对出现，缺一条另一条就可以假绿。**
    gpu_bricks = road_house.debug_read_frame_brick_count_gpu_sync()
    check("the GPU brick counter agrees with the CPU record", gpu_bricks == bricks,
          "gpu=%d cpu=%d" % (gpu_bricks, bricks))
    # 顺带：GPU 上画的基础网格 / 材质是不是我们以为的那两样（母材质被静默换掉的那一类）。
    mismatch = road_house.debug_get_gpu_asset_mismatch_sync()
    check("the GPU draws the base mesh and material we think it does", mismatch == "", mismatch)

    # ---- 幂等：同一世界状态再复评一次，门集合不许变 ----
    road_house.reevaluate_site()
    check("reevaluate is idempotent", road_house.get_open_door_count() == doors,
          "doors=%d" % road_house.get_open_door_count())

    # ---- 拱间墩：双阈迟回（PierStyleMaxWidth 60 / PierStyleRestoreWidth 75）----
    #
    # 跨度 = 段距 − 拱宽，而拱宽 = round((段距 − PierWidth) × 离地收窄 / 量子) × 量子
    # ⇒ 平地上**跨度 ≈ PierWidth ± 量子/2**（DoorWidthQuantum = 2）。所以直接拉 PierWidth
    # 就等于直接拉跨度，不必为测试另开后门。
    #
    # ⚠️ 断言必须落在 get_pier_span_count() 上：迟回是**路径依赖**的 —— 同一个跨度，从窄边
    # 过来是墩、从宽边过来是墙 —— 而从外面只看得见三角形数，那个数还同时被拱宽、洞数、屋面
    # 一起拖着动，翻不翻样式根本分辨不出来。
    def piers_at(width):
        road_house.set_editor_property("PierWidth", float(width))
        road_house.reevaluate_site()
        return road_house.get_pier_span_count()

    base_pier = road_house.get_editor_property("PierWidth")
    road_house.rebuild_house()          # 清掉迟回记忆，从确定的状态进
    narrow = piers_at(50.0)             # 跨度 ≈ 50 ≤ 60 ⇒ 墩
    check("adjacent arches leave pier spans", narrow > 0, "piers=%d" % narrow)
    band_below = piers_at(68.0)         # 跨度 ≈ 68，落在 (60, 75) 迟回带内
    check("widening into the hysteresis band keeps the piers", band_below == narrow,
          "piers=%d (was %d)" % (band_below, narrow))
    wide = piers_at(85.0)               # 跨度 ≈ 85 ≥ 75 ⇒ 转回灰泥墙
    check("past the high threshold the piers turn back into wall", wide == 0, "piers=%d" % wide)
    band_above = piers_at(68.0)         # **同一个 68**，这次从宽边过来
    check("the same span stays a wall when it is approached from above", band_above == 0,
          "piers=%d (from below it was %d)" % (band_above, band_below))
    back = piers_at(50.0)
    check("dropping back under the low threshold restores the piers", back == narrow,
          "piers=%d (was %d)" % (back, narrow))
    road_house.set_editor_property("PierWidth", base_pier)
    road_house.rebuild_house()

    # 墩宽与砖脚宽度是一对必须配平的参数：两条砖脚各骑在跨度两端、各伸进跨度半个
    # FrameBrickDepth ⇒ 墩宽超过它，墩正中就会露出一条能看穿的缝（渲染层的缝，不是几何洞，
    # 但一样难看）。这条守着**出厂默认值**，别让谁单独调走其中一个。
    cdo = unreal.get_default_object(unreal.CSHouseActor)
    cdo_pier = cdo.get_editor_property("PierWidth")
    cdo_brick = cdo.get_editor_property("FrameBrickDepth")
    check("the shipped pier is narrow enough for its two brick jambs to close up",
          cdo_pier <= cdo_brick + 0.01,
          "PierWidth=%.1f FrameBrickDepth=%.1f" % (cdo_pier, cdo_brick))

    # ---- 擦掉道路 → 拱合拢 ----
    ground.reset_paint()
    road_house.rebuild_house()
    check("erasing the road closes every arch", road_house.get_open_door_count() == 0,
          "doors=%d" % road_house.get_open_door_count())
    check("closing the arches removes the frame bricks", road_house.get_frame_brick_count() == 0,
          "bricks=%d" % road_house.get_frame_brick_count())

    # ---- 上一条只证明了 **CPU 的记录**是 0。GPU 上那个计数器呢？----
    #
    # 这是本项目第三次栽的那一类 bug 的守门人（既有 bug，D7 那轮靠出图才发现）：砖数掉到 0 时
    # `RebuildFrame` 撤走实例源并清空交接缓存，于是**下一轮 `EnsureFrameComponent` 把同一批
    # buffer 又交回去** —— 带着陈旧的计数器。画面上 12 层砖原样立着，而 `get_frame_brick_count()`
    # / 三角形数 / 零阻塞**所有无头断言全绿**。复现路径很日常：画一条路，再擦掉。
    #
    # ⚠️ 这条断言**验过会报红**：把 `RebuildFrame` 里"撤实例源之前先把 counter 清零"那几行
    # 临时注释掉，同一轮跑出来是 `gpu=96 cpu=0`（passed=174 failed=1 / REGRESS FAILED），
    # 而上面那条 `closing the arches removes the frame bricks bricks=0` **仍然是 PASS** ——
    # 这一对就是"CPU 说 0、GPU 说 96"的现场。
    #
    # 多这一次 `reevaluate_site()` 是**防御性的，不是凑数**：撤实例源的那一刻组件手上什么都
    # 没有（读到的当然是 0），陈旧计数器要等到**下一次交接**才回到组件身上。
    # 实测这一关不加它也红 —— 但那是因为 `rebuild_house()` 内部已经重入过一次 `RebuildFrame`，
    # 是个偶然。加上它，判据就不依赖"上面那句话恰好会重入几次"。
    road_house.reevaluate_site()
    gpu_bricks = road_house.debug_read_frame_brick_count_gpu_sync()
    check("erasing the road zeroes the GPU brick counter too, not just the CPU record",
          gpu_bricks == 0, "gpu=%d cpu=%d" % (gpu_bricks, road_house.get_frame_brick_count()))

    # ---- 承重柱：HeightOffset=150 ⇒ 落座 z=150，周界 6 柱 ----
    pillar_house.reevaluate_site()
    near("pillar house seats at HeightOffset", pillar_house.get_actor_location().z, 150.0, 0.6, "cm")
    check("perimeter yields 6 pillars", pillar_house.get_pillar_count() == 6,
          "pillars=%d" % pillar_house.get_pillar_count())

    # ---- 落回地面 ⇒ 柱全消 ----
    pillar_house.set_editor_property("HeightOffset", 0.0)
    pillar_house.rebuild_house()
    near("dropping the house seats it on the ground", pillar_house.get_actor_location().z, 0.0, 0.6, "cm")
    check("a grounded house has no pillars", pillar_house.get_pillar_count() == 0,
          "pillars=%d" % pillar_house.get_pillar_count())
    pillar_house.set_editor_property("HeightOffset", 150.0)

    # ---- 摆位快路径：只平移、门集合不翻转时不许重建房体 ----
    # （形状/摆位两级哈希拆开之后才可达的那条路径）
    # 必须在**有门**的状态下测：0 == 0 那种断言就算摆位路径整个坏掉也照样绿。
    paint_line(ground, loc.x, loc.y - 800.0, loc.x, loc.y + 800.0, 16)
    road_house.reevaluate_site()
    before = road_house.get_opening_count()
    check("the translation test starts with a non-empty opening set", before > 0, "openings=%d" % before)

    base = road_house.get_actor_location()
    flush_before = unreal.CSMesh.get_blocking_flush_count()
    for i in range(1, 11):
        # 沿路走（路是南北向的），门集合因此不该翻转 —— 走横向会真的开关门，那是另一回事。
        road_house.set_actor_location(unreal.Vector(base.x, base.y + i * 5.0, base.z), False, False)
        road_house.reevaluate_site()
    flush_delta = unreal.CSMesh.get_blocking_flush_count() - flush_before

    check("a pure translation keeps the opening set", road_house.get_opening_count() == before,
          "openings=%d (was %d)" % (road_house.get_opening_count(), before))
    # P2 验收门：整栋一次 EditMeshAsync 之后，复评期间不许有任何阻塞刷新。
    check("10 translated reevaluations block the game thread zero times", flush_delta == 0,
          "flushes=%d" % flush_delta)

    road_house.set_actor_location(base, False, False)
    ground.reset_paint()
    road_house.rebuild_house()

    # ---- P2 验收门：一次落笔期间零阻塞刷新 ----
    flush_before = unreal.CSMesh.get_blocking_flush_count()
    ground.begin_paint_stroke()
    for i in range(12):
        y = loc.y - 600.0 + i * 100.0
        z = ground.sample_height(unreal.Vector2D(loc.x, y))
        ground.apply_paint_stroke(unreal.Vector(loc.x, y, z))
        ground.flush_paint_to_gpu(False)      # EdMode 每帧做的就是这一下
    flush_delta = unreal.CSMesh.get_blocking_flush_count() - flush_before
    check("a 12-dab stroke blocks the game thread zero times", flush_delta == 0,
          "flushes=%d" % flush_delta)
    ground.end_paint_stroke()
    ground.reset_paint()
    road_house.rebuild_house()

    # ---- 脊向滞回：长短轴穿越时屋顶不翻面 ----
    axis_before = road_house.get_editor_property("RidgeAxis")
    road_house.set_editor_property("FootprintSize", unreal.Vector2D(600.0, 620.0))   # Y 只长 3%，在滞回带内
    road_house.reevaluate_site()
    check("inside the hysteresis band the ridge holds",
          road_house.get_editor_property("RidgeAxis") == axis_before,
          "axis=%s (was %s)" % (road_house.get_editor_property("RidgeAxis"), axis_before))
    road_house.set_editor_property("FootprintSize", unreal.Vector2D(600.0, 900.0))   # 远超 1.15 倍
    road_house.reevaluate_site()
    check("clearly past the band the ridge flips",
          road_house.get_editor_property("RidgeAxis") != axis_before,
          "axis=%s" % road_house.get_editor_property("RidgeAxis"))
    road_house.set_editor_property("FootprintSize", unreal.Vector2D(600.0, 400.0))
    road_house.reevaluate_site()

    # ---- P2 验收门：**拉尺寸**期间零阻塞刷新 ----
    # 上面三条同族断言测的是“平移”与“落笔”，**没有一条改过 FootprintSize**，
    # 而拉尺寸恰恰是最糟的那条路：房体顶点数、柱数、门框砖实例源的包围盒
    # 全都是 FootprintSize 的函数，没预留的话每一帧都要重新分配 + 重走一次阻塞交接。
    #
    # 必须在**有门**的状态下测：门框砖是唯一走实例化交接（SetInstanceSourceGPU，
    # 内部 SetStreamLayoutSync / ResizeStreamsSync / EnsureCapacitySync + 无条件 EditMeshSync）
    # 的一路，没砖的话这条断言就算交接路整个坏掉也照样绿。
    paint_line(ground, loc.x, loc.y - 800.0, loc.x, loc.y + 800.0, 16)
    road_house.set_editor_property("FootprintSize", unreal.Vector2D(600.0, 400.0))
    road_house.rebuild_house()      # 容量与包围盒的一次性成本付在这里，不许落进拖动里
    check("the resize test starts with frame bricks", road_house.get_frame_brick_count() > 0,
          "bricks=%d" % road_house.get_frame_brick_count())

    flush_before = unreal.CSMesh.get_blocking_flush_count()
    for i in range(1, 13):
        # 模拟拖动：每帧长 5 cm（真实拖拽的量级），12 帧共 60 cm。
        # 只长 X（本来就是长轴），脊向不会翻 —— 翻脊是另一回事，混进来就分不清
        # 到底是“拉尺寸阻塞”还是“翻脊重建阻塞”。
        road_house.set_editor_property("FootprintSize", unreal.Vector2D(600.0 + i * 5.0, 400.0))
        road_house.reevaluate_site()
    flush_delta = unreal.CSMesh.get_blocking_flush_count() - flush_before
    check("dragging FootprintSize for 12 frames blocks the game thread zero times", flush_delta == 0,
          "flushes=%d" % flush_delta)

    # ---- P2 的另一半：拖尺寸时门框砖到底跟没跟上（2026-08-31 疑案 A 的判据）----
    #
    # 上面那条只保证「拖得不阻塞」，它对「砖根本没跟着走」一个字都说不了：
    # 砖数 / GPU 回读 / 三角形数 / 零阻塞四条断言在缺陷复现时**全部照绿**。
    #
    # ⚠️ **必须拉 Y，拉 X 报不出来**（实测）：路沿 Y 走，门因此开在 ±Y 那两条边上，
    # 而它们的**长度 = FootprintSize.X**。拉 X 会把这两条边的长度一起改掉 ⇒ 洞的
    # 边局部量跟着变 ⇒ 无论哈希对不对都会重排，缺陷被掩盖（故意把墙框架项从哈希里
    # 拿掉、拉 X 那条循环仍然 scatters=12，一点都不红）。
    # 拉 **Y** 才是那条「墙移动了、边局部量一个字没变」的路。往**小**里拉，
    # X=600 始终是长轴，翻脊不会混进来。
    road_house.set_editor_property("FootprintSize", unreal.Vector2D(600.0, 400.0))
    road_house.rebuild_house()
    scatter_before = road_house.get_frame_scatter_count()
    for i in range(1, 13):
        road_house.set_editor_property("FootprintSize", unreal.Vector2D(600.0, 400.0 - i * 5.0))
        road_house.reevaluate_site()
    scatter_delta = road_house.get_frame_scatter_count() - scatter_before
    # 判下界不判等号：哈希短路吸收无效唤醒是**设计**。缺陷复现时这里是 **0**。
    check("dragging FootprintSize across the door-bearing walls re-lays the frame bricks",
          scatter_delta >= 10, "scatters=%d (want >= 10 over 12 drag frames)" % scatter_delta)
    road_house.set_editor_property("FootprintSize", unreal.Vector2D(600.0, 400.0))
    road_house.reevaluate_site()

    # 消 flush 不能靠“少摆砖”换：拖完的几何必须与“直接按最终尺寸全量重建”逐字相同。
    # （get_triangle_count_sync 本身就是一次阻塞回读，只能在测量窗口**之外**调。）
    drag_mesh = road_house.get_tiny_glade_mesh()
    drag_tris = settle_tris(drag_mesh)
    drag_bricks = road_house.get_frame_brick_count()
    drag_openings = road_house.get_opening_count()
    check("the dragged house still has geometry", drag_tris > 0 and drag_bricks > 0,
          "tris=%d bricks=%d" % (drag_tris, drag_bricks))

    road_house.rebuild_house()
    rebuilt_mesh = road_house.get_tiny_glade_mesh()
    rebuilt_tris = settle_tris(rebuilt_mesh)
    check("the dragged body matches a full rebuild at the same size", rebuilt_tris == drag_tris,
          "dragged=%d rebuilt=%d" % (drag_tris, rebuilt_tris))
    check("the dragged frame matches a full rebuild at the same size",
          road_house.get_frame_brick_count() == drag_bricks and road_house.get_opening_count() == drag_openings,
          "bricks=%d/%d openings=%d/%d" % (road_house.get_frame_brick_count(), drag_bricks,
                                           road_house.get_opening_count(), drag_openings))

    # ---- 同一条纪律的柱版：柱走独立网格，容量预留是另一处 ----
    pillar_house.set_editor_property("FootprintSize", unreal.Vector2D(600.0, 400.0))
    pillar_house.rebuild_house()
    check("the pillar resize test starts with pillars", pillar_house.get_pillar_count() > 0,
          "pillars=%d" % pillar_house.get_pillar_count())
    flush_before = unreal.CSMesh.get_blocking_flush_count()
    for i in range(1, 13):
        pillar_house.set_editor_property("FootprintSize", unreal.Vector2D(600.0 + i * 5.0, 400.0))
        pillar_house.reevaluate_site()
    flush_delta = unreal.CSMesh.get_blocking_flush_count() - flush_before
    check("dragging a pillared house for 12 frames blocks the game thread zero times", flush_delta == 0,
          "flushes=%d" % flush_delta)
    check("the dragged pillared house still has pillars", pillar_house.get_pillar_count() > 0,
          "pillars=%d" % pillar_house.get_pillar_count())

    road_house.set_editor_property("FootprintSize", unreal.Vector2D(600.0, 400.0))
    pillar_house.set_editor_property("FootprintSize", unreal.Vector2D(600.0, 400.0))
    ground.reset_paint()
    road_house.rebuild_house()
    pillar_house.rebuild_house()


# =============================================================================
# L_TerrainOpsDemo：高度场 + 石阶 + 落座跟随
# =============================================================================
def demo_terrain_ops():
    unreal.log("========== L_TerrainOpsDemo ==========")
    unreal.EditorLoadingAndSavingUtils.load_map("/PCGPlugins/HouseTest/L_TerrainOpsDemo")

    ground = find("Ground_Demo")
    shaper = find("Shaper_Mound")
    check("actors present", ground and shaper)
    if not (ground and shaper):
        return

    # 链 A 的裙边噪声/二次抬升**显式写死**，理由同 S2 抖动那一段：靠 CDO 默认值的话哪天默认
    # 改了、断言还绿着，覆盖面却悄悄变了。
    shaper.set_editor_property("SkirtNoiseAmount", 0.5)
    shaper.set_editor_property("SkirtNoiseWavelength", 300.0)
    shaper.set_editor_property("SkirtNoiseSeed", 0)
    shaper.set_editor_property("SecondaryLiftScale", 0.021)
    ground.reset_paint()
    shaper.rebuild_terrain()

    c = shaper.get_actor_location()
    lift = shaper.get_editor_property("LiftHeight")
    lift_scale = shaper.get_editor_property("SecondaryLiftScale")
    # ⬇ 剖面上的采样位置全部由 Radius / Falloff 算出来，不写死。
    # 岩壳（链 B）把塑形物拉到了 600 / 800（5.53 m 的胞腔要求裙边装得下），
    # 原来写死的 900 / 500 那两个数字于是全错位：900 从"羽化之外"变成了裙边中段。
    radius = shaper.get_editor_property("Radius")
    falloff = shaper.get_editor_property("FalloffDistance")
    reach = radius + falloff

    # ---- 剖面：盘内恒为台高，盘外 smoothstep 羽化到 0。半径 300 / 羽化 400 / 台高 300 ----
    # ⚠️ 台顶不再等于 LiftHeight：二次抬升（原型 attribwrangle5）按原型口径**照抬台顶**
    # 一档 SecondaryLiftScale。写成表达式而不是 306.3，改档位时这条不会变成假绿。
    near("mound top = LiftHeight plus the secondary lift",
         ground.sample_height(unreal.Vector2D(c.x, c.y)), lift * (1.0 + lift_scale), 1.0, "cm")
    near("outside the falloff is flat",
         ground.sample_height(unreal.Vector2D(c.x + reach + 200.0, c.y)), 0.0, 0.5, "cm")

    # ---- 裙边噪声：只减不加 ----
    # 关掉噪声取同一点作参照，而不是和一个常数比 —— 噪声是**随位置**啃的，任何常数期望值
    # 都只是在测"这一点恰好被啃了多少"，改波长/种子就得跟着改，那种断言守不住任何东西。
    # S = 0.5 的等值线在盘边往外半个羽化处（smoothstep 对称）—— 写成表达式，
    # 改 Radius / Falloff 时这条不会变成假绿。
    skirt = unreal.Vector2D(c.x + radius + 0.5 * falloff, c.y)
    h_noisy = ground.sample_height(skirt)
    shaper.set_editor_property("SkirtNoiseAmount", 0.0)
    shaper.rebuild_terrain()
    h_clean = ground.sample_height(skirt)
    check("the skirt noise only erodes, never raises", h_noisy <= h_clean + 1e-3,
          "noisy=%.3f clean=%.3f cm" % (h_noisy, h_clean))
    check("the skirt noise really bites", h_clean - h_noisy > 1.0,
          "eroded=%.3f cm" % (h_clean - h_noisy))
    # 噪声关掉之后剖面必须逐字回到那条纯 smoothstep（+ 二次抬升）：S=0.5 处 h = Top·(0.5 + k·0.5^1.5)。
    # 容差沿用原来的 3 cm：镜像是 50 cm 格上的双线性重建，曲面在格中间本来就会下垂一点。
    near("skirt midpoint is half height once the noise is off", h_clean,
         lift * (0.5 + lift_scale * 0.5 ** 1.5), 3.0, "cm")
    shaper.set_editor_property("SkirtNoiseAmount", 0.5)
    shaper.rebuild_terrain()

    # ---- 落座跟随：台顶放一栋房子，抬台 Δ 屋顶恰好升 Δ，且升降对称 ----
    house = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).spawn_actor_from_class(
        unreal.load_class(None, "/PCGPlugins/HouseTest/BP_TinyGladeHouse.BP_TinyGladeHouse_C"),
        unreal.Vector(c.x, c.y, 0.0))
    check("spawned a house on the mound", house is not None)
    if house:
        # CDO 的默认值不传播到同会话 spawn 的实例 —— 实例上必须再写一份（实测陷阱 ②）。
        house.set_editor_property("Ground", ground)
        house.set_editor_property("HeightOffset", 0.0)
        house.rebuild_house()
        # 落座的判据是"房子踩在**当前**台顶上"，不是踩在某个常数上 —— 拿 sample_height 当期望值，
        # 这条断言就不会随剖面公式（二次抬升、裙边噪声）一起失效，测的仍然是"跟不跟得住"。
        top0 = ground.sample_height(unreal.Vector2D(c.x, c.y))
        near("house seats on the mound top", house.get_actor_location().z, top0, 0.6, "cm")

        # ⬇ 写成 lift + 100 而不是写死 400：演示塑形物的台高已经因为岩壳抬高了（见
        # TinyGladeResizeDemos.py 里那段推导），写死 400 会变成"降低"，下面那条 +100 的断言就假了。
        shaper.set_editor_property("LiftHeight", lift + 100.0)
        shaper.rebuild_terrain()
        house.reevaluate_site()
        top1 = ground.sample_height(unreal.Vector2D(c.x, c.y))
        near("raising the mound raises the house to the new top",
             house.get_actor_location().z, top1, 0.6, "cm")
        near("LiftHeight +100 lifts the plateau by exactly 100 x (1 + secondary lift)",
             top1 - top0, 100.0 * (1.0 + lift_scale), 0.6, "cm")

        shaper.set_editor_property("LiftHeight", lift)
        shaper.rebuild_terrain()
        house.reevaluate_site()
        near("lowering it drops the house right back (absolute, not a ratchet)",
             house.get_actor_location().z, top0, 0.6, "cm")

        # 绝对式而非增量式：同一地形连续复评两次房子不许再动。
        z1 = house.get_actor_location().z
        house.reevaluate_site()
        house.reevaluate_site()
        near("repeated reevaluation does not drift", house.get_actor_location().z, z1, 0.01, "cm")

        unreal.get_editor_subsystem(unreal.EditorActorSubsystem).destroy_actor(house)


# =============================================================================
# L_TerrainOpsDemo：GPU 石阶（marching squares，计划「石阶改造」的 S1/S2/S3）
#
# **全项目唯一的一条石阶路**：石阶归地面，一次 dispatch 从合成后的高度场扫等值线。
# 上面 demo_terrain_ops 里原先还有一段测旧路（塑形物自持 StepComponents，CPU 排层/弧段/铺装）
# 的断言，连同那条产线路径一起随 2026-08-30「裁决一」第二步删除了（`legacy_shaper_steps`
# 那个测试自备夹具本来就是专门为让旧路断言活下来才做的，旧路走了它就没有存在理由）。
#
# S1 的核心验收项是第二段：**两座相接土台的接合处不断裂**。旧路的等高线用"关于本座中心
# 星形"的闭式解求半径，接合处那段等值线不属于任何一座，必然断段；marching squares 扫的是
# 合成之后的场，接合处只是普通的一格。
# =============================================================================
STAIR_MESH = "/Game/TinyGlade/Meshes/stairs_step/StaticMeshes/stairs_step.stairs_step"   # 实测 100×100×100 cm 居中立方体
# TG 的 15% 支线用的就是这颗（`_rocky_terrain_stairs_stairs.cs:511-547`）。原件最长轴实测 1.352 m，
# TG 的 mix(0.2, 0.4) 因此是 27–54 cm —— `StairPebbleSize` 的默认值就是这么来的。
PEBBLE_MESH = "/Game/TinyGlade/Meshes/stairs_pebble/StaticMeshes/stairs_pebble.stairs_pebble"


def read_stairs(ground):
    """(数量, 世界空间原点列表)。回读只用于验收 —— 运行路径一个字节都不回读。"""
    res = ground.debug_read_stairs_sync()
    if isinstance(res, tuple):
        return int(res[0]), list(res[1])
    return int(res), []


def read_pebbles(ground):
    """(数量, 世界空间原点列表)。同 read_stairs，读的是石子那一对 buffer。"""
    res = ground.debug_read_stair_pebbles_sync()
    if isinstance(res, tuple):
        return int(res[0]), list(res[1])
    return int(res), []


def demo_gpu_stairs():
    unreal.log("========== L_TerrainOpsDemo / GPU stairs (S1 + S2 jitter) ==========")
    unreal.EditorLoadingAndSavingUtils.load_map("/PCGPlugins/HouseTest/L_TerrainOpsDemo")

    ground = find("Ground_Demo")
    shaper = find("Shaper_Mound")
    step_mesh = unreal.load_asset(STAIR_MESH)
    pebble_mesh = unreal.load_asset(PEBBLE_MESH)
    check("actors and step mesh present", ground and shaper and step_mesh)
    check("the TG pebble mesh is present", pebble_mesh is not None)
    if not (ground and shaper and step_mesh):
        return

    # 石阶归**地面**：扫描域是全局的（marching squares 扫的是全部塑形物合成之后的高度场），
    # 格不与任何一座对齐，归任一座都不对。
    ground.set_editor_property("StairMesh", step_mesh)
    ground.set_editor_property("StairStepHeight", 30.0)
    ground.set_editor_property("StairCellSize", 100.0)          # ⚠️ 必须 ≈ 石阶长度，见 C++ 注释
    ground.set_editor_property("StairBlockSize", unreal.Vector(60.0, 100.0, 30.0))
    ground.set_editor_property("StairRoadThreshold", 0.35)
    ground.set_editor_property("StairZOffset", 0.0)
    # S2 的逐实例抖动**显式写死**：下面几条断言（尤其是零阻塞那条）要说清自己覆盖的是哪一档
    # 抖动，靠 CDO 默认值的话哪天默认改了、断言还绿着，覆盖面却悄悄变了。
    ground.set_editor_property("StairLengthBloat", 1.06)
    ground.set_editor_property("StairLengthJitter", 0.10)
    ground.set_editor_property("StairSizeJitter", 0.12)
    ground.set_editor_property("StairYawJitter", 6.0)
    ground.set_editor_property("StairJitterSeed", 1)
    # 小石子（TG 的 15% 支线）同样**显式写死**：靠 CDO 默认值的话哪天默认改了、断言还绿着，
    # 覆盖面却悄悄变了 —— 与上面几条抖动参数同一条理由。
    ground.set_editor_property("StairPebbleMesh", pebble_mesh)
    ground.set_editor_property("StairPebbleChance", 0.15)
    ground.set_editor_property("StairPebbleSize", unreal.Vector2D(27.0, 54.0))
    ground.reset_paint()
    shaper.rebuild_terrain()

    c = shaper.get_actor_location()
    radius = shaper.get_editor_property("Radius")
    falloff = shaper.get_editor_property("FalloffDistance")
    lift = shaper.get_editor_property("LiftHeight")
    reach = radius + falloff

    # ---- 未画路 ⇒ 0 级 ----
    ground.rebuild_stairs()
    count, _ = read_stairs(ground)
    check("no road -> no GPU stairs", count == 0, "stairs=%d" % count)

    # ---- 画路穿台 ⇒ 长出石阶（S1 验收项前半）----
    paint_line(ground, c.x, c.y - reach - 200.0, c.x, c.y + reach + 200.0, 24)
    count, origins = read_stairs(ground)
    check("a road over the mound grows GPU stairs", count > 0, "stairs=%d" % count)
    check("every counted instance came back with a row", len(origins) == count,
          "rows=%d count=%d" % (len(origins), count))

    # ---- TG 的 15% 小石子 ----
    # 数量带宽 [5%, 30%] 只抓"门开反了 / 概率没接上"这类整数量级的错。
    # **"随机源是格身份不是槽位"那条纪律由单测守**（GroundStairs.PebblesAreCellDeterministic
    # 复算哈希逐颗对账）—— 数量类断言对它一个字都说不出来，这正是 S1 在 `.w` 上错了一整轮
    # 而全绿的原因。这里只守"这一支在演示关卡里真的活着"。
    p_count, p_origins = read_pebbles(ground)
    check("the 15% pebble branch actually fires", p_count > 0, "pebbles=%d" % p_count)
    check("every counted pebble came back with a row", len(p_origins) == p_count,
          "rows=%d count=%d" % (len(p_origins), p_count))
    if count > 0:
        ratio = float(p_count) / float(count)
        check("the pebble rate is in the neighbourhood of TG's 15%", 0.05 < ratio < 0.30,
              "pebbles=%d stairs=%d ratio=%.3f" % (p_count, count, ratio))

    # ---- 生产者的 buffer 与**组件手上那一份**是同一批吗 ----
    #
    # `read_stairs` 读的是 `StairBuffers`（生产者自持），而画面上画的是组件被交接过去的那一份。
    # 门框砖那个既有 bug 的现场恰恰是两者不一致 —— 生产者以为清干净了，组件却握着一批
    # 带陈旧计数器的 buffer。这两条把"生产者写了什么"与"绘制会读到什么"分开断言。
    gpu_stairs = ground.debug_read_stair_count_gpu_sync()
    check("the GPU stair counter the draw consumes agrees with the producer's",
          gpu_stairs == count, "gpu=%d producer=%d" % (gpu_stairs, count))
    gpu_pebbles = ground.debug_read_stair_pebble_count_gpu_sync()
    check("the GPU pebble counter the draw consumes agrees with the producer's",
          gpu_pebbles == p_count, "gpu=%d producer=%d" % (gpu_pebbles, p_count))
    # 顺带：GPU 上画的基础网格 / 材质是不是我们以为的那两样（`StairMesh`/`StairMaterial`
    # 一直是 NULL 那一枪就是从这里打进来的）。
    mismatch = ground.debug_get_gpu_asset_mismatch_sync()
    check("the GPU draws the stair mesh and material we think it does", mismatch == "", mismatch)

    # ---- 接合处不断裂（S1 核心验收项）----
    # 第二座土台放在中心连线上：两盘不相交，但裙边彻底融成一体。
    # 间距写成 2 × (Radius + 0.25 × Falloff) 而不是写死 800：它让接合点恰好落在每座
    # 剖面的 T = 0.25 处（S = 0.84375）—— 拉大塑形物时接合处的**相对**高度因此不变，
    # 下面那条断言也就不必跟着改数字。
    separation = 2.0 * (radius + 0.25 * falloff)
    junction_x = c.x + 0.5 * separation
    second = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).spawn_actor_from_class(
        unreal.load_class(None, "/PCGPlugins/HouseTest/BP_GroundShaper.BP_GroundShaper_C"),
        unreal.Vector(c.x + separation, c.y, 0.0))
    check("spawned the adjoining mound", second is not None)
    if second:
        # CDO 的默认值不传播到同会话 spawn 的实例 —— 实例上必须再写一份（实测陷阱，同房子那条）。
        # 参数一律从第一座拿 —— 写死 300/400/300 的话，拉大第一座之后两座尺寸不一致，
        # 接合处的形状就不再是对称的腰，而断言还继续绿着。
        for name, value in (("Ground", ground), ("Radius", radius), ("FalloffDistance", falloff),
                            ("LiftHeight", lift)):
            second.set_editor_property(name, value)
        second.rebuild_terrain()

        # 真的"相接"：接合线上既不是 0（各自独立）也不是台高（完全重合）。
        # 拿**当前台顶**当尺子而不是写死的 200..290 cm：两座同尺寸、间距按上面那个
        # 表达式取时，接合点恒在 S = 0.84375，即台顶的 ≈ 0.86 倍。
        top_h = ground.sample_height(unreal.Vector2D(c.x, c.y))
        junction_h = ground.sample_height(unreal.Vector2D(junction_x, c.y))
        check("the two mounds really merge at the junction",
              0.66 * top_h < junction_h < 0.97 * top_h,
              "h=%.1f cm top=%.1f cm ratio=%.3f" % (junction_h, top_h, junction_h / max(top_h, 1e-6)))

        # 路**沿接合线**画，与两座中心的连线垂直 —— 这条路上的每一条等值线都是合成体的"腰"，
        # 不是关于任何一座中心的圆。旧路在这里必然断段。
        ground.reset_paint()
        paint_line(ground, junction_x, c.y - falloff, junction_x, c.y + falloff, 12)
        count, origins = read_stairs(ground)
        check("a road along the junction grows stairs", count > 0, "stairs=%d" % count)

        # 容差取扫描格边长（石阶每格每层出一级，横向精度就是一格），不写死 100。
        cell = ground.get_editor_property("StairCellSize")
        on_junction = sum(1 for p in origins if abs(p.x - junction_x) <= cell)
        below = sum(1 for p in origins if p.y < c.y)
        above = sum(1 for p in origins if p.y >= c.y)
        check("stairs land on the junction line itself", on_junction > 0, "onJunction=%d" % on_junction)
        check("the waist contour crosses the road on both sides of the mound axis",
              below > 0 and above > 0, "below=%d above=%d" % (below, above))

        # 层连续 = 弧段没断。层号相对最低层算，断言因此不依赖 StairRise/StairZOffset 的推导。
        if origins:
            min_z = min(p.z for p in origins)
            step_h = ground.get_editor_property("StairStepHeight")
            levels = set(int(round((p.z - min_z) / step_h)) for p in origins)
            top = max(levels)
            check("the climb produces at least eight distinct levels", top >= 7, "levels=%d" % (top + 1))
            check("no level is missing: the contour never breaks across the junction",
                  all(l in levels for l in range(top + 1)),
                  "have=%d want=%d" % (len(levels), top + 1))

        unreal.get_editor_subsystem(unreal.EditorActorSubsystem).destroy_actor(second)

    # ---- S2：同一份世界状态重扫，抖动必须逐位重现 ----
    # 随机源是**格身份**（格坐标 + 层号 + 段号 + 种子），不是 InterlockedAdd 拿到的槽位 ——
    # 槽位由线程组完成顺序决定，用它当种子的症状是"画一笔路，整片石阶乱跳"。Z 坐标里带着
    # StairRise × Z 抖动系数，所以比原点全等就足以看出抖动有没有重掷。
    # （逐格精确的证明在单测 GroundStairs.JitterIsCellDeterministic，这里只守回归。）
    ground.reset_paint()
    paint_line(ground, c.x, c.y - reach - 200.0, c.x, c.y + reach + 200.0, 24)
    n_a, o_a = read_stairs(ground)
    ground.rebuild_stairs()
    n_b, o_b = read_stairs(ground)
    same = (n_a == n_b and n_a > 0
            and sorted((round(p.x, 2), round(p.y, 2), round(p.z, 3)) for p in o_a)
                == sorted((round(p.x, 2), round(p.y, 2), round(p.z, 3)) for p in o_b))
    check("rescanning reproduces the per-instance jitter bit for bit", same,
          "n=%d/%d" % (n_a, n_b))
    # 石子走的是同一套格身份哈希（只换盐），所以同一条纪律、同一条断言形态。
    q_a = read_pebbles(ground)[1]
    ground.rebuild_stairs()
    q_b = read_pebbles(ground)[1]
    check("rescanning reproduces the pebbles bit for bit",
          len(q_a) > 0 and sorted((round(p.x, 2), round(p.y, 2), round(p.z, 3)) for p in q_a)
                           == sorted((round(p.x, 2), round(p.y, 2), round(p.z, 3)) for p in q_b),
          "n=%d/%d" % (len(q_a), len(q_b)))

    # ---- 零回读纪律的验收门：**石阶开着**的时候画一笔，不许有任何阻塞刷新 ----
    # 上面 L_HouseGroundDemo 那条同名断言跑的时候石阶是关的，走不到这条路径。
    # 固定容量的全部意义就在这里：EnsureBuffers 稳态零 enqueue、Scan 录完 pass 就返回、
    # 容量与包围盒都不随落笔变，所以不触发阻塞的 SetInstanceSourceGPU。
    #
    # 历史注记：这条断言从前必须先把塑形物自持的旧路石阶关掉才测得准（不关报 6 次刷新 ——
    # 旧路容量按"本次排了多少条记录"算，画路时逐笔扩容 + 逐笔重走阻塞的 SetInstanceSourceGPU）。
    # 旧路已随 2026-08-30「裁决一」整条删除，那三行关停因此一并去掉：现在场上只有这一条石阶路，
    # 这个 0 是它自己的成本，不掺别人的。
    ground.reset_paint()
    flush_before = unreal.CSMesh.get_blocking_flush_count()
    ground.begin_paint_stroke()
    for i in range(12):
        y = c.y - reach + i * (2.0 * reach / 12.0)
        ground.apply_paint_stroke(unreal.Vector(c.x, y, ground.sample_height(unreal.Vector2D(c.x, y))))
        ground.flush_paint_to_gpu(False)      # EdMode 每帧做的就是这一下，内部会重扫石阶
    flush_delta = unreal.CSMesh.get_blocking_flush_count() - flush_before
    # S2 的抖动整个活在同一趟 dispatch 里：不加 buffer、不加 pass，容量与包围盒仍然只由**配置**
    # 算出（包围盒的最坏缩放把抖动上界算进去了，一个实例都不读）⇒ bNeedHandover 不会因抖动成立。
    # 这条断言就是那句话的证据，不是肉眼看的。
    check("a 12-dab stroke with GPU stairs and S2 jitter on blocks the game thread zero times",
          flush_delta == 0, "flushes=%d" % flush_delta)
    ground.end_paint_stroke()

    # ---- 擦掉路 ⇒ 石阶全消（与门拱同一条"道路决定开口"的语言）----
    ground.reset_paint()
    ground.rebuild_stairs()
    count, _ = read_stairs(ground)
    check("erasing the road removes every GPU stair", count == 0, "stairs=%d" % count)
    # 石子是石阶的从属支线：没有台阶就不该有石头（counter 每趟重扫都被清零，这是它唯一的复位路径）。
    p_count, _ = read_pebbles(ground)
    check("erasing the road removes every pebble too", p_count == 0, "pebbles=%d" % p_count)

    # ---- 同一件事，问 GPU 上那个**绘制真正消费**的计数器（门框砖那条的石阶版）----
    # 石阶这条路每次重扫都重走一遍 dispatch（counter 在 pass 里被清零），实例源除了销毁组件
    # 之外从不撤走 —— 所以理论上不会有"陈旧计数器又被交接回去"那个窗口。这两条是把那句
    # "理论上"钉成回归：哪天有人给石阶加一条撤实例源的早退，它们会先红。
    ground.rebuild_stairs()
    gpu_stairs = ground.debug_read_stair_count_gpu_sync()
    check("erasing the road zeroes the GPU stair counter too", gpu_stairs == 0, "gpu=%d" % gpu_stairs)
    gpu_pebbles = ground.debug_read_stair_pebble_count_gpu_sync()
    check("erasing the road zeroes the GPU pebble counter too", gpu_pebbles == 0, "gpu=%d" % gpu_pebbles)

    ground.set_editor_property("StairMesh", None)   # 关掉整条路径，别影响后续用例/出图
    ground.set_editor_property("StairPebbleMesh", None)
    ground.rebuild_stairs()




# =============================================================================
# L_TerrainOpsDemo：披挂岩壳（计划 D9 链 B）
#
# 三条核心验收项：
#   ① 陡坡上出现 / 平地上不出现（裁决四：裙边高度一行代码都不写，它是坡度的副产品）
#   ② 画路之后壳**连续下沉**而不是突然消失（裁决五）—— **量下沉量，不量三角数**
#   ③ 它真的会被画出来（组件/网格/流/材质逐环检查）
#
# ⚠️ ③ 是今天那个教训的直接产物：GPU 石阶的 StairMesh / StairMaterial 在两张演示关卡里
# 一直是 NULL，石阶在画面里是一撮黑块，而单测与回归全绿 —— 因为验收全部走 readback，
# 而 **readback 证明的是"buffer 里有数"，对"画的是哪张网格、有没有材质"一个字都没说**。
# =============================================================================
def read_shell(ground):
    """(三角数, 逐顶点世界位置)。回读只用于验收 —— 运行路径一个字节都不回读。"""
    res = ground.debug_read_rock_shell_sync()
    if isinstance(res, tuple):
        return int(res[0]) // 3, list(res[1])
    return int(res) // 3, []


def shell_alive(pos, tri):
    """⚠️ 判据是第 0 个顶点不是 NaN。kernel 关掉一个三角时**只写第 0 个顶点**
    一个 NaN（一个就够让整个三角在裁剪阶段出局，同 TG），另外两个原样留着上一次的值。
    逐顶点数 NaN 会把死三角的后两个顶点当成活的。"""
    p = pos[tri * 3]
    return not (p.x != p.x or p.y != p.y or p.z != p.z)


def demo_rock_shell():
    unreal.log("========== L_TerrainOpsDemo / rock shell (链 B) ==========")
    unreal.EditorLoadingAndSavingUtils.load_map("/PCGPlugins/HouseTest/L_TerrainOpsDemo")

    ground = find("Ground_Demo")
    shaper = find("Shaper_Mound")
    check("rock shell: actors present", ground and shaper)
    if not (ground and shaper):
        return

    ground.set_editor_property("bRockShell", True)
    ground.set_editor_property("StairMesh", None)   # 本节量的是岩壳，少一条 GPU 路少一份噪声
    ground.reset_paint()
    shaper.rebuild_terrain()

    c = shaper.get_actor_location()
    radius = shaper.get_editor_property("Radius")
    falloff = shaper.get_editor_property("FalloffDistance")
    reach = radius + falloff

    # ---- 图案：Docs/TinyGlade/CSRockShellPattern.md「首次导入后必须核对的四项」的机读版 ----
    vals = list(ground.get_rock_shell_pattern_stats())
    if len(vals) == 6:
        _ok, tris, uvs, max_cell, flip, dir_agree = vals
    else:
        tris, uvs, max_cell, flip, dir_agree = vals
    check("rock shell pattern: 49,598 triangles", tris == 49598, "tris=%d" % tris)
    check("rock shell pattern: >= 3 UV channels", uvs >= 3, "uvs=%d" % uvs)
    # UV1.x 一旦被导入路径归一化到 0..1，cell_id 就废了，而且完全静默：
    # 全场退化成一个胞腔，壳看起来只是"没有块感"，没有任何报错。
    check("rock shell pattern: UV1.x is not normalised (cell ids survive)", max_cell > 600.0,
          "maxCellId=%.1f" % max_cell)
    check("rock shell pattern: recomputed dir agrees with the baked channel", dir_agree > 0.9,
          "dot=%.4f flipWinding=%s" % (dir_agree, flip))

    # ---- ③ 它真的会被画出来 ----
    # ⚠️ 一律调 get_*_undrawable_reason()，**别调 is_*_drawable()**：UE Python 把
    # "bool 返回值 + 一个 out 参数"收成单一返回值 —— 可画时拿到空串、**不可画时拿到 `None`**，
    # 原因串整个丢掉，恰好在唯一需要它的时候失效（红灯只能说"画不出来"，说不出为什么）。
    # `str(None)` == "None" 还会让原因串看着像是真给了一句，实际什么都没说。
    why = str(ground.get_rock_shell_undrawable_reason())
    check("the rock shell is actually drawable (component/mesh/streams/material)", why == "", why)
    check("the drape pass really ran", ground.get_rock_shell_displace_count() > 0,
          "passes=%d" % ground.get_rock_shell_displace_count())

    # ---- GPU 侧真值：岩壳走网格路不是实例路，所以它的计数器是间接绘制参数里的索引数 ----
    # ⚠️ 这个数**不会**跟着壳的死活变（壳是靠往顶点写 NaN 让三角自己塌掉的），它答的是
    # "这次 DrawIndexedIndirect 到底会不会读到东西"。壳还剩几个三角活着由下面的 NaN 判据管。
    gpu_indices = ground.debug_read_rock_shell_draw_index_count_gpu_sync()
    check("the rock shell's indirect draw really consumes the pattern's indices",
          gpu_indices == tris * 3, "gpuIndices=%d want=%d" % (gpu_indices, tris * 3))

    # ---- ① 陡坡上出现、平地上不出现 ----
    tri_count, pos = read_shell(ground)
    check("read the rock shell back", tri_count == tris and len(pos) == tris * 3,
          "tris=%d verts=%d" % (tri_count, len(pos)))
    if tri_count != tris:
        return

    on_plateau = on_skirt = on_flat = live = 0
    for t in range(tri_count):
        if not shell_alive(pos, t):
            continue
        live += 1
        p = pos[t * 3]
        r = math.hypot(p.x - c.x, p.y - c.y)
        if r < radius * 0.6:
            on_plateau += 1
        elif r > reach + 400.0:
            on_flat += 1
        elif radius < r < reach:
            on_skirt += 1
    unreal.log("ROCKSHELL live=%d plateau=%d skirt=%d flat=%d" % (live, on_plateau, on_skirt, on_flat))
    check("the shell grows on the steep skirt", on_skirt > 50, "skirt=%d" % on_skirt)
    check("no shell on the flat plateau", on_plateau == 0, "plateau=%d" % on_plateau)
    check("no shell on the flat ground beyond the falloff", on_flat == 0, "flat=%d" % on_flat)

    # ---- ② 画路 ⇒ **连续下沉**，不是消失 ----
    before = pos
    live_before = live
    paint_line(ground, c.x, c.y - reach - 200.0, c.x, c.y + reach + 200.0, 24)
    _, after = read_shell(ground)

    live_after = 0
    sunk = []
    off_road_drift = 0.0
    # ⚠️ **"路外"不能用顶点当前 XY 上的路权重来判**。下沉是沿**地形法线**的，法线在
    # 陡坡上是斜的 ⇒ 沉下去的顶点 XY 也跑了（最多 RoadSink × |N.xy| ≈ 128 cm），
    # 跑完之后它可能已经落在路外，于是被当成"路外却动了 80 cm"。
    # 改成按**路的几何**判：路是 x = c.x 的一条直线，离它远于笔刷半径 + 位移上限才算路外。
    brush_r = ground.get_editor_property("BrushRadius")
    off_road_x = brush_r + ground.get_editor_property("RockShellRoadSink") + 100.0
    for t in range(tri_count):
        alive_after = shell_alive(after, t)
        if alive_after:
            live_after += 1
        if not alive_after or not shell_alive(before, t):
            continue
        for k in range(3):
            i = t * 3 + k
            delta = after[i].z - before[i].z
            road = ground.sample_road_weight(unreal.Vector2D(before[i].x, before[i].y))
            if road > 0.5:
                sunk.append(delta)
            elif abs(before[i].x - c.x) > off_road_x:
                off_road_drift = max(off_road_drift, abs(delta))
    mean_sink = sum(sunk) / len(sunk) if sunk else 0.0
    deepest = min(sunk) if sunk else 0.0
    unreal.log("ROCKSHELL road: samples=%d meanSink=%.1f deepest=%.1f offRoadDrift=%.3f"
               % (len(sunk), mean_sink, deepest, off_road_drift))

    # 这一条就是裁决五：用 NaN 关掉的话活三角会掉一大块，而画路不改变高度场、
    # 坡度 mask 一点没变 ⇒ 活三角必须**一个不少**。
    check("painting a road hides no shell triangle at all (it sinks, it does not vanish)",
          live_after == live_before, "before=%d after=%d" % (live_before, live_after))
    check("there is shell under the road to measure", len(sunk) > 20, "samples=%d" % len(sunk))
    if sunk:
        sink_limit = (ground.get_editor_property("RockShellRoadSink")
                      + ground.get_editor_property("RockShellCellRelief")
                      + ground.get_editor_property("RockShellNoiseAmount") + 5.0)
        check("the shell under the road really sank", mean_sink < -40.0, "mean=%.1f cm" % mean_sink)
        check("the deepest point sank most of a RoadSink",
              deepest < -0.5 * ground.get_editor_property("RockShellRoadSink"),
              "deepest=%.1f cm" % deepest)
        check("the sink never exceeds RoadSink plus the shell thickness",
              -deepest <= sink_limit, "deepest=%.1f limit=%.1f" % (-deepest, sink_limit))
    check("the shell away from the road does not move at all", off_road_drift < 0.01,
          "drift=%.4f cm" % off_road_drift)

    # ---- 零阻塞纪律扩到新的那一趟 pass ----
    # 披挂 pass 是逐 dab 跑的（道路权重变了就得重披挂），所以它必须与石阶同一个形状：
    # 常驻定长、ENQUEUE 录完就返回、不碰任何 *Sync 入口。
    # 历史注记同 demo_gpu_stairs：这条从前也要先关掉塑形物自持的旧路石阶（不关报 6 次刷新）。
    # 旧路已随裁决一删除，关停的三行随之去掉。
    ground.reset_paint()
    flush_before = unreal.CSMesh.get_blocking_flush_count()
    ground.begin_paint_stroke()
    for i in range(12):
        y = c.y - reach + i * (2.0 * reach / 12.0)
        ground.apply_paint_stroke(unreal.Vector(c.x, y, ground.sample_height(unreal.Vector2D(c.x, y))))
        ground.flush_paint_to_gpu(False)
    flush_delta = unreal.CSMesh.get_blocking_flush_count() - flush_before
    check("a 12-dab stroke with the rock shell on blocks the game thread zero times",
          flush_delta == 0, "flushes=%d" % flush_delta)
    ground.end_paint_stroke()

    # ---- 删掉塑形物 ⇒ 壳自己消失（裁决二：没有一行注销代码）----
    ground.reset_paint()
    saved = (c.x, c.y, c.z, radius, falloff, shaper.get_editor_property("LiftHeight"))
    shaper.set_editor_property("LiftHeight", 0.0)
    shaper.rebuild_terrain()
    _, flat_pos = read_shell(ground)
    live_flat = sum(1 for t in range(tri_count) if shell_alive(flat_pos, t))
    check("flattening the terrain removes every shell triangle (no de-registration code)",
          live_flat == 0, "live=%d" % live_flat)
    shaper.set_editor_property("LiftHeight", saved[5])
    shaper.rebuild_terrain()



SKIRT_MESHES = ["barrel", "firewood", "basket"]
SKIRT_MATERIAL = "/PCGPlugins/HouseTest/M_TinyGladeDecor.M_TinyGladeDecor"
CLUTTER = "/Game/TinyGlade/Meshes/clutter/%s/StaticMeshes/%s.%s"


def demo_skirt_decor():
    """塑形物裙边摆件（D12 锚点层的**第五家**）。

    **归地面，不归塑形物** —— 依据在 `CSGroundDecor.h` 的文件头（一句话：塑形物只提供高度场，
    地面负责派生几何），与石阶、披挂岩壳同一条归属理由，所以这一节也放在
    `L_TerrainOpsDemo` 上、跟在那两条后面。

    ⚠️ **第三条（`get_skirt_decor_undrawable_reason`）才是这一节的重点**，前两条只是它的前提。
    本项目在"readback 全绿而画面上什么都没有"这件事上栽过两次（石阶的 `StairMesh` 恒 NULL、
    母材质没勾 `bUsedWithInstancedStaticMeshes` 被静默换成默认材质），判据必须落在渲染那一侧。
    像素那一侧还有 `Scripts/TinyGladeShotSkirt.py`（含一次故意破坏世界侧的对照），两者缺一不可。
    """
    unreal.log("========== L_TerrainOpsDemo :: skirt decor ==========")
    unreal.EditorLoadingAndSavingUtils.load_map("/PCGPlugins/HouseTest/L_TerrainOpsDemo")

    ground = find("Ground_Demo")
    shaper = find("Shaper_Mound")
    material = unreal.load_asset(SKIRT_MATERIAL)
    meshes = [unreal.load_asset(CLUTTER % (n, n, n)) for n in SKIRT_MESHES]
    check("skirt: actors present", ground and shaper)
    check("skirt: the clutter meshes are present", all(m is not None for m in meshes),
          "meshes=%s" % [n for n, m in zip(SKIRT_MESHES, meshes) if m is None])
    # 材质与房子那四家**共用同一张** `M_TinyGladeDecor`：clutter 的颜色全烘在顶点流里，
    # 一张母材质就够，两张只会在下一次调色时分叉。它已经勾了 used_with_instanced_static_meshes
    # （没勾的话下面那条 drawable 断言会直接把原因串打出来）。
    check("skirt: the decor master material is present", material is not None, SKIRT_MATERIAL)
    if not (ground and shaper and material and all(meshes)):
        return

    # 参数**显式写死**，不靠 CDO 默认值：靠默认值的话哪天默认改了、断言还绿着，
    # 覆盖面却悄悄变了（与 demo_gpu_stairs 那几行同一条理由）。
    ground.set_editor_property("bSkirtDecorEnabled", True)
    ground.set_editor_property("SkirtDecorMeshes", meshes)
    ground.set_editor_property("SkirtDecorMaterial", material)
    ground.set_editor_property("SkirtDecorSpacing", 260.0)
    ground.set_editor_property("SkirtDecorBandT", 0.62)
    ground.set_editor_property("SkirtDecorMinSpacing", 140.0)
    ground.set_editor_property("SkirtDecorRoadReject", 0.3)
    ground.set_editor_property("SkirtDecorScale", 0.6)
    ground.set_editor_property("SkirtDecorScaleJitter", 0.18)
    ground.set_editor_property("SkirtDecorSeed", 11)

    ground.reset_paint()
    shaper.rebuild_terrain()
    ground.rebuild_skirt_decor()

    c = shaper.get_actor_location()
    radius = shaper.get_editor_property("Radius")
    falloff = shaper.get_editor_property("FalloffDistance")
    band_r = radius + falloff * 0.62

    anchors = ground.get_skirt_decor_anchor_count()
    instances = ground.get_skirt_decor_instance_count()
    check("a mound grows skirt anchors on its own falloff band", anchors > 0, "anchors=%d" % anchors)
    check("some of those anchors actually carry a prop", instances > 0, "instances=%d" % instances)
    # **一个锚点最多一件** —— 这就是"密度由锚点数量决定"的形式化（不是场里的阈值）。
    check("no skirt anchor carries more than one prop", instances <= anchors,
          "instances=%d anchors=%d" % (instances, anchors))
    # 锚点数 = 环周长 / 弧长间距。落在这个区间之外说明生产公式变了而容量预留没跟着变 ——
    # 那就是一次静默截断（少摆几件，没有任何计数会报红）。
    want = int(round(2.0 * math.pi * band_r / 260.0))
    check("the skirt anchor count is the ring circumference over the spacing",
          abs(anchors - want) <= 1, "anchors=%d want=%d band_r=%.0f" % (anchors, want, band_r))

    # ---- 它真的会被画出来（渲染侧逐环 + 材质支持实例化）----
    # ⚠️ 调原因版，**不要** `is_skirt_decor_drawable()`：UE Python 把"bool 返回值 + 一个 out
    # 参数"收成单一返回值 —— 可画时拿到空串、不可画时拿到 `None`，原因串整个丢掉。
    why = str(ground.get_skirt_decor_undrawable_reason())
    check("the skirt decor is actually drawable (components/base mesh/instance source/ISM-capable material)",
          why == "", why)

    # ---- GPU 侧真值：组件手上那些被 indirect draw 消费的计数器之和 ----
    gpu_instances = ground.debug_read_skirt_decor_instance_count_gpu_sync()
    check("the GPU skirt counter agrees with the CPU record", gpu_instances == instances,
          "gpu=%d cpu=%d" % (gpu_instances, instances))

    # ---- 幂等：同一世界状态再摆一次，一件都不许变 ----
    ground.rebuild_skirt_decor()
    check("skirt decor rebuild is idempotent",
          ground.get_skirt_decor_instance_count() == instances
          and ground.get_skirt_decor_anchor_count() == anchors,
          "anchors=%d instances=%d" % (ground.get_skirt_decor_anchor_count(),
                                       ground.get_skirt_decor_instance_count()))

    # ---- 路穿裙边 ⇒ 那一段让开（与石阶严格互补：路上长的是台阶）----
    paint_line(ground, c.x, c.y - radius - falloff - 200.0, c.x, c.y + radius + falloff + 200.0, 24)
    roaded = ground.get_skirt_decor_anchor_count()
    stairs, _ = read_stairs(ground)
    check("the road really grew stairs here (otherwise the next check is vacuous)",
          stairs > 0, "stairs=%d" % stairs)
    check("the road takes skirt anchors off the band it crosses", roaded < anchors,
          "roaded=%d clean=%d" % (roaded, anchors))
    check("but it does not clear the whole ring", roaded > 0, "roaded=%d" % roaded)

    # ---- 擦掉道路 ⇒ 摆件**逐位**复位（不是"差不多"）----
    ground.reset_paint()
    check("erasing the road restores exactly the skirt decor it removed",
          ground.get_skirt_decor_anchor_count() == anchors
          and ground.get_skirt_decor_instance_count() == instances,
          "anchors=%d/%d instances=%d/%d" % (ground.get_skirt_decor_anchor_count(), anchors,
                                             ground.get_skirt_decor_instance_count(), instances))

    # ---- 台子塌平 ⇒ 裙边没了 ⇒ 摆件跟着消失（没有一行注销代码，同岩壳裁决二的形状）----
    lift = shaper.get_editor_property("LiftHeight")
    shaper.set_editor_property("LiftHeight", 0.0)
    shaper.rebuild_terrain()
    check("flattening the mound removes every skirt prop (no de-registration code)",
          ground.get_skirt_decor_anchor_count() == 0 and ground.get_skirt_decor_instance_count() == 0,
          "anchors=%d instances=%d" % (ground.get_skirt_decor_anchor_count(),
                                       ground.get_skirt_decor_instance_count()))
    shaper.set_editor_property("LiftHeight", lift)
    shaper.rebuild_terrain()
    check("raising it again brings them back", ground.get_skirt_decor_instance_count() == instances,
          "instances=%d/%d" % (ground.get_skirt_decor_instance_count(), instances))

    # ---- 零阻塞：12 dab 一笔，裙边摆件开着（第十二条 flushes=0）----
    #
    # 这条钉的是"容量上限走 CSShaperSteps::ReserveCount"那一步：上限是**半径的连续函数**
    # （环周长 / 间距），直接喂 ReserveCapacity（只对齐 64）就会像藤蔓那轮一样每涨过一个间距
    # 重新分配一次 —— 实测一段拖动 21 次阻塞刷新。
    ground.begin_paint_stroke()
    flush_before = unreal.CSMesh.get_blocking_flush_count()
    reach = radius + falloff
    for i in range(12):
        y = c.y - reach + i * (2.0 * reach / 12.0)
        ground.apply_paint_stroke(unreal.Vector(c.x, y, ground.sample_height(unreal.Vector2D(c.x, y))))
        ground.flush_paint_to_gpu(False)
    flush_delta = unreal.CSMesh.get_blocking_flush_count() - flush_before
    check("a 12-dab stroke with the skirt decor on blocks the game thread zero times",
          flush_delta == 0, "flushes=%d" % flush_delta)
    ground.end_paint_stroke()

    # ---- 拖塑形物：**开着裙边摆件不比关着多付一次阻塞刷新** ----
    #
    # 拖动是这一家最糟的那条路：环心每一帧都在动，上界是半径的连续函数，容量与包围盒都可能
    # 每帧重来（藤蔓那轮就是这么栽的，实测一段拖动 21 次）。
    #
    # ⚠️ 这一条**只能写成差值**，不能写成"绝对 0"：拖塑形物这条路上本来就有
    # **每帧一次**的阻塞刷新，与摆件无关 —— `ACSGroundActor::RefreshHeightsInRegion` 调的
    # `UCSMeshOps::DisplaceGroundShapers` 用的是 `EditMeshSync`（`CSMeshOps.cpp:1551`）。
    # 实测（`skirt OFF/ON × shell OFF/ON` 四组）四组**都是 12 次**，即摆件贡献恰好 0。
    # 现有的十一条 `flushes=0` 谁都没测过拖塑形物这条路，所以这是本轮顺带测出来的既有项，
    # **只报告、未处理**（消它要把地面位移改成异步编辑，超出本轮范围）。
    ground.reset_paint()

    def drag_flushes():
        before = unreal.CSMesh.get_blocking_flush_count()
        for i in range(1, 13):
            shaper.set_actor_location(unreal.Vector(c.x + i * 5.0, c.y, c.z), False, False)
            shaper.rebuild_terrain()
        delta = unreal.CSMesh.get_blocking_flush_count() - before
        shaper.set_actor_location(unreal.Vector(c.x, c.y, c.z), False, False)
        shaper.rebuild_terrain()
        return delta

    ground.set_editor_property("bSkirtDecorEnabled", False)
    ground.rebuild_skirt_decor()
    base_flushes = drag_flushes()
    ground.set_editor_property("bSkirtDecorEnabled", True)
    ground.rebuild_skirt_decor()
    on_flushes = drag_flushes()
    check("turning the skirt decor on adds zero blocking flushes to a 12-frame shaper drag",
          on_flushes == base_flushes, "on=%d off=%d" % (on_flushes, base_flushes))
    check("the dragged mound still carries the same number of skirt props",
          ground.get_skirt_decor_instance_count() == instances,
          "instances=%d/%d" % (ground.get_skirt_decor_instance_count(), instances))


def demo_house_vine():
    """墙面藤蔓（D13）。

    ⚠️ **第三条（`is_vine_drawable`）才是这一节的重点**，前两条只是它的前提。
    D13 比石阶更容易中"画面上什么都没有而断言全绿"那一枪：现成的 `MI_ivy_*` 全都挂在
    `M_TG_Texture` 下，而它**没有勾 `bUsedWithInstancedStaticMeshes`** ⇒ 引擎会把材质
    **静默换成默认材质**，症状与"没绑材质"逐像素相同。段数、叶数、零阻塞这三条在那种情况下
    照样全绿 —— 所以判据必须落在渲染那一侧。
    """
    unreal.log("========== L_HouseGroundDemo :: vine ==========")
    unreal.EditorLoadingAndSavingUtils.load_map("/PCGPlugins/HouseTest/L_HouseGroundDemo")
    ground = find("Ground_Demo")
    house = find("House_Road")
    check("vine: actors present", ground and house)
    if not (ground and house):
        return

    ground.reset_paint()
    house.rebuild_house()

    branches = house.get_vine_segment_count()
    leaves = house.get_vine_leaf_count()
    flowers = house.get_vine_flower_count()
    check("a bare house grows vines on its walls", branches > 0, "branches=%d" % branches)
    check("the vines carry leaves", leaves > 0, "leaves=%d" % leaves)
    # 花是第二档新加的第三个调色板。演示房子配了 `ivy_flower` ⇒ 这里必须 > 0；
    # ⚠️ 但网格留空时 0 是**合法**的，所以门挂在"这栋房子配了花网格"这个前提上。
    if house.get_editor_property("VineFlowerMesh"):
        check("the vines bloom (ivy_flower, the third palette)", flowers > 0, "flowers=%d" % flowers)
    # 上限是纯配置量（周长 / 间距 × 每根最多几段），容量按它一次预留、之后永不扩容。
    # 数量掉出这个区间说明规划公式变了，而预留量没跟着变 —— 那就是一次静默截断。
    check("the vine count is in the right ballpark", 120 <= branches <= 900, "branches=%d" % branches)

    # ---- 它真的会被画出来（渲染侧逐环 + 材质支持实例化）----
    # 调原因版，不调 is_vine_drawable()：理由逐字见 demo_rock_shell 里那段。
    why = str(house.get_vine_undrawable_reason())
    check("the vine is actually drawable (components/base mesh/instance source/ISM-capable material)",
          why == "", why)

    # ---- GPU 侧真值：枝/叶两个组件手上那个被 indirect draw 消费的计数器 ----
    # 上面那条 `get_vine_undrawable_reason()` 只答"能不能画"，答不了"实际会画多少"。
    gpu_branches = house.debug_read_vine_branch_count_gpu_sync()
    gpu_leaves = house.debug_read_vine_leaf_count_gpu_sync()
    gpu_flowers = house.debug_read_vine_flower_count_gpu_sync()
    check("the GPU branch counter agrees with the CPU record", gpu_branches == branches,
          "gpu=%d cpu=%d" % (gpu_branches, branches))
    check("the GPU leaf counter agrees with the CPU record", gpu_leaves == leaves,
          "gpu=%d cpu=%d" % (gpu_leaves, leaves))
    check("the GPU flower counter agrees with the CPU record", gpu_flowers == flowers,
          "gpu=%d cpu=%d" % (gpu_flowers, flowers))
    mismatch = house.debug_get_gpu_asset_mismatch_sync()
    check("the GPU draws the meshes and materials this house thinks it does (vine pass)",
          mismatch == "", mismatch)

    # ---- 关掉再打开：这是藤这条路上"陈旧计数器"的入口 ----
    #
    # 关掉时 `RebuildVine` 提前返回并撤走实例源，交接缓存跟着被清空 ⇒ 重新打开时
    # `EnsureVineComponents` 把**同一批 buffer** 交回组件。门框砖那个既有 bug 就是这个形状。
    # ⚠️ 这一关里 `CSHouseVine::Pack` 紧接着就会跑一遍（它自己的空表分支会清零），所以这条
    # 断言**只守得住"重新交接之后计数器是对的"**，守不住"Pack 走不到"那个更窄的窗口
    # （`bVineBaseMeshReady` 为假时才发生，无头脚本造不出来）。那个窗口现在由
    # `RebuildVine` 里显式的 `CSShaperSteps::ZeroCounters` 挡着，但**没有断言覆盖它** ——
    # 如实记在这里，别以为它被测到了。
    house.set_editor_property("bVineEnabled", False)
    house.rebuild_house()
    gpu_off = house.debug_read_vine_branch_count_gpu_sync()   # 回读是阻塞的，每条断言只读一次
    check("disabling the vine drops it from the GPU too", gpu_off == 0, "gpu=%d" % gpu_off)
    house.set_editor_property("bVineEnabled", True)
    house.rebuild_house()
    gpu_back = house.debug_read_vine_branch_count_gpu_sync()
    check("re-enabling the vine hands back a correct counter, not the previous generation's",
          gpu_back == branches, "gpu=%d want=%d" % (gpu_back, branches))

    # ---- 幂等：同一世界状态再复评一次，藤一段都不许变 ----
    house.reevaluate_site()
    check("vine rebuild is idempotent", house.get_vine_segment_count() == branches,
          "branches=%d" % house.get_vine_segment_count())

    # ---- 山墙三角：藤爬过檐口（第二档）----
    # ⚠️ 判据是**关掉之后段数必须变少**，不是"开着的时候段数大于某个常数"：后者在
    # 山墙那两面一段都没多长时照样绿（它只要总数够大就行），而这一条只有山墙真的多长了藤
    # 才成立。关掉 / 打开各测一次，末了恢复默认，别把状态留给下一节。
    house.set_editor_property("bVineClimbGable", False)
    house.rebuild_house()
    no_gable = house.get_vine_segment_count()
    house.set_editor_property("bVineClimbGable", True)
    house.rebuild_house()
    check("vines climb the gable triangle above the eave", branches > no_gable,
          "climb=%d no-climb=%d" % (branches, no_gable))
    check("turning gable climbing back on restores exactly the same vines",
          house.get_vine_segment_count() == branches,
          "branches=%d want=%d" % (house.get_vine_segment_count(), branches))

    # ---- 开门 ⇒ 藤避让墙洞（TG 的 `ivy_grower` 读集里有 `PrevWallHoles`）----
    loc = house.get_actor_location()
    paint_line(ground, loc.x, loc.y - 800.0, loc.x, loc.y + 800.0, 17)
    house.rebuild_house()
    doors = house.get_open_door_count()
    holed = house.get_vine_segment_count()
    check("the vine test really opened arches (otherwise the next check is vacuous)",
          doors > 0, "doors=%d" % doors)
    check("opening arches makes the vine give way to the wall holes", holed < branches,
          "branches %d -> %d" % (branches, holed))
    check("the holes only prune the vine near them, they do not wipe the walls",
          holed > branches // 3, "branches=%d" % holed)

    # ---- 擦掉道路 ⇒ 洞合拢 ⇒ 藤逐段复位（**逐位**，不是"差不多"）----
    ground.reset_paint()
    house.rebuild_house()
    check("erasing the road restores exactly the vines the holes had pruned",
          house.get_vine_segment_count() == branches,
          "branches=%d want=%d" % (house.get_vine_segment_count(), branches))

    # ---- 零阻塞：拖尺寸 12 帧，藤开着 ----
    #
    # ⚠️ 起始尺寸必须**显式钉死**再 rebuild_house()，与上面那条同族断言逐字同形：
    # 一次性的容量与包围盒成本付在 rebuild 里，不许落进测量窗口。
    #
    # 🐛 这条断言第一次跑就抓到一个真 bug（已修，留档）：藤的容量上限是
    # `周长 / VineStrandSpacing × VineMaxSegments`，是 FootprintSize 的**连续函数**，
    # 而 `CSShaperSteps::ReserveCapacity` 只对齐到 64 ⇒ 拖动中每涨过一根藤的间距就重新分配一次，
    # 实测每次 **5 次阻塞刷新**、一段拖动累计 **21 次**。修法是再走一遍 `CSShaperSteps::ReserveCount`
    # 的台阶（×1.5 对齐 4096）—— 与门框砖包围盒那条是同一个失败模式、同一个解法。
    house.set_editor_property("FootprintSize", unreal.Vector2D(600.0, 400.0))
    house.rebuild_house()
    base_branches = house.get_vine_segment_count()
    check("the vine resize test starts with vines", base_branches > 0, "branches=%d" % base_branches)
    before = unreal.CSMesh.get_blocking_flush_count()
    for i in range(1, 13):
        house.set_editor_property("FootprintSize", unreal.Vector2D(600.0 + i * 5.0, 400.0))
        house.reevaluate_site()
    flush_delta = unreal.CSMesh.get_blocking_flush_count() - before
    check("dragging FootprintSize for 12 frames with vines on blocks the game thread zero times",
          flush_delta == 0, "flushes=%d" % flush_delta)
    check("the dragged house still has vines", house.get_vine_segment_count() > 0,
          "branches=%d" % house.get_vine_segment_count())
    house.set_editor_property("FootprintSize", unreal.Vector2D(600.0, 400.0))
    house.rebuild_house()


def demo_house_decor():
    """装饰摆件（D12 的**锚点那一半**）。

    ⚠️ **第三条（`is_decor_drawable`）才是这一节的重点**，前两条只是它的前提。
    本项目在"readback 全绿而画面上什么都没有"这件事上栽过两次（石阶的 `StairMesh` 恒 NULL、
    母材质没勾 `bUsedWithInstancedStaticMeshes` 被静默换成默认材质），所以判据必须落在
    渲染那一侧。像素那一侧还有 `Scripts/TinyGladeShotDecor.py`，两者缺一不可。

    ⚠️ 这里**不测复杂度场**：计划 D12 的 `RT_DecorField` + tile-argmax 在 TG 里没有对位物，
    取舍是挂起的决策 C2，本轮只实现锚点这一半。
    """
    unreal.log("========== L_HouseGroundDemo :: decor ==========")
    unreal.EditorLoadingAndSavingUtils.load_map("/PCGPlugins/HouseTest/L_HouseGroundDemo")
    ground = find("Ground_Demo")
    house = find("House_Road")
    check("decor: actors present", ground and house)
    if not (ground and house):
        return

    ground.reset_paint()
    house.rebuild_house()

    anchors = house.get_decor_anchor_count()
    instances = house.get_decor_instance_count()
    check("a bare house grows decor anchors on its own features", anchors > 0, "anchors=%d" % anchors)
    check("some of those anchors actually carry a prop", instances > 0, "instances=%d" % instances)
    # **一个锚点最多一件** —— 这就是"密度由锚点数量决定"的形式化（不是场里的阈值）。
    check("no anchor carries more than one prop", instances <= anchors,
          "instances=%d anchors=%d" % (instances, anchors))
    # 上限是纯配置量（周长 / 间距 + 檐脊两条线），容量按它一次预留、之后永不扩容。
    # 数量掉出这个区间说明生产公式变了而预留量没跟着变 —— 那就是一次静默截断。
    check("the decor count is in the right ballpark", 8 <= anchors <= 120, "anchors=%d" % anchors)

    # ---- 它真的会被画出来（渲染侧逐环 + 材质支持实例化）----
    # 调原因版，不调 is_decor_drawable()：理由逐字见 demo_rock_shell 里那段。
    why = str(house.get_decor_undrawable_reason())
    check("the decor is actually drawable (components/base mesh/instance source/ISM-capable material)",
          why == "", why)

    # ---- GPU 侧真值：所有 palette 组件手上那些被 indirect draw 消费的计数器之和 ----
    gpu_instances = house.debug_read_decor_instance_count_gpu_sync()
    check("the GPU decor counter agrees with the CPU record", gpu_instances == instances,
          "gpu=%d cpu=%d" % (gpu_instances, instances))
    mismatch = house.debug_get_gpu_asset_mismatch_sync()
    check("the GPU draws the meshes and materials this house thinks it does (decor pass)",
          mismatch == "", mismatch)

    # ---- 幂等：同一世界状态再复评一次，摆件一件都不许变 ----
    house.reevaluate_site()
    check("decor rebuild is idempotent", house.get_decor_instance_count() == instances,
          "instances=%d" % house.get_decor_instance_count())

    # ---- 画路开拱 ⇒ **门那一家**长出来（TG 的 add_autoclutter_around_gates）----
    loc = house.get_actor_location()
    paint_line(ground, loc.x, loc.y - 800.0, loc.x, loc.y + 800.0, 17)
    house.rebuild_house()
    doors = house.get_open_door_count()
    gate = house.get_decor_gate_anchor_count()
    check("the decor test really opened arches (otherwise the next check is vacuous)",
          doors > 0, "doors=%d" % doors)
    # ⚠️ 必须看**门那一家**的锚点，不能看总数：开拱同时会把门口净空与路面上的墙脚锚点排掉，
    # 两边数量可以刚好抵消（实测碰到过：总数 20 → 20）。
    check("opening arches grows the gate anchor family", gate > 0, "gate=%d doors=%d" % (gate, doors))
    # 道路排除（TG 的 PathRaster mask）：门前引道那两个锚岔在路两侧，路面上的那些被丢掉。
    check("the road does not get props standing on it (some anchors were rejected)",
          house.get_decor_anchor_count() >= gate, "anchors=%d gate=%d"
          % (house.get_decor_anchor_count(), gate))

    # ---- 擦掉道路 ⇒ 拱合拢 ⇒ 摆件**逐位**复位（不是"差不多"）----
    ground.reset_paint()
    house.rebuild_house()
    check("erasing the road restores exactly the decor the arches had added",
          house.get_decor_anchor_count() == anchors
          and house.get_decor_instance_count() == instances
          and house.get_decor_gate_anchor_count() == 0,
          "anchors=%d/%d instances=%d/%d gate=%d"
          % (house.get_decor_anchor_count(), anchors,
             house.get_decor_instance_count(), instances,
             house.get_decor_gate_anchor_count()))

    # ---- 零阻塞：拖尺寸 12 帧，摆件开着 ----
    #
    # ⚠️ 起始尺寸必须**显式钉死**再 rebuild_house()，与藤蔓那条同族断言逐字同形：
    # 一次性的容量与包围盒成本付在 rebuild 里，不许落进测量窗口。
    #
    # 这条钉的是"容量上限走 CSShaperSteps::ReserveCount"那一步：上限是 FootprintSize 的**连续函数**
    # （周长 / 间距 + 檐脊两条线），直接喂 ReserveCapacity（只对齐 64）就会像藤蔓那轮一样
    # 每涨过一个间距重新分配一次 —— 实测一段拖动 21 次阻塞刷新。
    house.set_editor_property("FootprintSize", unreal.Vector2D(600.0, 400.0))
    house.rebuild_house()
    base_anchors = house.get_decor_anchor_count()
    check("the decor resize test starts with decor", base_anchors > 0, "anchors=%d" % base_anchors)
    before = unreal.CSMesh.get_blocking_flush_count()
    for i in range(1, 13):
        house.set_editor_property("FootprintSize", unreal.Vector2D(600.0 + i * 5.0, 400.0))
        house.reevaluate_site()
    flush_delta = unreal.CSMesh.get_blocking_flush_count() - before
    check("dragging FootprintSize for 12 frames with decor on blocks the game thread zero times",
          flush_delta == 0, "flushes=%d" % flush_delta)
    check("the dragged house still has decor", house.get_decor_instance_count() > 0,
          "instances=%d" % house.get_decor_instance_count())
    house.set_editor_property("FootprintSize", unreal.Vector2D(600.0, 400.0))
    house.rebuild_house()


def make_window(edge, center_s, width, sill_z, height, shape=None):
    """一条窗户诉求。`Windows` 是一份**显式列表** —— 本项目没有"按剩余墙面自动填窗"的规则，
    也不该有（TG 的窗 100% 是玩家手放的，而放置 UI 不在这一轮范围内）。"""
    w = unreal.CSHouseWindow()
    w.set_editor_property("edge_index", edge)
    w.set_editor_property("center_s", float(center_s))
    w.set_editor_property("width", float(width))
    w.set_editor_property("sill_z", float(sill_z))
    w.set_editor_property("height", float(height))
    if shape is not None:
        w.set_editor_property("shape", shape)
    return w


def demo_house_window():
    """窗（D8）。

    ⚠️ **`get_window_undrawable_reason` 那条才是这一节的重点**，数量断言只是它的前提。
    窗比藤蔓/摆件更容易中"数值全绿而画面上什么都没有"那一枪，因为**洞根本不是几何** ——
    它是墙材质用 OpacityMask 逐像素 discard 切出来的（裁决三：避免所有真几何洞）。
    墙材质一旦不是 Masked，洞数、砖数、三角形数、零阻塞四条断言**全部照绿**，画面上却一个洞没有。

    ⚠️ 窗放在**短墙**（边 1 / 边 3）上：演示关卡那条路是南北向的、拱开在两面长墙上，
    把窗也放长墙就会被门整条边挤掉 —— 那是 D6 的预期行为（门拱优先），但会让别的断言变成空判据。
    最后专门有一段测那件事。
    """
    unreal.log("========== L_HouseGroundDemo :: window ==========")
    unreal.EditorLoadingAndSavingUtils.load_map("/PCGPlugins/HouseTest/L_HouseGroundDemo")
    ground = find("Ground_Demo")
    house = find("House_Road")
    check("window: actors present", ground and house)
    if not (ground and house):
        return

    ground.reset_paint()
    house.set_editor_property("Windows", [])
    house.rebuild_house()

    base_openings = house.get_opening_count()
    base_bricks = house.get_frame_brick_count()
    base_tris = settle_tris(house.get_tiny_glade_mesh())
    check("a house with no window requests has no windows", house.get_window_count() == 0,
          "windows=%d" % house.get_window_count())

    # ---- 三扇窗：短墙（边 1 长 400 - 2x24 = 352，护角 60 ⇒ 可用 [60, 292]）----
    windows = [
        make_window(1, 120.0, 78.0, 90.0, 110.0),
        make_window(1, 232.0, 78.0, 90.0, 110.0),
        make_window(3, 176.0, 78.0, 90.0, 110.0),
    ]
    house.set_editor_property("Windows", windows)
    house.rebuild_house()

    made = house.get_window_count()
    check("the explicit list opens three windows", made == 3,
          "windows=%d rejected=%d" % (made, house.get_window_reject_count()))
    check("the windows join the same openings table as the doors",
          house.get_opening_count() == base_openings + 3,
          "openings=%d (was %d)" % (house.get_opening_count(), base_openings))
    # 洞是 clip 切的、几何永远实心 —— 但窗台那块实心盒是**新几何**，三角形数必须涨。
    win_tris = settle_tris(house.get_tiny_glade_mesh())
    check("the sill boxes add real geometry (holes are clipped, sills are solid)", win_tris > base_tris,
          "tris=%d (was %d)" % (win_tris, base_tris))
    # 窗台与窗楣那圈砖：一扇窗一条闭合砖路（左樘 + 平顶 + 右樘 + **窗台底边**），
    # 周长 (110 + 78) x 2 = 376 cm、砖长 26 ⇒ 每扇约 14 块。
    win_bricks = house.get_frame_brick_count()
    check("each window grows its own ring of frame bricks", win_bricks >= base_bricks + 3 * 10,
          "bricks=%d (was %d)" % (win_bricks, base_bricks))

    # ---- 它真的会被画出来（渲染侧逐环 + 墙材质必须是 Masked）----
    # 调原因版，不调 is_window_drawable()：理由逐字见 demo_house_vine 里那段。
    why = str(house.get_window_undrawable_reason())
    check("the window is actually drawable (body/Masked wall material/frame bricks/ISM-capable material)",
          why == "", why)

    # ---- 幂等：同一世界状态再复评一次，窗一扇都不许变 ----
    house.reevaluate_site()
    check("window rebuild is idempotent",
          house.get_window_count() == made and house.get_frame_brick_count() == win_bricks,
          "windows=%d bricks=%d" % (house.get_window_count(), house.get_frame_brick_count()))

    # ---- 窗台下限：贴地的窗被谓词拒（几何合法，观感荒唐）----
    house.set_editor_property("Windows", windows + [make_window(1, 60.0, 60.0, 0.0, 150.0)])
    house.rebuild_house()
    check("a window sitting on the floor is rejected while the other three survive",
          house.get_window_count() == 3 and house.get_window_reject_count() == 1,
          "windows=%d rejected=%d" % (house.get_window_count(), house.get_window_reject_count()))

    # ---- 门拱优先（D6）：南墙上的窗，画路开拱之后必须让位 ----
    #
    # ⚠️ 这条是 P2 那件事的执行面：用户看到的是"窗放上去就消失"，而它其实是预期行为。
    # 没有 get_window_reject_count() 这个回执，"被门吃掉"与"根本没放"在画面上逐像素相同。
    south = windows + [make_window(0, 300.0, 78.0, 90.0, 110.0)]
    house.set_editor_property("Windows", south)
    house.rebuild_house()
    check("with no road the south wall takes a window too", house.get_window_count() == 4,
          "windows=%d" % house.get_window_count())

    loc = house.get_actor_location()
    paint_line(ground, loc.x, loc.y - 800.0, loc.x, loc.y + 800.0, 17)
    house.rebuild_house()
    doors = house.get_open_door_count()
    check("the window test really opened arches (otherwise the next check is vacuous)", doors > 0,
          "doors=%d" % doors)
    check("arches win: the south window is rejected and says so",
          house.get_window_count() == 3 and house.get_window_reject_count() == 1,
          "windows=%d rejected=%d doors=%d" % (house.get_window_count(),
                                               house.get_window_reject_count(), doors))

    # ---- 擦掉道路 ⇒ 拱合拢 ⇒ 那扇窗**逐位**回来 ----
    ground.reset_paint()
    house.rebuild_house()
    check("erasing the road gives the south window back",
          house.get_window_count() == 4 and house.get_window_reject_count() == 0,
          "windows=%d rejected=%d" % (house.get_window_count(), house.get_window_reject_count()))

    # ---- 零阻塞：拖尺寸 12 帧，窗开着 ----
    #
    # 起始尺寸显式钉死再 rebuild_house()，与藤蔓/摆件那两条同族断言逐字同形：
    # 一次性的容量与包围盒成本付在 rebuild 里，不许落进测量窗口。
    # 窗让每个洞的砖路多出第四段（窗台底边），容量却仍是常量（FrameReserveCapacity 一次预留）
    # —— 这条钉的就是"别为了窗把扩容加回来"。
    house.set_editor_property("Windows", windows)
    house.set_editor_property("FootprintSize", unreal.Vector2D(600.0, 400.0))
    house.rebuild_house()
    check("the window resize test starts with windows", house.get_window_count() > 0,
          "windows=%d" % house.get_window_count())
    before = unreal.CSMesh.get_blocking_flush_count()
    for i in range(1, 13):
        house.set_editor_property("FootprintSize", unreal.Vector2D(600.0 + i * 5.0, 400.0))
        house.reevaluate_site()
    flush_delta = unreal.CSMesh.get_blocking_flush_count() - before
    check("dragging FootprintSize for 12 frames with windows on blocks the game thread zero times",
          flush_delta == 0, "flushes=%d" % flush_delta)
    check("the dragged house still has windows", house.get_window_count() > 0,
          "windows=%d" % house.get_window_count())

    # ---- 关掉开关 ⇒ 窗与它们的砖**逐位**消失（对照组，也是出图脚本用的那个开关）----
    house.set_editor_property("FootprintSize", unreal.Vector2D(600.0, 400.0))
    house.rebuild_house()
    on_bricks = house.get_frame_brick_count()
    house.set_editor_property("bWindowsEnabled", False)
    house.rebuild_house()
    check("switching windows off removes exactly the openings and bricks they added",
          house.get_window_count() == 0 and house.get_opening_count() == base_openings
          and house.get_frame_brick_count() == base_bricks,
          "windows=%d openings=%d/%d bricks=%d/%d (on: %d)"
          % (house.get_window_count(), house.get_opening_count(), base_openings,
             house.get_frame_brick_count(), base_bricks, on_bricks))

    house.set_editor_property("bWindowsEnabled", True)
    house.set_editor_property("Windows", [])
    ground.reset_paint()
    house.rebuild_house()


def spawn_house(label, x, y, z, size_x, size_y):
    """从 BP CDO 生成一栋房子。

    ⚠️ **必须从 `BP_TinyGladeHouse` 生成，不能 `spawn_actor_from_class(ACSHouseActor)`**：
    墙材质 / 砖网格 / 砖材质那一整套是 `TinyGladeMakeWallMaterials.py` 与 `TinyGladeSetupFrame.py`
    烘进 **BP CDO** 的（坑表那条"脚本会把值烘进 BP CDO 与关卡实例两处"）。裸 actor 的材质全是
    None ⇒ `get_seam_undrawable_reason()` 当场红，而那是本节最想测的一条。

    ⚠️ 也**不改演示关卡**：这栋房是脚本临时生成、用完销毁的，`.umap` 一个字节都不动。
    坑表里"脚本把空值存进 .umap 而 umap 不在 git 跟踪内"那一枪只能挨一次。
    """
    bp = unreal.EditorAssetLibrary.load_asset("/PCGPlugins/HouseTest/BP_TinyGladeHouse")
    if not bp:
        return None
    sub = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actor = sub.spawn_actor_from_object(bp, unreal.Vector(x, y, z))
    if not actor:
        return None
    actor.set_actor_label(label)
    actor.set_editor_property("FootprintSize", unreal.Vector2D(size_x, size_y))
    actor.rebuild_house()
    return actor


def demo_house_seam():
    """D7 接缝（裁决二）。

    接缝是**纯函数**：输入两房 footprint / 朝向 / 高度，输出砖列。零共享状态、零跨房簿记、
    零撤销 —— 没有接缝 actor，两栋房各自算出**同一条**缝，两边都画。

    ⚠️ **"逐位相同"那条硬证据在单测里**（`House.SeamIsAPureFunction`，memcmp 级别）。这一节
    是它的端到端回声：两栋房报出来的交点数与砖数必须一致，且**只重建其中一栋**不改变任何一边。

    ⚠️ `get_seam_undrawable_reason()` 那条才是本节的重点，数量断言只是它的前提。接缝有**两半**，
    两半各自能静默失效：洞那一半根本不是几何（墙材质不是 Masked ⇒ 两栋房的墙原样互相插着），
    砖那一半走 GPU 实例路（母材质没勾 ISM ⇒ 引擎静默换默认材质）。两种情形下交点数 / 砖数 /
    裁剪段数 / 三角形数**全部照绿**。
    """
    unreal.log("========== L_HouseGroundDemo :: seam ==========")
    unreal.EditorLoadingAndSavingUtils.load_map("/PCGPlugins/HouseTest/L_HouseGroundDemo")
    ground = find("Ground_Demo")
    house = find("House_Road")
    check("seam: actors present", ground and house)
    if not (ground and house):
        return

    ground.reset_paint()
    house.set_editor_property("Windows", [])
    house.set_editor_property("FootprintSize", unreal.Vector2D(600.0, 400.0))
    house.rebuild_house()

    base_corners = house.get_seam_corner_count()
    base_cuts = house.get_seam_cut_count()
    base_bricks = house.get_frame_brick_count()
    base_tris = settle_tris(house.get_tiny_glade_mesh())
    check("a lone house has no seam", base_corners == 0 and base_cuts == 0,
          "corners=%d cuts=%d" % (base_corners, base_cuts))

    # ---- 摆一栋压上来的邻居：600x400 与 500x500 偏移 (250, 150) ⇒ 轮廓真相交 ----
    loc = house.get_actor_location()
    other = spawn_house("House_SeamNeighbour", loc.x + 250.0, loc.y + 150.0, loc.z, 500.0, 500.0)
    check("seam: the neighbour spawned", other is not None)
    if other is None:
        return

    try:
        house.rebuild_house()
        other.rebuild_house()

        corners = house.get_seam_corner_count()
        cuts = house.get_seam_cut_count()
        bricks = house.get_frame_brick_count()
        seam_bricks = house.get_seam_brick_count()
        check("two overlapping houses grow a seam", corners > 0 and seam_bricks > 0,
              "corners=%d seam_bricks=%d" % (corners, seam_bricks))
        check("the seam clips the walls that poke into the neighbour", cuts > 0, "cuts=%d" % cuts)
        check("the seam bricks join the door-frame bricks in one component",
              bricks == base_bricks + seam_bricks,
              "bricks=%d base=%d seam=%d" % (bricks, base_bricks, seam_bricks))

        # ---- 纯函数性：两栋房各自算出同一条缝（逐位那条在单测里）----
        check("both houses see the same seam", other.get_seam_corner_count() == corners
              and other.get_seam_brick_count() == seam_bricks,
              "A(corners=%d bricks=%d) B(corners=%d bricks=%d)"
              % (corners, seam_bricks, other.get_seam_corner_count(), other.get_seam_brick_count()))

        # 只重建**其中一栋**：没有归属、没有簿记 ⇒ 两边一个数都不许动。
        other.rebuild_house()
        check("rebuilding only one house leaves both seams untouched",
              house.get_seam_corner_count() == corners and house.get_seam_brick_count() == seam_bricks
              and other.get_seam_corner_count() == corners and other.get_seam_brick_count() == seam_bricks,
              "A(corners=%d bricks=%d) B(corners=%d bricks=%d)"
              % (house.get_seam_corner_count(), house.get_seam_brick_count(),
                 other.get_seam_corner_count(), other.get_seam_brick_count()))

        # ---- 邻居的 `bSeamEnabled` 不许影响这栋房（顺序无关的执行面）----
        #
        # ⚠️ **这一条真的红过。** 第一版让 `GatherSeamNeighbours` 读了邻居的 `bSeamEnabled`
        # （"一条缝要么两边都出，要么都不出"），于是分两句改开关时，先重建的那栋看到的是对方的
        # 旧值：出图里 A 报 corners=0 而 B 报 corners=2 —— 正好是本轮要证的对称性的反面。
        # `bSeamEnabled` 是"**我**画不画我这一份"，不是世界状态；裁决二列的输入里也没有它。
        other.set_editor_property("bSeamEnabled", False)
        other.rebuild_house()
        house.rebuild_house()
        check("the neighbour's own seam switch does not touch this house's half",
              house.get_seam_corner_count() == corners and house.get_seam_brick_count() == seam_bricks
              and other.get_seam_brick_count() == 0,
              "A(corners=%d bricks=%d) B(bricks=%d)"
              % (house.get_seam_corner_count(), house.get_seam_brick_count(), other.get_seam_brick_count()))
        other.set_editor_property("bSeamEnabled", True)
        other.rebuild_house()
        house.rebuild_house()

        # ---- 洞是 clip 出来的，几何仍然实心（裁决三，全局不变量）----
        seam_tris = settle_tris(house.get_tiny_glade_mesh())
        check("the seam never carves real geometry (panels get split, not deleted)", seam_tris >= base_tris,
              "tris=%d (was %d)" % (seam_tris, base_tris))

        # ---- 它真的会被画出来（渲染侧逐环 + 墙材质必须是 Masked + 砖材质必须支持实例化）----
        # 调原因版，不调 is_seam_drawable()：后者不可画时返回 None，str(None) 会伪造一句原因。
        why = str(house.get_seam_undrawable_reason())
        check("the seam is actually drawable (Masked wall material / seam bricks / ISM-capable material)",
              why == "", why)
        why_other = str(other.get_seam_undrawable_reason())
        check("and so is the neighbour's half of it", why_other == "", why_other)

        # ---- 幂等 ----
        house.reevaluate_site()
        check("seam rebuild is idempotent",
              house.get_seam_corner_count() == corners and house.get_seam_brick_count() == seam_bricks,
              "corners=%d bricks=%d" % (house.get_seam_corner_count(), house.get_seam_brick_count()))

        # ---- 零阻塞：拖尺寸 12 帧，接缝开着 ----
        #
        # 接缝砖与门框砖共用**同一份常驻容量**，所以这条钉的是"别为了接缝把扩容加回来"。
        # 起始尺寸显式钉死再 rebuild_house()，一次性的容量与包围盒成本不许落进测量窗口。
        house.set_editor_property("FootprintSize", unreal.Vector2D(600.0, 400.0))
        house.rebuild_house()
        check("the seam resize test starts with a seam", house.get_seam_brick_count() > 0,
              "seam_bricks=%d" % house.get_seam_brick_count())
        before = unreal.CSMesh.get_blocking_flush_count()
        for i in range(1, 13):
            house.set_editor_property("FootprintSize", unreal.Vector2D(600.0 + i * 5.0, 400.0))
            house.reevaluate_site()
        flush_delta = unreal.CSMesh.get_blocking_flush_count() - before
        check("dragging FootprintSize for 12 frames with a seam on blocks the game thread zero times",
              flush_delta == 0, "flushes=%d" % flush_delta)
        check("the dragged house still has a seam", house.get_seam_brick_count() > 0,
              "seam_bricks=%d" % house.get_seam_brick_count())

        # ---- 关掉开关 ⇒ 砖与裁剪**逐位**消失（对照组，也是出图脚本用的那个开关）----
        house.set_editor_property("FootprintSize", unreal.Vector2D(600.0, 400.0))
        house.rebuild_house()
        on_bricks = house.get_frame_brick_count()
        house.set_editor_property("bSeamEnabled", False)
        house.rebuild_house()
        check("switching the seam off removes exactly the bricks and cuts it added",
              house.get_seam_corner_count() == 0 and house.get_seam_brick_count() == 0
              and house.get_seam_cut_count() == 0 and house.get_frame_brick_count() == base_bricks,
              "corners=%d seam_bricks=%d cuts=%d bricks=%d/%d (on: %d)"
              % (house.get_seam_corner_count(), house.get_seam_brick_count(), house.get_seam_cut_count(),
                 house.get_frame_brick_count(), base_bricks, on_bricks))
        house.set_editor_property("bSeamEnabled", True)
        house.rebuild_house()

        # ---- 触发条件是 footprint **真重叠**，不是"靠得近"：挪开就没缝 ----
        other.set_actor_location(unreal.Vector(loc.x + 1400.0, loc.y, loc.z), False, False)
        other.rebuild_house()
        house.rebuild_house()
        check("moving the neighbour clear of the footprint drops the seam entirely",
              house.get_seam_corner_count() == 0 and house.get_seam_brick_count() == 0
              and house.get_frame_brick_count() == base_bricks,
              "corners=%d seam_bricks=%d bricks=%d/%d"
              % (house.get_seam_corner_count(), house.get_seam_brick_count(),
                 house.get_frame_brick_count(), base_bricks))
        # 挪开之后房体也要回到原样：接缝裁剪是派生物，没有残留。
        check("and the body goes back to the shape it had before the neighbour arrived",
              settle_tris(house.get_tiny_glade_mesh()) == base_tris,
              "tris=%d want=%d" % (settle_tris(house.get_tiny_glade_mesh()), base_tris))
    finally:
        # 临时邻居用完就销毁 —— 关卡不留痕迹，也不保存。
        unreal.get_editor_subsystem(unreal.EditorActorSubsystem).destroy_actor(other)
        house.rebuild_house()


def demo_house_resize():
    """D5 拉尺寸：禁带 + 单边推拉 + 连续拖动期派生物跟得住。

    ⚠️ 这一节测的是**机制**，不是交互控件：抓手 / gizmo / EdMode 不在 D5 这一轮范围内，
    尺寸一律从 `push_edge`（机制入口）与属性改。将来的 handle actor 只是往 push_edge 喂 Offset，
    这里钉住的每一条不变量它都继承得到。
    """
    unreal.log("========== L_HouseGroundDemo: D5 resize ==========")
    unreal.EditorLoadingAndSavingUtils.load_map("/PCGPlugins/HouseTest/L_HouseGroundDemo")

    ground = find("Ground_Demo")
    house = find("House_Road")
    check("resize actors present", ground and house)
    if not (ground and house):
        return

    # 必须在**有门有砖**的状态下测：空房子那条路就算整个坏掉也照样绿（同 :120 那条纪律）。
    loc = house.get_actor_location()
    ground.reset_paint()
    paint_line(ground, loc.x, loc.y - 800.0, loc.x, loc.y + 800.0, 16)
    house.set_editor_property("FootprintSize", unreal.Vector2D(600.0, 400.0))
    house.rebuild_house()
    check("the resize section starts with arches and bricks",
          house.get_open_door_count() > 0 and house.get_frame_brick_count() > 0,
          "doors=%d bricks=%d" % (house.get_open_door_count(), house.get_frame_brick_count()))

    # ---- ① 单边推拉：对侧墙不动、被推墙恰好走 Δ（"拖 1 m 走 2 m" 的钉子）----
    # 纯函数那两条单测在 CSHouseLogicTests，这里验的是**接到真 actor 上之后仍然成立** ——
    # 落座会改 Z、构造脚本会重跑，中途任何一处把中心写回去，这条就会红。
    # ⚠️ **`get_editor_property` 取结构体给的是活视图，不是副本**（实测踩到）：把返回的
    # Vector2D 存下来当"改之前的值"用，读到的其实是改之后的 —— 症状是"对侧墙走了半个 Δ、
    # 被推墙也只走半个 Δ"，看着**一模一样像那个 2× 父子回路缺陷**，实际是脚本自己读错了。
    # 一律当场拆成 float。
    yaw = math.radians(house.get_actor_rotation().yaw)
    before_loc = house.get_actor_location()
    size_before_x = float(house.get_editor_property("FootprintSize").x)
    # edge 1 = 东墙，局部外法线 +X。
    applied = house.push_edge(1, 120.0)
    after_loc = house.get_actor_location()
    size_after_x = float(house.get_editor_property("FootprintSize").x)
    outer = (math.cos(yaw), math.sin(yaw))
    east_before = (before_loc.x + outer[0] * size_before_x * 0.5, before_loc.y + outer[1] * size_before_x * 0.5)
    east_after = (after_loc.x + outer[0] * size_after_x * 0.5, after_loc.y + outer[1] * size_after_x * 0.5)
    west_before = (before_loc.x - outer[0] * size_before_x * 0.5, before_loc.y - outer[1] * size_before_x * 0.5)
    west_after = (after_loc.x - outer[0] * size_after_x * 0.5, after_loc.y - outer[1] * size_after_x * 0.5)
    near("pushing the east wall applies the whole offset", applied, 120.0, 0.01, " cm")
    check("the west wall does not move when the east wall is pushed",
          abs(west_after[0] - west_before[0]) < 0.05 and abs(west_after[1] - west_before[1]) < 0.05,
          "dx=%.4f dy=%.4f" % (west_after[0] - west_before[0], west_after[1] - west_before[1]))
    near("the east wall moves exactly the offset",
         math.hypot(east_after[0] - east_before[0], east_after[1] - east_before[1]), 120.0, 0.05, " cm")

    # ---- ② 禁带：连续推拉扫过翻轴点，脊向在**平滑步**里一次都不翻 ----
    #
    # ⚠️ 断言的口径（2026-08-30 裁决四，收缩过，理由写在 CSHouseLogicTests 那条同名用例里）：
    # 从 X 长拖到 Y 长必然要改一次脊向 —— 那是拓扑必然，禁带管不了。禁带保证的是
    # **改的那一步同时是尺寸跳变的那一步**：用户读到"房子换了个形状"，而不是"屋顶自己转了 90°"。
    # 平滑步里一次都不翻 + 尺寸从不停在带内，这两条合起来才是"挡住翻轴现象"的可验证形态。
    house.set_editor_property("FootprintSize", unreal.Vector2D(400.0, 320.0))
    house.set_editor_property("RidgeAxis", unreal.CSRidgeAxis.X)
    house.rebuild_house()
    band = house.get_footprint_band_range(2)          # edge 2 = 北墙，推的是 Y
    check("the ridge-flip threshold sits inside the band",
          band.x < 400.0 * 1.15 < band.y, "band=[%.1f, %.1f] threshold=%.1f" % (band.x, band.y, 400.0 * 1.15))

    axis = house.get_editor_property("RidgeAxis")
    smooth_flips, jump_flips, inside_band, prev_y = 0, 0, 0, house.get_editor_property("FootprintSize").y
    for _ in range(60):
        house.push_edge(2, 5.0)
        size = house.get_editor_property("FootprintSize")
        jumped = abs(size.y - prev_y) > 5.0 + 1e-3
        # 带随 X 走，X 在这段里不动，所以每帧重问一次也是同一条 —— 但别写死，X 会被别的用例改。
        rng = house.get_footprint_band_range(2)
        if rng.x < size.y < rng.y:
            inside_band += 1
        now = house.get_editor_property("RidgeAxis")
        if now != axis:
            if jumped:
                jump_flips += 1
            else:
                smooth_flips += 1
        axis, prev_y = now, size.y
    check("a continuous drag never flips the ridge on a smooth step", smooth_flips == 0,
          "smooth=%d jump=%d" % (smooth_flips, jump_flips))
    check("no dragged size ever rests inside the band", inside_band == 0, "inside=%d" % inside_band)
    check("the one ridge flip rides the band jump", jump_flips == 1, "jump=%d" % jump_flips)
    check("the drag still ends with the ridge along the long axis",
          axis == unreal.CSRidgeAxis.Y, "axis=%s" % axis)
    # 净位移守恒：卡口只重排位移的分布，不吞长度。拖 300 cm 墙就走 300 cm。
    near("total wall travel equals total drag travel",
         house.get_editor_property("FootprintSize").y, 320.0 + 300.0, 0.05, " cm")

    # ---- ③ 拖动期零阻塞（第十一条 flushes=0），且派生物跟得住 ----
    #
    # 与上面那些 set_editor_property 拖动的区别：push_edge 是**机制入口**，它会多做两件事
    # （标脏 subsystem、按禁带修正尺寸）。这条断言保证那两件事没有把容量/包围盒的稳态破掉。
    house.set_editor_property("FootprintSize", unreal.Vector2D(600.0, 400.0))
    house.rebuild_house()
    check("the push-edge flush test starts with frame bricks", house.get_frame_brick_count() > 0,
          "bricks=%d" % house.get_frame_brick_count())
    flush_before = unreal.CSMesh.get_blocking_flush_count()
    for _ in range(12):
        house.push_edge(1, 5.0)        # 只长 X（本来就是长轴），不掺翻脊
    flush_delta = unreal.CSMesh.get_blocking_flush_count() - flush_before
    check("dragging with push_edge for 12 frames blocks the game thread zero times", flush_delta == 0,
          "flushes=%d" % flush_delta)

    drag_tris = settle_tris(house.get_tiny_glade_mesh())
    drag = (house.get_frame_brick_count(), house.get_opening_count(),
            house.get_vine_segment_count(), house.get_decor_instance_count())
    house.rebuild_house()
    rebuilt = (house.get_frame_brick_count(), house.get_opening_count(),
               house.get_vine_segment_count(), house.get_decor_instance_count())
    # 「派生物跟得住」的可断言形态：拖完的一切必须与"直接按最终尺寸全量重建"逐字相同。
    # 只验三角形数会漏掉砖/藤/摆件 —— 它们各走各的组件，各有各的容量棘轮。
    check("the dragged house matches a full rebuild at the same size",
          settle_tris(house.get_tiny_glade_mesh()) == drag_tris and drag == rebuilt,
          "dragged tris=%d bricks/openings/vines/decor=%s rebuilt=%s" % (drag_tris, drag, rebuilt))

    # ---- ④ 松手：累加器归位，下一次拖动的第一帧不许自己跳 ----
    house.set_editor_property("FootprintSize", unreal.Vector2D(400.0, 320.0))
    house.rebuild_house()
    for _ in range(16):
        house.push_edge(2, 5.0)        # 攒到带心边上：再推一步就会跨带
    stuck = house.get_editor_property("FootprintSize").y
    house.push_edge(2, 0.0, True)      # 松手
    first = house.push_edge(2, 5.0)    # 新一次拖动的第一帧
    # 不清累加器的话这一帧会直接跨带（跳 160 cm）—— 手才刚按下去房子就自己换了个形状。
    check("releasing resets the drag accumulator so the next drag cannot jump",
          abs(first) <= 5.0 + 1e-3, "stuck=%.1f first=%.3f" % (stuck, first))

    house.set_editor_property("FootprintSize", unreal.Vector2D(600.0, 400.0))
    ground.reset_paint()
    house.rebuild_house()


demo_house_ground()
demo_house_resize()
demo_terrain_ops()
demo_gpu_stairs()
demo_rock_shell()
demo_skirt_decor()
demo_house_vine()
demo_house_decor()
demo_house_window()
demo_house_seam()

unreal.log("========== SUMMARY ==========")
unreal.log("passed=%d failed=%d" % (PASSES[0], len(FAILS)))
for f in FAILS:
    unreal.log_error("  failed: %s" % f)
unreal.log("REGRESS FAILED" if FAILS else "REGRESS OK")
