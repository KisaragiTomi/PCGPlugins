# 地面派生链的默认值：2026-09-06 调参记录

`ACSGroundActor` 的四条派生链（地被 / 石阶 / 石阶材质 / 岩壳）在 2026-09-06 集中改过一轮。
本文只记**改了什么、从什么改到什么、为什么**，以及改动之间的横向影响。

2026-09-12 的地面三层材质修正另见 [CSGroundMaterial.md](CSGroundMaterial.md)，包含草土深色过渡、噪声土斑、反编译依据与用户参考图。

⚠️ **每个参数自己的语义不在这里** —— 权威是源码声明处的注释（`CSGroundActor.h` 的
`FCSGroundCoverSpecies` 与 `CS Ground|Stairs` / `CS Ground|Rock Shell` 两组）。本文不复述，
只在需要读懂表格时给一句话。

## 岩壳：默认值维持原样，实调值只活在关卡里

用户在 `L_TerrainOpsDemo` 上把岩壳调到满意，当天先把这 9 个值提为出厂默认，**随即又还原**
—— 用户看图判定原来那套默认更好。所以：

- **C++ 默认 = TG 口径那一套，没有变动。**
- **实调值只留在 `L_TerrainOpsDemo` 的 `Ground_Demo` 实例上**，作为那张演示关卡的覆盖。
- `BP_TinyGladeGround` 在这 9 个上**一个都没覆盖**（它只覆盖 `RockShellMaterial` 与
  `RockShellPatternMesh`），所以三层里只有关卡那层有值。

两套值对照（左列是现行默认，右列是演示关卡里的覆盖）：

| 参数 | C++ 默认（现行） | `L_TerrainOpsDemo` 覆盖 | 差异 |
| --- | --- | --- | --- |
| `RockShellPatternScale` | 0.35 | 0.653361 | 胞腔 1.9 m → 3.6 m |
| `RockShellReliefFloor` | 0 | 0.132798 | |
| `RockShellCellExpand` | 12 | 0 | 关掉逐胞腔外扩 |
| `RockShellSlopeLo` | 0.75 | 0.288 | ≈ 37° → 16.1° |
| `RockShellSlopeHi` | 1.25 | 0.662244 | ≈ 51° → 33.5° |
| `RockShellRoadSink` | 160 | 285.7728 | |
| `RockShellCellJitter` | 0 | 0.326267 | |
| `RockShellChipAmount` | 10 | 35 | |
| `RockShellChipWavelength` | 300 | 120 | |

⚠️ **右列的权威是 `.umap`，不是本表**。记在这里只是因为二进制资产里的值没法直接读，
真要用请以关卡为准。字面量是能逐位往返回同一个 `float32` 的最短小数，不是四舍五入。

⚠️ **这一列不要"顺手"提为默认**。真要再提一次，先算 `FCSRockShellContractTest` 的三条不变量
（它是**对着 CDO** 断言的，而这 9 个值里有 4 个在判据里）：

```text
SlopeHi > SlopeLo
RoadSink > CellRelief + NoiseAmount + ChipAmount + BaseLift
1 / RoadFade < StairRoadThreshold
```

实调那套当时是算过的，三条都成立（`ChipAmount` 10 → 35 把第二条右边抬了 25，仍有余量）。
以后动 `Chip*` / `CellRelief` / `NoiseAmount` / `BaseLift` 任意一个，都要重算第二条。

## 地被：草与花

| 参数 | 旧 | 新 | 理由 |
| --- | --- | --- | --- |
| `FCSGroundCoverSpecies::MaxSlopeDegrees` | 55° | **30°** | 55° 让草长到 `tan 55° = 1.43` 的坡上，比岩壳铺满的坡（`SlopeHi = 1.25`）还陡，草直接从石头缝里长出来 |
| `FCSGroundCoverSpecies::bCastShadow` | （新增） | **false** | 用户裁决：草和花不投影。一株一 instance、满密度 50 株/m²，投影是这条路上最贵的一项 |

