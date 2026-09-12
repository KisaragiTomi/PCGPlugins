# -*- coding: utf-8 -*-
"""
建 TinyGlade 墙体的两个材质 + 墙面的可调 MI。

M_TinyGladeWall（Masked）  —— 洞由 OpacityMask 判据逐像素 discard 切出；
    **墙面 = 灰泥，砖 = 贴图 + 遮罩**（2026-09-03 加，见下面「灰泥剥落」一节）。
MI_TinyGladeWall           —— 上面那张的实例，所有贴图/手感参数都在这里调。房子挂的是它。
M_TinyGladeRoof（Opaque） —— 屋面。

（M_TinyGladeReveal 已于 2026-09-11 删除：它原是洞口内壁，内壁作废后借作承重柱材质；柱子后来
    由 TinyGladeSetupPillar.py 改挂 M_TinyGladeBrick，它就没人引用了。本脚本因此不再建它、也不再
    写 PillarMaterial —— 柱材质归 TinyGladeSetupPillar.py 管，这里再写会把它改回去。）

⚠️ OpacityMask 里那段 HLSL 是 CSHouseProfile.h 里 CSHouse_ClipKeeps() 的逐字翻译。
   两处不一致就会出现"CPU 谓词说能放、画面上切穿帮"，而且不会报任何错。改一处必须改两处。

-------------------------------------------------------------------------------
灰泥剥落（TG `_nani_plaster.raster.hlsl` 的移植，两处**故意不同构**）
-------------------------------------------------------------------------------
TG 那边：砖实例铺满整面墙 → 灰泥是**另一张网格**盖在外面 → PS 用噪声 alpha-test `discard`
把灰泥挖穿，洞里看见的是**真砖**。

本项目：墙板本体**就是**灰泥（`CSHouse_BuildBodySoup` 的实心盒），灰泥背后什么都没有——
照抄 `discard` 只会穿到屋里。所以：

  **不同构 ①：输出端从 discard 换成贴图混合。** 侵蚀场逐字照抄（`Peel*0.5 + fbm` 与
  `smoothstep(0, 4m, 高度)` 的比较，TG ps_main:262-268/379），但越过阈值不是丢像素，
  而是把 albedo / normal / roughness 换成砖那套贴图。

  **不同构 ②：噪声域从 uv 换成世界 3D。** TG 的 uv 沿整面墙连续；本项目 `AddQuad` 的 UV0
  **每块面板从 0 重开**（`CSHouseActor.cpp:114-120`），照抄 uv 域会让每块面板长出一模一样的
  剥落图案。改用世界坐标 3D value noise（正是 TG 砖 VS 抖顶点用的那种），跨面板连续、零重复。
  高度梯度仍然用 UV0.y —— 它对主面板恰好就是"离墙脚多高"（`AddPanel` 的 V = Up*(H-Z0)）。

三条 mask，两种边缘处理（用户 2026-09-03 裁决）：

  · **剥落露砖** `peelM`：侵蚀场越过高度阈值 ⇒ 灰泥掉了。**要表现厚度** ——
    边上留一条 `band`，albedo 褪到砂浆色（TG 的 `_847` 过渡带），法线沿侵蚀场的切空间梯度
    翻出一道卷边（TG 的 `_851` + ps_main:1034），读作"灰泥有一层厚度，这里是它的断面"。
  · **凸出砖** `prot`：一部分砖**恒高于灰泥面**，从灰泥里顶出来。做法是给灰泥定一条"液面"
    （基准 − 低频起伏 − 靠近剥落边的变薄），砖的高度图超过液面就露头 —— 纯高度阈值，
    轮廓必然是整块石头的顶面。**不表现厚度** —— 它不是灰泥掉了，是砖本来就高出去，
    所以 `band *= (1 - prot)`。
  · 最终 `BrickMask = max(peelM, prot)`。

砖贴图不是"砖纹"而是**色卡 × 石板图案**的组合：插件里没有带砌体花纹的砖贴图，
`brick_colors_*` 是 TG 的斑驳色卡（TG 的砖是真几何，贴图只染色），唯一带砌体图案的是
`stone_floor_2_*`（成排圆角块）。两张都是 MI 参数，换成真砖贴图只需要在 MI 上改。
（顺带核过一次：`bricks_beige/dark/grey/light/yellow`、`plaster_peel_low/medium/high`、
`nani_solid_albedo_layer00..08`、`wall` 这些名字很像的贴图**全是 32×32 的 UI 图标**，
不是可用的表面贴图 —— 别再去翻一遍。）

-------------------------------------------------------------------------------
色卡是「逐房配色选项」，不是「那张灰泥贴图」（2026-09-04 订正）
-------------------------------------------------------------------------------
`plaster_colors_layer00..07` 与 `brick_colors_layer00..04` 是 TG 让玩家逐房选的**配色档**：
P00 三文鱼粉 / P01 赭黄 / P02 灰蓝 / P03 炭灰 / **P04 淡灰绿奶白** / P05 鼠尾草绿 /
P06 浅褐 / P07 藕粉；B00 米黄带红纹 / **B01 淡奶黄** / B02 墨橄榄 / B03 橄榄黄 / B04 灰藕。

2026-09-03 挑的是「layer00」，那只是**下标 0**，不是"默认那档"。在关卡光照下（天光下半球
是草地绿 `(0.10,0.14,0.055)`，D14 有意为之）P00 读成暗红褐、砖读成橄榄鳞片。改成
参照图 `Docs/TinyGlade/img/tiny-glade-ref-corner-pillar.jpg` 里那一档：**灰泥 P04 + 砖 B01**。

⚠️ 这是**默认值**，不是结论：换一档只要在 MI 上改 `PlasterColor` / `BrickColor` 一个参数。
将来逐房差异应当走顶点色 `Color.A` 色号 + 1D 调色板（合卷「逐房差异」那条 finding），
不是给每栋房造 MID。
"""
import unreal

