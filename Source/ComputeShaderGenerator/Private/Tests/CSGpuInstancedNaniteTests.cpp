#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSGpuInstancedMeshComponent.h"
#include "CSGpuInstancedNaniteComponent.h"

#include "Components/SceneCaptureComponent2D.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/Material.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RenderUtils.h"
#include "Tests/AutomationEditorCommon.h"
#include "TextureResource.h"
#include "UObject/Package.h"

namespace
{
	/** 相机离地高度与画幅。90° 视角下，地面一侧的半宽恰好等于高度。 */
	constexpr double CSNaniteTest_CameraHeight = 600.0;
	constexpr int32 CSNaniteTest_ImageSize = 64;
	/** 实例间距。Cube 是居中的 100cm 立方体，300 的间距在画面上留出 ~6 像素的空隙。 */
	constexpr double CSNaniteTest_Spacing = 300.0;

	/** 引擎 Cube 复制一份、开 Nanite、同步构建。测的是渲染路，不能赌 Content/ 里有没有 Nanite 资产。 */
	UStaticMesh* CSNaniteTest_MakeNaniteCube()
	{
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (!Cube) return nullptr;

		UStaticMesh* Mesh = DuplicateObject<UStaticMesh>(Cube, GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), TEXT("SM_CSNaniteModeCube")));
		FMeshNaniteSettings Settings = Mesh->GetNaniteSettings();
		Settings.bEnabled = true;
		Mesh->SetNaniteSettings(Settings);
		// 传 OutErrors 会关掉异步编译 —— 测试要的就是这一行返回时渲染数据已经在了。
		TArray<FText> Errors;
		Mesh->Build(/*bInSilent*/ true, &Errors);
		return Mesh;
	}

	/** 「十字 + 四角」九个位置：十字那五个是要看见的，四角是用来藏的。
	 *  两组在任何翻转 / 90° 旋转下都各自映射回自己，所以判据不依赖相机的轴向约定。 */
	TArray<FVector> CSNaniteTest_CrossPositions()
	{
		const double S = CSNaniteTest_Spacing;
		return { FVector(0, 0, 0), FVector(S, 0, 0), FVector(-S, 0, 0), FVector(0, S, 0), FVector(0, -S, 0) };
	}

	TArray<FVector> CSNaniteTest_CornerPositions()
	{
		const double S = CSNaniteTest_Spacing;
		return { FVector(S, S, 0), FVector(S, -S, 0), FVector(-S, S, 0), FVector(-S, -S, 0) };
	}

	/** 按组件的打包约定（FCSGpuInstanceSourceGPU）把平移打成 5 float4 一行。 */
	void CSNaniteTest_AppendRow(TArray<FVector4f>& Rows, const FVector& Location, float Random)
	{
		const FMatrix44f M = FMatrix44f(FTransform(Location).ToMatrixWithScale());
		Rows.Add(FVector4f(M.M[0][0], M.M[0][1], M.M[0][2], 0.0f));
		Rows.Add(FVector4f(M.M[1][0], M.M[1][1], M.M[1][2], 0.0f));
		Rows.Add(FVector4f(M.M[2][0], M.M[2][1], M.M[2][2], 0.0f));
		Rows.Add(FVector4f(M.M[3][0], M.M[3][1], M.M[3][2], Random));
		Rows.Add(FVector4f(FVector3f(Location), 87.0f)); // 剔除球：Nanite 不读，照约定填
	}

	/** 像生产者那样在 GPU 上备一份实例源：行 + 计数器，都停在 SRVMask。 */
	FCSGpuInstanceSourceGPU CSNaniteTest_MakeGpuSource(const TArray<FVector4f>& Rows, uint32 LiveCount, const FBox& LocalBounds)
	{
		FCSGpuInstanceSourceGPU Source;
		ENQUEUE_RENDER_COMMAND(CSNaniteTestMakeSource)([&Source, Rows, LiveCount](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			FRDGBufferRef RowsBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), Rows.Num()), TEXT("CSNaniteTest.Rows"));
			GraphBuilder.QueueBufferUpload(RowsBuffer, Rows.GetData(), Rows.Num() * sizeof(FVector4f), ERDGInitialDataFlags::None);
			FRDGBufferRef CountBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("CSNaniteTest.Count"));
			GraphBuilder.QueueBufferUpload(CountBuffer, &LiveCount, sizeof(uint32), ERDGInitialDataFlags::None);
			Source.PackedInstances = GraphBuilder.ConvertToExternalBuffer(RowsBuffer);
			Source.Counter = GraphBuilder.ConvertToExternalBuffer(CountBuffer);
			GraphBuilder.SetBufferAccessFinal(RowsBuffer, ERHIAccess::SRVMask);
			GraphBuilder.SetBufferAccessFinal(CountBuffer, ERHIAccess::SRVMask);
			GraphBuilder.Execute();
		});
		FlushRenderingCommands();
		Source.Capacity = uint32(Rows.Num() / 5);
		Source.LocalBounds = LocalBounds;
		return Source;
	}

	/** 点刷那样的点云源：世界空间位置 + 朝上的法线，计数器只放前 LiveCount 个。 */
	FCSGpuInstancePointSourceGPU CSNaniteTest_MakePointSource(const TArray<FVector>& WorldPositions, uint32 LiveCount)
	{
		TArray<FVector4f> Positions;
		TArray<FVector4f> Normals;
		FBox WorldBounds(ForceInit);
		for (const FVector& P : WorldPositions)
		{
			Positions.Add(FVector4f(FVector3f(P), 1.0f));
			Normals.Add(FVector4f(0.0f, 0.0f, 1.0f, 0.0f));
			WorldBounds += P;
		}

		FCSGpuInstancePointSourceGPU Source;
		ENQUEUE_RENDER_COMMAND(CSNaniteTestMakePointSource)([&Source, Positions, Normals, LiveCount](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			auto Upload = [&GraphBuilder](const TArray<FVector4f>& Data, const TCHAR* Name)
			{
				FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), Data.Num()), Name);
				GraphBuilder.QueueBufferUpload(Buffer, Data.GetData(), Data.Num() * sizeof(FVector4f), ERDGInitialDataFlags::None);
				TRefCountPtr<FRDGPooledBuffer> Pooled = GraphBuilder.ConvertToExternalBuffer(Buffer);
				GraphBuilder.SetBufferAccessFinal(Buffer, ERHIAccess::SRVMask);
				return Pooled;
			};
			Source.Positions = Upload(Positions, TEXT("CSNaniteTest.PointPositions"));
			Source.Normals = Upload(Normals, TEXT("CSNaniteTest.PointNormals"));
			FRDGBufferRef CountBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("CSNaniteTest.PointCount"));
			GraphBuilder.QueueBufferUpload(CountBuffer, &LiveCount, sizeof(uint32), ERDGInitialDataFlags::None);
			Source.Counter = GraphBuilder.ConvertToExternalBuffer(CountBuffer);
			GraphBuilder.SetBufferAccessFinal(CountBuffer, ERHIAccess::SRVMask);
			GraphBuilder.Execute();
		});
		FlushRenderingCommands();
		Source.Capacity = uint32(WorldPositions.Num());
		Source.InstanceScale = 1.0f;
		Source.WorldBounds = WorldBounds;
		return Source;
	}

	/** 生产者"原地重写计数器、不重新交接"的那种改法。 */
	void CSNaniteTest_WriteCounter(const FCSGpuInstanceSourceGPU& Source, uint32 LiveCount)
	{
		ENQUEUE_RENDER_COMMAND(CSNaniteTestWriteCounter)([Counter = Source.Counter, LiveCount](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			FRDGBufferRef CountBuffer = GraphBuilder.RegisterExternalBuffer(Counter);
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CountBuffer, PF_R32_UINT)), LiveCount);
			GraphBuilder.SetBufferAccessFinal(CountBuffer, ERHIAccess::SRVMask);
			GraphBuilder.Execute();
		});
		FlushRenderingCommands();
	}

	/** 从 Centre 正上方俯拍一张场景深度。真渲染一帧 —— GPU-Scene 的更新（包括我们的写入 pass）就发生在这里面。 */
	TArray<float> CSNaniteTest_CaptureDepth(UWorld* World, const FVector& Centre)
	{
		UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
		Target->RenderTargetFormat = RTF_RGBA32f;
		Target->ClearColor = FLinearColor::Black;
		Target->InitAutoFormat(CSNaniteTest_ImageSize, CSNaniteTest_ImageSize);
		Target->UpdateResourceImmediate(true);

		FActorSpawnParameters SpawnParameters;
		SpawnParameters.ObjectFlags = RF_Transient;
		ASceneCapture2D* CaptureActor = World->SpawnActor<ASceneCapture2D>(
			Centre + FVector(0.0, 0.0, CSNaniteTest_CameraHeight), FRotator(-90.0, 0.0, 0.0), SpawnParameters);
		USceneCaptureComponent2D* Capture = CaptureActor->GetCaptureComponent2D();
		Capture->TextureTarget = Target;
		Capture->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
		Capture->FOVAngle = 90.0f;
		Capture->bCaptureEveryFrame = false;
		Capture->bCaptureOnMovement = false;

		// 帧末的组件更新（换代理、变换）在测试体里没人替我们跑。
		World->SendAllEndOfFrameUpdates();
		// 拍两次：Nanite 的根页在第一帧里才上传，不赌它同帧可见。
		Capture->CaptureScene();
		Capture->CaptureScene();
		FlushRenderingCommands();

		TArray<FLinearColor> Pixels;
		Target->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Pixels);
		CaptureActor->Destroy();

		TArray<float> Depth;
		Depth.Reserve(Pixels.Num());
		for (const FLinearColor& Pixel : Pixels) Depth.Add(Pixel.R);
		return Depth;
	}

	/** 实例（相对拍摄中心）在画面上落在哪个像素 —— 取立方体高度中点的深度，3x3 邻域都在剪影内。 */
	FIntPoint CSNaniteTest_Project(const FVector& Relative)
	{
		const double Half = CSNaniteTest_ImageSize * 0.5;
		const double Scale = Half / CSNaniteTest_CameraHeight;
		// pitch -90 的相机：画面右 = +Y、画面上 = +X。九个位置对这个约定对称，写反了判据也不变。
		return FIntPoint(int32(FMath::RoundToDouble(Half + Relative.Y * Scale)), int32(FMath::RoundToDouble(Half - Relative.X * Scale)));
	}

	/** 3x3 邻域里有几何（深度落在相机与地面之间，背景是远平面）。 */
	bool CSNaniteTest_HasGeometry(const TArray<float>& Depth, const FIntPoint& Pixel, bool bRequireAll)
	{
		int32 Hits = 0, Samples = 0;
		for (int32 Dy = -1; Dy <= 1; ++Dy)
		{
			for (int32 Dx = -1; Dx <= 1; ++Dx)
			{
				const int32 X = FMath::Clamp(Pixel.X + Dx, 0, CSNaniteTest_ImageSize - 1);
				const int32 Y = FMath::Clamp(Pixel.Y + Dy, 0, CSNaniteTest_ImageSize - 1);
				const float D = Depth[Y * CSNaniteTest_ImageSize + X];
				++Samples;
				if (D > 1.0f && D < float(CSNaniteTest_CameraHeight) * 2.0f) ++Hits;
			}
		}
		return bRequireAll ? Hits == Samples : Hits > 0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuInstancedMeshNaniteModeAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.GpuInstancedMesh.NaniteMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

/**
 * Nanite 路的端到端判据：**真渲染**，从深度图里数实例。另外守着"走不走 Nanite 由资产决定"这条规则：
 * 没开 Nanite 的资产不建替身；资产上把 Nanite 关掉并重建，组件要自己退回 GPU 剔除路。
 *
 * CPU 侧的断言（替身在、代理是 Nanite、写入发给了当前代理）只证明"我们以为发出去了"。GPU-Scene 的
 * 实例区间里到底写了什么，引擎没有公开的回读口 —— 所以唯一的真值是画面：该出现的位置有深度、
 * 该藏的位置没有、组件挪走之后旧位置空了。这三件事分别守着写入 pass 的三条分支：
 *   ① 活着的槽写实例变换（InitializeInstanceSceneDataLS）；
 *   ② 计数器之外的槽写 HIDDEN —— 容量 > 活着的个数时，尾巴是上一代或者垃圾；
 *   ③ 图元一动就重写 —— GPU-Scene 存的是世界变换，引擎对 GPU-only 实例从不重传。
 * 任何一条漏了，CPU 侧所有断言照绿，画面是错的（本项目的经典失效形状）。
 */
bool FCSGpuInstancedMeshNaniteModeAutomationTest::RunTest(const FString& Parameters)
{
	if (!UseNanite(GMaxRHIShaderPlatform))
	{
		AddWarning(TEXT("这台机器 / 这个 RHI 不支持 Nanite，Nanite 路无从验起（跳过，不算通过）。"));
		return true;
	}

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	UStaticMesh* NaniteCube = CSNaniteTest_MakeNaniteCube();
	if (!TestNotNull(TEXT("Nanite cube"), NaniteCube)) return false;
	if (!TestTrue(TEXT("复制出来的 Cube 有 Nanite 数据"), NaniteCube->HasValidNaniteData())) return false;

	// 离原点远远的：新关卡里就算有什么东西，也进不了俯拍的画面。
	const FVector Origin(0.0, 0.0, 20000.0);
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags = RF_Transient;
	AActor* Host = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParameters);
	if (!TestNotNull(TEXT("Host actor"), Host)) return false;
	USceneComponent* Root = NewObject<USceneComponent>(Host, TEXT("Root"));
	Host->SetRootComponent(Root);
	Root->RegisterComponent();
	Root->SetWorldLocation(Origin);

	UCSGpuInstancedMeshComponent* Instanced = NewObject<UCSGpuInstancedMeshComponent>(Host, NAME_None, RF_Transient);
	Instanced->SetupAttachment(Root);
	Instanced->RegisterComponent();
	// 引擎默认材质为所有用途都编好了着色器，不会在测试里等一轮材质编译。
	Instanced->InstanceMaterial = UMaterial::GetDefaultMaterial(MD_Surface);

	// 没有开关可设：先给一张没开 Nanite 的资产，必须留在 GPU 剔除路上。
	UStaticMesh* PlainCube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Plain engine cube"), PlainCube)) return false;
	if (!PlainCube->IsNaniteEnabled())
	{
		Instanced->SetBaseMesh(PlainCube);
		TestFalse(TEXT("没开 Nanite 的资产 ⇒ 不走 Nanite 路"), Instanced->IsNaniteRenderPath());
		TestNull(TEXT("没开 Nanite 的资产 ⇒ 不建替身"), Instanced->GetNaniteComponent());
	}
	else
	{
		AddWarning(TEXT("引擎的 BasicShapes/Cube 开着 Nanite（或 r.Nanite.ForceEnableMeshes），'没开就不走' 这一条验不了。"));
	}

	// 换成开了 Nanite 的那张：什么都不用设，自己换路。
	Instanced->SetBaseMesh(NaniteCube);

	const TArray<FVector> Cross = CSNaniteTest_CrossPositions();
	const TArray<FVector> Corners = CSNaniteTest_CornerPositions();
	TArray<FVector> AllNine = Cross;
	AllNine.Append(Corners);

	auto ExpectPicture = [this](const TCHAR* Stage, const TArray<float>& Depth, const TArray<FVector>& Present, const TArray<FVector>& Absent)
	{
		for (const FVector& P : Present)
		{
			TestTrue(FString::Printf(TEXT("[%s] (%.0f, %.0f) 处画出了实例"), Stage, P.X, P.Y), CSNaniteTest_HasGeometry(Depth, CSNaniteTest_Project(P), /*bRequireAll*/ true));
		}
		for (const FVector& P : Absent)
		{
			TestFalse(FString::Printf(TEXT("[%s] (%.0f, %.0f) 处什么都没有"), Stage, P.X, P.Y), CSNaniteTest_HasGeometry(Depth, CSNaniteTest_Project(P), /*bRequireAll*/ false));
		}
	};

	// ---------------------------------------------------------------------------
	// CPU 实例数组：十字五个
	// ---------------------------------------------------------------------------
	{
		TArray<FTransform> Transforms;
		for (const FVector& P : Cross) Transforms.Add(FTransform(P));
		Instanced->SetInstances(Transforms);
	}

	TestTrue(TEXT("BaseMesh 开了 Nanite ⇒ 自动走 Nanite 路"), Instanced->IsNaniteRenderPath());
	UCSGpuInstancedNaniteComponent* Nanite = Instanced->GetNaniteComponent();
	if (!TestNotNull(TEXT("Nanite 替身建起来了"), Nanite)) return false;
	TestEqual(TEXT("GPU-Scene 槽数 = CPU 实例数"), Nanite->GetGpuSceneSlotCount(), uint32(Cross.Num()));
	TestEqual(TEXT("诊断计数 = CPU 实例数"), Instanced->DebugReadDrawnInstanceCountSync(), Cross.Num());
	{
		const FString Mismatch = Instanced->DebugGetDrawnAssetMismatchSync();
		TestTrue(FString::Printf(TEXT("画的就是这一族（%s）"), Mismatch.IsEmpty() ? TEXT("一致") : *Mismatch), Mismatch.IsEmpty());
	}
	TestNull(TEXT("本体自己不进场景（画的是替身）"), Instanced->GetSceneProxy());

	const uint32 WritesBeforeCpu = Nanite->DebugGetDispatchedWriteCount();
	const TArray<float> CpuDepth = CSNaniteTest_CaptureDepth(World, Origin);
	AddInfo(FString::Printf(TEXT("[CPU 源] 派发了 %u 次写入"), Nanite->DebugGetDispatchedWriteCount() - WritesBeforeCpu));
	TestTrue(TEXT("渲染时 GPU-Scene 写入 pass 真被派发了"), Nanite->DebugGetDispatchedWriteCount() > WritesBeforeCpu);
	ExpectPicture(TEXT("CPU 源"), CpuDepth, Cross, Corners);

	// ---------------------------------------------------------------------------
	// GPU packed 源：容量 9，先活 9 个，再原地把计数器改成 5 —— 四角必须重新藏起来
	// ---------------------------------------------------------------------------
	TArray<FVector4f> Rows;
	for (int32 Index = 0; Index < Cross.Num(); ++Index) CSNaniteTest_AppendRow(Rows, Cross[Index], 0.1f * float(Index));
	for (int32 Index = 0; Index < Corners.Num(); ++Index) CSNaniteTest_AppendRow(Rows, Corners[Index], 0.5f + 0.1f * float(Index));
	const FBox SourceBounds(FVector(-CSNaniteTest_Spacing - 100.0, -CSNaniteTest_Spacing - 100.0, -100.0), FVector(CSNaniteTest_Spacing + 100.0, CSNaniteTest_Spacing + 100.0, 100.0));
	const FCSGpuInstanceSourceGPU GpuSource = CSNaniteTest_MakeGpuSource(Rows, /*LiveCount*/ 9u, SourceBounds);
	Instanced->SetInstanceSourceGPU(GpuSource);

	TestTrue(TEXT("替身喂的是 GPU 源"), Nanite == Instanced->GetNaniteComponent() && Nanite->IsFeedingGpuSource());
	TestEqual(TEXT("GPU-Scene 槽数 = 源容量"), Nanite->GetGpuSceneSlotCount(), 9u);
	TestEqual(TEXT("诊断读到的是 GPU 计数器"), Instanced->DebugReadDrawnInstanceCountSync(), 9);
	{
		// 槽数 5 → 9 ⇒ 代理整个换掉，而且是在帧末更新里换的：FStaticMeshComponentBulkReregisterContext
		// 让新代理晚于 CreateRenderState_Concurrent 才出生，那个钩子发不出写入（2026-09-10 实测：这一拍
		// 派发 0 次、九个位置全空，重发一次才画出来）。补写靠 CaptureScene 开头那次 SendAllEndOfFrameUpdates
		// 的 Pre 广播 —— 与游戏里 BeginRenderingViewFamily 同一个时机。
		const uint32 WritesBefore = Nanite->DebugGetDispatchedWriteCount();
		const TArray<float> Depth = CSNaniteTest_CaptureDepth(World, Origin);
		const uint32 Dispatched = Nanite->DebugGetDispatchedWriteCount() - WritesBefore;
		TestTrue(FString::Printf(TEXT("[GPU 源 9/9] 帧末批量重建出来的新代理当帧就收到了写入（派发 %u 次）"), Dispatched), Dispatched > 0);
		ExpectPicture(TEXT("GPU 源 9/9"), Depth, AllNine, {});
	}

	CSNaniteTest_WriteCounter(GpuSource, 5u);
	// 游戏里是替身每帧的 Tick 重写；测试体里没有 Tick，重交接一次同一份源走的是同一个入口（槽数不变 ⇒ 当帧重写）。
	Instanced->SetInstanceSourceGPU(GpuSource);
	TestEqual(TEXT("诊断读到缩回来的计数"), Instanced->DebugReadDrawnInstanceCountSync(), 5);
	ExpectPicture(TEXT("GPU 源 5/9"), CSNaniteTest_CaptureDepth(World, Origin), Cross, Corners);

	// ---------------------------------------------------------------------------
	// 挪走整个 actor：实例必须跟着走，旧位置必须空
	// ---------------------------------------------------------------------------
	const FVector Moved = Origin + FVector(4000.0, 0.0, 0.0);
	Root->SetWorldLocation(Moved);
	ExpectPicture(TEXT("挪走后的新位置"), CSNaniteTest_CaptureDepth(World, Moved), Cross, Corners);
	ExpectPicture(TEXT("挪走后的旧位置"), CSNaniteTest_CaptureDepth(World, Origin), {}, AllNine);

	// ---------------------------------------------------------------------------
	// 点云源：点是世界空间的，Nanite 路先用经典路那个打包 pass 现打临时行、再写 GPU-Scene。
	// 放在挪走之后做：组件此时不在世界原点，WorldToComponent 那一步写反了就会整体偏走。
	// ---------------------------------------------------------------------------
	{
		TArray<FVector> WorldPoints;
		for (const FVector& P : AllNine) WorldPoints.Add(Moved + P);
		Instanced->SetInstanceSourceFromPoints(CSNaniteTest_MakePointSource(WorldPoints, /*LiveCount*/ 5u));
		TestEqual(TEXT("[点云源] GPU-Scene 槽数 = 点容量"), Nanite->GetGpuSceneSlotCount(), 9u);
		const uint32 WritesBefore = Nanite->DebugGetDispatchedWriteCount();
		const TArray<float> Depth = CSNaniteTest_CaptureDepth(World, Moved);
		TestTrue(TEXT("[点云源] 写入 pass 派发了"), Nanite->DebugGetDispatchedWriteCount() > WritesBefore);
		ExpectPicture(TEXT("点云源 5/9"), Depth, Cross, Corners);
	}

	// ---------------------------------------------------------------------------
	// 资产上把 Nanite 关掉并重建：组件要自己退回 GPU 剔除路，替身必须整个消失
	//
	// 走的是编辑器里真实的那条链：OnPostMeshBuild → 挂一次性 Pre-EOF → 下一次 SendAllEndOfFrameUpdates
	// 开头重判。同步 Build 也会经过 FinishBuildInternal，所以 PostMeshBuild 就在这一行里广播。
	// ---------------------------------------------------------------------------
	{
		FMeshNaniteSettings Settings = NaniteCube->GetNaniteSettings();
		Settings.bEnabled = false;
		NaniteCube->SetNaniteSettings(Settings);
		TArray<FText> Errors;
		NaniteCube->Build(/*bInSilent*/ true, &Errors);
		World->SendAllEndOfFrameUpdates();

		TestFalse(TEXT("资产关了 Nanite ⇒ 重建后不再走 Nanite 路"), Instanced->IsNaniteRenderPath());
		TestNull(TEXT("资产关了 Nanite ⇒ 替身被销毁"), Instanced->GetNaniteComponent());
		// 退回 GPU 剔除路之后那条路要真的接得住：点云源照样有常驻缓冲、有布局。
		TestTrue(TEXT("退回 GPU 剔除路后常驻缓冲建起来了"), Instanced->GetGpuLayout().IsValid());
	}

	Host->Destroy();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
