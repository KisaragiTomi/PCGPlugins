#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

// 实例路逐段材质（2026-09-15，用户："附属物的材质不对。让它使用我的资产的正确材质"）。
//
// 守的是三件事，失效方式都是"不报错、画面错"：
//   ① 基础网格有几个材质段就发几个 draw，每段默认画**资产那个槽上挂的**材质 —— 以前空 InstanceMaterial
//      一律画引擎默认材质，设了就整张盖一张，多材质资产（`barrel` = 贴图木头 + 顶点色铁箍）从来没画对过；
//   ② 各 actor 的覆盖属性（DecorMaterial / RoofFinialMaterial ……）留空 = 用资产材质，设了 = 整体覆盖，
//      清空之后必须真的退回资产材质（普通静态网格组件以前会一直带着上一次写进去的覆盖）；
//   ③ 负随机数 = 隐藏实例（组件级契约）：烘焙不带它，材质表随段带进烘焙件。
// 引擎自带的网格全是单段，所以两段的测试网格现场建（瞬态，走与烘焙出口同一条 BuildStaticMesh）。

#include "CSGpuInstancedMeshComponent.h"
#include "CSHouseActor.h"
#include "CSHouseVine.h"   // BuildBaseMesh：摆件 / 裙边摆件把资产读成外部快照的那一份读取器
#include "CSMesh.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EditorAssetLibrary.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/Material.h"
#include "MeshDescription.h"
#include "RenderGraphBuilder.h"
#include "RenderingThread.h"
#include "StaticMeshResources.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	/** 母材质勾了实例化用途的瞬态材质。判据只读标志，不需要真编译（同 GroundDecor.SkirtPropsSurviveBake）。 */
	UMaterial* CSSectionTest_MakeMaterial(const TCHAR* Name)
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), UMaterial::StaticClass(), Name), RF_Transient);
		Material->bUsedWithInstancedStaticMeshes = true;
		return Material;
	}

	/** 每个三角各用自己的三个顶点（不共享），逐顶点属性齐全，槽号逐三角给。三角 t 摆在 x = 200·t 处，互不重叠。 */
	FCSGpuMeshCPUData CSSectionTest_MakeTriangles(const TArray<int32>& Slots, const TArray<UMaterialInterface*>& Materials)
	{
		FCSGpuMeshCPUData Data;
		for (int32 Triangle = 0; Triangle < Slots.Num(); ++Triangle)
		{
			const float X = 200.0f * float(Triangle);
			const int32 Base = Data.Positions.Num();
			Data.Positions.Append({ FVector3f(X, 0.0f, 0.0f), FVector3f(X + 100.0f, 0.0f, 0.0f), FVector3f(X, 100.0f, 0.0f) });
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				Data.Normals.Add(FVector3f::ZAxisVector);
				Data.Tangents.Add(FVector3f::XAxisVector);
				Data.BinormalSigns.Add(1.0f);
				Data.Colors.Add(FVector4f(1.0f, 1.0f, 1.0f, 1.0f));
			}
			Data.TexCoords().Append({ FVector2f(0.0f, 0.0f), FVector2f(1.0f, 0.0f), FVector2f(0.0f, 1.0f) });
			Data.Indices.Append({ uint32(Base), uint32(Base + 1), uint32(Base + 2) });
			Data.TriangleMaterialSlots.Add(Slots[Triangle]);
		}
		for (UMaterialInterface* Material : Materials) Data.Materials.Add(Material);
		Data.SourceSpace = FCSGpuMeshCPUData::ESpace::ComponentLocal;
		Data.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerVertex;
		return Data;
	}

	/** 两段两材质的瞬态 StaticMesh：三角 0 在槽 1、三角 1 在槽 0（故意不按槽排）。 */
	UStaticMesh* CSSectionTest_MakeTwoSectionMesh(UMaterialInterface* SlotA, UMaterialInterface* SlotB)
	{
		const FCSGpuMeshCPUData Data = CSSectionTest_MakeTriangles({ 1, 0 }, {});
		FCSGpuMeshConvertOptions Options;
		Options.bBakeToLocalSpace = false;   // 数据本来就是局部空间
		Options.bFillEmptySlotsWithDefaultMaterial = false;
		FCSGpuMeshAssetOptions AssetOptions;
		AssetOptions.bTransient = true;
		return UCSGpuMeshComponent::BuildStaticMesh(GetTransientPackage(), nullptr, Data, { SlotA, SlotB }, Options, AssetOptions);
	}

	/** 快照里的全部三角（按三个索引记、排好序），用来比"重排之后三角集合没变"。 */
	TArray<FIntVector> CSSectionTest_SortedTriangles(const FCSGpuInstancedBaseMesh& Snapshot)
	{
		TArray<FIntVector> Triangles;
		for (int32 I = 0; I + 2 < Snapshot.Indices.Num(); I += 3)
		{
			Triangles.Add(FIntVector(int32(Snapshot.Indices[I]), int32(Snapshot.Indices[I + 1]), int32(Snapshot.Indices[I + 2])));
		}
		Triangles.Sort([](const FIntVector& A, const FIntVector& B) { return A.X != B.X ? A.X < B.X : (A.Y != B.Y ? A.Y < B.Y : A.Z < B.Z); });
		return Triangles;
	}

	/** 像生产者那样在 GPU 上备一份实例源（行 + 计数器，停在 SRVMask）。与 Nanite 测试那份同一个做法。 */
	FCSGpuInstanceSourceGPU CSSectionTest_MakeGpuSource(const TArray<FVector4f>& Rows, const FBox& LocalBounds)
	{
		FCSGpuInstanceSourceGPU Source;
		const uint32 LiveCount = uint32(Rows.Num() / CS_GPU_INSTANCED_ROW_FLOAT4S);
		ENQUEUE_RENDER_COMMAND(CSSectionTestMakeSource)([&Source, Rows, LiveCount](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			FRDGBufferRef RowsBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), Rows.Num()), TEXT("CSSectionTest.Rows"));
			GraphBuilder.QueueBufferUpload(RowsBuffer, Rows.GetData(), Rows.Num() * sizeof(FVector4f), ERDGInitialDataFlags::None);
			FRDGBufferRef CountBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("CSSectionTest.Count"));
			GraphBuilder.QueueBufferUpload(CountBuffer, &LiveCount, sizeof(uint32), ERDGInitialDataFlags::None);
			Source.PackedInstances = GraphBuilder.ConvertToExternalBuffer(RowsBuffer);
			Source.Counter = GraphBuilder.ConvertToExternalBuffer(CountBuffer);
			GraphBuilder.SetBufferAccessFinal(RowsBuffer, ERHIAccess::SRVMask);
			GraphBuilder.SetBufferAccessFinal(CountBuffer, ERHIAccess::SRVMask);
			GraphBuilder.Execute();
		});
		FlushRenderingCommands();
		Source.Capacity = LiveCount;
		Source.LocalBounds = LocalBounds;
		return Source;
	}

	void CSSectionTest_AppendRow(TArray<FVector4f>& Rows, const FVector& Location, float Random)
	{
		Rows.Add(FVector4f(1.0f, 0.0f, 0.0f, 0.0f));
		Rows.Add(FVector4f(0.0f, 1.0f, 0.0f, 0.0f));
		Rows.Add(FVector4f(0.0f, 0.0f, 1.0f, 0.0f));
		Rows.Add(FVector4f(FVector3f(Location), Random));
		Rows.Add(FVector4f(FVector3f(Location), 300.0f));
	}

	/** 测试世界里的宿主 actor + 已注册的实例组件（有实例才会分配常驻网格，注册过的组件同步传完）。 */
	UCSGpuInstancedMeshComponent* CSSectionTest_MakeRegisteredComponent(UWorld* World)
	{
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.ObjectFlags = RF_Transient;
		AActor* Host = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParameters);
		if (!Host) return nullptr;
		USceneComponent* Root = NewObject<USceneComponent>(Host, TEXT("Root"));
		Host->SetRootComponent(Root);
		Root->RegisterComponent();
		UCSGpuInstancedMeshComponent* Instanced = NewObject<UCSGpuInstancedMeshComponent>(Host, NAME_None, RF_Transient);
		Instanced->SetupAttachment(Root);
		Instanced->RegisterComponent();
		return Instanced;
	}
}

