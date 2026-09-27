#include "CSWallActor.h"

#include "CSGpuInstancedMeshComponent.h"
#include "CSGpuMeshTypes.h"
#include "CSGroundActor.h"
#include "CSMesh.h"
#include "CSMeshOps.h"
#include "CSMeshRenderComponent.h"
#include "CSVineTube.h"
#include "Components/SplineComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"   // TActorIterator
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogTinyGladeWall, Log, All);

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSWallActor_ 前缀。

int32 CSWallActor_Q(double Value, double Quantum) { return int32(FMath::RoundToDouble(Value / Quantum)); }

uint32 CSWallActor_Hash(const TArray<int32>& Values)
{
	return Values.Num() ? FCrc::MemCrc32(Values.GetData(), Values.Num() * sizeof(int32)) : 0u;
}

/**
 * 砖路元素的哈希，量在**组件空间**（与房子 `CSHouse_HashElementFrames` 同一个理由：`FPath` 全是路局部量，
 * 墙整体挪了而路形不变时它一个字都不变，框架必须进哈希）。
 */
uint32 CSWallActor_HashElements(const TArray<CSHouseFrame::FElement>& Elements, const FMatrix44f& WorldToComponent)
{
	TArray<int32> H;
	H.Reserve(Elements.Num() * 14);
	for (const CSHouseFrame::FElement& E : Elements)
	{
		const FVector3f O(WorldToComponent.TransformPosition(E.Frame.Origin));
		const FVector3f U(WorldToComponent.TransformVector(E.Frame.AxisU));
		H.Append({ E.BrickCount, CSWallActor_Q(E.Path.TotalLen(), 0.5), CSWallActor_Q(E.LayoutScale, 0.001),
			CSWallActor_Q(E.Path.TopZ, 0.5), CSWallActor_Q(E.Path.BaseZ, 0.5), int32(E.Path.MidKind),
			CSWallActor_Q(O.X, 0.5), CSWallActor_Q(O.Y, 0.5), CSWallActor_Q(O.Z, 0.5),
			CSWallActor_Q(U.X, 0.001), CSWallActor_Q(U.Y, 0.001), CSWallActor_Q(U.Z, 0.001),
			int32(E.RandomBase), CSWallActor_Q(E.QuoinHalfTurnCos, 0.001) });
	}
	return CSWallActor_Hash(H);
}

/** 交接包围盒按 200 cm 台阶外扩对齐（只涨不缩由 `MergeHandoverBounds` 负责）：拖样条时不每帧重交。 */
FBox CSWallActor_SnapBounds(const FBox& Box)
{
	if (!Box.IsValid) return Box;
	const double Q = CSShaperSteps::BoundsQuantum;
	auto Down = [Q](double V) { return FMath::FloorToDouble(V / Q) * Q - Q; };
	auto Up = [Q](double V) { return FMath::CeilToDouble(V / Q) * Q + Q; };
	return FBox(FVector(Down(Box.Min.X), Down(Box.Min.Y), Down(Box.Min.Z)), FVector(Up(Box.Max.X), Up(Box.Max.Y), Up(Box.Max.Z)));
}

const TArray<FCSWallOpening>& CSWallActor_NoOpenings()
{
	static const TArray<FCSWallOpening> Empty;
	return Empty;
}
}

const TCHAR* ACSWallActor::DefaultWallMaterialPath = TEXT("/PCGPlugins/HouseTest/MI_TinyGladeWall");
const TCHAR* ACSWallActor::DefaultBrickMeshPath = TEXT("/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/brick");

ACSWallActor::ACSWallActor()
{
	// 合批唤醒的兑现点：平时关着，有欠账才开（同房子 / 楼梯）。
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	// 默认一条 L 形的墙（照 TG 参考截图：一段直墙、一个直角、再一段）。点全是 Linear —— 拐角是硬的，
	// 拐角外侧出一根角石；想要弯墙把点改成 Curve 即可。
	Spline = CreateDefaultSubobject<USplineComponent>(TEXT("Spline"));
	Spline->SetupAttachment(RootComponent);
	Spline->ClearSplinePoints(false);
	Spline->AddSplinePoint(FVector(0.0, 0.0, 0.0), ESplineCoordinateSpace::Local, false);
	Spline->AddSplinePoint(FVector(600.0, 0.0, 0.0), ESplineCoordinateSpace::Local, false);
	Spline->AddSplinePoint(FVector(600.0, 450.0, 0.0), ESplineCoordinateSpace::Local, false);
	for (int32 Index = 0; Index < 3; ++Index) Spline->SetSplinePointType(Index, ESplinePointType::Linear, false);
	Spline->UpdateSpline();

	// 名字稳定 ⇒ 烘焙出来的资产名稳定（基类 `FCSInstancedFamily` 那段注释）。
	BrickComponent = CreateDefaultSubobject<UCSGpuInstancedMeshComponent>(TEXT("WallBricks"));
	BrickComponent->SetupAttachment(RootComponent);
	VineBranchComponent = CreateDefaultSubobject<UCSGpuInstancedMeshComponent>(TEXT("VineBranch"));
	VineBranchComponent->SetupAttachment(RootComponent);
	VineLeafComponent = CreateDefaultSubobject<UCSGpuInstancedMeshComponent>(TEXT("VineLeaf"));
	VineLeafComponent->SetupAttachment(RootComponent);
	VineFlowerComponent = CreateDefaultSubobject<UCSGpuInstancedMeshComponent>(TEXT("VineFlower"));
	VineFlowerComponent->SetupAttachment(RootComponent);

	// 藤管几何在世界空间 ⇒ 相对变换钉成恒等（组件构造时已标成绝对变换，同房子 `VineTubeComponent`）。
	VineTubeComponent = CreateDefaultSubobject<UCSMeshRenderComponent>(TEXT("VineTubeMesh"));
	VineTubeComponent->SetupAttachment(RootComponent);
	VineTubeComponent->SetRelativeTransform(FTransform::Identity);

	// 默认资产 = 房子蓝图挂的那一批。`FObjectFinderOptional`：找不到只是没有默认值，不把 CDO 构造带崩。
	static ConstructorHelpers::FObjectFinderOptional<UMaterialInterface> WallMaterialAsset(DefaultWallMaterialPath);
	static ConstructorHelpers::FObjectFinderOptional<UStaticMesh> BrickAsset(DefaultBrickMeshPath);
	static ConstructorHelpers::FObjectFinderOptional<UStaticMesh> BranchAsset(TEXT("/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/ivy_branch"));
	static ConstructorHelpers::FObjectFinderOptional<UStaticMesh> LeafAsset(TEXT("/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/ivy_leaf"));
	static ConstructorHelpers::FObjectFinderOptional<UStaticMesh> FlowerAsset(TEXT("/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/ivy_flower"));
	static ConstructorHelpers::FObjectFinderOptional<UMaterialInterface> BranchMaterialAsset(TEXT("/PCGPlugins/HouseTest/M_TinyGladeIvyBranch"));
	static ConstructorHelpers::FObjectFinderOptional<UMaterialInterface> LeafMaterialAsset(TEXT("/PCGPlugins/HouseTest/M_TinyGladeIvyLeaf"));
	static ConstructorHelpers::FObjectFinderOptional<UMaterialInterface> FlowerMaterialAsset(TEXT("/PCGPlugins/HouseTest/M_TinyGladeIvyFlower"));
	WallMaterial = WallMaterialAsset.Get();
	BrickMesh = BrickAsset.Get();
	Vine.BranchMesh = BranchAsset.Get();
	Vine.LeafMesh = LeafAsset.Get();
	Vine.FlowerMesh = FlowerAsset.Get();
	Vine.BranchMaterial = BranchMaterialAsset.Get();
	Vine.LeafMaterial = LeafMaterialAsset.Get();
	Vine.FlowerMaterial = FlowerMaterialAsset.Get();
}

