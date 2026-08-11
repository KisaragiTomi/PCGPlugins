#pragma once

#include "CoreMinimal.h"
#include "CSGpuDebugDraw.h"
#include "Containers/Queue.h"
#include "Math/Box.h"
#include "Templates/Function.h"

class FRDGBuilder;
class FSceneView;
class FSceneViewStateInterface;

//// 深度缓冲笔刷采样服务。
////
//// 为什么需要它：场景深度只在渲染器内部存在，而且只有跑完 base pass 之后才是完整的。任何
//// 手工构造的 FSceneView 都拿不到它。所以和 FGDFSampleService 一样，注册一个
//// SceneViewExtension，在 PostRenderBasePassDeferred_RenderThread 回调里把待处理的采样请求
//// 挂到引擎自己的 GraphBuilder 上 —— 那一刻 FSceneTextureUniformParameters::SceneDepthTexture
//// 已经写满。
////
//// 与 GDF 服务的关键差别：这里**没有任何回读**。采样结果直接 append 进调用方持有的
//// FCSGpuDebugPooledSource（float4 位置 / float4 法线 / uint 计数），点数与点集全程留在 GPU 上，
//// 显示路径直接消费同一组 buffer。代价是 CPU 侧再也不知道画了多少点，笔刷数据不随关卡保存。

// 一次 stroke update 的采样输入。
struct FCSDepthBrushSampleRequest
{
	// 累积目标。持有 pooled 引用，请求在队列里排队期间 buffer 不会被释放。
	FCSGpuDebugPooledSource Output;

	// 只在这个视图状态对应的视口里采样：编辑器里同时有多个视口在渲染，用别的视口的深度
	// 会把点摆到用户没在看的地方。空指针表示不限定（第一个真视图就处理）。
	FSceneViewStateInterface* ViewState = nullptr;

	FVector BrushCentre = FVector::ZeroVector;
	float   BrushRadius = 500.0f;
	float   MinSpacing = 0.0f;

	// 本次要撒多少候选。整批共用一个线程组，所以上限就是组大小（256）。
	int32  SampleCount = 0;
	// 拉开相邻两次 stroke update 的散点分布；shader 会对它做哈希。
	uint32 RandomSeed = 0;

	// 无效盒表示不限制绘制范围。
	FBox PaintBounds = FBox(ForceInit);

	// 游戏线程回调，在本请求的 pass 录进渲染图之后触发。可空。调用方用它重建显示：此刻发出的
	// 渲染命令一定排在这个 pass 后面，看到的就是刚 append 完的计数。
	TFunction<void()> OnDispatched;
};

// 进程级单例：持有 view extension 与请求队列。
class COMPUTESHADERGENERATOR_API FCSDepthBrushSampleService
{
public:
	static FCSDepthBrushSampleService& Get();

	// 模块启动/关闭时调用（注册/注销 view extension）。Enqueue 会自行 Startup。
	void Startup();
	void Shutdown();

	// 游戏线程入队，立即返回。
	void EnqueueSample(FCSDepthBrushSampleRequest&& Request);

private:
	friend class FCSDepthBrushViewExtension;

	TSharedPtr<class FCSDepthBrushViewExtension, ESPMode::ThreadSafe> ViewExtension;

	// 生产者=游戏线程，消费者=渲染线程。
	TQueue<FCSDepthBrushSampleRequest, EQueueMode::Mpsc> SampleQueue;
};