PKG = "/PCGPlugins/HouseTest"
TEX = "/PCGPlugins/HouseTest/TinyGladeAsset/Textures"
MEL = unreal.MaterialEditingLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()

# 本次建图真正创建出来的参数名（`scalar` / `vector` / `texparam` 三个小写手各自登记）。
# `prune_orphan_overrides` 拿它当 MI 覆盖的存在性判据 —— 见那个函数的 docstring。
WALL_PARAMS = set()

# CSHouse_ClipKeeps() 的逐字翻译 + 洞缘噪声：返回 1 = 保留，0 = discard。
#
# ⚠️ 噪声推的是**判据的阈值**，不是 q。推 q 等于把整个洞揉变形（洞心也跟着漂、还会
# 和砖层裁出来的剪影对不上）；推阈值只让边界起毛，洞心恒是洞、洞外恒是墙。
#
# ⚠️ **单边：只许把洞啃小，不许推大**（`t = 1 - Amp*v`，v ∈ [0,1]）。窗的预制框正好盖住
# **标称**洞口，洞一旦被噪声推大就会在框外露出裁剪断口 —— 画面上是一条直接看穿墙的缝。
# 往里啃则是灰泥盖过洞缘一点点，正是 TG 那种灰泥剥落压边的观感。
#
# ⚠️ 噪声取样走**世界投影**（与砖纹同一套轴选择），不是 q 空间：q 是按洞归一化的，
# 拿它当噪声域会让大洞小洞的毛边看起来一样密，而且洞一改尺寸整圈毛边就重排。
CLIP_HLSL = """
int shape = (int)(Shape * 255.0f + 0.5f);
if (shape > 2) return 1.0f;   // 255 = 这块面板没有洞（也兜住任何非法值，与原判据同义）

float2 p = Q;

float t = 1.0f;
if (Amp > 0.0f)
{
    float3 an = abs(N);
    float2 w;
    if (an.z > max(an.x, an.y)) w = float2(WP.x, WP.y);
    else if (an.x > an.y)       w = float2(WP.y, WP.z);
    else                        w = float2(WP.x, WP.z);
    w *= Freq;

    float v = 0.0f, a = 1.0f, norm = 0.0f;
    for (int o = 0; o < 2; ++o)   // 两个八度：边界上看得见的只有低频那层，第二层负责去掉格子感
    {
        float2 i0 = floor(w), f0 = frac(w);
        f0 = f0 * f0 * (3.0f - 2.0f * f0);
        float2 h = float2(127.1f, 311.7f);
        float n00 = frac(sin(dot(i0,               h)) * 43758.5453f);
        float n10 = frac(sin(dot(i0 + float2(1,0), h)) * 43758.5453f);
        float n01 = frac(sin(dot(i0 + float2(0,1), h)) * 43758.5453f);
        float n11 = frac(sin(dot(i0 + float2(1,1), h)) * 43758.5453f);
        v += a * lerp(lerp(n00, n10, f0.x), lerp(n01, n11, f0.x), f0.y);
        norm += a;
        a *= 0.5f;
        w *= 2.03f;   // 非整数倍率，免得两层的格子对齐出十字纹
    }
    t = 1.0f - Amp * (v / norm);
}

bool inside;
if (shape == 1)       inside = max(abs(p.x), abs(p.y)) < t;                          // Rect
else if (shape == 2)  inside = dot(p, p) < t * t;                                    // Circle
else                  inside = (p.y <= 0.0f) ? (abs(p.x) < t) : (dot(p, p) < t * t); // Arch (shape == 0)
return inside ? 0.0f : 1.0f;
"""

# 世界空间墙面投影。UV0 每块面板从 0 重开（见文件头「不同构 ②」），砖纹照它走会在每个洞
# 两侧断掉；改用世界坐标投影，沿一面墙连续，只在 90° 转角换轴 —— 那处本来就压着角石。
# 除以 200 是 CSHouse_UVScale，所以 tiling = 1 时纹理周期正好 2 m。
PROJ_HLSL = """
float3 an = abs(N);
float2 p;
if (an.z > max(an.x, an.y)) p = float2(WP.x, WP.y);   // 墙顶 / 墙底
else if (an.x > an.y)       p = float2(WP.y, WP.z);   // 法线朝 X 的那两面
else                        p = float2(WP.x, WP.z);
return p * 0.005f;
"""