// -----------------------------------------------------------------------------
// 合批：本帧标脏、下一帧重建（与房子同一条裁决）
// -----------------------------------------------------------------------------

void ACSWallActor::RequestReevaluate()
{
	if (bSubscriptionsDisabled || IsTemplate() || !GetWorld()) return;
	bReevaluatePending = true;
	SetActorTickEnabled(true);
#if WITH_EDITOR
	// 非实时视口 / Slate 拖动节流会整段跳过 actor tick：挂一次性的核心 ticker 兜底（同楼梯）。
	if (!GetWorld()->IsGameWorld() && !EditorRebuildTicker.IsValid())
	{
		EditorRebuildTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [this](float)
		{
			EditorRebuildTicker.Reset();
			FlushPendingReevaluate();
			return false;
		}));
	}
#endif
}

void ACSWallActor::FlushPendingReevaluate() const
{
	// const_cast：补上的是"已经欠下的那一次"，补完后对外可见的状态与当场就跑逐位相同（同房子）。
	if (bReevaluatePending && !bInReevaluate) const_cast<ACSWallActor*>(this)->ReevaluateSite();
}

void ACSWallActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	// **先关再跑**：重建途中又来的通知会把 tick 重新打开、留给下一帧。
	SetActorTickEnabled(false);
	FlushPendingReevaluate();
}

void ACSWallActor::SubscribeChanges()
{
	if (USceneComponent* Root = GetRootComponent(); Root && !RootTransformChangedHandle.IsValid())
	{
		RootTransformChangedHandle = Root->TransformUpdated.AddUObject(this, &ACSWallActor::HandleComponentTransformChanged);
	}
	// 蓝图重实例化 / 撤销可能换掉组件：从旧对象解绑后再订阅当前对象（同楼梯）。
	if (ObservedSpline.Get() == Spline.Get()) return;
	UnsubscribeSplineChanges();
	ObservedSpline = Spline;
	if (!Spline) return;
PRAGMA_DISABLE_EXPERIMENTAL_WARNINGS
	SplineChangedHandle = Spline->GetOnSplineChanged().AddUObject(this, &ACSWallActor::RequestReevaluate);
	SplineUpdatedHandle = Spline->GetOnSplineUpdated().AddUObject(this, &ACSWallActor::RequestReevaluate);
PRAGMA_ENABLE_EXPERIMENTAL_WARNINGS
	SplineTransformChangedHandle = Spline->TransformUpdated.AddUObject(this, &ACSWallActor::HandleComponentTransformChanged);
}

void ACSWallActor::UnsubscribeChanges()
{
#if WITH_EDITOR
	FTSTicker::RemoveTicker(EditorRebuildTicker);
	EditorRebuildTicker.Reset();
#endif
	UnsubscribeSplineChanges();
	if (USceneComponent* Root = GetRootComponent(); Root && RootTransformChangedHandle.IsValid()) Root->TransformUpdated.Remove(RootTransformChangedHandle);
	RootTransformChangedHandle.Reset();
}

void ACSWallActor::UnsubscribeSplineChanges()
{
	if (USplineComponent* Previous = ObservedSpline.Get())
	{
PRAGMA_DISABLE_EXPERIMENTAL_WARNINGS
		Previous->GetOnSplineChanged().Remove(SplineChangedHandle);
		Previous->GetOnSplineUpdated().Remove(SplineUpdatedHandle);
PRAGMA_ENABLE_EXPERIMENTAL_WARNINGS
		Previous->TransformUpdated.Remove(SplineTransformChangedHandle);
	}
	ObservedSpline.Reset();
	SplineChangedHandle.Reset();
	SplineUpdatedHandle.Reset();
	SplineTransformChangedHandle.Reset();
}

void ACSWallActor::HandleComponentTransformChanged(USceneComponent* /*Component*/, EUpdateTransformFlags /*Flags*/, ETeleportType /*Teleport*/)
{
	RequestReevaluate();
}

// -----------------------------------------------------------------------------
// 地面
// -----------------------------------------------------------------------------

