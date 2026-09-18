#include "CSHouseFeatureMarker.h"

#include "CSGpuInstancedMeshComponent.h"   // 门前踏步的砖（窗的门形态）
#include "CSGroundActor.h"                 // 门口采地面：踏步 / 栏杆的判据
#include "CSHouseLibrary.h"
#include "CSStairs.h"                      // BuildDoorSteps / ShouldAddDoorRails
#include "Materials/MaterialInterface.h"
#include "Components/SceneComponent.h"
#include "Components/BillboardComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
// ⚠️ `PickSprite` 那个 `FObjectFinderOptional<UTexture2D>` 要**完整类型**才能把
// `TObjectPtr<UTexture2D>` 退化成 `UObject*`。unity 构建里别的 TU 恰好带进来了，所以全量
// 构建一路绿 —— 而单文件 / Live Coding 编译当场 C2664（`ConstructorHelpers.h(133)`）。
// 这正是本仓库那条"unity 藏缺失 include"的老坑，2026-09-06 由 `-SingleFile` 抓到。
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogTinyGladeMarker, Log, All);

ACSHouseFeatureMarker::ACSHouseFeatureMarker()
{
	// 纪律 ②：编辑器 world 里 actor 默认不 tick。这里开 + override ShouldTickIfViewportsOnly，
	// 两者缺一整条"洞跟手"就静默失效。基类默认关着 tick（抓手多数是纯事件驱动的），
	// 本族要逐帧重解析宿主，所以在这里翻开。
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	// 附属物**自己的**可视网格，三件（2026-09-06 裁决：房子只出洞，物归这里）。
	//
	// ⚠️ **不要拿 `MakeEditorGizmoProp` 配它们** —— 那是给箭头锥那类编辑器道具用的，会置
	// `bIsEditorOnly` + `HiddenInGame`，游戏里窗框就整个没了。这里只关碰撞：宿主解析走
	// 解析求交不走 trace，网格参与碰撞对谁都没好处（TG 的窗也是另出一份 `_collision`）。
	//
	// ⚠️ **三件全部挂在根下，不要挂在 `OpeningMesh` 下** —— 挂它下面的话，将来谁给本体加一个
	// 缩放，过梁和窗台会跟着一起缩，而那两件在 TG 里本来就是独立摆位的 setdressing。
	//
	// ⚠️ 子对象名保留 `TEXT("Mesh")`：属性名从 `MeshComponent` 改成了 `OpeningMesh`，保住子对象名
	// 是为了让已经摆在关卡里的标记（如果有）还能认出这一件，不至于加载后拿到一个空指针。
	auto MakePiece = [this](UStaticMeshComponent*& Out, const TCHAR* Name)
	{
		Out = CreateDefaultSubobject<UStaticMeshComponent>(Name);
		Out->SetupAttachment(RootComponent);
		Out->SetMobility(EComponentMobility::Movable);
		Out->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	};
	UStaticMeshComponent* Piece = nullptr;
	MakePiece(Piece, TEXT("Mesh"));    OpeningMesh = Piece;
	MakePiece(Piece, TEXT("Lintel"));  LintelMesh = Piece;
	MakePiece(Piece, TEXT("Sill"));    SillMesh = Piece;
	MakePiece(Piece, TEXT("Glass"));   GlassMesh = Piece;

#if WITH_EDITORONLY_DATA
	// 编辑器拾取件：被拒时三件网格全藏，没有它就点不中这个 actor 了（见声明处）。
	// 配法与 `ACSGroundShaperActor` 的 sprite 逐字相同 —— 那六行没理由各写一遍。
	PickSprite = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("PickSprite"));
	if (PickSprite)
	{
		static ConstructorHelpers::FObjectFinderOptional<UTexture2D> SpriteTexture(TEXT("/Engine/EditorResources/S_Actor"));
		PickSprite->Sprite = SpriteTexture.Get();
		PickSprite->SetupAttachment(RootComponent);
		PickSprite->bIsScreenSizeScaled = true;
		PickSprite->SetHiddenInGame(true);
	}
#endif

	// 身份在构造期就定下来，而不是等第一次登记 —— 登记时才掷的话，"先 spawn 后摆位"这条
	// 常见路径上会出现"同一个标记两次登记拿到两个 SourceId"，谓词随之把它当成两扇窗。
	MarkerId = FGuid::NewGuid();
}

void ACSHouseFeatureMarker::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	if (IsTemplate()) return;

	// 先按自己这一档的网格重排四件：子蓝图换了 `OpeningMesh`（2x1 / gothic）之后，过梁该抬
	// 多高是跟着那一件走的，不重排就会沿用父类 CDO 里按 1x1 算出来的高度。
	// **纯几何、不打射线** —— 放在下面那道 `RF_WasLoaded` 闸之前是安全的。
	RefreshPieceLayout();

	// ⚠️ **本函数只服务「从存档加载」，spawn 出来的一概不碰。**
	//
	// `GEditor->AddActor`（拖入视口 / `spawn_actor_from_class` / Python spawn 全走它）
	// **先落位、后应用旋转**，而本函数在两者之间就会被调到 —— 那一刻朝向还是默认的 +X，
	// 在这里解析会打中背后完全不相干的房子（2026-09-06 实测：咬上 11 m 外的另一栋）。
	// 早先这条路是"先解析、错了下次再纠正"，因为没有任何东西会写 actor 的变换；
	// 加了按锚点吸附之后就不成立了：一旦把标记搬到误判的那面墙上，下一次解析的输入就是那个
	// 错值 —— **错误自我固化**。所以根子上不要在这里解析，而不是事后加一位标志去挡。
	//
	// spawn 的正确入口是 `PostEditMove`：`AddActor` 在应用完变换之后一定会补一次
	// （基类纪律 ③ 会把它降级成非最终裁决），那时朝向已经是对的。
	if (!HasAnyFlags(RF_WasLoaded)) return;

	// **加载：按锚点复原，一条射线都不打。** 宿主由 attach 关系给出（attach 是 UE 自己
	// 序列化的，不用我们再存一份宿主引用）。⚠️ 这里改用射线就是 2026-09-05 分析出的那个坑：
	// 重新射线是拿标记的世界位置去**覆盖**锚点，等于让洞去追标记 —— 症状是"重开关卡窗又挪一次位"。
	if (Anchor.IsValidAnchor())
	{
		if (ACSHouseActor* Attached = Cast<ACSHouseActor>(GetAttachParentActor()))
		{
			// 直接写 Host 而不走 SetHost：SetHost 会先 DetachFromHost，把我们正要用的那条
			// attach 关系拆掉（`OnDetachFromHost` 里有 `DetachFromActor`）。
			Host = Attached;
			RegisterAnchor(*Attached);
			SnapToAnchor();
			return;
		}
	}

	// 旧存档里没有锚点（本特性之前存的关卡）：此刻变换已经反序列化完整，射线回填是安全的。
	// `bFinal = false`：加载时序里房子未必已经注册，这一刻找不到宿主不该判死刑。
	HandleDrag(false);
}

