// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "DynamicMeshActor.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "UDynamicMesh.h"
#include "Curves/CurveLinearColor.h"
#include "Engine/TextureRenderTarget2D.h"

#include "GeometryVisualization.generated.h"


/**
 * 
 */

UENUM(BlueprintType)
enum class ERenderTargetChannelIndex : uint8
{
	R = 0 UMETA(DisplayName = "R"),
	G = 1 UMETA(DisplayName = "G"),
	B = 2 UMETA(DisplayName = "B"),
	A = 3 UMETA(DisplayName = "A"),
};

UCLASS()
class GEOMETRYSCRIPTEXTRAEDITOR_API UGeometryVisualization : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = Visualize)
	static void VisualizingVertexNormal(UDynamicMesh* TargetMesh, FTransform Transform, float Length = 100);

	/** 将RenderTarget以NxN点阵形式可视化，选择指定通道作为点的高度。
	 *  @param RenderTarget    要可视化的RenderTarget
	 *  @param GridSize        点阵的N值（N x N个点），最小2
	 *  @param ChannelIndex    选择哪个通道作为高度 (0=R, 1=G, 2=B, 3=A)
	 *  @param WorldCenter     点阵在世界空间中的中心位置
	 *  @param WorldExtent     点阵在世界空间XY平面上的覆盖范围
	 *  @param HeightScale     通道值到世界Z高度的缩放系数
	 *  @param Duration        调试点的显示时间（秒），默认5秒
	 *  @param PointColor      调试点的颜色（同时受通道值亮度影响）
	 *  @param PointSize       调试点的屏幕空间大小
	 */
	UFUNCTION(BlueprintCallable, Category = Visualize, meta = (DevelopmentOnly))
	static void VisualizeRenderTargetAsPointGrid(
		UTextureRenderTarget2D* RenderTarget,
		int32 GridSize = 32,
		ERenderTargetChannelIndex ChannelIndex = ERenderTargetChannelIndex::R,
		FVector WorldCenter = FVector::ZeroVector,
		FVector2D WorldExtent = FVector2D(500.0f, 500.0f),
		float HeightScale = 100.0f,
		float Duration = 5.0f,
		FLinearColor PointColor = FLinearColor::White,
		float PointSize = 5.0f
	);
};



