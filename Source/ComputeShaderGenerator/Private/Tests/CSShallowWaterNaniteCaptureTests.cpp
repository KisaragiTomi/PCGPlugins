#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "ComputeShaderShallowWater.h"
#include "CSBoxSceneCollection.h"
#include "CSBoxSceneCollectionImpl.h"
#include "CSNaniteHeightCapture.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "RenderUtils.h"
#include "Tests/AutomationEditorCommon.h"
#include "TextureResource.h"
#include "UObject/Package.h"

namespace
{
	/** 拍摄中心放在高处：新关卡里就算有什么东西，也进不了高度图。 */
	const FVector CSSWNaniteTest_Origin(0.0, 0.0, 20000.0);
	constexpr double CSSWNaniteTest_CaptureSize = 2560.0;
	/** WorldPixelSize：TextureSize = 2560 / 20 = 128。 */
	constexpr double CSSWNaniteTest_TexelSize = 20.0;
	constexpr int32 CSSWNaniteTest_TextureSize = 128;
	/** CameraHeight = Origin.Z + MaxHeight。 */
	constexpr double CSSWNaniteTest_MaxHeight = 1000.0;
	/** 128 = 48 + 48 + 32：3×3 块，末块不整，块缝落在 texel 48 / 96（相对中心 -320 / +640）。 */
	constexpr int32 CSSWNaniteTest_TileSize = 48;
	constexpr float CSSWNaniteTest_Tolerance = 0.5f;
	/** CSSW 的清屏哨兵：depth = ActorZ + MaxHeight + 9000 ⇒ 高度 -9000。 */
	constexpr float CSSWNaniteTest_Sentinel = -9000.0f;

