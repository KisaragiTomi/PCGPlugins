#pragma once

#include "CoreMinimal.h"
#include "CSGpuDebugDraw.h" // FCSGpuDebugPooledSource

class UCSMesh;

/**
 * 一次点箭头几何生成的请求：GPU 常驻的点集 + 一个长度。
 *
 * 柱身/锥头的比例不在这里给：它们由 ArrowLength 派生（见 .cpp），因为唯一的调用方只知道
 * "箭头该有多长"，而把四个互相牵制的形状参数摊在接口上，只会让三个默认值永远不被改动，
 * 却又让人以为它们是可调的。
 */
struct FCSPointArrowBuildParams
{
	/** float4 位置 / float4 法线 / uint2 计数（[0] = 有效点数）。位置与法线都是世界空间。 */
	FCSGpuDebugPooledSource Source;

	/** 画多少个箭头的上限，也是分配尺寸。<= 0 表示"整个 Source.Capacity"；总是被夹到容量内。 */
	int32 MaxArrows = 0;

	/** 箭头总长（沿法线）。 */
	float ArrowLength = 25.0f;

	/** 写进顶点色。只有开了 vertex color 的材质看得见；默认表面材质看不见。 */
	FLinearColor ArrowColor = FLinearColor::Yellow;
};

/**
 * 把 Params.Source 里的每个点展开成一个沿其法线朝向的箭头，写进 Target 的常驻 stream 集。
 *
 * 几何归网格对象所有，不归任何 scene proxy：渲染状态重建只是重新绑定，不会像代理自持缓冲
 * 那样把生成 compute 重跑一遍；存盘（UCSMeshOps::CopyToStaticMesh）也不再要求组件正在渲染它。
 *
 * 每箭头顶点/索引数固定（见 CSPointArrowMesh.usf），所以容量在 CPU 侧直接由点数上限算出 ——
 * 这条路上没有"输出多大只有 GPU 知道"的容量回读。真正画几个仍由 GPU 计数决定。
 *
 * 输出是世界空间（源点就是世界坐标，shader 不做任何空间变换），而常驻集按约定就是世界空间，
 * 故这里没有 road 那样的 TransformMesh 烘焙步骤。画它的组件必须以绝对变换渲染。
 *
 * 游戏线程；阻塞（渲染 flush）。返回 false 表示这一次没有生成任何几何：网格里留着的是**上一次**的
 * 箭头（容量可能已经涨上去了），把它继续画在已经对不上的点集旁边看起来像一次成功的生成，所以
 * 调用方必须自己决定清掉还是留着。
 */
bool BuildPointArrowGeometryIntoMesh(UCSMesh* Target, const FCSPointArrowBuildParams& Params);
