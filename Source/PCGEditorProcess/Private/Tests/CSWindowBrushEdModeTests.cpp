#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSWindowBrushEdMode.h"

#include "CSBrushEdModeBase.h"
#include "CSHouseActor.h"
#include "CSHouseFeatureMarker.h"
#include "CSHouseLibrary.h"
#include "CoreGlobals.h"                    // IsRunningCommandlet
#include "Editor.h"                         // GEditor / GLevelEditorModeTools
#include "EditorModeManager.h"              // FEditorModeTools
#include "EditorModeRegistry.h"             // FEditorModeRegistry
#include "Engine/Blueprint.h"               // UBlueprint::GeneratedClass
#include "Engine/EngineBaseTypes.h"         // EInputEvent / IE_Pressed
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "EngineUtils.h"                    // TActorIterator
#include "InputCoreTypes.h"                 // EKeys::Escape
#include "Kismet2/KismetEditorUtilities.h"  // CanCreateBlueprintOfClass
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Class.h"                  // HasAnyClassFlags / CLASS_Abstract

// -----------------------------------------------------------------------------
// 窗笔刷 EdMode 本体（D8，2026-09-06 用户裁决："点一下加一扇窗"）
//
// **为什么这一层需要单独的用例。** 落地那一步 `UCSHouseLibrary::PlaceMarkerAlongRay` 已经被
// `House.WindowBrushPlacement`（纯 CPU）与 `demo_house_window`（真演示关卡）两头钉住了，但
// EdMode 这一层**一行断言都没有** —— 而它恰恰是三条"错了不报红"的接线所在：
//
//   ① **光标求交**：房子的 gpumesh 全线 `NoCollision`，基类默认那套 `FoliageTrace` 一栋都打不到。
//      叶子必须覆盖 `TraceCandidatePoint` 走解析求交。忘了覆盖的症状是"笔刷球贴着地面走、
//      墙上永远点不出窗"，而 `PlaceMarkerAlongRay` 的每一条断言照绿。
//   ② **外法线的朝向**：`CommitSamples` 拿它反推那条射入墙里的短射线。取反了的话射线从墙里
//      往外打，`CSHouse_RayHitWall` 的"只认外表面"判据恒不成立 ⇒ 点哪儿都不出窗，同样零报红。
//   ③ **一次点击 = 一扇窗**：基类默认的圆盘散布会在一次点击里撒出好几扇、而且都不在你瞄的
//      地方，所以叶子把 `SamplePendingPoints` 覆盖成空实现。这条只有从"落笔之后窗数是几"
//      才看得出来。
//
// **做得到与做不到，逐条说清楚。**
//   - **做得到**：叶子契约（①②③ + 目标 actor 的生命周期 + `bExitAfterCommit` 的取值）——
//     `FCSWindowBrushEdModeProbe` 把那几个 protected 虚函数用 `using` 提成 public 就能直接调，
//     不需要视口、不需要 Slate、不需要模式管理器。见 `WindowBrushEdMode`。
//   - **做得到**：注册 → 按钮 → 激活 → 目标 → 退出这条链 —— 走真的 `GLevelEditorModeTools()`，
//     见 `WindowBrushModeActivation`。
//   - ⚠️ **做不到：把"落笔"本身从鼠标事件驱动一遍。** `FCSBrushEdModeBase::InputKey` 的
//     **LMB-按下**分支无条件解引用 `Viewport->KeyState(...)` 与 `ViewportClient->GetCurrentWidgetAxis()`，
//     而无头下造不出真的 `FEditorViewportClient`（它要一个 Slate 视口 widget）。传 nullptr 会
//     当场崩，把整套测试一起带走。`BeginStroke` / `CommitStroke` 又都是基类 private，够不着。
//     所以本文件**不驱动鼠标**：`CommitSamples` 直接调（那正是 `CommitStroke` 对叶子唯一的调用），
//     `bExitAfterCommit` 拆成两半分别钉 —— 叶子确实要求了退出（settings 断言），以及模式确实
//     退得掉（`WindowBrushModeActivation` 里走 Esc 那条同样通向 `ExitTemporaryMode()` 的路）。
//     **没有测到的是基类里 `if (bExitAfterCommit) ExitTemporaryMode();` 那一行的字面连接** ——
//     这里如实记一笔，不拿一条假装测了的断言把它盖住。
// -----------------------------------------------------------------------------