// -----------------------------------------------------------------------------
// ① 组件：逐段取资产材质、整体覆盖、外部快照、段数上限（纯 CPU，不注册、不碰 GPU）
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuInstancedSectionMaterialsTest,
	"PCGPlugins.ComputeShaderGenerator.GpuInstancedMesh.SectionMaterials",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSGpuInstancedSectionMaterialsTest::RunTest(const FString& Parameters)
{
	UMaterial* MatA = CSSectionTest_MakeMaterial(TEXT("M_CSSectionTest_A"));
	UMaterial* MatB = CSSectionTest_MakeMaterial(TEXT("M_CSSectionTest_B"));
	UMaterial* Override = CSSectionTest_MakeMaterial(TEXT("M_CSSectionTest_Override"));

	UStaticMesh* Mesh = CSSectionTest_MakeTwoSectionMesh(MatA, MatB);
	if (!TestNotNull(TEXT("两段测试网格建出来了"), Mesh)) return false;
	const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	if (!TestTrue(TEXT("测试网格有渲染数据"), RenderData && RenderData->LODResources.Num() > 0)) return false;
	const FStaticMeshLODResources& AssetLod0 = RenderData->LODResources[0];
	// 前提：真的是两段、两个槽。一段的话下面每一条都会"意外地"绿。
	if (!TestEqual(TEXT("测试网格 LOD0 有两个材质段"), AssetLod0.Sections.Num(), 2)) return false;
	if (!TestEqual(TEXT("测试网格有两个材质槽"), Mesh->GetStaticMaterials().Num(), 2)) return false;

	// --- 资产路：SetBaseMesh ---
	{
		UCSGpuInstancedMeshComponent* Component = NewObject<UCSGpuInstancedMeshComponent>(GetTransientPackage());
		Component->SetBaseMesh(Mesh);
		const FCSGpuInstancedBaseMesh& Snapshot = Component->GetBaseMeshSnapshot();
		TestTrue(TEXT("快照合法"), Snapshot.IsValid());
		if (TestEqual(TEXT("资产有几个段，快照就有几个段（一段一个 draw）"), Component->GetNumSections(), AssetLod0.Sections.Num()))
		{
			uint32 Covered = 0;
			for (int32 Section = 0; Section < Snapshot.Sections.Num(); ++Section)
			{
				const FCSGpuInstancedSection& Mine = Snapshot.Sections[Section];
				const FStaticMeshSection& Theirs = AssetLod0.Sections[Section];
				TestEqual(*FString::Printf(TEXT("段 %d 的材质槽照抄资产"), Section), Mine.MaterialIndex, Theirs.MaterialIndex);
				TestEqual(*FString::Printf(TEXT("段 %d 的索引区间照抄资产"), Section), Mine.FirstIndex, Snapshot.LODs[0].FirstIndex + Theirs.FirstIndex);
				TestEqual(*FString::Printf(TEXT("段 %d 的索引数照抄资产"), Section), Mine.NumIndices, Theirs.NumTriangles * 3u);
				TestEqual(*FString::Printf(TEXT("段 %d 属于 LOD0"), Section), Mine.LodIndex, 0);
				// **这一条就是用户那句话本身**：没设覆盖时，每段画的是资产那个槽上挂的材质。
				TestTrue(*FString::Printf(TEXT("段 %d 默认画资产槽 %d 的材质"), Section, Mine.MaterialIndex),
					Component->GetSectionMaterial(Section) == Mesh->GetMaterial(Mine.MaterialIndex));
				Covered += Mine.NumIndices;
			}
			TestEqual(TEXT("各段合起来正好覆盖 LOD0 的每一个索引"), Covered, Snapshot.LODs[0].NumIndices);
		}
		TestEqual(TEXT("材质槽数跟资产走"), Component->GetNumMaterials(), 2);
		TestTrue(TEXT("GetMaterial(0) = 资产槽 0"), Component->GetMaterial(0) == MatA);
		TestTrue(TEXT("GetMaterial(1) = 资产槽 1"), Component->GetMaterial(1) == MatB);
		TArray<UMaterialInterface*> Used;
		Component->GetUsedMaterials(Used);
		TestTrue(TEXT("GetUsedMaterials 两张资产材质都报"), Used.Contains(MatA) && Used.Contains(MatB));
		TestEqual(TEXT("资产材质都勾了实例化用途 ⇒ 判据是绿的"), Component->GetMaterialUndrawableReason(), FString());

		// 整体覆盖：设了就盖住每一段。
		Component->SetInstanceMaterial(Override);
		for (int32 Section = 0; Section < Component->GetNumSections(); ++Section)
		{
			TestTrue(*FString::Printf(TEXT("设了 InstanceMaterial，段 %d 画覆盖材质"), Section), Component->GetSectionMaterial(Section) == Override);
		}
		TestTrue(TEXT("覆盖时 GetMaterial 每个槽都是覆盖材质"), Component->GetMaterial(1) == Override);
		// 清掉覆盖：退回资产材质，不是退回默认材质。
		Component->SetInstanceMaterial(nullptr);
		TestTrue(TEXT("清掉覆盖之后段 0 退回资产材质"), Component->GetSectionMaterial(0) == Mesh->GetMaterial(Snapshot.Sections[0].MaterialIndex));
		TestTrue(TEXT("清掉覆盖之后 GetMaterial(1) 退回资产槽 1"), Component->GetMaterial(1) == MatB);

		// 故意破坏：资产某一槽的母材质没勾实例化用途，判据必须当场变红、并指出是哪一槽。一道从来没红过的门等于没有门。
		MatB->bUsedWithInstancedStaticMeshes = false;
		const FString Broken = Component->GetMaterialUndrawableReason();
		AddInfo(FString::Printf(TEXT("故意破坏后的原因串：%s"), *Broken));
		TestTrue(TEXT("摩掉槽 1 的实例化标志之后判据报红"), Broken.Contains(TEXT("bUsedWithInstancedStaticMeshes")) && Broken.Contains(TEXT("材质槽 1")));
		MatB->bUsedWithInstancedStaticMeshes = true;
	}

	// --- 外部快照路：CSHouseVine::BuildBaseMesh → SetBaseMeshFromGpuData（摆件 / 裙边摆件走的就是这条） ---
	{
		FCSGpuMeshCPUData Data;
		if (TestTrue(TEXT("BuildBaseMesh 读得出测试网格"), CSHouseVine::BuildBaseMesh(Mesh, 2, Data)))
		{
			TestEqual(TEXT("BuildBaseMesh 带上了资产的材质表"), Data.Materials.Num(), 2);
			if (Data.Materials.Num() == 2)
			{
				TestTrue(TEXT("...槽 0 / 槽 1 照抄"), Data.Materials[0] == MatA && Data.Materials[1] == MatB);
			}
			if (TestEqual(TEXT("BuildBaseMesh 逐三角带上了材质槽"), Data.TriangleMaterialSlots.Num(), int32(AssetLod0.IndexBuffer.GetNumIndices() / 3)))
			{
				for (const FStaticMeshSection& Section : AssetLod0.Sections)
				{
					for (uint32 Triangle = Section.FirstIndex / 3u; Triangle < Section.FirstIndex / 3u + Section.NumTriangles; ++Triangle)
					{
						TestEqual(*FString::Printf(TEXT("三角 %u 落在它所在段的槽"), Triangle), Data.TriangleMaterialSlots[int32(Triangle)], Section.MaterialIndex);
					}
				}
			}

			UCSGpuInstancedMeshComponent* Component = NewObject<UCSGpuInstancedMeshComponent>(GetTransientPackage());
			Component->SetBaseMeshFromGpuData(Data);
			if (TestEqual(TEXT("外部快照同样一槽一段"), Component->GetNumSections(), 2))
			{
				for (int32 Section = 0; Section < 2; ++Section)
				{
					const int32 Slot = Component->GetBaseMeshSnapshot().Sections[Section].MaterialIndex;
					TestTrue(*FString::Printf(TEXT("外部快照段 %d（槽 %d）默认画数据自带的那张材质"), Section, Slot),
						Component->GetSectionMaterial(Section) == Data.Materials[Slot].Get());
				}
			}
			TestEqual(TEXT("外部快照的判据同样是绿的"), Component->GetMaterialUndrawableReason(), FString());
		}
	}

	// --- 外部快照：槽不相邻时重排索引，三角集合不变；没给槽表时整张一段 ---
	{
		const FCSGpuMeshCPUData Data = CSSectionTest_MakeTriangles({ 1, 0, 1 }, { MatA, MatB });
		UCSGpuInstancedMeshComponent* Component = NewObject<UCSGpuInstancedMeshComponent>(GetTransientPackage());
		Component->SetBaseMeshFromGpuData(Data);
		const FCSGpuInstancedBaseMesh& Snapshot = Component->GetBaseMeshSnapshot();
		if (TestEqual(TEXT("两个槽 ⇒ 两段"), Snapshot.Sections.Num(), 2))
		{
			TestEqual(TEXT("段按槽号升序：段 0 = 槽 0"), Snapshot.Sections[0].MaterialIndex, 0);
			TestEqual(TEXT("槽 0 只有一个三角"), Snapshot.Sections[0].NumIndices, 3u);
			TestEqual(TEXT("槽 1 的两个三角被归到一起"), Snapshot.Sections[1].NumIndices, 6u);
			TestEqual(TEXT("段 1 紧接段 0"), Snapshot.Sections[1].FirstIndex, Snapshot.Sections[0].FirstIndex + Snapshot.Sections[0].NumIndices);
			TestTrue(TEXT("段 0 画槽 0 的材质"), Component->GetSectionMaterial(0) == MatA);
			TestTrue(TEXT("段 1 画槽 1 的材质"), Component->GetSectionMaterial(1) == MatB);
		}
		FCSGpuInstancedBaseMesh Original;
		Original.Indices = Data.Indices;
		TestTrue(TEXT("重排只换了三角的顺序，三角集合逐个不变"), CSSectionTest_SortedTriangles(Snapshot) == CSSectionTest_SortedTriangles(Original));

		FCSGpuMeshCPUData NoSlots = Data;
		NoSlots.TriangleMaterialSlots.Reset();
		NoSlots.Materials.Reset();
		UCSGpuInstancedMeshComponent* Plain = NewObject<UCSGpuInstancedMeshComponent>(GetTransientPackage());
		Plain->SetBaseMeshFromGpuData(NoSlots);
		TestEqual(TEXT("没给槽表 ⇒ 整张一段"), Plain->GetNumSections(), 1);
		TestTrue(TEXT("...索引与数据逐位相同（老用法不受影响）"), Plain->GetBaseMeshSnapshot().Indices == NoSlots.Indices);
		TestTrue(TEXT("...没有材质表时那一段解析成空（代理画默认材质）"), Plain->GetSectionMaterial(0) == nullptr);
		TestTrue(TEXT("...判据把空槽说出来"), Plain->GetMaterialUndrawableReason().Contains(TEXT("是空的")));
	}

	// --- 段数上限：多出来的槽并进最后一段，三角一个不丢 ---
	{
		const int32 NumSlots = CS_GPU_INSTANCED_MAX_DRAWS + 8;
		TArray<int32> Slots;
		for (int32 Slot = 0; Slot < NumSlots; ++Slot) Slots.Add(Slot);
		const FCSGpuMeshCPUData Data = CSSectionTest_MakeTriangles(Slots, {});
		UCSGpuInstancedMeshComponent* Component = NewObject<UCSGpuInstancedMeshComponent>(GetTransientPackage());
		Component->SetBaseMeshFromGpuData(Data);
		const FCSGpuInstancedBaseMesh& Snapshot = Component->GetBaseMeshSnapshot();
		TestEqual(TEXT("段数钳在 CS_GPU_INSTANCED_MAX_DRAWS"), Snapshot.Sections.Num(), CS_GPU_INSTANCED_MAX_DRAWS);
		TestTrue(TEXT("钳过之后快照依然合法"), Snapshot.IsValid());
		uint32 Covered = 0;
		for (const FCSGpuInstancedSection& Section : Snapshot.Sections) Covered += Section.NumIndices;
		TestEqual(TEXT("三角一个不丢"), Covered, uint32(NumSlots * 3));
		if (Snapshot.Sections.Num() == CS_GPU_INSTANCED_MAX_DRAWS)
		{
			TestEqual(TEXT("最后一段吃下多出来的全部槽"), Snapshot.Sections.Last().NumIndices, uint32((NumSlots - CS_GPU_INSTANCED_MAX_DRAWS + 1) * 3));
		}
	}

	return true;
}