# 侵蚀场 + 三条 mask。返回 float4(BrickMask, Band, LipX, LipY)。
# 噪声写成三重内联循环而不是 helper 函数：Custom 节点的代码被塞进一个函数体里，
# 里面不能再声明函数。
PEEL_HLSL = """
float3 T = Parameters.TangentToWorld[0];
float3 B = Parameters.TangentToWorld[1];

// 三个采样点：中心 + 切/副切方向各偏 GradEps 厘米，用来求场的梯度（卷边法线要它）。
float3 Off[3];
Off[0] = float3(0.0f, 0.0f, 0.0f);
Off[1] = T * GradEps;
Off[2] = B * GradEps;

float NS = max(NoiseScale, 1e-4f) * 0.005f;   // 世界 cm → 噪声晶格；1 ⇒ 2 m，2 ⇒ 1 m（TG 口径）
float Fld[3];
for (int s = 0; s < 3; ++s)
{
    float3 P = (WP + Off[s]) * NS;
    float amp = 0.5f;
    float acc = 0.0f;
    for (int o = 0; o < 3; ++o)                // 3 倍频，与 TG ps_main 的 fbm 同构
    {
        float3 i0 = floor(P);
        float3 fr = P - i0;
        fr = fr * fr * (3.0f - 2.0f * fr);
        float v = 0.0f;
        for (int c = 0; c < 8; ++c)            // 8 角哈希三线性插值
        {
            float3 d = float3(c & 1, (c >> 1) & 1, (c >> 2) & 1);
            uint3 q = (uint3)((int3)(i0 + d) + 65536);   // +65536：负坐标转 uint 前挪正
            uint h = q.x * 1597334673u ^ q.y * 3812015801u ^ q.z * 2798796415u;
            h ^= h >> 15; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
            float w = lerp(1.0f - fr.x, fr.x, d.x) * lerp(1.0f - fr.y, fr.y, d.y) * lerp(1.0f - fr.z, fr.z, d.z);
            v += w * (float(h) * 2.3283064e-10f);
        }
        acc += amp * (v * 2.0f - 1.0f);
        amp *= 0.55f;
        P *= 1.9f;
    }
    Fld[s] = acc;
}

// ---- TG 的侵蚀场四项（ps_main:262-268）。Coverage 那项默认关：顶点色 A 现在恒为 0
//      （CSHouse_Semantic 的保留位），直接读会把整面墙判成剥光。
float cov  = lerp(1.0f, saturate(Coverage), saturate(UseCoverage));
// PeelBias 是**本项目独有的一项**，TG 那边没有对位物 —— 它顶替的是 TG 的覆盖度项。
// TG 的灰泥是一张只铺在部分墙面上的独立网格，`cov` 在每块灰泥的边缘自然衰减，
// 于是"哪儿是灰泥、哪儿不是"由网格决定，噪声只负责啃边。本项目墙板本体就是灰泥、
// 铺满整面墙，`cov ≡ 1`，噪声一个人说了算 ⇒ 照抄 TG 的 bias 会让墙脚剥掉六成
// （2026-09-04 引擎实测：墙脚砖占 66%、半墙高 43%），与计划 §327「墙面本身是灰泥、
// 砖只出现在三处」正好相反。PeelBias 把整条门槛整体抬起来，补回那份缺失的覆盖度。
float bias = PeelBias + Peel * 0.5f + smoothstep(0.0f, 1.0f, 1.0f - cov) * CoverageWeight;
float hf   = smoothstep(0.0f, max(HeightFade, 1e-3f), WallH);   // 墙脚 0 → 墙上 1
float k    = lerp(2.0f, 2.8f, saturate(Peel * 2.0f));

// d > 0 ⇒ 灰泥已掉。判据逐字是 TG 的 `E*mix(2,2.8,peel) > 2*smoothstep(0,4,uv.y)`。
float d0 = (bias + Fld[0]) * k - hf * 2.0f;
float d1 = (bias + Fld[1]) * k - hf * 2.0f;
float d2 = (bias + Fld[2]) * k - hf * 2.0f;

// **场值 → 世界厘米**：除以场在单位长度上的变化率。少了这一步，断面带的宽度会随噪声的
// 陡缓忽宽忽窄（噪声平缓处糊成一大片）；换算之后 BandWidth / PeelSoftness 都是厘米，
// BandWidth 就直接读作"这层灰泥有多厚"。
float2 g   = float2(d1 - d0, d2 - d0);
float gl   = max(length(g), 1e-6f);
float dist = d0 * (GradEps / gl);                 // 正 = 砖那侧
float peelM = smoothstep(-max(PeelSoftness, 1e-3f), max(PeelSoftness, 1e-3f), dist);

// ---- 凸出砖：灰泥"液面"= 基准 − 低频起伏 − 靠近剥落边的变薄；石头高过液面就顶出来。
// 纯高度阈值 ⇒ 轮廓永远是整块石头的顶面，既不会切出月牙、也不会落进砖缝。
// （⚠️ 别改回"用 seed 图逐格选砖"：stone_floor_2 的 seed 分格与 height 的石块**对不齐**，
//   实测选出来的是格子边框，画面上是一圈圈月牙。）
float level = ProtrudeLevel - Fld[0] * ProtrudeVar
            - smoothstep(-max(ProtrudeRange, 1e-3f), 0.0f, dist) * ProtrudeThin;
float prot = saturate(smoothstep(level, level + max(ProtrudeSoftness, 1e-3f), BrickH) * (1.0f - peelM));

// ---- 灰泥断面带：只属于剥落边。凸出砖不是"灰泥掉了"，不给厚度（用户裁决）。
float band = (1.0f - peelM) * smoothstep(-max(BandWidth, 1e-3f), 0.0f, dist) * (1.0f - prot);

// ---- 卷边法线：场的梯度指向砖那侧 = 台阶朝下的方向，法线就该往那边倒。
float2 lip = (g / gl) * (band * LipStrength);

return float4(saturate(max(peelM, prot)), band, lip.x, lip.y);
"""

