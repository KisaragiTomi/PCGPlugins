#include "CSHouseVine.h"

#include "CSGpuMeshTypes.h"
#include "CSHouseProfile.h"
#include "ComputeShaderGenerateHelper.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "Engine/StaticMesh.h"
#include "GlobalShader.h"
#include "Rendering/ColorVertexBuffer.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ShaderParameterStruct.h"
#include "StaticMeshResources.h"

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSHouseVine_ 前缀。

constexpr int32 CSHouseVine_GroupSize = 64;

class FCSHouseVinePackCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSHouseVinePackCS);
	SHADER_USE_PARAMETER_STRUCT(FCSHouseVinePackCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, VineRecords)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RWVineInstances)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWVineCounter)
		SHADER_PARAMETER(FMatrix44f, VineWorldToComponent)
		SHADER_PARAMETER(FVector3f, VineBaseSphereCentre)
		SHADER_PARAMETER(FVector3f, VineBlockSize)
		SHADER_PARAMETER(float, VineBaseSphereRadius)
		SHADER_PARAMETER(uint32, VineRecordCount)
		SHADER_PARAMETER(uint32, VineMaxInstances)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), CSHouseVine_GroupSize);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCSHouseVinePackCS, "/Plugin/PCGPlugins/Shaders/Private/CSHouseVine.usf", "PackHouseVineCS", SF_Compute);

/** 记录数组 → 上传用的 float4 平铺。布局与 `CSHouseVine.usf` 文件头逐字对应。 */
void CSHouseVine_Flatten(const TArray<CSHouseVine::FRecord>& In, TArray<FVector4f>& Out)
{
	Out.Reset(In.Num() * 3);
	for (const CSHouseVine::FRecord& R : In)
	{
		Out.Add(FVector4f(R.WorldPos.X, R.WorldPos.Y, R.WorldPos.Z, R.LengthScale));
		Out.Add(FVector4f(R.Dir.X, R.Dir.Y, R.Dir.Z, R.Random01));
		Out.Add(FVector4f(R.Normal.X, R.Normal.Y, R.Normal.Z, R.SizeScale));
	}
}
}

