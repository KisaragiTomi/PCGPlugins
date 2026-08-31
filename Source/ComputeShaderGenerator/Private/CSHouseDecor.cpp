#include "CSHouseDecor.h"

#include "ComputeShaderGenerateHelper.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ShaderParameterStruct.h"

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSHouseDecor_ 前缀
//（与 CSHouseVine.cpp 的 CSHouseVine_、CSHouseActor.cpp 的 CSHouse_ 都不同）。

constexpr int32 CSHouseDecor_GroupSize = 64;

class FCSHouseDecorPackCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSHouseDecorPackCS);
	SHADER_USE_PARAMETER_STRUCT(FCSHouseDecorPackCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, DecorRecords)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RWDecorInstances)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWDecorCounter)
		SHADER_PARAMETER(FMatrix44f, DecorWorldToComponent)
		SHADER_PARAMETER(FVector3f, DecorBaseSphereCentre)
		SHADER_PARAMETER(FVector3f, DecorBlockSize)
		SHADER_PARAMETER(float, DecorBaseSphereRadius)
		SHADER_PARAMETER(uint32, DecorRecordCount)
		SHADER_PARAMETER(uint32, DecorMaxInstances)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), CSHouseDecor_GroupSize);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCSHouseDecorPackCS, "/Plugin/PCGPlugins/Shaders/Private/CSHouseDecor.usf", "PackHouseDecorCS", SF_Compute);

/** 记录数组 → 上传用的 float4 平铺。布局与 `CSHouseDecor.usf` 文件头逐字对应。 */
void CSHouseDecor_Flatten(const TArray<CSHouseDecor::FRecord>& In, TArray<FVector4f>& Out)
{
	Out.Reset(In.Num() * 3);
	for (const CSHouseDecor::FRecord& R : In)
	{
		Out.Add(FVector4f(R.WorldPos.X, R.WorldPos.Y, R.WorldPos.Z, R.Scale));
		Out.Add(FVector4f(R.Facing.X, R.Facing.Y, R.Facing.Z, R.Random01));
		Out.Add(FVector4f(R.Up.X, R.Up.Y, R.Up.Z, R.ScaleZ));
	}
}

/** (s, z) 落在这面墙的某个洞里（外扩 Clearance）？门口净空就是靠这一条留出来的。 */
bool CSHouseDecor_InsideHole(const TArray<FCSWallOpening>& Openings, int32 EdgeIndex, float S, float Z, float Clearance)
{
	for (const FCSWallOpening& O : Openings)
	{
		if (O.EdgeIndex != EdgeIndex || !O.IsValid()) continue;
		if (S < O.S0() - Clearance || S > O.S1() + Clearance) continue;
		if (Z < O.Z0 - Clearance || Z > O.Z1 + Clearance) continue;
		return true;
	}
	return false;
}

const CSHouseVine::FWallStrip* CSHouseDecor_FindStrip(const TArray<CSHouseVine::FWallStrip>& Strips, int32 EdgeIndex)
{
	for (const CSHouseVine::FWallStrip& Strip : Strips)
	{
		if (Strip.EdgeIndex == EdgeIndex) return &Strip;
	}
	return nullptr;
}
}