void ACSWallActor::ResolveGroundAndSubscribe()
{
	if (!IsValid(Ground) && GetWorld())
	{
		// 与房子 / 楼梯同一口径：场景里第一块地面。
		Ground = nullptr;
		GroundChangedHandle.Reset();
		for (TActorIterator<ACSGroundActor> It(GetWorld()); It; ++It) { Ground = *It; break; }
	}
	if (!IsValid(Ground))
	{
		// 还没有地面：先挂在类级广播上等它出现（地面晚于墙出现时没人会再来叫醒这面墙 —— 同房子 09-21 那条）。
		if (!AnyGroundRebuiltHandle.IsValid()) AnyGroundRebuiltHandle = ACSGroundActor::OnAnyGroundRebuilt.AddUObject(this, &ACSWallActor::HandleAnyGroundRebuilt);
		return;
	}
	if (AnyGroundRebuiltHandle.IsValid())
	{
		ACSGroundActor::OnAnyGroundRebuilt.Remove(AnyGroundRebuiltHandle);
		AnyGroundRebuiltHandle.Reset();
	}
	if (!GroundChangedHandle.IsValid()) GroundChangedHandle = Ground->OnGroundChanged.AddUObject(this, &ACSWallActor::HandleGroundChanged);
}

void ACSWallActor::UnsubscribeGround()
{
	if (IsValid(Ground) && GroundChangedHandle.IsValid()) Ground->OnGroundChanged.Remove(GroundChangedHandle);
	GroundChangedHandle.Reset();
	if (AnyGroundRebuiltHandle.IsValid()) ACSGroundActor::OnAnyGroundRebuilt.Remove(AnyGroundRebuiltHandle);
	AnyGroundRebuiltHandle.Reset();
}

void ACSWallActor::HandleGroundChanged(ACSGroundActor* /*ChangedGround*/, const FBox& /*ChangedBounds*/)
{
	// 不过滤变更盒：无效唤醒由各产物的哈希短路吸收（同房子 v1 直推）。
	RequestReevaluate();
}

void ACSWallActor::HandleAnyGroundRebuilt(ACSGroundActor* RebuiltGround, const FBox& /*ChangedBounds*/)
{
	// 类级广播：编辑器世界与 PIE 世界的地面都会来，只认自己世界的；只标脏，不在别人的广播途中改委托表。
	if (IsValid(RebuiltGround) && RebuiltGround->GetWorld() == GetWorld()) RequestReevaluate();
}

bool ACSWallActor::SampleGround(const FVector2D& WorldXY, float& OutZ) const
{
	// `TrySampleHeight` 在地面范围外返回 false —— 不能用 `SampleHeight`（范围外退回地面 actor 的 Z，等于凭空垫一块平地）。
	return IsValid(Ground) && Ground->TrySampleHeight(WorldXY, OutZ);
}

// -----------------------------------------------------------------------------
// 样条 → 路径
// -----------------------------------------------------------------------------

void ACSWallActor::BuildPathFromSpline(FCSWallPath& Out) const
{
	Out = FCSWallPath();
	if (!Spline) return;
	const int32 NumPoints = Spline->GetNumberOfSplinePoints();
	if (NumPoints < 2) return;

	const bool bClosed = Spline->IsClosedLoop();
	const int32 NumSegments = bClosed ? NumPoints : NumPoints - 1;
	const double Spacing = FMath::Max(double(SampleSpacing), 10.0);
	const double SplineLength = Spline->GetSplineLength();

	auto AddPoint = [&Out](const FVector& P, double Z, bool bKink)
	{
		const FVector2D XY(P.X, P.Y);
		// 去重：控制点重合、零长段 —— 斜接在零长段上无从谈起（`CSWall::BuildSkins` 拒收）。重合时保住角标记。
		if (Out.Points.Num() > 0 && FVector2D::DistSquared(Out.Points.Last(), XY) < 1.0)
		{
			if (bKink) Out.Kinks.Last() = 1;
			return;
		}
		Out.Points.Add(XY);
		Out.BaseZ.Add(Z);
		Out.TopZ.Add(Z);
		Out.Kinks.Add(bKink ? 1 : 0);
	};

	for (int32 Segment = 0; Segment < NumSegments; ++Segment)
	{
		const int32 I0 = Segment;
		const int32 I1 = (Segment + 1) % NumPoints;
		const double D0 = Spline->GetDistanceAlongSplineAtSplinePoint(I0);
		const double D1 = (bClosed && I1 == 0) ? SplineLength : double(Spline->GetDistanceAlongSplineAtSplinePoint(I1));
		const double Z0 = Spline->GetLocationAtSplinePoint(I0, ESplineCoordinateSpace::World).Z;
		const double Z1 = Spline->GetLocationAtSplinePoint(I1, ESplineCoordinateSpace::World).Z;
		// 每段都采到控制点上（K = 0 就是控制点）：拐角恰好落在一个采样点上，角石与墙顶断点才有处可落。
		const int32 Steps = FMath::Clamp(FMath::CeilToInt32((D1 - D0) / Spacing), 1, 1024);
		for (int32 K = 0; K < Steps; ++K)
		{
			const double T = double(K) / double(Steps);
			const FVector P = Spline->GetLocationAtDistanceAlongSpline(float(FMath::Lerp(D0, D1, T)), ESplineCoordinateSpace::World);
			// 高度不走样条的三次插值（两点之间会过冲），在控制点之间线性插（同楼梯）。
			AddPoint(P, FMath::Lerp(Z0, Z1, T), K == 0);
		}
	}
	if (!bClosed)
	{
		const FVector Last = Spline->GetLocationAtSplinePoint(NumPoints - 1, ESplineCoordinateSpace::World);
		AddPoint(Last, Last.Z, true);
	}
	else if (Out.Points.Num() >= 2 && FVector2D::DistSquared(Out.Points[0], Out.Points.Last()) < 1.0)
	{
		Out.Points.Pop();
		Out.BaseZ.Pop();
		Out.TopZ.Pop();
		Out.Kinks.Pop();
	}
	Out.bClosed = bClosed && Out.Points.Num() >= 3;
}

