#include "CSHouseFrame.h"

#include "CSGpuSharedLayout.ush"   // CS_HOUSE_FRAME_PATH_FLOAT4S / CS_HOUSE_FRAME_FLAG_*（与 CSHouseFrame.usf 同一份）

#include "ComputeShaderGenerateHelper.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ShaderParameterStruct.h"

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSHouseFrame_ 前缀
//（与 CSHouseDecor.cpp 的 CSHouseDecor_、CSGroundShaperSteps.cpp 的 CSShaperSteps_ 都不同）。

constexpr int32 CSHouseFrame_GroupSize = 64;

/** 逐路常量占几个 float4。与 `CSHouseFrame.usf` 共用 CSGpuSharedLayout.ush 里的同一份 #define ——
 *  以前是两侧各写一份字面量，改一处忘另一处的症状是砖全在别处而不报错（2026-09-07 审查 B3）。
 *  消费者只有本文件与那个 .usf（`FramePaths` 全工程只有它读），所以扩行是局部的 —— 与 packed
 *  **实例**行那 CS_GPU_INSTANCED_ROW_FLOAT4S 个 float4 完全是两码事，那份才是九家 + 剔除 pass 共用的契约。 */
constexpr int32 CSHouseFrame_PathStride = CS_HOUSE_FRAME_PATH_FLOAT4S;

/** Row4.x 的位。与 kernel 里那几个 `FRAME_FLAG_*` 是同一份 #define。 */
constexpr uint32 CSHouseFrame_FlagLeftJamb = CS_HOUSE_FRAME_FLAG_LEFT_JAMB;
constexpr uint32 CSHouseFrame_FlagRightJamb = CS_HOUSE_FRAME_FLAG_RIGHT_JAMB;
constexpr uint32 CSHouseFrame_FlagMidArc = CS_HOUSE_FRAME_FLAG_MID_ARC;
constexpr uint32 CSHouseFrame_FlagMidFlat = CS_HOUSE_FRAME_FLAG_MID_FLAT;

class FCSHouseFrameScatterCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSHouseFrameScatterCS);
	SHADER_USE_PARAMETER_STRUCT(FCSHouseFrameScatterCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, FramePaths)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RWFrameInstances)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWFrameCounter)
		SHADER_PARAMETER(FMatrix44f, FrameWorldToComponent)
		SHADER_PARAMETER(FVector3f, FrameBaseSphereCentre)
		SHADER_PARAMETER(FVector3f, FrameBlockSize)
		SHADER_PARAMETER(float, FrameBaseSphereRadius)
		SHADER_PARAMETER(uint32, FramePathCount)
		SHADER_PARAMETER(uint32, FrameBrickTotal)
		SHADER_PARAMETER(uint32, FrameMaxInstances)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), CSHouseFrame_GroupSize);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCSHouseFrameScatterCS, "/Plugin/PCGPlugins/Shaders/Private/CSHouseFrame.usf", "ScatterHouseFrameCS", SF_Compute);