namespace CSHouseDecor
{
float Hash01(uint32 H)
{
	H = ((H >> ((H >> 28) + 4u)) ^ H) * 277803737u;
	return float((H >> 22 ^ H) & 0xFFFFFFu) / float(0x1000000u);
}

uint32 IdentityHash(EFamily Family, int32 AnchorId, uint32 Salt, int32 Seed)
{
	// 常数与 `CSHouseVine::IdentityHash` / `CSGroundStairs.usf::CSStairs_CellSeed` 同一套 ——
	// 三条路的"身份"口径要一眼看得出同源。
	uint32 H = uint32(uint8(Family) + 1u) * 0x9E3779B1u;
	H ^= uint32(AnchorId + 1048576) * 0x85EBCA77u;
	H ^= Salt * 0x165667B1u;
	H ^= uint32(Seed) * 0x9E3779B9u;
	return H;
}

float FParams::FillChance(EFamily Family) const
{
	switch (Family)
	{
	case EFamily::Gate: return GateFillChance;
	case EFamily::WallFoot: return WallFootFillChance;
	case EFamily::Eave: return EaveFillChance;
	case EFamily::Ridge: return RidgeFillChance;
	case EFamily::Skirt: return SkirtFillChance;
	default: return 0.0f;   // 窗户那家恒不长（见头文件：等 C1）
	}
}

void BuildAnchors(const FSite& Site, const FParams& Params, TArray<FAnchor>& OutAnchors)
{
	OutAnchors.Reset();

	// 落高与道路排除。**两者都只在这里做** —— `BuildPlan` 必须是能进纯 CPU 单测的纯函数，
	// 而这两件事要读地面镜像。采样器缺席时退回"一律落在房底、谁都不排除"，测试就走那条。
	auto SeatAndAccept = [&Site, &Params](const FVector2D& WorldXY, FVector& OutLocation) -> bool
	{
		if (Site.SampleRoadWeight && Site.SampleRoadWeight(WorldXY) > Params.RoadReject) return false;
		const double Z = Site.SampleGroundZ ? double(Site.SampleGroundZ(WorldXY)) : Site.BaseZ;
		OutLocation = FVector(WorldXY.X, WorldXY.Y, Z);
		return true;
	};

	// -------------------------------------------------------------------------
	// ① 门 / 拱（TG 的 `add_autoclutter_around_gates`）
	// -------------------------------------------------------------------------
	for (const FCSWallOpening& O : Site.Openings)
	{
		if (!O.IsValid()) continue;
		// ⚠️ **窗户那两家（`add_autoclutter_{around,on}_windows`）本轮不实现** ——
		// D8 窗户整个卡在 C1，窗一扇都长不出来。这个 `continue` 就是那道预留的接口：
		// C1 拍板、窗落地之后，在这里按 `_flowerbed_locations` 的候选点表展开即可。
		if (O.Type != ECSOpeningType::Door) continue;

		const CSHouseVine::FWallStrip* Strip = CSHouseDecor_FindStrip(Site.Strips, O.EdgeIndex);
		if (!Strip || Strip->Length <= UE_KINDA_SMALL_NUMBER) continue;

		// 身份由**几何**导出：边号 + 量化到 10 cm 的沿边位置。用洞表下标的话，在 0 号边多开
		// 一个门会把后面所有洞推一位 ⇒ 全屋摆件重掷一遍（而且不会有任何断言报红）。
		const int32 GateBase = (O.EdgeIndex & 3) * 1000003 + FMath::Clamp(FMath::RoundToInt(O.CenterS / 10.0f), 0, 65535) * 4;

		for (int32 Side = 0; Side < 2; ++Side)
		{
			const float Sign = Side == 0 ? -1.0f : 1.0f;
			const float S = FMath::Clamp(O.CenterS + Sign * (O.HalfWidth() + Params.GateSideOffset),
				0.0f, Strip->Length);
			const FVector P = Strip->Origin + Strip->U * S + Strip->N * Params.GateStandOff;

			FAnchor Anchor;
			Anchor.Family = EFamily::Gate;
			Anchor.AnchorId = GateBase + Side;
			Anchor.Facing = Strip->N;
			Anchor.Up = FVector::UpVector;
			if (SeatAndAccept(FVector2D(P.X, P.Y), Anchor.Location)) OutAnchors.Add(Anchor);
		}

		// 门前引道两侧 —— **这就是"道路两侧"那一家**。它锚在门上，而门本身是道路推导出来的
		// （D6：地面顶点色 R 过阈的子段点亮成拱），所以不需要、也不允许去扫描道路栅格找路缘：
		// 那已经是一个场，属于 C2 挂起的那一半。落在路面上的那一个会被 `SeatAndAccept` 的
		// 道路权重挡掉，剩下的自然分列路的两侧。
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const float Sign = Side == 0 ? -1.0f : 1.0f;
			const FVector P = Strip->Origin + Strip->U * O.CenterS
				+ Strip->N * Params.GateApproachDistance
				+ Strip->U * (Sign * Params.GateApproachSpread);

			FAnchor Anchor;
			Anchor.Family = EFamily::Gate;
			Anchor.AnchorId = GateBase + 2 + Side;
			Anchor.Facing = -Strip->N;   // 引道上的摆件朝房子看，读起来像"路边摊冲着门"
			Anchor.Up = FVector::UpVector;
			if (SeatAndAccept(FVector2D(P.X, P.Y), Anchor.Location)) OutAnchors.Add(Anchor);
		}
	}

