#include "CSStairsActor.h"

#include "CSGpuInstancedMeshComponent.h"
#include "CSGroundActor.h"
#include "Components/SplineComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"   // TActorIterator
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogTinyGladeStairs, Log, All);

const TCHAR* ACSStairsActor::DefaultBrickMeshPath = TEXT("/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/brick");

ACSStairsActor::ACSStairsActor()
{
	// 楼梯路径（用户 2026-09-16："默认 actor 中添加 spline component"）。默认一条上行的三点样条：
	// 水平约 520 cm、升 250 cm，坡度 ≈ 0.48 —— 落在踏步档（0.25 < s ≤ 2.5）里，放进关卡不调参就出楼梯。
	Spline = CreateDefaultSubobject<USplineComponent>(TEXT("Spline"));
	Spline->SetupAttachment(RootComponent);
	Spline->ClearSplinePoints(false);
	Spline->AddSplinePoint(FVector(0.0, 0.0, 0.0), ESplineCoordinateSpace::Local, false);
	Spline->AddSplinePoint(FVector(250.0, 0.0, 125.0), ESplineCoordinateSpace::Local, false);
	Spline->AddSplinePoint(FVector(500.0, 150.0, 250.0), ESplineCoordinateSpace::Local, false);
	Spline->UpdateSpline();

	// 与 `ACSPointBrushActor` 同一种配法：默认子对象、挂根下。名字稳定 ⇒ 烘焙出来的资产名也稳定
	// （基类 `FCSInstancedFamily` 那段注释说的正是 NewObject 自动命名会让资产名漂移）。
	BrickComponent = CreateDefaultSubobject<UCSGpuInstancedMeshComponent>(TEXT("StairBricks"));
	BrickComponent->SetupAttachment(RootComponent);

	// `FObjectFinderOptional`：找不到只是没有默认砖，不会把 CDO 构造带崩（同 `ACSWindowMarker`）。
	static ConstructorHelpers::FObjectFinderOptional<UStaticMesh> BrickAsset(DefaultBrickMeshPath);
	BrickMesh = BrickAsset.Get();
}

CSStairs::FParams ACSStairsActor::MakeParams() const
{
	CSStairs::FParams Params;
	Params.StepLength = StepLength;
	Params.MinBlockHeight = MinBlockHeight;
	Params.DepthOverlap = DepthOverlap;
	Params.BuryTolerance = BuryTolerance;
	Params.TreadClearance = TreadClearance;
	Params.bSolidToGround = bSolidToGround;
	return Params;
}

CSStairs::FGroundSampler ACSStairsActor::MakeGroundSampler() const
{
	if (!bFollowGround || !IsValid(Ground)) return CSStairs::FGroundSampler();
	// 同步用完即弃（`RebuildStairs` 一次调用之内），裸指针不会活过地面本身。
	const ACSGroundActor* G = Ground;
	return [G](const FVector2D& XY) { return G->SampleHeight(XY); };
}