void ACSHouseFeatureMarker::OnDetachFromHost()
{
	// 基类保证 `Destroyed` 与 `EndPlay` 两条路都会走到这里（编辑器 world 只发前者，
	// PIE 结束与关卡卸载只发后者），且这里必须幂等 —— 注销找不到就什么都不做。
	if (ACSHouseActor* Old = Host.Get())
	{
		Old->UnregisterFeatureMarker(MarkerId);
		// 场景图上也要断开。attach 是"整栋房子移动 / 旋转 / 落座抬升时窗跟着走"那条通路的
		// **全部**实现（一行同步代码都不用写），换宿主时不断开的话，窗会被旧房子继续拖着走。
		DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	}
	bCausesCut = false;
	SetMeshPiecesVisible(false);
}

void ACSHouseFeatureMarker::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// 拖拽期：每 tick 解析 + 登记，但**不判自毁**（基类纪律 ④）。洞跟手就是这一句。
	HandleDrag(false);

	// 兜底：漏掉 PostEditMove(bFinished=true) 时不至于永久 tick（计划 D8 明写）。
	const FVector Now = GetActorLocation();
	IdleSeconds = Now.Equals(LastTickLocation, 0.01) ? IdleSeconds + DeltaSeconds : 0.0f;
	LastTickLocation = Now;
	if (IdleSeconds >= DragIdleSeconds)
	{
		SetActorTickEnabled(false);
		IdleSeconds = 0.0f;
		HandleDrag(true);
	}
}

#if WITH_EDITOR
void ACSHouseFeatureMarker::PostEditMove(bool bFinished)
{
	// 本类在这里**只管 tick 开关**。降级（spawn 后那次假松手）、裁决、自毁时机全在基类，
	// 与拉尺寸抓手共用一份 —— 所以顺序是先开关 tick，再交给 Super 去裁决。
	if (!bFinished)
	{
		// 开 tick，并把静止计时清零 —— 不清的话第二次拖动会带着上一次的余额进来，
		// 有可能一动就立刻触发兜底收尾。
		IdleSeconds = 0.0f;
		LastTickLocation = GetActorLocation();
		SetActorTickEnabled(true);
	}
	else
	{
		SetActorTickEnabled(false);
		IdleSeconds = 0.0f;
	}

	Super::PostEditMove(bFinished);
}

void ACSHouseFeatureMarker::PostEditUndo()
{
	Super::PostEditUndo();
	if (IsTemplate()) return;

	// 撤销恢复的是 `Anchor`（UPROPERTY，`OnHandleDrag` 改它之前调过 `Modify()`）与 actor 变换，
	// **恢复不了**宿主那张登记表 —— 它是 transient 的派生物，事务系统根本不认识它。
	// 所以这里要按恢复出来的锚点重登记一次；少了这一步，撤销一次移动之后 actor 回到旧位置、
	// 墙上的洞却还留在新位置。
	if (ACSHouseActor* H = Host.Get())
	{
		if (Anchor.IsValidAnchor())
		{
			RegisterAnchor(*H);
			SnapToAnchor();
			return;
		}
	}
	// 宿主引用是 transient 的，撤销之后可能已经空了 —— 退回解析。此刻 actor 变换已被事务
	// 恢复到旧位置，所以射线给出的正是撤销前的那个锚点。
	HandleDrag(false);
}
#endif

void ACSHouseFeatureMarker::AdoptAnchor(ACSHouseActor* NewHost, const FCSWallAnchor& InAnchor)
{
	if (!NewHost || !InAnchor.IsValidAnchor()) return;

	// `Modify()` 让锚点进事务 —— 与 `OnHandleDrag` 同一条理由：撤销要能把它一起回滚。
	Modify();
	Anchor = InAnchor;

	// 换宿主纪律（先注销旧的再挂新的）归基类 `SetHost`；attach 之后整栋房子的平移/旋转/落座
	// 就由场景图带着窗一起走，一行同步代码都不用写。
	SetHost(NewHost);
	if (GetAttachParentActor() != NewHost)
	{
		AttachToActor(NewHost, FAttachmentTransformRules::KeepWorldTransform);
	}

	RegisterAnchor(*NewHost);

	// ⚠️ **无条件吸附**。点击就是放置，没有"拖到一半"这种中间态 —— 那条纪律（回写变换只在最终
	// 裁决时做）针对的是拖拽途中和 gizmo 抢方向盘，这里没有 gizmo 在动。不摆的话，被谓词拒掉的
	// 标记会留在生成时那个临时位姿上（相机跟前），用户根本找不到它。
	SnapToAnchor();
	if (bCausesCut) LastAcceptedAnchor = Anchor;
}

void ACSHouseFeatureMarker::SnapToAnchor()
{
	ACSHouseActor* H = Host.Get();
	if (!H || !Anchor.IsValidAnchor()) return;

	float DemandWidth = 0.0f, DemandHeight = 0.0f;
	GetDemandSize(DemandWidth, DemandHeight);

	// 锚点存的是**洞底**，标记本体锚在窗**心** ⇒ 抬半个窗高（口径与 MakeDemand 同源，
	// 两处写岔的症状是"窗整体偏高半扇"）。构建空间的合成归房子（三处必须同一个变换）。
	SetActorTransform(H->AnchorToWorld(Anchor, DemandHeight * 0.5f, WallStandoff));
	OnAnchorSnapped();
}

