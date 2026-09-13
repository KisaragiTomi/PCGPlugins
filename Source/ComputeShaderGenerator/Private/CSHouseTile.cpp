#include "CSHouseTile.h"

#include "CSHouseProfile.h"         // CSHouse_GetEdge：边号 → 边框架（外法线 = −In），唯一真源
#include "CSHouseVine.h"            // IdentityHash / Hash01：逐实例随机的身份哈希，别再造一份
#include "ComputeShaderGenerateHelper.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ShaderParameterStruct.h"

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSHouseTile_ 前缀
//（与 CSHouseVine.cpp 的 CSHouseVine_、CSHouseDecor.cpp 的 CSHouseDecor_ 都不同）。

constexpr int32 CSHouseTile_GroupSize = 64;

/** 排数 / 列数的硬上限：极端参数（间距被设成 0.01）下不该让容量与循环无界增长。 */
constexpr int32 CSHouseTile_MaxRows = 256;
constexpr int32 CSHouseTile_MaxColumns = 512;

class FCSHouseTilePackCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSHouseTilePackCS);
	SHADER_USE_PARAMETER_STRUCT(FCSHouseTilePackCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, TileRecords)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RWTileInstances)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWTileCounter)
		SHADER_PARAMETER(FMatrix44f, TileWorldToComponent)
		SHADER_PARAMETER(FVector3f, TileBaseSphereCentre)
		SHADER_PARAMETER(FVector3f, TileBlockSize)
		SHADER_PARAMETER(float, TileBaseSphereRadius)
		SHADER_PARAMETER(uint32, TileRecordCount)
		SHADER_PARAMETER(uint32, TileMaxInstances)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), CSHouseTile_GroupSize);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCSHouseTilePackCS, "/Plugin/PCGPlugins/Shaders/Private/CSHouseTile.usf", "PackHouseTileCS", SF_Compute);

/** 记录数组 → 上传用的 float4 平铺。布局与 `CSHouseTile.usf` 文件头逐字对应。 */
void CSHouseTile_Flatten(const TArray<CSHouseTile::FRecord>& In, TArray<FVector4f>& Out)
{
	Out.Reset(In.Num() * 4);
	for (const CSHouseTile::FRecord& R : In)
	{
		Out.Add(FVector4f(R.WorldPos.X, R.WorldPos.Y, R.WorldPos.Z, R.Random01));
		Out.Add(FVector4f(R.AxisX.X, R.AxisX.Y, R.AxisX.Z, R.SizeX));
		Out.Add(FVector4f(R.AxisY.X, R.AxisY.Y, R.AxisY.Z, R.SizeY));
		Out.Add(FVector4f(R.AxisZ.X, R.AxisZ.Y, R.AxisZ.Z, R.SizeZ));
	}
}

/**
 * 轴排列的兜底：三根轴必须互不相同，否则基会退化成奇异矩阵（画出来是一片拉到无穷远的黑面）。
 * 自动判定失手或蓝图里手填错时退回 (上坡 X / 沿排 Y / 法线 Z)，画得对不对另说，至少画得出来。
 */
CSHouseTile::FMeshAxes CSHouseTile_SanitizeAxes(const CSHouseTile::FMeshAxes& In)
{
	CSHouseTile::FMeshAxes Out = In;
	Out.UpSlope = FMath::Clamp(Out.UpSlope, 0, 2);
	Out.AlongRow = FMath::Clamp(Out.AlongRow, 0, 2);
	Out.Normal = FMath::Clamp(Out.Normal, 0, 2);
	if (Out.UpSlope == Out.AlongRow || Out.UpSlope == Out.Normal || Out.AlongRow == Out.Normal)
	{
		Out.UpSlope = 0;
		Out.AlongRow = 1;
		Out.Normal = 2;
	}
	if (!(FMath::Abs(Out.NormalSign) > 0.0f)) Out.NormalSign = 1.0f;
	return Out;
}

