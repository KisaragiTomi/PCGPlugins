#pragma once

#include "CoreMinimal.h"
#include "CSGpuMeshComponent.h"
#include "ComputeShaderMeshGenerator.h" // FCSBoxScenePreparedData
#include "RenderGraphResources.h"
#include "CSDisplayComponent.generated.h"

class UMaterialInterface;
struct FCSGpuDebugPooledSource;
struct FCSSurfaceVoxelGPUBuffers;

/** 组件当前在显示什么。决定创建哪种 SceneProxy，并守卫只对三角汤有意义的回读。 */
UENUM()
enum class ECSDisplayMode : uint8
{
	None,
	TriangleSoup,     // 场景三角汤：标准 stream 集 + DrawIndexedIndirect
	VoxelDirections,  // 体素方向线 + 中心点：position-only 顶点工厂 + 双次 indirect draw
	VoxelQuads,       // 体素孤立面片
	PointArrows,      // 点集箭头网格：走标准 stream 集，可用真材质、可回读存盘
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
 * 点集箭头显示请求的渲染线程快照。每个点展开成一个沿其法线朝向的箭头（柱身 + 锥头），
 * 位置与方向由同一个形状表达，不再需要"点 + 单独方向线"两次绘制。
 */
struct FCSDisplayPointArrowData
{
	TRefCountPtr<FRDGPooledBuffer> Positions; // float4，xyz = 世界位置
	TRefCountPtr<FRDGPooledBuffer> Normals;   // float4，xyz = 世界法线（箭头指向）
	TRefCountPtr<FRDGPooledBuffer> Counter;   // uint2，[0] = 有效点数
	int32 MaxArrowsToDraw = 0;                // 容量上限（决定 GPU buffer 大小）
	float ArrowLength = 25.0f;                // 箭头总长
	float ShaftRadius = 1.5f;                 // 柱身方形截面半边长
	float HeadRadius = 4.0f;                  // 锥体底面半径
	float HeadFraction = 0.35f;               // 锥头占总长比例
	FLinearColor ArrowColor = FLinearColor::Yellow;
	FBox WorldBounds = FBox(ForceInit);

	bool IsValid() const
	{
		return Positions.IsValid() && Normals.IsValid() && Counter.IsValid() && MaxArrowsToDraw > 0;
	}
};

/**
 * 统一的 GPU 内容显示组件。取代原先的 UCSDirectTriangleMeshComponent 与
 * UCSMeshGeneratorDebugComponent —— 它们做的是同一件事：把只存在于 GPU 上的内容画出来。
 *
 * 支持三种内容（一个实例同时只显示一种，后一次 Show* 覆盖前一次）：
 *   - 场景三角汤（AComputeShaderMeshGenerator 提取的 box-scene 快照，可回读存盘）
 *   - 体素方向线 + 中心点
 *   - 体素孤立面片
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
class COMPUTESHADERGENERATOR_API UCSDisplayComponent : public UCSGpuMeshComponent
{
	GENERATED_BODY()

public:
	UCSDisplayComponent();

	/** 三角汤模式使用的材质。为空时用引擎默认表面材质。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Display")
	TObjectPtr<UMaterialInterface> MeshMaterial;

	// -------------------------------------------------------------------------
	// 显示入口。Lifetime 语义见类注释。
	// -------------------------------------------------------------------------

	/** 显示 game thread 预备好的 box-scene 三角快照。
	 *  VertexCapacity = 三角容量 * 3（决定持久 GPU buffer 大小）；WorldBounds 为几何世界包围盒。 */
	void ShowTriangleSoup(const FCSBoxScenePreparedData& InPrepared, uint32 InVertexCapacity,
		const FBox& InWorldBounds, float Lifetime = -1.0f);

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

	/** 把一组点画成箭头网格（每点一个沿其法线朝向的箭头）。
	 *  走标准 stream 集，因此用的是 MeshMaterial 而非一帧调试材质，并可经
	 *  SaveRenderedMeshToStaticMesh 存成资产。返回提交的箭头容量。 */
	int32 ShowPointArrows(
		const FCSGpuDebugPooledSource& Source,
		int32 MaxArrowsToDraw,
		float ArrowLength,
		FLinearColor ArrowColor,
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

protected:
	//~ UCSGpuMeshComponent interface
	virtual UMaterialInterface* GetRenderMaterial() const override { return MeshMaterial; }
	/** 三角汤与点箭头都走 FCSGpuMeshSceneProxy 基座，可回读；体素调试模式必须挡住。 */
	virtual bool IsGpuMeshProxyActive() const override
	{
		return Mode == ECSDisplayMode::TriangleSoup || Mode == ECSDisplayMode::PointArrows;
	}

private:
	void SubmitVoxelData(FCSDisplayVoxelData&& InData, float Lifetime);
	void ScheduleClear(float Lifetime);

	ECSDisplayMode Mode = ECSDisplayMode::None;

	// 三角汤源（CreateSceneProxy 时取快照；重新提交会重建代理）
	FCSBoxScenePreparedData PendingPrepared;
	uint32 PendingVertexCapacity = 0;

	// 体素源
	FCSDisplayVoxelData PendingVoxelData;

	// 点箭头源
	FCSDisplayPointArrowData PendingArrowData;

	FTimerHandle ClearTimerHandle;
};