namespace CSHouseVine
{
float Hash01(uint32 H)
{
	H = ((H >> ((H >> 28) + 4u)) ^ H) * 277803737u;
	return float((H >> 22 ^ H) & 0xFFFFFFu) / float(0x1000000u);
}

uint32 IdentityHash(int32 EdgeIndex, int32 Strand, int32 Segment, uint32 Salt, int32 Seed)
{
	// 常数与 `CSGroundStairs.usf::CSStairs_CellSeed` 同一套 —— 两条路的"身份"口径要看得出同源。
	uint32 H = uint32(EdgeIndex + 1048576) * 0x9E3779B1u;
	H ^= uint32(Strand + 1048576) * 0x85EBCA77u;
	H ^= uint32(Segment + 1048576) * 0xC2B2AE3Du;
	H ^= Salt * 0x165667B1u;
	H ^= uint32(Seed) * 0x9E3779B9u;
	return H;
}

bool IsInsideOpening(const TArray<FCSWallOpening>& Openings, int32 EdgeIndex, float S, float Z, float Clearance)
{
	const float C = FMath::Max(Clearance, 0.0f);
	for (const FCSWallOpening& O : Openings)
	{
		if (O.EdgeIndex != EdgeIndex || !O.IsValid()) continue;
		// 拱脚线以下无下界（见头文件），所以洞底这条边界只能自己补。
		if (Z < O.Z0 - C) continue;

		// 把**洞**胀大再算场，而不是把场的结果外扩：拱胀大后拱脚线 Z1−HW 逐位不变、
		// 半圆半径 +C，恰好是那条曲线的等距外偏移；矩形与圆同理。
		FCSWallOpening Fat = O;
		Fat.Width = O.Width + 2.0f * C;
		Fat.Z0 = O.Z0 - C;
		Fat.Z1 = O.Z1 + C;
		const FCSOpeningClipField Field = CSHouse_ComputeClipField(Fat);
		if (!CSHouse_ClipKeeps(Field, Field.Eval(S, Z))) return true;
	}
	return false;
}

/**
 * 藤脚落在洞里时**沿墙脚侧移**到洞外最近的空位，返回是否找到。
 *
 * ⚠️ 第一档是"藤脚在洞里就整根不长"，代价实测：演示房子一开六个拱，两面长墙的根有一大半
 * 直接落在拱底下（拱是**落地**的，Z0 = 0 ⇒ 整个洞宽的墙脚都没了），成片秃掉。
 * 侧移读起来正是真藤在门洞旁边的样子 —— 门口两侧丛生、门洞里干净。
 *
 * 演示房子实测（六拱全开、其余开关按第一档摆平，2026-08-31）：**319 → 158 变成 319 → 198**，
 * 保留率 0.495 → 0.621。这一条是"拱附近变稀"唯一有效的解药 —— 换洞判据（外接矩形 → 解析
 * clip 场）在同一场景里只多出 0~2 段，别把两者搞混（见 `CSHouseVine::IsInsideOpening`）。
 *
 * 搜索半径限成一个藤位（`Slot`）：再远就等于把两根藤挤到同一个位置，视觉上是一簇假的密丛。
 */
static bool CSHouseVine_EscapeRoot(const TArray<FCSWallOpening>& Openings, int32 EdgeIndex,
	float& InOutS, float MinS, float MaxS, float Clearance, float Slot, float FirstStepZ)
{
	// 根部与**第一段的落点**都要在洞外：只查 z=0 的话，贴着拱脚安家的藤第一步就撞回洞里。
	auto Blocked = [&](float S)
	{
		return IsInsideOpening(Openings, EdgeIndex, S, 0.0f, Clearance)
			|| IsInsideOpening(Openings, EdgeIndex, S, FirstStepZ, Clearance);
	};
	if (!Blocked(InOutS)) return true;

	const float Step = FMath::Max(Slot * 0.125f, 2.0f);
	for (float Offset = Step; Offset <= Slot; Offset += Step)
	{
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const float S = FMath::Clamp(InOutS + (Side == 0 ? -Offset : Offset), MinS, MaxS);
			if (!Blocked(S)) { InOutS = S; return true; }
		}
	}
	return false;
}