namespace
{
/**
 * 把叶子契约提成 public 的测试探针。
 *
 * ⚠️ **绝不能调 `Enter()`**：`FEdMode::Enter()` 无条件解引用 `Owner->GetSelectedActors()`，
 * 而 `Owner`（模式管理器）只有经 `FEditorModeTools::ActivateMode` 造出来的实例才有。
 * 本探针是自己 `MakeShared` 出来的，`Owner == nullptr` ⇒ 进去就崩。
 * 叶子契约那几个函数一个都不碰 `Owner`，所以不进 `Enter()` 完全够用。
 */
class FCSWindowBrushEdModeProbe : public FCSWindowBrushEdMode
{
public:
	using FCSWindowBrushEdMode::ClearBrushTarget;
	using FCSWindowBrushEdMode::CommitSamples;
	using FCSWindowBrushEdMode::GetBrushSettings;
	using FCSWindowBrushEdMode::GetBrushTargetActor;
	using FCSWindowBrushEdMode::SamplePendingPoints;
	using FCSWindowBrushEdMode::TraceCandidatePoint;
};
}   // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSWindowBrushEdModeTest,
	"PCGPlugins.PCGEditorProcess.House.WindowBrushEdMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSWindowBrushEdModeTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	ACSHouseActor* House = World->SpawnActor<ACSHouseActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("House"), House)) return false;
	// 与 `House.WindowMarker` 同一条理由：把属性面板那一路窗摘干净，否则计数断言会时红时绿。
	House->Windows.Reset();
	House->ReevaluateSite();

	TSharedRef<FCSWindowBrushEdModeProbe> Probe = MakeShared<FCSWindowBrushEdModeProbe>();

	// -------------------------------------------------------------------------
	// ③ 笔刷参数：一次点击一扇窗、落笔即退
	// -------------------------------------------------------------------------
	{
		const FCSBrushSettings Settings = Probe->GetBrushSettings();
		TestTrue(TEXT("the window brush asks to exit after the first commit (one click = one window)"),
			Settings.bExitAfterCommit);
		TestEqual(TEXT("and it samples exactly one point per move"), Settings.SamplesPerMouseMove, 1);
		// 背面剔除交给 `CSHouse_RayHitWall`（它只认外表面）—— 基类再来一道是重复判据，
		// 而且它用的是**扫掠法线**，对解析命中给出的法线口径不同。
		TestFalse(TEXT("back-face rejection is left to CSHouse_RayHitWall, not done twice"),
			Settings.bRejectBackFaces);
		TestTrue(TEXT("spacing rejection is off (there is no committed set to space against)"),
			Settings.MinSpacing == 0.0f);
		TestTrue(TEXT("the cursor sphere has a visible radius"), Settings.Radius > 0.0f);
	}

	// -------------------------------------------------------------------------
	// 目标 actor 的生命周期：没目标就什么都不做
	// -------------------------------------------------------------------------
	{
		TestNull(TEXT("a fresh mode holds no target"), Probe->GetBrushTargetActor());

		FHitResult Hit;
		TestFalse(TEXT("and traces nothing without one"),
			Probe->TraceCandidatePoint(FVector(0.0, -1000.0, 150.0), FVector(0.0, 1000.0, 150.0), Hit));

		Probe->SetTargetActor(House);
		TestEqual(TEXT("SetTargetActor takes it"), Probe->GetBrushTargetActor(), Cast<AActor>(House));
	}

	// -------------------------------------------------------------------------
	// ①② 光标求交：世界点落在墙的外表面上，法线朝外
	// -------------------------------------------------------------------------
	//
	// 房子在原点、默认 footprint ⇒ −Y 那面墙的**外表面**恰在 `Y = -FootprintSize.Y / 2`
	// （口径与 `House.WindowMarker` 里那条吸附断言同源）。射线从墙外沿 +Y 垂直打进去，
	// 所以命中点的 X / Z 必须**原样保留**，只有 Y 被拉到墙面上。
	const double HalfY = House->FootprintSize.Y * 0.5;
	const double AimX = 40.0;
	const double AimZ = 150.0;
	const FVector RayStart(AimX, -HalfY - 300.0, AimZ);
	const FVector RayEnd(AimX, -HalfY + 300.0, AimZ);
	{
		FHitResult Hit;
		if (!TestTrue(TEXT("a ray at the wall hits it analytically (engine collision would not)"),
				Probe->TraceCandidatePoint(RayStart, RayEnd, Hit)))
		{
			return false;
		}

		const FVector Want(AimX, -HalfY, AimZ);
		TestTrue(FString::Printf(TEXT("the hit lands on the wall's outer face (%s, wanted %s)"),
				*Hit.ImpactPoint.ToCompactString(), *Want.ToCompactString()),
			Hit.ImpactPoint.Equals(Want, 0.5));
		// 基类读的是 `ImpactPoint`，但 `Location` 也得填对 —— 两者写岔的话，将来谁把基类
		// 改成读 `Location`（那才是引擎 trace 的惯例）笔刷球就会跳到原点去。
		TestTrue(TEXT("Location and ImpactPoint agree"), Hit.Location.Equals(Hit.ImpactPoint, 0.01));
		TestTrue(TEXT("the hit is marked blocking"), Hit.bBlockingHit);

		// —— ②：法线朝**外** ——
		//
		// ⚠️ 这条是本文件最要紧的一枪。`CommitSamples` 用 `-LastHitNormal` 当射入方向；法线
		// 取反了的话那条射线从墙里往外打，`CSHouse_RayHitWall` 的"只认外表面"判据恒不成立
		// ⇒ 点哪儿都不出窗，而上面每一条断言照绿。判据写成"与射线相向"而不是硬编 (0,−1,0)：
		// 前者是**外法线**这个词的定义，换个方向打、换栋房转个角度都成立。
		TestTrue(FString::Printf(TEXT("the normal faces the incoming ray, i.e. it is the OUTWARD normal (%s)"),
				*Hit.ImpactNormal.ToCompactString()),
			FVector::DotProduct(Hit.ImpactNormal, (RayEnd - RayStart).GetSafeNormal()) < 0.0);
		TestTrue(TEXT("and for this wall that is -Y"),
			Hit.ImpactNormal.Equals(FVector(0.0, -1.0, 0.0), 0.01));
		TestTrue(TEXT("it is unit length (CommitSamples steps along it by a fixed standoff)"),
			FMath::IsNearlyEqual(Hit.ImpactNormal.Size(), 1.0, 0.001));
		TestTrue(TEXT("Normal and ImpactNormal agree"), Hit.Normal.Equals(Hit.ImpactNormal, 0.01));
	}

	// 打空：不命中就是不命中（笔刷球随即隐藏，落笔什么都不生成）。
	{
		FHitResult Hit;
		TestFalse(TEXT("a ray into the sky hits nothing"),
			Probe->TraceCandidatePoint(FVector(0.0, 0.0, 100000.0), FVector(0.0, 0.0, 200000.0), Hit));
	}

	// -------------------------------------------------------------------------
	// ③ 落笔：一次 commit **恰好**一扇窗，落在光标命中点上
	// -------------------------------------------------------------------------
	{
		// 基类在 stroke 的每一步都调 `SamplePendingPoints`。叶子把它覆盖成了空实现，所以
		// 调多少次都不该多出窗来 —— 这就是"一次点击 = 一扇窗"的可执行形式。
		Probe->SamplePendingPoints();
		Probe->SamplePendingPoints();
		Probe->SamplePendingPoints();

		// ⚠️ 必须先重新 trace：上面那次"打空"把 `bLastHitValid` 清掉了，而 `CommitSamples`
		// 靠它挡住"笔刷球不在墙上时松手"。这一步同时钉住了那道闸确实存在（下面 ClearBrushTarget
		// 之后那条反例才不是空判据）。
		FHitResult Hit;
		if (!TestTrue(TEXT("re-aim at the wall before releasing"),
				Probe->TraceCandidatePoint(RayStart, RayEnd, Hit)))
		{
			return false;
		}

		Probe->CommitSamples(TArray<FCSBrushSample>());

		TestEqual(TEXT("one release puts exactly one marker on the house"),
			House->GetFeatureMarkerCount(), 1);
		TestEqual(TEXT("cutting exactly one window (the disc sampler really is off)"),
			House->GetWindowCount(), 1);
		TestEqual(TEXT("and nothing was rejected"), House->GetWindowRejectCount(), 0);

		// 窗落在**瞄的那面墙上、瞄的那个位置**（而不是相机跟前、也不是隔壁那面墙）。
		// 判据取世界位置：锚点/CenterS 那一维归 `House.WindowBrushPlacement`，这里只问
		// "EdMode 把光标命中点交对了没有"。
		ACSHouseFeatureMarker* Placed = nullptr;
		for (TActorIterator<ACSHouseFeatureMarker> It(World); It; ++It)
		{
			Placed = *It;
			break;
		}
		if (TestNotNull(TEXT("the marker exists in the world"), Placed))
		{
			TestEqual(TEXT("its host is the house we aimed at"), Placed->GetHost(), House);
			TestTrue(TEXT("it really cuts"), Placed->CausesCut());
			const FVector At = Placed->GetActorLocation();
			TestTrue(FString::Printf(TEXT("it sits at the cursor's arc length, not somewhere else (x=%.1f)"), At.X),
				FMath::IsNearlyEqual(At.X, AimX, 0.5));
			TestTrue(FString::Printf(TEXT("standing off the wall we aimed at, on the outside (y=%.1f)"), At.Y),
				FMath::IsNearlyEqual(At.Y, -HalfY - Placed->WallStandoff, 0.5));

			// ⚠️ 先退选再删。`CommitSamples` 会把新窗**选中**（那是它该做的），而这里直接
			// `DestroyActor` 会在编辑器的选中集里留下一个已成 garbage 的条目 —— 下一次
			// `CommitSamples` 开头的 `SelectNone` 扫到它就吐一句
			// `LogActorLevelEditorSelection: Warning: ... actor has invalid flags`
			// （内部标志 0x00204000 = `Garbage | ReachabilityFlag0`）。那是**本用例自己制造的
			// 脏选中**，不是产品缺陷：编辑器里正常删 actor 会连同选中一起清掉。
			// 留着它会让一条绿灯用例带一句吓人的警告，以后有人来查真问题时先被它绊一跤。
			if (GEditor) GEditor->SelectNone(false, true);
			World->DestroyActor(Placed);
		}
		House->ReevaluateSite();
		TestEqual(TEXT("deleting it closes the hole"), House->GetWindowCount(), 0);
	}

	// -------------------------------------------------------------------------
	// 全世界扫：笔刷开着时点哪栋就往哪栋上放，不限于按钮所属的那一栋
	// -------------------------------------------------------------------------
	//
	// ⚠️ 这条钉的是 `TraceCandidatePoint` 里"扫 `PickHouse`（world 里全部房子）而不是只算 `TargetActor`"
	// 那句注释。写成只算目标房的话，用户点第二栋房时笔刷球会贴在第一栋上 —— 而目标房那边的
	// 每一条断言照绿。
	{
		ACSHouseActor* Other = World->SpawnActor<ACSHouseActor>(
			FVector(0.0, 4000.0, 0.0), FRotator::ZeroRotator);
		if (!TestNotNull(TEXT("Second house"), Other)) return false;
		Other->Windows.Reset();
		Other->ReevaluateSite();

		// 目标仍然是第一栋房，但射线瞄的是第二栋的 −Y 墙。
		const double OtherHalfY = Other->FootprintSize.Y * 0.5;
		const FVector OtherStart(0.0, 4000.0 - OtherHalfY - 300.0, 150.0);
		const FVector OtherEnd(0.0, 4000.0 - OtherHalfY + 300.0, 150.0);

		FHitResult Hit;
		if (TestTrue(TEXT("the brush hits a house other than its own target"),
				Probe->TraceCandidatePoint(OtherStart, OtherEnd, Hit)))
		{
			TestTrue(TEXT("on that house's wall, not the target's"),
				FMath::IsNearlyEqual(Hit.ImpactPoint.Y, 4000.0 - OtherHalfY, 0.5));

			Probe->CommitSamples(TArray<FCSBrushSample>());
			TestEqual(TEXT("and the window lands on the house under the cursor"),
				Other->GetWindowCount(), 1);
			TestEqual(TEXT("while the mode's own target gets nothing"), House->GetWindowCount(), 0);
		}
	}

	// -------------------------------------------------------------------------
	// 收工：丢掉目标 ⇒ 落笔什么都不生成（Exit 靠这条把关卡里的东西全放开）
	// -------------------------------------------------------------------------
	{
		Probe->ClearBrushTarget();
		TestNull(TEXT("ClearBrushTarget drops the actor"), Probe->GetBrushTargetActor());

		int32 MarkersBefore = 0;
		for (TActorIterator<ACSHouseFeatureMarker> It(World); It; ++It) ++MarkersBefore;

		Probe->CommitSamples(TArray<FCSBrushSample>());

		int32 MarkersAfter = 0;
		for (TActorIterator<ACSHouseFeatureMarker> It(World); It; ++It) ++MarkersAfter;
		TestEqual(TEXT("a release with no target creates nothing"), MarkersAfter, MarkersBefore);

		FHitResult Hit;
		TestFalse(TEXT("and the cursor traces nothing either"),
			Probe->TraceCandidatePoint(RayStart, RayEnd, Hit));
	}

	return true;
}

