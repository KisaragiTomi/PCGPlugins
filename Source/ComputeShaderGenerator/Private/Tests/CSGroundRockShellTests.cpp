#include "CSGroundActor.h"
#include "CSGroundRockShell.h"
#include "CSGroundShaperActor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "Misc/AutomationTest.h"
#include "EditorAssetLibrary.h"
#include "MeshDescription.h"
#include "Tests/AutomationEditorCommon.h"

#if WITH_EDITOR

/**
 * 披挂岩壳（计划 D9 链 B）的验收。三条核心判据各占一个测试：
 *
 *   ① `RockShell.Contract`      —— 纯 CPU：默认值之间那几条**不成文但承重**的不等式。
 *   ② `RockShell.DrapesOnSlopes`—— 陡坡上出现 / 平地上不出现 / 高度跟住解析场 / **它真的画得出来**。
 *   ③ `RockShell.RoadSinksNotHides` —— 画路之后壳**连续下沉**而不是消失：量下沉量，不量三角数；
 *                                      且路缘是缓坡不是折痕（与模糊半径 0 的对照组比梯度）。
 *
 * ⚠️ ③ 的形状是刻意的。今天刚踩过：GPU 石阶的 `StairMesh` / `StairMaterial` 在两张演示关卡里
 * 一直是 NULL，石阶在画面里是一撮黑块，而单测 53/53、回归 55 条全绿 —— 因为验收全部走
 * readback 断言，而 **readback 证明的是"buffer 里有数"，对"画的是哪张网格、有没有材质"
 * 一个字都没说**。所以 ② 里有一条 `IsRockShellDrawable`，它检查的是渲染那一侧的每一环。
 */
namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSRockShellTest_ 前缀
// （与 CSStairsTest_ / CSShaperFieldTest_ / CSRockShell_ 必须都不同）。

/** 地面：128 格 × 50 cm = 64 m。中心 3200，塑形物影响半径 1400 ⇒ 内外都留得下平地。 */
constexpr int32 CSRockShellTest_Cells = 128;
constexpr float CSRockShellTest_CellSize = 50.0f;
constexpr double CSRockShellTest_Centre = CSRockShellTest_Cells * CSRockShellTest_CellSize * 0.5;

/**
 * 与演示关卡同一档。三个数字是**一条链**，单改其中一个会把本文件的断言拆掉：
 *
 *   胞腔 5.53 m ⇒ 裙边要装得下 ⇒ `Falloff = 800`；
 *   坡度软阈照抄 TG 的 smoothstep(0.75, 1.25) ⇒ max|∇h| = Lift × 1.5 / Falloff 必须过 0.75
 *   ⇒ `Lift ≥ 400`；要拿满 mask 则 `Lift ≥ 667`。取 700 ⇒ max|∇h| = 1.3125。
 *
 * ⚠️ 早先这里写的是 300（演示关卡的老值），max|∇h| 只有 0.5625，**整个土台都在阈下** ——
 * 实测只长出 178 个三角（全靠裙边噪声碰巧抄过阈）。断言还是绿的，但测的已经不是预期里那个制度。
 */
constexpr float CSRockShellTest_Radius = 600.0f;
constexpr float CSRockShellTest_Falloff = 800.0f;
constexpr float CSRockShellTest_Lift = 700.0f;

/**
 * 本档土台的最大坡度，= `Lift × 1.5 / Falloff`（剖面是 smoothstep，最大斜率 1.5）。
 *
 * **容差里必须有它**：表面起伏的幅度自 2026-08-31 起是「坡度 = 1 时的满幅」而不是常幅
 * （照 TG `displace:721`），所以壳自身厚度里那一项是 `NoiseAmount × 本值`，不是 `NoiseAmount`。
 * 上面那三个常数动了，这个数跟着动 —— 别把它写成字面量。
 */
constexpr double CSRockShellTest_MaxSlope = double(CSRockShellTest_Lift) * 1.5 / double(CSRockShellTest_Falloff);

ACSGroundActor* CSRockShellTest_SpawnGround(UWorld* World, UMaterialInterface* ShellMaterial)
{
	ACSGroundActor* Ground = World->SpawnActor<ACSGroundActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!Ground) return nullptr;
	Ground->NumCellsX = CSRockShellTest_Cells;
	Ground->NumCellsY = CSRockShellTest_Cells;
	Ground->CellSize = CSRockShellTest_CellSize;
	// 石阶关掉：本文件量的是岩壳，多一条 GPU 路只会多一份噪声与耗时。两者的互补关系由
	// ① 的纯 CPU 不等式守着，不需要在这里真的把石阶也摆出来。
	Ground->StairMesh = nullptr;
	Ground->bRockShell = true;
	Ground->RockShellMaterial = ShellMaterial;
	// 图案资产：C++ CDO 刻意留空（只硬编码引擎自带资产，见 ACSGroundActor::RockShellPatternMesh），
	// 演示关卡由 BP_TinyGladeGround 填。这里从那张蓝图的 CDO 抄一份，测试才与演示关卡吃同一张图案
	// （2026-09-02 资产迁进 /PCGPlugins/HouseTest 后 C++ 默认值被去掉，四条测试因此空手而归）。
	// 蓝图不在时保持空，让下面"图案抽得出来"的断言把话说清楚，而不是在这里静默跳过。
	if (Ground->RockShellPatternMesh.IsNull())
	{
		UClass* DemoClass = LoadClass<ACSGroundActor>(nullptr, TEXT("/PCGPlugins/HouseTest/BP_TinyGladeGround.BP_TinyGladeGround_C"));
		if (DemoClass) Ground->RockShellPatternMesh = GetDefault<ACSGroundActor>(DemoClass)->RockShellPatternMesh;
	}
	Ground->RebuildGroundMesh();
	return Ground;
}

ACSGroundShaperActor* CSRockShellTest_SpawnMound(UWorld* World, double X, double Y)
{
	ACSGroundShaperActor* Shaper = World->SpawnActor<ACSGroundShaperActor>(FVector(X, Y, 0.0), FRotator::ZeroRotator);
	if (!Shaper) return nullptr;
	Shaper->Radius = CSRockShellTest_Radius;
	Shaper->FalloffDistance = CSRockShellTest_Falloff;
	Shaper->LiftHeight = CSRockShellTest_Lift;
	Shaper->RebuildTerrain();
	return Shaper;
}