# 高度图存得很暗（stone_floor_2 的 R 通道峰值只有 0.30），直接用的话砖缝几乎没有明暗差、
# 凸出砖的阈值也全挤在低位。先归一化一次，两个消费者（侵蚀场 / albedo）共用同一份。
HEIGHT_HLSL = """
return saturate(H * Gain);
"""

ALBEDO_HLSL = """
float3 plaster = Plaster * PlasterTint;
float3 mortar  = Mortar * MortarTint;
float3 brick   = Brick * BrickTint * lerp(GrooveDarken, 1.0f, BrickH);
return lerp(lerp(plaster, mortar, Band), brick, Mask);
"""

NORMAL_HLSL = """
float3 n = normalize(lerp(NPlaster, NBrick, Mask));
n += float3(Lip, 0.0f);       // 卷边：灰泥断面朝砖那侧倒
return normalize(n);
"""


# ---------------------------------------------------------------------------
# 建图小写手
# ---------------------------------------------------------------------------
def make_or_load(name):
    """已存在就清空表达式原地重建 —— **不要 delete_asset**：关卡与 BP 引用的是资产路径，
    删掉再建虽然路径相同，但会把当前已加载的引用打成空。

    ⚠️ **`delete_all_material_expressions` 一次只删一趟**（2026-09-03 实测：84 个节点调一次
    只剩 41 个）。少了这个循环，重跑一次就把两代节点叠在一张图里：连线是新的、画面看着对，
    但**参数表是两代的并集** —— MI 面板上会冒出上一版的 `BandWidth` / `ProtrudeLow`，
    同名参数还会读到旧那份的默认值（实测 PeelAmount 报 0.35 而图上连的是 0.25）。
    没有任何报错，只有 MI 面板能看出来。"""
    path = "%s/%s" % (PKG, name)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        mat = unreal.EditorAssetLibrary.load_asset(path)
        for _ in range(16):
            if MEL.get_num_material_expressions(mat) == 0:
                break
            MEL.delete_all_material_expressions(mat)
        left = MEL.get_num_material_expressions(mat)
        if left:
            raise RuntimeError("%s: %d expressions survived the purge" % (name, left))
        return mat
    return TOOLS.create_asset(name, PKG, unreal.Material, unreal.MaterialFactoryNew())


def node(mat, cls, x, y):
    return MEL.create_material_expression(mat, cls, x, y)


def const1(mat, v, x, y):
    e = node(mat, unreal.MaterialExpressionConstant, x, y)
    e.set_editor_property("r", v)
    return e


def const3(mat, r, g, b, x, y):
    e = node(mat, unreal.MaterialExpressionConstant3Vector, x, y)
    e.set_editor_property("constant", unreal.LinearColor(r, g, b, 1.0))
    return e


def scalar(mat, name, v, group, prio, x, y):
    e = node(mat, unreal.MaterialExpressionScalarParameter, x, y)
    WALL_PARAMS.add(name)
    e.set_editor_property("parameter_name", name)
    e.set_editor_property("default_value", v)
    e.set_editor_property("group", group)
    e.set_editor_property("sort_priority", prio)
    return e


def vector(mat, name, r, g, b, group, prio, x, y):
    e = node(mat, unreal.MaterialExpressionVectorParameter, x, y)
    WALL_PARAMS.add(name)
    e.set_editor_property("parameter_name", name)
    e.set_editor_property("default_value", unreal.LinearColor(r, g, b, 1.0))
    e.set_editor_property("group", group)
    e.set_editor_property("sort_priority", prio)
    return e


