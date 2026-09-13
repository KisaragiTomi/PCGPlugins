#include "CSHouseHeightHandleActor.h"

#include "CSHouseActor.h"
#include "CSHouseResize.h"   // CSHouseResize_EdgeOuterLocal —— 边号口径只有这一份
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

ACSHouseHeightHandleActor::ACSHouseHeightHandleActor()
{
	// 根组件、不 tick、Movable 都由基类构造好了。

	// 引擎立方体是 100 cm 见方、原点在中心 —— 缩放成细长条即可，不需要自建网格。
	// 长宽厚在 `UpdateFrameGeometry` 里按 footprint 现算（房子会被拉尺寸）。
	static ConstructorHelpers::FObjectFinderOptional<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));

	for (int32 Edge = 0; Edge < 4; ++Edge)
	{
		const FName Name(*FString::Printf(TEXT("Bar%d"), Edge));
		UStaticMeshComponent* Bar = CreateDefaultSubobject<UStaticMeshComponent>(Name);
		Bar->SetupAttachment(RootComponent);
		MakeEditorGizmoProp(Bar);
		if (UStaticMesh* Mesh = CubeMesh.Get()) Bar->SetStaticMesh(Mesh);
		BarComponents[Edge] = Bar;
	}
}

void ACSHouseHeightHandleActor::InitializeHandle(ACSHouseActor* InHost)
{
	SetHost(InHost);

	for (UStaticMeshComponent* Bar : BarComponents) ApplyHighlightMaterial(Bar);

	SnapToCanonical();
}

FVector ACSHouseHeightHandleActor::ComputeCanonicalWorldLocation() const
{
	const ACSHouseActor* H = Host.Get();
	// 宿主没了就原地不动：跳回世界原点会让"房子删了但抓手还在"这一瞬间变成"抓手飞走了"。
	if (!H) return GetActorLocation();

	// 房心正上方、檐口高度。框因此同时是"墙有多高"的读数。
	return H->GetActorTransform().TransformPosition(FVector(0.0, 0.0, H->WallHeight));
}

void ACSHouseHeightHandleActor::SnapToCanonical()
{
	SetActorLocation(ComputeCanonicalWorldLocation());

	// 朝向归位：用户拿 gizmo 转过它的话框会歪。我们从不读 actor 的旋转，转它无害 —— 但难看。
	if (const ACSHouseActor* H = Host.Get())
	{
		SetActorRotation(H->GetActorRotation());
	}

	// 房子被拉尺寸之后框要跟着长/缩。
	UpdateFrameGeometry();

	// 记账量与摆位**必须一起更新**：只摆位不重置，下一次 `PostEditMove` 会把程序刚制造的
	// 这段位移当成用户拖的，墙高会自己跳一下（与拉尺寸抓手同一条纪律）。
	LastConsumedWorld = GetActorLocation();
}

void ACSHouseHeightHandleActor::UpdateFrameGeometry()
{
	const ACSHouseActor* H = Host.Get();
	if (!H) return;

	// 框是**包围盒的可视化**：`FootprintSize` 就是 footprint 折线的包围盒（异形房子的形状被拉伸到它），
	// 所以四根条子恒围成一个矩形、与房子有几条边无关。高度抓手只有一个自由度，框的形状不承载信息。
	const FVector2D Frame = H->FootprintSize * double(FrameScale);
	const FVector2D HalfFrame = Frame * 0.5;
	constexpr double CubeSize = 100.0;   // 引擎 Cube 的边长
	const double Thick = double(FrameThickness);

	for (int32 Edge = 0; Edge < 4; ++Edge)
	{
		UStaticMeshComponent* Bar = BarComponents[Edge];
		if (!Bar) continue;

		const FVector2D Outer = CSHouseResize_EdgeOuterLocal(Edge);

		// 条子沿**自己那面墙的走向**躺着，也就是垂直于外法线的那根轴。
		// 边 0/2（南北，外法线 ±Y）的条子沿 X 跑；边 1/3（东西，外法线 ±X）的沿 Y 跑。
		const bool bRunsAlongX = FMath::IsNearlyZero(Outer.X);

		// 四角要闭合：沿 X 的两根各**多出一个厚度**盖住角上的方块，沿 Y 的两根相应**缩掉一个
		// 厚度**抵住它们。不这么配的话，要么四角各缺一个洞，要么八根条子在角上互相穿模。
		const double Length = bRunsAlongX
			? Frame.X + Thick
			: FMath::Max(Frame.Y - Thick, Thick);

		// 摆位：actor 自己在房心正上方，所以条子的相对位置就是"沿外法线推半个框"。
		Bar->SetRelativeLocation(FVector(Outer.X * HalfFrame.X, Outer.Y * HalfFrame.Y, 0.0));
		Bar->SetRelativeRotation(FRotationMatrix::MakeFromX(
			bRunsAlongX ? FVector(1.0, 0.0, 0.0) : FVector(0.0, 1.0, 0.0)).Rotator());
		Bar->SetRelativeScale3D(FVector(Length / CubeSize, Thick / CubeSize, Thick / CubeSize));
	}
}

float ACSHouseHeightHandleActor::ConsumeDragToHost(bool bFinished)
{
	// 走基类那条唯一执行面 —— 无宿主自毁、最终裁决的时机判定都在那儿，全族一份。
	LastAppliedOffset = 0.0f;
	HandleDrag(bFinished);
	return LastAppliedOffset;
}

bool ACSHouseHeightHandleActor::OnHandleDrag(bool bFinished)
{
	ACSHouseActor* H = Host.Get();
	// 返回 false ⇒ 基类在 `bFinished` 时按 `bDestroyWhenHostless` 自毁。
	if (!H) return false;

	// 只取**世界 Z**：把框拖歪不该改高度。歪掉的水平分量在下面的统一回位里被清掉。
	//
	// ⚠️ 用 `GetActorLocation() − LastConsumedWorld` 而不是"当前位置 − 规范位置"，理由与拉尺寸
	// 抓手逐字相同：顶在 `MinWallHeight` 上时 `Applied != Offset`，拿请求值记账残差会一路
	// 累积，松手瞬间房子跳一大截。
	const float Offset = float(GetActorLocation().Z - LastConsumedWorld.Z);

	const float Applied = H->PushHeight(Offset, bFinished);

	// 全部抓手统一重摆（含自己）：改墙高会让四个拉尺寸锥子的规范高度一起变
	// （它们挂在 `WallHeight × HandleHeightFraction` 上），不重摆就会留在原来的高度上。
	H->SnapResizeHandles();

	LastAppliedOffset = Applied;
	return true;
}

void ACSHouseHeightHandleActor::OnDetachFromHost()
{
	// 房子那张表要把这一格清掉，否则 `IsInResizeMode()` 会一直答 true，编辑器侧的失选监听
	// 就永远等不到退出。基类保证 `Destroyed` 与 `EndPlay` 两条路都会走到这里，且幂等。
	if (ACSHouseActor* H = Host.Get())
	{
		H->NotifyResizeHandleDestroyed(this);
	}
}
