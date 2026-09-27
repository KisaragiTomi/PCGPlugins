#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSGpuInstancedMeshComponent.h"
#include "CSGroundActor.h"
#include "CSHouseActor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "RenderingThread.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Package.h"

// -----------------------------------------------------------------------------
// 建筑周边灌木（第七条派生链，照 TG `_garden_spawn_bushes_buildings.cs`）
//
// 走真路径：房子自己把外皮推给地面（`PublishFootprintToGround`），地面在 GPU 上撒，回读断言。
// 判据都是**几何的**：每一株都落在环带里、不在屋里、不在路上；花与本体同一个变换且只是子集；
// 同样的输入重散出同一批（按排序比，槽位顺序由 GPU 决定）；删房子灌木归零。
// -----------------------------------------------------------------------------

namespace
{
/** 点到凸多边形（逆时针）外皮的有符号距离：外正内负。与 kernel 的 `CSCover_BuildingDistance` 同一口径。 */
double CSBushTest_SignedDistance(const TArray<FVector2D>& Polygon, const FVector2D& P)
{
	bool bInside = true;
	double Plane = -1e30;
	double Edge = 1e30;
	for (int32 I = 0; I < Polygon.Num(); ++I)
	{
		const FVector2D A = Polygon[I];
		const FVector2D E = Polygon[(I + 1) % Polygon.Num()] - A;
		const double L2 = E.SizeSquared();
		if (L2 < 1e-4) continue;
		const double S = FVector2D::DotProduct(P - A, FVector2D(E.Y, -E.X)) / FMath::Sqrt(L2);
		bInside = bInside && S <= 0.0;
		Plane = FMath::Max(Plane, S);
		const double T = FMath::Clamp(FVector2D::DotProduct(P - A, E) / L2, 0.0, 1.0);
		Edge = FMath::Min(Edge, FVector2D::Distance(P, A + E * T));
	}
	return bInside ? Plane : Edge;
}

TArray<FVector2D> CSBushTest_WorldFootprint(const ACSHouseActor* House)
{
	TArray<FVector2D> Out;
	// 与 `ACSHouseActor::GetBuildTransform` 同一个定义（它是私有的）：只取 yaw 与位置。
	const FTransform World(FRotator(0.0, House->GetActorRotation().Yaw, 0.0), House->GetActorLocation());
	for (const FVector2D& V : House->GetFootprint().Verts) Out.Add(FVector2D(World.TransformPosition(FVector(V, 0.0))));
	return Out;
}

TArray<FVector> CSBushTest_Sorted(TArray<FVector> Points)
{
	Points.Sort([](const FVector& L, const FVector& R) { return L.X != R.X ? L.X < R.X : L.Y < R.Y; });
	return Points;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundBuildingBushRingTest,
	"PCGPlugins.ComputeShaderGenerator.Ground.BuildingBushesRing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGroundBuildingBushRingTest::RunTest(const FString& Parameters)
{
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (!TestNotNull(TEXT("Engine cube"), Cube) || !TestNotNull(TEXT("Engine sphere"), Sphere)) return false;

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	// 默认 64×64 格 × 50 cm：从原点铺到 (3200, 3200)。草关掉，本用例只看灌木。
	ACSGroundActor* Ground = World->SpawnActor<ACSGroundActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Ground actor"), Ground)) return false;
	Ground->StairMesh = nullptr;
	Ground->bGroundCoverEnabled = false;
	FCSGroundBuildingBushes& B = Ground->BuildingBushes;
	B.Mesh = Cube;
	B.FlowerMesh = Sphere;
	B.FlowerChance = 0.3f;
	B.DensityPerSqM = 4.0f;   // 比默认密：统计性的断言要样本
	B.RingInner = 30.0f;
	B.RingWidth = 300.0f;
	Ground->RebuildGroundMesh();

	// ① 没有房子：一株都不长（灌木只长在房子周围）。
	TArray<FVector> Body;
	TestEqual(TEXT("没有房子：零灌木"), Ground->DebugReadBuildingBushOriginsSync(0, Body), 0);

	// ② 房子落在地面正中：它自己把外皮推给地面。
	ACSHouseActor* House = World->SpawnActor<ACSHouseActor>(FVector(1600.0, 1600.0, 0.0), FRotator(0.0, 25.0, 0.0));
	if (!TestNotNull(TEXT("House actor"), House)) return false;
	House->FlushPendingReevaluate();
	TestEqual(TEXT("房子登记了外皮"), Ground->GetBuildingFootprintCount(), 1);
	const TArray<FVector2D> Footprint = CSBushTest_WorldFootprint(House);

	const int32 BodyCount = Ground->DebugReadBuildingBushOriginsSync(0, Body);
	AddInfo(FString::Printf(TEXT("灌木本体 %d 株"), BodyCount));
	if (!TestTrue(TEXT("房子周围长出了灌木"), BodyCount > 20)) return false;

	int32 OutOfRing = 0;
	double MinD = 1e30, MaxD = -1e30;
	for (const FVector& P : Body)
	{
		const double D = CSBushTest_SignedDistance(Footprint, FVector2D(P));
		MinD = FMath::Min(MinD, D);
		MaxD = FMath::Max(MaxD, D);
		if (D <= B.RingInner - 1.0 || D >= B.RingInner + B.RingWidth + 1.0) ++OutOfRing;
	}
	AddInfo(FString::Printf(TEXT("离外皮距离 [%.1f, %.1f] cm（环带 [%.0f, %.0f]）"), MinD, MaxD, B.RingInner, B.RingInner + B.RingWidth));
	TestEqual(TEXT("每一株都在环带里（不在屋里、不跑出环带）"), OutOfRing, 0);

	// ③ 花与本体同一个变换，而且只是一部分。
	TArray<FVector> Flowers;
	const int32 FlowerCount = Ground->DebugReadBuildingBushOriginsSync(1, Flowers);
	int32 Orphans = 0;
	for (const FVector& F : Flowers)
	{
		const bool bOnBush = Body.ContainsByPredicate([&F](const FVector& P) { return P.Equals(F, 0.01); });
		if (!bOnBush) ++Orphans;
	}
	const double Ratio = double(FlowerCount) / double(FMath::Max(BodyCount, 1));
	AddInfo(FString::Printf(TEXT("开花 %d / %d = %.2f（期望约 %.2f）"), FlowerCount, BodyCount, Ratio, B.FlowerChance));
	TestEqual(TEXT("每一朵花都落在某棵灌木的原点上"), Orphans, 0);
	TestTrue(TEXT("开花比例接近 FlowerChance"), Ratio > 0.15 && Ratio < 0.45);

	// ④ 同样的输入重散出同一批：摘掉再登记回去（真的重跑一趟 GPU）。
	Ground->RemoveBuildingFootprint(House);
	TArray<FVector> Empty;
	TestEqual(TEXT("摘掉外皮：灌木归零"), Ground->DebugReadBuildingBushOriginsSync(0, Empty), 0);
	House->RequestReevaluate();
	House->FlushPendingReevaluate();
	TArray<FVector> Again;
	Ground->DebugReadBuildingBushOriginsSync(0, Again);
	const TArray<FVector> SortedA = CSBushTest_Sorted(Body);
	const TArray<FVector> SortedB = CSBushTest_Sorted(Again);
	bool bSame = SortedA.Num() == SortedB.Num();
	for (int32 I = 0; bSame && I < SortedA.Num(); ++I) bSame = SortedA[I].Equals(SortedB[I], 0.01);
	TestTrue(TEXT("重散出同一批灌木（位置只由格身份决定）"), bSame);

	// ⑤ 路上不长：画一条横穿环带的路。
	Ground->BeginPaintStroke();
	for (int32 Dab = 0; Dab <= 24; ++Dab)
	{
		const double X = FMath::Lerp(600.0, 2600.0, double(Dab) / 24.0);
		Ground->ApplyPaintStroke(FVector(X, 1600.0, Ground->SampleHeight(FVector2D(X, 1600.0))));
	}
	Ground->EndPaintStroke();
	TArray<FVector> WithRoad;
	Ground->DebugReadBuildingBushOriginsSync(0, WithRoad);
	int32 OnRoad = 0;
	for (const FVector& P : WithRoad) if (Ground->SampleRoadWeight(FVector2D(P)) >= B.RoadMaskEnd + 0.01f) ++OnRoad;
	AddInfo(FString::Printf(TEXT("画路后 %d 株（画路前 %d）"), WithRoad.Num(), BodyCount));
	TestEqual(TEXT("路上一株都不长"), OnRoad, 0);
	TestTrue(TEXT("路真的吃掉了一部分"), WithRoad.Num() < BodyCount);

	// ⑥ 删房子：周围的灌木跟着消失。
	House->Destroy();
	TArray<FVector> Gone;
	TestEqual(TEXT("删房子：外皮注销"), Ground->GetBuildingFootprintCount(), 0);
	TestEqual(TEXT("删房子：灌木归零"), Ground->DebugReadBuildingBushOriginsSync(0, Gone), 0);
	return true;
}

// -----------------------------------------------------------------------------
// Nanite：灌木资产开了 Nanite 时，实例组件自动交给引擎的 Nanite 管线画（没有开关）。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundBuildingBushNaniteTest,
	"PCGPlugins.ComputeShaderGenerator.Ground.BuildingBushesUseNanite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGroundBuildingBushNaniteTest::RunTest(const FString& Parameters)
{
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Engine cube"), Cube)) return false;
	// 复制一份开 Nanite 同步构建（同 CSSW 那条的做法），不赌引擎 Cube 自己的设置。
	UStaticMesh* NaniteCube = DuplicateObject<UStaticMesh>(Cube, GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), TEXT("SM_CSBushNaniteCube")));
	FMeshNaniteSettings Settings = NaniteCube->GetNaniteSettings();
	Settings.bEnabled = true;
	NaniteCube->SetNaniteSettings(Settings);
	TArray<FText> Errors;
	NaniteCube->Build(/*bInSilent*/ true, &Errors);

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;
	ACSGroundActor* Ground = World->SpawnActor<ACSGroundActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Ground actor"), Ground)) return false;
	Ground->StairMesh = nullptr;
	Ground->bGroundCoverEnabled = false;
	Ground->BuildingBushes.Mesh = NaniteCube;
	Ground->BuildingBushes.FlowerMesh = nullptr;
	Ground->RebuildGroundMesh();
	ACSHouseActor* House = World->SpawnActor<ACSHouseActor>(FVector(1600.0, 1600.0, 0.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("House actor"), House)) return false;
	House->FlushPendingReevaluate();