def texparam(mat, name, tex_name, sampler, group, prio, x, y):
    e = node(mat, unreal.MaterialExpressionTextureSampleParameter2D, x, y)
    WALL_PARAMS.add(name)
    e.set_editor_property("parameter_name", name)
    e.set_editor_property("group", group)
    e.set_editor_property("sort_priority", prio)
    e.set_editor_property("sampler_type", sampler)
    tex = unreal.EditorAssetLibrary.load_asset("%s/%s" % (TEX, tex_name))
    if tex is None:
        raise RuntimeError("texture missing: %s/%s" % (TEX, tex_name))
    e.set_editor_property("texture", tex)
    return e


def custom(mat, title, code, inputs, out_type, x, y):
    e = node(mat, unreal.MaterialExpressionCustom, x, y)
    e.set_editor_property("code", code)
    e.set_editor_property("output_type", out_type)
    e.set_editor_property("description", title)
    ins = []
    for n in inputs:
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", n)
        ins.append(ci)
    e.set_editor_property("inputs", ins)
    return e


def mask(mat, src, r, g, b, a, x, y):
    e = node(mat, unreal.MaterialExpressionComponentMask, x, y)
    e.set_editor_property("r", r)
    e.set_editor_property("g", g)
    e.set_editor_property("b", b)
    e.set_editor_property("a", a)
    MEL.connect_material_expressions(src, "", e, "")
    return e


def mul(mat, a, a_out, b, b_out, x, y):
    e = node(mat, unreal.MaterialExpressionMultiply, x, y)
    MEL.connect_material_expressions(a, a_out, e, "A")
    MEL.connect_material_expressions(b, b_out, e, "B")
    return e


def lerp3(mat, a, b, alpha, x, y):
    e = node(mat, unreal.MaterialExpressionLinearInterpolate, x, y)
    MEL.connect_material_expressions(a, "", e, "A")
    MEL.connect_material_expressions(b, "", e, "B")
    MEL.connect_material_expressions(alpha, "", e, "Alpha")
    return e


# ---------------------------------------------------------------------------
# 数据贴图的 sRGB：高度 / 粗糙度 / seed 是数据不是颜色，导进来时都带着 sRGB=true。
# 不改的话阈值全落在 gamma 曲线上（不会报错，只是手感对不上参数）。
# ---------------------------------------------------------------------------
def fix_data_textures():
    fixed = []
    for n in ("stone_floor_2_height", "stone_floor_2_roughness"):
        t = unreal.EditorAssetLibrary.load_asset("%s/%s" % (TEX, n))
        if t and t.get_editor_property("srgb"):
            t.set_editor_property("srgb", False)
            unreal.EditorAssetLibrary.save_loaded_asset(t)
            fixed.append(n)
    return fixed


