#include "CSGroundShaperSteps.h"

#include "CSMesh.h"                    // UCSMesh::CountedBlockingFlush —— 阻塞刷新的唯一计数入口
#include "RenderGraphBuilder.h"         // ZeroCounters 自建图（同上，unity 会掩盖）
#include "RenderGraphUtils.h"           // AllocatePooledBuffer / AddClearUAVPass（unity 构建会掩盖掉这一条，-SingleFile 才抓得到）
#include "RenderingThread.h"

namespace CSShaperSteps
{
/** 在渲染线程把不够大的 palette 补齐到 Capacities 给的尺寸。阻塞：Work 在栈上按引用捕获。 */
static bool CSShaperSteps_GrowTo(TArray<FPaletteBuffers>& Palettes, const TArray<uint32>& Capacities, const TArray<int32>& Grow)
{
	if (Grow.IsEmpty()) return true;

	TArray<FPaletteBuffers> Work = Palettes;
	ENQUEUE_RENDER_COMMAND(CSGroundStepEnsureCapacity)(
		[&Work, Grow, Capacities](FRHICommandListImmediate&)
		{
			for (const int32 Index : Grow)
			{
				// typed buffer（不是 structured）：剔除 pass 用 Buffer<float4> / Buffer<uint> 视图。
				Work[Index].PackedInstances = AllocatePooledBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), Capacities[Index] * 5u), TEXT("CSShaperSteps.PackedInstances"));
				Work[Index].Counter = AllocatePooledBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("CSShaperSteps.Counter"));
				Work[Index].Capacity = Capacities[Index];
			}
		});
	// 阻塞到 pooled 引用可用为止（同 CSPointBrushActor 的上传路径）。
	// **必须走计数通道**：这里以前是一个裸 FlushRenderingCommands，而计数器在 CSMesh.cpp 里，
	// 于是"交互期零阻塞"那几条断言对这条路径完全瞎 —— 拖尺寸每帧扩容也照样报 0 次刷新。
	// 断言看不见的阻塞等于不存在的纪律。
	UCSMesh::CountedBlockingFlush();

	Palettes = MoveTemp(Work);
	return true;
}

bool ReserveCapacity(TArray<FPaletteBuffers>& Palettes, uint32 MinCapacityPerPalette)
{
	if (Palettes.IsEmpty()) return false;
	const uint32 Want = FMath::Max(Align(MinCapacityPerPalette, 64u), 64u);

	TArray<uint32> Capacities;
	TArray<int32> Grow;
	Capacities.SetNumUninitialized(Palettes.Num());
	for (int32 Index = 0; Index < Palettes.Num(); ++Index)
	{
		Capacities[Index] = FMath::Max(Want, Palettes[Index].Capacity);
		if (!Palettes[Index].PackedInstances.IsValid() || !Palettes[Index].Counter.IsValid()
			|| Palettes[Index].Capacity < Capacities[Index])
		{
			Grow.Add(Index);
		}
	}
	return CSShaperSteps_GrowTo(Palettes, Capacities, Grow);
}

void ZeroCounters(TArray<FPaletteBuffers>& Palettes)
{
	bool bAnyValid = false;
	for (const FPaletteBuffers& Buffers : Palettes) bAnyValid |= Buffers.IsValid();
	if (!bAnyValid) return;

	// 按值捕获（`TRefCountPtr` 拷贝即加引用），录完直接 return —— 这条路不许阻塞：
	// 它会被"关掉藤/关掉摆件"这类属性改动踩到，而那也是拖动交互的一部分。
	ENQUEUE_RENDER_COMMAND(CSShaperStepsZeroCounters)(
		[Work = Palettes](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSShaperSteps.ZeroCounters"));
			for (const FPaletteBuffers& Buffers : Work)
			{
				if (!Buffers.IsValid()) continue;
				FRDGBufferRef CounterRef = GraphBuilder.RegisterExternalBuffer(Buffers.Counter, TEXT("CSShaperSteps.Counter"));
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CounterRef, PF_R32_UINT)), 0u);
				// 剔除 pass 只读这个 buffer 且不负责恢复状态 —— producer 自己留在 SRVMask
				// （与 CSHouseFrame::Scatter / CSHouseVine::Pack 同一条约定）。
				GraphBuilder.SetBufferAccessFinal(CounterRef, ERHIAccess::SRVMask);
			}
			GraphBuilder.Execute();
		});
}

void ReleaseOnRenderThread(TArray<FPaletteBuffers>& Palettes)
{
	bool bAnyValid = false;
	for (const FPaletteBuffers& Buffers : Palettes) bAnyValid |= Buffers.PackedInstances.IsValid() || Buffers.Counter.IsValid();
	if (!bAnyValid)
	{
		Palettes.Reset();
		return;
	}

	ENQUEUE_RENDER_COMMAND(CSGroundStepRelease)(
		[Released = MoveTemp(Palettes)](FRHICommandListImmediate&) mutable
		{
			Released.Reset();
		});
	Palettes.Reset();
}
}
