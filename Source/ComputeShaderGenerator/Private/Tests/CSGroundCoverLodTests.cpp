#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSGroundActor.h"
#include "CSGpuInstancedMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "RenderingThread.h"
#include "Tests/AutomationEditorCommon.h"

// -----------------------------------------------------------------------------
// 地被的近 / 远两档（2026-09-22：近处单根草只撒相机周围一块窗口，远处组合草整张地面撒一次）
//
// 这一条上**只有三件事**是回归时真会坏的，也只钉这三件：
//  ① 组件表的形状：物种自己的下标不变，远处条目追加在**全部物种之后**（脚本按下标 0 = 草）；
//  ② 近处只落在窗口里、远处铺满整张地面；两个组件的距离带首尾相接（不然交界要么空一圈要么叠一圈）；
//  ③ **窗口挪动时同一格的草一株不变** —— 随机源用的是整张网格里的格号而不是窗口内下标。
//     写错的症状是相机一走，整片草地跟着"翻滚"，而数量、日志、断言全都正常。
//
// 密度取 1 株/m²（格距 1 m）：窗口 141² 格，够钉住上面三条，又不用为了跑个用例去散 100 万株。
// -----------------------------------------------------------------------------

namespace
{
	/** 按基础网格认组件：近处用 Cube、远处用 Cylinder，两者因此分得开。 */
	UCSGpuInstancedMeshComponent* FindCoverComponent(const ACSGroundActor* Ground, const UStaticMesh* Mesh)
	{
		TInlineComponentArray<UCSGpuInstancedMeshComponent*> Components;
		Ground->GetComponents(Components);
		for (UCSGpuInstancedMeshComponent* One : Components)
		{
			if (One->BaseMesh == Mesh) return One;
		}
		return nullptr;
	}

	FBox2D BoundsOf(const TArray<FVector>& Origins)
	{
		FBox2D Box(ForceInit);
		for (const FVector& One : Origins) Box += FVector2D(One.X, One.Y);
		return Box;
	}