# ---------------------------------------------------------------------------
def build_wall():
    mat = make_or_load("M_TinyGladeWall")
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    # 双面会露出墙板背面成黑洞。断口由门框砖填满（内壁那条路已作废），所以这里必须保持单面。
    mat.set_editor_property("two_sided", False)

    # ---- 世界位置 / 法线：洞缘噪声与世界投影 UV **共用**这两个源 ----
    # （建在洞之前是因为下面的 clip 要用；两处各建一份只是浪费节点。）
    wpos = node(mat, unreal.MaterialExpressionWorldPosition, -2400, -600)
    vnrm = node(mat, unreal.MaterialExpressionVertexNormalWS, -2400, -470)

    # ---- 洞 ----
    uv1 = node(mat, unreal.MaterialExpressionTextureCoordinate, -2000, 900)
    uv1.set_editor_property("coordinate_index", 1)   # UV1 = 解析裁剪场 q
    vcol = node(mat, unreal.MaterialExpressionVertexColor, -2000, 1050)
    # 0.045：78 cm 的窗上约合 1.8 cm 的毛边，肉眼看得出不是直线、又远小于预制框的覆盖余量。
    p_hole_amp = scalar(mat, "HoleEdgeNoise", 0.045, "00 Hole", 0, -2000, 1200)
    # 0.35 ⇒ 世界周期约 2.9 m 的低频 + 第二个八度，毛边尺度落在砖长量级。
    p_hole_freq = scalar(mat, "HoleEdgeNoiseScale", 0.35, "00 Hole", 1, -2000, 1280)
    clip = custom(mat, "TinyGladeArchClip", CLIP_HLSL, ["Q", "Shape", "WP", "N", "Amp", "Freq"],
                  unreal.CustomMaterialOutputType.CMOT_FLOAT1, -1700, 950)
    MEL.connect_material_expressions(uv1, "", clip, "Q")
    MEL.connect_material_expressions(vcol, "B", clip, "Shape")
    MEL.connect_material_expressions(wpos, "", clip, "WP")
    MEL.connect_material_expressions(vnrm, "", clip, "N")
    MEL.connect_material_expressions(p_hole_amp, "", clip, "Amp")
    MEL.connect_material_expressions(p_hole_freq, "", clip, "Freq")
    MEL.connect_material_property(clip, "", unreal.MaterialProperty.MP_OPACITY_MASK)

    # ---- 世界投影 UV ----
    proj = custom(mat, "TGWallProjUV", PROJ_HLSL, ["WP", "N"],
                  unreal.CustomMaterialOutputType.CMOT_FLOAT2, -2150, -540)
    MEL.connect_material_expressions(wpos, "", proj, "WP")
    MEL.connect_material_expressions(vnrm, "", proj, "N")

    p_brick_tile = scalar(mat, "BrickTiling", 1.0, "01 Brick", 0, -2150, -380)
    # 0.2 ⇒ 世界周期 10 m，一面 6 m 的墙只看得到一整块渐变，灰泥贴图等于没采。0.5 ⇒ 4 m。
    p_plas_tile = scalar(mat, "PlasterTiling", 0.5, "02 Plaster", 0, -2150, -300)
    p_mort_tile = scalar(mat, "MortarTiling", 0.4, "02 Plaster", 1, -2150, -220)
    uv_brick = mul(mat, proj, "", p_brick_tile, "", -1900, -420)
    uv_plas = mul(mat, proj, "", p_plas_tile, "", -1900, -300)
    uv_mort = mul(mat, proj, "", p_mort_tile, "", -1900, -180)

    # ---- 贴图（全是 MI 参数）----
    t_brick = texparam(mat, "BrickColor", "brick_colors_layer01",
                       unreal.MaterialSamplerType.SAMPLERTYPE_COLOR, "01 Brick", 1, -1650, -900)
    t_brickn = texparam(mat, "BrickNormal", "stone_floor_2_normal",
                        unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL, "01 Brick", 2, -1650, -700)
    t_brickh = texparam(mat, "BrickHeight", "stone_floor_2_height",
                        unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR, "01 Brick", 3, -1650, -500)
    t_plas = texparam(mat, "PlasterColor", "plaster_colors_layer04",
                      unreal.MaterialSamplerType.SAMPLERTYPE_COLOR, "02 Plaster", 2, -1650, -100)
    t_plasn = texparam(mat, "PlasterNormal", "default_normal",
                       unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL, "02 Plaster", 3, -1650, 100)
    t_mort = texparam(mat, "MortarColor", "mortar",
                      unreal.MaterialSamplerType.SAMPLERTYPE_COLOR, "02 Plaster", 4, -1650, 300)
    for s, uv in ((t_brick, uv_brick), (t_brickn, uv_brick), (t_brickh, uv_brick),
                  (t_plas, uv_plas), (t_plasn, uv_plas), (t_mort, uv_mort)):
        MEL.connect_material_expressions(uv, "", s, "UVs")

    p_gain = scalar(mat, "BrickHeightGain", 3.4, "01 Brick", 8, -1400, -430)
    brick_h = custom(mat, "TGBrickHeight", HEIGHT_HLSL, ["H", "Gain"],
                     unreal.CustomMaterialOutputType.CMOT_FLOAT1, -1200, -470)
    MEL.connect_material_expressions(t_brickh, "R", brick_h, "H")
    MEL.connect_material_expressions(p_gain, "", brick_h, "Gain")

    # ---- 侵蚀场 ----
    uv0 = node(mat, unreal.MaterialExpressionTextureCoordinate, -1650, 500)
    uv0.set_editor_property("coordinate_index", 0)
    wall_h = mask(mat, uv0, False, True, False, False, -1400, 500)   # UV0.y = 离墙脚多高 / 200cm

    peel_args = [
        # 03 Peel：PeelSoftness / BandWidth / ProtrudeRange 的单位是**世界厘米**
        ("Peel", scalar(mat, "PeelAmount", 0.25, "03 Peel", 0, -1400, 620)),
        # 见 PEEL_HLSL 里 PeelBias 那段注释：负值 = 抬高整条剥落门槛，补 TG 的覆盖度项。
        ("PeelBias", scalar(mat, "PeelBias", -0.30, "03 Peel", 1, -1400, 655)),
        ("NoiseScale", scalar(mat, "NoiseScale", 3.0, "03 Peel", 1, -1400, 690)),
        ("HeightFade", scalar(mat, "HeightFade", 2.0, "03 Peel", 2, -1400, 760)),
        ("PeelSoftness", scalar(mat, "PeelSoftness", 0.6, "03 Peel", 3, -1400, 830)),
        ("BandWidth", scalar(mat, "PlasterThickness", 2.0, "03 Peel", 4, -1400, 900)),
        ("LipStrength", scalar(mat, "LipStrength", 1.1, "03 Peel", 5, -1400, 970)),
        ("GradEps", scalar(mat, "GradEps", 2.0, "03 Peel", 6, -1400, 1040)),
        ("UseCoverage", scalar(mat, "UseVertexCoverage", 0.0, "03 Peel", 7, -1400, 1110)),
        ("CoverageWeight", scalar(mat, "CoverageWeight", 1.0, "03 Peel", 8, -1400, 1180)),
        # 0.88（仿真档）在引擎里读成一地碎屑；0.93 之后是"偶尔几块石头从灰泥里顶出来"，
        # 与用户 2026-09-03 那句"一部分砖恒高于灰泥面"对得上（2026-09-04 三档同机位对照选定）。
        ("ProtrudeLevel", scalar(mat, "ProtrudeLevel", 0.93, "04 Protrude", 0, -1400, 1250)),
        ("ProtrudeVar", scalar(mat, "ProtrudeVariation", 0.11, "04 Protrude", 1, -1400, 1320)),
        ("ProtrudeThin", scalar(mat, "ProtrudeEdgeBoost", 0.15, "04 Protrude", 2, -1400, 1390)),
        ("ProtrudeRange", scalar(mat, "ProtrudeEdgeRange", 25.0, "04 Protrude", 3, -1400, 1460)),
        ("ProtrudeSoftness", scalar(mat, "ProtrudeSoftness", 0.05, "04 Protrude", 4, -1400, 1530)),
    ]
    peel = custom(mat, "TGPlasterPeel", PEEL_HLSL,
                  ["WP", "WallH", "BrickH", "Coverage"] + [n for n, _ in peel_args],
                  unreal.CustomMaterialOutputType.CMOT_FLOAT4, -950, 600)
    MEL.connect_material_expressions(wpos, "", peel, "WP")
    MEL.connect_material_expressions(wall_h, "", peel, "WallH")
    MEL.connect_material_expressions(brick_h, "", peel, "BrickH")
    MEL.connect_material_expressions(vcol, "A", peel, "Coverage")
    for n, e in peel_args:
        MEL.connect_material_expressions(e, "", peel, n)

    m_mask = mask(mat, peel, True, False, False, False, -700, 480)    # BrickMask
    m_band = mask(mat, peel, False, True, False, False, -700, 560)    # 断面带
    m_lip = mask(mat, peel, False, False, True, True, -700, 640)      # 卷边梯度 xy

    # ---- 合成 ----
    v_btint = vector(mat, "BrickTint", 1.0, 1.0, 1.0, "01 Brick", 5, -1200, -1000)
    v_ptint = vector(mat, "PlasterTint", 1.0, 1.0, 1.0, "02 Plaster", 5, -1200, -920)
    # TG 的砂浆色表里 id 8/10/12 那一档（_nani_plaster ps_main:400）。
    v_mtint = vector(mat, "MortarTint", 0.400, 0.366, 0.244, "02 Plaster", 6, -1200, -840)
    # 0.45（2026-09-03 对着无光照仿真定的）在引擎里把砖缝压成纯黑沟，整面墙读成鳞片；
    # 参照图 `img/tiny-glade-ref-corner-pillar.jpg` 里的缝是**细暗线**，不是黑沟。
    p_groove = scalar(mat, "GrooveDarken", 0.75, "01 Brick", 6, -1200, -760)

    albedo = custom(mat, "TGWallAlbedo", ALBEDO_HLSL,
                    ["Plaster", "Mortar", "Brick", "BrickH", "Mask", "Band",
                     "PlasterTint", "MortarTint", "BrickTint", "GrooveDarken"],
                    unreal.CustomMaterialOutputType.CMOT_FLOAT3, -400, -400)
    MEL.connect_material_expressions(t_plas, "RGB", albedo, "Plaster")
    MEL.connect_material_expressions(t_mort, "RGB", albedo, "Mortar")
    MEL.connect_material_expressions(t_brick, "RGB", albedo, "Brick")
    MEL.connect_material_expressions(brick_h, "", albedo, "BrickH")
    MEL.connect_material_expressions(m_mask, "", albedo, "Mask")
    MEL.connect_material_expressions(m_band, "", albedo, "Band")
    MEL.connect_material_expressions(v_ptint, "", albedo, "PlasterTint")
    MEL.connect_material_expressions(v_mtint, "", albedo, "MortarTint")
    MEL.connect_material_expressions(v_btint, "", albedo, "BrickTint")
    MEL.connect_material_expressions(p_groove, "", albedo, "GrooveDarken")
    MEL.connect_material_property(albedo, "", unreal.MaterialProperty.MP_BASE_COLOR)

    nrm = custom(mat, "TGWallNormal", NORMAL_HLSL, ["NPlaster", "NBrick", "Mask", "Lip"],
                 unreal.CustomMaterialOutputType.CMOT_FLOAT3, -400, 100)
    MEL.connect_material_expressions(t_plasn, "RGB", nrm, "NPlaster")
    MEL.connect_material_expressions(t_brickn, "RGB", nrm, "NBrick")
    MEL.connect_material_expressions(m_mask, "", nrm, "Mask")
    MEL.connect_material_expressions(m_lip, "", nrm, "Lip")
    MEL.connect_material_property(nrm, "", unreal.MaterialProperty.MP_NORMAL)

    p_prough = scalar(mat, "PlasterRoughness", 0.85, "02 Plaster", 7, -700, 260)
    p_brough = scalar(mat, "BrickRoughness", 0.92, "01 Brick", 7, -700, 340)
    rough = lerp3(mat, p_prough, p_brough, m_mask, -400, 300)
    MEL.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)

    MEL.recompile_material(mat)
    unreal.EditorAssetLibrary.save_loaded_asset(mat)
    return mat