bool ACSWallActor::ApplyGround(FCSWallPath& Path, const FCSWallSkins& Skins) const
{
	const int32 N = Path.NumPoints();
	const bool bUseGround = bFollowGround && IsValid(Ground);
	bool bAny = false;

	TArray<double> Top;
	Top.SetNumUninitialized(N);
	for (int32 Index = 0; Index < N; ++Index)
	{
		// 进来时 BaseZ 是样条自己的高度（`BuildPathFromSpline`）。
		double Base = Path.BaseZ[Index];
		double TopZ = Base + WallHeight;
		if (bUseGround)
		{
			// 墙厚两侧与中线三处：墙脚取最低点再往下埋（坡上哪一侧都不露缝），墙顶按中线处的地面起算。
			float Samples[3] = { 0.0f, 0.0f, 0.0f };
			const bool bHit[3] = {
				SampleGround(Path.Points[Index], Samples[0]),
				SampleGround(Skins.Left[Index], Samples[1]),
				SampleGround(Skins.Right[Index], Samples[2]) };
			double Lowest = TNumericLimits<double>::Max();
			double Sum = 0.0;
			int32 Count = 0;
			for (int32 K = 0; K < 3; ++K)
			{
				if (!bHit[K]) continue;
				Lowest = FMath::Min(Lowest, double(Samples[K]));
				Sum += Samples[K];
				++Count;
			}
			if (Count > 0)
			{
				Base = Lowest - GroundSink;
				TopZ = (bHit[0] ? double(Samples[0]) : Sum / Count) + WallHeight;
				bAny = true;
			}
		}
		Path.BaseZ[Index] = Base;
		Top[Index] = TopZ;
	}

	// 墙顶线平滑：地面 50 cm 一格，墙顶原样跟着走会一格一抖。墙脚不平滑（它要贴地）。开口两端钉住。
	const int32 Passes = bUseGround ? FMath::Clamp(TopSmoothing, 0, 16) : 0;
	for (int32 Pass = 0; Pass < Passes; ++Pass)
	{
		TArray<double> Next = Top;
		for (int32 Index = 0; Index < N; ++Index)
		{
			if (!Path.bClosed && (Index == 0 || Index == N - 1)) continue;
			const int32 Prev = (Index - 1 + N) % N;
			const int32 After = (Index + 1) % N;
			Next[Index] = (Top[Prev] + 2.0 * Top[Index] + Top[After]) * 0.25;
		}
		Top = MoveTemp(Next);
	}

	// 墙顶至少高出墙脚四分之一个墙高：陡坎下平滑过的墙顶可能压到墙脚以下，那一段会翻面。
	for (int32 Index = 0; Index < N; ++Index)
	{
		Path.TopZ[Index] = FMath::Max(Top[Index], Path.BaseZ[Index] + double(WallHeight) * 0.25);
	}
	return bAny;
}

// -----------------------------------------------------------------------------
// 重求值
// -----------------------------------------------------------------------------

void ACSWallActor::RebuildWall()
{
	bForceFullRebuild = true;
	ReevaluateSite();
}

bool ACSWallActor::GetWallPath(FCSWallPath& OutPath, FCSWallSkins& OutSkins, FTransform& OutWorld) const
{
	// 样条墙的路径就是上一次重建用的那一份（世界空间：样条 + 贴地之后）。
	FlushPendingReevaluate();
	OutPath = BuiltPath;
	OutSkins = BuiltSkins;
	OutWorld = FTransform::Identity;
	return BuiltPath.IsValid() && BuiltSkins.IsValid();
}

void ACSWallActor::ReevaluateSite()
{
	if (bInReevaluate || bSubscriptionsDisabled || IsTemplate() || !GetWorld()) return;
	TGuardValue<bool> Guard(bInReevaluate, true);
	// 欠账在开头清：重建途中新到的通知重新置位，由下一次 Tick 兑现。
	bReevaluatePending = false;

	SubscribeChanges();
	ResolveGroundAndSubscribe();

	// ① 样条 → 路径 → 皮线 → 贴地。
	FCSWallPath Path;
	BuildPathFromSpline(Path);
	FCSWallSkins Skins;
	const bool bValid = CSWall::BuildSkins(Path, WallThickness, Skins);
	bOnGround = bValid && ApplyGround(Path, Skins);
	BuiltPath = MoveTemp(Path);
	BuiltSkins = MoveTemp(Skins);
	if (!bValid)
	{
		BuiltPath = FCSWallPath();
		BuiltSkins = FCSWallSkins();
	}

	// ② 墙体 ③ 砖 ④ 藤。三样各自哈希守卫；路径无效时各自走清空分支。
	RebuildBody();
	RebuildBricks();
	RebuildVine();

	bForceFullRebuild = false;
	++RebuildCount;
}

// -----------------------------------------------------------------------------
// 墙体
// -----------------------------------------------------------------------------

void ACSWallActor::RebuildBody()
{
	if (!BuiltPath.IsValid() || !BuiltSkins.IsValid())
	{
		ClearBody();
		return;
	}

	// 形状哈希：路径逐点（0.5 cm）+ 墙厚 + 平滑阈值。材质不进（纯外观量走重绑，见 PostEditChangeProperty）。
	TArray<int32> H;
	H.Reserve(BuiltPath.NumPoints() * 4 + 4);
	H.Append({ BuiltPath.NumPoints(), int32(BuiltPath.bClosed), CSWallActor_Q(WallThickness, 0.1), CSWallActor_Q(CornerTurnDegrees, 0.1) });
	for (int32 Index = 0; Index < BuiltPath.NumPoints(); ++Index)
	{
		H.Append({ CSWallActor_Q(BuiltPath.Points[Index].X, 0.5), CSWallActor_Q(BuiltPath.Points[Index].Y, 0.5),
			CSWallActor_Q(BuiltPath.BaseZ[Index], 0.5), CSWallActor_Q(BuiltPath.TopZ[Index], 0.5) });
	}
	const uint32 NewHash = CSWallActor_Hash(H);
	// "网格对象在不在"不够：删除后撤销，复活的网格对象还在、显存却已被组件还掉了（同房子 IsSlotMeshLive）。
	if (!bForceFullRebuild && NewHash == BodyHash && IsSlotMeshLive(TinyGladeMesh)) return;
	BodyHash = NewHash;

	// 墙体与房子同一个 `CSWall::BuildBody`（同一个面板规划器），出面方式取 `Continuous`：沿墙连续 UV、贴地、弯墙平滑。
	FCSGpuMeshCPUData Snapshot;
	CSWall::FBodyParams Params;
	Params.Surface = ECSWallSurface::Continuous;
	Params.CornerTurnDegrees = CornerTurnDegrees;
	CurrentBodyTriangles = CSWall::BuildBody(BuiltPath, BuiltSkins, Params, Snapshot);
	++BodyUploadCount;
	SubmitBody(MakeShared<FCSGpuMeshCPUData, ESPMode::ThreadSafe>(MoveTemp(Snapshot)));
	UE_LOG(LogTinyGladeWall, Verbose, TEXT("[TinyGladeWall] %s body rebuilt: points=%d tris=%d length=%.0f"),
		*GetName(), BuiltPath.NumPoints(), CurrentBodyTriangles, BuiltSkins.Length);
}

