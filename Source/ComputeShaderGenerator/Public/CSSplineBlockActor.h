#pragma once

#include "CoreMinimal.h"
#include "CSTinyGlade.h"
#include "Math/RandomStream.h"
#include "CSSplineBlockActor.generated.h"

class UMaterialInterface;
class USplineComponent;
class UStaticMesh;

/**
 * Spline 块排布演示：Tiny Glade 城齿/檐口式的"离散刚体块沿样条累积"。
 *
 * 与引擎 SplineMeshComponent 的本质区别：块不做弯曲变形 —— 每个块是刚体，只取自己
 * 中心弧长处的样条位置/切向摆放；整条的贴合靠"块数量 + 统一 pitch 缩放"完成
 * （SolveBlockLayout）。缩放只作用于沿样条方向（块局部 X）与块间距 Gap，
 * 截面（Y/Z）不缩，保持城齿观感。
 *
 * 数据流：BlockPalette 每个 StaticMesh 提取一次 CPU 三角（LOD0 渲染缓冲，重心移到
 * 原点）→ 逐块烘上样条世界变换 append 进一份 FCSGpuMeshCPUData → 基类
 * UploadTinyGladeSnapshot 上传；多材质槽时补一次 BuildMaterialSections。
 *
 * 常驻流是世界空间（渲染组件绝对变换），所以拖 spline 点、移动 actor 都必须全量
 * 重建 —— 两条路径都会重跑构造脚本，OnConstruction 是统一触发点。
 *
 * 绕序：StaticMesh 三角进常驻流时按 CopyFromStaticMesh 的翻转口径处理（交换角点
 * 1/2，顶点法线不取反）—— 两边的面法线口径差一个负号，见 CSMeshBuild.h。
 */
UCLASS(Blueprintable, BlueprintType)
class COMPUTESHADERGENERATOR_API ACSSplineBlockActor : public ACSTinyGlade
{
	GENERATED_BODY()

public:
	ACSSplineBlockActor();

	// -------------------------------------------------------------------------
	// Block Settings
	// -------------------------------------------------------------------------

	/** 块型盘。每个条目一个材质槽（槽 i = 条目 i 的槽 0 材质）；提取不到 CPU 三角数据的条目跳过。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS SplineBlock")
	TArray<TObjectPtr<UStaticMesh>> BlockPalette;

	/** 贪心随机选块的种子。OnConstruction 高频重建下保证同参数同结果，拖动时不闪变。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS SplineBlock")
	int32 Seed = 0;

	/** 块间距 cm（沿样条方向）。与块长一起参与整体 pitch 缩放。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS SplineBlock", meta = (ClampMin = "0.0"))
	float Gap = 0.0f;

	/** 非空则全体块都用它（单槽）；空则各 palette 条目用自己 mesh 的槽 0 材质。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS SplineBlock")
	TObjectPtr<UMaterialInterface> OverrideMaterial;

	// -------------------------------------------------------------------------
	// Public API
	// -------------------------------------------------------------------------

	/** 从 palette + spline 全量重建块网格。空结果（palette 全无效 / 样条长度 0）时清空显示。 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS SplineBlock")
	void RebuildBlocks();

	UFUNCTION(BlueprintPure, Category = "CS SplineBlock")
	USplineComponent* GetSpline() const { return Spline; }

	/**
	 * 排布求解（纯 CPU，可单测）：贪心随机填充到首次越界（最后一块必然越界），
	 * 再在两个候选里择优 ——
	 *   A) 保留最后一块，整体压缩：scaleA = TotalLength / S       （≤ 1）
	 *   B) 去掉最后一块，整体拉伸：scaleB = TotalLength / S_prev  （≥ 1）
	 * 取 |log(scale)| 较小者（压缩与拉伸对称计价）；序列只有一块时强制 A。
	 * 返回统一缩放系数（作用于沿样条方向的块长与 gap，即 pitch 整体缩放），
	 * OutSequence 是 palette 索引序列。TotalLength ≤ 0 或 PaletteLengths 全部
	 * 非正时返回 0 且序列为空 —— 调用方以此清空显示。
	 */
	static float SolveBlockLayout(float TotalLength, float InGap,
		const TArray<float>& PaletteLengths, FRandomStream& Rand,
		TArray<int32>& OutSequence);

	//~ AActor interface
	virtual void OnConstruction(const FTransform& Transform) override;

private:
	/** 排布参照的样条。拖它的点 / 移动 actor 都会重跑构造脚本 → RebuildBlocks。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS SplineBlock", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USplineComponent> Spline;
};