	// -------------------------------------------------------------------------
	// ② 墙脚
	// -------------------------------------------------------------------------
	{
		const float Spacing = FMath::Max(Params.WallFootSpacing, 20.0f);
		for (const CSHouseVine::FWallStrip& Strip : Site.Strips)
		{
			if (Strip.Length <= Spacing * 0.5f) continue;
			const int32 Count = FMath::Max(1, FMath::RoundToInt(Strip.Length / Spacing));
			const float Slot = Strip.Length / float(Count);

			for (int32 Index = 0; Index < Count; ++Index)
			{
				const float S = (float(Index) + 0.5f) * Slot;
				// 门口正前方要留净空 —— 不留的话一开门就是一堵桶。洞的 Z 区间对墙脚没有意义
				// （摆件贴地），所以只用 S 判，Z 传 0。
				if (CSHouseDecor_InsideHole(Site.Openings, Strip.EdgeIndex, S, 0.0f, Params.WallFootHoleClearance)) continue;

				const FVector P = Strip.Origin + Strip.U * S + Strip.N * Params.WallFootStandOff;
				FAnchor Anchor;
				Anchor.Family = EFamily::WallFoot;
				Anchor.AnchorId = (Strip.EdgeIndex & 3) * 65536 + Index;
				Anchor.Facing = Strip.N;
				Anchor.Up = FVector::UpVector;
				if (SeatAndAccept(FVector2D(P.X, P.Y), Anchor.Location)) OutAnchors.Add(Anchor);
			}
		}
	}

