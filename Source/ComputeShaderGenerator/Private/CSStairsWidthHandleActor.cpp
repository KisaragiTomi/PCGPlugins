#include "CSStairsWidthHandleActor.h"

#include "CSHouseHandleActor.h"
#include "CSStairsActor.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

ACSStairsWidthHandleActor::ACSStairsWidthHandleActor()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent->SetMobility(EComponentMobility::Movable);
	ArrowComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Arrow"));
	ArrowComponent->SetupAttachment(RootComponent);
	ACSHouseHandleActor::MakeEditorGizmoProp(ArrowComponent);
	ArrowOutlineComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ArrowOutline"));
	ArrowOutlineComponent->SetupAttachment(ArrowComponent);
	ACSHouseHandleActor::MakeEditorGizmoProp(ArrowOutlineComponent);
	// 兜底锥子；TG 箭头在 InitializeHandle 里惰性换上（脚本烘的资产，理由同房子那边）。
	static ConstructorHelpers::FObjectFinderOptional<UStaticMesh> Cone(TEXT("/Engine/BasicShapes/Cone.Cone"));
	ArrowComponent->SetStaticMesh(Cone.Get());
	ArrowComponent->SetRelativeScale3D(FVector(0.35, 0.35, 0.5));
}

void ACSStairsWidthHandleActor::InitializeHandle(ACSStairsActor* InHost, int32 InSide)
{
	if (Host.Get() != InHost) DetachFromHost();
	Host = InHost;
	Side = InSide < 0 ? -1 : 1;
	if (UStaticMesh* TGArrow = ACSHouseHandleActor::LoadTGArrowMesh(TEXT("flat_arrow")))
	{
		ArrowComponent->SetStaticMesh(TGArrow);
		ArrowOutlineComponent->SetStaticMesh(ACSHouseHandleActor::LoadTGArrowMesh(TEXT("flat_arrow_outline")));
		ACSHouseHandleActor::ApplyOutlineMaterial(ArrowOutlineComponent);
		// 头指网格 +Y、原点在箭尾 ⇒ 原大、包围盒中心挪到 actor 原点（gizmo 在箭头正中）。actor 自己转到
		// "+Y 朝外、+Z 朝上"（SnapToCanonical），组件只需平移，与房子墙面箭头同一套摆法。
		ArrowComponent->SetRelativeScale3D(FVector::OneVector);
		ArrowComponent->SetRelativeLocation(-TGArrow->GetBounds().Origin);
		bTGArrow = true;
	}
	ACSHouseHandleActor::ApplyHighlightMaterial(ArrowComponent);
	SnapToCanonical();
}

FVector ACSStairsWidthHandleActor::GetOuterNormalWorld() const
{
	FVector Center, Right;
	float WidthScale;
	return Host.IsValid() && Host->GetWidthHandleFrame(Center, Right, WidthScale) ? Right * Side : FVector::ZeroVector;
}

FVector ACSStairsWidthHandleActor::ComputeCanonicalWorldLocation() const
{
	FVector Center, Right;
	float WidthScale;
	if (!Host.IsValid() || !Host->GetWidthHandleFrame(Center, Right, WidthScale)) return GetActorLocation();
	return Center + Right * Side * (Host->Width * WidthScale * 0.5f + HandleOffset) + FVector(0, 0, HandleLift);
}

void ACSStairsWidthHandleActor::SnapToCanonical()
{
	if (!Host.IsValid()) return;
	SetActorLocation(ComputeCanonicalWorldLocation());
	const FVector Outer = GetOuterNormalWorld();
	if (!Outer.IsNearlyZero())
	{
		// TG 箭头：+Y 朝外、+Z 朝上（躺平）；兜底锥子：尖（+Z）朝外。
		SetActorRotation((bTGArrow ? FRotationMatrix::MakeFromYZ(Outer, FVector::UpVector) : FRotationMatrix::MakeFromZ(Outer)).Rotator());
	}
	// 保持示意箭头的世界大小，不随宿主的非均匀缩放变形。
	SetActorScale3D(FVector::OneVector);
	LastConsumedHostLocal = Host->GetActorTransform().InverseTransformPosition(GetActorLocation());
}

bool ACSStairsWidthHandleActor::HandleDrag(bool bFinished)
{
	LastAppliedOffset = 0.0f;
	ACSStairsActor* Stairs = Host.Get();
	if (!Stairs || Stairs->IsActorBeingDestroyed())
	{
		if (bFinished && !IsTemplate()) Destroy();
		return false;
	}
	FVector Center, Right;
	float WidthScale;
	if (!Stairs->GetWidthHandleFrame(Center, Right, WidthScale)) return false;
	const FVector Baseline = Stairs->GetActorTransform().TransformPosition(LastConsumedHostLocal);
	const float Offset = float(FVector::DotProduct(GetActorLocation() - Baseline, Right * Side));
	const float PreviousWidth = Stairs->Width;
	const float AppliedWidth = Stairs->SetStairWidth(PreviousWidth + 2.0f * Offset / WidthScale);
	LastAppliedOffset = (AppliedWidth - PreviousWidth) * WidthScale * 0.5f;
	// 包括限位/无效轴拖动：归位并消费余量，反向拖动立即生效，不积攒拖拽欠账。
	Stairs->SnapResizeHandles();
	return true;
}

float ACSStairsWidthHandleActor::ConsumeDragToHost(bool bFinished)
{
	HandleDrag(bFinished);
	return LastAppliedOffset;
}

void ACSStairsWidthHandleActor::DetachFromHost()
{
	ACSStairsActor* Stairs = Host.Get();
	Host.Reset();
	if (Stairs) Stairs->NotifyResizeHandleDestroyed(this);
}

void ACSStairsWidthHandleActor::Destroyed()
{
	DetachFromHost();
	Super::Destroyed();
}

void ACSStairsWidthHandleActor::EndPlay(const EEndPlayReason::Type Reason)
{
	DetachFromHost();
	Super::EndPlay(Reason);
}

#if WITH_EDITOR
void ACSStairsWidthHandleActor::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	// NotPlaceable：只由宿主 World->SpawnActor 生成，不接受 AddActor 的合成落地事件。
	HandleDrag(bFinished);
}

void ACSStairsWidthHandleActor::PostEditUndo()
{
	Super::PostEditUndo();
	SnapToCanonical();
}
#endif