void ACSStairsActor::BuildRuns(TArray<CSStairs::FRun>& OutRuns) const
{
	OutRuns.Reset();
	if (!Spline) return;

	const int32 NumPoints = Spline->GetNumberOfSplinePoints();
	if (NumPoints < 2) return;

	const CSStairs::FParams Params = MakeParams();
	const CSStairs::FGroundSampler GroundSampler = MakeGroundSampler();
	const bool bClosed = Spline->IsClosedLoop();
	const int32 NumSegments = bClosed ? NumPoints : NumPoints - 1;

	CSStairs::FRun Current;
	for (int32 Segment = 0; Segment < NumSegments; ++Segment)
	{
		const int32 I0 = Segment;
		const int32 I1 = (Segment + 1) % NumPoints;
		const float D0 = Spline->GetDistanceAlongSplineAtSplinePoint(I0);
		const float D1 = (bClosed && I1 == 0) ? Spline->GetSplineLength() : Spline->GetDistanceAlongSplineAtSplinePoint(I1);
		const FVector P0 = Spline->GetLocationAtSplinePoint(I0, ESplineCoordinateSpace::World);
		const FVector P1 = Spline->GetLocationAtSplinePoint(I1, ESplineCoordinateSpace::World);
		// 节点宽 = 属性宽 × 该点缩放的 Y：视口里缩放样条点就是调这一端的宽度（TG 逐节点的 `width`）。
		const float W0 = Width * float(FMath::Abs(Spline->GetScaleAtSplinePoint(I0).Y));
		const float W1 = Width * float(FMath::Abs(Spline->GetScaleAtSplinePoint(I1).Y));

		// 密采样：点距 ≤ `SampleSpacing`，两端都采（附录 D §1.3 第 2 步）。
		const float SegmentLength = FMath::Max(D1 - D0, 0.0f);
		const int32 Divisions = FMath::Max(FMath::CeilToInt(SegmentLength / Params.SampleSpacing), 1);

		TArray<CSStairs::FSample> Samples;
		Samples.Reserve(Divisions + 1);
		for (int32 K = 0; K <= Divisions; ++K)
		{
			const float T = float(K) / float(Divisions);
			const float Distance = FMath::Lerp(D0, D1, T);
			const FVector OnSpline = Spline->GetLocationAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);
			FVector2D Dir = FVector2D(Spline->GetTangentAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World)).GetSafeNormal();
			if (Dir.IsNearlyZero()) Dir = FVector2D(P1 - P0).GetSafeNormal();
			if (Dir.IsNearlyZero()) Dir = FVector2D(1.0, 0.0);

			CSStairs::FSample Sample;
			Sample.Width = FMath::Lerp(W0, W1, T);
			Sample.Dir = Dir;
			// ⚠️ **高度不走样条的 Z，在两端之间线性插值**（TG：样条只管 XZ，高度单独插值）。
			// 样条的三次插值在两点之间会过冲，楼梯中段就会出现"先下后上"的鼓包。
			const double LerpZ = FMath::Lerp(P0.Z, P1.Z, double(T));
			Sample.Position = FVector(OnSpline.X, OnSpline.Y, LerpZ);
			Sample.Position.Z = CSStairs::ClampToGround(Sample.Position, Dir, Sample.Width, GroundSampler, Params);
			Samples.Add(Sample);
		}

		// 分类用**首尾弦**（夹地之后的高度），与 TG `determine_segment_type` 同口径。
		const FVector& First = Samples[0].Position;
		const FVector& Last = Samples.Last().Position;
		const CSStairs::ESegmentType Type = CSStairs::ClassifySegment(float(Last.Z - First.Z),
			float(FVector2D::Distance(FVector2D(First.X, First.Y), FVector2D(Last.X, Last.Y))), Params);

		// 同类型的相邻段并成一个 run（共用的那个端点只留一份）；换类型就收尾、另起一个 —— 新 run 带上
		// 交界点，两段在交界处首尾相接，不会断开一级。
		if (Current.Samples.IsEmpty())
		{
			Current.Type = Type;
			Current.Samples = MoveTemp(Samples);
		}
		else if (Current.Type == Type)
		{
			Current.Samples.Append(Samples.GetData() + 1, Samples.Num() - 1);
		}
		else
		{
			OutRuns.Add(MoveTemp(Current));
			Current = CSStairs::FRun();
			Current.Type = Type;
			Current.Samples = MoveTemp(Samples);
		}
	}
	if (Current.Samples.Num() >= 2) OutRuns.Add(MoveTemp(Current));
}