void ACSWallActor::SubmitBody(TSharedPtr<FCSGpuMeshCPUData, ESPMode::ThreadSafe> Snapshot)
{
	FCSMeshSlotUpload Upload;
	Upload.Materials = { WallMaterial };
	Upload.NumTexCoordSets = 2;   // UV1 = 裁剪场（本版恒为哨兵，墙材质照读）
	SubmitMeshSlotAsync(TinyGladeMeshComponent, TinyGladeMesh, BodySlot, Snapshot, Upload, [this]() { OnBodyEditComplete(); });
}

void ACSWallActor::OnBodyEditComplete()
{
	// 最新态合并：在途期间攒下的最后一份现在补发（拖样条时常态命中）。
	if (const TSharedPtr<FCSGpuMeshCPUData, ESPMode::ThreadSafe> Next = TakePending(BodySlot.Pending)) SubmitBody(Next);
}

void ACSWallActor::ClearBody()
{
	if (TinyGladeMesh || BodySlot.Pending.IsValid()) ClearMeshSlot(TinyGladeMeshComponent, TinyGladeMesh, BodySlot.Pending);
	BodyHash = 0;
	CurrentBodyTriangles = 0;
}

// -----------------------------------------------------------------------------
// 砖：压顶 + 垛口 + 角石（与房子门框砖同一条 GPU 解析砖路）
// -----------------------------------------------------------------------------

FBox ACSWallActor::WorldBoundsToComponent(const FBox& WorldBounds, const USceneComponent* Component)
{
	if (!Component || !WorldBounds.IsValid) return WorldBounds;
	return WorldBounds.TransformBy(Component->GetComponentTransform().Inverse());
}

FBox ACSWallActor::GetWallWorldBounds(double ExtraTop) const
{
	FBox Box(ForceInit);
	if (!BuiltSkins.IsValid() || BuiltSkins.Left.Num() != BuiltPath.NumPoints()) return Box;
	for (int32 Index = 0; Index < BuiltPath.NumPoints(); ++Index)
	{
		for (const FVector2D& XY : { BuiltSkins.Left[Index], BuiltSkins.Right[Index] })
		{
			Box += FVector(XY.X, XY.Y, BuiltPath.BaseZ[Index]);
			Box += FVector(XY.X, XY.Y, BuiltPath.TopZ[Index] + ExtraTop);
		}
	}
	return Box;
}

CSShaperSteps::EHandoverResult ACSWallActor::EnsureBrickComponent()
{
	if (!IsValid(BrickComponent)) return CSShaperSteps::EHandoverResult::NotReady;
	// 空 = 画砖网格资产自己的材质；设了才整体覆盖。两个 setter 都是"变了才写"。
	BrickComponent->SetInstanceMaterial(BrickMaterial);
	BrickComponent->SetBaseMesh(BrickMesh);

	if (BrickGpuBuffers.Num() != 1)
	{
		CSShaperSteps::ReleaseOnRenderThread(BrickGpuBuffers);
		BrickGpuBuffers.SetNum(1);
		BrickHandover.Capacities.Reset();
	}

	// 块尺寸 = 想要的尺寸 / 网格自身尺寸（TG 的 brick 是 100³ 居中盒）。轴向约定同 kernel：
	// X = 进深（平顶段上是**竖直**的层高）、Y = 沿路长度（乘胀大）、Z = 横跨墙厚。
	const FBox Local = BrickMesh ? BrickMesh->GetBoundingBox() : FBox(ForceInit);
	BrickGpuBuffers[0].BaseSphereCentre = Local.IsValid ? FVector3f(Local.GetCenter()) : FVector3f::ZeroVector;
	BrickGpuBuffers[0].BaseSphereRadius = Local.IsValid ? float(Local.GetExtent().Size()) : 0.0f;
	const FVector MeshSize = Local.IsValid ? Local.GetSize() : FVector(1.0);
	const float Across = WallThickness + 2.0f * FMath::Max(BrickProtrude, 0.0f);
	BrickGpuBuffers[0].BlockSize = FVector3f(
		float(BrickHeight / FMath::Max(MeshSize.X, 1.0)),
		float(BrickLength * FMath::Max(BrickBloat, 1.0f) / FMath::Max(MeshSize.Y, 1.0)),
		float(Across / FMath::Max(MeshSize.Z, 1.0)));

	if (!BrickMesh) return CSShaperSteps::EHandoverResult::UpToDate;

	// 容量注册期一次付清、只截断不扩容（零阻塞纪律，同房子门框砖）。
	CSShaperSteps::ReserveCapacity(BrickGpuBuffers, uint32(FMath::Clamp(BrickReserveCapacity, 64, 65536)));

	// 交接包围盒：组件空间的保守盒，200 cm 台阶 + 只涨不缩 ⇒ 拖样条时不每帧重交。
	FBox LocalBounds = WorldBoundsToComponent(GetWallWorldBounds(BrickHeight * 3.0 + BrickLength), BrickComponent);
	if (!LocalBounds.IsValid) LocalBounds = FBox(FVector(-100.0), FVector(100.0));
	LocalBounds = CSShaperSteps::MergeHandoverBounds(CSWallActor_SnapBounds(LocalBounds.ExpandBy(Across)), BrickHandover, bForceFullRebuild);
	return CSShaperSteps::HandOverInstanceSource(
		CSShaperSteps::MakeHandoverSource(BrickComponent, BrickGpuBuffers[0], /*bWithCustomData*/ false),
		LocalBounds, BrickHandover);
}