	/** 引擎 Cube 复制一份、开 Nanite、同步构建。立方体的 Nanite 几何与 fallback 完全相同，A/B 才有逐 texel 的真值。 */
	UStaticMesh* CSSWNaniteTest_MakeNaniteCube()
	{
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (!Cube) return nullptr;

		UStaticMesh* Mesh = DuplicateObject<UStaticMesh>(Cube, GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), TEXT("SM_CSSWNaniteCaptureCube")));
		FMeshNaniteSettings Settings = Mesh->GetNaniteSettings();
		Settings.bEnabled = true;
		// 三角形路径跳过"没有 fallback"的 Nanite 网格；A/B 对比与隐藏道具那条都要它在，不赌平台默认值。
		Settings.GenerateFallback = ENaniteGenerateFallback::Enabled;
		Mesh->SetNaniteSettings(Settings);
		// 传 OutErrors 会关掉异步编译 —— 这一行返回时渲染数据必须已经在了。
		TArray<FText> Errors;
		Mesh->Build(/*bInSilent*/ true, &Errors);
		return Mesh;
	}

	/** 带 CSSW 标签的道具。Offset 相对拍摄中心；引擎 Cube 是居中的 100cm 立方体。 */
	AStaticMeshActor* CSSWNaniteTest_SpawnProp(UWorld* World, UStaticMesh* Mesh, const FVector& Offset, const FRotator& Rotation, const FVector& Scale)
	{
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.ObjectFlags = RF_Transient;
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(),
			FTransform(Rotation, CSSWNaniteTest_Origin + Offset, Scale), SpawnParameters);
		if (!Actor) return nullptr;

		UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetStaticMesh(Mesh);
		// 引擎默认材质为所有用途都编好了着色器，不会在测试里等一轮材质编译。
		Component->SetMaterial(0, UMaterial::GetDefaultMaterial(MD_Surface));
		Actor->Tags.Add(FName(TEXT("CSSW")));
		return Actor;
	}

	/** 回读 RT_SceneDepth 并换成高度（CameraHeight - depth）。回读自带 flush。 */
	TArray<float> CSSWNaniteTest_ReadGround(ACSShallowWaterCapture* Water)
	{
		TArray<FLinearColor> Pixels;
		Water->RT_SceneDepth->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Pixels);

		const float CameraHeight = float(Water->GetActorLocation().Z + Water->MaxHeight);
		TArray<float> Ground;
		Ground.Reserve(Pixels.Num());
		for (const FLinearColor& Pixel : Pixels) Ground.Add(CameraHeight - Pixel.R);
		return Ground;
	}

	/** 相对拍摄中心的世界 XY 落在哪个 texel（行优先下标）。 */
	int32 CSSWNaniteTest_Texel(double OffsetX, double OffsetY)
	{
		const int32 X = FMath::FloorToInt32((OffsetX + CSSWNaniteTest_CaptureSize * 0.5) / CSSWNaniteTest_TexelSize);
		const int32 Y = FMath::FloorToInt32((OffsetY + CSSWNaniteTest_CaptureSize * 0.5) / CSSWNaniteTest_TexelSize);
		return Y * CSSWNaniteTest_TextureSize + X;
	}

	bool CSSWNaniteTest_IsEmpty(float Ground)
	{
		return Ground < CSSWNaniteTest_Sentinel + 1.0f;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSShallowWaterNaniteCaptureAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.ShallowWater.NaniteCapture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

/**
 * CSSW 高度图里 Nanite 道具交给渲染器（custom render pass）去拍的端到端判据，全部是真渲染 + 回读：
 *   ① 分流：按 Nanite 画着的交给渲染器；普通网格、隐藏的 Nanite 网格留在三角形路径，不能从图里消失；
 *   ② 同序：CaptureAll 之后紧接着的回读就含渲染器那份，不用多等一帧；
 *   ③ 对齐：每块的正交视锥、朝向、深度换算与三角形路径逐 texel 一致。立方体的 Nanite 几何与 fallback
 *      完全相同，所以两条路径拍同一批道具必须得到同一张图 —— 块边长压到 48，块缝上也不能差；
 *   ④ 约定：比 CameraHeight 还高的几何钳到 CameraHeight，没东西的 texel 保留清屏哨兵。
 * 只断言 CPU 侧"pass 加进去了"证明不了任何事：朝向写反、差半个 texel、深度偏移算错，CPU 侧照样全绿。
 */
bool FCSShallowWaterNaniteCaptureAutomationTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("CSSW 默认用渲染器拍 Nanite"), GetDefault<ACSShallowWaterCapture>()->bCaptureNaniteWithRenderer);

	if (!UseNanite(GMaxRHIShaderPlatform))
	{
		AddWarning(TEXT("这台机器 / 这个 RHI 不支持 Nanite，渲染器路径无从验起（跳过，不算通过）。"));
		return true;
	}

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	UStaticMesh* NaniteCube = CSSWNaniteTest_MakeNaniteCube();
	UStaticMesh* PlainCube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Nanite cube"), NaniteCube) || !TestNotNull(TEXT("Engine cube"), PlainCube)) return false;
	if (!TestTrue(TEXT("复制出来的 Cube 有 Nanite 数据"), NaniteCube->HasValidNaniteData())) return false;
	const bool bPlainCubeIsPlain = !PlainCube->HasValidNaniteData();
	if (!bPlainCubeIsPlain) AddWarning(TEXT("引擎的 BasicShapes/Cube 开着 Nanite（或 r.Nanite.ForceEnableMeshes），'普通网格留在三角形路径' 这一条验不了。"));

	// 轴对齐立方体的中心都放在 ≡5 (mod 20) 的位置：边落在 texel 中心之间，两条路径的光栅化规则差异碰不到。
	const FVector FlatScale(2.0, 2.0, 1.0);
	AStaticMeshActor* A = CSSWNaniteTest_SpawnProp(World, NaniteCube, FVector(-895.0, -895.0, 100.0), FRotator::ZeroRotator, FlatScale);   // 顶 +150
	AStaticMeshActor* B = CSSWNaniteTest_SpawnProp(World, PlainCube, FVector(1005.0, -895.0, 300.0), FRotator::ZeroRotator, FlatScale);    // 顶 +350
	AStaticMeshActor* C = CSSWNaniteTest_SpawnProp(World, NaniteCube, FVector(-895.0, 1005.0, 200.0), FRotator(20.0, 30.0, 0.0), FVector(3.0, 3.0, 1.0));
	AStaticMeshActor* D = CSSWNaniteTest_SpawnProp(World, NaniteCube, FVector(1005.0, 1005.0, 100.0), FRotator::ZeroRotator, FlatScale);   // 隐藏，顶 +150
	AStaticMeshActor* E = CSSWNaniteTest_SpawnProp(World, NaniteCube, FVector(-895.0, 205.0, 1150.0), FRotator::ZeroRotator, FlatScale);   // 整个在相机之上
	// 斜放的薄板横跨 x = y = -320 的块缝（texel 48）：块与块之间的相机平移若有偏差，斜面上立刻读出来。
	AStaticMeshActor* G = CSSWNaniteTest_SpawnProp(World, NaniteCube, FVector(-315.0, -315.0, 20.0), FRotator(10.0, 15.0, 0.0), FVector(6.0, 6.0, 0.4));
	if (!A || !B || !C || !D || !E || !G)
	{
		AddError(TEXT("道具没生成出来"));
		return false;
	}
	D->GetStaticMeshComponent()->SetVisibility(false);

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags = RF_Transient;
	ACSShallowWaterCapture* Water = World->SpawnActor<ACSShallowWaterCapture>(ACSShallowWaterCapture::StaticClass(), FTransform(CSSWNaniteTest_Origin), SpawnParameters);
	if (!TestNotNull(TEXT("CSSW actor"), Water)) return false;
	Water->CaptureSize = float(CSSWNaniteTest_CaptureSize);
	Water->WorldPixelSize = float(CSSWNaniteTest_TexelSize);
	Water->MaxHeight = float(CSSWNaniteTest_MaxHeight);
	// 构造脚本那条路是 0.01s 的防抖定时器，测试体里没人推定时器，直接调它的落点。
	Water->ConstructionComponent();

	FBox QueryBox = Water->GetGeneratorBoundsWorldBox();
	TestEqual(TEXT("捕获范围 Min.X = 中心 - CaptureSize/2"), QueryBox.Min.X, CSSWNaniteTest_Origin.X - CSSWNaniteTest_CaptureSize * 0.5, 0.01);
	TestEqual(TEXT("捕获范围 Min.Y = 中心 - CaptureSize/2"), QueryBox.Min.Y, CSSWNaniteTest_Origin.Y - CSSWNaniteTest_CaptureSize * 0.5, 0.01);
	TestEqual(TEXT("捕获范围 Max.X = 中心 + CaptureSize/2"), QueryBox.Max.X, CSSWNaniteTest_Origin.X + CSSWNaniteTest_CaptureSize * 0.5, 0.01);

	// ① 分流（与 CaptureAll 同一个查询盒：Z 向两侧各扩 MaxHeight）
	World->SendAllEndOfFrameUpdates();
	QueryBox.Min.Z = FMath::Min(QueryBox.Min.Z, CSSWNaniteTest_Origin.Z - CSSWNaniteTest_MaxHeight);
	QueryBox.Max.Z = FMath::Max(QueryBox.Max.Z, CSSWNaniteTest_Origin.Z + CSSWNaniteTest_MaxHeight);
	{
		FCSBoxSceneCollectOptions Options = Water->MakeBoxSceneCollectOptions(QueryBox);
		Options.RequiredActorTags = { FName(TEXT("CSSW")) };
		Options.bIncludeLandscape = false;
		Options.bCollectNaniteRenderComponents = true;
		const FCSBoxScenePreparedData Prepared = CSBoxSceneCollection::CollectBoxSceneTriangles(World, Options);
		if (!TestTrue(TEXT("收集结果有效"), Prepared.IsValid())) return false;

		TSet<const UPrimitiveComponent*> Routed;
		for (const TWeakObjectPtr<UPrimitiveComponent>& Component : Prepared.Impl->NaniteRenderComponents) Routed.Add(Component.Get());
		TestTrue(TEXT("A（Nanite）交给渲染器"), Routed.Contains(A->GetStaticMeshComponent()));
		TestTrue(TEXT("C（Nanite，斜放）交给渲染器"), Routed.Contains(C->GetStaticMeshComponent()));
		TestTrue(TEXT("E（Nanite，相机之上）交给渲染器"), Routed.Contains(E->GetStaticMeshComponent()));
		TestTrue(TEXT("G（Nanite，横跨块缝）交给渲染器"), Routed.Contains(G->GetStaticMeshComponent()));
		TestFalse(TEXT("D（隐藏的 Nanite）渲染器拍不到 ⇒ 留在三角形路径"), Routed.Contains(D->GetStaticMeshComponent()));
		if (bPlainCubeIsPlain)
		{
			TestFalse(TEXT("B（普通网格）留在三角形路径"), Routed.Contains(B->GetStaticMeshComponent()));
			TestEqual(TEXT("三角形路径上只剩 B 与 D"), Prepared.Impl->ResolvedRequests.Num(), 2);
		}
	}

	IConsoleVariable* TileSizeCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.CSNaniteHeightCapture.MaxTileSize"));
	if (!TestNotNull(TEXT("块边长 CVar"), TileSizeCVar)) return false;
	const int32 PreviousTileSize = TileSizeCVar->GetInt();
	TileSizeCVar->Set(CSSWNaniteTest_TileSize, ECVF_SetByCode);
	ON_SCOPE_EXIT { TileSizeCVar->Set(PreviousTileSize, ECVF_SetByCode); };

	// 新建的 Nanite 资源在第一次渲染里才上传根页，不赌它同帧可见：先拍一次暖身。
	Water->CaptureAll();
	FlushRenderingCommands();

	// ② 同序：这次之后不 flush，直接回读（回读自带 flush）—— 读到的必须已经含渲染器合并的那份。
	const uint64 MergedBefore = CSNaniteHeightCapture::DebugGetMergedTileCount();
	Water->CaptureAll();
	const TArray<float> Rendered = CSSWNaniteTest_ReadGround(Water);
	TestEqual(TEXT("渲染线程合并的块数 = 3×3"), int32(CSNaniteHeightCapture::DebugGetMergedTileCount() - MergedBefore), 9);
	if (!TestEqual(TEXT("回读的 texel 数"), Rendered.Num(), CSSWNaniteTest_TextureSize * CSSWNaniteTest_TextureSize)) return false;

	// ④ 数值与约定（也顺带守着朝向：四个道具分在四个象限）
	const float Base = float(CSSWNaniteTest_Origin.Z);
	TestEqual(TEXT("A（Nanite，渲染器）顶面"), Rendered[CSSWNaniteTest_Texel(-895.0, -895.0)], Base + 150.0f, CSSWNaniteTest_Tolerance);
	TestEqual(TEXT("B（普通网格）顶面"), Rendered[CSSWNaniteTest_Texel(1005.0, -895.0)], Base + 350.0f, CSSWNaniteTest_Tolerance);
	TestEqual(TEXT("D（隐藏的 Nanite，走 fallback）没有从图里消失"), Rendered[CSSWNaniteTest_Texel(1005.0, 1005.0)], Base + 150.0f, CSSWNaniteTest_Tolerance);
	TestEqual(TEXT("E（相机之上）钳到 CameraHeight"), Rendered[CSSWNaniteTest_Texel(-895.0, 205.0)], Base + float(CSSWNaniteTest_MaxHeight), CSSWNaniteTest_Tolerance);
	TestEqual(TEXT("没有几何的 texel 保留清屏哨兵"), Rendered[CSSWNaniteTest_Texel(405.0, -895.0)], CSSWNaniteTest_Sentinel, 1.0f);

	// ③ 与三角形路径逐 texel 对比
	Water->bCaptureNaniteWithRenderer = false;
	const uint64 MergedBeforeTriangles = CSNaniteHeightCapture::DebugGetMergedTileCount();
	Water->CaptureAll();
	const TArray<float> Triangles = CSSWNaniteTest_ReadGround(Water);
	Water->bCaptureNaniteWithRenderer = true;
	TestEqual(TEXT("关掉开关后不再走渲染器"), int32(CSNaniteHeightCapture::DebugGetMergedTileCount() - MergedBeforeTriangles), 0);
	if (!TestEqual(TEXT("回读的 texel 数（三角形路径）"), Triangles.Num(), Rendered.Num())) return false;

	const int32 Size = CSSWNaniteTest_TextureSize;
	int32 Compared = 0;
	int32 Mismatched = 0;
	int32 CoverageMismatched = 0;
	float WorstError = 0.0f;
	for (int32 Y = 1; Y < Size - 1; ++Y)
	{
		for (int32 X = 1; X < Size - 1; ++X)
		{
			const int32 Index = Y * Size + X;
			if (CSSWNaniteTest_IsEmpty(Rendered[Index]) != CSSWNaniteTest_IsEmpty(Triangles[Index])) ++CoverageMismatched;

			// 剪影边缘两边的光栅化规则不同（GPU 顶点吸附 vs 像素中心包含测试），只比 3×3 邻域两边都有几何的 texel。
			bool bInterior = true;
			for (int32 Dy = -1; Dy <= 1 && bInterior; ++Dy)
			{
				for (int32 Dx = -1; Dx <= 1 && bInterior; ++Dx)
				{
					const int32 Neighbour = (Y + Dy) * Size + (X + Dx);
					bInterior = !CSSWNaniteTest_IsEmpty(Rendered[Neighbour]) && !CSSWNaniteTest_IsEmpty(Triangles[Neighbour]);
				}
			}
			if (!bInterior) continue;

			++Compared;
			const float Error = FMath::Abs(Rendered[Index] - Triangles[Index]);
			WorstError = FMath::Max(WorstError, Error);
			if (Error > CSSWNaniteTest_Tolerance) ++Mismatched;
		}
	}
	AddInfo(FString::Printf(TEXT("逐 texel 对比：比了 %d 个，超差 %d 个，最大误差 %.3f cm，覆盖不一致 %d 个"), Compared, Mismatched, WorstError, CoverageMismatched));
	TestTrue(TEXT("可比的 texel 足够多（A/B/C/D/E/G 的内部）"), Compared >= 300);
	TestEqual(TEXT("渲染器路径与三角形路径逐 texel 一致"), Mismatched, 0);
	// 整体差半个 / 一个 texel 时，六个道具的剪影边会成排地对不上；正确对齐时只剩斜边上极个别的吸附差。
	TestTrue(TEXT("覆盖范围一致（只容许斜边上个别 texel 的吸附差）"), CoverageMismatched <= 6);

	return true;
}

#endif