// -----------------------------------------------------------------------------
// 按钮 → 模块 → 模式管理器这条链（注册 / 激活 / 目标 / 退得掉）
//
// 上面那个用例完全绕开了 `FEditorModeTools`，所以"`StartWindowBrush` 按下去到底有没有让这个
// 模式激活起来、目标有没有交到手上"一条都没测到。这一条走**真的** `GLevelEditorModeTools()`。
//
// ⚠️ `GetBrushTargetActor()` 在真实例上够不着（protected，而拿到的是基类指针）。这里用一个
// **间接判据**：`FCSBrushEdModeBase::InputKey` 的第一行就是 `if (!GetBrushTargetActor()) return false;`
// ⇒ Esc 的返回值就是"目标设上了没有"。顺带这条 Esc 通向的正是 `bExitAfterCommit` 用的那个
// `ExitTemporaryMode()`，所以"模式退得掉"也一起钉了。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSWindowBrushModeActivationTest,
	"PCGPlugins.PCGEditorProcess.House.WindowBrushModeActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSWindowBrushModeActivationTest::RunTest(const FString& Parameters)
{
	// ---- 注册与订阅：这两条不需要模式管理器，任何环境都能判 ----
	TestTrue(TEXT("the window brush mode is registered with the editor mode registry"),
		FEditorModeRegistry::Get().GetFactoryMap().Contains(FCSWindowBrushEdMode::EM_CSWindowBrush));
	TestTrue(TEXT("and PCGEditorProcess subscribed to the house's StartWindowBrush request"),
		ACSHouseActor::OnWindowBrushRequest.IsBound());

	// ⚠️ `GLevelEditorModeTools()` 在 commandlet 里是 `checkf` 硬崩，没有 `GEditor` 时是 ensure。
	// 无头 automation（`-ExecCmds`）两者都成立，但这条闸留着 —— 崩一次会带走整套测试。
	if (IsRunningCommandlet() || !GEditor)
	{
		AddInfo(TEXT("No global mode manager in this environment (commandlet / no GEditor); "
					 "the activation half of this test is skipped, the registry half above still ran."));
		return true;
	}

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	ACSHouseActor* House = World->SpawnActor<ACSHouseActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("House"), House)) return false;

	FEditorModeTools& ModeTools = GLevelEditorModeTools();
	const FEditorModeID ModeID = FCSWindowBrushEdMode::EM_CSWindowBrush;

	// 进来时不该是激活的（别的用例留下来的话下面全是假绿）。
	if (ModeTools.IsModeActive(ModeID)) ModeTools.DeactivateMode(ModeID);

	// ---- 手动激活但**不给目标**：Esc 应答 false（反例，让下面那条不是空判据）----
	{
		ModeTools.ActivateMode(ModeID);
		TestTrue(TEXT("ActivateMode brings the window brush up"), ModeTools.IsModeActive(ModeID));

		FCSWindowBrushEdMode* Mode = ModeTools.GetActiveModeTyped<FCSWindowBrushEdMode>(ModeID);
		if (TestNotNull(TEXT("and the active mode really is FCSWindowBrushEdMode"), Mode))
		{
			TestFalse(TEXT("with no target it swallows nothing, not even Esc"),
				Mode->InputKey(nullptr, nullptr, EKeys::Escape, IE_Pressed));
			TestTrue(TEXT("so it is still up"), ModeTools.IsModeActive(ModeID));
		}
		ModeTools.DeactivateMode(ModeID);
		TestFalse(TEXT("DeactivateMode takes it down again"), ModeTools.IsModeActive(ModeID));
	}

	// ---- 按钮那条真链：`StartWindowBrush()` → 委托 → 模块 → 激活 + 交目标 ----
	{
		House->StartWindowBrush();
		TestTrue(TEXT("pressing StartWindowBrush on the house brings the mode up"),
			ModeTools.IsModeActive(ModeID));

		FCSWindowBrushEdMode* Mode = ModeTools.GetActiveModeTyped<FCSWindowBrushEdMode>(ModeID);
		if (TestNotNull(TEXT("the button's mode instance"), Mode))
		{
			// 目标设上了 ⇒ Esc 被吃掉（`InputKey` 的第一行就是那道判据），且模式退出。
			// 这条同时证明了 `ExitTemporaryMode()` → `DeactivateMode` 这段接线是通的 ——
			// `bExitAfterCommit` 落笔退出走的是同一个函数。
			TestTrue(TEXT("the module handed it the house that asked (Esc is now consumed)"),
				Mode->InputKey(nullptr, nullptr, EKeys::Escape, IE_Pressed));
			TestFalse(TEXT("and Esc exits the mode (same ExitTemporaryMode bExitAfterCommit uses)"),
				ModeTools.IsModeActive(ModeID));
		}
	}

	// ---- 换一栋房再按一次：重进 + 换目标（按钮不是一次性的）----
	{
		ACSHouseActor* Other = World->SpawnActor<ACSHouseActor>(
			FVector(0.0, 4000.0, 0.0), FRotator::ZeroRotator);
		if (TestNotNull(TEXT("Second house"), Other))
		{
			Other->StartWindowBrush();
			TestTrue(TEXT("a second press brings it up again"), ModeTools.IsModeActive(ModeID));
			FCSWindowBrushEdMode* Mode = ModeTools.GetActiveModeTyped<FCSWindowBrushEdMode>(ModeID);
			if (TestNotNull(TEXT("the re-entered mode instance"), Mode))
			{
				TestTrue(TEXT("retargeted to the second house"),
					Mode->InputKey(nullptr, nullptr, EKeys::Escape, IE_Pressed));
			}
		}
	}

	// 收工：别把模式留给下一个用例。
	if (ModeTools.IsModeActive(ModeID)) ModeTools.DeactivateMode(ModeID);
	TestFalse(TEXT("the mode is left deactivated for whatever runs next"),
		ModeTools.IsModeActive(ModeID));

	return true;
}