⚠️ **改 C++ 默认对现有内容是死的**：`BP_TinyGladeGround` 的 `Grass` 结构与 C++ CDO 不同
（`Mesh` 填了 `SM_TG_GrassBlade`），UE 的 delta 序列化把**整个结构**原样写进了资产，包括那个 55。
所以 `MaxSlopeDegrees` 是**两层都改了**才生效的。`bCastShadow` 是新增成员，资产里没有它的标签，
加载时吃构造函数默认值，不需要改资产。

⚠️ 坡度门控是**硬阈值**（`CSGroundCover.usf` 的「4) 坡度门控」直接 `return`），而它上面那条遮罩
门控是**概率拒绝**。阈值挂在 55° 时坡本身少见，那条直边基本看不到；降到 30° 之后它落进常见坡度区。
真被看出来了就把坡度门控也改成概率拒绝，与遮罩共用同一套格身份哈希（边界才不会重扫时闪烁）。

⚠️ 30° 这个值是**按 C++ 默认那套岩壳软阈**（`0.75~1.25` ≈ 37°~51°）定的：草在石头露头之前收干净。
`L_TerrainOpsDemo` 用的是自己那套覆盖（`0.288~0.662` ≈ 16°~33.5°），**壳比草先出现** ——
那张关卡里 16°~30° 这一段草和石头是叠着的。这是关卡侧的取舍，不是默认值的问题。

## 裙边摆件：整条默认关

| 参数 | 旧 | 新 |
| --- | --- | --- |
| `ACSGroundActor::bSkirtDecorEnabled` | true | **false** |

2026-09-06 用户裁决：**摆件只该长在房子周围，不该跟地形隆起有任何关系**。

摆件的锚点层本来就是**五家分产**（`CSHouseDecor.h` 的 `EFamily`）：

| 家族 | 生产者 | 载体 | 现状 |
| --- | --- | --- | --- |
| 门 / 拱两侧 | `CSHouseDecor::BuildAnchors` | `ACSHouseActor` | 在跑 |
| 墙脚 | 同上 | `ACSHouseActor` | 在跑 |
| 檐口 / 屋脊 | 同上 | `ACSHouseActor` | 在跑 |
| 窗 | 同上 | `ACSHouseActor` | **未实现**（窗主体 `ACSWindowMarker` 还没落地，留了空分支） |
| **塑形物裙边** | `CSGroundDecor::BuildSkirtAnchors` | `ACSGroundActor` | **本次关掉** |

所以这条裁决落成一个 bool 就够了：`bSkirtDecorEnabled = false` **只关第五家**，房子那三家在另一个
actor 上、有自己的 `ACSHouseActor::bDecorEnabled`（默认开，`BP_TinyGladeHouse` 里三张网格表与
`DecorMaterial` 都配好了），一点不受影响。

⚠️ **代码没有删**。`CSGroundDecor.h` 那一整套（含它那篇「为什么归地面不归塑形物」的归属论证）
原样留着，网格表与材质也仍旧配在 `BP_TinyGladeGround` 上 —— 这是一个勾就能拿回来的开关。
要真删得连 `EFamily::Skirt`、`FParams` 里那四个 `Skirt*` 旋钮、以及 `ACSGroundActor` 侧的
组件与缓冲一起拆，那是另一件事，**未裁决**。

⚠️ 关卡与 BP 都**没有覆盖**这个 bool，所以改 C++ 默认直接生效 —— 但**运行中的编辑器不会热换模块**，
得重启（或 Live Coding）才看得到。

## 石阶：两条新门控

| 参数 | 默认 | 作用 |
| --- | --- | --- |
| `StairMinMoundHeight` | **60** cm | 塑形物峰高低于它就整座不出台阶；0 = 关掉 |
| `bStairDropTopStep` | **true** | 掐掉每座最顶上那一级（台顶那圈） |

60 = 2 × `StairStepHeight`：一级 30 cm 的落差是个路沿不是楼梯，摆一排石块反而像路障。