/** 沿直线落 N 笔。⚠️ 笔刷是 3D 球，坡面上必须自己 SampleHeight 贴地，否则整段落空。 */
void CSRockShellTest_PaintLine(ACSGroundActor* Ground, double X0, double Y0, double X1, double Y1, int32 Dabs)
{
	Ground->BeginPaintStroke();
	for (int32 Index = 0; Index <= Dabs; ++Index)
	{
		const double T = double(Index) / double(FMath::Max(Dabs, 1));
		const double X = FMath::Lerp(X0, X1, T);
		const double Y = FMath::Lerp(Y0, Y1, T);
		Ground->ApplyPaintStroke(FVector(X, Y, Ground->SampleHeight(FVector2D(X, Y))));
	}
	Ground->EndPaintStroke();   // 内部 FlushPaintToGpu(true) → RebuildStairs() → RebuildRockShell()
}

/**
 * 三角活着吗。
 *
 * ⚠️ **判据是"第 0 个顶点不是 NaN"，不是"这个顶点不是 NaN"**。kernel 关掉一个三角时**只写
 * 第 0 个顶点**一个 NaN（一个就够让整个三角在裁剪阶段出局，写入量省掉 8/9，同 TG），
 * 另外两个顶点原样留着上一次的值。逐顶点数 NaN 会把死三角的后两个顶点当成活的 ——
 * 实测就是这么错的：没有塑形物时逐顶点数出 99,196 个"活顶点"，
 * 而 148,794 − 99,196 = 49,598 正好是三角数，即**每个三角都死了**。
 */
bool CSRockShellTest_TriangleAlive(const TArray<FVector>& Positions, int32 Tri)
{
	const int32 Base = Tri * 3;
	return Positions.IsValidIndex(Base + 2) && !Positions[Base].ContainsNaN();
}

/** 材质：引擎基础材质 —— 判据是"非空"，不是"哪一张"，所以不碰项目资产。 */
UMaterialInterface* CSRockShellTest_LoadShellMaterial()
{
	return LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
}
}

// -----------------------------------------------------------------------------
// ① 默认值之间那几条承重的不等式（纯 CPU，读 CDO）
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSRockShellContractTest,
	"PCGPlugins.ComputeShaderGenerator.RockShell.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSRockShellContractTest::RunTest(const FString&)
{
	const ACSGroundActor* CDO = GetDefault<ACSGroundActor>();

	// **岩壳默认是开的，而且开关不是"材质为空"** —— 这一条直接来自石阶那个坑：
	// 拿资产是否为空兼任开关，就会在演示关卡里一直是空的而所有断言照绿。
	TestTrue(TEXT("岩壳默认开着"), CDO->bRockShell);
	// 图案资产的默认值**是空的**：C++ 只硬编码引擎自带资产，这张图案网格由
	// `/PCGPlugins/HouseTest/BP_TinyGladeGround` 填。CDO 上因此无路径可断言，
	// 「演示关卡里到底填没填」改由 `TinyGladeDemoRegression.py` 在关卡上验。

	// 坡度软阈：Hi 必须严格大于 Lo，否则 smoothstep 是 0/0 ⇒ 整片 mask 变 NaN ⇒
	// NaN 顺着 Relief 写进位置，症状是"壳整个消失"，而且看起来跟坡度判据毫无关系。
	TestTrue(TEXT("坡度软阈 Hi > Lo"), CDO->RockShellSlopeHi > CDO->RockShellSlopeLo);

	// **壳与石阶严格互补**（计划：石阶要 road > 阈、碎石要 road < 阈），但裁决五禁止在壳的
	// 显隐判据里出现 road —— 互补因此是靠"连续下沉在石阶阈值之前就走完"实现的：
	// road = 1/RoadFade 是壳眼里的足迹边缘，石阶开始出现的 StairRoadThreshold 必须落在足迹之内。
	// （2026-09-11 起足迹先糊开再压壳，边缘处只沉一半；石阶离边缘还隔着笔刷衰减带，那一截
	// 由 ③ 的实测兜着，这里只守不等式本身。）
	const float ShellFootprintAt = 1.0f / FMath::Max(CDO->RockShellRoadFade, 1.0f);
	TestTrue(
		FString::Printf(TEXT("壳的路足迹(%.3f) 早于石阶出现(%.3f)"), ShellFootprintAt, CDO->StairRoadThreshold),
		ShellFootprintAt < CDO->StairRoadThreshold);

	// 下沉量必须盖得住壳自身的起伏，否则路面上还会露出石头尖。
	// 三项：沿法线的厚度 + 表面起伏（`NoiseAmount` 是**坡度 = 1 时**的满幅，这里按 1 估）
	// + 基准偏移把壳浮起来的那一截（`BaseLift`，路上会翻成 −BaseSink，所以只算 Lift 一边）。
	const float OwnRelief = CDO->RockShellCellRelief + CDO->RockShellNoiseAmount
		+ CDO->RockShellChipAmount + CDO->RockShellBaseLift;
	TestTrue(
		FString::Printf(TEXT("下沉量 %.0f cm 盖得住壳自身起伏 %.0f cm"), CDO->RockShellRoadSink, OwnRelief),
		CDO->RockShellRoadSink > OwnRelief);

	// 厚度层不是可选项：调到 0，披挂出来的就只是一张贴着地形的毯子，读不成一块块石头。
	TestTrue(TEXT("沿法线的厚度层是开着的"), CDO->RockShellCellRelief > 0.0f);
	return true;
}