	// -------------------------------------------------------------------------
	// ③ 檐口 + ④ 屋脊（TG 的 `add_birdnests`，读集里有 `Query<&Roof>`）
	//
	// ⚠️ 高度**一律从 `CSHouseRoof.h` 的求值器取**，这里一条屋顶方程都不写（计划 D4：
	// 屋面方程一旦散开，铺瓦 / 铺梁 / 落窗谓词就会各写一份，彼此差一点点就穿帮）。
	// -------------------------------------------------------------------------
	{
		const FCSRoofDesc& Roof = Site.Roof;
		const double HalfRidgeOuter = Roof.RidgeLength() * 0.5 + Roof.Overhang;
		const double AcrossOuter = Roof.EaveOuter();
		const float EaveZ = CSHouseRoof_EaveOuterZ(Roof) + Params.RoofStandOff;

		const float EaveSpacing = FMath::Max(Params.EaveSpacing, 20.0f);
		const int32 EaveCount = FMath::Max(1, FMath::RoundToInt(2.0 * HalfRidgeOuter / EaveSpacing));
		const double EaveSlot = 2.0 * HalfRidgeOuter / double(EaveCount);

		for (int32 Side = 0; Side < 2; ++Side)
		{
			const double Sign = Side == 0 ? -1.0 : 1.0;
			for (int32 Index = 0; Index < EaveCount; ++Index)
			{
				const double Along = -HalfRidgeOuter + (double(Index) + 0.5) * EaveSlot;
				const FVector Local = Roof.RidgeToLocal(Along, Sign * AcrossOuter, EaveZ);

				FAnchor Anchor;
				Anchor.Family = EFamily::Eave;
				Anchor.AnchorId = Side * 65536 + Index;
				Anchor.Location = Site.World.TransformPosition(Local);
				Anchor.Facing = Site.World.TransformVectorNoScale(Roof.RidgeToLocal(0.0, Sign, 0.0)).GetSafeNormal();
				// 屋顶那两家**不落地、也不受道路排除**：鸟窝挂在檐口上，与地面无关。
				// 而且它们立直（Up = 世界上方向）而不是跟着坡歪 —— 歪着的鸟窝读起来像穿模。
				Anchor.Up = FVector::UpVector;
				OutAnchors.Add(Anchor);
			}
		}

		const double HalfRidge = Roof.RidgeLength() * 0.5;
		const float RidgeZ = CSHouseRoof_RidgeZ(Roof) + Params.RoofStandOff;
		const float RidgeSpacing = FMath::Max(Params.RidgeSpacing, 20.0f);
		const int32 RidgeCount = FMath::Max(1, FMath::RoundToInt(2.0 * HalfRidge / RidgeSpacing));
		const double RidgeSlot = 2.0 * HalfRidge / double(RidgeCount);

		for (int32 Index = 0; Index < RidgeCount; ++Index)
		{
			const double Along = -HalfRidge + (double(Index) + 0.5) * RidgeSlot;
			const FVector Local = Roof.RidgeToLocal(Along, 0.0, RidgeZ);

			FAnchor Anchor;
			Anchor.Family = EFamily::Ridge;
			Anchor.AnchorId = Index;
			Anchor.Location = Site.World.TransformPosition(Local);
			// 脊上的东西朝哪边纯属观感，用身份哈希掷 —— 全朝同一边会读出一条直线。
			const double Sign = Hash01(IdentityHash(EFamily::Ridge, Index, 5u, Params.Seed)) < 0.5f ? -1.0 : 1.0;
			Anchor.Facing = Site.World.TransformVectorNoScale(Roof.RidgeToLocal(0.0, Sign, 0.0)).GetSafeNormal();
			Anchor.Up = FVector::UpVector;
			OutAnchors.Add(Anchor);
		}
	}
}