void ACSStairsActor::RebuildStairs()
{
	if (IsTemplate() || !GetWorld() || !BrickComponent) return;

	ResolveGroundAndSubscribe();

	TArray<CSStairs::FRun> Runs;
	BuildRuns(Runs);

	const CSStairs::FParams Params = MakeParams();
	const CSStairs::FGroundSampler GroundSampler = MakeGroundSampler();

	TArray<CSStairs::FBrick> Bricks;
	int32 Steps = 0;
	int32 LadderRuns = 0;
	for (int32 RunIndex = 0; RunIndex < Runs.Num(); ++RunIndex)
	{
		// 梯子段（坡度 > 2.5）TG 出的是木梯（`construct_ladder`），本项目没有对位资产 ⇒ **不出几何、出声**，
		// 而不是硬按踏步砌：68° 以上的"踏步"每级 46 高 19 深，看着像一堵锯齿墙（附录 D §9.3）。
		if (Runs[RunIndex].Type == CSStairs::ESegmentType::Ladder)
		{
			++LadderRuns;
			continue;
		}
		// 种子按 run 分开：前一段多一级，不该把后面每一段的切砖全部重掷一遍。
		const uint32 RunSeed = HashCombine(GetTypeHash(Seed), GetTypeHash(RunIndex));
		Steps += CSStairs::BuildRunBricks(Runs[RunIndex], Params, GroundSampler, RunSeed, Bricks);
	}
	if (LadderRuns > 0)
	{
		UE_LOG(LogTinyGladeStairs, Warning,
			TEXT("[TinyGladeStairs] %s: %d 段坡度超过 %.2f（TG 在这里出梯子），本项目不出梯子 —— 这几段留空。把样条点拉开或降低高差。"),
			*GetName(), LadderRuns, Params.LadderMinSlope);
	}
	CurrentLadderRunCount = LadderRuns;

	// 砖 → 实例变换。**按网格自己的包围盒换算**，不假定它就是 100 cm 居中立方体：换一张砖资产也摆得对。
	TArray<FTransform> Transforms;
	if (BrickMesh)
	{
		const FBox MeshBox = BrickMesh->GetBoundingBox();
		const FVector MeshSize = MeshBox.GetSize().ComponentMax(FVector(UE_KINDA_SMALL_NUMBER));
		const FVector MeshCenter = MeshBox.GetCenter();
		Transforms.Reserve(Bricks.Num());
		for (const CSStairs::FBrick& Brick : Bricks)
		{
			const FVector Scale = Brick.Size / MeshSize;
			// 网格中心经缩放、旋转之后要落在砖心上 ⇒ 平移 = 砖心 − R·(S·网格中心)。
			const FVector Location = Brick.Center - Brick.Rotation.RotateVector(MeshCenter * Scale);
			Transforms.Add(FTransform(Brick.Rotation, Location, Scale));
		}
	}

	// 幂等短路：砖表（量化到 0.1 cm）+ 网格 / 材质身份 + 组件变换。`SetInstances` 带一次阻塞刷新，
	// 地面每广播一次就会叫到每一座楼梯，绝大多数根本没变。组件变换进哈希是因为实例存的是组件局部量。
	TArray<int32> HashInput;
	HashInput.Reserve(Transforms.Num() * 10 + 8);
	HashInput.Add(int32(GetTypeHash(BrickMesh.Get())));
	HashInput.Add(int32(GetTypeHash(BrickMaterial.Get())));
	const FTransform ComponentTransform = BrickComponent->GetComponentTransform();
	auto Q = [](double V) { return int32(FMath::RoundToDouble(V * 10.0)); };
	for (const FVector& V : { ComponentTransform.GetLocation(), ComponentTransform.GetRotation().Euler() }) HashInput.Append({ Q(V.X), Q(V.Y), Q(V.Z) });
	for (const FTransform& T : Transforms)
	{
		const FVector L = T.GetLocation();
		const FVector S = T.GetScale3D();
		const FQuat R = T.GetRotation();
		HashInput.Append({ Q(L.X), Q(L.Y), Q(L.Z), Q(S.X * 100.0), Q(S.Y * 100.0), Q(S.Z * 100.0),
			Q(R.X * 1000.0), Q(R.Y * 1000.0), Q(R.Z * 1000.0), Q(R.W * 1000.0) });
	}
	const uint32 NewHash = FCrc::MemCrc32(HashInput.GetData(), HashInput.Num() * sizeof(int32));

	CurrentBricks = MoveTemp(Bricks);
	CurrentStepCount = BrickMesh ? Steps : 0;

	const bool bInstancesMatch = BrickComponent->GetInstanceCount() == Transforms.Num();
	if (NewHash == BrickHash && bInstancesMatch && BrickComponent->BaseMesh == BrickMesh) return;
	BrickHash = NewHash;

	BrickComponent->SetInstanceMaterial(BrickMaterial);
	BrickComponent->SetBaseMesh(BrickMesh);
	if (Transforms.IsEmpty())
	{
		BrickComponent->ClearInstances();
	}
	else
	{
		BrickComponent->SetInstances(Transforms, /*bWorldSpace*/ true);
	}
	++UploadCount;

	UE_LOG(LogTinyGladeStairs, Verbose, TEXT("[TinyGladeStairs] %s rebuilt: runs=%d steps=%d bricks=%d"),
		*GetName(), Runs.Num(), CurrentStepCount, Transforms.Num());
}

void ACSStairsActor::ReevaluateSite()
{
	RebuildStairs();
}

void ACSStairsActor::GetInstancedFamilies(TArray<FCSInstancedFamily>& OutFamilies) const
{
	OutFamilies.Add({ BrickComponent, TEXT("楼梯砖"), TEXT("StairBricks"), CurrentStepCount > 0 });
}

void ACSStairsActor::ResolveGroundAndSubscribe()
{
	if (!bFollowGround) return;
	if (!IsValid(Ground) && GetWorld())
	{
		// 与房子同一口径：场景里第一块地面。换地面场景先退订再来。
		UnsubscribeGround();
		for (TActorIterator<ACSGroundActor> It(GetWorld()); It; ++It) { Ground = *It; break; }
	}
	if (!IsValid(Ground) || GroundChangedHandle.IsValid()) return;
	GroundChangedHandle = Ground->OnGroundChanged.AddUObject(this, &ACSStairsActor::HandleGroundChanged);
}

void ACSStairsActor::UnsubscribeGround()
{
	if (IsValid(Ground) && GroundChangedHandle.IsValid()) Ground->OnGroundChanged.Remove(GroundChangedHandle);
	GroundChangedHandle.Reset();
}

void ACSStairsActor::HandleGroundChanged(ACSGroundActor* ChangedGround, const FBox& ChangedBounds)
{
	// 变化区域与楼梯的包围盒不相交就不必重算（笔刷每帧广播，大多数楼梯离它很远）。
	// 包围盒取样条的世界包围盒，再往外扩一个宽度 —— 贴地判据看的是左右各 1/4 宽处的地面。
	if (Spline && ChangedBounds.IsValid)
	{
		const FBox SplineBox = Spline->Bounds.GetBox().ExpandBy(FVector(Width, Width, 1.0e6));
		if (!SplineBox.Intersect(ChangedBounds)) return;
	}
	RebuildStairs();
}

void ACSStairsActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnsubscribeGround();
	Super::EndPlay(EndPlayReason);
}

void ACSStairsActor::Destroyed()
{
	UnsubscribeGround();
	Super::Destroyed();
}