// -----------------------------------------------------------------------------
// ② 陡坡上出现 / 平地上不出现 / 高度跟住解析场 / 它真的画得出来
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSRockShellDrapeTest,
	"PCGPlugins.ComputeShaderGenerator.RockShell.DrapesOnSlopes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSRockShellDrapeTest::RunTest(const FString&)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	UMaterialInterface* ShellMaterial = CSRockShellTest_LoadShellMaterial();
	if (!TestNotNull(TEXT("岩壳材质"), ShellMaterial)) return false;

	ACSGroundActor* Ground = CSRockShellTest_SpawnGround(World, ShellMaterial);
	if (!TestNotNull(TEXT("Ground actor"), Ground)) return false;

	// 图案资产：**断言它在**，不是"没有就跳过"。跳过等于把这条验收删掉，
	// 而删掉之后没人会发现 —— 石阶那个坑就是这么长出来的。
	int32 Triangles = 0, UVChannels = 0;
	float MaxCellId = 0.0f, DirAgreement = 0.0f;
	bool bFlipWinding = false;
	const bool bPatternOk = Ground->GetRockShellPatternStats(Triangles, UVChannels, MaxCellId, bFlipWinding, DirAgreement);
	AddInfo(FString::Printf(TEXT("图案：%d 三角 / %d UV 通道 / CellId 上界 %.1f / 翻绕序 %s / dir 一致度 %.4f"),
		Triangles, UVChannels, MaxCellId, bFlipWinding ? TEXT("是") : TEXT("否"), DirAgreement));
	if (!TestTrue(TEXT("图案抽得出来（跑过 Scripts/TinyGladeImportRockShell.py + TinyGladeSetupRockShell.py 没有）"), bPatternOk))
	{
		return false;
	}
	// 「首次导入后必须核对的四项」的机读版（Docs/TinyGlade/CSRockShellPattern.md）。
	TestEqual(TEXT("展开后三角数 = 原件的 49,598"), Triangles, 49598);
	TestTrue(TEXT("UV 通道 ≥ 3（逐顶点胞腔数据没丢）"), UVChannels >= 3);
	// UV1.x 一旦被导入路径归一化到 0..1，cell_id 就废了，而且完全静默：全场退化成一个胞腔，
	// 壳看起来只是"没有块感"，没有任何报错。
	TestTrue(FString::Printf(TEXT("UV1.x 没有被归一化（CellId 上界 %.1f，期望 608）"), MaxCellId),
		MaxCellId > 600.0f);
	TestTrue(FString::Printf(TEXT("现算的 DirToCentroid 与烘焙件一致 %.4f（导入器没翻平面轴）"), DirAgreement),
		DirAgreement > 0.9f);

	ACSGroundShaperActor* Mound = CSRockShellTest_SpawnMound(World, CSRockShellTest_Centre, CSRockShellTest_Centre);
	if (!TestNotNull(TEXT("Shaper mound"), Mound)) return false;

	// ⚠️ **披挂契约要在「隆起中性」下量**（2026-08-31 用户规格上线后加的这一段）。
	// 「石头隆起」那一组（RiseMultiplier / RiseExtend / CellExpand）是**有意**把壳抬离地面的，
	// 与本用例要钉的「壳贴着坡面、偏差只等于壳自身厚度」正好相反 —— 两件事必须分开测，
	// 否则只能靠不停放宽容差来续命，而那等于把这条验收删掉。
	// 隆起本身在下面单独断言，量的是"抬起来了多少"，不是"贴得多紧"。
	const float SavedRiseMultiplier = Ground->RockShellRiseMultiplier;
	const float SavedRiseExtend = Ground->RockShellRiseExtend;
	const float SavedRiseNoise = Ground->RockShellRiseNoiseAmount;
	const float SavedCellExpand = Ground->RockShellCellExpand;
	const float SavedPatternScale = Ground->RockShellPatternScale;
	const float SavedReliefFloor = Ground->RockShellReliefFloor;
	// 基准偏移（TG `:563`）与「石头隆起」那一组是同一类东西 —— **有意**把壳整体推离坡面，
	// 所以同样归中性。它自己的判据在下面 ⓒ'' 单独立着。
	const float SavedBaseLift = Ground->RockShellBaseLift;
	const float SavedBaseSink = Ground->RockShellBaseSink;
	Ground->RockShellBaseLift = 0.0f;
	Ground->RockShellBaseSink = 0.0f;
	Ground->RockShellRiseMultiplier = 1.0f;
	Ground->RockShellRiseExtend = 0.0f;
	Ground->RockShellRiseNoiseAmount = 0.0f;
	Ground->RockShellCellExpand = 0.0f;
	// 图案缩放也归中性（原生 1.0）。**理由不是"调到能过"**：下面那条容差里的
	// 「镜像重建余量」是在原生密度下实测标定的，而它量的是 4000 个采样的**最大值** ——
	// `PatternScale = 0.35` 让活三角从 1,112 涨到 7,922，同一份重建误差在密得多的点集上
	// 取最大自然更差（实测 66 → 77.3 cm）。密度是别的用例的事，本条只钉"壳贴着坡面"。
	Ground->RockShellPatternScale = 1.0f;
	// 厚度的 mask 下限同样归中性：容差里的 `CellRelief` 那一项是按"厚度被 mask 压着"标定的，
	// 抬下限等于让更多顶点拿到接近满额的厚度，最大垂直距离随之上去（实测 0.5 ⇒ 83.9 cm）。
	Ground->RockShellReliefFloor = 0.0f;
	Ground->RebuildHeightsFromShapers();

	// 披挂 pass 必须真的跑过 —— 否则下面读到的只是分配时那份平的静止姿态，断言绿着却什么都没测
	// （同 GroundShaper.CpuGpuFieldParity 用 GetGpuDisplaceCount 防假绿的那一手）。
	TestTrue(FString::Printf(TEXT("披挂 pass 跑过（%d 次）"), Ground->GetRockShellDisplaceCount()),
		Ground->GetRockShellDisplaceCount() > 0);

	// **它真的画得出来**：组件注册/可见、网格绑定、常驻流已分配、包围盒有效、材质非空。
	// readback 对这几条一个字都说不了。
	FString Reason;
	TestTrue(FString::Printf(TEXT("岩壳会被画出来（%s）"), *Reason), Ground->IsRockShellDrawable(Reason));

	TArray<FVector> Positions;
	const int32 Count = Ground->DebugReadRockShellSync(Positions);
	AddInfo(FString::Printf(TEXT("回读 %d 个岩壳顶点"), Count));
	if (!TestEqual(TEXT("回读到的顶点数 = 三角数 × 3"), Count, Triangles * 3)) return false;

	// 分三个环带统计：台顶（平）/ 裙边（陡）/ 盘外远处（平）。
	// 判据只看"有没有活着的顶点"与"活着的顶点在不在该在的地方"。
	const FVector2D Centre(CSRockShellTest_Centre, CSRockShellTest_Centre);
	int32 LiveOnPlateau = 0, LiveOnSkirt = 0, LiveOnFlat = 0, LiveTotal = 0;
	double WorstDrapeError = 0.0;
	int32 DrapeSamples = 0;
	for (int32 Tri = 0; Tri < Triangles; ++Tri)
	{
		if (!CSRockShellTest_TriangleAlive(Positions, Tri)) continue;
		++LiveTotal;
		// 三角的位置取第 0 个顶点（活三角的三个顶点都被写过，任取一个即可代表它在哪个环带）。
		const FVector& P = Positions[Tri * 3];
		const double R = FVector2D::Distance(FVector2D(P.X, P.Y), Centre);
		if (R < CSRockShellTest_Radius * 0.6) ++LiveOnPlateau;
		else if (R > CSRockShellTest_Radius + CSRockShellTest_Falloff + 400.0) ++LiveOnFlat;
		else if (R > CSRockShellTest_Radius && R < CSRockShellTest_Radius + CSRockShellTest_Falloff) ++LiveOnSkirt;

		// 披挂必须贴着解析坡面（裁决三的构造保证）。
		//
		// 量的是**到坡面的垂直距离**，不是 Z 差。壳沿地形法线推出一个厚度，法线在陡坡上
		// 是斜的 ⇒ 顶点的 **XY 也跑了**，拿它新 XY 上的场高去减它的 Z，差里会多出
		// `坡度 × 水平位移`一项。实测过：直接比 Z 差得到 81.5 cm，而沿法线的位移上限
		// 只有 36 cm —— 多出来的全是这一项，而且裙边噪声还会把局部坡度抬得比剖面的
		// max|∇h| = Lift×1.5/Falloff 更陡，所以那个容差根本写不出一个封闭形式。
		// 除以 sqrt(1+|∇h|²) 就把那一项消掉，剩下的恰好就是壳的厚度。
		if (DrapeSamples < 4000 && R > CSRockShellTest_Radius && R < CSRockShellTest_Radius + CSRockShellTest_Falloff)
		{
			const double D = double(Ground->CellSize);
			const double Field = Ground->SampleHeight(FVector2D(P.X, P.Y));
			const double Gx = (Ground->SampleHeight(FVector2D(P.X + D, P.Y)) - Ground->SampleHeight(FVector2D(P.X - D, P.Y))) / (2.0 * D);
			const double Gy = (Ground->SampleHeight(FVector2D(P.X, P.Y + D)) - Ground->SampleHeight(FVector2D(P.X, P.Y - D))) / (2.0 * D);
			const double Perp = FMath::Abs(double(P.Z) - Field) / FMath::Sqrt(1.0 + Gx * Gx + Gy * Gy);
			WorstDrapeError = FMath::Max(WorstDrapeError, Perp);
			++DrapeSamples;
		}
	}
	AddInfo(FString::Printf(TEXT("活着的三角 %d：台顶 %d / 裙边 %d / 盘外远处 %d；到坡面的最大垂直距离 %.1f cm（%d 个采样）"),
		LiveTotal, LiveOnPlateau, LiveOnSkirt, LiveOnFlat, WorstDrapeError, DrapeSamples));

	// ⓐ 陡坡（裙边）上出现
	TestTrue(FString::Printf(TEXT("陡坡上长出岩壳（裙边活三角 %d）"), LiveOnSkirt), LiveOnSkirt > 100);
	// ⓑ 平地上不出现：台顶是平的（盘内恒为台高），盘外羽化完也是平的
	TestEqual(TEXT("平台顶上不长岩壳"), LiveOnPlateau, 0);
	TestEqual(TEXT("羽化之外的平地上不长岩壳"), LiveOnFlat, 0);
	// ⓒ 披挂真的贴在坡面上（不是浮在空中，也不是塌进地里）
	// 容差 = 壳自己的厚度（沿法线的 Relief + 表面起伏）+ 镜像重建的余量：
	// SampleHeight 是 50 cm 格上的双线性重建，而 kernel 读的是解析场，曲面在格中间本来就会下垂。
	//
	// ⚠️ 起伏那一项是 `NoiseAmount × 坡度` 而不是 `NoiseAmount`（2026-08-31 起幅度 ∝ 坡度，
	// 照 TG `displace:721`）。写成常数会低估上界，本条就会在陡坡上莫名其妙地红。
	const double DrapeTolerance =
		double(Ground->RockShellCellRelief)
		+ double(Ground->RockShellNoiseAmount) * CSRockShellTest_MaxSlope
		+ double(Ground->RockShellChipAmount)
		+ 30.0;
	TestTrue(FString::Printf(TEXT("披挂贴住解析坡面（到面垂直距离 %.1f cm ≤ %.1f）"), WorstDrapeError, DrapeTolerance),
		DrapeSamples > 0 && WorstDrapeError <= DrapeTolerance);

	// ⓒ' 「石头隆起」（用户规格 ③）：把倍数打开，同一批裙边顶点必须**整体抬高**。
	//
	// 量的是同一组采样在两种配置下的 Z 均值之差，而不是绝对高度 —— 绝对值受壳厚、噪声、
	// 逐胞腔浮高影响，差值把它们全消掉，剩下的只有隆起本身。
	// 倍数写死 2.0 而不是读属性：读属性的话，谁把默认值调回 1.0 这条就变成恒真的空判据。
	double SumFlat = 0.0;
	int32 NumFlat = 0;
	for (int32 Tri = 0; Tri < Triangles; ++Tri)
	{
		if (!CSRockShellTest_TriangleAlive(Positions, Tri)) continue;
		const FVector& P = Positions[Tri * 3];
		const double R = FVector2D::Distance(FVector2D(P.X, P.Y), Centre);
		if (R > CSRockShellTest_Radius && R < CSRockShellTest_Radius + CSRockShellTest_Falloff)
		{
			SumFlat += double(P.Z);
			++NumFlat;
		}
	}

	Ground->RockShellRiseMultiplier = 2.0f;
	Ground->RebuildHeightsFromShapers();
	TArray<FVector> Raised;
	Ground->DebugReadRockShellSync(Raised);

	double SumRaised = 0.0;
	int32 NumRaised = 0;
	for (int32 Tri = 0; Tri < Triangles; ++Tri)
	{
		if (!CSRockShellTest_TriangleAlive(Raised, Tri)) continue;
		const FVector& P = Raised[Tri * 3];
		const double R = FVector2D::Distance(FVector2D(P.X, P.Y), Centre);
		if (R > CSRockShellTest_Radius && R < CSRockShellTest_Radius + CSRockShellTest_Falloff)
		{
			SumRaised += double(P.Z);
			++NumRaised;
		}
	}

	const double MeanFlat = NumFlat > 0 ? SumFlat / double(NumFlat) : 0.0;
	if (NumFlat > 0 && NumRaised > 0)
	{
		const double MeanRaised = SumRaised / double(NumRaised);
		AddInfo(FString::Printf(TEXT("隆起：倍数 1.0 时裙边均高 %.1f cm（%d 点），倍数 2.0 时 %.1f cm（%d 点）"),
			MeanFlat, NumFlat, MeanRaised, NumRaised));
		// 裙边的场高在 0..台高 之间，倍数从 1 到 2 至少要抬起几十 cm 才算真起作用。
		TestTrue(FString::Printf(TEXT("隆起倍数把壳抬起来了（+%.1f cm）"), MeanRaised - MeanFlat),
			MeanRaised - MeanFlat > 20.0);
	}
	else
	{
		TestTrue(TEXT("两种配置下裙边都有采样点（否则上面那条是空判据）"), false);
	}

	// ⓒ'' TG `:563` 的**基准偏移**：把 `BaseLift` 打开，同一批裙边顶点同样必须整体抬高。
	//
	// 与 ⓒ' 是两条独立的体积来源，必须分开测：③ 的倍数按**台高**放大（缓坡小土台上几乎给不出
	// 体积），本项是**有界常量**（多高的台子都只浮这么多）。只测其中一条的话，另一条被误删了
	// 也不会有人发现 —— 而"壳看着没有体积"正是这两条都缺时的症状。
	//
	// 倍数先归回 1.0，否则量到的是隆起而不是基准偏移。`BaseLift` 写死 100 而不是读属性，
	// 同 ⓒ' 的理由：读属性的话，谁把默认调成 0 这条就变成恒真的空判据。
	Ground->RockShellRiseMultiplier = 1.0f;
	Ground->RockShellBaseLift = 100.0f;
	Ground->RebuildHeightsFromShapers();
	TArray<FVector> Lifted;
	Ground->DebugReadRockShellSync(Lifted);

	double SumLifted = 0.0;
	int32 NumLifted = 0;
	for (int32 Tri = 0; Tri < Triangles; ++Tri)
	{
		if (!CSRockShellTest_TriangleAlive(Lifted, Tri)) continue;
		const FVector& P = Lifted[Tri * 3];
		const double R = FVector2D::Distance(FVector2D(P.X, P.Y), Centre);
		if (R > CSRockShellTest_Radius && R < CSRockShellTest_Radius + CSRockShellTest_Falloff)
		{
			SumLifted += double(P.Z);
			++NumLifted;
		}
	}

	if (NumFlat > 0 && NumLifted > 0)
	{
		const double MeanLifted = SumLifted / double(NumLifted);
		AddInfo(FString::Printf(TEXT("基准偏移：BaseLift 0 时裙边均高 %.1f cm（%d 点），100 cm 时 %.1f cm（%d 点）"),
			MeanFlat, NumFlat, MeanLifted, NumLifted));
		// 偏移按 `saturate(Rock − Road)` 淡入，裙边上 Rock 大多在 0.1~0.5 ⇒ 100 cm 到不了满额。
		// 20 cm 是"这一层真的接上了"的下界，不是它的标称值。
		TestTrue(FString::Printf(TEXT("基准偏移把壳浮起来了（+%.1f cm）"), MeanLifted - MeanFlat),
			MeanLifted - MeanFlat > 20.0);
	}
	else
	{
		TestTrue(TEXT("BaseLift 打开后裙边仍有采样点（否则上面那条是空判据）"), false);
	}

	Ground->RockShellBaseLift = SavedBaseLift;
	Ground->RockShellBaseSink = SavedBaseSink;
	Ground->RockShellRiseMultiplier = SavedRiseMultiplier;
	Ground->RockShellRiseExtend = SavedRiseExtend;
	Ground->RockShellRiseNoiseAmount = SavedRiseNoise;
	Ground->RockShellCellExpand = SavedCellExpand;
	Ground->RockShellPatternScale = SavedPatternScale;
	Ground->RockShellReliefFloor = SavedReliefFloor;
	Ground->RebuildHeightsFromShapers();

	// ⓒ''' **反火山口：默认档下壳的绝对形态必须是 TG 的**（2026-08-31 看图裁决后补的断言）。
	//
	// 这条缺口正是"顶圈高出台顶 2 m、读成一圈独立石墙"那个缺陷逃过全部断言的原因：
	// ⓒ 的披挂契约在**中性档**量、ⓒ' 只量倍数的**差值** —— 默认档的绝对高度从来没人量过。
	// TG 的壳从不离开地形（全部位移有界），所以判据是：恢复默认之后，**没有任何活顶点
	// 高过台顶 + 有界预算**。预算按属性现算，谁把默认调回"无界抬升"这里立刻红。
	{
		TArray<FVector> Defaults;
		Ground->DebugReadRockShellSync(Defaults);
		const double PlateauZ = Ground->SampleHeight(FVector2D(CSRockShellTest_Centre, CSRockShellTest_Centre));
		double WorstAbovePlateau = -TNumericLimits<double>::Max();
		int32 DefaultLive = 0;
		for (int32 Tri = 0; Tri < Triangles; ++Tri)
		{
			if (!CSRockShellTest_TriangleAlive(Defaults, Tri)) continue;
			++DefaultLive;
			WorstAbovePlateau = FMath::Max(WorstAbovePlateau, double(Defaults[Tri * 3].Z) - PlateauZ);
		}
		// 预算 = 基准浮起 + 顶圈浮高 + 表面起伏满幅（∝ 坡度）+ 40 cm 余量（镜像重建 + 半径淡入）。
		const double CrownBudget =
			double(Ground->RockShellBaseLift) + double(Ground->RockShellCellRelief)
			+ double(Ground->RockShellNoiseAmount) * CSRockShellTest_MaxSlope
			+ double(Ground->RockShellChipAmount) + 40.0;
		AddInfo(FString::Printf(TEXT("默认档：活三角 %d，最高点相对台顶 %+.1f cm（预算 %.1f）"),
			DefaultLive, WorstAbovePlateau, CrownBudget));
		TestTrue(FString::Printf(TEXT("默认档下没有石墙冠：最高点 ≤ 台顶 + %.0f cm（实测 %+.1f）"),
			CrownBudget, WorstAbovePlateau),
			DefaultLive > 100 && WorstAbovePlateau <= CrownBudget);
	}

	// ⓓ 删掉塑形物 ⇒ 高度场塌回 ⇒ 坡度降到阈下 ⇒ 那批胞腔自己写 NaN。
	// **裁决二的执行面**：没有一行注销代码，归属簿记整个消失。
	World->DestroyActor(Mound);
	Ground->RebuildHeightsFromShapers();
	Ground->DebugReadRockShellSync(Positions);
	int32 LiveAfter = 0;
	for (int32 Tri = 0; Tri < Triangles; ++Tri) { if (CSRockShellTest_TriangleAlive(Positions, Tri)) ++LiveAfter; }
	AddInfo(FString::Printf(TEXT("删掉塑形物之后活着的三角 %d / %d"), LiveAfter, Triangles));
	TestEqual(TEXT("删掉塑形物后岩壳自己消失（不需要任何注销代码）"), LiveAfter, 0);
	return true;
}