// -----------------------------------------------------------------------------
// ② 组件上 GPU：一段一个 arg set；烘焙带材质表、不带隐藏实例
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuInstancedSectionDrawsTest,
	"PCGPlugins.ComputeShaderGenerator.GpuInstancedMesh.SectionDrawsAndBake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGpuInstancedSectionDrawsTest::RunTest(const FString& Parameters)
{
	UMaterial* MatA = CSSectionTest_MakeMaterial(TEXT("M_CSSectionTest_GpuA"));
	UMaterial* MatB = CSSectionTest_MakeMaterial(TEXT("M_CSSectionTest_GpuB"));
	UStaticMesh* Mesh = CSSectionTest_MakeTwoSectionMesh(MatA, MatB);
	if (!TestNotNull(TEXT("两段测试网格"), Mesh)) return false;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("引擎 Cube（单段）"), Cube)) return false;

	// CreateNewMap 会跑 GC：上面这几个对象没有任何引用，不钉住就被回收，后面拿到的全是野指针（09-15 实测崩在这）。
	const TStrongObjectPtr<UObject> KeepMatA(MatA), KeepMatB(MatB), KeepMesh(Mesh), KeepCube(Cube);
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;
	UCSGpuInstancedMeshComponent* Instanced = CSSectionTest_MakeRegisteredComponent(World);
	if (!TestNotNull(TEXT("注册过的实例组件"), Instanced)) return false;

	// --- 一段一个 DrawIndexedIndirect：常驻网格声明的 arg set 数 = 段数 ---
	Instanced->SetBaseMesh(Mesh);
	Instanced->AddInstance(FTransform::Identity);
	const UCSMesh* GpuMesh = Instanced->GetGpuMesh();
	if (!TestNotNull(TEXT("有实例之后常驻网格分配了"), GpuMesh)) return false;
	TestEqual(TEXT("两段 ⇒ 两个 arg set"), GpuMesh->GetIndirectDrawCount(), 2);
	// 上传到 GPU 的那份与快照对得上、每段的材质能画（非阻塞两条 + 着色器映射探针）。
	TestEqual(TEXT("GPU 上画的网格 / 每段材质都是我们以为的那一份"), Instanced->DebugGetDrawnAssetMismatchSync(), FString());

	Instanced->SetBaseMesh(Cube);
	TestEqual(TEXT("换成单段网格 ⇒ 布局收回一个 arg set"), Instanced->GetGpuMesh() ? Instanced->GetGpuMesh()->GetIndirectDrawCount() : -1, 1);

	// --- 烘焙：材质表随段带进资产；负随机数的实例不烘 ---
	Instanced->SetBaseMesh(Mesh);
	TArray<FVector4f> Rows;
	CSSectionTest_AppendRow(Rows, FVector(0.0, 0.0, 0.0), 0.25f);
	CSSectionTest_AppendRow(Rows, FVector(1000.0, 0.0, 0.0), -1.0f);   // 隐藏（门框砖转角墩剔除的那种哨兵）
	CSSectionTest_AppendRow(Rows, FVector(2000.0, 0.0, 0.0), 0.75f);
	const FBox Bounds(FVector(-500.0, -500.0, -500.0), FVector(2500.0, 500.0, 500.0));
	Instanced->SetInstanceSourceGPU(CSSectionTest_MakeGpuSource(Rows, Bounds));
	TestEqual(TEXT("GPU 计数器是三行（隐藏只是不画，行照样在）"), Instanced->DebugReadDrawnInstanceCountSync(), 3);

	const FString BakePath = FString::Printf(TEXT("/Game/Automation/GpuInstanced/SM_SectionBake_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UStaticMesh* Baked = Instanced->SaveToStaticMesh(FTransform::Identity, BakePath, /*bReplaceExistingAsset*/ true, /*bSaveAsset*/ false);
	if (TestNotNull(TEXT("烘得出资产"), Baked))
	{
		// 从资产自己的 MeshDescription 读（同 FrameBricksSurviveBake）：渲染数据可能还在异步构建。
		if (const FMeshDescription* Description = Baked->GetMeshDescription(0))
		{
			TestEqual(TEXT("三角数 = 可见实例数 × 基础网格三角数（隐藏的那个不烘）"), Description->Triangles().Num(), 2 * Mesh->GetNumTriangles(0));
			TestEqual(TEXT("两个材质槽的三角各成一组"), Description->PolygonGroups().Num(), 2);
		}
		else
		{
			AddError(TEXT("烘焙件没有 MeshDescription"));
		}
		const TArray<FStaticMaterial>& BakedMaterials = Baked->GetStaticMaterials();
		if (TestEqual(TEXT("没设覆盖 ⇒ 资产的两个材质槽随网格带进烘焙件"), BakedMaterials.Num(), 2))
		{
			TestTrue(TEXT("...槽 0 / 槽 1 是资产那两张"), BakedMaterials[0].MaterialInterface == MatA && BakedMaterials[1].MaterialInterface == MatB);
		}
		UEditorAssetLibrary::DeleteAsset(BakePath);
	}

	Instanced->GetOwner()->Destroy();
	return true;
}

// -----------------------------------------------------------------------------
// ③ actor：覆盖属性留空 = 资产材质，设了 = 整体覆盖，清空 = 退回资产材质
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuInstancedEmptyOverrideTest,
	"PCGPlugins.ComputeShaderGenerator.GpuInstancedMesh.EmptyOverrideUsesAssetMaterials",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGpuInstancedEmptyOverrideTest::RunTest(const FString& Parameters)
{
	UMaterial* MatA = CSSectionTest_MakeMaterial(TEXT("M_CSSectionTest_HouseA"));
	UMaterial* MatB = CSSectionTest_MakeMaterial(TEXT("M_CSSectionTest_HouseB"));
	UMaterial* Override = CSSectionTest_MakeMaterial(TEXT("M_CSSectionTest_HouseOverride"));
	UStaticMesh* Mesh = CSSectionTest_MakeTwoSectionMesh(MatA, MatB);
	if (!TestNotNull(TEXT("两段测试网格"), Mesh)) return false;

	// CreateNewMap 会跑 GC：上面这几个对象没有任何引用，不钉住就被回收，后面拿到的全是野指针（09-15 实测崩在这）。
	const TStrongObjectPtr<UObject> KeepMatA(MatA), KeepMatB(MatB), KeepOverride(Override), KeepMesh(Mesh);
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;
	ACSHouseActor* House = World->SpawnActor<ACSHouseActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("House actor"), House)) return false;

	// 摆件（实例路，外部快照）与尖顶（普通静态网格组件）各挂一张两段网格，覆盖属性全部留空。
	House->bDecorEnabled = true;
	House->DecorGateMeshes.Reset();
	House->DecorRoofMeshes.Reset();
	House->DecorWallFootMeshes = { Mesh };
	House->DecorMaterial = nullptr;
	House->RoofFinialMesh = Mesh;
	House->RoofFinialMaterial = nullptr;

	// 找出挂着这张网格的那几个组件。实例组件是 NewObject 自动命名的，只能按内容认：
	// 摆件快照是外部快照（BaseMesh 为空），段数 2、且位置数与测试网格 LOD0 相同。
	const int32 MeshVertices = int32(Mesh->GetRenderData()->LODResources[0].GetNumVertices());
	auto FindDecor = [House, MeshVertices]()
	{
		TArray<UCSGpuInstancedMeshComponent*> Found;
		TArray<UCSGpuInstancedMeshComponent*> All;
		House->GetComponents<UCSGpuInstancedMeshComponent>(All);
		for (UCSGpuInstancedMeshComponent* Component : All)
		{
			if (Component->BaseMesh == nullptr && Component->GetNumSections() == 2
				&& Component->GetBaseMeshSnapshot().Positions.Num() == MeshVertices)
			{
				Found.Add(Component);
			}
		}
		return Found;
	};
	auto FindFinials = [House, Mesh]()
	{
		TArray<UStaticMeshComponent*> Found;
		TArray<UStaticMeshComponent*> All;
		House->GetComponents<UStaticMeshComponent>(All);
		for (UStaticMeshComponent* Component : All)
		{
			if (Component->GetStaticMesh() == Mesh) Found.Add(Component);
		}
		return Found;
	};
	auto ExpectDecor = [this, &FindDecor](const TCHAR* Stage, UMaterialInterface* Slot0, UMaterialInterface* Slot1)
	{
		const TArray<UCSGpuInstancedMeshComponent*> Decor = FindDecor();
		if (!TestTrue(FString::Printf(TEXT("%s：找到了摆件组件"), Stage), Decor.Num() > 0)) return;
		for (const UCSGpuInstancedMeshComponent* Component : Decor)
		{
			for (int32 Section = 0; Section < Component->GetNumSections(); ++Section)
			{
				const int32 Slot = Component->GetBaseMeshSnapshot().Sections[Section].MaterialIndex;
				const UMaterialInterface* Expected = Slot == 0 ? Slot0 : Slot1;
				TestTrue(FString::Printf(TEXT("%s：摆件段 %d（槽 %d）画 '%s'，实际 '%s'"), Stage, Section, Slot,
					*GetNameSafe(Expected), *GetNameSafe(Component->GetSectionMaterial(Section))), Component->GetSectionMaterial(Section) == Expected);
			}
		}
	};
	auto ExpectFinials = [this, &FindFinials](const TCHAR* Stage, UMaterialInterface* Slot0, UMaterialInterface* Slot1)
	{
		const TArray<UStaticMeshComponent*> Finials = FindFinials();
		if (!TestTrue(FString::Printf(TEXT("%s：尖顶立起来了"), Stage), Finials.Num() > 0)) return;
		for (const UStaticMeshComponent* Component : Finials)
		{
			TestTrue(FString::Printf(TEXT("%s：尖顶槽 0 画 '%s'，实际 '%s'"), Stage, *GetNameSafe(Slot0), *GetNameSafe(Component->GetMaterial(0))), Component->GetMaterial(0) == Slot0);
			TestTrue(FString::Printf(TEXT("%s：尖顶槽 1 画 '%s'，实际 '%s'"), Stage, *GetNameSafe(Slot1), *GetNameSafe(Component->GetMaterial(1))), Component->GetMaterial(1) == Slot1);
		}
	};

	// ---- 留空：资产材质 ----
	House->RebuildHouse();
	ExpectDecor(TEXT("留空"), MatA, MatB);
	ExpectFinials(TEXT("留空"), MatA, MatB);
	TestEqual(TEXT("留空时尖顶判据是绿的（每个槽都有材质）"), House->GetRoofFinialUndrawableReason(), FString());

	// ---- 设了：整体覆盖，每一段 / 每一个槽 ----
	House->DecorMaterial = Override;
	House->RoofFinialMaterial = Override;
	House->RebuildHouse();
	ExpectDecor(TEXT("整体覆盖"), Override, Override);
	ExpectFinials(TEXT("整体覆盖"), Override, Override);

	// ---- 再清空：必须真的退回资产材质（尖顶组件是复用的，以前会一直带着上一次的覆盖）----
	House->DecorMaterial = nullptr;
	House->RoofFinialMaterial = nullptr;
	House->RebuildHouse();
	ExpectDecor(TEXT("清空覆盖"), MatA, MatB);
	ExpectFinials(TEXT("清空覆盖"), MatA, MatB);

	House->Destroy();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