def build_wall_instance(parent):
    """墙面的可调实例。贴图默认值已经在母材质上，这里显式写一遍：MI 面板里能直接看见、直接换。"""
    path = "%s/MI_TinyGladeWall" % PKG
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        mi = unreal.EditorAssetLibrary.load_asset(path)
    else:
        mi = TOOLS.create_asset("MI_TinyGladeWall", PKG, unreal.MaterialInstanceConstant,
                                unreal.MaterialInstanceConstantFactoryNew())
    MEL.set_material_instance_parent(mi, parent)
    for pname, tname in (("BrickColor", "brick_colors_layer01"),
                         ("BrickNormal", "stone_floor_2_normal"),
                         ("BrickHeight", "stone_floor_2_height"),
                         ("PlasterColor", "plaster_colors_layer04"),
                         ("PlasterNormal", "default_normal"),
                         ("MortarColor", "mortar")):
        tex = unreal.EditorAssetLibrary.load_asset("%s/%s" % (TEX, tname))
        MEL.set_material_instance_texture_parameter_value(mi, pname, tex)
    prune_orphan_overrides(mi)
    MEL.update_material_instance(mi)
    unreal.EditorAssetLibrary.save_loaded_asset(mi)
    return mi


def prune_orphan_overrides(mi):
    """删掉母材质里已经没有的参数覆盖（`WALL_PARAMS` 是本次建图真正创建的参数名）。

    ⚠️ **孤儿覆盖在 MI 面板上是看不见的**（面板由母材质的参数表驱动），所以这件事只有靠脚本
    自己做。2026-09-04 实测：MI 上还留着一条 `BrickSeed = stone_floor_2_seed`，来自被废弃的
    「用 seed 图逐格选砖」那版（废弃理由见 `PEEL_HLSL` 里那条 ⚠️）。它今天只是死字节，
    但换掉那张贴图时会留一份不会有人再看的引用 —— 而且母材质哪天要是**重新**引入同名参数，
    这条旧覆盖会立刻生效、且完全不知道从哪来。

    判据用**本次建图记下的名字**，不去问引擎「母材质有没有这个参数」：那几个
    `get_material_default_*_parameter_value` 对不存在的参数是返回缺省值而不是报错，
    拿它当存在性判据会一条都删不掉。
    """
    dropped = []
    for kind, values in (("scalar", "scalar_parameter_values"),
                         ("vector", "vector_parameter_values"),
                         ("texture", "texture_parameter_values")):
        keep = []
        for entry in mi.get_editor_property(values):
            name = str(entry.get_editor_property("parameter_info").get_editor_property("name"))
            if name in WALL_PARAMS:
                keep.append(entry)
            else:
                dropped.append("%s:%s" % (kind, name))
        mi.set_editor_property(values, keep)
    unreal.log("MI orphan overrides dropped: %s" % (", ".join(dropped) if dropped else "(none)"))
    return dropped