// -----------------------------------------------------------------------------
// 窗子蓝图那一族还活着吗（2026-09-06 退掉拖放入口之后的复核）
//
// `NotPlaceable` 与 `NotBlueprintable` **都会沿继承链生效**，而两者的失效方式都很安静：
//   · `Blueprintable` 掉了 ⇒ 右键菜单里再也建不出以 `ACSWindowMarker` 为父的蓝图，**不报错**；
//   · 已有的 `BP_Window_*` 也不会当场坏掉，要到下次有人想加一档窗才发现。
// 所以这里既问"新的还建得出来吗"（`CanCreateBlueprintOfClass` —— 内容浏览器用的就是它），
// 也问"旧的还完好吗"（两个子蓝图都 load 得动、父类没跑偏、生成类能实例化）。
//
// ⚠️ 无头测不了"在蓝图编辑器里点开"这件事本身（那要 Slate 起一个资产编辑器）。这里测的是
// 它依赖的那几样：资产 load 得动、`GeneratedClass` 有效、父类链对、类不是抽象的。
// 这是**代理判据**，不是"我打开过了"。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSWindowBrushBlueprintsTest,
	"PCGPlugins.PCGEditorProcess.House.WindowBrushBlueprints",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSWindowBrushBlueprintsTest::RunTest(const FString& Parameters)
{
	// ---- 还能不能**新建**一档窗（`Blueprintable` 有没有被 NotPlaceable 那一行连累掉）----
	TestTrue(TEXT("the content browser can still author a Blueprint child of ACSWindowMarker"),
		FKismetEditorUtilities::CanCreateBlueprintOfClass(ACSWindowMarker::StaticClass()));
	// 反例：两个基类都是 `NotBlueprintable`，所以上一条不是"什么类都能建"的空判据。
	TestFalse(TEXT("but not of the abstract NotBlueprintable base (so the check above is real)"),
		FKismetEditorUtilities::CanCreateBlueprintOfClass(ACSHouseFeatureMarker::StaticClass()));

	// ---- 已有的两档窗还完好吗 ----
	const TCHAR* Paths[] =
	{
		TEXT("/PCGPlugins/HouseTest/BP_Window_Cottage_1x1.BP_Window_Cottage_1x1"),
		TEXT("/PCGPlugins/HouseTest/BP_Window_Gothic_1x1.BP_Window_Gothic_1x1"),
	};

	for (const TCHAR* Path : Paths)
	{
		UBlueprint* BP = LoadObject<UBlueprint>(nullptr, Path);
		if (!TestNotNull(*FString::Printf(TEXT("%s loads"), Path), BP)) continue;

		UClass* Generated = BP->GeneratedClass;
		if (!TestNotNull(*FString::Printf(TEXT("%s has a generated class"), Path), Generated)) continue;

		TestTrue(*FString::Printf(TEXT("%s still derives from ACSWindowMarker"), Path),
			Generated->IsChildOf(ACSWindowMarker::StaticClass()));
		// 这一条是"笔刷退路"那件事的资产侧：`WindowBrushClass` 指到它身上必须 spawn 得出来。
		TestFalse(*FString::Printf(TEXT("%s is not abstract, so the brush can spawn it"), Path),
			Generated->HasAnyClassFlags(CLASS_Abstract));
		// `NotPlaceable` 继承下来了 —— 拖进视口那条路对子蓝图同样关着（正是想要的）。
		TestTrue(*FString::Printf(TEXT("%s inherited NotPlaceable (drag-and-drop stays retired)"), Path),
			Generated->HasAnyClassFlags(CLASS_NotPlaceable));
	}

	return true;
}

#endif   // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
