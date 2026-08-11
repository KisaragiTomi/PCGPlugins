#pragma once

#include "CoreMinimal.h"
#include "CSGpuMeshSceneProxy.h"
#include "CSDisplayComponent.h"

/**
 * 把一组 GPU 常驻的点（位置 + 法线）画成箭头网格的代理。
 *
 * 与 FCSDisplayTriangleSceneProxy 同构：注册标准三角 stream 集，然后在 BuildGeometry 里
 * 跑两个 compute pass（展开箭头 + 建 indirect args）填满基座持有的 buffer。因为走的是
 * FCSGpuMeshSceneProxy 这条描述符驱动的路径，箭头能用真实材质，并且免费继承
 * UCSGpuMeshComponent 的 CPU 回读 / 存 StaticMesh 能力——这正是它相对
 * FCSDisplayVoxelSceneProxy（position-only 顶点工厂 + 一帧调试材质）的区别。
 *
 * 每个箭头的顶点/索引数固定（见 CSPointArrowMesh.usf），故容量在 CPU 侧按点数直接算出。
 */
class FCSDisplayPointArrowSceneProxy final : public FCSGpuMeshSceneProxy
{
public:
	/** 一个箭头的顶点数 / 三角数 / 索引数，必须与 CSPointArrowMesh.usf 中的常量一致。 */
	static constexpr uint32 VertsPerArrow = 13;
	static constexpr uint32 TrisPerArrow = 14;
	static constexpr uint32 IndicesPerArrow = TrisPerArrow * 3;

	FCSDisplayPointArrowSceneProxy(UCSDisplayComponent* Component, const FCSDisplayPointArrowData& InData);
	virtual ~FCSDisplayPointArrowSceneProxy() override;

	virtual SIZE_T GetTypeHash() const override;

protected:
	//~ FCSGpuMeshSceneProxy interface
	virtual void RegisterStreams() override;
	virtual void BuildGeometry(FRHICommandListBase& RHICmdList) override;

private:
	FCSDisplayPointArrowData Data;
	uint32 ArrowCapacity = 0;
};