void ACSWallActor::RebuildBricks()
{
	const CSShaperSteps::EHandoverResult HandoverResult = EnsureBrickComponent();
	const int32 Capacity = FMath::Clamp(BrickReserveCapacity, 64, 65536);

	TArray<CSHouseFrame::FElement> Elements;
	CurrentTop = CSWall::FTopResult();
	CurrentQuoinColumns = 0;
	int32 Count = 0;
	const uint32 Seed = HashCombine(GetTypeHash(BrickSeed), 0x57414C4Cu);   // 'WALL'
	if (bBricksEnabled && BrickMesh && BuiltPath.IsValid() && BuiltSkins.IsValid())
	{
		CSHouseFrame::FBrickParams Params;
		Params.Length = FMath::Max(BrickLength, 1.0f);
		Params.Gap = 0.0f;
		Params.MaxBricks = Capacity;

		CSWall::FTopParams Top;
		Top.BrickLength = Params.Length;
		Top.CourseHeight = FMath::Max(BrickHeight, 1.0f);
		Top.Sink = FMath::Max(CopingSink, 0.0f);
		Top.bCoping = bCoping;
		Top.bMerlons = bMerlons;
		Top.MerlonEvery = MerlonEvery;
		Top.CornerTurnDegrees = CornerTurnDegrees;
		Top.Seed = Seed;
		CurrentTop = CSWall::BuildTopElements(BuiltPath, BuiltSkins, Top, Params, Elements);

		// 角石与房子同一个 `CSWall::BuildQuoins`：独立墙的规矩（墙端 + 按夹角判的拐角、凸侧出）。
		TArray<CSHouseQuoin::FQuoin> Quoins;
		CSWall::FQuoinParams QuoinSite;
		QuoinSite.Kind = ECSWallKind::Freestanding;
		QuoinSite.bEnds = bEndQuoins;
		QuoinSite.bCorners = bCornerQuoins;
		QuoinSite.CornerTurnDegrees = CornerTurnDegrees;
		CurrentQuoinColumns = CSWall::BuildQuoins(BuiltPath, BuiltSkins, QuoinSite, Quoins);
		if (!Quoins.IsEmpty())
		{
			CSHouseFrame::FBrickParams QuoinParams = Params;
			QuoinParams.Jitter = FMath::Max(QuoinJitter, 0.0f);
			QuoinParams.SplitJitter = FMath::Max(QuoinSplitJitter, 0.0f);
			const FVector MeshSize = BrickMesh->GetBoundingBox().GetSize();
			const FVector2f MeshInvSize(1.0 / FMath::Max(MeshSize.X, 1.0), 1.0 / FMath::Max(MeshSize.Z, 1.0));
			CSHouseQuoin::BuildQuoinElements(Quoins, Seed, QuoinParams, Elements, MeshInvSize);
		}
		Count = CSHouseFrame::NextBrickSlot(Elements);
		if (Count >= Capacity)
		{
			UE_LOG(LogTinyGladeWall, Warning, TEXT("[TinyGladeWall] %s 砖数撞上常驻容量 %d（只截断不扩容）—— 调大 BrickReserveCapacity。"),
				*GetName(), Capacity);
		}
	}

	// 世界 → 组件：用组件自己的变换求逆（同房子 RebuildFrame 那条口径）。必须在哈希之前算。
	const FMatrix44f WorldToComponent = IsValid(BrickComponent)
		? FMatrix44f(BrickComponent->GetComponentTransform().ToInverseMatrixWithScale()) : FMatrix44f::Identity;
	// 一块砖都没有时哈希必须是 0（不是某个非零常数）：否则"没有砖"的墙每次重求值都判成变了、
	// 撤一次实例源、下次再交一次 —— 每轮两次阻塞（房子 RebuildFrame 那段注释里的实测）。
	uint32 NewHash = 0;
	if (Count > 0)
	{
		const TArray<int32> BlockInput = { CSWallActor_Q(BrickHeight, 0.1), CSWallActor_Q(BrickLength, 0.1), CSWallActor_Q(BrickBloat, 0.001),
			CSWallActor_Q(WallThickness + 2.0f * BrickProtrude, 0.1), int32(GetTypeHash(BrickMesh.Get())), int32(Seed) };
		NewHash = HashCombine(CSWallActor_HashElements(Elements, WorldToComponent), CSWallActor_Hash(BlockInput));
	}

	const bool bBuffersReady = BrickGpuBuffers.Num() == 1 && BrickGpuBuffers[0].IsValid() && BrickHandover.Capacities.Num() == 1;
	// 刚交接过（扩容换了清零的新 buffer / 包围盒变了）就必须重排一次，哈希没变也不许早退（房子审查 B1）。
	const bool bMustRescatter = HandoverResult == CSShaperSteps::EHandoverResult::HandedOver && Count > 0;
	if (!bMustRescatter && NewHash == BrickHash && CurrentBrickCount == Count && (Count == 0 || bBuffersReady)) return;

	if (Count == 0)
	{
		// ⚠️ 撤实例源之前先把 counter 清零（空表的 Scatter 就是干这个的）：撤掉之后交接缓存被清空，
		// 下一轮会把同一批 buffer 交回去，陈旧计数器会把上一代的砖再画一遍（房子门框砖栽过）。
		if (IsValid(BrickComponent) && bBuffersReady) CSHouseFrame::Scatter(Elements, BrickGpuBuffers, WorldToComponent);
		if (IsValid(BrickComponent)) BrickComponent->ClearInstanceSourceGPU();
		BrickHandover.Reset();
		BrickHash = 0;
		CurrentBrickCount = 0;
		++BrickScatterCount;
		return;
	}
	// 源没交出去（组件不在 / 容量没备好）：这一轮不录，哈希不推进，下一轮重试。
	if (!IsValid(BrickComponent) || !bBuffersReady) return;

	BrickHash = NewHash;
	CurrentBrickCount = Count;
	++BrickScatterCount;
	// **一个字节都不分配**：砖数已按常驻容量截断。整面墙一个 dispatch。
	CSHouseFrame::Scatter(Elements, BrickGpuBuffers, WorldToComponent);
	UE_LOG(LogTinyGladeWall, Verbose, TEXT("[TinyGladeWall] %s bricks scattered: coping=%d merlons=%d quoinColumns=%d total=%d"),
		*GetName(), CurrentTop.CopingBricks, CurrentTop.MerlonBricks, CurrentQuoinColumns, Count);
}

