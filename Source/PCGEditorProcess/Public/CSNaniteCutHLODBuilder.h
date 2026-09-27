#pragma once

#include "CoreMinimal.h"
#include "CSMeshVisibilityCull.h"
#include "CSNaniteCut.h"
#include "WorldPartition/HLOD/HLODBuilder.h"
#include "CSNaniteCutHLODBuilder.generated.h"

/**
 * UCSNaniteCutHLODBuilder 的设置。
 *
 * CutError 由切换距离反推：Nanite 在 MinVisibleDistance 处画这些物体时误差恰好是 PixelError 个像素，
 * 于是 HLOD 出现那一刻的几何就是 Nanite 在那个距离本来会画的样子，切换几乎看不出来。
 */
UCLASS(Blueprintable, Config = Engine, PerObjectConfig)
class PCGEDITORPROCESS_API UCSNaniteCutHLODBuilderSettings : public UHLODBuilderSettings
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	virtual void ComputeHLODHash(FHLODHashBuilder& InHashBuilder) const override;

	/** HLOD 直接引用源材质，不烘焙贴图。 */
	virtual bool IsReusingSourceMaterials() const override { return true; }
#endif

	/** 切换距离上允许的屏幕误差（像素）。1 = Nanite 默认精度；远处的 HLOD 可以放宽到 2～4 省三角形。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nanite Cut", meta = (ClampMin = "0.1"))
	float PixelError = 1.0f;

	/** 参考视图的屏幕宽度（像素）。默认同引擎 Simplify 构建器；目标分辨率更高时按比例调大（4K 填 3840）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nanite Cut", meta = (ClampMin = "1"))
	float ReferenceScreenWidth = 1920.0f;

	/** 参考视图的水平 FOV（度）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nanite Cut", meta = (ClampMin = "1", ClampMax = "179"))
	float ReferenceHorizontalFOV = 90.0f;

	/** 输出网格开 Nanite：比切换距离更远时继续连续 LOD。开了之后 HLOD 显示前要预热（RequiresWarmup）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nanite Cut")
	bool bEnableNaniteOutput = false;

	/** 更细的页没驻留时：照写驻留着的（那几处偏粗），或整个源不写。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nanite Cut")
	ECSNaniteCutIncompletePolicy IncompletePolicy = ECSNaniteCutIncompletePolicy::UseAvailable;

	/** 不完整的源顺手请求全部流送页：页要跨帧才流进来，编辑器里过几帧再重建一次 HLOD 就是完整的。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nanite Cut")
	bool bRequestMissingPages = true;

	/** actor 或组件带这个标签就不进 HLOD。None = 不按标签排除。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nanite Cut")
	FName ExcludeTag = UCSNaniteCutOps::DefaultExcludeTag();

	/**
	 * 剔除从外面看不见的三角形：被别的物体整个挡住的物体、互相穿插的内部面、封闭房间与山洞深处。
	 * 视点放在 MinVisibleDistance 上（HLOD 只在那以外显示）。只算 HLOD 自己的遮挡，地形不参与。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nanite Cut")
	bool bCullHidden = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nanite Cut", meta = (EditCondition = "bCullHidden"))
	FCSMeshVisibilityCullOptions CullOptions;
};

/**
 * World Partition HLOD 构建器：把一群 Nanite 静态网格按各自的 Nanite 截面合并成一份静态网格。
 *
 * 每个源（普通组件与 ISM 的每个实例）按 CutError = 世界误差 ÷ 实例最小轴缩放取截面，全部写进一个
 * UCSMesh（GPU 上一次计数 + 一次写出），读回后以 HLOD 包围盒中心为枢轴建成静态网格。
 * foliage 管的小物体不进 HLOD，遇到 foliage 组件直接跳过；actor 或组件带 ExcludeTag 的跳过；
 * 没有 Nanite 数据的网格同样跳过并记日志。筛选规则与 ACSNaniteCutHLODActor 共用（UCSNaniteCutOps）。
 *
 * 用法：HLOD Layer 资产里 Layer Type 选 Custom，HLOD Builder Class 选 CSNaniteCutHLODBuilder。
 * 需要真 RHI（GPU 抽取），-nullrhi 下什么都不产出。设计见插件根 NaniteClusterHLOD_Plan.md。
 */
UCLASS()
class PCGEDITORPROCESS_API UCSNaniteCutHLODBuilder : public UHLODBuilder
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	virtual bool RequiresWarmup() const override;
	virtual TSubclassOf<UHLODBuilderSettings> GetSettingsClass() const override;
	virtual TArray<UActorComponent*> Build(const FHLODBuildContext& InHLODBuildContext, const TArray<UActorComponent*>& InSourceComponents) const override;
#endif
};