/** 有效排距 cm。≤ 0 时由网格尺寸反解：**瓦按原尺寸画，排距把它压出 RowOverlap 那么多重叠**。 */
float CSHouseTile_RowPitch(const CSHouseTile::FParams& Params)
{
	if (Params.RowPitch > 0.0f) return Params.RowPitch;
	const float Native = FMath::Max(Params.Axes.NativeAlongSlope(), 1.0f);
	return FMath::Max(Native / FMath::Max(Params.RowOverlap, 0.05f), 1.0f);
}

/** 有效列距 cm。同上。 */
float CSHouseTile_ColumnPitch(const CSHouseTile::FParams& Params)
{
	if (Params.ColumnPitch > 0.0f) return Params.ColumnPitch;
	const float Native = FMath::Max(Params.Axes.NativeAcrossRow(), 1.0f);
	return FMath::Max(Native / FMath::Max(Params.ColumnOverlap, 0.05f), 1.0f);
}

/**
 * 坡长（檐口外沿 → 屋面最高处，沿坡面量），所有坡面共用同一个值（同坡度、同外挑）。
 * 到不了最高处的坡面（矩形的短边面、多边形上先收尖的面）在后面几排自然区间为空。
 */
float CSHouseTile_SlopeLength(const FCSRoofDesc& Roof, double MaxInset)
{
	const float CosP = FMath::Max(Roof.CosPitch(), UE_KINDA_SMALL_NUMBER);
	return (float(MaxInset) + FMath::Max(Roof.Overhang, 0.0f)) / CosP;
}
}