void BuildPlan(const TArray<FAnchor>& Anchors, const FParams& Params,
	const TArray<FPaletteRange>& Ranges, int32 PaletteCount, FPlan& OutPlan)
{
	OutPlan.Reset(PaletteCount);
	if (PaletteCount <= 0 || Ranges.Num() < int32(EFamily::Count)) return;

	const float MinSpacingSq = FMath::Square(FMath::Max(Params.MinSpacing, 0.0f));

	// 已接受的位置。O(N²) 是**有意的**：N 的上界就是 `MaxRecordsBound`（周长 / 间距那几项），
	// 演示房子量级是几十。计划 D12 那张 occupancy 网格是为"全网格 tile-argmax"准备的，
	// 锚点这一半根本到不了需要它的规模，先加网格只会多一个能错的地方。
	TArray<FVector> Placed;
	Placed.Reserve(Anchors.Num());

	for (const FAnchor& Anchor : Anchors)
	{
		const FPaletteRange& Range = Ranges[int32(Anchor.Family)];
		// 这一家没配网格 ⇒ 一件都不长。窗户那家永远走这里（`Count` 恒 0）。
		if (Range.Count <= 0) continue;

		const uint32 Id = IdentityHash(Anchor.Family, Anchor.AnchorId, 17u, Params.Seed);
		if (Hash01(Id) >= Params.FillChance(Anchor.Family)) continue;

		// 最小间距：**先放的挤掉后放的**，所以顺序必须钉死。`BuildAnchors` 按
		// 家族 → 边号 → 沿边序号产出，是构件遍历序 —— 与计划 D12「层内处理顺序钉死为
		// 格坐标字典序」同一条纪律（同输入必同输出，diff 之后一件都不动）。
		bool bTooClose = false;
		for (const FVector& P : Placed)
		{
			if (FVector::DistSquared(P, Anchor.Location) < MinSpacingSq) { bTooClose = true; break; }
		}
		if (bTooClose) continue;
		Placed.Add(Anchor.Location);

		const int32 Palette = Range.First
			+ int32(IdentityHash(Anchor.Family, Anchor.AnchorId, 29u, Params.Seed) % uint32(Range.Count));
		if (!OutPlan.ByPalette.IsValidIndex(Palette)) continue;

		const float YawJitter = (Hash01(IdentityHash(Anchor.Family, Anchor.AnchorId, 37u, Params.Seed)) - 0.5f)
			* 2.0f * Params.YawJitter;
		const FVector Up = Anchor.Up.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
		const FVector Facing = FQuat(Up, YawJitter).RotateVector(Anchor.Facing);

		const float Jitter = 1.0f + (Hash01(IdentityHash(Anchor.Family, Anchor.AnchorId, 43u, Params.Seed)) - 0.5f)
			* 2.0f * Params.ScaleJitter;
		// 竖向只抖一半：整排箱子高矮差太多会读成"尺寸乱了"，而横向差一点只是"东西不一样大"。
		const float JitterZ = 1.0f + (Hash01(IdentityHash(Anchor.Family, Anchor.AnchorId, 53u, Params.Seed)) - 0.5f)
			* Params.ScaleJitter;

		FRecord Rec;
		Rec.WorldPos = FVector3f(Anchor.Location);
		Rec.Scale = FMath::Max(Params.BaseScale * Jitter, 0.01f);
		Rec.Facing = FVector3f(Facing);
		Rec.Up = FVector3f(Up);
		Rec.ScaleZ = FMath::Max(JitterZ, 0.01f);
		Rec.Random01 = Hash01(IdentityHash(Anchor.Family, Anchor.AnchorId, 71u, Params.Seed));
		OutPlan.ByPalette[Palette].Add(Rec);
	}
}

int32 MaxRecordsBound(const FVector2D& Footprint, float Overhang, float PierWidth, const FParams& Params)
{
	const double Perimeter = 2.0 * (FMath::Abs(Footprint.X) + FMath::Abs(Footprint.Y));

	// 门：一面墙最多能开几个洞由「墩宽 + 最小洞宽」定死（`PierWidth` 是相邻洞之间必须留的墩），
	// 每个洞出 4 个锚（两侧 + 引道两侧）。这样上界只依赖配置，不依赖当前有几扇门 ——
	// 依赖门数的话，画一笔路就可能越过一次容量台阶、当场付一次设备同步。
	const double GatePitch = FMath::Max(double(PierWidth) + 60.0, 60.0);
	const int32 GateAnchors = FMath::CeilToInt(Perimeter / GatePitch) * 4;

	const int32 WallFootAnchors = FMath::CeilToInt(Perimeter / FMath::Max(Params.WallFootSpacing, 20.0f)) + 4;

	// 脊向未知时取长轴作上界（`RidgeLength()` 只可能是 X 或 Y 之一）。
	const double RidgeSpan = FMath::Max(FMath::Abs(Footprint.X), FMath::Abs(Footprint.Y)) + 2.0 * FMath::Abs(Overhang);
	const int32 EaveAnchors = FMath::CeilToInt(2.0 * RidgeSpan / FMath::Max(Params.EaveSpacing, 20.0f)) + 4;
	const int32 RidgeAnchors = FMath::CeilToInt(RidgeSpan / FMath::Max(Params.RidgeSpacing, 20.0f)) + 4;

	return GateAnchors + WallFootAnchors + EaveAnchors + RidgeAnchors;
}

