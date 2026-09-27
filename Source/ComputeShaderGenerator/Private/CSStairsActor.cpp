#include "CSStairsActor.h"

#include "CSGpuInstancedMeshComponent.h"
#include "CSGroundActor.h"
#include "CSHouseActor.h"
#include "CSHouseLibrary.h"
#include "Components/SplineComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"   // TActorIterator
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogTinyGladeStairs, Log, All);

namespace
{
void CSStairsActor_UploadPieces(UCSGpuInstancedMeshComponent* Component, UStaticMesh* Mesh, UMaterialInterface* Material,
	const TArray<CSStairs::FBrick>& Pieces, uint32& LastHash)
{
	if (!Component) return;
	TArray<FTransform> Transforms;
	TArray<int32> HashInput;
	HashInput.Append({ int32(GetTypeHash(Mesh)), int32(GetTypeHash(Material)) });
	auto Q = [](double V) { return int32(FMath::RoundToDouble(V * 1000.0)); };
	const FTransform WorldToLocal = Component->GetComponentTransform().Inverse();
	if (Mesh)
	{
		const FBox Box = Mesh->GetBoundingBox();
		const FVector Size = Box.GetSize().ComponentMax(FVector(0.01));
		for (const CSStairs::FBrick& B : Pieces)
		{
			const FVector Scale = B.Size / Size;
			const FVector Location = B.Center - B.Rotation.RotateVector(Box.GetCenter() * Scale);
			const FTransform T = FTransform(B.Rotation, Location, Scale) * WorldToLocal;
			Transforms.Add(T);
			for (const FVector& V : { T.GetLocation(), T.GetRotation().Euler(), T.GetScale3D() }) HashInput.Append({ Q(V.X), Q(V.Y), Q(V.Z) });
		}
	}
	const uint32 Hash = FCrc::MemCrc32(HashInput.GetData(), HashInput.Num() * sizeof(int32));
	if (LastHash == Hash && Component->BaseMesh == Mesh && Component->GetInstanceCount() == Transforms.Num()) return;
	LastHash = Hash;
	Component->SetBaseMesh(Mesh);
	Component->SetInstanceMaterial(Material);
	if (Transforms.IsEmpty()) Component->ClearInstances();
	else Component->SetInstances(Transforms, false);
}
}

const TCHAR* ACSStairsActor::DefaultBrickMeshPath = TEXT("/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/brick");

ACSStairsActor::ACSStairsActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
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
	RailingComponent = CreateDefaultSubobject<UCSGpuInstancedMeshComponent>(TEXT("StairRailing"));
	RailingComponent->SetupAttachment(RootComponent);
	LadderComponent = CreateDefaultSubobject<UCSGpuInstancedMeshComponent>(TEXT("StairLadders"));
	LadderComponent->SetupAttachment(RootComponent);
	static ConstructorHelpers::FObjectFinderOptional<UStaticMesh> WoodAsset(TEXT("/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/wooden_plank"));
	WoodMesh = WoodAsset.Get();

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
	Params.bArchedSupport = bArchedSupport;
	Params.Railing = Railing;
	Params.bNarrowByRailing = Railing != ECSStairsRailing::None;
	return Params;
}

CSStairs::FGroundSampler ACSStairsActor::MakeGroundSampler() const
{
	if (!bFollowGround || !IsValid(Ground)) return CSStairs::FGroundSampler();
	// 同步用完即弃（`RebuildStairs` 一次调用之内），裸指针不会活过地面本身。
	const ACSGroundActor* G = Ground;
	return [G](const FVector2D& XY) { return G->SampleHeight(XY); };
}