namespace CSHouseTile
{
void BuildPlan(const FCSRoofDesc& Roof, const FTransform& World, const FParams& InParams, TArray<FRecord>& OutTiles)
{
	OutTiles.Reset();

	const FCSHouseFootprint& Footprint = Roof.Footprint;
	const int32 NumFaces = Footprint.NumEdges();
	if (!Footprint.IsValidFootprint()) return;

	FCSRoofSkeleton Skeleton;
	CSHouseRoof_BuildSkeleton(Footprint, double(FMath::Max(Roof.Overhang, 0.0f)), Skeleton);
	// 最大内切圆半径不到 1 cm 的"屋顶"不铺（矩形时代的「任一半边 ≤ 1 cm」）。
	if (Skeleton.MaxInset <= 1.0) return;

	FParams Params = InParams;
	Params.Axes = CSHouseTile_SanitizeAxes(Params.Axes);

	const float CosP = FMath::Max(Roof.CosPitch(), UE_KINDA_SMALL_NUMBER);
	const float SinP = Roof.SinPitch();
	const float TanP = Roof.TanPitch();
	const float Overhang = FMath::Max(Roof.Overhang, 0.0f);
	const float SlopeLen = CSHouseTile_SlopeLength(Roof, Skeleton.MaxInset);
	if (SlopeLen <= 1.0f) return;

	// 排：沿坡面**等分**（目标间距只决定份数）。所有坡面共用同一份排数与排距 ⇒ 每一排的高度
	// 在每个面上逐位相同，角斜脊上两侧的瓦因此是对齐的，不会错半排。
	const int32 Rows = FMath::Clamp(FMath::RoundToInt(SlopeLen / CSHouseTile_RowPitch(Params)), 1, CSHouseTile_MaxRows);
	const double RowStep = double(SlopeLen) / double(Rows);
	const float ColumnPitch = CSHouseTile_ColumnPitch(Params);

	const int32 AxisUp = Params.Axes.UpSlope;
	const int32 AxisAlong = Params.Axes.AlongRow;
	const int32 AxisNormal = Params.Axes.Normal;

	// 手性：排列的奇偶 × 法线取向决定这组基是左手还是右手，左手基会把瓦**镜像**过去
	// （背面朝外、光照整个翻掉，而实例数与位置断言全绿）。在**沿排轴**上补一个符号掰回来 ——
	// 沿排方向本来就没有正反之分（瓦沿排是对称的），法线与上坡向都不能动。
	FVector Probe[3];
	Probe[AxisUp] = FVector(1.0, 0.0, 0.0);
	Probe[AxisAlong] = FVector(0.0, 1.0, 0.0);
	Probe[AxisNormal] = FVector(0.0, 0.0, double(Params.Axes.NormalSign) >= 0.0 ? 1.0 : -1.0);
	const double AlongSign = FVector::DotProduct(FVector::CrossProduct(Probe[0], Probe[1]), Probe[2]) < 0.0 ? -1.0 : 1.0;

	// ≤ 0 = 沿用网格原生厚度（旧行为）；> 0 = 直接给绝对厚度 cm。见 FParams::Thickness 的注释。
	const float Thickness = Params.Thickness > 0.0f
		? Params.Thickness
		: FMath::Max(Params.Axes.NativeThickness(), 0.1f);
	const float SizeScale = FMath::Max(Params.SizeScale, 0.01f);

	OutTiles.Reserve(MaxTilesBound(Roof, Params));

	for (int32 Side = 0; Side < NumFaces; ++Side)
	{
		// 外法线与沿边方向。⚠️ 沿边取 `(n.y, -n.x)` 而不是 `(-n.y, n.x)`：只有这一支能让
		// (上坡, 沿排, 法线) 成右手基，另一支差一个镜像。逆时针折线上它恰好是 −U。
		const FCSHouseEdgeFrame Edge = CSHouse_GetEdge(Side, Footprint, 0.0f);
		if (Edge.Len <= 0.0f) continue;
		const FVector2D Outward(-Edge.In.X, -Edge.In.Y);
		const FVector2D Along(Outward.Y, -Outward.X);

		// 局部（actor 空间）的三条轴。法线朝上外、上坡向指向屋脊、沿排水平。
		const FVector NormalLocal(Outward.X * SinP, Outward.Y * SinP, CosP);
		const FVector UpSlopeLocal(-Outward.X * CosP, -Outward.Y * CosP, SinP);
		const FVector AlongLocal(Along.X * AlongSign, Along.Y * AlongSign, 0.0);

		for (int32 Row = 0; Row < Rows; ++Row)
		{
			// 沿坡面的弧长（自檐口外沿起）→ 内距。d = −Overhang 是檐口外沿，d = MaxInset 是屋面最高处。
			const double SlopeS = (double(Row) + 0.5) * RowStep;
			const double Inset = -double(Overhang) + SlopeS * double(CosP);

			// 这一排在本坡面上的沿边区间（见 `CSHouseRoof_FaceSpanAtInset`）。矩形上是 [d, Len − d]，
			// 即"半宽随内距 1:1 收窄"；收到不足 1 cm 就是角上的尖，那一排没有瓦。
			double T0 = 0.0, T1 = 0.0;
			if (!CSHouseRoof_FaceSpanAtInset(Footprint, Side, Inset, T0, T1)) continue;
			const double Width = T1 - T0;
			if (Width <= 1.0) continue;
			const FVector2D RowBase = Edge.Start + Edge.In * Inset;

			const int32 Columns = FMath::Clamp(FMath::RoundToInt(Width / ColumnPitch), 1, CSHouseTile_MaxColumns);
			const double ColumnStep = Width / double(Columns);

			for (int32 Column = 0; Column < Columns; ++Column)
			{
				// 身份 = (面号, 排号, 列号, 佐料, 用户种子)。**刻意不含位置** —— 拖房子时
				// 屋面在动，位置派生的种子会让整片瓦在拖动过程里不停重掷（同藤蔓那条纪律）。
				const uint32 Id = CSHouseVine::IdentityHash(Side, Row, Column, 71u, Params.Seed);
				const float Random01 = CSHouseVine::Hash01(Id);
				const float ScaleJ = 1.0f + Params.ScaleJitter * (CSHouseVine::Hash01(
					CSHouseVine::IdentityHash(Side, Row, Column, 72u, Params.Seed)) - 0.5f) * 2.0f;
				const float YawJ = Params.YawJitter * (CSHouseVine::Hash01(
					CSHouseVine::IdentityHash(Side, Row, Column, 73u, Params.Seed)) - 0.5f) * 2.0f;
				const float LiftJ = Params.LiftJitter * (CSHouseVine::Hash01(
					CSHouseVine::IdentityHash(Side, Row, Column, 74u, Params.Seed)) - 0.5f) * 2.0f;

				// 列从 T1 端起排（沿 Along = −U 走），与矩形时代「U 从 −半宽起」同序 ⇒ 身份 (面, 排, 列) 不变。
				const FVector2D XY = RowBase + Edge.U * (T1 - (double(Column) + 0.5) * ColumnStep);
				const double Z = double(Roof.EaveZ) + double(TanP) * Inset;
				const FVector Local = FVector(XY.X, XY.Y, Z) + NormalLocal * (double(Params.StandOff) + double(LiftJ));

				// 绕法线抖一点朝向：(上坡, 沿排, 法线) 是右手基，绕第三根轴转 a 就是这两句。
				const double CosY = FMath::Cos(double(YawJ)), SinY = FMath::Sin(double(YawJ));
				const FVector UpJittered = UpSlopeLocal * CosY + AlongLocal * SinY;
				const FVector AlongJittered = AlongLocal * CosY - UpSlopeLocal * SinY;

				FVector Dirs[3];
				Dirs[AxisUp] = World.TransformVectorNoScale(UpJittered).GetSafeNormal();
				Dirs[AxisAlong] = World.TransformVectorNoScale(AlongJittered).GetSafeNormal();
				Dirs[AxisNormal] = World.TransformVectorNoScale(NormalLocal * double(Params.Axes.NormalSign)).GetSafeNormal();

				float Sizes[3];
				// 瓦画多大 = 实际间距 × 重叠系数：等分给出的实际间距逐排不同，瓦跟着变，
				// 屋面因此永远铺满 —— 拿目标间距去算的话最后一排/一列会露出底下的天空。
				// `SizeScale` 只乘平面内两轴：排距不动 ⇒ 瓦数不变，只是每片变大或变小。
				// 想改瓦数请调 RowPitch / ColumnPitch，那是另一件事。
				Sizes[AxisUp] = float(RowStep) * Params.RowOverlap * ScaleJ * SizeScale;
				Sizes[AxisAlong] = float(ColumnStep) * Params.ColumnOverlap * ScaleJ * SizeScale;
				Sizes[AxisNormal] = Thickness;

				// 枢轴补偿：实例变换把网格顶点 v 送到 `原点 + Σ v_i · 方向_i · 缩放_i`，
				// 所以要让**包围盒中心**落在 Local 上，原点得先把中心那一项减掉。
				// 枢轴本来就在中心的资产（NativeCentre = 0）这一步是恒等，一分钱不花。
				FVector Centred = World.TransformPosition(Local);
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					const double Scale = double(Sizes[Axis]) / double(FMath::Max(Params.Axes.NativeSize[Axis], 0.01f));
					Centred -= Dirs[Axis] * (double(Params.Axes.NativeCentre[Axis]) * Scale);
				}

				FRecord& Rec = OutTiles.AddDefaulted_GetRef();
				Rec.WorldPos = FVector3f(Centred);
				Rec.Random01 = Random01;
				Rec.AxisX = FVector3f(Dirs[0]);
				Rec.AxisY = FVector3f(Dirs[1]);
				Rec.AxisZ = FVector3f(Dirs[2]);
				Rec.SizeX = Sizes[0];
				Rec.SizeY = Sizes[1];
				Rec.SizeZ = Sizes[2];
			}
		}
	}