void BuildPlan(const TArray<FWallStrip>& Strips, const TArray<FCSWallOpening>& Openings,
	const FParams& Params, FPlan& OutPlan)
{
	OutPlan.Reset();

	const float SegLen = FMath::Max(Params.SegmentLength, 1.0f);
	const float Spacing = FMath::Max(Params.StrandSpacing, 10.0f);
	const int32 MaxSeg = FMath::Clamp(Params.MaxSegments, 1, 256);

	// 转角跨墙要能找到"隔壁那面墙"。EdgeIndex 是环形的（`CSHouse_GetEdge` 的 0..3 绕一圈），
	// 模数取实际出现过的最大编号 + 1 —— 矩形房是 4，将来多边形 footprint 也自洽。
	// ⚠️ 线性扫而不是建 TMap：这是纯函数、拖动时每帧都要跑，四面墙的扫描比一次哈希表分配便宜。
	int32 EdgeModulus = 0;
	for (const FWallStrip& Strip : Strips) EdgeModulus = FMath::Max(EdgeModulus, Strip.EdgeIndex + 1);
	auto FindStrip = [&Strips](int32 EdgeIndex) -> const FWallStrip*
	{
		for (const FWallStrip& Strip : Strips) { if (Strip.EdgeIndex == EdgeIndex) return &Strip; }
		return nullptr;
	};
	// 藤脚离墙角留半个段长：贴着转角起藤会有一半的段穿进隔壁那面墙里。
	// ⚠️ 逐墙算 —— 跨墙以后墙长换了，沿用起点那面墙的余量会让藤在短墙上越界。
	auto MarginOf = [SegLen](const FWallStrip& W) { return FMath::Min(SegLen * 0.5f, W.Length * 0.25f); };

	for (const FWallStrip& Root : Strips)
	{
		if (Root.Length <= Spacing * 0.5f || Root.Height <= SegLen) continue;

		const int32 StrandCount = FMath::Max(1, FMath::RoundToInt(Root.Length / Spacing));
		const float Slot = Root.Length / float(StrandCount);
		const float RootMargin = MarginOf(Root);

		for (int32 Strand = 0; Strand < StrandCount; ++Strand)
		{
			const FWallStrip* Wall = &Root;
			float Margin = RootMargin;
			// 起点：格中心 + 半格以内的抖动。**身份里没有位置** —— 见头文件那段。
			const float RootJitter = (Hash01(IdentityHash(Root.EdgeIndex, Strand, -1, 3u, Params.Seed)) - 0.5f) * Slot;
			float S = FMath::Clamp((float(Strand) + 0.5f) * Slot + RootJitter, Margin, Root.Length - Margin);
			float Z = 0.0f;
			// 初始倾角：左右各一半，别让整面墙的藤都朝同一边歪。
			float Angle = (Hash01(IdentityHash(Root.EdgeIndex, Strand, -1, 5u, Params.Seed)) - 0.5f) * 2.0f * Params.MaxLean * 0.5f;

			// 藤脚在洞里（门口正下方）：先试着侧移出去，实在挪不开才放弃这根。
			if (!CSHouseVine_EscapeRoot(Openings, Wall->EdgeIndex, S, Margin, Wall->Length - Margin,
				Params.HoleClearance, Slot, SegLen)) continue;

			for (int32 Segment = 0; Segment < MaxSeg; ++Segment)
			{
				// ⚠️ **身份一律用"起点那面墙"的编号**，不是当前所在的墙：跨墙以后用当前墙的话，
				// 同一根藤在拐弯前后会拿到两套随机，而且拐不拐弯本身又由随机决定 ⇒ 自指。
				// 身份 = (起点墙, 藤号, 段号, 佐料, 种子)，跨墙对它是透明的。
				const uint32 Id = IdentityHash(Root.EdgeIndex, Strand, Segment, 11u, Params.Seed);
				Angle += (Hash01(Id) - 0.5f) * 2.0f * Params.Wander;
				Angle = FMath::Clamp(Angle, -Params.MaxLean, Params.MaxLean);

				const FWallStrip* NextWall = Wall;
				float NextS = S + FMath::Sin(Angle) * SegLen;
				const float NextZ = Z + FMath::Cos(Angle) * SegLen;

				// 长到墙顶就收。**墙顶是 S 的函数**：檐墙恒 = 墙高，山墙是那条三角斜边
				// （藤因此能爬过檐口高度、继续在山墙上长）。这条线以上是屋面板，不是墙。
				if (NextZ > Wall->TopAt(FMath::Clamp(NextS, 0.0f, Wall->Length))) break;

				// 撞墙角。两条出路：
				//  · 跨到隔壁那面墙继续长（TG 的 `check_for_wall_jump`）—— 藤绕着房子转角爬，
				//    这是 TG 里最显眼的一条藤蔓行为，第一档没做；
				//  · 没掷中（或隔壁那面墙不在输入里，比如单墙单测）就把倾角**镜像**回来。
				//    镜像而不是夹死：夹死会让藤沿着墙角笔直往上爬一长条，一眼看出是程序生成的。
				if (NextS < Margin || NextS > Wall->Length - Margin)
				{
					const bool bForward = NextS > Wall->Length - Margin;
					const FWallStrip* Neighbour = EdgeModulus > 1
						? FindStrip((Wall->EdgeIndex + (bForward ? 1 : EdgeModulus - 1)) % EdgeModulus)
						: nullptr;
					const bool bJump = Neighbour && Neighbour != Wall
						&& Neighbour->Length > MarginOf(*Neighbour) * 2.0f
						&& Hash01(IdentityHash(Root.EdgeIndex, Strand, Segment, 53u, Params.Seed)) < Params.JumpChance;
					if (bJump)
					{
						const float NeighbourMargin = MarginOf(*Neighbour);
						// 越角以后沿墙方向不翻身：原来"往前"到了新墙上仍然是"往前"（从墙头进），
						// 倾角保持不变，藤读起来是**连着**绕过转角的，而不是在转角处折了一下。
						const float Entry = bForward ? NeighbourMargin : Neighbour->Length - NeighbourMargin;
						const float Overshoot = bForward ? (NextS - (Wall->Length - Margin)) : (Margin - NextS);
						NextS = FMath::Clamp(bForward ? Entry + Overshoot : Entry - Overshoot,
							NeighbourMargin, Neighbour->Length - NeighbourMargin);
						NextWall = Neighbour;
					}
					else
					{
						Angle = -Angle;
						NextS = FMath::Clamp(S + FMath::Sin(Angle) * SegLen, Margin, Wall->Length - Margin);
					}
				}

				// 墙洞：先**绕**，绕不过去再直着往上顶一次，还不行才停。
				// ⚠️ "撞上就停"试过，作废：演示房子一开六个拱，两面长墙的藤当场从 319 段掉到 132，
				// 拱之间的墙面成片秃掉 —— 因为每根藤是从墙脚长上去的，拱正好压在它的根上。
				// 第二次尝试取**竖直**（Angle = 0）而不是再镜像一次：窄墩两侧都是洞，能穿过去的
				// 只有竖直那一条路；再镜像一次只会在两个洞之间原地摆动。
				// 这也更接近 TG 的 `intersect_ivy_growth_w_wall_segment`（逐段求交后改向，不是整根砍掉）。
				if (IsInsideOpening(Openings, NextWall->EdgeIndex, NextS, NextZ, Params.HoleClearance))
				{
					const float Mirror = FMath::Clamp(S - FMath::Sin(Angle) * SegLen, Margin, Wall->Length - Margin);
					if (NextWall == Wall && !IsInsideOpening(Openings, Wall->EdgeIndex, Mirror, NextZ, Params.HoleClearance))
					{
						Angle = -Angle;
						NextS = Mirror;
					}
					else if (NextWall == Wall && !IsInsideOpening(Openings, Wall->EdgeIndex, S, NextZ, Params.HoleClearance))
					{
						Angle = 0.0f;
						NextS = S;
					}
					else
					{
						break;   // 被夹在洞与墙角之间，继续绕就是原地打转
					}
				}

				const FVector A = Wall->Origin + Wall->U * S + Wall->Up * Z + Wall->N * Params.StandOff;
				const FVector B = NextWall->Origin + NextWall->U * NextS + NextWall->Up * NextZ + NextWall->N * Params.StandOff;
				const FVector Delta = B - A;
				const float Len = float(Delta.Size());
				// 跨墙那一段横跨两个墙平面，截面基取**两面墙法线的平均**：取任一面都会让那一段
				// 的截面斜插进另一面墙里，而转角恰恰是最显眼的位置。同墙时它逐位等于 Wall->N。
				const FVector SegNormal = (Wall->N + NextWall->N).GetSafeNormal(UE_SMALL_NUMBER, Wall->N);
				if (Len > UE_KINDA_SMALL_NUMBER)
				{
					FRecord Rec;
					Rec.WorldPos = FVector3f(A);
					// ⚠️ 长度轴**故意胀大**（默认 1.15）：与门框砖同一条 TG 实证 ——
					// 相邻两段共用穿越点、缝是负的，段数一变只是穿插量微调；正缝则会在
					// 藤的每个折点露出一条亮缝，而折点恰恰是最显眼的地方。
					Rec.LengthScale = Len * FMath::Max(Params.Bloat, 1.0f);
					Rec.Dir = FVector3f(Delta / Len);
					Rec.Normal = FVector3f(SegNormal);
					Rec.Random01 = Hash01(IdentityHash(Root.EdgeIndex, Strand, Segment, 71u, Params.Seed));
					// 越往上越细，藤才有"长出来"的方向感（TG 的 IvySegment 自带 start/end_thickness）。
					Rec.SizeScale = FMath::Lerp(1.0f, 0.55f, float(Segment) / float(MaxSeg));
					OutPlan.Branch.Add(Rec);
				}

				const FVector Along = Delta.GetSafeNormal();
				if (!Along.IsNearlyZero()
					&& Hash01(IdentityHash(Root.EdgeIndex, Strand, Segment, 23u, Params.Seed)) < Params.LeafChance)
				{
					const float Side = Hash01(IdentityHash(Root.EdgeIndex, Strand, Segment, 31u, Params.Seed)) < 0.5f ? -1.0f : 1.0f;
					const float Spread = 0.6f + 0.8f * Hash01(IdentityHash(Root.EdgeIndex, Strand, Segment, 37u, Params.Seed));
					// 叶子从段的中点斜着支出去：方向 = 段方向绕墙面法线转 ±(35°..80°)。
					const float Turn = Side * Spread;
					const FVector Sideways = FVector::CrossProduct(SegNormal, Along).GetSafeNormal();
					const FVector LeafDir = (Along * FMath::Cos(Turn) + Sideways * FMath::Sin(Turn)).GetSafeNormal();
					if (!LeafDir.IsNearlyZero())
					{
						FRecord Leaf;
						Leaf.WorldPos = FVector3f((A + B) * 0.5 + SegNormal * (Params.StandOff * 0.5));
						const float Jitter = 1.0f + (Hash01(IdentityHash(Root.EdgeIndex, Strand, Segment, 43u, Params.Seed)) - 0.5f)
							* 2.0f * Params.LeafSizeJitter;
						Leaf.LengthScale = FMath::Max(Params.LeafSize * Jitter, 1.0f);
						Leaf.Dir = FVector3f(LeafDir);
						Leaf.Normal = FVector3f(SegNormal);
						Leaf.Random01 = Hash01(IdentityHash(Root.EdgeIndex, Strand, Segment, 97u, Params.Seed));
						Leaf.SizeScale = Jitter;
						OutPlan.Leaf.Add(Leaf);
					}
				}

				// 花（TG 的 `ivy_flower`）。只开在藤的上半截：TG 的花挂在**长成了的**藤上，
				// 而且贴着地面那一圈会被地形与杂物挡住，纯白付实例。
				const int32 FlowerFrom = FMath::CeilToInt(float(MaxSeg) * FMath::Clamp(Params.FlowerFromFrac, 0.0f, 1.0f));
				if (Segment >= FlowerFrom && !Along.IsNearlyZero()
					&& Hash01(IdentityHash(Root.EdgeIndex, Strand, Segment, 59u, Params.Seed)) < Params.FlowerChance)
				{
					// 花簇朝**外上方**张开（`ivy_flower` 实测底面在 Z=0、簇沿自身 +Z 张开）。
					// ⚠️ 基准向量给 `Along` 而不是墙法线：kernel 用 cross(Normal, Dir) 搭面内轴，
					// 而花的 Dir 本身就以墙法线为主 ⇒ 传墙法线会近似共线，被 kernel 的退化判据丢掉，
					// 症状是"花一朵都不出现"而 counter 却是对的。
					const float Tilt = (Hash01(IdentityHash(Root.EdgeIndex, Strand, Segment, 67u, Params.Seed)) - 0.5f) * 0.7f;
					const FVector Sideways = FVector::CrossProduct(SegNormal, Along).GetSafeNormal();
					const FVector FlowerDir = (SegNormal * 0.85 + FVector(0.0, 0.0, 0.45) + Sideways * Tilt).GetSafeNormal();
					if (!FlowerDir.IsNearlyZero())
					{
						FRecord Flower;
						Flower.WorldPos = FVector3f(B + SegNormal * (Params.StandOff * 0.5));
						const float Jitter = 1.0f + (Hash01(IdentityHash(Root.EdgeIndex, Strand, Segment, 89u, Params.Seed)) - 0.5f)
							* 2.0f * Params.LeafSizeJitter;
						// 高 = 宽 × 实测高宽比：`BlockSize` 只把 xy 钉成 FlowerSize 的方截面，
						// z 得由记录自己说，否则花被拉成柱子（见 FParams::FlowerAspect）。
						Flower.LengthScale = FMath::Max(Params.FlowerSize * Params.FlowerAspect * Jitter, 1.0f);
						Flower.Dir = FVector3f(FlowerDir);
						Flower.Normal = FVector3f(Along);
						Flower.Random01 = Hash01(IdentityHash(Root.EdgeIndex, Strand, Segment, 101u, Params.Seed));
						Flower.SizeScale = Jitter;
						OutPlan.Flower.Add(Flower);
					}
				}

				S = NextS;
				Z = NextZ;
				if (NextWall != Wall)
				{
					Wall = NextWall;
					Margin = MarginOf(*Wall);
				}
			}
		}
	}
}

