#pragma once

#include "CoreMinimal.h"

class UBillboardComponent;
class UBoxComponent;
class USceneComponent;

/**
 * 「盒子 + 角标 + 示意立方体」的摆放：BP_Boolean 的构造脚本原来逐个节点连的那三件事，搬到这里给各家共用
 * （BP_Boolean 的构造脚本已清空，ACSNaniteCutHLODActor 的角标也走这里，两边不再各写一份）。
 *
 *   - 盒子按 Extent 定尺寸；
 *   - 角标落在盒子的最大角：与盒子同父级时，相对位置 = 盒子相对位置 + Extent；
 *     直接挂在盒子下时只传 Extent。
 *   - 100 cm 的示意立方体（引擎 /Engine/BasicShapes/Cube，原点在中心）按 Extent / 50 缩放，正好贴住盒子。
 *
 * 组件传空就跳过对应那一步。要在构造脚本 / OnConstruction 里调：这些都是相对变换的写入。
 */
namespace CSBoundsVisual
{
	COMPUTESHADERGENERATOR_API void Apply(UBoxComponent* Box, const FVector& Extent,
		UBillboardComponent* CornerBillboard = nullptr, USceneComponent* UnitCubeVisual = nullptr);

	/** 只挪角标：盒子已经是想要的尺寸（或者没有盒子组件）时用。 */
	COMPUTESHADERGENERATOR_API void PlaceCornerBillboard(UBillboardComponent* CornerBillboard, const FVector& Extent,
		const FVector& BoxRelativeLocation = FVector::ZeroVector);
}