// -----------------------------------------------------------------------------
// 藤（与房子 D13 同一套：纯函数规划 + 叶花实例 + 枝管子 + 生长相位）
// -----------------------------------------------------------------------------

CSShaperSteps::EHandoverResult ACSWallActor::EnsureVineComponents()
{
	// 组件 / 季节与生长 MID / 基础网格快照 / 交接：墙的基类 `ACSWallBase::EnsureVineRig`（房子同一份）。
	// 这里只给样条墙自己的两样：常驻容量（两面墙长）与交接包围盒（墙的世界包围盒）。
	const FCSWallVineSettings& V = Vine;
	return EnsureVineRig(V, [this, &V](uint32& OutMaxRecords, FBox& OutLocalBounds)
	{
		// 容量按配置上限一次付清（两面墙的长度 / 藤距 × 每根最多几段），再走一次 ReserveCount 的台阶 ——
		// 上限是墙长的连续函数，直接喂会让拖样条时每涨过一个藤距就重分配一次（房子那条实测 21 次阻塞）。
		const double FaceLength = 2.0 * (BuiltSkins.Length + WallThickness);
		const int32 MaxStrands = FMath::CeilToInt32(FaceLength / FMath::Max(double(V.StrandSpacing), 20.0)) + 2;
		OutMaxRecords = uint32(FMath::Clamp(
			CSShaperSteps::ReserveCount(MaxStrands * FMath::Clamp(V.MaxSegments, 1, 128)), 64, 1 << 16));

		// 交接包围盒：组件空间的保守盒，200 cm 台阶（只涨不缩由基类的 MergeHandoverBounds 负责）⇒ 拖样条时不每帧重交。
		const double Reach = FMath::Max(V.LeafSize, V.FlowerSize) + V.StandOff + WallThickness;
		FBox LocalBounds = WorldBoundsToComponent(GetWallWorldBounds(Reach).ExpandBy(Reach), VineLeafComponent);
		if (!LocalBounds.IsValid) LocalBounds = FBox(FVector(-100.0), FVector(100.0));
		OutLocalBounds = CSWallActor_SnapBounds(LocalBounds);
	}, bForceFullRebuild);
}

void ACSWallActor::ClearVine()
{
	const bool bHadAny = CurrentVineStrands != 0 || CurrentVineLeaves != 0 || CurrentVineFlowers != 0 || CurrentVineBranches != 0
		|| VineHandover.Capacities.Num() > 0 || VineTubeMesh != nullptr;
	if (!bHadAny) return;
	// 先清 counter 再撤实例源、管子与交接缓存（顺序的理由见 `ACSWallBase::ClearVineRig`）。
	ClearVineRig();
	CurrentVineStrands = 0;
	CurrentVineBranches = 0;
	CurrentVineLeaves = 0;
	CurrentVineFlowers = 0;
	VineHash = 0;
}

void ACSWallActor::RebuildVine()
{
	const FCSWallVineSettings& V = Vine;
	if (!V.bEnabled || (!V.bUseTube && !V.BranchMesh) || !V.LeafMesh || !BuiltPath.IsValid() || !BuiltSkins.IsValid())
	{
		ClearVine();
		return;
	}

	const CSShaperSteps::EHandoverResult HandoverResult = EnsureVineComponents();
	if (!bVineBaseMeshReady) return;

	// 墙脚埋进地里 `GroundSink`，藤从地面（墙脚线 + 埋深）长起；藤爬到墙顶为止（压顶砖在它上面）。
	const float BaseLift = (bFollowGround && bOnGround) ? GroundSink : 0.0f;
	double MinHeight = TNumericLimits<double>::Max();
	for (int32 Index = 0; Index < BuiltPath.NumPoints(); ++Index)
	{
		MinHeight = FMath::Min(MinHeight, BuiltPath.TopZ[Index] - BuiltPath.BaseZ[Index] - BaseLift);
	}
	// 藤的墙面与房子同一个 `CSWall::BuildVineStrips`：独立墙两面都长（房子只长外皮）。
	TArray<CSHouseVine::FWallStrip> Strips;
	CSWall::FVineStripParams StripSite;
	StripSite.Kind = ECSWallKind::Freestanding;
	StripSite.BaseLift = BaseLift;
	StripSite.VineHeight = float(MinHeight);
	StripSite.GroundSampleSpacing = V.GroundSampleSpacing;
	StripSite.CornerTurnDegrees = CornerTurnDegrees;
	CSWall::BuildVineStrips(BuiltPath, BuiltSkins, StripSite,
		[this](const FVector2D& XY, float& OutZ) { return SampleGround(XY, OutZ); }, Strips);

	// 参数表与房子同一份（`FCSWallVineSettings` → `FParams`，花网格没配时花的概率钉 0）。
	const CSHouseVine::FParams Params = MakeVineParams(V);

	CSHouseVine::FPlan Plan;
	CSHouseVine::BuildPlan(Strips, CSWallActor_NoOpenings(), Params, Plan);

	// 幂等短路。短路点在规划**之后**（规划是纯 CPU、同时是哈希最诚实的来源 —— 同房子）。
	double GapSummary = 0.0;
	for (const CSHouseVine::FWallStrip& S : Strips)
	{
		for (float G : S.GroundGaps) GapSummary = FMath::Max(GapSummary, double(G));
	}
	const TArray<int32> HashInput = {
		Plan.Branch.Num(), Plan.Leaf.Num(), Plan.Flower.Num(), Plan.Strands.Num(), int32(BodyHash),
		CSWallActor_Q(MinHeight, 1.0), CSWallActor_Q(GapSummary, 1.0),
		CSWallActor_Q(V.StrandSpacing, 0.5), CSWallActor_Q(V.SegmentLength, 0.5), V.MaxSegments,
		CSWallActor_Q(V.Wander, 0.01), CSWallActor_Q(V.MaxLean, 0.01), CSWallActor_Q(V.MaxTurn, 0.01), CSWallActor_Q(V.Bloat, 0.01),
		CSWallActor_Q(V.Thickness, 0.1), CSWallActor_Q(V.StandOff, 0.1), CSWallActor_Q(V.LeafSize, 0.1),
		CSWallActor_Q(V.LeafChance, 0.01), CSWallActor_Q(V.LeafSizeJitter, 0.01), V.Seed,
		CSWallActor_Q(Params.FlowerChance, 0.01), CSWallActor_Q(V.FlowerFromFrac, 0.01), CSWallActor_Q(V.FlowerSize, 0.1),
		CSWallActor_Q(V.JumpChance, 0.01), CSWallActor_Q(V.MaxGroundGap, 1.0),
		CSWallActor_Q(V.TipTaperLength, 1.0), CSWallActor_Q(V.TipTaperMin, 0.01), CSWallActor_Q(V.GrowSpeed, 1.0),
		int32(V.bUseTube), V.TubeSegments, V.TubeSubdivide };
	const uint32 NewHash = CSWallActor_Hash(HashInput);

	const bool bBuffersReady = AreVineBuffersReady();
	const bool bMustRepack = HandoverResult == CSShaperSteps::EHandoverResult::HandedOver;
	if (!bMustRepack && NewHash == VineHash && VineHandover.Capacities.Num() == CSHouseVine::Palette_Num && bBuffersReady) return;
	if (!bBuffersReady) return;

	VineHash = NewHash;
	CurrentVineStrands = Plan.Strands.Num();
	CurrentVineBranches = Plan.Branch.Num();
	CurrentVineLeaves = Plan.Leaf.Num();
	CurrentVineFlowers = Plan.Flower.Num();

	// 生长相位 → 叶 / 花回填 → 打包实例 → 枝的管子：墙的基类 `ACSWallBase::PackVine`（房子同一份）。
	PackVine(V, Params, Strips, Plan);

	UE_LOG(LogTinyGladeWall, Verbose, TEXT("[TinyGladeWall] %s vine packed: strands=%d branches=%d leaves=%d flowers=%d"),
		*GetName(), CurrentVineStrands, CurrentVineBranches, CurrentVineLeaves, CurrentVineFlowers);
}

