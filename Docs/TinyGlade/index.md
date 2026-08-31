# Tiny Glade 复刻：文档索引

本目录集中存放 Tiny Glade 式交互房屋系统（`ACSGroundActor` / `ACSHouseActor` / `ACSGroundShaperActor` 一族）的全部设计、对照与逆向文档。
2026-08-31 从插件根与 `Docs/` 归拢至此，同时把五份对照文档合成一卷、两轮逆向报告合成一卷。

## 从哪读起

- **要改代码** → 先读 [`TinyGladeHouse_Plan.md`](TinyGladeHouse_Plan.md)：唯一的裁决记录，D1–D14 的设计与阶段验收门都在里面。
- **要接着推进** → 读 [`TinyGlade_模块对照与进度.md` 卷零](TinyGlade_模块对照与进度.md#vol-0)：自监督循环的状态文件，含模块状态表、待拍板清单、验收门与「踩过的坑」。
- **想翻某条已定结论** → 先查 [`TinyGlade_对比逆向报告.md`](TinyGlade_对比逆向报告.md) 的「被否条目」与「看似该抄其实不该抄」，别重复推翻。

## 文档清单

| 文件 | 内容 | 合并前 |
| --- | --- | --- |
| [`TinyGladeHouse_Plan.md`](TinyGladeHouse_Plan.md) | 设计裁决主文档：D1–D14、阶段计划 P0–P9、风险、开放问题 | 原在插件根 |
| [`TinyGlade_模块对照与进度.md`](TinyGlade_模块对照与进度.md) | 五卷合卷：完成进度（卷零）+ 四份模块对照（卷一～卷四） | 5 份 |
| [`TinyGlade_对比逆向报告.md`](TinyGlade_对比逆向报告.md) | 两卷合卷：两轮多 agent 对比逆向评审的原始报告 | 2 份 |
| [`CSGroundShaper.md`](CSGroundShaper.md) | 地形塑形物（D9）：Houdini 原型 → UE 的逐节点对照与算法替换 | 原在 `Docs/` |
| [`CSRockShellPattern.md`](CSRockShellPattern.md) | 披挂岩壳（D9 链 B）：烘焙件的通道契约与五条实测订正 | 原在 `Docs/` |

合卷各卷的入口锚点：

| 锚点 | 卷 | 原文件名 |
| --- | --- | --- |
| [`#vol-0`](TinyGlade_模块对照与进度.md#vol-0) | 卷零 · 完成进度与循环协议 | `TinyGlade_模块对照进度.md` |
| [`#vol-1`](TinyGlade_模块对照与进度.md#vol-1) | 卷一 · 拉尺寸（D5）与接缝角柱（D7） | `TinyGlade_拉尺寸与接缝对照.md` |
| [`#vol-2`](TinyGlade_模块对照与进度.md#vol-2) | 卷二 · 窗户（D8）与装饰／藤蔓（D12/D13） | `TinyGlade_窗户与装饰对照.md` |
| [`#vol-3`](TinyGlade_模块对照与进度.md#vol-3) | 卷三 · 楼梯模块 | `TinyGlade_楼梯模块对照.md` |
| [`#vol-4`](TinyGlade_模块对照与进度.md#vol-4) | 卷四 · 渲染与光照（D14） | `TinyGlade_渲染光照对照.md` |
| [`#round-1`](TinyGlade_对比逆向报告.md#round-1) | 卷一 · 第一轮：门洞机制与改进清单 | `TinyGlade_对比逆向报告_第一轮.md` |
| [`#round-2`](TinyGlade_对比逆向报告.md#round-2) | 卷二 · 第二轮：未覆盖维度 | `TinyGlade_对比逆向报告_第二轮.md` |

## 图索引

| 图 | 说明 | 被谁引用 |
| --- | --- | --- |
| [`tiny-glade-house-change-flows.svg`](tiny-glade-house-change-flows.svg) | 地面变化 vs 房子移动各自改什么（D3/D4/D6/D7/D9 的直推链） | 计划 D3 |
| [`tiny-glade-decor-placement-flow.svg`](tiny-glade-decor-placement-flow.svg) | 摆件／植被放置流程：场在 GPU、异步回读、diff 应用 | 计划 D12 |
| [`CSGroundShaper_PrototypeMapping.svg`](CSGroundShaper_PrototypeMapping.svg) | `TinyGlade.hip /obj/geo1` 三条链 → `ACSGroundShaperActor` 的逐节点对照 | `CSGroundShaper.md` |
| [`CSGroundShaper_Algorithms.svg`](CSGroundShaper_Algorithms.svg) | UE 端怎么替掉 Houdini 的解法：四处算法替换 | `CSGroundShaper.md`、计划 D9 |
| [`CSGroundStairs_Logic.svg`](CSGroundStairs_Logic.svg) | 石阶的 GPU 逻辑：marching squares、100% GPU 决策、零回读 | 合卷卷零「石阶 S1」 |
| [`CSRockShellPattern.preview.png`](CSRockShellPattern.preview.png) | 岩壳原件碎裂图案（TG `rocky_terrain_shell.glb`，609 胞腔） | `CSRockShellPattern.md` |
| [`CSRockShellPattern.fallback.preview.png`](CSRockShellPattern.fallback.preview.png) | 后备生成器的图案（真 Voronoi + Lloyd × 6） | `CSRockShellPattern.md` |
| [`img/TG_continuous_arches.png`](img/TG_continuous_arches.png) | 连续拱之间没有「墙」这个表面的实拍裁决 | 计划 D6、`CSHouseProfile.h` |
| `img/tiny-glade-ref-*.jpg` | 五张 TG 实拍参考：拱间墩／角柱／垛口／双拱墩／深度融合无缝 | 计划 D6/D7 |

⚠️ 两张预览 PNG 同时是脚本的**功能性输出路径**（`Scripts/BakeRockShellPattern.py` 与 `Scripts/VerifyRockShellGlb.py` 的 `DEFAULT_PREVIEW_RELPATH`），改名或再次搬动要同步改那两处常量。

## 完成进度速览

⚠️ **权威进度在合卷卷零，本表只是入口摘要。** 卷零最后更新于 2026-08-30 23:16，而 08-31 上午另有四个模块落地 —— 下表已按源码核对补上，卷零本身尚未回填。

| 模块 | 状态 | 落地位置 |
| --- | --- | --- |
| D1 地面 / D2 顶点色笔刷 / D3 直推通知 | 已落地 | `CSGroundActor.{h,cpp}` |
| D4 房屋 + 屋面（脊向滞回、墙-顶三处收边） | 已落地 | `CSHouseActor.{h,cpp}`、`CSHouseRoof.h` |
| D6 门洞（逐像素 clip + 门框砖） | 已落地 | `CSHouseFrame.{h,cpp}`、`CSHouseFrame.usf` |
| D9 承重柱 + 塑形物（裙边噪声 / 二次抬升 / 披挂岩壳） | 已落地 | `CSGroundShaperActor.cpp`、`CSGroundRockShell.{cpp,usf}` |
| D10 subsystem / D11 Spline 块排布 | 已落地 | `CSHouseSubsystem.{h,cpp}` |
| D13 藤蔓（枝 319 / 叶 216） | 第一档已落地 | `CSHouseVine.{h,cpp,usf}` |
| D14 渲染与光照 | 观感一轮已落地 | `Scripts/TinyGladeSetupLighting.py` |
| 楼梯 S1 + S2 + S3 | 已落地，**旧路已删干净** | `CSGroundStairs.{h,cpp,usf}` |
| D5 拉尺寸 | **2026-08-31 落地**：单边推拉 + 尺寸禁带（纯函数层，无交互控件） | `CSHouseResize.h` |
| D7 接缝 | **2026-08-31 落地**：纯函数接缝砖，洞走 clip 不挖真几何 | `CSHouseSeam.h` |
| D8 窗 | **2026-08-31 落地洞与谓词**；`ACSWindowMarker` 交互 actor 仍未写 | `CSHouseProfile.h` |
| D12 摆件 | **2026-08-31 落地锚点层**（五家锚点）；复杂度场那一半按 C2 有意未做 | `CSHouseDecor.{h,cpp,usf}`、`CSGroundDecor.{h,cpp}` |

⛳ **待用户拍板只剩一条**：门洞触发规则（本项目道路驱动 vs TG 与道路无关）。其余九条已全部关闭，逐条结论见卷零「待用户拍板」。

## 证据来源

按可信度排列。解 PDB 系统签名的用法与逐条取舍见卷零同名小节。

| 来源 | 路径 | 说明 |
| --- | --- | --- |
| PDB 符号 | `D:/MyProject/Tiny Glade/tmp/pdb_symbols.txt` | 97033 条，**最硬的证据** |
| 逆向分析 | `D:/MyProject/Tiny Glade/MESH_GENERATION_ANALYSIS.md` | 461 行，自带 `【确凿】/【推测】/【待确认】` 标注 |
| 反编译着色器 | `D:/MyProject/Tiny Glade/tmp/shaders` | GLSL 实证 |
| 提取资产 | `Content/TinyGlade/`（`/Game/TinyGlade`） | 474 个 StaticMesh，尺寸一律实测不猜 |
| Houdini 原型 | `D:/MyProject/Houdini/TinyGlade/TinyGlade.hip` | 用户的意图原型，对照写法见 `CSGroundShaper.md` |
| 两轮对照评审 | [`TinyGlade_对比逆向报告.md`](TinyGlade_对比逆向报告.md) | 含被否条目，别重复推翻 |
| 模块对照 | [`TinyGlade_模块对照与进度.md`](TinyGlade_模块对照与进度.md) 卷一～卷四 | 本循环自产，被卷零引用的部分已经驱动方复核 |

⚠️ 四卷各自的「证据标注约定」**没有合并**：口径逐卷不同（证据种类、易误采提醒、轴向换算都不一样），压成一张表会丢信息。读某一卷时以该卷自己的约定为准。

## 相关但不在本目录

| 位置 | 内容 | 为什么不在这里 |
| --- | --- | --- |
| `Plugins/PCGPlugins/Scripts/TinyGlade*.py` | 55 个演示搭建 / 出图 / 回归脚本 | 可执行文件，不是文档 |
| `Plugins/PCGPlugins/Source/ComputeShaderGenerator/` | `CSHouse*` / `CSGround*` 的类注释 | 逐字段口径以源码注释为准，MD 不复述 |
| `/PCGPlugins/HouseTest/` | `L_HouseGroundDemo`、`L_TerrainOpsDemo` 两张演示关卡 | UE 资产 |
| `doc/index.md` | 项目级文档索引 | 已收录本目录 |

## 目录布局

```text
Docs/TinyGlade/
├─ index.md                              # 本文件
├─ TinyGladeHouse_Plan.md                # 设计裁决主文档
├─ TinyGlade_模块对照与进度.md            # 合卷：卷零进度 + 卷一～卷四对照
├─ TinyGlade_对比逆向报告.md              # 合卷：两轮逆向评审
├─ CSGroundShaper.md                     # 地形塑形物实现文档
├─ CSRockShellPattern.md                 # 披挂岩壳烘焙件契约
├─ *.svg                                 # 5 张流程／算法图
├─ CSRockShellPattern*.preview.png       # 2 张图案预览（脚本功能性输出路径）
└─ img/                                  # 实拍参考图（1 张裁决图 + 5 张 TG 参考）
```
