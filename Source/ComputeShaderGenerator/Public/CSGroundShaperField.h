#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"

/**
 * 地形塑形物高度场的 **CPU 孪生**，与 `Shaders/Private/CSGroundShaperField.ush` 逐行对照。
 *
 * 为什么要有这一份：GPU 侧决定地面网格与石阶长什么样，CPU 侧决定房子落座、拾取、道路查询
 * 读到的高度。两侧分叉不报任何错 —— 症状是石阶浮在坡面上方几厘米、房子陷进土台几厘米，
 * 只在裙边中段看得出来，而且两条路各自都"自洽"。所以这里的每一行都必须是 ush 的转写：
 *
 *   · 只用 uint32 环绕算术 + 加减乘除 + sqrt。这几样在 C++ 与 HLSL 里是同一套语义。
 *   · **不用 sin/cos/pow/exp**，也不用 HLSL 内置 `noise()`（CPU 侧根本没有对应物）；
 *     超越函数两边的实现不同，差值会大到断言看得见。
 *   · 全程 float，不是 double：GPU 只有 float，CPU 用 double 算完再截断不是同一个数。
 *
 * 单测 `PCGPlugins.ComputeShaderGenerator.GroundShaper.CpuGpuFieldParity` 把两侧在整张地面上
 * 逐顶点比一遍；改了 ush 而忘了改这里（或反过来）会立刻报红。
 */
namespace CSGroundShaperField
{
/**
 * 每座塑形物在 `GroundShaperParams` 里占几个 float4。ush 里的下标算式（`i * 3u + k`）与
 * `ACSGroundActor::BuildShaperGpuParams` / `UCSMeshOps::DisplaceGroundShapers` /
 * `CSGroundStairs::Scan` 里的 `Num() / 3` 全部依赖它。
 *
 *   [3i + 0] Profile = (中心 X, 中心 Y, Radius, FalloffDistance)
 *   [3i + 1] Top     = (台高, 裙边噪声幅度(归一化), 噪声频率 = 1/波长, 二次抬升系数)
 *   [3i + 2] Noise   = (噪声种子, 0, 0, 0)
 */
inline constexpr int32 Float4sPerShaper = 3;

/** fbm 倍频数。与 ush 的 CSGSF_NOISE_OCTAVES 同步。 */
inline constexpr uint32 NoiseOctaves = 3;

inline uint32 Hash(uint32 Cx, uint32 Cy, uint32 Seed)
{
	uint32 H = Cx * 0x9E3779B1u;
	H ^= Cy * 0x85EBCA77u;
	H ^= Seed * 0xC2B2AE3Du;
	H ^= H >> 15;
	H *= 0x2545F491u;
	H ^= H >> 13;
	return H;
}

/** [0,1)。低 24 bit / 2^24：float 尾数正好 24 bit，商在两侧都是精确值。 */
inline float Hash01(uint32 Cx, uint32 Cy, uint32 Seed)
{
	return float(Hash(Cx, Cy, Seed) & 0x00FFFFFFu) * (1.0f / 16777216.0f);
}

/** 整数格点 value noise，t²(3−2t) 插值，返回 [-1, 1)。 */
inline float ValueNoise(float Px, float Py, uint32 Seed)
{
	const float Fx = FMath::FloorToFloat(Px);
	const float Fy = FMath::FloorToFloat(Py);
	const float Tx = Px - Fx;
	const float Ty = Py - Fy;
	const float Ux = Tx * Tx * (3.0f - 2.0f * Tx);
	const float Uy = Ty * Ty * (3.0f - 2.0f * Ty);
	// floor 之后再转 int 才对负坐标成立；int → uint 两侧都是模 2^32 的位保持转换。
	const uint32 Cx = uint32(int32(Fx));
	const uint32 Cy = uint32(int32(Fy));

	const float H00 = Hash01(Cx, Cy, Seed);
	const float H10 = Hash01(Cx + 1u, Cy, Seed);
	const float H01 = Hash01(Cx, Cy + 1u, Seed);
	const float H11 = Hash01(Cx + 1u, Cy + 1u, Seed);

	// 展开写而不用 FMath::Lerp：求值顺序必须与 ush 逐字相同。
	const float A = H00 + (H10 - H00) * Ux;
	const float B = H01 + (H11 - H01) * Ux;
	return (A + (B - A) * Uy) * 2.0f - 1.0f;
}

/** 归一化 fbm。常数照 Tiny Glade 实测（gain 0.55 / lacunarity 1.9 / 每层旋转 36.87°）。 */
inline float Turbulence(float Px, float Py, uint32 Seed)
{
	float Sum = 0.0f;
	float Amp = 1.0f;
	float Norm = 0.0f;
	float Qx = Px;
	float Qy = Py;
	for (uint32 Octave = 0; Octave < NoiseOctaves; ++Octave)
	{
		Sum += ValueNoise(Qx, Qy, Seed + Octave * 101u) * Amp;
		Norm += Amp;
		Amp *= 0.55f;
		const float Rx = Qx * 0.8f - Qy * 0.6f;
		const float Ry = Qx * 0.6f + Qy * 0.8f;
		Qx = Rx * 1.9f;
		Qy = Ry * 1.9f;
	}
	return Sum / Norm;
}

/**
 * 单座塑形物的高度贡献（相对地面基面，恒 ≥ 0）。ush 的 `GroundShaperEvalOne` 的转写 ——
 * 逐条注释在 ush 那一侧，这里只留必须就地知道的两点。
 */
inline float EvalShaper(const FVector2f& P, const FVector4f& A, const FVector4f& B, const FVector4f& C)
{
	const float Dx = P.X - A.X;
	const float Dy = P.Y - A.Y;
	const float Skirt = FMath::Max(0.0f, FMath::Sqrt(Dx * Dx + Dy * Dy) - A.Z);
	if (Skirt >= A.W) return 0.0f;

	const float T = A.W > 1e-6f ? Skirt / A.W : 0.0f;
	const float W = 1.0f - T;
	float S = W * W * (3.0f - 2.0f * W);

	// 只减不加（abs 之后做减法），权重 (1−S) 让台顶不受扰、裙边最碎。噪声域是本座局部坐标。
	if (B.Y > 0.0f)
	{
		const float Qx = Dx * B.Z;
		const float Qy = Dy * B.Z;
		const uint32 Seed = uint32(FMath::Max(C.X, 0.0f));
		S = FMath::Clamp(S - FMath::Abs(Turbulence(Qx, Qy, Seed) * (1.0f - S)) * B.Y, 0.0f, 1.0f);
	}

	// S·sqrt(S) 而不是 pow(S, 1.5)：GPU 的 pow 走 exp2/log2，与 CPU 的 pow 不是同一个数。
	return FMath::Max(0.0f, B.X) * (S + B.W * S * FMath::Sqrt(S));
}

/** 该座的峰值高度（台顶，含二次抬升）。包围盒 / MaxAbsHeight 用，别拿 B.X 当上界。 */
inline float PeakHeight(const FVector4f& B)
{
	return FMath::Max(0.0f, B.X) * (1.0f + FMath::Max(0.0f, B.W));
}

/** 整份打包参数上的合成场：多座重叠**取 max**（计划 D9 的合成裁决）。 */
inline float EvalField(const FVector2f& P, TConstArrayView<FVector4f> Params)
{
	float H = 0.0f;
	for (int32 Base = 0; Base + Float4sPerShaper <= Params.Num(); Base += Float4sPerShaper)
	{
		H = FMath::Max(H, EvalShaper(P, Params[Base], Params[Base + 1], Params[Base + 2]));
	}
	return H;
}
}