#if WITH_EDITOR
void ACSHouseFeatureMarker::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// 顺序要紧：先按新参数重排四件，再拿重排后的包围盒去登记 —— 反过来的话这一轮登记的
	// 还是旧尺寸，要等下一次事件才追上。
	RefreshPieceLayout();

	// `bFinal = false`：这只是改参数，不是"松手"。传 true 的话，一个暂时没宿主的标记
	// （比如刚从别的墙拖开）会在改一下滑块的时候把自己删掉。
	HandleDrag(false);
}
#endif

void ACSHouseFeatureMarker::RefreshPieceLayout()
{
	// 倍率写进**组件自己的相对缩放**，而不是 actor 缩放：`GetDemandSize` 量的正是
	// "网格包围盒 × 组件相对变换"，所以洞会自动跟着变，判定那一路一行都不用改。
	//
	// 资产的局部轴：+X 宽、+Y 朝外、+Z 高（组件带 +90° yaw 把它转到墙面上）。
	// 深度不缩 —— 窗框再宽也不该变厚，那只会让它从墙里穿出去。
	const FVector Scale(PieceScale.X, 1.0, PieceScale.Y);
	for (UStaticMeshComponent* Piece : { OpeningMesh.Get(), LintelMesh.Get(), SillMesh.Get(), GlassMesh.Get() })
	{
		if (Piece) Piece->SetRelativeScale3D(Scale);
	}

	// 子类多出来的件（窗的门形态）**排在下面那几个早退之前**：没挂过梁的窗也得把门摆对。
	RefreshExtraPieceLayout();

	// 过梁抬到框体顶上。**现算而不是写死 87.5**：换 2x1 / 3x1 / gothic 或者一动倍率，
	// 常数就错了 —— 而"过梁陷进框里"和"浮在半空"都不会有任何断言报红。
	if (!LintelMesh || !OpeningMesh) return;
	const UStaticMesh* Body = OpeningMesh->GetStaticMesh();
	const UStaticMesh* Lintel = LintelMesh->GetStaticMesh();
	if (!Body || !Lintel) return;

	// 两件都量 actor 空间下的 Z 半高（`TransformBy` 把 yaw 与倍率一并算进去）。
	const double BodyHalf = Body->GetBounds().TransformBy(OpeningMesh->GetRelativeTransform()).BoxExtent.Z;
	const double LintelHalf = Lintel->GetBounds().TransformBy(LintelMesh->GetRelativeTransform()).BoxExtent.Z;
	LintelMesh->SetRelativeLocation(FVector(0.0, 0.0, BodyHalf + LintelHalf));
}

void ACSHouseFeatureMarker::SetMeshPiecesVisible(bool bVisible)
{
	// ⚠️ **按类型藏，不走根组件的传播**：传播会把编辑器拾取件（`PickSprite`）一起藏掉，
	// 而它存在的唯一目的就是"被拒之后还点得中"。按类型还顺带覆盖子蓝图里新加的网格件 ——
	// "加了第四件忘了藏"是个不会报错的坑。
	TInlineComponentArray<UStaticMeshComponent*> Pieces;
	GetComponents(Pieces);
	for (UStaticMeshComponent* Piece : Pieces)
	{
		// 形态不对的那一组照藏：窗贴墙脚变成门之后，窗框 / 窗台 / 玻璃不能还挂在门上。
		if (Piece) Piece->SetVisibility(bVisible && IsPieceInCurrentForm(Piece));
	}
	OnMeshPiecesVisibilityChanged(bVisible);
}

FCSWallAnchor ACSHouseFeatureMarker::MakeAnchorFromHit(const FCSWallHit& Hit, const ACSHouseActor& InHost) const
{
	// 命中点是本体**心**的高度，而锚点存的是洞底 ⇒ 减半个洞高。不减的症状是"窗整体偏高半扇"，
	// 而且贴着檐口拖的时候会莫名其妙判 `AboveEave`。夹到 0 以上，负数由谓词判 `SillTooLow`。
	const float SillZ = FMath::Max(0.0f, Hit.Z - GetDemandHalfHeight());
	return CSHouse_MakeWallAnchor(Hit, InHost.GetFootprint(), InHost.WallThickness, SillZ);
}

void ACSHouseFeatureMarker::ApplyHostVerdict(ECSFeatureReject Reason)
{
	LastReject = Reason;
	bCausesCut = (Reason == ECSFeatureReject::None);

	// 被拒 ⇒ 藏网格。对位 TG 的 `validate_blueprints`：被拒的蓝图**不实例化**，存储里还留着
	// `[PDB]`。不藏的症状很具体：一块窗框贴在一面没有洞的实墙上。
	SetMeshPiecesVisible(bCausesCut);
}

void ACSHouseFeatureMarker::RegisterAnchor(ACSHouseActor& InHost)
{
	if (!Anchor.IsValidAnchor()) return;

	const FCSHouseWindow Demand = MakeDemand(Anchor, InHost);

	// **裁决走房子的谓词，标记不自己判**：谓词是唯一真源（计划 D8「剖面求值器必须是唯一真源」），
	// 这里再写一份"看起来差不多"的判据，就会出现"标记说能放、房子砌不出"。
	FCSWallOpening Probe;
	Probe.Type = ECSOpeningType::Window;
	Probe.Shape = Demand.Shape;
	Probe.EdgeIndex = Demand.EdgeIndex;
	Probe.CenterS = Demand.CenterS;
	Probe.Width = Demand.Width;
	Probe.Z0 = Demand.SillZ;
	Probe.Z1 = Demand.SillZ + Demand.Height;
	// 与 `BuildWindowOpenings` 同一份字段：探针漏带它的话，门形态在这里判 `SillTooLow`、
	// 房子重求值时却放行 —— 回执与画面互相矛盾，直到下一次重建才被盖掉。
	Probe.bDoorForm = Demand.bDoorForm;
	Probe.SourceId = MarkerId;
	const ECSFeatureReject Reason = InHost.QueryFeatureReject(Probe);

	// ⚠️ **被拒也照样登记**（计划 D8：「被拒的诉求留在列表里、只是这一轮不出洞」）。
	// 拒了就撤登记的话，"把窗从被门拱占住的墙拖到隔壁墙"会先把它删掉，再也回不来。
	InHost.RegisterFeatureMarker(MarkerId, Demand, this);

	// 先在这里回写一次：`RegisterFeatureMarker` 在诉求没变时会早退，那一路不会重求值，
	// 房子也就不会推裁决过来。真重建了的话，`NotifyMarkersRebuilt` 会拿最终结果再盖一次。
	ApplyHostVerdict(Reason);
}

