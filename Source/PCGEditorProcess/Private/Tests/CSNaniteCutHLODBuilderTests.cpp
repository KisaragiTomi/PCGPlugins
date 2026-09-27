#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSNaniteCutHLODBuilder.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "FoliageInstancedStaticMeshComponent.h"
#include "GameFramework/Actor.h"
#include "Materials/Material.h"
#include "RenderUtils.h"
#include "StaticMeshResources.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Package.h"
#include "WorldPartition/HLOD/HLODBuilder.h"

namespace
{
	/** 引擎 Cube 复制一份、开 Nanite、同步构建。12 个三角就是一个 cluster，根 = 整个立方体，截面的三角数可以精确预言。 */
	UStaticMesh* CSNaniteCutHLODTest_MakeNaniteCube()
	{
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (!Cube) return nullptr;

		UStaticMesh* Mesh = DuplicateObject<UStaticMesh>(Cube, GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), TEXT("SM_CSNaniteCutHLODCube")));
		FMeshNaniteSettings Settings = Mesh->GetNaniteSettings();
		Settings.bEnabled = true;
		Mesh->SetNaniteSettings(Settings);
		// 传 OutErrors 会关掉异步编译 —— 这一行返回时渲染数据必须已经在了。
		TArray<FText> Errors;
		Mesh->Build(/*bInSilent*/ true, &Errors);
		return Mesh;
	}

	UStaticMeshComponent* CSNaniteCutHLODTest_SpawnMesh(UWorld* World, UStaticMesh* Mesh, const FTransform& Transform)
	{
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.ObjectFlags = RF_Transient;
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Transform, SpawnParameters);
		if (!Actor) return nullptr;
		UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetStaticMesh(Mesh);
		Component->SetMaterial(0, UMaterial::GetDefaultMaterial(MD_Surface));
		return Component;
	}

	template <typename TComponent>
	TComponent* CSNaniteCutHLODTest_AddInstanced(UWorld* World, UStaticMesh* Mesh, const TArray<FTransform>& Instances)
	{
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.ObjectFlags = RF_Transient;
		AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParameters);
		if (!Actor) return nullptr;
		TComponent* Component = NewObject<TComponent>(Actor);
		Actor->SetRootComponent(Component);
		Component->SetStaticMesh(Mesh);
		Component->RegisterComponent();
		for (const FTransform& Instance : Instances) Component->AddInstance(Instance, /*bWorldSpace*/ true);
		return Component;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSNaniteCutHLODBuilderAutomationTest,
	"PCGPlugins.PCGEditorProcess.NaniteCutHLOD.Build",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

/**
 * 走引擎的 UHLODBuilder::Build 入口（与 World Partition 构建 HLOD 时同一个），断言：
 *   ① 普通组件、镜像组件、ISM 的每个实例都进了 HLOD，foliage 组件与带排除标签的（打在 actor 上 / 打在组件上）
 *      被跳过 —— 三角数正好是 5 个立方体；
 *   ② 输出是一个静态网格组件，网格建在 AssetsOuter 里、去掉了 Public/Standalone，组件放在 WorldPosition；
 *   ③ 网格烘到了枢轴的局部空间：放到 WorldPosition 之后，世界包围盒与源物体的并集一致；
 *   ④ 设置：沿用源材质；开 Nanite 输出时要求预热；
 *   ⑤ 外部可见性剔除（默认开）：视点只在地平线以上，每个立方体的底面永远看不到、被删，别的面都在。
 * ①～③ 关掉剔除断言精确三角数，⑤ 再开着建一遍。
 */
