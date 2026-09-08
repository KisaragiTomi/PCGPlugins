# Tiny Glade 复刻：文档索引

本目录集中存放 Tiny Glade 式交互房屋系统（`ACSGroundActor` / `ACSHouseActor` / `ACSGroundShaperActor` 一族）的全部设计、对照与逆向文档。
2026-08-31 从插件根与 `Docs/` 归拢至此，同时把五份对照文档合成一卷、两轮逆向报告合成一卷。

## 从哪读起

- **要改代码** → 先读 [`TinyGladeHouse_Plan.md`](TinyGladeHouse_Plan.md)：唯一的裁决记录，D1–D14 的设计与阶段验收门都在里面。
- **要接着推进** → 读 [`TinyGlade_模块对照与进度.md` 卷零](TinyGlade_模块对照与进度.md#vol-0)：自监督循环的状态文件，含模块状态表、待拍板清单、验收门与「踩过的坑」。
- **要动任何"砖"** → 读 [`TinyGlade_模块对照与进度.md` 卷五](TinyGlade_模块对照与进度.md#vol-5)：门框 / 拱圈石 / 转角 / 墙裙 / 承重柱 / 接缝 / 垛口 / 上沿在 TG 里是不是一套，以及唯一一处真 SplineMesh 在哪。
- **要动窗（D8）** → 只读 [`TinyGladeWindow.md`](TinyGladeWindow.md)：计划 D8 整章 + TG 侧对照 + 时间线已在 2026-09-06 合到这一篇，开头有「现状速查表」（每条带日期与验证它的单测 / 回归断言名）。原文各处只剩存根。
- **要重构编排层** → 先读 [`../../TinyGlade_结构审查.md`](../../TinyGlade_结构审查.md)：2026-09-06 的整体结构审查，含实例化产物管线复制、`ReevaluateSite` 隐式数据流与哈希缺口清单、矩形假设与两套墙体的待拍板项；2026-09-07 追加「GPU 基座与打包契约」（21–25）与「生命周期与持久化」（26–30）两节。
- **要做树／树叶** → 读 [`TinyGlade_树冠着色.md`](TinyGlade_树冠着色.md)：TG 树冠的 shader 侧全链路（VS 八段变形、深度浮雕、专用延迟光照），几何侧仍由 `build_tree.py` / `Tree.hip` owner。
- **想翻某条已定结论** → 先查 [`TinyGlade_对比逆向报告.md`](TinyGlade_对比逆向报告.md) 的「被否条目」与「看似该抄其实不该抄」，别重复推翻。

## 文档清单

| 文件 | 内容 | 合并前 |
| --- | --- | --- |
| [`TinyGladeHouse_Plan.md`](TinyGladeHouse_Plan.md) | 设计裁决主文档：D1–D14、阶段计划 P0–P9、风险、开放问题 | 原在插件根 |
| [`TinyGlade_模块对照与进度.md`](TinyGlade_模块对照与进度.md) | 六卷合卷：完成进度（卷零）+ 模块对照（卷一～卷五） | 5 份 + 卷五新增 |
| [`TinyGladeWindow.md`](TinyGladeWindow.md) | **窗户（D8）专卷**：现状速查表 + 计划 D8 整章 + TG 侧对照（原卷二 §一～§四 / §7.2 / C4）+ 时间线（原卷零四条）+ 已作废但保留的结论 + 未完成事项 | *（2026-09-06 从计划书 D8 与合卷卷零/卷二抽出，原处留存根）* |
| [`TinyGlade_对比逆向报告.md`](TinyGlade_对比逆向报告.md) | 两卷合卷：两轮多 agent 对比逆向评审的原始报告 | 2 份 |
| [`CSGroundShaper.md`](CSGroundShaper.md) | 地形塑形物（D9）：Houdini 原型 → UE 的逐节点对照与算法替换 | 原在 `Docs/` |
| [`CSRockShellPattern.md`](CSRockShellPattern.md) | 披挂岩壳（D9 链 B）：烘焙件的通道契约与五条实测订正 | 原在 `Docs/` |
| [`TinyGlade_树冠着色.md`](TinyGlade_树冠着色.md) | **树冠着色专卷**：叶卡片的 VS 变形、紧凑 G-buffer、树冠专用延迟光照，含深度浮雕与逆光旁路两条核心机制、其它叶类（藤蔓叶／灌木／背景树／落叶粒子）对照、移植到 UE 的四条缺口 | *（2026-09-07 新增）* |
| [`CSGroundTuning.md`](CSGroundTuning.md) | 地面派生链（地被 / 石阶 / 石阶材质 / 岩壳）的默认值：2026-09-06 那轮改了什么，以及演示关卡那套岩壳覆盖值的留档 | *（2026-09-06 新增）* |
| [`CSRockShellEdgeBevel.md`](CSRockShellEdgeBevel.md) | 岩壳假倒角：TG 像素层缺口的机制证据与 UE 落地。**运行时壳 = v3**（邻接进 UV + 逐趟披挂重写，走 `M_TG_Texture` 的静态开关 `RockShellBevel`）；**直摆资产 = v2**（顶点色载荷，走 `M_TinyGladeRockShell`，legacy 但仍在用） | *（2026-09-01 新增，09-04 落地 v3，09-05 重整）* |
| [`TinyGlade_结构审查_附录A_墙体剖面.md`](TinyGlade_结构审查_附录A_墙体剖面.md) | 结构审查的子代理报告 A（深挖 C）：16 行洞曲线消费者对照表、`CLIP_HLSL` 逐语句对照、逐函数的矩形假设清单与折线升级规模；结论已并入 `TinyGlade_结构审查.md` 大问题 3、4 | *（2026-09-07 新增）* |
| [`TinyGlade_结构审查_附录B_GPU基座.md`](TinyGlade_结构审查_附录B_GPU基座.md) | 结构审查的子代理报告 B：GPU 基座与九条打包路的布局对照表、31 处阻塞点计数表、容量 / 确定性契约；结论已并入插件根的 `TinyGlade_结构审查.md` 第 21–25 条 | *（2026-09-07 新增）* |
| [`TinyGlade_结构审查_附录C_生命周期.md`](TinyGlade_结构审查_附录C_生命周期.md) | 结构审查的子代理报告 C（深挖 G）：六个类的状态分类表（权威 / 派生 / 持久记忆 / 拖动期临时 / 跨 actor 登记）、六条生命周期路径矩阵、钩子对称性矩阵、既有结论校核表；结论已并入插件根的 `TinyGlade_结构审查.md` 第 26–30 条并订正第 12 条 | *（2026-09-07 新增）* |

合卷各卷的入口锚点：

| 锚点 | 卷 | 原文件名 |
| --- | --- | --- |
| [`#vol-0`](TinyGlade_模块对照与进度.md#vol-0) | 卷零 · 完成进度与循环协议 | `TinyGlade_模块对照进度.md` |
| [`#vol-1`](TinyGlade_模块对照与进度.md#vol-1) | 卷一 · 拉尺寸（D5）与接缝角柱（D7） | `TinyGlade_拉尺寸与接缝对照.md` |
| [`#vol-2`](TinyGlade_模块对照与进度.md#vol-2) | 卷二 · 窗户（D8）与装饰／藤蔓（D12/D13）。⚠️ 窗那半（§一～§四 / §7.2 / C4）已于 2026-09-06 迁到 [`TinyGladeWindow.md`](TinyGladeWindow.md)，原处只剩存根；装饰／藤蔓那半照旧在这里 | `TinyGlade_窗户与装饰对照.md` |
| [`#vol-3`](TinyGlade_模块对照与进度.md#vol-3) | 卷三 · 楼梯模块 | `TinyGlade_楼梯模块对照.md` |
| [`#vol-4`](TinyGlade_模块对照与进度.md#vol-4) | 卷四 · 渲染与光照（D14） | `TinyGlade_渲染光照对照.md` |
| [`#vol-5`](TinyGlade_模块对照与进度.md#vol-5) | 卷五 · 砖构件：门框 / 转角 / 垛口 / 接缝 / 上沿 | *（2026-08-31 新增，无前身）* |
| [`#round-1`](TinyGlade_对比逆向报告.md#round-1) | 卷一 · 第一轮：门洞机制与改进清单 | `TinyGlade_对比逆向报告_第一轮.md` |
| [`#round-2`](TinyGlade_对比逆向报告.md#round-2) | 卷二 · 第二轮：未覆盖维度 | `TinyGlade_对比逆向报告_第二轮.md` |

## 图索引

| 图 | 说明 | 被谁引用 |
| --- | --- | --- |
| [`tiny-glade-house-change-flows.svg`](tiny-glade-house-change-flows.svg) | 地面变化 vs 房子移动各自改什么（D3/D4/D6/D7/D9 的直推链） | 计划 D3 |
| [`tiny-glade-decor-placement-flow.svg`](tiny-glade-decor-placement-flow.svg) | 摆件／植被放置流程：场在 GPU、异步回读、diff 应用 | 计划 D12 |
| [`tiny-glade-reevaluate-hidden-dataflow.svg`](tiny-glade-reevaluate-hidden-dataflow.svg) | `ReevaluateSite` 的隐式数据流：11 步靠成员变量传中间结果，写 / 读 / 读写逐一标出，顺序约束只在注释里 | 结构审查 大问题 2 |
| [`tiny-glade-module-interaction.svg`](tiny-glade-module-interaction.svg) | 模块交互图：房子 / 地面 / 子系统 / 标记 / 抓手 / 塑形物 / 编辑器模块 / 遗留藤蔓之间的同步调用、委托广播、世界扫描三种边，圈码对应审查小节 | 结构审查 深挖 D（8–19） |
| [`tiny-glade-undo-chain.svg`](tiny-glade-undo-chain.svg) | 撤销链路：一次细节面板改 `FootprintSize` 的记录期、`FTransaction::Apply` 四步（`PreEditUndo` → 全部 `Restore` → 组件优先排序 → 逐个 `PostEditUndo`）、插件钩子的扇出（同一次 Ctrl+Z 四次进 `ReevaluateSite`），底栏是事务看不见的状态 | 结构审查 深挖 G（27） |
| [`CSGroundShaper_PrototypeMapping.svg`](CSGroundShaper_PrototypeMapping.svg) | `TinyGlade.hip /obj/geo1` 三条链 → `ACSGroundShaperActor` 的逐节点对照 | `CSGroundShaper.md` |
| [`CSGroundShaper_Algorithms.svg`](CSGroundShaper_Algorithms.svg) | UE 端怎么替掉 Houdini 的解法：四处算法替换 | `CSGroundShaper.md`、计划 D9 |
| [`CSGroundStairs_Logic.svg`](CSGroundStairs_Logic.svg) | 石阶的 GPU 逻辑：marching squares、100% GPU 决策、零回读 | 合卷卷零「石阶 S1」 |
| [`CSHouseDoor_Logic.svg`](CSHouseDoor_Logic.svg) | 门洞的当前逻辑：闭环求解、弦长即门宽、转角配成墩；末栏是「已修 / 仍开着」的现状 | 计划 D6 |
| [`CSRockShellEdgeBevel_Logic.svg`](CSRockShellEdgeBevel_Logic.svg) | 缺口法线的生成：顶点色慢变量 → 逐像素噪声倒角 → 折痕截面的法线弯折。⚠️ 只画 **v2**（直摆资产）那条链路，运行时壳的 v3 走 UV 载荷 | `CSRockShellEdgeBevel.md` |
| [`CSRockShellEdgeBevel_TGNormals.svg`](CSRockShellEdgeBevel_TGNormals.svg) | TG 原版法线流转：逐三角 SSBO 字段，CS 每帧重算写回、PS 按 id 查自己与邻面 | `CSRockShellEdgeBevel.md` |
| [`TinyGlade_树冠着色_Pipeline.svg`](TinyGlade_树冠着色_Pipeline.svg) | 树冠着色链路：资产层 → VS 八段 → PS 四段 → G-buffer/深度 → 专用延迟光照；橙块与橙线是贯穿三处的深度浮雕 | `TinyGlade_树冠着色.md` |
| [`TinyGlade_树冠着色_深度浮雕.svg`](TinyGlade_树冠着色_深度浮雕.svg) | 深度浮雕的入门解释：平卡片相交是直线 → 按厚度图谎报深度 → 交线沿叶簇轮廓弯曲；末栏是「只有深度是假的」这条代价 | `TinyGlade_树冠着色.md` |
| [`CSRockShellPattern.preview.png`](CSRockShellPattern.preview.png) | 岩壳原件碎裂图案（TG `rocky_terrain_shell.glb`，609 胞腔） | `CSRockShellPattern.md` |
| [`CSRockShellPattern.fallback.preview.png`](CSRockShellPattern.fallback.preview.png) | 后备生成器的图案（真 Voronoi + Lloyd × 6） | `CSRockShellPattern.md` |
| [`img/TG_continuous_arches.png`](img/TG_continuous_arches.png) | 连续拱之间没有「墙」这个表面的实拍裁决 | 计划 D6、`CSHouseProfile.h` |
| `img/tiny-glade-ref-*.jpg` | 五张 TG 实拍参考：拱间墩／角柱／垛口／双拱墩／深度融合无缝 | 计划 D6/D7 |
| [`img/tiny-glade-ref-door-in-arch.png`](img/tiny-glade-ref-door-in-arch.png) | 门在拱里的实拍：轮廓严丝合缝贴着拱圈石内缘 ⇒ **门是比洞更大的矩形，被墙切出剪影**，不是拱形网格 | 卷零「D6 续」③ |
| [`img/tiny-glade-ref-corner-arch-passage.png`](img/tiny-glade-ref-corner-arch-passage.png) | 转角实拍：**两道拱共用一根角柱，没有木门** | 卷零「D6 续」④ |

⚠️ 两张预览 PNG 同时是脚本的**功能性输出路径**（`Scripts/BakeRockShellPattern.py` 与 `Scripts/VerifyRockShellGlb.py` 的 `DEFAULT_PREVIEW_RELPATH`），改名或再次搬动要同步改那两处常量。

## 完成进度速览

⚠️ **权威进度在合卷卷零，本表只是入口摘要。** 卷零最后更新于 2026-08-30 23:16，而 08-31 上午另有四个模块落地 —— 下表已按源码核对补上，卷零本身尚未回填。

| 模块 | 状态 | 落地位置 |
| --- | --- | --- |
| D1 地面 / D2 顶点色笔刷 / D3 直推通知 | 已落地 | `CSGroundActor.{h,cpp}` |
| D4 房屋 + 屋面（脊向由长轴连续导出、墙-顶收边） | 已落地。⚠️ 早先这一行写的「脊向滞回」已随四坡屋顶于 2026-08-31 删除，见下方 D4 屋面行 | `CSHouseActor.{h,cpp}`、`CSHouseRoof.h` |
| D6 门洞（逐像素 clip + 门框砖） | 已落地；**2026-09-04 重做触发与宽度口径**：门宽 = 路在墙上截出的弦长，过宽切成拱廊 | `CSHouseDoorRuns.h`、`CSHouseFrame.{h,cpp}`、`CSHouseFrame.usf` |
| D6 门扇（`DoorLeafMesh` 一族） | **2026-09-04 新增**：一洞一个静态网格组件，按洞宽缩放 | `CSHouseActor::RebuildDoorLeaves`、`Scripts/TinyGladeSetupDoorLeaf.py` |
| D9 承重柱 + 塑形物（裙边噪声 / 二次抬升 / 披挂岩壳） | 已落地 | `CSGroundShaperActor.cpp`、`CSGroundRockShell.{cpp,usf}` |
| D10 subsystem / D11 Spline 块排布 | 已落地 | `CSHouseSubsystem.{h,cpp}` |
| D13 藤蔓（枝 319 / 叶 216） | 第一档已落地 | `CSHouseVine.{h,cpp,usf}` |
| D14 渲染与光照 | 观感一轮已落地 | `Scripts/TinyGladeSetupLighting.py` |
| 楼梯 S1 + S2 + S3 | 已落地，**旧路已删干净** | `CSGroundStairs.{h,cpp,usf}` |
| D5 拉尺寸 | **2026-08-31 机制层 + 2026-09-05 交互层，整条已通**：单边推拉纯函数 + `PushEdge` 入口 + `EnterResizeMode` 生成的四个抓手 actor（标准 gizmo 拖，失选自动退出）。尺寸禁带随四坡屋顶同日删除，见 D4 行 | `CSHouseResize.h`、`CSHouseResizeHandleActor.{h,cpp}`、`CSHouseResizeSelectionWatcher.{h,cpp}` |
| D7 接缝（形状相交） | **2026-08-31 落地**：纯函数接缝砖，洞走 clip 不挖真几何 | `CSHouseSeam.h` |
| D7 转角角石（墙自身转角） | **2026-08-31 落地**：四角竖直砖柱，与接缝柱共用 `CSHouseFrame::AppendColumn`。**D7 两半至此都合上** | `CSHouseQuoin.h` |
| D7 包边石（A8） | **2026-08-31 落地**：墙顶压顶 + 墙脚勒脚，共用 `CSHouseFrame::AppendFlatRun`；勒脚按洞切段 | `CSHouseTrim.h` |
| D4 屋面（四坡 + 瓦） | **2026-08-31 落地**：双坡实体板整套删除，屋面全部由瓦铺成。脊向由长轴导出、平局归 X（**用户已裁掉裁决四**） | `CSHouseTile.{h,cpp,usf}` |
| D4 屋面收尾（脊瓦 + 尖顶） | **2026-08-31 落地**：瓦厚 6 cm、排距定档 827 片/栋、五条脊线盖脊瓦、脊端点各立一根尖顶（正方形退化成一根） | `CSHouseTile.cpp`、`CSHouseActor::RebuildRoofFinials` |
| D9 岩壳「石头隆起」 | **2026-08-31 落地**：六个蓝图可调参数（朝外扩张 / 隆起倍数 / 噪波 / 台顶外扩 / 末端上限）。⚠️ **同日晚看图裁决：隆起三参默认归中性**（无界抬升在演示档下读成火山口壁），降级为风格化旋钮 | `CSGroundRockShell.usf` |
| D9 岩壳体积（TG 两层） | **2026-08-31 落地**：`displace:563` 的基准偏移（`BaseLift`/`BaseSink`）+ 表面起伏改「幅度 ∝ 坡度、偏正」。⚠️ `RockShellNoiseAmount` 语义随之变成「坡度 = 1 时的满幅」 | `CSGroundRockShell.usf` |
| 墙面材质（灰泥剥落 + 凸出砖） | **2026-09-03 建图 / 09-04 引擎验收**：TG `_nani_plaster` 的移植，两处有意不同构；09-04 补了 `PeelBias`（顶替 TG 缺失的覆盖度项）与四个默认值改档 | `Scripts/TinyGladeMakeWallMaterials.py`、`M_TinyGladeWall` |
| D8 窗 | **2026-08-31 整个模块合上**：洞与谓词 + `ACSWindowMarker` 交互 actor（射线解析宿主 / 登记诉求 / 换宿主 / 无宿主自毁）。**2026-09-06 交互层重定向**：附属物自带 mesh、锚点是权威、窗不出框砖。逐条见 [`TinyGladeWindow.md`](TinyGladeWindow.md) | `CSHouseProfile.h`、`CSHouseFeatureMarker.{h,cpp}` |
| D12 摆件 | **2026-08-31 落地锚点层**（五家锚点）；复杂度场那一半按 C2 有意未做 | `CSHouseDecor.{h,cpp,usf}`、`CSGroundDecor.{h,cpp}` |

✅ **待用户拍板的最后一条已于 2026-09-04 关闭**（不是拍板，是**证伪**）：「TG 的门与道路无关」这条前提是错的 —— TG 的拱**就是道路驱动**，拱宽 = 路在墙上截出的那一段长度（`ArchSegment (*)(WallPathSegment)`）。分歧只剩口径（二值化 vs 连续、等分 vs 打分求解），逐条见卷零「❌ 已推翻：门洞的触发规则与 TG 不同」。

## 证据来源

按可信度排列。解 PDB 系统签名的用法与逐条取舍见卷零同名小节。

| 来源 | 路径 | 说明 |
| --- | --- | --- |
| PDB 符号 | `D:/MyProject/Tiny Glade/tmp/pdb_symbols.txt` | 97033 条，**最硬的证据** |
| 逆向分析 | `D:/MyProject/Tiny Glade/MESH_GENERATION_ANALYSIS.md` | 461 行，自带 `【确凿】/【推测】/【待确认】` 标注 |
| 反编译着色器 | `D:/MyProject/Tiny Glade/tmp/shaders` | GLSL 实证 |
| 提取资产 | `Content/HouseTest/TinyGladeAsset/`（`/PCGPlugins/HouseTest/TinyGladeAsset`） | 474 个 StaticMesh，尺寸一律实测不猜 |
| Houdini 原型 | `D:/MyProject/Houdini/TinyGlade/TinyGlade.hip` | 用户的意图原型，对照写法见 `CSGroundShaper.md` |
| 两轮对照评审 | [`TinyGlade_对比逆向报告.md`](TinyGlade_对比逆向报告.md) | 含被否条目，别重复推翻 |
| 模块对照 | [`TinyGlade_模块对照与进度.md`](TinyGlade_模块对照与进度.md) 卷一～卷四 | 本循环自产，被卷零引用的部分已经驱动方复核 |

⚠️ 四卷各自的「证据标注约定」**没有合并**：口径逐卷不同（证据种类、易误采提醒、轴向换算都不一样），压成一张表会丢信息。读某一卷时以该卷自己的约定为准。

## 相关但不在本目录

| 位置 | 内容 | 为什么不在这里 |
| --- | --- | --- |
| `Plugins/PCGPlugins/Scripts/TinyGlade*.py` | 60 个演示搭建 / 出图 / 回归脚本（09-04 新增墙面材质三件 + `TinyGladeSetupDoorLeaf.py` / `TinyGladeShotDoorWidth.py`） | 可执行文件，不是文档 |
| `Plugins/PCGPlugins/Source/ComputeShaderGenerator/` | `CSHouse*` / `CSGround*` 的类注释 | 逐字段口径以源码注释为准，MD 不复述 |
| `/PCGPlugins/HouseTest/` | `L_HouseGroundDemo`、`L_TerrainOpsDemo` 两张演示关卡 | UE 资产 |
| `doc/index.md` | 项目级文档索引 | 已收录本目录 |
| `Plugins/PCGPlugins/TinyGlade_结构审查.md` | 2026-09-06 整体结构审查：编排层复制、隐式数据流与哈希缺口、矩形假设、两套墙体、建议顺序与待拍板；09-06 晚追加模块交互 / 冗余代码两节（8–20），09-07 追加 GPU 基座与打包契约（21–25，子代理报告见本目录附录 B）与生命周期 / 持久化（26–30，子代理报告见本目录附录 C） | 审查结论放插件根，与 `README.md` 并列，首屏可见 |

## 目录布局

```text
Docs/TinyGlade/
├─ index.md                              # 本文件
├─ TinyGladeHouse_Plan.md                # 设计裁决主文档
├─ TinyGlade_模块对照与进度.md            # 合卷：卷零进度 + 卷一～卷四对照
├─ TinyGladeWindow.md                    # 窗户（D8）专卷：计划 + TG 对照 + 时间线
├─ TinyGlade_对比逆向报告.md              # 合卷：两轮逆向评审
├─ CSGroundShaper.md                     # 地形塑形物实现文档
├─ CSRockShellPattern.md                 # 披挂岩壳烘焙件契约
├─ CSRockShellEdgeBevel.md               # 岩壳假倒角（像素层缺口）落地
├─ CSGroundTuning.md                     # 地面派生链的默认值与调参裁决记录
├─ TinyGlade_树冠着色.md                  # 树冠 shader 侧全链路（几何侧在 build_tree.py / Tree.hip）
├─ TinyGlade_结构审查_附录A_墙体剖面.md    # 结构审查子代理报告 A（深挖 C：墙体 / 剖面契约 / 矩形假设）
├─ TinyGlade_结构审查_附录B_GPU基座.md    # 结构审查子代理报告 B（GPU 基座与打包契约）
├─ TinyGlade_结构审查_附录C_生命周期.md    # 结构审查子代理报告 C（生命周期与持久化）
├─ geo/                                  # 壳源数据：rocky_terrain.json / *.glb / *.FBX；bevel/ 下为倒角烘焙件
├─ *.svg                                 # 11 张流程／算法图
├─ CSRockShellPattern*.preview.png       # 2 张图案预览（脚本功能性输出路径）
└─ img/                                  # 实拍参考图（1 张裁决图 + 5 张 TG 参考）
```