bool ACSHouseFeatureMarker::OnHandleDrag(bool bFinal)
{
	const UWorld* HouseWorld = GetWorld();
	if (!HouseWorld) return false;

	const FVector Origin = GetActorLocation();

	// ① 射线：沿自身 +X（面向墙的方向）。解析求交，不是引擎 trace —— 纪律 ①。
	//
	// ⚠️ 这里**无条件**打射线。2026-09-06 曾为"放置那一次朝向还没应用"加过一道
	// `IsPlacementResolve()` 闸门；`NotPlaceable` 之后**根本不存在放置那一次**了（生成走
	// `PlaceMarkerAlongRay`，它自带命中、不经过本函数），闸门随之作废。本函数现在只服务
	// **gizmo 拖动**——那时 actor 早已构造完整，朝向可信。
	FCSWallHit Hit;
	ACSHouseActor* Found = UCSHouseLibrary::PickHouse(HouseWorld, Origin, GetActorForwardVector(), HostProbeDistance, Hit);

	// ② 落空了再就近找一次：兜"贴着墙但朝向没摆正"。两条都空才算无宿主。
	//    ⚠️ 这一条同时是**吸附之后的退路**：`SnapToAnchor` 把标记摆在外皮外 `WallStandoff` 处，
	//    `WallStandoff = 0` 时射线的 `Dist` 恰好是 0、被 `Dist <= 0` 挡掉，只剩就近版能咬住。
	if (!Found && SnapDistance > 0.0f)
	{
		Found = UCSHouseLibrary::PickHouseNear(HouseWorld, Origin, SnapDistance, Hit);
	}

	if (!Found)
	{
		DetachFromHost();
		LastReject = ECSFeatureReject::NotOnWall;
		// 返回 false ⇒ 基类在最终裁决那一刻按 `bDestroyWhenHostless` 自毁（基类纪律 ④）。
		// 自毁不在这里做：拖拽途中判自毁是两族共同的坑，归基类统一挡。
		if (bFinal && bDestroyWhenHostless && !IsTemplate())
		{
			UE_LOG(LogTinyGladeMarker, Verbose,
				TEXT("[TinyGladeMarker] %s 找不到宿主房，按 D8 裁决自毁。"), *GetName());
		}
		return false;
	}

	// 换宿主的"先注销再挂新的"纪律由基类 `SetHost` 保证（顺序反了会在两房相邻时留下一份
	// 永远没人来收的重复登记）。
	SetHost(Found);
	// attach 到宿主：整栋房子移动 / 旋转 / 落座抬升从此由场景图带着走，零同步代码。
	// footprint / 墙高的变化不在场景图里，那条走 `ACSHouseActor::NotifyMarkersRebuilt`。
	if (GetAttachParentActor() != Found)
	{
		AttachToActor(Found, FAttachmentTransformRules::KeepWorldTransform);
	}

	// **世界位置 → 锚点**（拖 gizmo 的方向）。反方向（锚点 → 世界）是 `SnapToAnchor`，
	// 两条路千万别混：房子变了的时候走反方向，重新射线就是让洞去追标记。
	// 口径（洞底 = 命中 Z − 半高、窗贴墙脚变门）在 `MakeAnchorFromHit` 一处，笔刷落笔也走它。
	// ⚠️ **先算、后写**：子类拿当前 `Anchor` 做窗 ↔ 门的迟滞，先改了锚点迟滞就没有参照了。
	const FCSWallAnchor Resolved = MakeAnchorFromHit(Hit, *Found);

	// `Modify()` 让锚点进事务 —— 撤销要能把它一起回滚，否则 `PostEditUndo` 拿到的是新锚点。
	Modify();
	Anchor = Resolved;

	RegisterAnchor(*Found);

	if (bFinal)
	{
		if (bCausesCut)
		{
			LastAcceptedAnchor = Anchor;
		}
		else if (LastAcceptedAnchor.IsValidAnchor() && LastAcceptedAnchor != Anchor)
		{
			// **松手时被拒 ⇒ 弹回最后一个被答应的位置**（计划 D8「回位规则」，对位 TG 的
			// `backup::DecoratorBackup` / `create_or_restore_decorator_backup` `[PDB]`）。
			// 从未被接受过的不回退：那种标记保持游离，网格已由 `ApplyHostVerdict` 藏起来。
			Anchor = LastAcceptedAnchor;
			RegisterAnchor(*Found);
		}
		// 有落脚点才吸附。**吸附只在松手时做**：拖拽中程序回写变换会和 gizmo 抢方向盘，
		// 画面上抖成一团 —— 与 D5 拉尺寸抓手同一条教训。
		if (bCausesCut) SnapToAnchor();
	}
	return true;
}