bool FCSNaniteCutHLODBuilderAutomationTest::RunTest(const FString& Parameters)
{
	if (!UseNanite(GMaxRHIShaderPlatform))
	{
		AddWarning(TEXT("这台机器 / 这个 RHI 不支持 Nanite，HLOD 截面构建无从验起（跳过，不算通过）。"));
		return true;
	}

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	UStaticMesh* Cube = CSNaniteCutHLODTest_MakeNaniteCube();
	if (!TestNotNull(TEXT("Nanite cube"), Cube) || !TestTrue(TEXT("复制出来的 Cube 有 Nanite 数据"), Cube->HasValidNaniteData())) return false;
	const int32 CubeTriangles = Cube->GetRenderData()->LODResources[0].GetNumTriangles();

	// 离原点远、带旋转和镜像：枢轴和变换烘错了才看得出来。
	const FVector Center(20000.0, -15000.0, 300.0);
	UStaticMeshComponent* A = CSNaniteCutHLODTest_SpawnMesh(World, Cube, FTransform(FRotator(0.0, 25.0, 0.0), Center + FVector(600.0, 0.0, 0.0), FVector(2.0)));
	UStaticMeshComponent* B = CSNaniteCutHLODTest_SpawnMesh(World, Cube, FTransform(FRotator::ZeroRotator, Center + FVector(-600.0, 400.0, 0.0), FVector(-1.0, 1.0, 1.0)));
	UInstancedStaticMeshComponent* Instanced = CSNaniteCutHLODTest_AddInstanced<UInstancedStaticMeshComponent>(World, Cube,
		{ FTransform(Center + FVector(0.0, -500.0, 0.0)), FTransform(Center + FVector(200.0, -500.0, 0.0)), FTransform(Center + FVector(400.0, -500.0, 0.0)) });
	UFoliageInstancedStaticMeshComponent* Foliage = CSNaniteCutHLODTest_AddInstanced<UFoliageInstancedStaticMeshComponent>(World, Cube,
		{ FTransform(Center + FVector(0.0, 800.0, 0.0)), FTransform(Center + FVector(300.0, 800.0, 0.0)) });
	// 带排除标签的两种：标签打在 actor 上，与打在组件上。
	UStaticMeshComponent* ExcludedByActorTag = CSNaniteCutHLODTest_SpawnMesh(World, Cube, FTransform(Center + FVector(0.0, 0.0, 600.0)));
	UStaticMeshComponent* ExcludedByComponentTag = CSNaniteCutHLODTest_SpawnMesh(World, Cube, FTransform(Center + FVector(300.0, 0.0, 600.0)));
	if (!A || !B || !Instanced || !Foliage || !ExcludedByActorTag || !ExcludedByComponentTag)
	{
		AddError(TEXT("源组件没生成出来"));
		return false;
	}
	ExcludedByActorTag->GetOwner()->Tags.Add(UCSNaniteCutOps::DefaultExcludeTag());
	ExcludedByComponentTag->ComponentTags.Add(UCSNaniteCutOps::DefaultExcludeTag());
	World->SendAllEndOfFrameUpdates();

	FBox ExpectedBounds(ForceInit);
	ExpectedBounds += A->Bounds.GetBox();
	ExpectedBounds += B->Bounds.GetBox();
	for (int32 Instance = 0; Instance < Instanced->GetInstanceCount(); ++Instance)
	{
		FTransform InstanceTransform;
		Instanced->GetInstanceTransform(Instance, InstanceTransform, /*bWorldSpace*/ true);
		ExpectedBounds += Cube->GetBoundingBox().TransformBy(InstanceTransform);
	}

	UCSNaniteCutHLODBuilder* Builder = NewObject<UCSNaniteCutHLODBuilder>(GetTransientPackage());
	UCSNaniteCutHLODBuilderSettings* Settings = NewObject<UCSNaniteCutHLODBuilderSettings>(Builder);
	Builder->SetHLODBuilderSettings(Settings);
	TestTrue(TEXT("可见性剔除默认开"), Settings->bCullHidden);
	Settings->bCullHidden = false;

	TestTrue(TEXT("沿用源材质"), Settings->IsReusingSourceMaterials());
	TestFalse(TEXT("不开 Nanite 输出时不要预热"), Builder->RequiresWarmup());

	FHLODBuildContext Context;
	Context.World = World;
	Context.SourceComponents = { A, B, Instanced, Foliage, ExcludedByActorTag, ExcludedByComponentTag };
	Context.AssetsOuter = GetTransientPackage();
	Context.AssetsBaseName = TEXT("CSNaniteCutHLODTest");
	Context.WorldPosition = ExpectedBounds.GetCenter();
	Context.MinVisibleDistance = 25600.0;

	const FHLODBuildResult Result = static_cast<const UHLODBuilder*>(Builder)->Build(Context);
	if (!TestEqual(TEXT("输出一个组件"), Result.HLODComponents.Num(), 1)) return false;

	UStaticMeshComponent* Output = Cast<UStaticMeshComponent>(Result.HLODComponents[0]);
	if (!TestNotNull(TEXT("输出是静态网格组件"), Output)) return false;
	UStaticMesh* OutputMesh = Output->GetStaticMesh();
	if (!TestNotNull(TEXT("输出组件有网格"), OutputMesh)) return false;

	TestTrue(TEXT("网格建在 AssetsOuter 里"), OutputMesh->GetOuter() == Context.AssetsOuter);
	TestFalse(TEXT("去掉了 RF_Public / RF_Standalone"), OutputMesh->HasAnyFlags(RF_Public | RF_Standalone));
	// 组件还没注册、也没有父级，相对位置就是它要落的世界位置（WP 把它挂到 HLOD actor 上时保持世界变换）。
	TestTrue(TEXT("组件放在 WorldPosition"), Output->GetRelativeLocation().Equals(Context.WorldPosition, 0.01));

	const int32 OutputTriangles = OutputMesh->GetRenderData()->LODResources[0].GetNumTriangles();
	AddInfo(FString::Printf(TEXT("立方体 %d 三角，HLOD %d 三角"), CubeTriangles, OutputTriangles));
	TestEqual(TEXT("两个组件 + 三个实例 = 5 个立方体，foliage 与带排除标签的（actor / 组件各一）不算"), OutputTriangles, 5 * CubeTriangles);

	const FBox OutputBounds = OutputMesh->GetBoundingBox().TransformBy(FTransform(Context.WorldPosition));
	TestTrue(TEXT("放到枢轴后与源物体的并集重合（Min）"), OutputBounds.Min.Equals(ExpectedBounds.Min, 1.0));
	TestTrue(TEXT("放到枢轴后与源物体的并集重合（Max）"), OutputBounds.Max.Equals(ExpectedBounds.Max, 1.0));

	// ⑤ 开剔除再建一遍：底面（每个立方体 1/6 的三角）从地平线以上看不到；侧面之间留着 1 m 的缝，陡的视角看得进去。
	Settings->bCullHidden = true;
	Context.AssetsBaseName = TEXT("CSNaniteCutHLODTest_Culled");
	const FHLODBuildResult Culled = static_cast<const UHLODBuilder*>(Builder)->Build(Context);
	const UStaticMeshComponent* CulledOutput = Culled.HLODComponents.Num() == 1 ? Cast<UStaticMeshComponent>(Culled.HLODComponents[0]) : nullptr;
	const UStaticMesh* CulledMesh = CulledOutput ? CulledOutput->GetStaticMesh().Get() : nullptr;
	if (TestNotNull(TEXT("开剔除也输出一个静态网格组件"), CulledOutput) && TestNotNull(TEXT("开剔除的输出有网格"), CulledMesh))
	{
		const int32 CulledTriangles = CulledMesh->GetRenderData()->LODResources[0].GetNumTriangles();
		const int32 BottomTriangles = CubeTriangles / 6;
		AddInfo(FString::Printf(TEXT("开剔除：HLOD %d 三角（不剔除 %d）"), CulledTriangles, OutputTriangles));
		TestTrue(TEXT("开剔除：删掉了看不见的底面"), CulledTriangles < OutputTriangles);
		TestTrue(TEXT("开剔除：除了底面一个都不少"), CulledTriangles >= 5 * (CubeTriangles - BottomTriangles));
	}

	Settings->bEnableNaniteOutput = true;
	TestTrue(TEXT("开 Nanite 输出时要求预热"), Builder->RequiresWarmup());
	return true;
}

#endif