void ACSStairsActor::GetTerraceConnections(TArray<FCSStairsTerraceLink>& OutLinks) const
{
	OutLinks.Reset();
	if (!bConnectTerraces || bConnectionsDisabled || IsActorBeingDestroyed() || !Spline || Spline->IsClosedLoop() || Spline->GetNumberOfSplinePoints() < 2) return;
	TArray<ACSHouseActor*> Houses;
	UCSHouseLibrary::GetHouses(GetWorld(), Houses);
	const int32 Last = Spline->GetNumberOfSplinePoints() - 1;
	for (int32 Point : { 0, Last })
	{
		const FVector P = Spline->GetLocationAtSplinePoint(Point, ESplineCoordinateSpace::World);
		const FVector N = Spline->GetLocationAtSplinePoint(Point == 0 ? 1 : Last - 1, ESplineCoordinateSpace::World);
		const float W = FMath::Max(Width * float(FMath::Abs(Spline->GetScaleAtSplinePoint(Point).Y)), 30.0f);
		FCSStairsTerraceLink Best;
		Best.PointIndex = Point;
		for (ACSHouseActor* House : Houses)
		{
			const FCSStairsTerraceConnection Candidate = CSStairs_ConnectTerrace(House->GetStairsTerraceSite(), P, N, W, TerraceSnapDistance, TerraceSnapHeight);
			if (!Candidate.bConnected || Candidate.DistanceSquared >= Best.Connection.DistanceSquared) continue;
			Best.Connection = Candidate;
			Best.House = House;
		}
		if (Best.House.IsValid()) OutLinks.Add(Best);
	}
}

