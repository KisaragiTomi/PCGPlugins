#include "CSBoundsVisual.h"

#include "Components/BillboardComponent.h"
#include "Components/BoxComponent.h"

void CSBoundsVisual::PlaceCornerBillboard(UBillboardComponent* CornerBillboard, const FVector& Extent,
	const FVector& BoxRelativeLocation)
{
	if (CornerBillboard) CornerBillboard->SetRelativeLocation(BoxRelativeLocation + Extent);
}

void CSBoundsVisual::Apply(UBoxComponent* Box, const FVector& Extent, UBillboardComponent* CornerBillboard,
	USceneComponent* UnitCubeVisual)
{
	if (Box) Box->SetBoxExtent(Extent, /*bUpdateOverlaps*/ true);
	PlaceCornerBillboard(CornerBillboard, Extent, Box ? Box->GetRelativeLocation() : FVector::ZeroVector);
	// 引擎 Cube 是 100 cm、原点在中心：半长 50 —— 缩放取 Extent / 50 才与盒子严丝合缝（原构造脚本的 ÷50 就是这个）。
	if (UnitCubeVisual) UnitCubeVisual->SetRelativeScale3D(Extent / 50.0);
}