/** 元素表 → 上传用的 float4 平铺。布局与 `CSHouseFrame.usf` 文件头逐字对应。 */
void CSHouseFrame_Flatten(const TArray<CSHouseFrame::FElement>& In, TArray<FVector4f>& Out)
{
	Out.Reset(In.Num() * CSHouseFrame_PathStride);
	for (const CSHouseFrame::FElement& E : In)
	{
		uint32 Flags = 0;
		if (E.Path.bLeftJamb) Flags |= CSHouseFrame_FlagLeftJamb;
		if (E.Path.bRightJamb) Flags |= CSHouseFrame_FlagRightJamb;
		if (E.Path.MidKind == CSHouseFrame::EMidKind::Arc) Flags |= CSHouseFrame_FlagMidArc;
		if (E.Path.MidKind == CSHouseFrame::EMidKind::Flat) Flags |= CSHouseFrame_FlagMidFlat;

		// Arc 存扫角、Flat 存长度：两者不会同时有意义，共用一个槽省一行 float4。
		const float MidMeasure = E.Path.MidKind == CSHouseFrame::EMidKind::Arc ? E.Path.MidSweep : E.Path.FlatLen;

		Out.Add(FVector4f(E.Frame.Origin.X, E.Frame.Origin.Y, E.Frame.Origin.Z, E.Path.CenterS));
		Out.Add(FVector4f(E.Frame.AxisU.X, E.Frame.AxisU.Y, E.Frame.AxisU.Z, E.Path.Radius));
		Out.Add(FVector4f(E.Frame.AxisV.X, E.Frame.AxisV.Y, E.Frame.AxisV.Z, E.Path.TopZ));
		Out.Add(FVector4f(E.Path.BaseZ, E.Path.LeftS, E.Path.RightS, MidMeasure));
		// 整数字段走位模式重解释（HLSL 侧 asuint）：float 尾数只到 24 bit，砖序号虽然远小于它，
		// 但"整数就该按整数传"能免掉将来容量放大后那种只在大数上出现的静默错位。
		Out.Add(FVector4f(
			*reinterpret_cast<const float*>(&Flags),
			*reinterpret_cast<const float*>(&E.BrickBegin),
			*reinterpret_cast<const float*>(&E.BrickCount),
			E.Pitch));
		Out.Add(FVector4f(E.HalfLen, E.LayoutScale,
			*reinterpret_cast<const float*>(&E.RandomBase), E.CrossScale));
		// [6] 剔除高度 | 端头剪切或角石网格尺寸倒数 | 同前 | 逐砖变化幅度。
		// QuoinScale 是判别字段：竖直包角无端头剪切，R6.yz 可承载基础网格尺寸。
		Out.Add(FVector4f(E.CullBelowZ,
			E.QuoinScale > 0.0f ? E.QuoinMeshInvSize.X : E.ShearAtS0,
			E.QuoinScale > 0.0f ? E.QuoinMeshInvSize.Y : E.ShearAtS1, E.Jitter));
		// [7] 分层抖动幅度（cm，≤ 0 = 等分）| 本条路的总弧长（kernel 拿它把 cm 归一化）| 角石比例 | 角石的 cos(转角/2)。
		Out.Add(FVector4f(E.SplitJitter, E.Path.TotalLen(), E.QuoinScale,
			E.QuoinScale > 0.0f ? E.QuoinHalfTurnCos : 0.0f));
	}
}
}