FCSHouseWindow ACSWindowMarker::MakeDemand(const FCSWallAnchor& InAnchor, const ACSHouseActor& InHost) const
{
	// 形态读**入参锚点**而不是成员 `Anchor`：诉求必须是入参的函数（两者此刻恰好相同，但别依赖它）。
	const bool bDoor = InAnchor.bDoorForm && CanBecomeDoor();
	float DemandWidth = 0.0f, DemandHeight = 0.0f;
	GetFormSize(bDoor, DemandWidth, DemandHeight);

	FCSHouseWindow Out;
	Out.EdgeIndex = InAnchor.EdgeIndex;
	// **诉求的弧长从锚点现算**，不是存下来的 —— 房子一改尺寸，同一个锚点算出来的就是新墙上
	// 的新弧长。这正是"推第 e 条边，e+1 那面墙没动窗却滑了 Δ"那个 bug 的修法。
	Out.CenterS = CSHouse_AnchorS(InAnchor, InHost.GetFootprint(), InHost.WallThickness);
	if (bDoor)
	{
		// TG 的门只要**门顶低于墙顶**就合法（`cull_oob_decorators`，附录 E §6.2），本项目的谓词还要求洞顶
		// 让出 `LintelBand`。门本身放得下时把**洞**夹到过梁带之下、网格不动 —— 门框顶压进过梁带几厘米，
		// 框的翻边本来就凸在墙面外。不夹的话默认档（262.5 高的门 vs 300 − 40 = 260）差 2.5 cm，
		// 默认房子上一扇门都放不下。门比墙还高就不夹，照旧由谓词判 `AboveEave`（= TG 剔除）。
		const float Usable = InHost.WallHeight - InHost.LintelBand;
		if (DemandHeight < InHost.WallHeight && DemandHeight > Usable) DemandHeight = FMath::Max(Usable, 1.0f);
	}

	Out.Width = DemandWidth;
	Out.Height = DemandHeight;
	Out.Shape = Shape;
	// 门恒落地。锚点在门形态下本来就写 0（`MakeAnchorFromHit`），这里再钉一次是防"锚点说门、
	// 高度却不是 0"的手改存档 —— 那种门会悬在半空、却被谓词当门放行。
	Out.SillZ = bDoor ? 0.0f : InAnchor.SillZ;
	Out.bDoorForm = bDoor;
	return Out;
}

bool ACSWindowMarker::CanBecomeDoor() const
{
	return bCanBecomeDoor && DoorMesh && DoorMesh->GetStaticMesh() != nullptr;
}

FCSWallAnchor ACSWindowMarker::MakeAnchorFromHit(const FCSWallHit& Hit, const ACSHouseActor& InHost) const
{
	float WindowWidth = 0.0f, WindowHeight = 0.0f;
	GetFormSize(false, WindowWidth, WindowHeight);

	bool bDoor = false;
	if (CanBecomeDoor())
	{
		float DoorWidth = 0.0f, DoorHeight = 0.0f;
		GetFormSize(true, DoorWidth, DoorHeight);
		// 阈值取 `max(DoorSnapHeight, WindowMinSillZ)`：TG 是 0（窗底低于墙脚才变门），而本项目的房子会把
		// 窗台低于 `WindowMinSillZ` 的窗直接拒掉 —— 不取 max，这一段拖下去窗先消失、再往下才变门。
		const float Threshold = FMath::Max(DoorSnapHeight, InHost.WindowMinSillZ);
		// 迟滞的参照是**当前**形态（`IsDoorForm` 读成员锚点）——所以基类要求"先调本函数、再写锚点"。
		bDoor = CSHouse_ResolveDoorForm(Hit.Z, WindowHeight, DoorHeight, Threshold, IsDoorForm());
	}

	// 门：洞底吸到墙脚（TG `is_bottom_door`）。窗：命中点是窗心 ⇒ 洞底 = 命中 Z − 半个窗高。
	const float SillZ = bDoor ? 0.0f : FMath::Max(0.0f, Hit.Z - WindowHeight * 0.5f);
	FCSWallAnchor Out = CSHouse_MakeWallAnchor(Hit, InHost.GetFootprint(), InHost.WallThickness, SillZ);
	Out.bDoorForm = bDoor;
	return Out;
}

bool ACSWindowMarker::IsPieceInCurrentForm(const UStaticMeshComponent* Piece) const
{
	// 门那一组：四个具名件，外加子蓝图里打了 `DoorForm` 标签的件。其余一律算窗那一组 ——
	// 子蓝图里新加、没打标签的件（花箱之类）是照着窗设计的，挂到门上不对（TG 的门同样不出过梁、
	// 窗台、花槽，附录 E §5.2）。
	static const FName DoorFormTag(TEXT("DoorForm"));
	const bool bDoorPiece = Piece == DoorMesh || Piece == DoorHatMesh || Piece == DoorBellMesh || Piece == DoorKransMesh
		|| Piece == DoorRailsMesh || (Piece && Piece->ComponentHasTag(DoorFormTag));
	if (bDoorPiece != IsDoorForm()) return false;
	// 门铃 / 花环四选一（TG `add_door_autoclutter`）：同一扇门恒定，拖来拖去不换。
	if (Piece == DoorBellMesh) return GetDoorClutterChoice() == 0;
	if (Piece == DoorKransMesh) return GetDoorClutterChoice() == 1;
	// 栏杆看上一次吸附时判出来的结果（要采地面，见 `RefreshDoorDressing`）。
	if (Piece == DoorRailsMesh) return bWantDoorRails;
	return true;
}

void ACSWindowMarker::OnAnchorSnapped()
{
	RefreshDoorDressing();
}

void ACSWindowMarker::OnMeshPiecesVisibilityChanged(bool bVisible)
{
	if (DoorStepBricks) DoorStepBricks->SetVisibility(bVisible && IsDoorForm() && DoorStepBrickCount > 0);
}

