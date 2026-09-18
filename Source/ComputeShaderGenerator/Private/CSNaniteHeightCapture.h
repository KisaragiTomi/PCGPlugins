#pragma once

// -----------------------------------------------------------------------------
// 用引擎渲染器把 Nanite 网格拍进俯视高度图（模块内部接口）。
//
// 三角形高度图路径（IndexedMeshToHeightmapCS）读的是 render LOD 的 index/position buffer，
// 对 Nanite 网格那只是 fallback 低模；全精度几何在 GPU 上只以压缩 cluster page 的形式待在
// 流送池里，自己去读等于重写一遍 Nanite 的剔除与光栅化。这里直接借渲染器：高度图每块一个
// 只画深度的 custom render pass（俯视正交、ShowOnly = 这些组件），渲染器拷出来的 SceneDepth
// 在 OnPostRender 里就地换算成 depth = CameraHeight - WorldZ 并按 min 合并 —— 与三角形路径、
// 地形 RenderHeightmap 的合并是同一套语义，谁先谁后结果都一样。
//
// custom render pass 只在"下一个渲染这个场景的 renderer"里执行。若等帧末的视口渲染，调用方
// 紧接着入队的读取 / 求解早就跑完了，所以这里立刻用一个空拍的 SceneCapture 建出 renderer：
// 渲染命令与调用前后入队的其它命令保持顺序，无头运行（没有视口）也照样出结果。
// -----------------------------------------------------------------------------

#include "CoreMinimal.h"

class UPrimitiveComponent;
class UStaticMeshComponent;
class UTextureRenderTarget2D;
class UWorld;

namespace CSNaniteHeightCapture
{
	/** [game thread] 这个世界现在能不能用渲染器拍（有 RHI、有场景）。不能时 Nanite 网格应留在三角形路径上。 */
	bool IsAvailable(const UWorld* World);

	/** [game thread] 组件此刻是否按 Nanite 画在场景里、并且会出现在捕获视图中。
	 *  没有代理（隐藏、编辑器里临时隐藏）、游戏中隐藏、排除出 scene capture 的，渲染器拍不到 ——
	 *  返回 false 让它留在三角形路径（fallback）上，而不是从高度图里消失。 */
	bool IsCapturableNaniteComponent(const UStaticMeshComponent* Component);

	struct FHeightmapRequest
	{
		/** 要拍的组件，通常就是 IsCapturableNaniteComponent 为 true 的那些。 */
		TArray<TWeakObjectPtr<UPrimitiveComponent>> Components;
		/** 浮点 RT，R = depth（CameraHeight - WorldZ），调用方已按自己的哨兵清好。 */
		UTextureRenderTarget2D* Heightmap = nullptr;
		/** XY 铺满整张图（texel 中心对齐，与 IndexedMeshToHeightmapCS 相同）；Z 决定近 / 远平面。 */
		FBox WorldBounds = FBox(ForceInit);
		float CameraHeight = 0.0f;
	};

	/** [game thread] 立刻渲染，结果 min 合并进 Request.Heightmap。返回 false 表示什么都没入队。 */
	bool CaptureIntoHeightmap(UWorld* World, const FHeightmapRequest& Request);

	/** 已经在渲染线程里合并过的块数（测试用：证明 pass 真的执行了，且排在随后的读取之前）。 */
	uint64 DebugGetMergedTileCount();
}