// -----------------------------------------------------------------------------
// ③ 画路之后壳**连续下沉**而不是消失（裁决五）
//
// 判据刻意**不看三角数**：用 NaN 关掉的话画路时三角会一个一个啪地消失（popping），
// 而那正是这条裁决要禁止的做法 —— 只数三角数的断言对"沉"和"消失"给出同样的绿灯。
// 所以这里同时量两件事：活顶点数**一个都不许少**，且路下的顶点**真的沉下去了**。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSRockShellRoadSinkTest,
	"PCGPlugins.ComputeShaderGenerator.RockShell.RoadSinksNotHides",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSRockShellRoadSinkTest::RunTest(const FString&)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	UMaterialInterface* ShellMaterial = CSRockShellTest_LoadShellMaterial();
	ACSGroundActor* Ground = CSRockShellTest_SpawnGround(World, ShellMaterial);
	if (!TestNotNull(TEXT("Ground actor"), Ground)) return false;

	int32 Triangles = 0, UVChannels = 0;
	float MaxCellId = 0.0f, DirAgreement = 0.0f;
	bool bFlipWinding = false;
	if (!TestTrue(TEXT("图案抽得出来"),
		Ground->GetRockShellPatternStats(Triangles, UVChannels, MaxCellId, bFlipWinding, DirAgreement)))
	{
		return false;
	}

	ACSGroundShaperActor* Mound = CSRockShellTest_SpawnMound(World, CSRockShellTest_Centre, CSRockShellTest_Centre);
	if (!TestNotNull(TEXT("Shaper mound"), Mound)) return false;

	TArray<FVector> Before;
	if (!TestTrue(TEXT("画路之前读得到岩壳"), Ground->DebugReadRockShellSync(Before) > 0)) return false;
	int32 LiveBefore = 0;
	for (int32 Tri = 0; Tri < Triangles; ++Tri) { if (CSRockShellTest_TriangleAlive(Before, Tri)) ++LiveBefore; }
	if (!TestTrue(FString::Printf(TEXT("画路之前壳是长出来的（%d 个活三角）"), LiveBefore), LiveBefore > 100)) return false;

	// 一条穿台的路。⚠️ 笔刷是 3D 球，必须贴地落笔。
	const double C = CSRockShellTest_Centre;
	const double Reach = CSRockShellTest_Radius + CSRockShellTest_Falloff;
	CSRockShellTest_PaintLine(Ground, C, C - Reach - 200.0, C, C + Reach + 200.0, 32);

	TArray<FVector> After;
	const int32 CountAfter = Ground->DebugReadRockShellSync(After);
	if (!TestEqual(TEXT("画路前后顶点数不变（常驻定长，永不重分配）"), CountAfter, Before.Num())) return false;

	int32 LiveAfter = 0;
	int32 SunkSamples = 0;
	double SumSink = 0.0, DeepestSink = 0.0;
	int32 OffRoadSamples = 0;
	double WorstOffRoadDrift = 0.0;
	// 路足迹糊开之后会往外伸一个模糊半径（2026-09-11），"路外"的界跟着外移同样的量。
	const double OffRoadX = double(Ground->BrushRadius + Ground->RockShellRoadBlurRadius + Ground->RockShellRoadSink) + 100.0;
	for (int32 Tri = 0; Tri < Triangles; ++Tri)
	{
		const bool bAliveAfter = CSRockShellTest_TriangleAlive(After, Tri);
		if (bAliveAfter) ++LiveAfter;
		// 只比**两次都活着**的三角：死三角的后两个顶点保留着上一次的值，把它们算进来会
		// 拿一堆恒为 0 的 ΔZ 把平均下沉量稀释掉（实测：−9.3 cm vs 真实值）。
		if (!bAliveAfter || !CSRockShellTest_TriangleAlive(Before, Tri)) continue;

		for (int32 K = 0; K < 3; ++K)
		{
			const int32 Index = Tri * 3 + K;
			// 顶点身份是稳定的（图案是烘死的，索引不动），所以逐下标比 Z 是有意义的。
			const double Delta = double(After[Index].Z) - double(Before[Index].Z);
			const double Road = Ground->SampleRoadWeight(FVector2D(Before[Index].X, Before[Index].Y));
			if (Road > 0.5)
			{
				SumSink += Delta;
				DeepestSink = FMath::Min(DeepestSink, Delta);
				++SunkSamples;
			}
			// ⚠️ **"路外"不能用顶点当前 XY 上的路权重来判**。下沉是沿**地形法线**的，
			// 法线在陡坡上是斜的 ⇒ 沉下去的顶点 XY 也跑了（最多 RoadSink × |N.xy|），
			// 跑完之后它可能落在路外，于是被当成"路外却动了大半米"（实测踩过）。
			// 按**路的几何**判：路是 x = Centre 的一条直线。
			else if (FMath::Abs(Before[Index].X - CSRockShellTest_Centre) > OffRoadX)
			{
				WorstOffRoadDrift = FMath::Max(WorstOffRoadDrift, FMath::Abs(Delta));
				++OffRoadSamples;
			}
		}
	}
	const double MeanSink = SunkSamples > 0 ? SumSink / double(SunkSamples) : 0.0;
	AddInfo(FString::Printf(TEXT("路下 %d 个顶点：平均 ΔZ %.1f cm、最深 %.1f cm；路外 %d 个顶点最大漂移 %.2f cm"),
		SunkSamples, MeanSink, DeepestSink, OffRoadSamples, WorstOffRoadDrift));

	// ⓐ **不是隐藏**：路不改变高度场，坡度 mask 一点没变 ⇒ 活着的三角必须**一个不少**。
	// 这条就是裁决五的判据：用 NaN 关掉的话这里会掉一大块；而"沉"与"消失"在只数三角数的
	// 断言里长得一模一样 —— 所以下面还要再量一次真实的下沉量。
	TestEqual(TEXT("画路之后活三角数一个都没少（壳没有被关掉）"), LiveAfter, LiveBefore);

	// ⓑ **是沉下去**：量下沉量本身。默认 RoadSink = 160 cm，沿地形法线下沉，
	// 所以竖直分量 = RoadSink × N.z ≤ RoadSink；裙边最陡处 N.z ≈ 0.66。
	if (!TestTrue(FString::Printf(TEXT("路下确实有壳可量（%d 个采样）"), SunkSamples), SunkSamples > 20)) return true;
	TestTrue(FString::Printf(TEXT("路下的壳沉下去了（平均 %.1f cm）"), MeanSink), MeanSink < -40.0);
	TestTrue(FString::Printf(TEXT("最深处沉了大半个 RoadSink（%.1f cm / %.0f cm）"), DeepestSink, Ground->RockShellRoadSink),
		DeepestSink < -0.5 * double(Ground->RockShellRoadSink));
	// 沉降不许超过物理上限。四项：`RoadSink` 本身，加上画路会**撤掉**的那些正向量 ——
	// 沿法线的厚度、表面起伏（幅度 ∝ 坡度）、以及基准偏移从 +BaseLift 翻到 −BaseSink 的整段落差。
	// 超了说明公式里多叠了一项。
	const double SinkLimit =
		double(Ground->RockShellRoadSink)
		+ double(Ground->RockShellCellRelief)
		+ double(Ground->RockShellNoiseAmount) * CSRockShellTest_MaxSlope
		+ double(Ground->RockShellChipAmount)
		+ double(Ground->RockShellBaseLift + Ground->RockShellBaseSink)
		+ 5.0;
	TestTrue(FString::Printf(TEXT("下沉量不超过 RoadSink + 自身厚度（%.1f cm ≤ %.1f）"), -DeepestSink, SinkLimit),
		-DeepestSink <= SinkLimit);

	// ⓒ 路外的壳**一动不动**：下沉是局部的，不是整张壳一起往下走。
	TestTrue(FString::Printf(TEXT("路外的壳纹丝不动（最大漂移 %.2f cm）"), WorstOffRoadDrift),
		OffRoadSamples == 0 || WorstOffRoadDrift < 0.01);

	// ⓔ **路缘是缓坡，不是折痕**（用户裁决 2026-09-11：路对壳的下压太激烈 ⇒ 壳采模糊后的路足迹）。
	//
	// 量的是**平均下沉剖面**的最陡处：活顶点按到路中线（x = Centre）的距离分 25 cm 一箱，
	// 逐箱求平均 ΔZ，取相邻两箱之差 / 箱宽的最大值。
	// ⚠️ **不能逐三角取梯度最大值**（第一版就这么写的，实测两组都在 20 上下，比的是噪声不是路缘）：
	// 角点不加表面起伏（`bIsCorner`，钉缝宽用），贴着它一两厘米的邻点却加满 ⇒ 画路把起伏撤掉时，
	// 短边两端的 ΔZ 就差出几十厘米 —— 这种逐点跳变与路缘多陡毫无关系。分箱平均把它平掉，
	// 剩下的只有路的沉降剖面。
	// 判据是**相对**的：同一条路、同一张壳，只把半径切到 0 再披挂一趟当对照 —— 绝对阈值会随
	// RoadSink / PatternScale 漂，相对比较不会。
	{
		auto SteepestSinkProfile = [&Before, Triangles](const TArray<FVector>& Sunk) -> double
		{
			constexpr double BinCm = 25.0;
			constexpr int32 NumBins = 36;       // 0 .. 900 cm，盖得住默认笔刷半径 + 模糊半径
			constexpr int32 MinPerBin = 20;     // 箱里点太少时平均值本身就是噪声
			TArray<double> SumDz;
			TArray<int32> Count;
			SumDz.Init(0.0, NumBins);
			Count.Init(0, NumBins);
			for (int32 Tri = 0; Tri < Triangles; ++Tri)
			{
				if (!CSRockShellTest_TriangleAlive(Sunk, Tri) || !CSRockShellTest_TriangleAlive(Before, Tri)) continue;
				for (int32 K = 0; K < 3; ++K)
				{
					const int32 Index = Tri * 3 + K;
					const int32 Bin = int32(FMath::Abs(double(Before[Index].X) - CSRockShellTest_Centre) / BinCm);
					if (Bin >= NumBins) continue;
					SumDz[Bin] += double(Sunk[Index].Z) - double(Before[Index].Z);
					++Count[Bin];
				}
			}
			double Steepest = 0.0;
			for (int32 Bin = 0; Bin + 1 < NumBins; ++Bin)
			{
				if (Count[Bin] < MinPerBin || Count[Bin + 1] < MinPerBin) continue;
				const double Step = SumDz[Bin + 1] / double(Count[Bin + 1]) - SumDz[Bin] / double(Count[Bin]);
				Steepest = FMath::Max(Steepest, FMath::Abs(Step) / BinCm);
			}
			return Steepest;
		};

		const double SteepBlurred = SteepestSinkProfile(After);
		const float BlurRadius = Ground->RockShellRoadBlurRadius;
		Ground->RockShellRoadBlurRadius = 0.0f;
		Ground->RebuildRockShell();
		TArray<FVector> AfterSharp;
		Ground->DebugReadRockShellSync(AfterSharp);
		const double SteepSharp = SteepestSinkProfile(AfterSharp);
		Ground->RockShellRoadBlurRadius = BlurRadius;
		Ground->RebuildRockShell();

		AddInfo(FString::Printf(TEXT("路缘平均下沉剖面的最陡处：模糊半径 %.0f cm 时 %.2f，半径 0 时 %.2f（cm/cm）"),
			BlurRadius, SteepBlurred, SteepSharp));
		// 夹具自证：不糊的时候确实有那道折痕，否则下面的比较可能只是两个小数在比。
		TestTrue(FString::Printf(TEXT("对照组（半径 0）确实有折痕（最陡 %.2f）"), SteepSharp), SteepSharp > 1.0);
		TestTrue(FString::Printf(TEXT("模糊之后路缘至少缓一半（%.2f vs %.2f）"), SteepBlurred, SteepSharp),
			BlurRadius > 0.0f && SteepBlurred < 0.5 * SteepSharp);
	}

	// ⓓ 擦掉路 ⇒ 壳原样浮回来（绝对式而非增量式）。
	Ground->ResetPaint();
	TArray<FVector> Restored;
	Ground->DebugReadRockShellSync(Restored);
	double WorstRestoreError = 0.0;
	int32 RestoreSamples = 0;
	for (int32 Tri = 0; Tri < Triangles; ++Tri)
	{
		if (!CSRockShellTest_TriangleAlive(Restored, Tri) || !CSRockShellTest_TriangleAlive(Before, Tri)) continue;
		for (int32 K = 0; K < 3; ++K)
		{
			const int32 Index = Tri * 3 + K;
			WorstRestoreError = FMath::Max(WorstRestoreError, double(FMath::Abs(Restored[Index].Z - Before[Index].Z)));
			++RestoreSamples;
		}
	}
	AddInfo(FString::Printf(TEXT("擦掉路之后 %d 个顶点最大复位误差 %.3f cm"), RestoreSamples, WorstRestoreError));
	TestTrue(FString::Printf(TEXT("擦掉路之后壳逐位复位（最大误差 %.3f cm）"), WorstRestoreError),
		RestoreSamples > 100 && WorstRestoreError < 0.01);
	return true;
}