bool BuildBaseMesh(UStaticMesh* Source, int32 LengthAxis, FCSGpuMeshCPUData& Out)
{
	Out = FCSGpuMeshCPUData();
	const FStaticMeshRenderData* RenderData = Source ? Source->GetRenderData() : nullptr;
	if (!RenderData || RenderData->LODResources.Num() == 0) return false;

	const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
	const uint32 NumVerts = LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();
	if (NumVerts < 3) return false;

	TArray<uint32> Indices;
	LOD.IndexBuffer.GetCopy(Indices);
	if (Indices.Num() < 3) return false;

	// 换轴：把源网格的长度轴旋到 +Z（kernel 的基约定只认 +Z）。绕单轴 90°，纯置换 + 变号，
	// 所以不引入任何数值误差，也不需要重算绕序（行列式恒 +1）。
	auto ToZ = [LengthAxis](const FVector3f& V)
	{
		switch (LengthAxis)
		{
		case 0: return FVector3f(-V.Z, V.Y, V.X);   // +X → +Z
		case 1: return FVector3f(V.X, -V.Z, V.Y);   // +Y → +Z
		default: return V;                          // 已经是 +Z
		}
	};

	Out.Positions.Reserve(int32(NumVerts));
	for (uint32 V = 0; V < NumVerts; ++V)
	{
		Out.Positions.Add(ToZ(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(V)));
	}
	Out.Indices = MoveTemp(Indices);
	Out.SourceSpace = FCSGpuMeshCPUData::ESpace::ComponentLocal;
	Out.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerVertex;
	Out.NumTexCoordChannels = 1;

	const uint32 NumTangentVerts = LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumVertices();
	const bool bHasTangentStream = NumTangentVerts == NumVerts;
	const bool bHasUVStream = bHasTangentStream && LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords() > 0;

	// ⚠️ **有流不等于有数据**：`ivy_branch` 的顶点流只有位置，导入器仍然给了一条切线流，
	// 里面全是零长法线；直接用它的后果是整条藤黑成剪影。所以判据是"读出来的东西合不合法"，
	// 不是"这条流在不在"。
	bool bNormalsUsable = bHasTangentStream;
	bool bUVsUsable = bHasUVStream;
	if (bHasTangentStream)
	{
		bool bAnyNonZeroUV = false;
		for (uint32 V = 0; V < NumVerts && bNormalsUsable; ++V)
		{
			if (LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(V).Size() < 0.5f) bNormalsUsable = false;
		}
		for (uint32 V = 0; bHasUVStream && V < NumVerts && !bAnyNonZeroUV; ++V)
		{
			bAnyNonZeroUV |= !LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(V, 0).IsNearlyZero();
		}
		bUVsUsable = bHasUVStream && bAnyNonZeroUV;
	}

	Out.Normals.SetNumZeroed(int32(NumVerts));
	Out.Tangents.SetNumZeroed(int32(NumVerts));
	Out.TexCoords().SetNumZeroed(int32(NumVerts));
	// 顶点色：**有就搬，没有才退白**。藤那两张都没有颜色流，所以这一段对 D13 是恒等的；
	// 它是为 **D12 的 clutter** 加的 —— TG 的杂物把颜色全烘在顶点流里（`Content/TinyGlade/Textures/`
	// 里没有一张 clutter 贴图，459 张贴图与 459 个 MI 一一对应、clutter 一个都不在其中）。
	// 丢掉它的症状是整批摆件变成同一种平色 —— 看着像"贴图没接上"，实际上本就没有贴图。
	// ⚠️ 搬过来的值是**线性域的、而且很暗**（直接从源 GLB 量：`barrel` 均值 .041、
	// `firewood` .233）。消费端把它当 base color 前必须先提亮，否则是一排黑影 ——
	// 这一条的供给侧写在 `Scripts/TinyGladeMakeDecorMaterial.py` 的 `COLOR_BOOST`。
	// ⚙ 归一化用 /255 而不是 `FLinearColor(FColor)`：`SetBaseMeshFromGpuData` 另一端是
	// `ToFColor(false)`（不做 sRGB 转换），两边必须同口径才能逐字节往返，
	// 也才与直接走 `SetBaseMesh` 那条路的结果一致。
	const bool bHasColorStream = LOD.VertexBuffers.ColorVertexBuffer.GetNumVertices() == NumVerts;
	if (bHasColorStream)
	{
		Out.Colors.SetNumUninitialized(int32(NumVerts));
		for (uint32 V = 0; V < NumVerts; ++V)
		{
			const FColor C = LOD.VertexBuffers.ColorVertexBuffer.VertexColor(V);
			Out.Colors[int32(V)] = FVector4f(float(C.R), float(C.G), float(C.B), float(C.A)) / 255.0f;
		}
	}
	else
	{
		Out.Colors.Init(FVector4f(1.0f, 1.0f, 1.0f, 1.0f), int32(NumVerts));
	}

	if (bNormalsUsable)
	{
		for (uint32 V = 0; V < NumVerts; ++V)
		{
			Out.Normals[int32(V)] = ToZ(FVector3f(LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(V)));
			Out.Tangents[int32(V)] = ToZ(LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentX(V));
		}
	}
	else
	{
		// 相邻面法线累加。`ivy_branch` 是 12 顶点 / 6 三角的开口三棱管、顶点不共享 ⇒
		// 每个顶点只被一个面引用，累加出来正好是**逐面平法线**（棱柱的正确答案，不是近似）。
		for (int32 I = 0; I + 2 < Out.Indices.Num(); I += 3)
		{
			const int32 A = int32(Out.Indices[I]), B = int32(Out.Indices[I + 1]), C = int32(Out.Indices[I + 2]);
			if (!Out.Positions.IsValidIndex(A) || !Out.Positions.IsValidIndex(B) || !Out.Positions.IsValidIndex(C)) continue;
			const FVector3f FaceN = FVector3f::CrossProduct(Out.Positions[B] - Out.Positions[A], Out.Positions[C] - Out.Positions[A]);
			Out.Normals[A] += FaceN;
			Out.Normals[B] += FaceN;
			Out.Normals[C] += FaceN;
		}
		for (int32 V = 0; V < Out.Normals.Num(); ++V)
		{
			// 退化面（零面积）留下的零法线退回径向朝外 —— 管壁的外法线本来就是径向。
			const FVector3f Radial = FVector3f(Out.Positions[V].X, Out.Positions[V].Y, 0.0f);
			Out.Normals[V] = Out.Normals[V].GetSafeNormal(UE_SMALL_NUMBER, Radial.GetSafeNormal(UE_SMALL_NUMBER, FVector3f(0, 0, 1)));
			Out.Tangents[V] = FVector3f::CrossProduct(FVector3f(0, 0, 1), Out.Normals[V]).GetSafeNormal(UE_SMALL_NUMBER, FVector3f(1, 0, 0));
		}
	}

	if (bUVsUsable)
	{
		for (uint32 V = 0; V < NumVerts; ++V) Out.TexCoords()[int32(V)] = LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(V, 0);
	}
	else
	{
		// 绕长度轴的柱面展开。**必须是柱面而不是平面投影**：实例被非均匀缩放
		// （截面 9 cm、长度 26 cm 起），平面 UV 会把贴图沿长度轴拉成条。
		FBox3f Bounds(ForceInit);
		for (const FVector3f& P : Out.Positions) Bounds += P;
		const float Span = FMath::Max(Bounds.Max.Z - Bounds.Min.Z, UE_SMALL_NUMBER);
		for (int32 V = 0; V < Out.Positions.Num(); ++V)
		{
			const FVector3f& P = Out.Positions[V];
			Out.TexCoords()[V] = FVector2f(
				FMath::Atan2(P.Y, P.X) / (2.0f * UE_PI) + 0.5f,
				(P.Z - Bounds.Min.Z) / Span);
		}
	}

	Out.BinormalSigns.Init(1.0f, int32(NumVerts));
	Out.TriangleMaterialSlots.Init(0, Out.Indices.Num() / 3);
	return true;
}