def build_plain(name, r, g, b):
    mat = make_or_load(name)
    base = const3(mat, r, g, b, -350, 0)
    MEL.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    rough = const1(mat, 0.9, -350, 120)
    MEL.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    MEL.recompile_material(mat)
    unreal.EditorAssetLibrary.save_loaded_asset(mat)
    return mat


fixed = fix_data_textures()
if fixed:
    unreal.log("sRGB fixed on data textures: %s" % ", ".join(fixed))

wall = build_wall()
wall_mi = build_wall_instance(wall)
roof = build_plain("M_TinyGladeRoof", 0.30, 0.16, 0.13)

st = MEL.get_statistics(wall)
unreal.log("M_TinyGladeWall stats: vs=%s ps=%s samplers=%s" % (
    st.get_editor_property("num_vertex_shader_instructions"),
    st.get_editor_property("num_pixel_shader_instructions"),
    st.get_editor_property("num_samplers")))

# ---- 挂到蓝图 CDO + 当前关卡里已有的房子 ----
# ⚠️ **不 load_map**：编辑器里可能开着别的、还没存的关卡（实测 2026-09-03 开着
# L_TerrainOpsDemo 且 dirty）。换关卡会弹存盘对话框把脚本卡死，或者把改动丢掉。
bp = unreal.EditorAssetLibrary.load_asset("%s/BP_TinyGladeHouse" % PKG)
if bp:
    cdo = unreal.get_default_object(bp.generated_class())
    for prop, mat in (("WallMaterial", wall_mi), ("RoofMaterial", roof)):
        cdo.set_editor_property(prop, mat)
    unreal.EditorAssetLibrary.save_loaded_asset(bp)
    unreal.log("assigned to BP_TinyGladeHouse CDO")

count = 0
for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():
    if "House" not in a.get_class().get_name():
        continue
    for prop, mat in (("WallMaterial", wall_mi), ("RoofMaterial", roof)):
        a.set_editor_property(prop, mat)
    a.call_method("RebuildHouse")
    count += 1
unreal.log("assigned to %d house actors in the CURRENT level (level not saved)" % count)
unreal.log("MATERIALS DONE")