namespace CSHouseFrame
{
bool MakeOpeningPath(const FCSWallOpening& Opening, FPath& OutPath)
{
	OutPath = FPath();
	if (!Opening.IsValid()) return false;

	// **砖骑在 clip 场那条曲线上**，不是骑在任何折线上 —— 洞在画面上的边缘就是这个场
	// （墙板是实心盒，洞由材质逐像素 discard）。四个参数的对位见头文件。
	const FCSOpeningClipField Field = CSHouse_ComputeClipField(Opening);
	if (!Field.bValid || Field.InvHalfWidth <= UE_KINDA_SMALL_NUMBER) return false;
	const float HW = 1.0f / Field.InvHalfWidth;

	OutPath.CenterS = Field.CenterS;
	switch (Field.Shape)
	{
	case ECSOpeningShape::Circle:
		// 圆洞没有"门樘"这回事：一圈砖，起点在最左（θ=0），扫满 2π 回到起点。
		OutPath.Radius = HW;
		OutPath.TopZ = Field.RefZ;
		OutPath.BaseZ = Field.RefZ;
		OutPath.LeftS = OutPath.RightS = Field.CenterS - HW;
		OutPath.MidKind = EMidKind::Arc;
		OutPath.MidSweep = 2.0f * PI;
		break;

	case ECSOpeningShape::Rect:
	{
		const float HH = Field.InvScaleZ > UE_KINDA_SMALL_NUMBER ? 1.0f / Field.InvScaleZ : 0.0f;
		OutPath.TopZ = Field.RefZ + HH;
		OutPath.BaseZ = Field.RefZ - HH;
		OutPath.LeftS = Field.CenterS - HW;
		OutPath.RightS = Field.CenterS + HW;
		OutPath.MidKind = EMidKind::Flat;
		OutPath.FlatLen = 2.0f * HW;
		OutPath.bLeftJamb = true;
		OutPath.bRightJamb = true;
		break;
	}

	case ECSOpeningShape::Arch:
	default:
		OutPath.Radius = HW;
		// ⚠️ 起拱线要**夹到洞底以上**：clip 场的 `RefZ = Z1 − 半宽` 没有这个夹（拱的判据在
		// 拱脚线以下本来就无下界，见 FCSOpeningClipField 的注释），而砖路必须与
		// `CSHouse_SampleOpeningProfile` 的 `SpringZ = max(Z1 − R, Z0)` 同口径 ——
		// 不夹的话矮拱（Z1 − R < Z0）的圆心会掉到洞底以下，半圆整个沉进地里。
		OutPath.TopZ = FMath::Max(Field.RefZ, Opening.Z0);
		OutPath.BaseZ = Opening.Z0;
		OutPath.LeftS = Field.CenterS - HW;
		OutPath.RightS = Field.CenterS + HW;
		OutPath.MidKind = EMidKind::Arc;
		OutPath.MidSweep = PI;
		OutPath.bLeftJamb = true;
		OutPath.bRightJamb = true;
		break;
	}

	return OutPath.TotalLen() > UE_KINDA_SMALL_NUMBER;
}

bool MakePierPath(const FCSWallOpening& Left, const FCSWallOpening& Right, FPath& OutPath)
{
	OutPath = FPath();
	float Span = 0.0f, TopZ = 0.0f;
	// 跨度与墩顶只认这一份判据（"两侧都是落地的拱"、墩顶取两条起拱线的较低者）——
	// 房体那边的墩裁剪场也是从它出来的，两处各写一份就会出现"砖砌到 A 高度、灰泥裁到 B 高度"。
	if (!CSHouse_PierSpanBetween(Left, Right, Span, TopZ)) return false;
	if (TopZ <= UE_KINDA_SMALL_NUMBER) return false;

	const float Centre = (Left.S1() + Right.S0()) * 0.5f;
	OutPath.BaseZ = 0.0f;   // PierSpanBetween 已经判过两侧洞底都贴地
	OutPath.TopZ = TopZ;
	OutPath.LeftS = Centre;
	OutPath.RightS = Centre;
	OutPath.CenterS = Centre;
	OutPath.MidKind = EMidKind::None;
	OutPath.bLeftJamb = true;
	return OutPath.TotalLen() > UE_KINDA_SMALL_NUMBER;
}

int32 BuildEdgeElements(const FWallFrame& Frame, TArrayView<const FCSWallOpening> EdgeOpenings,
	const FBrickParams& Params, TArray<FElement>& InOutElements)
{
	const float Length = FMath::Max(Params.Length, 1.0f);
	const int32 MaxBricks = FMath::Max(Params.MaxBricks, 0);

	// 全局砖序号是**跨边连续**的（一次 dispatch 铺完整栋房子），所以起点要从已有元素接着数。
	int32 Cursor = 0;
	for (const FElement& Existing : InOutElements) Cursor = FMath::Max(Cursor, Existing.BrickBegin + Existing.BrickCount);
	const int32 Before = Cursor;

	auto Emit = [&](const FPath& Path)
	{
		const float Total = Path.TotalLen();
		// 半块砖都摆不下的路不值得占一条元素（同旧路的 `RawLength < FrameBrickLength * 0.5`）。
		if (Total < Length * 0.5f) return;

		float Scale = 0.0f;
		int32 Count = SolveRun(Total, Length, Params.Gap, Scale);
		if (Count <= 0 || Scale <= 0.0f) return;

		// **只截断，绝不扩容**：容量是注册期一次付清的常量，交互期扩容 = 一次阻塞刷新。
		Count = FMath::Min(Count, MaxBricks - Cursor);
		if (Count <= 0) return;

		FElement Element;
		Element.Path = Path;
		Element.Frame = Frame;
		Element.BrickBegin = Cursor;
		Element.BrickCount = Count;
		Element.Pitch = (Length + FMath::Max(Params.Gap, 0.0f)) * Scale;
		Element.HalfLen = Length * Scale * 0.5f;
		Element.LayoutScale = Scale;
		// 门框砖的随机数仍然**就是**砖的全局槽位（`RandomBase + 路内序号 ≡ 全局序号`）——
		// 引入 `RandomBase` 是给接缝用的，这条路逐位不变。
		Element.RandomBase = uint32(Cursor);
		InOutElements.Add(Element);
		Cursor += Count;
	};

	// 一块横截面放大的**单砖**（柱头 / 柱础）：不走 `SolveRun`，一块砖恰好铺满 [Z0, Z1]。
	auto EmitSlab = [&](float S, float Z0, float Z1)
	{
		const float H = Z1 - Z0;
		if (H <= UE_KINDA_SMALL_NUMBER || Cursor >= MaxBricks) return;
		FElement Element;
		Element.Path.BaseZ = Z0;
		Element.Path.TopZ = Z1;
		Element.Path.LeftS = Element.Path.RightS = Element.Path.CenterS = S;
		Element.Path.MidKind = EMidKind::None;
		Element.Path.bLeftJamb = true;
		Element.Frame = Frame;
		Element.BrickBegin = Cursor;
		Element.BrickCount = 1;
		Element.Pitch = 0.0f;
		Element.HalfLen = H * 0.5f;
		Element.LayoutScale = H / Length;
		Element.RandomBase = uint32(Cursor);
		Element.CrossScale = Params.CapitalScale;
		InOutElements.Add(Element);
		++Cursor;
	};

	for (int32 Index = 0; Index < EdgeOpenings.Num(); ++Index)
	{
		const FCSWallOpening& Opening = EdgeOpenings[Index];

		// **窗不出框砖**（2026-09-06 用户裁决「附属物自己持有 mesh」的直接后果）：窗的洞缘由
		// 附属物自带的预制框盖住（`ACSWindowMarker::OpeningMesh`），房子再沿洞缘砌一圈就是
		// **双份几何** —— TG 那边窗洞周围同样一条砖路都没有（卷二 §1.2：洞缘靠预制框兜底）。
		// 窗周围的砖头补全（曾经沿洞底补的窗台底边第四段）已于 2026-09-10 整条删除，不是留着待回滚。
		//
		// ⚠️ 代价（已知并接受）：属性面板 `Windows` 那份没有标记、没有网格 ⇒ 从此是**裸洞**。
		// 那条路在文档里一直写着是"授权 / 测试用的便利入口"，真正的来源是标记。
		FPath Path;
		if (Opening.Type != ECSOpeningType::Window && MakeOpeningPath(Opening, Path))
		{
			// 墩侧不出门樘：那一截由墩自己那条砖路砌，两拱各铺各的正是竖缝的成因（见头文件）。
			if (Opening.StyleFlags & CSHouse_StylePierBefore) Path.bLeftJamb = false;
			if (Opening.StyleFlags & CSHouse_StylePierAfter) Path.bRightJamb = false;
			Emit(Path);
		}

		// 墩由**左邻**那一拱产出，只产一次。两侧的样式位是 `ResolvePierSpans` 成对打的，
		// 这里仍然两个都验：只信一个的话，将来谁在中间插一个洞就会静默漏掉/重复一根墩。
		if ((Opening.StyleFlags & CSHouse_StylePierAfter) && EdgeOpenings.IsValidIndex(Index + 1)
			&& (EdgeOpenings[Index + 1].StyleFlags & CSHouse_StylePierBefore))
		{
			FPath Pier;
			if (MakePierPath(Opening, EdgeOpenings[Index + 1], Pier))
			{
				// 小石柱（2026-09-04，实拍 `tiny-glade-ref-twin-arch-pier.jpg`）：柱础 + 柱身 + 柱头。
				// 柱头/柱础是横截面放大的**单砖**，柱身仍是原来那条均匀砖路，三段同一个 S。
				// 柱头高度夹在墩高的 40% 内 —— 墩很矮时（起拱线压得很低）两块帽子不能把柱身挤没。
				const float Cap = FMath::Clamp(Params.CapitalHeight, 0.0f, Pier.JambLen() * 0.4f);
				if (Cap > UE_KINDA_SMALL_NUMBER && Params.CapitalScale > 1.0f)
				{
					EmitSlab(Pier.CenterS, Pier.BaseZ, Pier.BaseZ + Cap);
					FPath Shaft = Pier;
					Shaft.BaseZ += Cap;
					Shaft.TopZ -= Cap;
					Emit(Shaft);
					EmitSlab(Pier.CenterS, Pier.TopZ - Cap, Pier.TopZ);
				}
				else
				{
					Emit(Pier);
				}
			}
		}
	}

	return Cursor - Before;
}

bool Scatter(const TArray<FElement>& Elements, const TArray<CSShaperSteps::FPaletteBuffers>& Palettes,
	const FMatrix44f& WorldToComponent)
{
	if (Palettes.IsEmpty() || !Palettes[0].IsValid()) return false;

	int32 BrickTotal = 0;
	for (const FElement& E : Elements) BrickTotal = FMath::Max(BrickTotal, E.BrickBegin + E.BrickCount);

	TArray<FVector4f> Flat;
	CSHouseFrame_Flatten(Elements, Flat);

	// 渲染线程一趟做完：整栋房子**一个** dispatch。Work 按值捕获（`TRefCountPtr` 拷贝即加引用），
	// 录完 pass 直接 return，不阻塞 GT。
	ENQUEUE_RENDER_COMMAND(CSHouseFrameScatter)(
		[Paths = MoveTemp(Flat), Work = Palettes[0], WorldToComponent, BrickTotal](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSHouseFrame.Scatter"));

			FRDGBufferRef PackedRef = GraphBuilder.RegisterExternalBuffer(Work.PackedInstances, TEXT("CSHouseFrame.PackedInstances"));
			FRDGBufferRef CounterRef = GraphBuilder.RegisterExternalBuffer(Work.Counter, TEXT("CSHouseFrame.Counter"));
			FRDGBufferUAVRef PackedUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(PackedRef, PF_A32B32G32R32F));
			FRDGBufferUAVRef CounterUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CounterRef, PF_R32_UINT));

