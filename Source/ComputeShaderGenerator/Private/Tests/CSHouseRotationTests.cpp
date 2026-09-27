#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSHouseActor.h"
#include "CSMesh.h"
#include "Engine/World.h"
#include "RenderingThread.h"
#include "Tests/AutomationEditorCommon.h"

// -----------------------------------------------------------------------------
// 房子绕 Z 自由旋转
//
// 房体是世界空间常驻流 + 绝对变换的渲染组件：actor 转了，几何不会自己跟着转，全靠摆位快路径
// （拖动帧，`ApplyMeshSlotPlacement` 的变换 pass）与松手全量重建把 yaw 烘进去。两条路都要验。
//
// ⚠️ 必须等异步编辑落地再量：摆位与重建都走 `EditMeshAsync`，录完图就返回，包围盒在完成回调
// 之前还是旧值 —— 不 Settle 直接读，量到的是"没转"（09-21 无头探针就踩过这一下）。
// ⚠️ `GetWorldBoundsApprox` 是算子维护的**保守**包围盒：摆位快路径把旧盒的角点变换过去，连续
// 转几次会越转越胖，所以拖动帧只断言"包得住理论值、中心对"；松手全量重建按真实几何重算，断言相等。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseFreeYawTest,
	"PCGPlugins.ComputeShaderGenerator.House.RotatesFreelyAroundZ",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSHouseFreeYawTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	const FVector Origin(500.0, -300.0, 0.0);
	ACSHouseActor* House = World->SpawnActor<ACSHouseActor>(Origin, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("House actor"), House)) return false;
	UCSMesh* Body = House->GetTinyGladeMesh();
	if (!TestNotNull(TEXT("房体网格"), Body)) return false;

	auto Settle = [House, Body]()
	{
		House->FlushPendingReevaluate();
		for (int32 Pump = 0; Pump < 16 && Body->IsEditInFlight(); ++Pump) FlushRenderingCommands();
		return !Body->IsEditInFlight();
	};
	if (!TestTrue(TEXT("初次建体已落地"), Settle())) return false;

	// yaw = 0 时房体 XY 包围盒就是它的外廓矩形（墙 + 檐口都在矩形内），转过 θ 之后的包围盒
	// 半宽应当是 (ex|cos| + ey|sin|, ex|sin| + ey|cos|)，中心绕 actor 原点转过去。
	const FBox B0 = Body->GetWorldBoundsApprox();
	if (!TestTrue(TEXT("房体有包围盒"), B0.IsValid != 0)) return false;
	const FVector E0 = B0.GetExtent();
	const FVector2D C0 = FVector2D(B0.GetCenter()) - FVector2D(Origin);
	AddInfo(FString::Printf(TEXT("yaw 0：半宽 (%.1f, %.1f, %.1f)，中心偏移 (%.1f, %.1f)"), E0.X, E0.Y, E0.Z, C0.X, C0.Y));

	auto ExpectRotated = [this, Body, &Origin, E0, C0](const TCHAR* Stage, double YawDeg, bool bExact)
	{
		const double R = FMath::DegreesToRadians(YawDeg);
		const double C = FMath::Abs(FMath::Cos(R)), S = FMath::Abs(FMath::Sin(R));
		const FVector2D WantExtent(E0.X * C + E0.Y * S, E0.X * S + E0.Y * C);
		const FVector2D WantCenter = FVector2D(Origin) + C0.GetRotated(YawDeg);
		const FBox B = Body->GetWorldBoundsApprox();
		const FVector2D GotExtent(B.GetExtent());
		const FVector2D GotCenter(B.GetCenter());
		const bool bExtentOk = bExact
			? GotExtent.Equals(WantExtent, 5.0)
			: GotExtent.X >= WantExtent.X - 5.0 && GotExtent.Y >= WantExtent.Y - 5.0;
		const FString Label = FString::Printf(TEXT("%s yaw=%.0f：半宽 (%.1f, %.1f) %s (%.1f, %.1f)，中心 (%.1f, %.1f) 期望 (%.1f, %.1f)"),
			Stage, YawDeg, GotExtent.X, GotExtent.Y, bExact ? TEXT("期望") : TEXT("应包住"), WantExtent.X, WantExtent.Y,
			GotCenter.X, GotCenter.Y, WantCenter.X, WantCenter.Y);
		TestTrue(Label, bExtentOk && GotCenter.Equals(WantCenter, 5.0));
	};

	for (const double Yaw : { 37.0, 123.0, -71.0 })
	{
		House->SetActorRotation(FRotator(0.0, Yaw, 0.0));
		// 拖动帧：摆位快路径（只把已有几何变换过去）。
		House->PostEditMove(/*bFinished*/ false);
		if (!TestTrue(TEXT("拖动帧的摆位已落地"), Settle())) return false;
		ExpectRotated(TEXT("拖动中"), Yaw, /*bExact*/ false);
		// 松手：全量重建，yaw 由 GetBuildTransform 直接烘进几何。
		House->PostEditMove(/*bFinished*/ true);
		if (!TestTrue(TEXT("松手重建已落地"), Settle())) return false;
		ExpectRotated(TEXT("松手后"), Yaw, /*bExact*/ true);
		TestEqual(TEXT("旋转没被改回"), double(House->GetActorRotation().Yaw), Yaw, 0.01);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