bool Pack(const FPlan& Plan, const TArray<CSShaperSteps::FPaletteBuffers>& Palettes, const FMatrix44f& WorldToComponent)
{
	const int32 Count = FMath::Min(Plan.ByPalette.Num(), Palettes.Num());
	if (Count <= 0) return false;

	TArray<TArray<FVector4f>> Flat;
	Flat.SetNum(Count);
	bool bAny = false;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		CSHouseDecor_Flatten(Plan.ByPalette[Index], Flat[Index]);
		bAny |= Palettes[Index].IsValid();
	}
	if (!bAny) return false;

	// 渲染线程一趟做完。Work 按**值**捕获（`TRefCountPtr` 拷贝即加引用），录完直接 return。
	ENQUEUE_RENDER_COMMAND(CSHouseDecorPack)(
		[Records = MoveTemp(Flat), Work = Palettes, WorldToComponent](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSHouseDecor.Pack"));

			for (int32 Index = 0; Index < Records.Num(); ++Index)
			{
				if (!Work.IsValidIndex(Index) || !Work[Index].IsValid()) continue;
				FRDGBufferRef PackedRef = GraphBuilder.RegisterExternalBuffer(Work[Index].PackedInstances, TEXT("CSHouseDecor.PackedInstances"));
				FRDGBufferRef CounterRef = GraphBuilder.RegisterExternalBuffer(Work[Index].Counter, TEXT("CSHouseDecor.Counter"));
				FRDGBufferUAVRef PackedUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(PackedRef, PF_A32B32G32R32F));
				FRDGBufferUAVRef CounterUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CounterRef, PF_R32_UINT));

				const uint32 RecordCount = uint32(Records[Index].Num() / 3);
				// 空 palette 必须显式清零：kernel 一个线程都不跑的话 counter 会留着上一次的值，
				// 症状是"摆件已经没了但画面上还在"，而且只在从有到无那一次出现。
				if (RecordCount == 0)
				{
					AddClearUAVPass(GraphBuilder, CounterUAV, 0u);
					GraphBuilder.SetBufferAccessFinal(PackedRef, ERHIAccess::SRVMask);
					GraphBuilder.SetBufferAccessFinal(CounterRef, ERHIAccess::SRVMask);
					continue;
				}

				CSHelper::FRDGStructuredBufferRefs RecordRefs = CSHelper::CreateUploadedStructuredBuffer<FVector4f>(
					GraphBuilder, Records[Index], TEXT("CSHouseDecor.Records"), false, true);
				if (!RecordRefs.SRV) continue;

				FCSHouseDecorPackCS::FParameters* PassParams = GraphBuilder.AllocParameters<FCSHouseDecorPackCS::FParameters>();
				PassParams->DecorRecords = RecordRefs.SRV;
				PassParams->RWDecorInstances = PackedUAV;
				PassParams->RWDecorCounter = CounterUAV;
				PassParams->DecorWorldToComponent = WorldToComponent;
				PassParams->DecorBaseSphereCentre = Work[Index].BaseSphereCentre;
				PassParams->DecorBlockSize = Work[Index].BlockSize;
				PassParams->DecorBaseSphereRadius = Work[Index].BaseSphereRadius;
				PassParams->DecorRecordCount = RecordCount;
				PassParams->DecorMaxInstances = Work[Index].Capacity;

				TShaderMapRef<FCSHouseDecorPackCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSHouseDecor.Pack"), Shader, PassParams,
					FComputeShaderUtils::GetGroupCount(int32(RecordCount), CSHouseDecor_GroupSize));

				// 剔除 pass 只读这两个 buffer，且明说不负责恢复它们的状态 —— producer 自己留在 SRVMask。
				GraphBuilder.SetBufferAccessFinal(PackedRef, ERHIAccess::SRVMask);
				GraphBuilder.SetBufferAccessFinal(CounterRef, ERHIAccess::SRVMask);
			}

			GraphBuilder.Execute();
		});

	return true;
}
}