	// -------------------------------------------------------------------------
	// 角斜脊 / 屋脊的盖瓦（用户 2026-08-31：「瓦片交汇处有 mesh 进行遮蔽」）
	//
	// 同一瓦片沿骨架的每条弧收口（矩形上 = 四条角斜脊 + 一条屋脊）。TG 的原始资产含
	// roof_tile / lod1 / backface，不是三个可随机互换的瓦型；这里只复用经 VS 尺寸适配的可见瓦片。
	// 未从“没有 roof_ridge 文件名”推断原版的收口生成算法。下面是按参考图的 UE 适配。
	if (Params.RidgeCapScale > 0.0f)
	{
		// 一条脊线 = 起点 → 终点 + 该处两坡法线的角平分。
		struct FRidgeLine { FVector A; FVector B; FVector Normal; };
		TArray<FRidgeLine, TInlineAllocator<16>> Lines;

		// 角平分线是闭式的：两侧坡面的外法线是 (外法线_a · sinP, cosP) 与 (外法线_b · sinP, cosP)，
		// 和归一化即得。脊上两面对冲 ⇒ 恒为竖直向上。
		for (const FCSRoofSkeletonArc& Arc : Skeleton.Arcs)
		{
			const FCSHouseEdgeFrame FA = CSHouse_GetEdge(Arc.FaceA, Footprint, 0.0f);
			const FCSHouseEdgeFrame FB = CSHouse_GetEdge(Arc.FaceB, Footprint, 0.0f);
			const FVector N(-(FA.In.X + FB.In.X) * double(SinP), -(FA.In.Y + FB.In.Y) * double(SinP), 2.0 * double(CosP));
			Lines.Add({
				FVector(Arc.A.X, Arc.A.Y, double(Roof.EaveZ) + Arc.InsetA * double(TanP)),
				FVector(Arc.B.X, Arc.B.Y, double(Roof.EaveZ) + Arc.InsetB * double(TanP)),
				N.GetSafeNormal() });
		}

		const float CapPitch = CSHouseTile_ColumnPitch(Params);
		for (int32 Line = 0; Line < Lines.Num(); ++Line)
		{
			const FRidgeLine& L = Lines[Line];
			const FVector Delta = L.B - L.A;
			const double Length = Delta.Size();
			if (Length <= 1.0) continue;
			const FVector Dir = Delta / Length;

			// 与铺瓦同一条纪律：目标间距只决定份数，实际间距由**等分**给出 ⇒ 脊永远盖满，
			// 尺寸连续变化时不会在末端忽多忽少一块。
			const int32 Count = FMath::Clamp(FMath::RoundToInt(Length / double(CapPitch)), 1, CSHouseTile_MaxColumns);
			const double Step = Length / double(Count);

			for (int32 Index = 0; Index < Count; ++Index)
			{
				// 身份用 Side = 面数 + 线号，与坡面的 (Side, Row, Column) 不会撞（矩形上就是原来的 4 + 线号）。
				const uint32 Id = CSHouseVine::IdentityHash(NumFaces + Line, Index, 0, 71u, Params.Seed);
				const float Random01 = CSHouseVine::Hash01(Id);
				const float ScaleJ = 1.0f + Params.ScaleJitter * (CSHouseVine::Hash01(
					CSHouseVine::IdentityHash(NumFaces + Line, Index, 0, 72u, Params.Seed)) - 0.5f) * 2.0f;

				const FVector LocalPos = L.A + Dir * ((double(Index) + 0.5) * Step)
					+ L.Normal * double(Params.StandOff + Thickness);

				// 瓦的顺坡轴沿脊搭接；宽度跨脊。中心抬一个包围盒厚度，避免坡面瓦穿出盖瓦。
				FVector Dirs[3];
				Dirs[Params.Axes.UpSlope] = World.TransformVectorNoScale(Dir).GetSafeNormal();
				Dirs[Params.Axes.Normal] = World.TransformVectorNoScale(
					L.Normal * double(Params.Axes.NormalSign)).GetSafeNormal();
				Dirs[Params.Axes.AlongRow] = FVector::CrossProduct(
					Dirs[Params.Axes.Normal], Dirs[Params.Axes.UpSlope]).GetSafeNormal();

				// ⚠️ **基必须是右手的**，与四面循环那条 `AlongSign` 同一个理由：镜像基会让瓦
				// 背面朝外、光照整个翻掉，而位置、瓦数、包围盒断言全绿（`House.TileOnRoof`
				// 第一次跑就抓到 55 个左手基）。沿脊方向本来就没有正反之分（瓦沿排对称），
				// 所以掰它最省 —— 法线与上坡向都不能动。
				if (FVector::DotProduct(FVector::CrossProduct(Dirs[0], Dirs[1]), Dirs[2]) < 0.0)
				{
					Dirs[Params.Axes.AlongRow] = -Dirs[Params.Axes.AlongRow];
				}

				float Sizes[3];
				Sizes[Params.Axes.UpSlope] = float(Step) * Params.ColumnOverlap * ScaleJ * SizeScale * Params.RidgeCapScale;
				Sizes[Params.Axes.AlongRow] = ColumnPitch * 0.80f * ScaleJ * SizeScale * Params.RidgeCapScale;
				Sizes[Params.Axes.Normal] = Thickness;

				// 枢轴补偿：与铺瓦逐字同一段（实例变换是 `原点 + Σ v_i·方向_i·缩放_i`）。
				FVector Centred = World.TransformPosition(LocalPos);
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					const double Scale = double(Sizes[Axis]) / double(FMath::Max(Params.Axes.NativeSize[Axis], 0.01f));
					Centred -= Dirs[Axis] * (double(Params.Axes.NativeCentre[Axis]) * Scale);
				}

				FRecord& Rec = OutTiles.AddDefaulted_GetRef();
				Rec.WorldPos = FVector3f(Centred);
				Rec.Random01 = Random01;
				Rec.AxisX = FVector3f(Dirs[0]);
				Rec.AxisY = FVector3f(Dirs[1]);
				Rec.AxisZ = FVector3f(Dirs[2]);
				Rec.SizeX = Sizes[0];
				Rec.SizeY = Sizes[1];
				Rec.SizeZ = Sizes[2];
			}
		}
	}
}

