#include "CSHouseHandleActor.h"

#include "CSHouseActor.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

ACSHouseHandleActor::ACSHouseHandleActor()
{
	// 抓手默认**不 tick**：它是事件驱动的（gizmo 发 `PostEditMove`）。需要拖拽期逐帧重算的
	// 子类（特征标记要让洞跟手）自己开，并且必须同时 override `ShouldTickIfViewportsOnly`
	// —— 编辑器 world 里 actor 默认一次 tick 都不会发生。
	PrimaryActorTick.bCanEverTick = false;
	PrimaryActorTick.bStartWithTickEnabled = false;

	// 零可视几何：只有一个根。可见的东西要么归宿主房产出，要么是子类自己加的编辑器示意道具。
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	// ⚠️ `USceneComponent` 默认 **Static**。不改成 Movable，`AttachToActor` 到房子下会当场报
	// "static component attached to movable parent"，而 gizmo 拖动更是无从谈起。
	RootComponent->SetMobility(EComponentMobility::Movable);
}

void ACSHouseHandleActor::MakeEditorGizmoProp(UPrimitiveComponent* Component)
{
	if (!Component) return;

	Component->SetMobility(EComponentMobility::Movable);
	// 不挡射线：抓手的示意几何一旦参与碰撞，宿主解析的射线会打在它自己身上。
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Component->SetHiddenInGame(true);
	Component->SetCastShadow(false);
	Component->bIsEditorOnly = true;
}

UMaterialInterface* ACSHouseHandleActor::LoadHighlightMaterial()
{
	static const TCHAR* const HighlightPath = TEXT("/PCGPlugins/HouseTest/M_CSHandleHighlight.M_CSHandleHighlight");
	return LoadObject<UMaterialInterface>(nullptr, HighlightPath);
}

void ACSHouseHandleActor::ApplyHighlightMaterial(UPrimitiveComponent* Component)
{
	if (!Component) return;
	if (UMaterialInterface* Highlight = LoadHighlightMaterial()) Component->SetMaterial(0, Highlight);
}

void ACSHouseHandleActor::ApplyOutlineMaterial(UPrimitiveComponent* Component)
{
	if (!Component) return;
	UMaterialInterface* Highlight = LoadHighlightMaterial();
	if (!Highlight) return;

	// 参数名是 `Scripts/TinyGladeMakeHandleMaterial.py` 定的：Emissive = HighlightColor × Intensity，
	// BaseColor 近黑。强度归零即"一圈暗边"。
	UMaterialInstanceDynamic* Outline = UMaterialInstanceDynamic::Create(Highlight, Component);
	Outline->SetScalarParameterValue(TEXT("Intensity"), 0.0f);
	Component->SetMaterial(0, Outline);
}

UStaticMesh* ACSHouseHandleActor::LoadTGArrowMesh(const TCHAR* Name)
{
	return LoadObject<UStaticMesh>(nullptr, *FString::Printf(TEXT("/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/%s.%s"), Name, Name));
}

bool ACSHouseHandleActor::HandleDrag(bool bFinished)
{
	const bool bHasHost = OnHandleDrag(bFinished);

	// 纪律 ④：自毁只在最终裁决时发生。拖拽途中判的话，窗户从房 A 拖向房 B 的**途中**
	// 会有若干帧两边都够不着 —— 那几帧里它会把自己删掉。
	if (!bHasHost && bFinished && bDestroyWhenHostless && !IsTemplate())
	{
		Destroy();
	}
	return bHasHost;
}

void ACSHouseHandleActor::SetHost(ACSHouseActor* NewHost)
{
	if (Host.Get() == NewHost) return;

	// 纪律 ①：先向旧的解绑再挂新的。顺序反了会在两房相邻时留下一份重复登记
	// （新旧同一个身份，旧那份永远没人来收）。
	DetachFromHost();
	Host = NewHost;
}

void ACSHouseHandleActor::DetachFromHost()
{
	OnDetachFromHost();
	Host = nullptr;
}

void ACSHouseHandleActor::Destroyed()
{
	DetachFromHost();
	Super::Destroyed();
}

void ACSHouseHandleActor::EndPlay(const EEndPlayReason::Type Reason)
{
	// 两处都调，`DetachFromHost` 幂等（子类的 `OnDetachFromHost` 也必须幂等），重复调没有代价。
	DetachFromHost();
	Super::EndPlay(Reason);
}

#if WITH_EDITOR
void ACSHouseHandleActor::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);

	// 纪律 ③：spawn 之后 `GEditor->AddActor` 立刻补的那一次 `bFinished=true` 不是松手，
	// **降级成非最终裁决**（`HandleDrag(false)`）。
	//
	// ⚠️ **2026-09-06 一度改成"整个跳过"，那是个 bug，已订正回降级。** 当时给的理由是
	// "`AddActor` 先落位、后应用旋转，这一刻朝向还没摆正" —— **理由是错的**：
	// `UEditorEngine::AddActor` 把 Rotation 一起传给 `SpawnActor`
	// （`EditorEngine.cpp`：`World->SpawnActor(Class, &Location, &Rotation, SpawnInfo)`），
	// 走到这一行时朝向已经是对的。真正会拿默认 +X 去解析的是 `PostRegisterAllComponents`
	// （它在 `SpawnActor` **内部**就跑完了），那条另有 `RF_WasLoaded` 闸门挡着，与本函数无关。
	//
	// ⚠️ 而"跳过"会**直接砸掉视口拖放**：`AddActor` 之后不再来任何事件（拖放的落点就是终点，
	// 悬停期那个是另一个会被销毁的预览 actor）。跳掉它 = 标记永远不解析。实测把窗拖到房子上
	// 一扇都不出（`spawn 后立刻: windows=0 markers=0 host=None`），而无头回归因为紧跟着显式调
	// `ResolveHostAndRegister`，一路全绿 —— 所以下面那条"spawn 完什么都不调"的断言是必须的。
	//
	// **降级是安全的**：`bFinished=false` 这一路不自毁（纪律 ④）、**也不回写变换**
	// （`SnapToAnchor` 只在最终裁决里调）。没人回写变换，"误判自我固化"就不成立 ——
	// 万一这一刻解析错了，下一次事件照样能自愈。
	//
	// ⚠️ 只有**能被 `AddActor` 生成**的子类会走到这里；`ACSHouseResizeHandleActor` 是
	// `NotPlaceable` 且只由房子 `World->SpawnActor` 生成，**从来收不到这次合成事件**。
	const bool bSynthetic = bFinished && !bHasBeenPlaced;
	bHasBeenPlaced = true;

	// ⚠️ 2026-09-06 一度还配了一道 `IsPlacementResolve()`，让子类在这一次别信自己的 forward。
	// 窗标记改成 `NotPlaceable`（生成只走 `PlaceMarkerAlongRay`）之后，能走到这条合成事件的
	// 子类已经一个都没有了，那道闸随之拆除 —— 降级本身留着，它挡的是纪律 ③ 那半（自毁）。
	HandleDrag(bSynthetic ? false : bFinished);
}
#endif
