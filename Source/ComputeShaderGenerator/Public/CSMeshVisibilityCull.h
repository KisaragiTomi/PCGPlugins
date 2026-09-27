#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "CSMeshVisibilityCull.generated.h"

class UCSMesh;

/** 外部可见性剔除的采样设置。视点距离不在这里：它跟着调用方的语义走（HLOD 取切换距离）。 */
USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSMeshVisibilityCullOptions
{
	GENERATED_BODY()

	/**
	 * 视线方向数，在仰角范围内按斐波那契球面均布。每个方向一张透视视图（视点在 ViewDistance 上）+
	 * 一张正交视图（无穷远）。方向越密，越不会漏掉只从很窄的角度（比如深屋里透过小窗）才看得见的东西。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Visibility", meta = (ClampMin = "1", ClampMax = "4096"))
	int32 NumDirections = 512;

	/**
	 * 视点仰角范围（度，相对包围球心的水平面）。只算网格自己的遮挡，地形等外部几何不参与：
	 * 下限默认 0°，等于把 HLOD 之外的地面当作挡住了下方视线。从山谷仰视高处 HLOD 的内容
	 * （屋檐底面之类）要把它调成负值。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Visibility", meta = (ClampMin = "-90", ClampMax = "90"))
	float MinElevationDegrees = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Visibility", meta = (ClampMin = "-90", ClampMax = "90"))
	float MaxElevationDegrees = 90.0f;

	/** 每张视图的边长（像素），视图恰好框住包围球。越细，越不会漏掉只露一条缝的三角形。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Visibility", meta = (ClampMin = "64", ClampMax = "4096"))
	int32 Resolution = 1024;

	/** 每个方向再加一张正交视图。透视视图在有限距离上，正交补上"更远处"看进凹处的那部分视线。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Visibility")
	bool bOrthographicViews = true;

	/** 与可见三角共享顶点的也保留：细长三角可能一个像素中心都没盖住而被漏采，这一圈补针眼。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Visibility")
	bool bKeepNeighbors = true;
};

USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSMeshVisibilityCullResult
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Visibility")
	int32 TrianglesBefore = 0;

	/** 至少在一张视图里是最近面的三角形数。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Visibility")
	int32 TrianglesVisible = 0;

	/** 留下的三角形数 = 可见 + 作为可见三角的邻居留下的。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Visibility")
	int32 TrianglesAfter = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Visibility")
	int32 NumViews = 0;

	/** 算完并且（需要的话）压实了。false 时网格原样没动。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Visibility")
	bool bApplied = false;
};

/**
 * 外部可见性剔除：从网格包围球外的一圈视点看它，删掉从来没被看见过的三角形 —— 完全被别的物体包住的
 * 物体、互相穿插的内部面、封闭房间与蛇形山洞深处。给 HLOD 用：HLOD 只在切换距离以外显示，这些三角形
 * 在那里永远画不出来。
 *
 * GPU 软件光栅（每张视图一趟深度、一趟"是不是最近面"），所有面按双面算（背面也遮挡、也能被看见）。
 * 压实保持留下的三角形的相对顺序，索引与逐三角材质号一起搬，顶点不动（没人引用的顶点留在原处）。
 * 改了三角形数，section 表随之作废：要分材质画的调用方之后再跑 UCSMeshOps::BuildMaterialSections。
 *
 * 同步、阻塞，游戏线程调用；要真 RHI。
 */
UCLASS()
class COMPUTESHADERGENERATOR_API UCSMeshVisibilityOps : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * ViewDistance：透视视点到包围球心的距离（cm），HLOD 传切换距离；小于包围球半径 × 1.5 时按后者算，
	 * 保证视点在网格外面。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh|Visibility")
	static UPARAM(DisplayName = "Target") UCSMesh* CullHiddenTriangles(
		UCSMesh* Target, float ViewDistance, const FCSMeshVisibilityCullOptions& Options, FCSMeshVisibilityCullResult& OutResult);
};
