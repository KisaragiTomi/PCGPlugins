#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"

/**
 * 一组 GPU 常驻的点：位置 + 法线 + 一个 GPU 写的计数，生产者在帧之间持有它，消费者把它注册进
 * 自己的 RDG 图。任何能填出「float4 位置 / float4 法线 / uint 计数」这三件套的东西，都能直接
 * 交给 BuildPointArrowGeometryIntoMesh 画成箭头。
 *
 * 三个 buffer 都要用 AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(...)) 分配 —— 消费方
 * 按 typed Buffer<float4> / Buffer<uint> 取视图，structured buffer 满足不了。
 *
 * 这个头文件曾经还装着 FCSGpuDebugDraw：一整套给自持缓冲的 scene proxy 用的辅助（position-only
 * 顶点工厂、索引缓冲/indirect args 分配、PT_LineList / PT_PointList 的着色提交，以及体素方向线
 * 和体素面片两个展开 pass）。它唯一的用户 FCSDisplayVoxelSceneProxy 已随 UCSDisplayComponent
 * 一起删除——调试几何改为写进 UCSMesh 的常驻流、由 UCSMeshRenderComponent 绘制，那条路不需要
 * 代理自持任何缓冲。剩下的就是这个纯数据结构。
 */
struct FCSGpuDebugPooledSource
{
	TRefCountPtr<FRDGPooledBuffer> Positions; // float4 (xyz world position, w unused)
	TRefCountPtr<FRDGPooledBuffer> Normals;   // float4 (xyz normal, w unused)
	TRefCountPtr<FRDGPooledBuffer> Counter;   // uint2 ([0] = live item count)
	int32 Capacity = 0;
	float ItemSize = 0.0f;                    // world size of one item; pads the debug bounds
	FBox WorldBounds = FBox(ForceInit);

	bool IsValid() const { return Positions.IsValid() && Normals.IsValid() && Counter.IsValid() && Capacity > 0; }
	void Reset() { *this = FCSGpuDebugPooledSource(); }
};