	/** 落点的定位键：格距 1 m，量化到 0.1 mm 足以区分不同格、又吸掉浮点噪声。 */
	FIntVector2 KeyOf(const FVector& P)
	{
		return FIntVector2(FMath::RoundToInt(P.X * 100.0), FMath::RoundToInt(P.Y * 100.0));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundCoverNearFarLayoutTest,
	"PCGPlugins.ComputeShaderGenerator.Ground.CoverNearFarLayout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGroundCoverNearFarLayoutTest::RunTest(const FString& Parameters)
{
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (!TestNotNull(TEXT("Engine cube"), Cube)) return false;
	if (!TestNotNull(TEXT("Engine cylinder"), Cylinder)) return false;

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	ACSGroundActor* Ground = World->SpawnActor<ACSGroundActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Ground actor"), Ground)) return false;
	Ground->StairMesh = nullptr;
	Ground->NumCellsX = 1024;   // 512 m 见方：窗口（140 m）要真的只是它的一小块
	Ground->NumCellsY = 1024;
	Ground->Flowers.Empty();
	Ground->bGroundCoverEnabled = true;
	Ground->Grass.Mesh = Cube;
	Ground->Grass.DensityPerSqM = 1.0f;          // 格距 1 m
	Ground->Grass.MaxInstances = 1048576;
	Ground->Grass.LOD.bEnabled = true;
	Ground->Grass.LOD.FarMesh = Cylinder;
	Ground->Grass.LOD.FarDensityPerSqM = 0.05f;  // 512 m 地面上约 1.3 万簇
	Ground->Grass.LOD.NearDistance = 6000.0f;
	Ground->Grass.LOD.FadeDistance = 1500.0f;
	Ground->Grass.LOD.RescatterDistance = 1000.0f;
	Ground->RebuildGroundMesh();

	// 无头进程没有视口（窗口平时跟的是"上一帧画过的透视视口"），所以显式钉一个中心。
	const FVector2D ViewA(20000.0, 20000.0);
	Ground->SetGroundCoverViewOverride(FVector(ViewA, 0.0));
	FlushRenderingCommands();

	// ① 组件表的形状
	if (!TestEqual(TEXT("两档 = 近 + 远两条"), Ground->GetGroundCoverSpeciesCount(), 2)) return false;
	TestEqual(TEXT("草仍是下标 0，远处那条追加在后面"), Ground->GetGroundCoverFarIndex(0), 1);

	TArray<FVector> NearA, Far;
	const int32 NearCount = Ground->DebugReadGroundCoverOriginsSync(0, NearA);
	const int32 FarCount = Ground->DebugReadGroundCoverOriginsSync(1, Far);
	AddInfo(FString::Printf(TEXT("近处 %d 株 / 远处 %d 簇"), NearCount, FarCount));

	// ② 近处只在窗口里；远处铺满整张地面
	const int32 HalfCells = FMath::CeilToInt32((6000.0f + 1000.0f) / 100.0f);   // 半宽 70 格
	TestEqual(TEXT("近处撒满整块窗口（平地未画路，一格一株）"), NearCount, FMath::Square(2 * HalfCells + 1));
	const FBox2D NearBox = BoundsOf(NearA);
	const FBox2D FarBox = BoundsOf(Far);
	AddInfo(FString::Printf(TEXT("近处窗口 %.0f×%.0f m @ (%.0f, %.0f)，远处 %.0f×%.0f m"),
		NearBox.GetSize().X / 100.0, NearBox.GetSize().Y / 100.0,
		NearBox.GetCenter().X / 100.0, NearBox.GetCenter().Y / 100.0,
		FarBox.GetSize().X / 100.0, FarBox.GetSize().Y / 100.0));
	TestTrue(TEXT("近处窗口约 140 m 见方（不是整张地面）"), NearBox.GetSize().X < 15000.0 && NearBox.GetSize().Y < 15000.0);
	TestTrue(TEXT("近处窗口以钉住的机位为心"), FVector2D::Distance(NearBox.GetCenter(), ViewA) < 200.0);
	TestTrue(TEXT("远处铺满整张地面"), FarBox.GetSize().X > 45000.0 && FarBox.GetSize().Y > 45000.0);
	TestTrue(TEXT("远处的数量按远处密度算（约 1.3 万簇）"), FarCount > 10000 && FarCount < 16000);

	// 距离带首尾相接：近处画到边界、远处从边界起画，共用一条过渡带
	UCSGpuInstancedMeshComponent* NearComp = FindCoverComponent(Ground, Cube);
	UCSGpuInstancedMeshComponent* FarComp = FindCoverComponent(Ground, Cylinder);
	if (!TestNotNull(TEXT("近处组件"), NearComp)) return false;
	if (!TestNotNull(TEXT("远处组件"), FarComp)) return false;
	AddInfo(FString::Printf(TEXT("近处 [0, %.0f] 远处 [%.0f, ∞)，过渡带 %.0f / %.0f cm"),
		NearComp->InstanceEndCullDistance, FarComp->InstanceStartCullDistance,
		NearComp->InstanceCullFadeDistance, FarComp->InstanceCullFadeDistance));
	TestTrue(TEXT("近处的外边界 = 远处的内边界"),
		FMath::IsNearlyEqual(NearComp->InstanceEndCullDistance, FarComp->InstanceStartCullDistance, 0.01f));
	TestTrue(TEXT("边界就在 NearDistance 上（窗口没被预算收小）"),
		NearComp->InstanceEndCullDistance > 5400.0f && NearComp->InstanceEndCullDistance <= 6000.0f);
	TestTrue(TEXT("两个组件共用同一条过渡带"),
		FMath::IsNearlyEqual(NearComp->InstanceCullFadeDistance, FarComp->InstanceCullFadeDistance, 0.01f)
		&& NearComp->InstanceCullFadeDistance > 0.0f);
	TestEqual(TEXT("远处没有外边界（画到天边）"), FarComp->InstanceEndCullDistance, 0.0f);
	TestEqual(TEXT("近处没有内边界（贴着相机也画）"), NearComp->InstanceStartCullDistance, 0.0f);

	// ③ 窗口搬过去：数量不变、重叠区里**同一格的落点逐位不变**、远处一株不动
	const FVector2D ViewB = ViewA + FVector2D(5000.0, 0.0);
	Ground->SetGroundCoverViewOverride(FVector(ViewB, 0.0));
	FlushRenderingCommands();
	TArray<FVector> NearB, FarB;
	const int32 NearCountB = Ground->DebugReadGroundCoverOriginsSync(0, NearB);
	const int32 FarCountB = Ground->DebugReadGroundCoverOriginsSync(1, FarB);
	TestEqual(TEXT("窗口搬家不改变容量与株数"), NearCountB, NearCount);
	TestEqual(TEXT("远处与相机无关（整张地面撒一次）"), FarCountB, FarCount);
	TestTrue(TEXT("窗口真的搬过去了"),
		FVector2D::Distance(BoundsOf(NearB).GetCenter(), ViewB) < 200.0);

	TSet<FIntVector2> Before;
	Before.Reserve(NearA.Num());
	for (const FVector& One : NearA) Before.Add(KeyOf(One));
	const FBox2D Overlap = BoundsOf(NearA).Overlap(BoundsOf(NearB));
	int32 Shared = 0;
	int32 InOverlap = 0;
	for (const FVector& One : NearB)
	{
		if (!Overlap.IsInside(FVector2D(One.X, One.Y))) continue;
		++InOverlap;
		if (Before.Contains(KeyOf(One))) ++Shared;
	}
	AddInfo(FString::Printf(TEXT("重叠区 %d 株，其中落点逐位不变的 %d 株"), InOverlap, Shared));
	TestTrue(TEXT("重叠区够大（窗口只挪了 50 m）"), InOverlap > NearCount / 3);
	// 随机源若误用窗口内下标，这一条会塌到 0 附近 —— 而数量、日志、别的断言全都照绿。
	TestEqual(TEXT("同一格的草在窗口搬家后一株没变"), Shared, InOverlap);

	// ④ 关掉两档：退回整张地面一档，距离带清零（否则近处组件会带着上一轮的外边界，远景全秃）
	Ground->Grass.LOD.bEnabled = false;
	Ground->RebuildGroundCover();
	FlushRenderingCommands();
	TestEqual(TEXT("关掉两档只剩一条"), Ground->GetGroundCoverSpeciesCount(), 1);
	TestEqual(TEXT("关掉两档后草没有远处条目"), Ground->GetGroundCoverFarIndex(0), int32(INDEX_NONE));
	UCSGpuInstancedMeshComponent* PlainComp = FindCoverComponent(Ground, Cube);
	if (!TestNotNull(TEXT("一档的组件"), PlainComp)) return false;
	TestEqual(TEXT("一档不带外边界"), PlainComp->InstanceEndCullDistance, 0.0f);
	TestEqual(TEXT("一档不带内边界"), PlainComp->InstanceStartCullDistance, 0.0f);
	TestEqual(TEXT("一档不带过渡带"), PlainComp->InstanceCullFadeDistance, 0.0f);
	TArray<FVector> Plain;
	const int32 PlainCount = Ground->DebugReadGroundCoverOriginsSync(0, Plain);
	AddInfo(FString::Printf(TEXT("一档：%d 株，铺 %.0f×%.0f m"), PlainCount,
		BoundsOf(Plain).GetSize().X / 100.0, BoundsOf(Plain).GetSize().Y / 100.0));
	TestTrue(TEXT("一档铺满整张地面"), BoundsOf(Plain).GetSize().X > 45000.0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
