#pragma once

#include "CoreMinimal.h"
#include "CSHouseRoof.h"

/** 露台接合只读输入。构建空间由房子提供，不能拿含缩放 / pitch 的 actor 变换替代。 */
struct FCSStairsTerraceSite
{
	FCSRoofDesc Roof;
	FTransform World = FTransform::Identity;
	float WallThickness = 30.0f;
};

/** 一端的派生接合。原始样条点不改写，移开后自动还原。 */
struct FCSStairsTerraceConnection
{
	bool bConnected = false;
	FVector Position = FVector::ZeroVector;
	FCSRoofParapetOpening Opening;
	double DistanceSquared = TNumericLimits<double>::Max();
};

/** 限定水平 / 竖向范围与外侧接近方向；不够宽的边、斜屋顶、擦边路径均不接合。 */
COMPUTESHADERGENERATOR_API FCSStairsTerraceConnection CSStairs_ConnectTerrace(
	const FCSStairsTerraceSite& Site, const FVector& Endpoint, const FVector& Neighbor,
	float Width, float HorizontalRange, float VerticalRange);
