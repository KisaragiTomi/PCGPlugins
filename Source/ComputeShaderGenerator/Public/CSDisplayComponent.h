#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "RenderGraphResources.h"
#include "CSDisplayComponent.generated.h"

struct FCSGpuDebugPooledSource;
struct FCSSurfaceVoxelGPUBuffers;

/** 组件当前在显示什么。决定创建哪种 SceneProxy。 */
UENUM()
enum class ECSDisplayMode : uint8
{
	None,
	VoxelDirections,  // 体素方向线 + 中心点：position-only 顶点工厂 + 双次 indirect draw
	VoxelQuads,       // 体素孤立面片
};

/** 体素显示请求的渲染线程快照（原 FCSMeshGeneratorDebugData）。 */
struct FCSDisplayVoxelData
{
	TRefCountPtr<FRDGPooledBuffer> Positions;
	TRefCountPtr<FRDGPooledBuffer> Normals;
	TRefCountPtr<FRDGPooledBuffer> Counter;
	int32 VoxelCapacity = 0;
	int32 MaxVoxelsToDraw = 0;
	float VoxelSize = 0.0f;
	float DirectionLength = 0.0f;
	float QuadScale = 1.0f;
	float NormalOffsetScale = 0.0f;
	FLinearColor DirectionColor = FLinearColor::Blue;
	FLinearColor PointColor = FLinearColor::Yellow;
	FBox WorldBounds = FBox(ForceInit);
	ECSDisplayMode Mode = ECSDisplayMode::VoxelDirections;
	bool bDrawPoints = true;
	bool bReverseOrientation = false;

	bool IsValid() const
	{
		return Positions.IsValid() && Normals.IsValid() && Counter.IsValid() && VoxelCapacity > 0;
	}
};

/**
 * 体素调试显示组件：把只存在于 GPU 上的表面体素画出来。
 *
 * 支持两种内容（一个实例同时只显示一种，后一次 Show* 覆盖前一次）：
 *   - 体素方向线 + 中心点
 *   - 体素孤立面片
 *
 * 两者都走 FCSDisplayVoxelSceneProxy：position-only 顶点工厂 + 一帧调试材质 + indirect draw。
 * 没有真实材质、没有回读、没有存盘 —— 这正是它区别于 UCSMeshRenderComponent 的地方，也是它
 * 不再继承 UCSGpuMeshComponent 的原因：那条基座上的每一样东西（GetRenderMaterial、
 * ReadbackMeshSync、SaveRenderedMeshToStaticMesh）都只服务于 FCSGpuMeshSceneProxy 叶子。
 *
 * 曾经在此的两种内容都已迁到 UCSMesh + UCSMeshRenderComponent，几何归网格对象所有，渲染状态
 * 重建只是重新绑定，而不像这里每次重建都要把生成 compute 重跑一遍：
 *   - 场景三角汤 -> AComputeShaderMeshGenerator::DirectGpuMesh
 *   - 点集箭头   -> ACSPointBrushActor::PointArrowMesh（CSPointArrowMesh.h）
 *
 * 生命周期由调用方通过 Lifetime 决定，常驻与临时共用一套定时器：
 *   Lifetime <  0  常驻（默认）
 *   Lifetime == 0  下一帧自动清除（一帧可视）
 *   Lifetime >  0  该秒数后自动清除
 *
 * 需要同时显示多组内容时，在 Actor 上追加一个本组件实例即可，无需改动本类。
 *
 * 数据都是世界空间，故本组件以绝对（世界原点）变换渲染，local space 即 world space。
 */
UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
class COMPUTESHADERGENERATOR_API UCSDisplayComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UCSDisplayComponent();

	// -------------------------------------------------------------------------
	// 显示入口。Lifetime 语义见类注释。
	// -------------------------------------------------------------------------

	/** 每个体素一条沿法线的方向线，外加可选的中心点。返回提交的容量。 */
	int32 ShowVoxelDirections(
		const FCSGpuDebugPooledSource& Source,
		float DirectionLength,
		FLinearColor DirectionColor,
		bool bDrawPoints,
		FLinearColor PointColor,
		int32 MaxDirectionsToDraw,
		float Lifetime = -1.0f);

	/** 表面体素入口；转调上面的 pooled-source 重载。 */
	int32 ShowVoxelDirections(
		const FCSSurfaceVoxelGPUBuffers& Source,
		float DirectionLength,
		FLinearColor DirectionColor,
		bool bDrawPoints,
		FLinearColor PointColor,
		int32 MaxDirectionsToDraw,
		float Lifetime = -1.0f);

	/** 每个有效体素一片朝向法线的孤立面片。 */
	bool ShowVoxelQuads(
		const FCSSurfaceVoxelGPUBuffers& Source,
		float QuadScale,
		float NormalOffsetScale,
		bool bReverseOrientation,
		float Lifetime = -1.0f);

	/** 清空显示并释放持有的 GPU 引用。取代原先的 ClearTriangleSource + ClearDebug。 */
	UFUNCTION(BlueprintCallable, Category = "CS Display")
	void ClearDisplay();

	ECSDisplayMode GetDisplayMode() const { return Mode; }

	//~ UPrimitiveComponent interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;

private:
	void SubmitVoxelData(FCSDisplayVoxelData&& InData, float Lifetime);
	void ScheduleClear(float Lifetime);

	ECSDisplayMode Mode = ECSDisplayMode::None;

	// 体素源
	FCSDisplayVoxelData PendingVoxelData;

	FTimerHandle ClearTimerHandle;
};
