// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"

// 表面体素的“只记录、不提交”接口。
//
// AComputeShaderMeshGenerator::PrepareBoxSceneSurfaceVoxelsGPU 会自己开三张 RDG 图
// （清零 / 分批体素化 / finalize+blur）、把结果 extract 成 pooled buffer，然后
// FlushRenderingCommands 阻塞 game thread —— 对不需要回读的消费者来说，那次 flush 纯粹是
// 为了让 game thread 能读一个 IsValid()。
//
// 这里把同一套 pass 拆成“CPU 侧准备”与“往调用者的图里记录”两步，于是体素可以和下游
// （藤蔓的 space colonization + 建网格）合并进同一张图：没有中间 Execute，没有 pooled
// extract，也没有 flush。旧接口原样保留给仍需要 pooled/回读语义的调用者。
//
// 用法：
//   game thread : Generator->PrepareSurfaceVoxelPassInputs(VoxelSize, 0.0f, Inputs);
//   render thread: FCSSurfaceVoxelPassOutputs Out;
//                  AddCSSurfaceVoxelPasses(GraphBuilder, Inputs, Out);
//                  // Out.* 是本图内的 FRDGBufferRef，可直接喂给后续 pass

struct FCSSurfaceVoxelPassInputsImpl;

/**
 * CPU 侧准备好的体素输入。持有已解析的三角形请求、地形三角形和体素化参数。
 *
 * 内部类型（FResolvedStaticMeshTriangleRequest 等）住在 ComputeShaderGenerator 的 Private
 * 头里，所以这里走 PIMPL：别的模块能持有、移动、传递这个 bundle，但看不到内部类型。
 *
 * Impl 用 TSharedPtr 而非 TUniquePtr：这个 bundle 从准备好到被 AddCSSurfaceVoxelPasses 读完
 * 全程只读，而它要跟着 FVineBuildInput 一路传到场景代理（代理是按值持有的），沿途会被拷贝。
 * 共享同一份 impl 既让拷贝保持合法，又避免把几十万条三角形请求真的复制一遍。
 */
struct COMPUTESHADERGENERATOR_API FCSSurfaceVoxelPassInputs
{
	FCSSurfaceVoxelPassInputs();
	~FCSSurfaceVoxelPassInputs();
	FCSSurfaceVoxelPassInputs(FCSSurfaceVoxelPassInputs&& Other);
	FCSSurfaceVoxelPassInputs& operator=(FCSSurfaceVoxelPassInputs&& Other);
	FCSSurfaceVoxelPassInputs(const FCSSurfaceVoxelPassInputs& Other);
	FCSSurfaceVoxelPassInputs& operator=(const FCSSurfaceVoxelPassInputs& Other);

	/** 有可体素化的几何（静态网格请求或地形三角形至少有一样）且体素尺寸合法。 */
	bool IsValid() const;

	/** 体素栅格原点（= 查询盒 Min），下游把体素索引换算回世界坐标要用。 */
	FVector GetVoxelOrigin() const;
	/** 体素化所覆盖的世界包围盒。 */
	FBox GetWorldBounds() const;
	float GetVoxelSize() const;
	/** 体素 buffer 的分配容量（= MaxVoxels）。真实数量只有 GPU 的 Counter[0] 知道，所以下游
	 *  一律按这个容量分配、按 Counter 取用 —— 与 FCSSurfaceVoxelGPUBuffers::VoxelCapacity 同义。 */
	uint32 GetVoxelCapacity() const;

	TSharedPtr<FCSSurfaceVoxelPassInputsImpl> Impl;
};

/**
 * AddCSSurfaceVoxelPasses 写出的图内资源。全部是调用者那张图里的 FRDGBufferRef，
 * 生命周期就是那张图 —— 不要跨图保存，需要跨图请自己 ExtractBuffer。
 */
struct FCSSurfaceVoxelPassOutputs
{
	FRDGBufferRef Positions = nullptr;        // float4 / voxel，xyz = 体素中心
	FRDGBufferRef Normals = nullptr;          // float4 / voxel，已 finalize（含 blur）
	FRDGBufferRef TargetPositions = nullptr;  // float4 / voxel，表面吸附目标点
	FRDGBufferRef Cells = nullptr;            // int4 / voxel，整数栅格坐标
	FRDGBufferRef Counter = nullptr;          // uint2，[0] = 有效体素数
	FRDGBufferRef HashSlots = nullptr;        // uint / slot，体素空间哈希（线性探测）
	FRDGBufferRef HashIndices = nullptr;      // uint / slot，与 HashSlots 配套

	uint32 VoxelCapacity = 0u;   // 分配容量；真实数量只有 Counter[0] 知道
	uint32 HashSlotCount = 0u;   // 2 的幂
	float VoxelSize = 0.0f;
	FVector VoxelOrigin = FVector::ZeroVector;
	FBox WorldBounds = FBox(ForceInit);

	bool bValid = false;
};

/**
 * 把清零、分批体素化、finalize 与 blur 全部记录进 GraphBuilder，不 Execute。
 *
 * 分批仍然保留：按三角形数切批，批次之间靠写同一组 UAV 形成依赖，RDG 会自动串行化并复用
 * 每批的三角形临时 buffer，所以显存行为与原来的“每批一张图”接近，但少了批次间的提交开销。
 *
 * @return 记录成功返回 true；输入无效或没有可体素化的几何返回 false（此时 Out.bValid 为 false）。
 */
COMPUTESHADERGENERATOR_API bool AddCSSurfaceVoxelPasses(
	FRDGBuilder& GraphBuilder,
	const FCSSurfaceVoxelPassInputs& In,
	FCSSurfaceVoxelPassOutputs& Out);