int32 MaxTilesBound(const FCSRoofDesc& Roof, const FParams& InParams)
{
	const FCSHouseFootprint& Footprint = Roof.Footprint;
	if (!Footprint.IsValidFootprint()) return 0;

	const float Overhang = FMath::Max(Roof.Overhang, 0.0f);
	FCSRoofSkeleton Skeleton;
	CSHouseRoof_BuildSkeleton(Footprint, double(Overhang), Skeleton);
	if (Skeleton.MaxInset <= 1.0) return 0;

	FParams Params = InParams;
	Params.Axes = CSHouseTile_SanitizeAxes(Params.Axes);

	const float SlopeLen = CSHouseTile_SlopeLength(Roof, Skeleton.MaxInset);
	// +1 是等分那一步的取整余量（`RoundToInt` 最多多给半格）。
	const int32 Rows = FMath::Clamp(FMath::CeilToInt(SlopeLen / CSHouseTile_RowPitch(Params)) + 1, 1, CSHouseTile_MaxRows);
	const float ColumnPitch = CSHouseTile_ColumnPitch(Params);

	int32 Total = 0;
	for (int32 Side = 0; Side < Footprint.NumEdges(); ++Side)
	{
		// 最宽的一排就是檐口外沿那一排：凸折线上每个坡面的排宽随内距单调收窄。
		double T0 = 0.0, T1 = 0.0;
		if (!CSHouseRoof_FaceSpanAtInset(Footprint, Side, -double(Overhang), T0, T1)) continue;
		const int32 Columns = FMath::Clamp(
			FMath::CeilToInt((T1 - T0) / double(ColumnPitch)) + 1, 1, CSHouseTile_MaxColumns);
		Total += Rows * Columns;
	}

	// 脊瓦：骨架的每条弧（矩形上 4 条角斜脊 + 1 条屋脊）。容量是**一次预留、超了截断**（零阻塞纪律），
	// 漏算这一段的症状是"屋脊末端少几块盖瓦"，而瓦数、零阻塞、三角数全都正常 —— 只有出图看得见。
	if (InParams.RidgeCapScale > 0.0f)
	{
		const double TanP = double(Roof.TanPitch());
		for (const FCSRoofSkeletonArc& Arc : Skeleton.Arcs)
		{
			const double Length = FMath::Sqrt(FVector2D::DistSquared(Arc.A, Arc.B)
				+ FMath::Square((Arc.InsetB - Arc.InsetA) * TanP));
			Total += FMath::Clamp(FMath::CeilToInt(Length / double(ColumnPitch)) + 1, 1, CSHouseTile_MaxColumns);
		}
	}
	return Total;
}