			// 一块砖都没有时必须**显式清零**：kernel 一个线程都不跑的话 counter 会留着上一次的值，
			// 症状是"拱已经合上了但砖还在"，而且只在从有到无那一次出现（同 CSHouseDecor::Pack）。
			if (Paths.IsEmpty() || BrickTotal <= 0)
			{
				AddClearUAVPass(GraphBuilder, CounterUAV, 0u);
			}
			else
			{
				CSHelper::FRDGStructuredBufferRefs PathRefs = CSHelper::CreateUploadedStructuredBuffer<FVector4f>(
					GraphBuilder, Paths, TEXT("CSHouseFrame.Paths"), false, true);
				if (PathRefs.SRV)
				{
					FCSHouseFrameScatterCS::FParameters* PassParams = GraphBuilder.AllocParameters<FCSHouseFrameScatterCS::FParameters>();
					PassParams->FramePaths = PathRefs.SRV;
					PassParams->RWFrameInstances = PackedUAV;
					PassParams->RWFrameCounter = CounterUAV;
					PassParams->FrameWorldToComponent = WorldToComponent;
					PassParams->FrameBaseSphereCentre = Work.BaseSphereCentre;
					PassParams->FrameBlockSize = Work.BlockSize;
					PassParams->FrameBaseSphereRadius = Work.BaseSphereRadius;
					PassParams->FramePathCount = uint32(Paths.Num() / CSHouseFrame_PathStride);
					PassParams->FrameBrickTotal = uint32(BrickTotal);
					PassParams->FrameMaxInstances = Work.Capacity;

					TShaderMapRef<FCSHouseFrameScatterCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSHouseFrame.Scatter"), Shader, PassParams,
						FComputeShaderUtils::GetGroupCount(BrickTotal, CSHouseFrame_GroupSize));
				}
			}

			// 剔除 pass 只读这两个 buffer，且明说不负责恢复它们的状态 —— producer 自己留在 SRVMask。
			GraphBuilder.SetBufferAccessFinal(PackedRef, ERHIAccess::SRVMask);
			GraphBuilder.SetBufferAccessFinal(CounterRef, ERHIAccess::SRVMask);

			GraphBuilder.Execute();
		});

	return true;
}
}
