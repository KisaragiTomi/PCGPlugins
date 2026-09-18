# Tiny Glade 复刻：文档索引

本目录集中存放 Tiny Glade 式交互房屋系统（`ACSGroundActor` / `ACSHouseActor` / `ACSGroundShaperActor` 一族）的全部设计、对照与逆向文档。
2026-08-31 从插件根与 `Docs/` 归拢至此，同时把五份对照文档合成一卷、两轮逆向报告合成一卷。

## 从哪读起

- **要改代码** → 先读 [`TinyGladeHouse_Plan.md`](TinyGladeHouse_Plan.md)：唯一的裁决记录，D1–D14 的设计与阶段验收门都在里面。
- **要接着推进** → 读 [`TinyGlade_模块对照与进度.md` 卷零](TinyGlade_模块对照与进度.md#vol-0)：自监督循环的状态文件，含模块状态表、待拍板清单、验收门与「踩过的坑」。
- **要动任何"砖"** → 读 [`TinyGlade_模块对照与进度.md` 卷五](TinyGlade_模块对照与进度.md#vol-5)：门框 / 拱圈石 / 转角 / 墙裙 / 承重柱 / 接缝 / 垛口 / 上沿在 TG 里是不是一套，以及唯一一处真 SplineMesh 在哪。
- **要查房屋材质与模型修正** → 读 [`HouseMaterialAudit_20260912.md`](HouseMaterialAudit_20260912.md)：转角覆盖、支撑柱截面、墙面法线与贴图配对、屋顶尖饰和草材质修复，附同机位对照及验证结果。
- **要查地面三层材质** → 读 [`CSGroundMaterial.md`](CSGroundMaterial.md)：草地／深色土／浅色土路的反编译混合公式、高度噪声、用户参考图与 UE 对照。
- **要动窗（D8）** → 只读 [`TinyGladeWindow.md`](TinyGladeWindow.md)：计划 D8 整章 + TG 侧对照 + 时间线已在 2026-09-06 合到这一篇，开头有「现状速查表」（每条带日期与验证它的单测 / 回归断言名）。原文各处只剩存根。窗贴墙脚变门的逆向依据见 [附录 E](TinyGlade_窗变门逆向_附录E.md)。
- **要动接缝 / 摞房（D7）** → 读计划 D7「接触记录 `FCSHouseContact` 与派生」：两端共持的记录、谁动谁发、松手提交、柱逐接触出（永不跨接触合并）；TG 侧证据在 `evidence/inter-shape-stitches*.asm`。⚠️ 横缝（摞房）只判定、不出产物 —— 2026-09-17 照反汇编加的承托梁已被用户判「TG 中没有这个结构」退回，别再加。
- **要动玩家绘制楼梯** → 读 [附录 D](TinyGlade_楼梯逆向_附录D_玩家绘制楼梯.md)：`ACSStairsActor` / `CSStairs.h` 照它实现，未做的部分逐条列在计划「开放问题」的楼梯条目里。
- **要重构编排层** → 先读 [`../../TinyGlade_结构审查.md`](../../TinyGlade_结构审查.md)：2026-09-06 的整体结构审查，含实例化产物管线复制、`ReevaluateSite` 隐式数据流与哈希缺口清单、矩形假设与两套墙体的待拍板项；2026-09-07 追加「GPU 基座与打包契约」（21–25）与「生命周期与持久化」（26–30）两节；2026-09-11 全文复核，开头的「复核总览」给出逐条现状、审查之后的用户裁决与原结论订正，新发现编为 31–38；2026-09-18 在「复核总览」后补记子系统删除、接缝改接触记录之后各条的现状。配图（模块交互 / 隐式数据流 / 撤销链路）已按 09-18 代码重画。
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
| [`TinyGlade_树冠着色.md`](TinyGlade_树冠着色.md) | **树冠着色专卷**：叶卡片的 VS 变形、紧凑 G-buffer、树冠专用延迟光照，含深度浮雕与逆光旁路两条核心机制、其它叶类（藤蔓叶／灌木／背景树／落叶粒子）对照、移植到 UE 的四条缺口（**2026-09-09 已落地为 `M_TinyGladeCanopy`**，建图脚本 `TinyGladeMakeCanopyMaterial.py`、验收 `TinyGladeShotCanopy.py`） | *（2026-09-07 新增，09-09 落地）* |
| [`CSGroundTuning.md`](CSGroundTuning.md) | 地面派生链（地被 / 石阶 / 石阶材质 / 岩壳）的默认值：2026-09-06 那轮改了什么，以及演示关卡那套岩壳覆盖值的留档 | *（2026-09-06 新增）* |
| [`CSGroundMaterial.md`](CSGroundMaterial.md) | 地面三层材质（草地 / 深色土 / 浅色土路）：反编译混合公式、高度噪声控制的露土斑块、用户参考图与 UE 同机位对照；`M_TinyGladeGround` / `MI_TinyGladeGround` | *（2026-09-12 新增）* |
| [`HouseMaterialAudit_20260912.md`](HouseMaterialAudit_20260912.md) | 房屋材质与模型修正：转角覆盖、支撑柱截面、墙面法线与贴图配对、屋顶尖饰、草材质；附 `house-audit-20260912-*` 同机位对照 | *（2026-09-12 新增）* |
| [`MeshMaterialRepair.md`](MeshMaterialRepair.md) | 提取网格的材质槽修正：`TinyGladeAsset/Meshes` 下 483 个静态网格逐一检查，重绑 21 个网格的 23 个槽，共享植物 / 窗玻璃材质覆盖 49 个网格 | *（2026-09-12 新增）* |
| [`VineObstacleTurning_20260912.md`](VineObstacleTurning_20260912.md) | 藤蔓遇墙洞 / 墙角的转向：旧代码直接改写绝对倾角 ⇒ 相邻段折角可达 100° 以上，改为预算内转向；TG 侧证据 `evidence/ivy-*-20260912.asm` | *（2026-09-12 新增）* |
| [`CSRockShellEdgeBevel.md`](CSRockShellEdgeBevel.md) | 岩壳假倒角：TG 像素层缺口的机制证据与 UE 落地。**运行时壳 = v3**（邻接进 UV + 逐趟披挂重写，走 `M_TG_Texture` 的静态开关 `RockShellBevel`）；**直摆资产 = v2**（顶点色载荷，legacy；消费材质 `M_TinyGladeRockShell` 2026-09-11 已删，重跑构建脚本即重建） | *（2026-09-01 新增，09-04 落地 v3，09-05 重整）* |
| [`TinyGlade_结构审查_附录A_墙体剖面.md`](TinyGlade_结构审查_附录A_墙体剖面.md) | 结构审查的子代理报告 A（深挖 C）：16 行洞曲线消费者对照表、`CLIP_HLSL` 逐语句对照、逐函数的矩形假设清单与折线升级规模；结论已并入 `TinyGlade_结构审查.md` 大问题 3、4 | *（2026-09-07 新增）* |
| [`TinyGlade_结构审查_附录B_GPU基座.md`](TinyGlade_结构审查_附录B_GPU基座.md) | 结构审查的子代理报告 B：GPU 基座与九条打包路的布局对照表、31 处阻塞点计数表、容量 / 确定性契约；结论已并入插件根的 `TinyGlade_结构审查.md` 第 21–25 条 | *（2026-09-07 新增）* |
| [`TinyGlade_结构审查_附录C_生命周期.md`](TinyGlade_结构审查_附录C_生命周期.md) | 结构审查的子代理报告 C（深挖 G）：六个类的状态分类表（权威 / 派生 / 持久记忆 / 拖动期临时 / 跨 actor 登记）、六条生命周期路径矩阵、钩子对称性矩阵、既有结论校核表；结论已并入插件根的 `TinyGlade_结构审查.md` 第 26–30 条并订正第 12 条 | *（2026-09-07 新增）* |
| [`TinyGlade_楼梯逆向_附录D_玩家绘制楼梯.md`](TinyGlade_楼梯逆向_附录D_玩家绘制楼梯.md) | 楼梯逆向子代理报告：§4.1 玩家绘制楼梯的数据模型、踏步算法与常量（50 cm 重采样、块高 60、×1.13、横向切砖）、支撑管线、穿墙洞、栏杆，§4.0 门前踏步的判据与砌法，资产清单与 UE 落地建议；`ACSStairsActor` / `CSStairs.h` 照它实现 | *（2026-09-16 新增）* |
| [`TinyGlade_窗变门逆向_附录E.md`](TinyGlade_窗变门逆向_附录E.md) | 窗变门逆向子代理报告：`snap_balcony_door` 的判据与吸附、`DecoratorSubtype` 派生、门与窗的档位 / 洞 / 件、栏杆与门前踏步判据、门铃 / 花环；`ACSWindowMarker` 门形态照它实现 | *（2026-09-16 新增）* |

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
| [`tiny-glade-house-change-flows.svg`](tiny-glade-house-change-flows.svg) | 地面变化 vs 房子移动各自改什么：一律只标脏、房子自己的 Tick 合批兑现；Colors / Heights 两支分别改门洞与落座 + 柱；接触记录拖动期冻结、松手提交、对端只标脏；楼梯按变更盒过滤。2026-09-18 按代码重画（旧版的 subsystem 分发、中立链接墙 actor、布尔重切都已不存在） | 计划 D3 |
| [`tiny-glade-decor-placement-flow.svg`](tiny-glade-decor-placement-flow.svg) | 摆件／植被放置流程：场在 GPU、异步回读、diff 应用。⚠️ **是 D12「复杂度场」那一半的设计稿、尚未实施**（C2 裁决五：保留自有设计、暂不做）；已落地的锚点层（房子四家 + 地面裙边一家）不在图里，图顶横幅有说明 | 计划 D12 |
| [`tiny-glade-reevaluate-hidden-dataflow.svg`](tiny-glade-reevaluate-hidden-dataflow.svg) | `ReevaluateSite` 的隐式数据流：12 步靠成员变量传中间结果，写 / 读 / 读写逐一标出，顺序约束只在注释里。09-18 按代码更新：新增「② 锚点重表达」列（09-11 版的 ②–⑪ 顺延为 ③–⑫）、接缝一步改为读写 `Contacts` 并由对端从外部写入、加冻结标志总线 | 结构审查 大问题 2 |
| [`tiny-glade-module-interaction.svg`](tiny-glade-module-interaction.svg) | 模块交互图：房子 / 地面 / `UCSHouseLibrary` / 接触记录 / 标记 / 抓手 / 塑形物 / 玩家楼梯 / 编辑器模块 / 遗留藤蔓之间的同步调用、委托广播、世界扫描三种边，圈码对应审查小节，✔ 标出已修的条目；09-18 按代码更新（子系统已删，右上角列出 09-11 以来的结构变化） | 结构审查 深挖 D（8–19） |
| [`tiny-glade-undo-chain.svg`](tiny-glade-undo-chain.svg) | 撤销链路：一次细节面板改 `FootprintSize` 的记录期、`FTransaction::Apply` 四步（`PreEditUndo` → 全部 `Restore` → 组件优先排序 → 逐个 `PostEditUndo`）、插件钩子的扇出（同一次 Ctrl+Z 三次进 `ReevaluateSite`；09-07 时是四次，09-10 起标记那次只置脏），底栏是事务看不见的状态（09-18 补接触记录与 `HouseId` 查重）；09-18 按代码更新行号 | 结构审查 深挖 G（27） |
| [`CSGroundShaper_PrototypeMapping.svg`](CSGroundShaper_PrototypeMapping.svg) | `TinyGlade.hip /obj/geo1` 三条链 → UE 的逐节点对照：塑形物只出高度场（链 A），碎石（链 B → 披挂岩壳）与石阶（链 C → marching squares）都归地面；斜纹底是 08-30 已删的旧石阶路。09-18 按代码核对（旧版把链 B 标成未实现、链 C 还指旧路） | `CSGroundShaper.md` |
| [`CSGroundShaper_Algorithms.svg`](CSGroundShaper_Algorithms.svg) | UE 端怎么替掉 Houdini 的解法，四处都已落地：① 高度剖面（含二次抬升）② 等高线 → 逐格 marching squares ③ 格心路权门控出级 ④ TG 式披挂岩壳（LipOffset 订正为 19.5 cm）；旧石阶路的闭式环半径 / 样条铺装只在面板底部留一行历史。09-18 按代码核对 | `CSGroundShaper.md`、计划 D9 |
| [`CSGroundStairs_Logic.svg`](CSGroundStairs_Logic.svg) | 石阶的 GPU 逻辑：marching squares、100% GPU 决策、零回读；单格早退是四道（09-06 加了矮包与台顶两道门控），S3 = 删旧路。09-18 按代码核对 | 合卷卷零「石阶 S1」 |
| [`CSHouseDoor_Logic.svg`](CSHouseDoor_Logic.svg) | 门洞的当前逻辑：闭环求解、弦长即门宽、转角配成墩；09-18 按斜接折线重画 ③（采样环 = 外皮折线，600×400 时周长 2000、80 个采样点、步长 25），补窗变门走同一张洞表；末栏「已修 / 仍开着」逐条对过代码，示例数字是离线复现 | 计划 D6 |
| [`CSRockShellEdgeBevel_Logic.svg`](CSRockShellEdgeBevel_Logic.svg) | 缺口法线的生成，两条链路并排：左 **v3**（运行时壳，现役：每趟披挂重写 UV1..UV6 → `M_TG_Texture` 静态开关 `RockShellBevel`）、右 **v2**（直摆资产，legacy：顶点色 → `M_TinyGladeRockShell`，09-11 已删、重跑脚本即重建）→ 折痕截面按 v3 默认档画；09-18 按代码核对 | `CSRockShellEdgeBevel.md` |
| [`CSRockShellEdgeBevel_TGNormals.svg`](CSRockShellEdgeBevel_TGNormals.svg) | TG 原版法线流转：逐三角 SSBO 字段，CS 每帧重算写回、PS 按 id 查自己与邻面；末栏「本项目 UE 版」09-18 改为 v3 的自身 + 三邻面法线（UV3..UV6） | `CSRockShellEdgeBevel.md` |
| [`TinyGlade_树冠着色_Pipeline.svg`](TinyGlade_树冠着色_Pipeline.svg) | 树冠着色链路：资产层 → VS 八段 → PS 四段 → G-buffer/深度 → 专用延迟光照；橙块与橙线是贯穿三处的深度浮雕 | `TinyGlade_树冠着色.md` |
| [`TinyGlade_树冠着色_深度浮雕.svg`](TinyGlade_树冠着色_深度浮雕.svg) | 深度浮雕的入门解释：平卡片相交是直线 → 按厚度图谎报深度 → 交线沿叶簇轮廓弯曲；末栏是「只有深度是假的」这条代价 | `TinyGlade_树冠着色.md` |
| [`CSRockShellPattern.preview.png`](CSRockShellPattern.preview.png) | 岩壳原件碎裂图案（TG `rocky_terrain_shell.glb`，609 胞腔） | `CSRockShellPattern.md` |
| [`CSRockShellPattern.fallback.preview.png`](CSRockShellPattern.fallback.preview.png) | 后备生成器的图案（真 Voronoi + Lloyd × 6） | `CSRockShellPattern.md` |
| [`img/TG_continuous_arches.png`](img/TG_continuous_arches.png) | 连续拱之间没有「墙」这个表面的实拍裁决 | 计划 D6、`CSHouseProfile.h` |
| `img/tiny-glade-ref-*.jpg` | 五张 TG 实拍参考：拱间墩／角柱／垛口／双拱墩／深度融合无缝 | 计划 D6/D7 |
| [`img/tiny-glade-ref-overhang-beam-post.png`](img/tiny-glade-ref-overhang-beam-post.png) | 上房悬挑实拍：悬挑底下是斜撑与落地木柱，竖缝处出砖柱 ⇒ **接缝是「接触」的派生之一**。⚠️ 09-17 照反汇编（`construct_rect_bottom_layer`）给上房加的一圈承托梁被用户判「TG 中没有这个结构」已退回；横缝至今只判定、不出产物 | 计划 D7「接触基类与派生」 |
| [`img/house-bottom-face-20260917-off.png`](img/house-bottom-face-20260917-off.png) / [`-on.png`](img/house-bottom-face-20260917-on.png) | 房体底面开关对照：上房 700×460 摞在下房檐口并往 +X 悬挑，从悬挑下往上看 —— 关时直接看进屋里、看见瓦的背面；开时是一整块朝下的灰泥底面 | 计划 D4「房体底面」 |
| `img/tiny-glade-ref-{ground-three-layers,corner-quoin,roof-tiles}.png` | 09-12 那轮的三张 TG 实拍：地面三层 / 转角角石 / 屋面瓦 | `CSGroundMaterial.md`、合卷 |
| `img/{house-audit,ground-material,quoin-reference,roof-reference}-20260912-*.png`、`img/mesh-materials-*.jpg` | 09-12 材质审计的同机位对照（before / after） | `HouseMaterialAudit_20260912.md`、`CSGroundMaterial.md`、合卷、`MeshMaterialRepair.md` |
| [`img/tiny-glade-ref-door-in-arch.png`](img/tiny-glade-ref-door-in-arch.png) | 门在拱里的实拍：轮廓严丝合缝贴着拱圈石内缘 ⇒ **门是比洞更大的矩形，被墙切出剪影**，不是拱形网格 | 卷零「D6 续」③ |
| [`img/tiny-glade-ref-corner-arch-passage.png`](img/tiny-glade-ref-corner-arch-passage.png) | 转角实拍：**两道拱共用一根角柱，没有木门** | 卷零「D6 续」④ |

⚠️ 两张预览 PNG 同时是脚本的**功能性输出路径**（`Scripts/BakeRockShellPattern.py` 与 `Scripts/VerifyRockShellGlb.py` 的 `DEFAULT_PREVIEW_RELPATH`），改名或再次搬动要同步改那两处常量。

## 完成进度速览

⚠️ **权威进度在合卷卷零，本表只是入口摘要。** 卷零的模块状态表 2026-08-31 已回填（此前停在 08-30 23:16），其后逐模块随落地更新；本表 2026-09-18 按源码核对过一遍。

| 模块 | 状态 | 落地位置 |
| --- | --- | --- |
| D1 地面 / D2 顶点色笔刷 / D3 直推通知 | 已落地 | `CSGroundActor.{h,cpp}` |
| D4 房屋 + 屋面（脊向由长轴连续导出、墙-顶收边） | 已落地。⚠️ 早先这一行写的「脊向滞回」已随四坡屋顶于 2026-08-31 删除，见下方 D4 屋面行 | `CSHouseActor.{h,cpp}`、`CSHouseRoof.h` |
| D4 房体底面 | **2026-09-17 落地**：墙内皮围出的那块在房底封上，朝下单面，开关 `bBottomFace`；从房子底下往上看才可见 | `CSHouseActor.cpp`（`CSHouse_BuildBodySoup`） |
| D6 门洞（逐像素 clip + 门框砖） | 已落地；**2026-09-04 重做触发与宽度口径**：门宽 = 路在墙上截出的弦长，过宽切成拱廊 | `CSHouseDoorRuns.h`、`CSHouseFrame.{h,cpp}`、`CSHouseFrame.usf` |
| D6 门扇（`DoorLeafMesh` 一族） | **2026-09-04 新增**：一洞一个静态网格组件，按洞宽缩放 | `CSHouseActor::RebuildDoorLeaves`、`Scripts/TinyGladeSetupDoorLeaf.py` |
| D9 承重柱 + 塑形物（裙边噪声 / 二次抬升 / 披挂岩壳） | 已落地 | `CSGroundShaperActor.cpp`、`CSGroundRockShell.{cpp,usf}` |
| D10 subsystem / D11 Spline 块排布 | D10 **2026-09-16 晚删除**（名单 = `UCSHouseLibrary::GetHouses`，找宿主 / 放窗搬到 `CSHouseLibrary.{h,cpp}`）；D11 已落地 | `CSHouseLibrary.{h,cpp}` |
| D13 藤蔓（枝 319 / 叶 216） | 第一档已落地 | `CSHouseVine.{h,cpp,usf}` |
| D14 渲染与光照 | 观感一轮已落地 | `Scripts/TinyGladeSetupLighting.py` |
| 楼梯 S1 + S2 + S3 | 已落地；S3 = 旧路（塑形物自持的 `BuildStepPlan` / `CSGroundSteps.usf`）已删，`RDG_SmoothSpline` / `SolveBlockLayout` 另有消费者故保留，`CSGroundStairs.usf` 里剩一个无人调用的 `StairInstanceRandom` | `CSGroundStairs.{h,cpp,usf}` |
| 玩家绘制楼梯（TG §4.1） | **2026-09-16 MVP 落地**：`ACSStairsActor` 默认带样条，TG 的 50 cm 重采样出级 + 踏步块底下按层砌砖到地面顶替支撑；拱 / 栏杆 / 梯子 / 穿墙未做。门前踏步（§4.0）挂在窗变成的门上。见 [附录 D](TinyGlade_楼梯逆向_附录D_玩家绘制楼梯.md) | `CSStairs.{h,cpp}`、`CSStairsActor.{h,cpp}` |
| D5 拉尺寸 | **2026-08-31 机制层 + 2026-09-05 交互层，整条已通**：单边推拉纯函数 + `PushEdge` 入口 + `EnterResizeMode` 生成的四个抓手 actor（标准 gizmo 拖，失选自动退出）。尺寸禁带随四坡屋顶同日删除，见 D4 行 | `CSHouseResize.h`、`CSHouseResizeHandleActor.{h,cpp}`、`CSHouseResizeSelectionWatcher.{h,cpp}` |
| D7 接缝（形状相交） | **2026-08-31 落地**纯函数接缝砖；**2026-09-16 改为接触记录**：共享记录两端各持、写入口验、谁动谁发、松手提交、一条缝只砌一次、柱逐接触出（不跨接触合并，数量与接触对应）+ TG 两条过滤；复制出的房子 `HouseId` 查重（09-16 夜）；横缝只判定不出产物（09-17 加的承托梁已被用户判「TG 中没有这个结构」退回） | `CSHouseContact.{h,cpp}`、`CSHouseSeam.h` |
| D7 转角角石（墙自身转角） | **2026-08-31 落地**：四角竖直砖柱，与接缝柱共用 `CSHouseFrame::AppendColumn`。**D7 两半至此都合上** | `CSHouseQuoin.h` |
| D7 包边石（A8） | **2026-08-31 落地**：墙顶压顶 + 墙脚勒脚，共用 `CSHouseFrame::AppendFlatRun`；勒脚按洞切段 | `CSHouseTrim.h` |
| D4 屋面（四坡 + 瓦） | **2026-08-31 落地**：双坡实体板整套删除，屋面全部由瓦铺成。脊向由长轴导出、平局归 X（**用户已裁掉裁决四**） | `CSHouseTile.{h,cpp,usf}` |
| D4 屋面收尾（脊瓦 + 尖顶） | **2026-08-31 落地**：瓦厚 6 cm、排距定档 827 片/栋、五条脊线盖脊瓦、脊端点各立一根尖顶（正方形退化成一根） | `CSHouseTile.cpp`、`CSHouseActor::RebuildRoofFinials` |
| D9 岩壳「石头隆起」 | **2026-08-31 落地**：六个蓝图可调参数（朝外扩张 / 隆起倍数 / 噪波 / 台顶外扩 / 末端上限）。⚠️ **同日晚看图裁决：隆起三参默认归中性**（无界抬升在演示档下读成火山口壁），降级为风格化旋钮 | `CSGroundRockShell.usf` |
| D9 岩壳体积（TG 两层） | **2026-08-31 落地**：`displace:563` 的基准偏移（`BaseLift`/`BaseSink`）+ 表面起伏改「幅度 ∝ 坡度、偏正」。⚠️ `RockShellNoiseAmount` 语义随之变成「坡度 = 1 时的满幅」 | `CSGroundRockShell.usf` |
| 墙面材质（灰泥剥落 + 凸出砖） | **2026-09-03 建图 / 09-04 引擎验收**：TG `_nani_plaster` 的移植，两处有意不同构；09-04 补了 `PeelBias`（顶替 TG 缺失的覆盖度项）与四个默认值改档 | `Scripts/TinyGladeMakeWallMaterials.py`、`M_TinyGladeWall` |
| D8 窗 | **2026-08-31 整个模块合上**：洞与谓词 + `ACSWindowMarker` 交互 actor（射线解析宿主 / 登记诉求 / 换宿主 / 无宿主自毁）。**2026-09-06 交互层重定向**：附属物自带 mesh、锚点是权威、窗不出框砖。**2026-09-16 窗贴墙脚变门**（TG `snap_balcony_door`，形态存锚点，附带门铃 / 花环 / 门前踏步 / 栏杆）。逐条见 [`TinyGladeWindow.md`](TinyGladeWindow.md) | `CSHouseProfile.h`、`CSHouseFeatureMarker.{h,cpp}` |
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
| `Plugins/PCGPlugins/Scripts/TinyGlade*.py` | 81 个演示搭建 / 出图 / 回归脚本（2026-09-18 计数；回归入口 `TinyGladeDemoRegression.py`） | 可执行文件，不是文档 |
| `Plugins/PCGPlugins/Source/ComputeShaderGenerator/` | `CSHouse*` / `CSGround*` 的类注释 | 逐字段口径以源码注释为准，MD 不复述 |
| `/PCGPlugins/HouseTest/` | `L_HouseGroundDemo`、`L_TerrainOpsDemo` 两张演示关卡 | UE 资产 |
| `doc/index.md` | 项目级文档索引 | 已收录本目录 |
| `Plugins/PCGPlugins/TinyGlade_结构审查.md` | 2026-09-06 整体结构审查：编排层复制、隐式数据流与哈希缺口、矩形假设、两套墙体、建议顺序与待拍板；09-06 晚追加模块交互 / 冗余代码两节（8–20），09-07 追加 GPU 基座与打包契约（21–25，子代理报告见本目录附录 B）与生命周期 / 持久化（26–30，子代理报告见本目录附录 C）；09-11 全文复核（复核总览、原结论订正、新增 31–38）；09-18 补记子系统删除与接触记录之后的现状 | 审查结论放插件根，与 `README.md` 并列，首屏可见 |

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
├─ CSGroundMaterial.md                   # 地面三层材质（09-12）
├─ HouseMaterialAudit_20260912.md        # 房屋材质与模型修正（09-12）
├─ MeshMaterialRepair.md                 # 提取网格的材质槽修正（09-12）
├─ VineObstacleTurning_20260912.md       # 藤蔓遇墙洞 / 墙角的转向（09-12）
├─ TinyGlade_树冠着色.md                  # 树冠 shader 侧全链路（几何侧在 build_tree.py / Tree.hip）
├─ TinyGlade_结构审查_附录A_墙体剖面.md    # 结构审查子代理报告 A（深挖 C：墙体 / 剖面契约 / 矩形假设）
├─ TinyGlade_结构审查_附录B_GPU基座.md    # 结构审查子代理报告 B（GPU 基座与打包契约）
├─ TinyGlade_结构审查_附录C_生命周期.md    # 结构审查子代理报告 C（生命周期与持久化；行号是 09-07 的）
├─ TinyGlade_楼梯逆向_附录D_玩家绘制楼梯.md # 楼梯逆向子代理报告（ACSStairsActor 的依据）
├─ TinyGlade_窗变门逆向_附录E.md          # 窗变门逆向子代理报告（窗的门形态的依据）
├─ *.svg                                 # 14 张流程／算法图（逐张见「图索引」）
├─ CSRockShellPattern*.preview.png       # 2 张图案预览（脚本功能性输出路径）
├─ TinyGlade.hip / Tree.hip              # Houdini 原型：地形塑形物 / 树；backup/ 是 TinyGlade.hip 的自动备份
├─ build_tree.py / measure_cards.py      # 在 Houdini 里生成 TG 式树、按资产口径量叶卡朝向
├─ lextab.py / yacctab.py                # pycparser（PLY）自动生成的解析表缓存，不是文档
├─ geo/                                  # 壳与树的源数据：rocky_terrain.json / *.glb / *.FBX；bevel/ 下为倒角烘焙件
├─ out/                                  # build_tree.py 的输出：树与树冠卡片的 .obj / .bgeo.sc、渲染图
├─ evidence/                             # 反汇编 / 着色器摘录等证据（文件名带日期）
└─ img/                                  # 实拍参考 + 出图对照（11 张 TG 参考、1 张裁决图、19 张本项目出图）
```