int32 ACSStairsActor::GetTerraceConnectionCount() const
{
	FlushPendingReevaluate();
	TArray<FCSStairsTerraceLink> Links;
	GetTerraceConnections(Links);
	return Links.Num();
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
	TArray<FCSStairsTerraceLink> Connections;
	GetTerraceConnections(Connections);
	auto EndpointShift = [&](int32 Point)
	{
		for (const FCSStairsTerraceLink& Link : Connections) if (Link.PointIndex == Point) return Link.Connection.Position - Spline->GetLocationAtSplinePoint(Point, ESplineCoordinateSpace::World);
		return FVector::ZeroVector;
	};

	CSStairs::FRun Current;
	for (int32 Segment = 0; Segment < NumSegments; ++Segment)
	{
		const int32 I0 = Segment;
		const int32 I1 = (Segment + 1) % NumPoints;
		const float D0 = Spline->GetDistanceAlongSplineAtSplinePoint(I0);
		const float D1 = (bClosed && I1 == 0) ? Spline->GetSplineLength() : Spline->GetDistanceAlongSplineAtSplinePoint(I1);
		const FVector Shift0 = EndpointShift(I0), Shift1 = EndpointShift(I1);
		const FVector P0 = Spline->GetLocationAtSplinePoint(I0, ESplineCoordinateSpace::World) + Shift0;
		const FVector P1 = Spline->GetLocationAtSplinePoint(I1, ESplineCoordinateSpace::World) + Shift1;
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
			const FVector OnSpline = Spline->GetLocationAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World) + FMath::Lerp(Shift0, Shift1, double(T));
			FVector2D Dir = (FVector2D(Spline->GetDirectionAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World))
				+ FVector2D(Shift1 - Shift0) / FMath::Max(double(SegmentLength), 1.0)).GetSafeNormal();
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
	if (bInRebuild || bConnectionsDisabled || IsTemplate() || !GetWorld() || !BrickComponent) return;
	TGuardValue<bool> Guard(bInRebuild, true);
	bReevaluatePending = false;
	SetActorTickEnabled(false);
	SubscribeChanges();

	ResolveGroundAndSubscribe();
	SnapResizeHandles();
	TArray<FCSStairsTerraceLink> Connections;
	GetTerraceConnections(Connections);
	TArray<int32> ConnectionInput;
	for (const FCSStairsTerraceLink& Link : Connections)
	{
		ConnectionInput.Append({ int32(GetTypeHash(Link.House)), Link.PointIndex, Link.Connection.Opening.Edge,
			int32(FMath::RoundToInt(Link.Connection.Opening.Start * 100.0)), int32(FMath::RoundToInt(Link.Connection.Opening.End * 100.0)) });
	}
	const uint32 NewConnectionHash = FCrc::MemCrc32(ConnectionInput.GetData(), ConnectionInput.Num() * sizeof(int32));
	if (ConnectionHash != NewConnectionHash)
	{
		ConnectionHash = NewConnectionHash;
		NotifyTerraces();
	}

	TArray<CSStairs::FRun> Runs;
	BuildRuns(Runs);

	const CSStairs::FParams Params = MakeParams();
	const CSStairs::FGroundSampler GroundSampler = MakeGroundSampler();

	TArray<CSStairs::FBrick> Bricks;
	TArray<CSStairs::FBrick> Rails, Ladders;
	int32 Steps = 0;
	int32 LadderRuns = 0;
	int32 LadderRungs = 0;
	for (int32 RunIndex = 0; RunIndex < Runs.Num(); ++RunIndex)
	{
		// 梯子段（坡度 > 2.5）用木板装配边梁与横档，踏步砖和普通栏杆只处理缓坡段。
		if (Runs[RunIndex].Type == CSStairs::ESegmentType::Ladder)
		{
			++LadderRuns;
			LadderRungs += CSStairs::BuildLadder(Runs[RunIndex], Params, Ladders);
			continue;
		}
		// 种子按 run 分开：前一段多一级，不该把后面每一段的切砖全部重掷一遍。
		const uint32 RunSeed = HashCombine(GetTypeHash(Seed), GetTypeHash(RunIndex));
		const int32 FirstBrick = Bricks.Num();
		const int32 RunSteps = CSStairs::BuildRunBricks(Runs[RunIndex], Params, GroundSampler, RunSeed, Bricks);
		Steps += RunSteps;
		for (const FCSStairsTerraceLink& Link : Connections)
		{
			const auto& Samples = Runs[RunIndex].Samples;
			const int32 TerminalStep = Samples[0].Position.Equals(Link.Connection.Position, 0.05) ? 0
				: (Samples.Last().Position.Equals(Link.Connection.Position, 0.05) ? RunSteps - 1 : INDEX_NONE);
			if (TerminalStep == INDEX_NONE) continue;
			for (int32 I = FirstBrick; I < Bricks.Num(); ++I)
			{
				CSStairs::FBrick& B = Bricks[I];
				if (B.StepIndex != TerminalStep || !FMath::IsNearlyEqual(B.Center.Z + B.Size.Z * 0.5, Link.Connection.Position.Z, 0.05) || B.Size.Z <= 1.0) continue;
				// 接口逻辑齐平，砖顶压低 1 cm；伸进露台的部分由石板覆盖，避免两个顶面共面闪烁。
				B.Center.Z -= 0.5;
				B.Size.Z -= 1.0;
			}
		}
		CSStairs::BuildRunRails(Runs[RunIndex], Params, GroundSampler, Rails);
	}
	CurrentLadderRunCount = LadderRuns;
	CurrentLadderRungCount = WoodMesh ? LadderRungs : 0;
	const bool bWoodRailing = Railing == ECSStairsRailing::Wooden;
	UStaticMesh* RailMesh = bWoodRailing ? WoodMesh.Get() : BrickMesh.Get();
	CurrentRailingCount = RailMesh ? Rails.Num() : 0;
	CSStairsActor_UploadPieces(RailingComponent, RailMesh, bWoodRailing ? WoodMaterial.Get() : BrickMaterial.Get(), Rails, RailingHash);
	CSStairsActor_UploadPieces(LadderComponent, WoodMesh, WoodMaterial, Ladders, LadderHash);

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
	for (const FVector& V : { ComponentTransform.GetLocation(), ComponentTransform.GetRotation().Euler(), ComponentTransform.GetScale3D() }) HashInput.Append({ Q(V.X), Q(V.Y), Q(V.Z) });
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
	OutFamilies.Add({ RailingComponent, TEXT("楼梯栏杆"), TEXT("StairRailing"), CurrentRailingCount > 0 });
	OutFamilies.Add({ LadderComponent, TEXT("木梯"), TEXT("StairLadders"), CurrentLadderRungCount > 0 });
}

void ACSStairsActor::ResolveGroundAndSubscribe()
{
	if (!bFollowGround)
	{
		UnsubscribeGround();
		return;
	}
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
	RequestReevaluate();
}

void ACSStairsActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bConnectionsDisabled = true;
	ExitResizeMode();
	UnsubscribeChanges();
	UnsubscribeGround();
	NotifyTerraces();
	Super::EndPlay(EndPlayReason);
}

void ACSStairsActor::Destroyed()
{
	bConnectionsDisabled = true;
	ExitResizeMode();
	UnsubscribeChanges();
	UnsubscribeGround();
	NotifyTerraces();
	Super::Destroyed();
}