bool Pack(const FPlan& Plan, const TArray<CSShaperSteps::FPaletteBuffers>& Palettes, const FMatrix44f& WorldToComponent)
{
	if (Palettes.Num() < Palette_Num) return false;

	TArray<TArray<FVector4f>> Flat;
	Flat.SetNum(Palette_Num);
	CSHouseVine_Flatten(Plan.Branch, Flat[Palette_Branch]);
	CSHouseVine_Flatten(Plan.Leaf, Flat[Palette_Leaf]);
	CSHouseVine_Flatten(Plan.Flower, Flat[Palette_Flower]);

	// ⚠️ 判据是"有没有可写的调色板"，**不是**"有没有记录"：全空时也必须走一趟，
	// 否则 counter 停在上一次的值 —— 症状是"藤已经排不出来了但画面上还在"，
	// 而且只在从有到无那一次出现（空表分支自己会 AddClearUAVPass）。
	bool bAny = false;
	for (int32 Index = 0; Index < Palette_Num; ++Index) bAny |= Palettes[Index].IsValid();
	if (!bAny) return false;

	// 渲染线程一趟做完。Work 按**值**捕获（`TRefCountPtr` 拷贝即加引用），录完直接 return。
	ENQUEUE_RENDER_COMMAND(CSHouseVinePack)(
		[Records = MoveTemp(Flat), Work = Palettes, WorldToComponent]
		(FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSHouseVine.Pack"));

			for (int32 Index = 0; Index < Palette_Num; ++Index)
			{
				if (!Work[Index].IsValid()) continue;
				FRDGBufferRef PackedRef = GraphBuilder.RegisterExternalBuffer(Work[Index].PackedInstances, TEXT("CSHouseVine.PackedInstances"));
				FRDGBufferRef CounterRef = GraphBuilder.RegisterExternalBuffer(Work[Index].Counter, TEXT("CSHouseVine.Counter"));
				FRDGBufferUAVRef PackedUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(PackedRef, PF_A32B32G32R32F));
				FRDGBufferUAVRef CounterUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CounterRef, PF_R32_UINT));

				const TArray<FVector4f>& Rows = Records[Index];
				const uint32 Count = uint32(Rows.Num() / 3);
				// 空调色板必须显式清零：kernel 一个线程都不跑的话 counter 会留着上一次的值，
				// 症状是"藤已经没了但画面上还在"，而且只在从有到无那一次出现。
				if (Count == 0)
				{
					AddClearUAVPass(GraphBuilder, CounterUAV, 0u);
					GraphBuilder.SetBufferAccessFinal(PackedRef, ERHIAccess::SRVMask);
					GraphBuilder.SetBufferAccessFinal(CounterRef, ERHIAccess::SRVMask);
					continue;
				}

				CSHelper::FRDGStructuredBufferRefs RecordRefs = CSHelper::CreateUploadedStructuredBuffer<FVector4f>(
					GraphBuilder, Rows, TEXT("CSHouseVine.Records"), false, true);
				if (!RecordRefs.SRV) continue;

				FCSHouseVinePackCS::FParameters* PassParams = GraphBuilder.AllocParameters<FCSHouseVinePackCS::FParameters>();
				PassParams->VineRecords = RecordRefs.SRV;
				PassParams->RWVineInstances = PackedUAV;
				PassParams->RWVineCounter = CounterUAV;
				PassParams->VineWorldToComponent = WorldToComponent;
				PassParams->VineBaseSphereCentre = Work[Index].BaseSphereCentre;
				PassParams->VineBlockSize = Work[Index].BlockSize;
				PassParams->VineBaseSphereRadius = Work[Index].BaseSphereRadius;
				PassParams->VineRecordCount = Count;
				PassParams->VineMaxInstances = Work[Index].Capacity;

				TShaderMapRef<FCSHouseVinePackCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSHouseVine.Pack"), Shader, PassParams,
					FComputeShaderUtils::GetGroupCount(int32(Count), CSHouseVine_GroupSize));

				// 剔除 pass 只读这两个 buffer，且明说不负责恢复它们的状态 —— producer 自己留在 SRVMask。
				GraphBuilder.SetBufferAccessFinal(PackedRef, ERHIAccess::SRVMask);
				GraphBuilder.SetBufferAccessFinal(CounterRef, ERHIAccess::SRVMask);
			}

			GraphBuilder.Execute();
		});

	return true;
}
}