void ACSWindowMarker::RefreshDoorDressing()
{
	const ACSHouseActor* H = Host.Get();
	TArray<CSStairs::FBrick> Bricks;
	bool bSteps = false;
	bWantDoorRails = false;

	if (IsDoorForm() && bCausesCut && H && Anchor.IsValidAnchor() && IsValid(H->Ground))
	{
		const ACSGroundActor* G = H->Ground;
		const CSStairs::FGroundSampler Sampler = [G](const FVector2D& XY) { return G->SampleHeight(XY); };

		float DoorWidth = 0.0f, DoorHeight = 0.0f;
		GetFormSize(true, DoorWidth, DoorHeight);

		// 门心取在**墙厚中线**上：派生变换站在外皮外 `WallStandoff` 处、+X 朝墙内，沿 +X 走回
		// `WallStandoff + T/2` 就是中线。TG 的第一排踏步中心离门心 28 cm、进深 38（N = 2），
		// 按外皮算的话第一排和墙之间会留 9 cm 的缝，按中线算正好顶进墙里 3 cm。
		const FVector Forward = GetActorForwardVector();
		const FVector Location = GetActorLocation();
		const FVector Mid = Location + Forward * double(WallStandoff + H->WallThickness * 0.5f);

		CSStairs::FDoorStepsInput Door;
		Door.DoorXY = FVector2D(Mid.X, Mid.Y);
		Door.Outward = FVector2D(-Forward.X, -Forward.Y);
		Door.DoorBottomZ = float(Location.Z) - DoorHeight * 0.5f;
		Door.Width = DoorWidth;

		if (bDoorSteps && DoorStepBrickMesh)
		{
			bSteps = CSStairs::BuildDoorSteps(Door, CSStairs::FDoorStepsParams(), Sampler, GetTypeHash(MarkerId), Bricks);
		}
		// 栏杆：墙脚（= 门洞底）比门下地面高出 15 cm、又没出踏步（TG 两者二选一兜底）。
		if (bDoorRails && DoorRailsMesh && DoorRailsMesh->GetStaticMesh())
		{
			bWantDoorRails = CSStairs::ShouldAddDoorRails(Door.DoorBottomZ, Sampler(Door.DoorXY), bSteps);
		}
	}

	// 砖 → 实例。按砖网格自己的包围盒换算（同 `ACSStairsActor::RebuildStairs`）。
	TArray<FTransform> Transforms;
	if (DoorStepBrickMesh && !Bricks.IsEmpty())
	{
		const FBox MeshBox = DoorStepBrickMesh->GetBoundingBox();
		const FVector MeshSize = MeshBox.GetSize().ComponentMax(FVector(UE_KINDA_SMALL_NUMBER));
		const FVector MeshCenter = MeshBox.GetCenter();
		Transforms.Reserve(Bricks.Num());
		for (const CSStairs::FBrick& Brick : Bricks)
		{
			const FVector Scale = Brick.Size / MeshSize;
			Transforms.Add(FTransform(Brick.Rotation, Brick.Center - Brick.Rotation.RotateVector(MeshCenter * Scale), Scale));
		}
	}
	DoorStepBrickCount = Transforms.Num();

	if (DoorStepBricks)
	{
		// 幂等短路：砖表 + 网格 / 材质身份 + 组件变换（实例存的是组件局部量）。
		TArray<int32> HashInput;
		auto Q = [](double V) { return int32(FMath::RoundToDouble(V * 10.0)); };
		HashInput.Append({ int32(GetTypeHash(DoorStepBrickMesh.Get())), int32(GetTypeHash(DoorStepMaterial.Get())), Transforms.Num() });
		const FTransform ComponentTransform = DoorStepBricks->GetComponentTransform();
		for (const FVector& V : { ComponentTransform.GetLocation(), ComponentTransform.GetRotation().Euler() }) HashInput.Append({ Q(V.X), Q(V.Y), Q(V.Z) });
		for (const FTransform& T : Transforms)
		{
			const FVector L = T.GetLocation();
			const FVector S = T.GetScale3D();
			const FQuat R = T.GetRotation();
			HashInput.Append({ Q(L.X), Q(L.Y), Q(L.Z), Q(S.X * 100.0), Q(S.Y * 100.0), Q(S.Z * 100.0), Q(R.Z * 1000.0), Q(R.W * 1000.0) });
		}
		const uint32 NewHash = FCrc::MemCrc32(HashInput.GetData(), HashInput.Num() * sizeof(int32));
		if (NewHash != DoorStepHash || DoorStepBricks->GetInstanceCount() != Transforms.Num())
		{
			DoorStepHash = NewHash;
			if (Transforms.IsEmpty())
			{
				// 没有砖就别碰网格：每扇窗都带着这个组件，不出踏步的那些连基础网格快照都不该建。
				if (DoorStepBricks->GetInstanceCount() > 0) DoorStepBricks->ClearInstances();
			}
			else
			{
				DoorStepBricks->SetInstanceMaterial(DoorStepMaterial);
				DoorStepBricks->SetBaseMesh(DoorStepBrickMesh);
				DoorStepBricks->SetInstances(Transforms, /*bWorldSpace*/ true);
			}
		}
	}

	// 栏杆与踏步的显隐跟着这一轮的判据重算（门铃 / 花环 / 门扇不受影响，重设一遍是幂等的）。
	SetMeshPiecesVisible(bCausesCut);
}