void ACSStairsActor::RequestReevaluate()
{
	if (bConnectionsDisabled || IsTemplate() || !GetWorld()) return;
	bReevaluatePending = true;
	SetActorTickEnabled(true);
#if WITH_EDITOR
	// LEVELTICK_TimeOnly / Slate 拖动节流可能完全跳过 actor tick。Core ticker 只挂一次，
	// 在完整的样条编辑操作结束后消费通知，兼容非实时视口及无头编辑器。
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

void ACSStairsActor::FlushPendingReevaluate() const
{
	if (bReevaluatePending && !bInRebuild) const_cast<ACSStairsActor*>(this)->RebuildStairs();
}

void ACSStairsActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	FlushPendingReevaluate();
	SetActorTickEnabled(bReevaluatePending);
}

void ACSStairsActor::SubscribeChanges()
{
	if (!TerraceChangedHandle.IsValid()) TerraceChangedHandle = ACSHouseActor::OnTerraceChanged.AddUObject(this, &ACSStairsActor::HandleTerraceChanged);
	if (USceneComponent* Root = GetRootComponent(); Root && !RootTransformChangedHandle.IsValid()) RootTransformChangedHandle = Root->TransformUpdated.AddUObject(this, &ACSStairsActor::HandleComponentTransformChanged);
	// 蓝图重实例化/撤销可能替换组件；从旧对象解绑后再订阅当前对象，不能只检查句柄有效。
	if (ObservedSpline.Get() == Spline.Get()) return;
	UnsubscribeSplineChanges();
	ObservedSpline = Spline;
	if (!Spline) return;
	// UE 5.7 原生通知：Updated 覆盖可视化器直接改曲线后 UpdateSpline；Changed 也覆盖组件独立撤销。
	// 只入队，不能在 setter 尚未结束或组件重建途中同步重建楼梯。
PRAGMA_DISABLE_EXPERIMENTAL_WARNINGS
	SplineChangedHandle = Spline->GetOnSplineChanged().AddUObject(this, &ACSStairsActor::RequestReevaluate);
	SplineUpdatedHandle = Spline->GetOnSplineUpdated().AddUObject(this, &ACSStairsActor::RequestReevaluate);
PRAGMA_ENABLE_EXPERIMENTAL_WARNINGS
	SplineTransformChangedHandle = Spline->TransformUpdated.AddUObject(this, &ACSStairsActor::HandleComponentTransformChanged);
}

void ACSStairsActor::UnsubscribeChanges()
{
#if WITH_EDITOR
	FTSTicker::RemoveTicker(EditorRebuildTicker);
	EditorRebuildTicker.Reset();
#endif
	UnsubscribeSplineChanges();
	ACSHouseActor::OnTerraceChanged.Remove(TerraceChangedHandle);
	TerraceChangedHandle.Reset();
	if (USceneComponent* Root = GetRootComponent(); Root && RootTransformChangedHandle.IsValid()) Root->TransformUpdated.Remove(RootTransformChangedHandle);
	RootTransformChangedHandle.Reset();
}

void ACSStairsActor::UnsubscribeSplineChanges()
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

void ACSStairsActor::NotifyTerraces() const
{
	TArray<ACSHouseActor*> Houses;
	UCSHouseLibrary::GetHouses(GetWorld(), Houses);
	for (ACSHouseActor* House : Houses) if (!House->IsActorBeingDestroyed()) House->RequestReevaluate();
}

void ACSStairsActor::HandleTerraceChanged(ACSHouseActor* House)
{
	if (House && House->GetWorld() == GetWorld()) RequestReevaluate();
}

void ACSStairsActor::HandleComponentTransformChanged(USceneComponent* Component, EUpdateTransformFlags Flags, ETeleportType Teleport)
{
	RequestReevaluate();
}

void ACSStairsActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	if (IsTemplate() || !GetWorld()) return;
	bConnectionsDisabled = false;
	SubscribeChanges();
	RequestReevaluate();
}

#if WITH_EDITOR
void ACSStairsActor::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	// Spline visualizer 每次控制点/切线拖动都会发 PostEditMove(false)。原生类及关闭
	// Run Construction Script on Drag 的蓝图也要跟手，不能只依赖 OnConstruction。
	RequestReevaluate();
	if (bFinished) FlushPendingReevaluate();
}

void ACSStairsActor::PostEditUndo()
{
	Super::PostEditUndo();
	bConnectionsDisabled = false;
	RebuildStairs();
	NotifyTerraces();
}
#endif
