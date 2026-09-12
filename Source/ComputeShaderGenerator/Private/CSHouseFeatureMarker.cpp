#include "CSHouseFeatureMarker.h"

#include "CSHouseSubsystem.h"
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

UCSHouseSubsystem* ACSHouseFeatureMarker::GetHouseSubsystem() const
{
	const UWorld* World = GetWorld();
	return World ? World->GetSubsystem<UCSHouseSubsystem>() : nullptr;
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
		if (Piece) Piece->SetVisibility(bVisible);
	}
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
	UCSHouseSubsystem* Subsystem = GetHouseSubsystem();
	if (!Subsystem) return false;

	const FVector Origin = GetActorLocation();

	// ① 射线：沿自身 +X（面向墙的方向）。解析求交，不是引擎 trace —— 纪律 ①。
	//
	// ⚠️ 这里**无条件**打射线。2026-09-06 曾为"放置那一次朝向还没应用"加过一道
	// `IsPlacementResolve()` 闸门；`NotPlaceable` 之后**根本不存在放置那一次**了（生成走
	// `PlaceMarkerAlongRay`，它自带命中、不经过本函数），闸门随之作废。本函数现在只服务
	// **gizmo 拖动**——那时 actor 早已构造完整，朝向可信。
	FCSWallHit Hit;
	ACSHouseActor* Found = Subsystem->PickHouse(Origin, GetActorForwardVector(), HostProbeDistance, Hit);

	// ② 落空了再就近找一次：兜"贴着墙但朝向没摆正"。两条都空才算无宿主。
	//    ⚠️ 这一条同时是**吸附之后的退路**：`SnapToAnchor` 把标记摆在外皮外 `WallStandoff` 处，
	//    `WallStandoff = 0` 时射线的 `Dist` 恰好是 0、被 `Dist <= 0` 挡掉，只剩就近版能咬住。
	if (!Found && SnapDistance > 0.0f)
	{
		Found = Subsystem->PickHouseNear(Origin, SnapDistance, Hit);
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
	float DemandWidth = 0.0f, DemandHeight = 0.0f;
	GetDemandSize(DemandWidth, DemandHeight);
	// 命中点是窗**心**的高度，而锚点存的是洞底 ⇒ 减半个窗高。不减的症状是"窗整体偏高半扇"，
	// 而且贴着檐口拖的时候会莫名其妙判 `AboveEave`。夹到 0 以上，负数由谓词判 `SillTooLow`。
	const float SillZ = FMath::Max(0.0f, Hit.Z - DemandHeight * 0.5f);

	// `Modify()` 让锚点进事务 —— 撤销要能把它一起回滚，否则 `PostEditUndo` 拿到的是新锚点。
	Modify();
	Anchor = CSHouse_MakeWallAnchor(Hit, Found->GetFootprint(), Found->WallThickness, SillZ);

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
	float DemandWidth = 0.0f, DemandHeight = 0.0f;
	GetDemandSize(DemandWidth, DemandHeight);

	FCSHouseWindow Out;
	Out.EdgeIndex = InAnchor.EdgeIndex;
	// **诉求的弧长从锚点现算**，不是存下来的 —— 房子一改尺寸，同一个锚点算出来的就是新墙上
	// 的新弧长。这正是"推第 e 条边，e+1 那面墙没动窗却滑了 Δ"那个 bug 的修法。
	Out.CenterS = CSHouse_AnchorS(InAnchor, InHost.GetFootprint(), InHost.WallThickness);
	Out.Width = DemandWidth;
	Out.Height = DemandHeight;
	Out.Shape = Shape;
	Out.SillZ = InAnchor.SillZ;
	return Out;
}

const TCHAR* ACSWindowMarker::DefaultFrameMeshPath =
	TEXT("/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/decorators_window_cottage_1x1");

const TCHAR* ACSWindowMarker::DefaultLintelMeshPath =
	TEXT("/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/setdressing_window_lintel");

const TCHAR* ACSWindowMarker::DefaultSillMeshPath =
	TEXT("/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/setdressing_window_sill");

const TCHAR* ACSWindowMarker::DefaultGlassMeshPath =
	TEXT("/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/window_cottage_1x1_glass");

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

	// 窗台**零偏移**：`setdressing_window_sill` 的 origin 是 (0, +28, −76)，已经在窗的局部空间里
	// 预摆好了（−76 正落在 160 高窗的下沿，+28 是朝外挑的鼻子）。
	// 过梁的 origin 居中 ⇒ 要自己抬到窗顶，抬多少由 `RefreshPieceLayout` 现算。
	RefreshPieceLayout();
}

void ACSWindowMarker::GetDemandSize(float& OutWidth, float& OutHeight) const
{
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