bool Pack(const TArray<FRecord>& Records, const CSShaperSteps::FPaletteBuffers& Buffers, const FMatrix44f& WorldToComponent)
{
	if (!Buffers.IsValid()) return false;

	TArray<FVector4f> Flat;
	CSHouseTile_Flatten(Records, Flat);

	// 渲染线程一趟做完。Work 按**值**捕获（`TRefCountPtr` 拷贝即加引用），录完直接 return。
	ENQUEUE_RENDER_COMMAND(CSHouseTilePack)(
		[Rows = MoveTemp(Flat), Work = Buffers, WorldToComponent](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSHouseTile.Pack"));

			FRDGBufferRef PackedRef = GraphBuilder.RegisterExternalBuffer(Work.PackedInstances, TEXT("CSHouseTile.PackedInstances"));
			FRDGBufferRef CounterRef = GraphBuilder.RegisterExternalBuffer(Work.Counter, TEXT("CSHouseTile.Counter"));
			FRDGBufferUAVRef PackedUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(PackedRef, PF_A32B32G32R32F));
			FRDGBufferUAVRef CounterUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CounterRef, PF_R32_UINT));

			const uint32 RecordCount = uint32(Rows.Num() / 4);
			// 空计划必须显式清零：kernel 一个线程都不跑的话 counter 会留着上一次的值，
			// 症状是"屋顶已经没了但画面上还在"，而且只在从有到无那一次出现。
			if (RecordCount == 0)
			{
				AddClearUAVPass(GraphBuilder, CounterUAV, 0u);
				GraphBuilder.SetBufferAccessFinal(PackedRef, ERHIAccess::SRVMask);
				GraphBuilder.SetBufferAccessFinal(CounterRef, ERHIAccess::SRVMask);
				GraphBuilder.Execute();
				return;
			}

			CSHelper::FRDGStructuredBufferRefs RecordRefs = CSHelper::CreateUploadedStructuredBuffer<FVector4f>(
				GraphBuilder, Rows, TEXT("CSHouseTile.Records"), false, true);
			if (!RecordRefs.SRV)
			{
				GraphBuilder.Execute();
				return;
			}

			FCSHouseTilePackCS::FParameters* PassParams = GraphBuilder.AllocParameters<FCSHouseTilePackCS::FParameters>();
			PassParams->TileRecords = RecordRefs.SRV;
			PassParams->RWTileInstances = PackedUAV;
			PassParams->RWTileCounter = CounterUAV;
			PassParams->TileWorldToComponent = WorldToComponent;
			PassParams->TileBaseSphereCentre = Work.BaseSphereCentre;
			PassParams->TileBlockSize = Work.BlockSize;
			PassParams->TileBaseSphereRadius = Work.BaseSphereRadius;
			PassParams->TileRecordCount = RecordCount;
			PassParams->TileMaxInstances = Work.Capacity;

			TShaderMapRef<FCSHouseTilePackCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSHouseTile.Pack"), Shader, PassParams,
				FComputeShaderUtils::GetGroupCount(int32(RecordCount), CSHouseTile_GroupSize));

			// 剔除 pass 只读这两个 buffer，且明说不负责恢复它们的状态 —— producer 自己留在 SRVMask。
			GraphBuilder.SetBufferAccessFinal(PackedRef, ERHIAccess::SRVMask);
			GraphBuilder.SetBufferAccessFinal(CounterRef, ERHIAccess::SRVMask);

			GraphBuilder.Execute();
		});

	return true;
}
}