// -----------------------------------------------------------------------------
// ④ 盖 / 裙分离：`bIsCapTri` 到得了材质，而且**烘成 StaticMesh 之后还在**
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSRockShellCapSkirtBakeTest,
	"PCGPlugins.ComputeShaderGenerator.RockShell.CapSkirtSurvivesBake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

/**
 * `bIsCapTri`（bit 26）原本只在 aux 槽 33 里躺着，**打了但没人读**。它现在被写进岩壳网格的
 * **顶点色 R**（1 = 盖 / 0 = 裙），由 `M_TG_Texture` 的 `RockShellCapSkirt` 支路消费。
 *
 * 这条测试守的是裁决六 ②「顶点色通道字典与多组 UV 必须随网格一起保住」。它必须**真烘一遍**：
 * GPU 常驻流 → `ReadbackResidentSync` → `BuildGpuMeshDescription` → StaticMesh 构建，
 * 中间任何一道把顶点色丢掉都**不报错**，症状只是烘出来的资产在编辑器里是一整片同色 ——
 * 而那时人已经离开这条链很久了。走 aux 流的话这里会直接空手而归：aux 是 `VET_None`，
 * 既不进顶点工厂也不进回读集。
 */
bool FCSRockShellCapSkirtBakeTest::RunTest(const FString&)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	UMaterialInterface* ShellMaterial = CSRockShellTest_LoadShellMaterial();
	if (!TestNotNull(TEXT("岩壳材质"), ShellMaterial)) return false;
	ACSGroundActor* Ground = CSRockShellTest_SpawnGround(World, ShellMaterial);
	if (!TestNotNull(TEXT("Ground actor"), Ground)) return false;

	int32 Triangles = 0, UVChannels = 0;
	float MaxCellId = 0.0f, DirAgreement = 0.0f;
	bool bFlipWinding = false;
	if (!TestTrue(TEXT("图案抽得出来"),
		Ground->GetRockShellPatternStats(Triangles, UVChannels, MaxCellId, bFlipWinding, DirAgreement)))
	{
		return false;
	}

	ACSGroundShaperActor* Mound = CSRockShellTest_SpawnMound(World, CSRockShellTest_Centre, CSRockShellTest_Centre);
	if (!TestNotNull(TEXT("Shaper mound"), Mound)) return false;

	// --- GPU 侧：通道里到底有没有两种值 ---
	int32 CapVerts = 0, SkirtVerts = 0;
	const int32 TotalVerts = Ground->DebugReadRockShellCapSplitSync(CapVerts, SkirtVerts);
	AddInfo(FString::Printf(TEXT("顶点色 R：盖 %d / 裙 %d，共 %d（三角 %d）"),
		CapVerts, SkirtVerts, TotalVerts, Triangles));
	if (!TestEqual(TEXT("顶点色回读到的顶点数 = 三角数 × 3"), TotalVerts, Triangles * 3)) return false;

	// **两种值都得有**。只有一种值时材质那一支恒等于常数，画面上是"没生效"而不是报错 ——
	// 这正是通道被写漏/被覆盖的症状。
	TestTrue(FString::Printf(TEXT("盖三角顶点存在（%d 个）"), CapVerts), CapVerts > 0);
	TestTrue(FString::Printf(TEXT("裙三角顶点存在（%d 个）"), SkirtVerts), SkirtVerts > 0);
	TestEqual(TEXT("盖 + 裙 = 全部顶点（没有第三种值）"), CapVerts + SkirtVerts, TotalVerts);

	// **逐三角常量**：盖/裙是逐三角标记，展开成 Tri*3+k 之后每种值的个数必然是 3 的倍数。
	// 不是的话说明色流被按顶点插值/焊接过了 —— 那会让材质在三角内部读到 0..1 之间的值，
	// 裙面于是出现半明半暗的渐变，看着像"AO 没烘对"，与通道毫无关系。
	TestEqual(TEXT("盖顶点数是 3 的倍数（逐三角常量）"), CapVerts % 3, 0);
	TestEqual(TEXT("裙顶点数是 3 的倍数（逐三角常量）"), SkirtVerts % 3, 0);

	// 原件实测：27,566 个盖三角 / 22,032 个裙三角（Docs/TinyGlade/CSRockShellPattern.md 的通道映射表）。
	// 对不上说明 UV2 的两个分量被通道错位换掉了 —— 那时 `bIsTopRim` 也一起错，壳的厚度层会失效，
	// 而厚度层失效只表现为"没有块感"，不报错。
	TestEqual(TEXT("盖三角数 = 原件的 27,566"), CapVerts / 3, 27566);
	TestEqual(TEXT("裙三角数 = 原件的 22,032"), SkirtVerts / 3, 22032);

	// --- 烘一遍：走的是组件自己那条 `SaveToStaticMesh` 出口，不另拼等价路径 ---
	const FString BakePath = FString::Printf(
		TEXT("/Game/Automation/RockShell/SM_RockShellCapSkirt_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	int32 BakedCap = 0, BakedSkirt = 0, BakedTris = 0;
	const bool bBaked = Ground->DebugBakeRockShellCapSplitSync(BakePath, BakedCap, BakedSkirt, BakedTris);
	AddInfo(FString::Printf(TEXT("烘成 StaticMesh：%d 三角，角点 盖 %d / 裙 %d"),
		BakedTris, BakedCap, BakedSkirt));
	if (!TestTrue(TEXT("岩壳有一条走得通的 SaveToStaticMesh 出口（裁决六 ①）"), bBaked)) return false;

	TestTrue(FString::Printf(TEXT("烘出来的资产有三角（%d 个）"), BakedTris), BakedTris > 0);
	// 烘的是**活着**的那一批：NaN 顶点在 BuildGpuMeshDescription 里被整个三角丢掉，
	// 所以这个数应当远小于图案的 49,598，同时远大于 0。
	TestTrue(FString::Printf(TEXT("烘出来的三角是活着的那一批（%d < %d）"), BakedTris, Triangles),
		BakedTris < Triangles);
	TestEqual(TEXT("烘出来的角点数 = 三角数 × 3"), BakedCap + BakedSkirt, BakedTris * 3);

	// **这两条就是裁决六 ② 的判据本身**：烘完之后通道里仍然两种值都在。
	TestTrue(FString::Printf(TEXT("烘完之后盖角点还在（%d 个）"), BakedCap), BakedCap > 0);
	TestTrue(FString::Printf(TEXT("烘完之后裙角点还在（%d 个）"), BakedSkirt), BakedSkirt > 0);
	TestEqual(TEXT("烘完之后盖角点数仍是 3 的倍数（没被插值掉）"), BakedCap % 3, 0);
	TestEqual(TEXT("烘完之后裙角点数仍是 3 的倍数（没被插值掉）"), BakedSkirt % 3, 0);

	UEditorAssetLibrary::DeleteAsset(BakePath);
	return true;
}

#endif   // WITH_EDITOR