// 藤的生长相位 / 管子递交在墙的基类 `ACSWallBase` 里（`CSHouseVine::ResolveSpawnTimes` / `SubmitVineTube`），房子同一份。

// -----------------------------------------------------------------------------
// 诊断 / 实例族 / 生命周期
// -----------------------------------------------------------------------------

int32 ACSWallActor::DebugReadBrickCountGpuSync() const
{
	FlushPendingReevaluate();
	return IsValid(BrickComponent) ? BrickComponent->DebugReadDrawnInstanceCountSync() : 0;
}

int32 ACSWallActor::DebugReadVineLeafCountGpuSync() const
{
	FlushPendingReevaluate();
	return IsValid(VineLeafComponent) ? VineLeafComponent->DebugReadDrawnInstanceCountSync() : 0;
}

FString ACSWallActor::DebugGetGpuAssetMismatchSync() const
{
	FlushPendingReevaluate();
	return Super::DebugGetGpuAssetMismatchSync();
}

void ACSWallActor::GetInstancedFamilies(TArray<FCSInstancedFamily>& OutFamilies) const
{
	// 不许在这里补票：族表必须无副作用（补票在诊断入口里）。
	OutFamilies.Add({ BrickComponent, TEXT("墙砖"), TEXT("WallBricks"), CurrentBrickCount > 0 });
	OutFamilies.Add({ VineBranchComponent, TEXT("藤枝"), TEXT("VineBranch"), !Vine.bUseTube && CurrentVineBranches > 0 });
	OutFamilies.Add({ VineLeafComponent, TEXT("藤叶"), TEXT("VineLeaf"), CurrentVineLeaves > 0 });
	OutFamilies.Add({ VineFlowerComponent, TEXT("藤花"), TEXT("VineFlower"), CurrentVineFlowers > 0 });
}

void ACSWallActor::ReleaseInstancedBuffers()
{
	// 只放本 actor 分配的生产者那一份；组件那一份由组件在 OnComponentDestroyed 里自己放。
	CSShaperSteps::ReleaseOnRenderThread(BrickGpuBuffers);
	CSShaperSteps::ReleaseOnRenderThread(VineGpuBuffers);
	BrickHandover.Reset();
	VineHandover.Reset();
	bVineBaseMeshReady = false;
	// 编辑器里删除可撤销、复活的是同一批对象：下一次重求值必须全量重建（同房子）。
	bForceFullRebuild = true;
	BrickHash = 0;
	VineHash = 0;
	BodyHash = 0;
}

// -----------------------------------------------------------------------------
// AActor
// -----------------------------------------------------------------------------

void ACSWallActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	if (IsTemplate() || !GetWorld()) return;
	bSubscriptionsDisabled = false;
	SubscribeChanges();
	RequestReevaluate();
}

void ACSWallActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 覆盖 PIE 结束与关卡卸载 —— 这两条 `Destroyed` 收不到。
	bSubscriptionsDisabled = true;
	UnsubscribeChanges();
	UnsubscribeGround();
	Super::EndPlay(EndPlayReason);
}

void ACSWallActor::Destroyed()
{
	// 编辑器 world 里只有这一条会来（没有 EndPlay）。
	bSubscriptionsDisabled = true;
	UnsubscribeChanges();
	UnsubscribeGround();
	Super::Destroyed();
}

#if WITH_EDITOR
void ACSWallActor::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	// 样条可视化器每次拖控制点 / 切线都发 PostEditMove(false)：跟手走合批，松手当场补齐（同楼梯）。
	RequestReevaluate();
	if (bFinished) FlushPendingReevaluate();
}

void ACSWallActor::PostEditUndo()
{
	Super::PostEditUndo();
	// 撤销删除时复活的是同一批对象（显存已被组件还掉）：全量重建。
	bSubscriptionsDisabled = false;
	RebuildWall();
}

void ACSWallActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	// 墙面材质是纯外观量：只重绑，不重建（墙体哈希里没有它 —— 同房子 D14）。砖 / 藤的材质在各自的
	// Ensure* 里每轮都会"变了才写"，构造脚本重跑那一趟就带上了。
	if (PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(ACSWallActor, WallMaterial))
	{
		BindTinyGladeMaterials({ WallMaterial });
	}
}
#endif