void ACSWindowMarker::RefreshExtraPieceLayout()
{
	if (!DoorMesh) return;

	// 与窗的四件同一个倍率（一扇缩小了的窗，变成门也该是缩小了的门）；进深不缩，理由同 `RefreshPieceLayout`。
	const FVector Scale(PieceScale.X, 1.0, PieceScale.Y);
	DoorMesh->SetRelativeScale3D(Scale);
	if (DoorHatMesh) DoorHatMesh->SetRelativeScale3D(Scale);

	// **把门网格的包围盒中心压到 actor 原点的 Y/Z 上**（原点 = 洞心，见 `SnapToAnchor`）。
	// 窗框资产的枢轴恰好居中所以窗那几件零偏移；门资产的枢轴不保证 —— 落地件常把枢轴放在底边，
	// 照搬零偏移的话门整体高出洞半扇，而谓词、洞、砖数全都是对的，一条断言都不会红。
	// 进深（X）保留资产自己的枢轴，与窗的几件同一口径（贴墙的深度由资产作者定）。
	FVector Offset = FVector::ZeroVector;
	if (const UStaticMesh* Mesh = DoorMesh->GetStaticMesh())
	{
		const FTransform NoMove(DoorMesh->GetRelativeRotation(), FVector::ZeroVector, Scale);
		const FVector Center = Mesh->GetBounds().TransformBy(NoMove).Origin;
		Offset = FVector(0.0, -Center.Y, -Center.Z);
	}
	DoorMesh->SetRelativeLocation(Offset);
	// 帽子跟门**同一个平移**：TG 的 `*_hat` 是在门的局部空间里预摆好的，单独居中就会和门错开。
	if (DoorHatMesh) DoorHatMesh->SetRelativeLocation(Offset);

	// 门铃 / 花环：TG 在**门的局部**摆（原点 = 门的资产原点，X 沿墙、Y 上、Z 朝外，附录 E §7）。
	// 换到 UE 资产轴（+X 宽、+Y 朝外、+Z 上）再转上墙面；偏移跟着倍率走，挂件本身不缩（门铃拉扁了不像门铃）。
	const FQuat FacingQuat = DoorMesh->GetRelativeRotation().Quaternion();
	auto PlaceClutter = [&](UStaticMeshComponent* Piece, const FVector& TGLocal)
	{
		if (!Piece) return;
		const FVector AssetLocal(TGLocal.X * Scale.X, TGLocal.Z, TGLocal.Y * Scale.Z);
		Piece->SetRelativeLocation(Offset + FacingQuat.RotateVector(AssetLocal));
	};
	PlaceClutter(DoorBellMesh, FVector(-25.0, 74.0, 10.0));
	PlaceClutter(DoorKransMesh, FVector(0.0, 50.0, 20.0));

	// 栏杆：资产原点即门心（附录 E §3.5），与门同倍率、同平移。
	if (DoorRailsMesh)
	{
		DoorRailsMesh->SetRelativeScale3D(Scale);
		DoorRailsMesh->SetRelativeLocation(Offset);
	}
}

void ACSWindowMarker::GetFormSize(bool bDoorForm, float& OutWidth, float& OutHeight) const
{
	if (bDoorForm)
	{
		OutWidth = 0.0f;
		OutHeight = 0.0f;
		// 门形态只认 `DoorMesh`（不算帽子，理由同窗不算过梁）。没挂网格 ⇒ 零尺寸 ⇒ `CanBecomeDoor`
		// 本来就是 false，调用方不会走到这里来要一扇不存在的门。
		const UStaticMesh* Mesh = DoorMesh ? DoorMesh->GetStaticMesh() : nullptr;
		if (!Mesh) return;
		const FBoxSphereBounds B = Mesh->GetBounds().TransformBy(DoorMesh->GetRelativeTransform());
		// 洞比门框每侧窄 `DoorHoleInset`（TG 的洞取碰撞网格，比渲染网格窄 —— 框的翻边压住洞缘）。
		OutWidth = FMath::Max(float(B.BoxExtent.Y) * 2.0f - 2.0f * DoorHoleInset * float(PieceScale.X), 1.0f);
		OutHeight = float(B.BoxExtent.Z) * 2.0f;
		return;
	}

	OutWidth = Width;
	OutHeight = Height;
	// ⚠️ **只读 `OpeningMesh`，不碰过梁与窗台。** 这条纪律的理由写在 `OpeningMesh` 的声明处：
	// 把盖顶件算进来，洞会悄悄变宽而画面上被它自己盖住，一条断言都不会红。
	if (!bAutoSizeFromMesh || !OpeningMesh) return;

	const UStaticMesh* Mesh = OpeningMesh->GetStaticMesh();
	// **没挂网格就退回手填值**，不要产出零尺寸：谓词对零宽洞只会淡淡地说一句 `Degenerate`，
	// 而画面上"窗没了"与"窗被门挤掉了"长得一模一样。
	// （三件都是"组件恒在、网格可空"：空槽就是这一件不存在，不另设开关。）
	if (!Mesh) return;

	// 洞开在墙面上 ⇒ 量的是 actor 局部的 **YZ 平面**（+X 是墙的内法线，见 `CSHouse_AnchorToLocal`）。
	// 网格自己的相对变换要算进来 —— 资产的朝向/缩放全靠它调。
	const FBoxSphereBounds B = Mesh->GetBounds().TransformBy(OpeningMesh->GetRelativeTransform());
	const float MeshWidth = float(B.BoxExtent.Y) * 2.0f;
	const float MeshHeight = float(B.BoxExtent.Z) * 2.0f;
	if (MeshWidth > 1.0f) OutWidth = MeshWidth;
	if (MeshHeight > 1.0f) OutHeight = MeshHeight;
}

const TCHAR* ACSWindowMarker::DefaultFrameMeshPath =
	TEXT("/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/decorators_window_cottage_1x1");

const TCHAR* ACSWindowMarker::DefaultLintelMeshPath =
	TEXT("/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/setdressing_window_lintel");

const TCHAR* ACSWindowMarker::DefaultSillMeshPath =
	TEXT("/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/setdressing_window_sill");

const TCHAR* ACSWindowMarker::DefaultGlassMeshPath =
	TEXT("/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/window_cottage_1x1_glass");

const TCHAR* ACSWindowMarker::DefaultDoorMeshPath =
	TEXT("/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/balcony_door_rank1");

const TCHAR* ACSWindowMarker::DefaultDoorBellMeshPath =
	TEXT("/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/door_bell");

const TCHAR* ACSWindowMarker::DefaultDoorKransMeshPath =
	TEXT("/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/door_krans");

const TCHAR* ACSWindowMarker::DefaultDoorRailsMeshPath =
	TEXT("/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/balcony_door_rank1_rails");

const TCHAR* ACSWindowMarker::DefaultBrickMeshPath =
	TEXT("/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/brick");