两条都**必须按整座塑形物的峰高判，不能按本格高度**：本格高度在高包的裙边同样很小，按它判会把
最低那几级也删掉，而那几级正是人要踩的。峰高 = `LiftHeight × (1 + 二次抬升系数)`，即
`GroundShaperEvalOne` 在台顶（`S → 1`）的取值，**不是裸的 `LiftHeight`**。

落地在两个新的场函数（`CSGroundShaperField.ush`）：

| 函数 | 给谁 | 返回 |
| --- | --- | --- |
| `GroundShaperHeightAtXYTallOnly(P, MinPeak)` | `StairMinMoundHeight` | 只把峰高 ≥ `MinPeak` 的塑形物算进来的高度场 |
| `GroundShaperPeakAtXY(P)` | `bStairDropTopStep` | 本点由哪座说了算，那一座的峰高 |

⚠️ 两个都**只许当门控用**，别拿返回值当地面高度：矮座与高座重叠处它们给的是高座那一份，比真地面低，
拿去解等值线或算块底 Z 会让石阶陷进土里。底下的几何仍旧读完整的 `GroundShaperHeightAtXY`。

⚠️ 顶层号用 `ceil` 不能用 `floor`：跨过的层是 `L·Step < Peak`，峰高正好是层高整数倍时
（例如 120 / 30）`L = 4` 那层因为 `H > Level` 取不到等号本来就不出等值线，真正的顶层是 3，
`floor` 会少掐一级。上限因此压到 `ceil(Peak/Step) − 2`。

## 石阶材质：改走 TG 的 `rocky_terrain`

石阶原本挂 `MI_TinyGladeStone`（常数暖灰 + `stone_floor` 的法线/粗糙度三平面，**没有 albedo 贴图**），
画面上读成塑料灰块、和周围岩壳不是一种石头。**这是配错了贴图，不是灯光或法线的问题。**

出处 `_rocky_terrain_stairs_stairs_indirect.raster.hlsl_*.ps_main.glsl`（104 行，逐行读过）：
TG 的石阶归在 **`rocky_terrain`** 那一族，采的是**和岩壳同一张 `rocky_terrain_texture`**。

| 层 | 旧 | 新 |
| --- | --- | --- |
| `StairMaterial` | `MI_TinyGladeStone` | `MI_TinyGladeStairStep`（`AlbedoMult = 1.2`） |
| `StairPebbleMaterial` | `M_TinyGladeStone` | `MI_TinyGladeStairPebble`（`AlbedoMult = 0.72 = 1.2 × 0.6`） |

母材质 `M_TinyGladeStairs` 由 `Scripts/TinyGladeMakeStairsMaterial.py` 建（幂等，可重跑），
四条常数照抄 TG：周期 1250 cm（TG 的 `× 0.08` 米制口径）、三平面权重 `abs(N)` **一次方不锐化**、
逐实例相位、`× 1.2` / pebble `× 0.6`。⚠️ 周期比岩壳那张细一倍多（岩壳 PS 是 `× 0.0333` = 30 m）
—— TG 有意让小构件平铺更密，**别去对齐岩壳**。

⚠️ 逐实例相位读的是 `PerInstanceRandom + VertexColor.A` 而不是裸的 `PerInstanceRandom`：烘成
StaticMesh 之后没有实例了，`PerInstanceRandom` 恒为 0，整片石阶会塌成同一个相位。两条路互斥
地为零，相加即可（同 `M_TinyGladeStone` 的裁决六 ③）。

⚠️ `M_TinyGladeStone` 本身没动 —— 屋顶尖顶（`TinyGladeSetupRoofTiles.py`）还在用它。

## 待办

- 地被：坡度门控要不要从硬阈值改成概率拒绝（与遮罩门控同一套写法）。
- 石阶材质：粗糙度取的是常数 0.85 —— TG 这条 pass 的 G-buffer 只打包 albedo + normal
  （`out uvec2`），粗糙度不在里面，没有可抄的值。当前阶段材质验收面只有「法线正确 + 颜色贴图
  正确」两条，粗糙度不追。