	UCSGpuInstancedMeshComponent* Component = Ground->GetBuildingBushComponent(0);
	if (!TestNotNull(TEXT("灌木组件"), Component)) return false;
	if (!NaniteCube->IsNaniteEnabled())
	{
		AddWarning(TEXT("这台机器 / 这个 RHI 上 Nanite 构建没开起来，Nanite 路验不了（跳过，不算通过）。"));
		return true;
	}
	TestTrue(TEXT("开了 Nanite 的灌木资产走 Nanite 路"), Component->IsNaniteRenderPath());
	TArray<FVector> Body;
	TestTrue(TEXT("Nanite 路下照样长出了灌木（实例源同一份）"), Ground->DebugReadBuildingBushOriginsSync(0, Body) > 0);
	return true;
}

// -----------------------------------------------------------------------------
// 开销：512 m 地面整块重散一次（CPU 录图 + GPU 执行 + 刷新往返，扣掉空刷新的底噪）。
// 不是严格门槛，只挡病态回归；数字写进日志供对照。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundBuildingBushCostTest,
	"PCGPlugins.ComputeShaderGenerator.Ground.BuildingBushesCost512m",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGroundBuildingBushCostTest::RunTest(const FString& Parameters)
{
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Engine cube"), Cube)) return false;
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	ACSGroundActor* Ground = World->SpawnActor<ACSGroundActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Ground actor"), Ground)) return false;
	Ground->StairMesh = nullptr;
	Ground->bGroundCoverEnabled = false;
	Ground->NumCellsX = 1024;   // 512 m
	Ground->NumCellsY = 1024;
	Ground->BuildingBushes.Mesh = Cube;
	Ground->RebuildGroundMesh();
	FlushRenderingCommands();

	// 20 栋 8×6 m 的房子撒在地面上（直接登记外皮，不走房子，量的是灌木这一条）。
	TArray<UObject*> Keys;
	auto RectAt = [](const FVector2D& C) { return TArray<FVector2D>{ C + FVector2D(-400, -300), C + FVector2D(400, -300), C + FVector2D(400, 300), C + FVector2D(-400, 300) }; };
	for (int32 I = 0; I < 20; ++I)
	{
		UObject* Key = NewObject<UStaticMesh>(GetTransientPackage());
		Keys.Add(Key);
		Ground->SetBuildingFootprint(Key, RectAt(FVector2D(3000.0 + 2400.0 * (I % 5), 3000.0 + 2400.0 * (I / 5))));
	}
	FlushRenderingCommands();

	constexpr int32 Runs = 10;
	double Flush = 0.0;
	for (int32 Run = 0; Run < Runs; ++Run)
	{
		const double T0 = FPlatformTime::Seconds();
		FlushRenderingCommands();
		Flush += FPlatformTime::Seconds() - T0;
	}
	double Total = 0.0;
	for (int32 Run = 0; Run < Runs; ++Run)
	{
		// 每一趟挪一栋房子一点点：输入哈希必然变，保证真的重散。
		const double T0 = FPlatformTime::Seconds();
		Ground->SetBuildingFootprint(Keys[0], RectAt(FVector2D(3000.0 + 10.0 * (Run + 1), 3000.0)));
		FlushRenderingCommands();
		Total += FPlatformTime::Seconds() - T0;
	}
	TArray<FVector> Body;
	const int32 Count = Ground->DebugReadBuildingBushOriginsSync(0, Body);
	const double PerRunMs = (Total - Flush) / Runs * 1000.0;
	AddInfo(FString::Printf(TEXT("512 m 地面、20 栋房子：整块重散一次 %.3f ms（含录图 + GPU；已扣空刷新 %.3f ms），长出 %d 株"),
		PerRunMs, Flush / Runs * 1000.0, Count));
	TestTrue(TEXT("长出了灌木"), Count > 0);
	TestTrue(TEXT("整块重散没有病态地慢（< 20 ms）"), PerRunMs < 20.0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