ACSWindowMarker::ACSWindowMarker()
{
	// 默认挂上 TG 的窗框板 —— 没有它标记在视口里既看不见也点不中，而"选中窗户就能拖"正是
	// 2026-09-06 裁决要的东西。`FObjectFinderOptional` 而不是 `FObjectFinder`：找不到只是没有
	// 默认网格（细节面板照样能指），**不会把 CDO 构造失败带崩整个模块** —— 本仓库 Content
	// 处于 LFS 混合状态，资产缺失是要当常态处理的。
	static ConstructorHelpers::FObjectFinderOptional<UStaticMesh> FrameMesh(DefaultFrameMeshPath);
	static ConstructorHelpers::FObjectFinderOptional<UStaticMesh> LintelAsset(DefaultLintelMeshPath);
	static ConstructorHelpers::FObjectFinderOptional<UStaticMesh> SillAsset(DefaultSillMeshPath);
	static ConstructorHelpers::FObjectFinderOptional<UStaticMesh> GlassAsset(DefaultGlassMeshPath);
	if (UStaticMesh* Mesh = FrameMesh.Get())  OpeningMesh->SetStaticMesh(Mesh);
	if (UStaticMesh* Mesh = LintelAsset.Get()) LintelMesh->SetStaticMesh(Mesh);
	if (UStaticMesh* Mesh = SillAsset.Get())   SillMesh->SetStaticMesh(Mesh);
	if (UStaticMesh* Mesh = GlassAsset.Get())  GlassMesh->SetStaticMesh(Mesh);

	// **+90° yaw**：资产的 +X 是宽、+Y 朝外，而本类约定 +X 是墙的**内**法线 ⇒
	// 资产 +X → actor ∓Y（宽度沿墙），资产 +Y → actor −X（朝外）。三件用同一个朝向，
	// 因为窗台 / 过梁本来就是在窗的局部空间里做的。逐条理由（含"+Y 朝外"怎么量出来的）
	// 写在 `DefaultFrameMeshPath` 的注释里。
	const FRotator Facing(0.0f, 90.0f, 0.0f);
	OpeningMesh->SetRelativeRotation(Facing);
	LintelMesh->SetRelativeRotation(Facing);
	SillMesh->SetRelativeRotation(Facing);
	// 玻璃与框体同一个朝向、同一个原点 —— 它就是嵌在框里的那一层（78 × 1.7 × 160）。
	GlassMesh->SetRelativeRotation(Facing);

	// 门形态的四件（2026-09-16）。配法与基类的四件逐项相同（挂根下、可动、无碰撞）；
	// 子对象名一经发布就别改 —— 已经存进关卡 / 蓝图的标记靠它认出这一件。
	const TPair<TObjectPtr<UStaticMeshComponent>*, const TCHAR*> DoorPieces[] = {
		{ &DoorMesh, TEXT("DoorMesh") }, { &DoorHatMesh, TEXT("DoorHat") },
		{ &DoorBellMesh, TEXT("DoorBell") }, { &DoorKransMesh, TEXT("DoorKrans") },
		{ &DoorRailsMesh, TEXT("DoorRails") } };
	for (const TPair<TObjectPtr<UStaticMeshComponent>*, const TCHAR*>& Entry : DoorPieces)
	{
		TObjectPtr<UStaticMeshComponent>* Slot = Entry.Key;
		UStaticMeshComponent* Piece = CreateDefaultSubobject<UStaticMeshComponent>(Entry.Value);
		Piece->SetupAttachment(RootComponent);
		Piece->SetMobility(EComponentMobility::Movable);
		Piece->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		// 与窗那几件同一个朝向：TG 的门资产与窗资产同一套局部轴（+X 宽、+Y 朝外）。
		Piece->SetRelativeRotation(Facing);
		// 出生时是窗 ⇒ 门那一组先藏着。之后的显隐一律由 `ApplyHostVerdict` 按形态重算。
		Piece->SetVisibility(false);
		*Slot = Piece;
	}
	static ConstructorHelpers::FObjectFinderOptional<UStaticMesh> DoorAsset(DefaultDoorMeshPath);
	static ConstructorHelpers::FObjectFinderOptional<UStaticMesh> DoorBellAsset(DefaultDoorBellMeshPath);
	static ConstructorHelpers::FObjectFinderOptional<UStaticMesh> DoorKransAsset(DefaultDoorKransMeshPath);
	static ConstructorHelpers::FObjectFinderOptional<UStaticMesh> DoorRailsAsset(DefaultDoorRailsMeshPath);
	static ConstructorHelpers::FObjectFinderOptional<UStaticMesh> BrickAsset(DefaultBrickMeshPath);
	if (UStaticMesh* Mesh = DoorAsset.Get()) DoorMesh->SetStaticMesh(Mesh);
	if (UStaticMesh* Mesh = DoorBellAsset.Get()) DoorBellMesh->SetStaticMesh(Mesh);
	if (UStaticMesh* Mesh = DoorKransAsset.Get()) DoorKransMesh->SetStaticMesh(Mesh);
	if (UStaticMesh* Mesh = DoorRailsAsset.Get()) DoorRailsMesh->SetStaticMesh(Mesh);
	DoorStepBrickMesh = BrickAsset.Get();

	// 门前踏步的实例组件：**不在构造里设基础网格**，等真要出踏步时才设（`RefreshDoorDressing`）——
	// 每扇窗都带着它，绝大多数一辈子不出踏步，没理由每扇都建一份基础网格快照。
	DoorStepBricks = CreateDefaultSubobject<UCSGpuInstancedMeshComponent>(TEXT("DoorStepBricks"));
	DoorStepBricks->SetupAttachment(RootComponent);
	DoorStepBricks->SetVisibility(false);

	// 窗台**零偏移**：`setdressing_window_sill` 的 origin 是 (0, +28, −76)，已经在窗的局部空间里
	// 预摆好了（−76 正落在 160 高窗的下沿，+28 是朝外挑的鼻子）。
	// 过梁的 origin 居中 ⇒ 要自己抬到窗顶，抬多少由 `RefreshPieceLayout` 现算。
	RefreshPieceLayout();
}

void ACSWindowMarker::GetDemandSize(float& OutWidth, float& OutHeight) const
{
	// **当前**形态的洞尺寸。`SnapToAnchor` / `GetDemandHalfHeight` 都经这里，所以门形态的标记
	// 自动按门高摆到墙脚上，基类一行不用改。
	GetFormSize(IsDoorForm(), OutWidth, OutHeight);
}
