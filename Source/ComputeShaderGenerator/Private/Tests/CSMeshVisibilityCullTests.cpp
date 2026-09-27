#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSGpuMeshTypes.h"
#include "CSMesh.h"
#include "CSMeshOps.h"
#include "CSMeshVisibilityCull.h"

#include "Engine/World.h"
#include "Tests/AutomationEditorCommon.h"

namespace
{
	/** 轴对齐立方体，每个面 4 个自己的顶点（面与面不共享顶点）、两个三角形。共边的角点逐位相同，光栅化不漏缝。 */
	void CSMeshVisibilityTest_AddBox(FCSGpuMeshCPUData& Mesh, const FVector3f& Center, float HalfSize, int32 MaterialSlot)
	{
		static const FVector3f Normals[6] = {
			FVector3f(1, 0, 0), FVector3f(-1, 0, 0), FVector3f(0, 1, 0), FVector3f(0, -1, 0), FVector3f(0, 0, 1), FVector3f(0, 0, -1) };
		static const FVector2f Corners[4] = { FVector2f(-1, -1), FVector2f(1, -1), FVector2f(1, 1), FVector2f(-1, 1) };
		for (const FVector3f& Normal : Normals)
		{
			const FVector3f U = FMath::Abs(Normal.Z) > 0.5f ? FVector3f(1, 0, 0) : FVector3f(0, 0, 1);
			const FVector3f V = FVector3f::CrossProduct(Normal, U);
			const uint32 Base = uint32(Mesh.Positions.Num());
			for (const FVector2f& Corner : Corners)
			{
				Mesh.Positions.Add(Center + (Normal + U * Corner.X + V * Corner.Y) * HalfSize);
				Mesh.Normals.Add(Normal);
				Mesh.Tangents.Add(U);
				Mesh.TexCoords().Add(Corner);
			}
			Mesh.Indices.Append({ Base, Base + 1u, Base + 2u, Base, Base + 2u, Base + 3u });
			Mesh.TriangleMaterialSlots.Append({ MaterialSlot, MaterialSlot });
		}
	}

	/** 三个顶点都在中心盒（半边长 100）里的三角形数 —— 被外壳整个包住的那个盒子。 */
	int32 CSMeshVisibilityTest_CountInner(const FCSGpuMeshCPUData& Mesh)
	{
		int32 Count = 0;
		for (int32 Corner = 0; Corner + 2 < Mesh.Indices.Num(); Corner += 3)
		{
			bool bInner = true;
			for (int32 K = 0; K < 3; ++K)
			{
				const FVector3f& P = Mesh.Positions[int32(Mesh.Indices[Corner + K])];
				bInner &= FMath::Max3(FMath::Abs(P.X), FMath::Abs(P.Y), FMath::Abs(P.Z)) <= 101.0f;
			}
			if (bInner) ++Count;
		}
		return Count;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSMeshVisibilityCullAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.MeshVisibility.CullHidden",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

/**
 * 外部可见性剔除：
 *   封闭外壳（10 m 立方体）里包着一个小立方体，外面另放一个小立方体。
 *   ① 全方向视点：被包住的 12 个三角全删，外壳与外面的盒子 24 个全留；压实后逐三角材质号跟着走
 *      （被包住的那个用材质 1，剩下的只能全是 0）；
 *   ② 视点只在地平线以上（默认）：两个外面的盒子的底面从上方永远看不到，只会更少、不会更多；
 *   ③ 包住的盒子无论如何不得留下。
 */
bool FCSMeshVisibilityCullAutomationTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	FCSGpuMeshCPUData Soup;
	CSMeshVisibilityTest_AddBox(Soup, FVector3f(0, 0, 0), 500.0f, 0);      // 外壳（封闭）
	CSMeshVisibilityTest_AddBox(Soup, FVector3f(0, 0, 0), 100.0f, 1);      // 被外壳整个包住
	CSMeshVisibilityTest_AddBox(Soup, FVector3f(2000, 0, 0), 100.0f, 0);   // 外面

	auto Upload = [this, World, &Soup]() -> UCSMesh*
	{
		UCSMesh* Mesh = UCSMeshOps::AllocateGpuMesh(World, 3, 3);
		return TestTrue(TEXT("Snapshot upload"), UCSMeshOps::CopyFromMeshSnapshot(Mesh, Soup)) ? Mesh : nullptr;
	};

	FCSMeshVisibilityCullOptions Options;
	Options.NumDirections = 96;
	Options.Resolution = 256;

	// ① 全方向
	UCSMesh* AllAround = Upload();
	if (!AllAround) return false;
	Options.MinElevationDegrees = -90.0f;
	FCSMeshVisibilityCullResult AllAroundResult;
	UCSMeshVisibilityOps::CullHiddenTriangles(AllAround, 10000.0f, Options, AllAroundResult);
	TestEqual(TEXT("剔除前 36 个三角"), AllAroundResult.TrianglesBefore, 36);
	TestEqual(TEXT("每个方向一张透视 + 一张正交"), AllAroundResult.NumViews, 2 * Options.NumDirections);
	TestTrue(TEXT("剔除生效"), AllAroundResult.bApplied);
	TestEqual(TEXT("全方向：留下外壳与外面的盒子，24 个"), AllAroundResult.TrianglesAfter, 24);

	FCSGpuMeshCPUData AfterAllAround;
	if (TestTrue(TEXT("Readback"), AllAround->ReadbackMeshSync(AfterAllAround)))
	{
		TestEqual(TEXT("回读的索引数与结果一致"), AfterAllAround.Indices.Num(), AllAroundResult.TrianglesAfter * 3);
		TestEqual(TEXT("被包住的盒子一个三角都不剩"), CSMeshVisibilityTest_CountInner(AfterAllAround), 0);
		bool bOnlySlotZero = AfterAllAround.TriangleMaterialSlots.Num() == AfterAllAround.Indices.Num() / 3;
		for (int32 Slot : AfterAllAround.TriangleMaterialSlots) bOnlySlotZero &= Slot == 0;
		TestTrue(TEXT("逐三角材质号跟着三角形一起压实（材质 1 只属于被包住的盒子）"), bOnlySlotZero);
	}
	AllAround->ReleaseDeferred();

	// ② 默认：视点只在地平线以上
	UCSMesh* AboveOnly = Upload();
	if (!AboveOnly) return false;
	Options.MinElevationDegrees = 0.0f;
	FCSMeshVisibilityCullResult AboveResult;
	UCSMeshVisibilityOps::CullHiddenTriangles(AboveOnly, 10000.0f, Options, AboveResult);
	TestTrue(TEXT("地平线以上：底面看不到，只会更少"), AboveResult.TrianglesAfter <= AllAroundResult.TrianglesAfter);
	TestTrue(TEXT("地平线以上：顶面与侧面都还在（每个外面的盒子至少 10 个）"), AboveResult.TrianglesAfter >= 20);
	FCSGpuMeshCPUData AfterAbove;
	if (TestTrue(TEXT("Readback"), AboveOnly->ReadbackMeshSync(AfterAbove)))
		TestEqual(TEXT("地平线以上：被包住的盒子同样不剩"), CSMeshVisibilityTest_CountInner(AfterAbove), 0);
	AboveOnly->ReleaseDeferred();
	return true;
}

#endif
