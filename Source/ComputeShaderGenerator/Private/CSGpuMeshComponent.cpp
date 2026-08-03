#include "CSGpuMeshComponent.h"
#include "CSGpuMeshSceneProxy.h"

#include "RHI.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "RHICommandList.h"

namespace
{
float UnpackSnorm8(uint32 PackedValue, uint32 ByteIndex)
{
	const int8 SignedValue = static_cast<int8>((PackedValue >> (ByteIndex * 8u)) & 0xffu);
	return FMath::Clamp(float(SignedValue) / 127.0f, -1.0f, 1.0f);
}

FVector3f UnpackSnorm8888XYZ(uint32 PackedValue)
{
	return FVector3f(
		UnpackSnorm8(PackedValue, 0),
		UnpackSnorm8(PackedValue, 1),
		UnpackSnorm8(PackedValue, 2)).GetSafeNormal();
}
}

UCSGpuMeshComponent::UCSGpuMeshComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

FBoxSphereBounds UCSGpuMeshComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	const FBox Box = LocalBounds.IsValid ? LocalBounds : FBox(FVector(-100.0), FVector(100.0));
	return FBoxSphereBounds(Box.TransformBy(LocalToWorld));
}

void UCSGpuMeshComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	if (UMaterialInterface* Mat = GetRenderMaterial()) OutMaterials.Add(Mat);
}

bool UCSGpuMeshComponent::ReadbackMeshSync(FCSGpuMeshCPUData& OutMeshData) const
{
	OutMeshData.Reset();
	if (!IsInGameThread() || !IsRegistered())
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSGpuMesh] Readback rejected: GameThread=%d Registered=%d."),
			IsInGameThread() ? 1 : 0, IsRegistered() ? 1 : 0);
		return false;
	}

	// Complete any pending proxy recreation before capturing the proxy pointer.
	FlushRenderingCommands();
	FCSGpuMeshSceneProxy* Proxy = static_cast<FCSGpuMeshSceneProxy*>(GetSceneProxy());
	if (!Proxy)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSGpuMesh] Readback rejected: scene proxy is unavailable after flushing render commands."));
		return false;
	}

	// --- 1) read the GPU-decided vertex/index counts from the MeshCounters buffer
	FRHIGPUBufferReadback* CountersReadback = new FRHIGPUBufferReadback(TEXT("CSGpuMesh.CountersReadback"));
	bool bCountersQueued = false;
	ENQUEUE_RENDER_COMMAND(CSGpuMeshEnqueueCounters)(
		[Proxy, CountersReadback, &bCountersQueued](FRHICommandListImmediate& RHICmdList)
		{
			bCountersQueued = Proxy->EnqueueCountersReadback(RHICmdList, CountersReadback);
		});
	FlushRenderingCommands();
	if (!bCountersQueued)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSGpuMesh] Readback failed: MeshCounters buffer is unavailable."));
		delete CountersReadback;
		return false;
	}

	uint32 VertexCount = 0;
	uint32 IndexCount = 0;
	bool bCountersRead = false;
	ENQUEUE_RENDER_COMMAND(CSGpuMeshConsumeCounters)(
		[CountersReadback, &VertexCount, &IndexCount, &bCountersRead](FRHICommandListImmediate& RHICmdList)
		{
			if (!CountersReadback->IsReady()) RHICmdList.SubmitAndBlockUntilGPUIdle();
			if (CountersReadback->IsReady() && CountersReadback->GetGPUSizeBytes() >= sizeof(uint32) * 2u)
			{
				if (const uint32* Counts = static_cast<const uint32*>(CountersReadback->Lock(sizeof(uint32) * 2u)))
				{
					VertexCount = Counts[0];
					IndexCount = Counts[1];
					CountersReadback->Unlock();
					bCountersRead = true;
				}
			}
			delete CountersReadback;
		});
	FlushRenderingCommands();
	if (!bCountersRead)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSGpuMesh] Readback failed: MeshCounters GPU readback did not complete."));
		return false;
	}

	const uint32 VertexCapacity = Proxy->GetVertexCapacity();
	const uint32 IndexCapacity = Proxy->GetIndexCapacity();
	if (VertexCount < 3 || VertexCount > VertexCapacity)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSGpuMesh] Readback failed: vertex count %u invalid for capacity %u."), VertexCount, VertexCapacity);
		return false;
	}
	if (IndexCount < 3 || IndexCount % 3u != 0 || IndexCount > IndexCapacity)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSGpuMesh] Readback failed: index count %u invalid for capacity %u."), IndexCount, IndexCapacity);
		return false;
	}

	// --- 2) enqueue one readback per mesh stream (descriptor order)
	TArray<FCSGpuStreamDesc> ReadbackDescs;
	Proxy->GetMeshReadbackDescs(ReadbackDescs);
	if (ReadbackDescs.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSGpuMesh] Readback failed: proxy exposes no mesh-readback streams."));
		return false;
	}

	TArray<FRHIGPUBufferReadback*> StreamReadbacks;
	StreamReadbacks.Reserve(ReadbackDescs.Num());
	for (int32 i = 0; i < ReadbackDescs.Num(); ++i)
		StreamReadbacks.Add(new FRHIGPUBufferReadback(TEXT("CSGpuMesh.StreamReadback")));

	bool bMeshQueued = false;
	ENQUEUE_RENDER_COMMAND(CSGpuMeshEnqueueStreams)(
		[Proxy, VertexCount, IndexCount, StreamReadbacks, &bMeshQueued](FRHICommandListImmediate& RHICmdList)
		{
			bMeshQueued = Proxy->EnqueueMeshReadback(RHICmdList, VertexCount, IndexCount, StreamReadbacks);
		});
	FlushRenderingCommands();
	if (!bMeshQueued)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSGpuMesh] Readback failed: mesh stream copies could not be queued."));
		for (FRHIGPUBufferReadback* RB : StreamReadbacks) delete RB;
		return false;
	}

	// --- 3) lock each stream and fill the CPU data by semantic
	OutMeshData.Positions.SetNumUninitialized(VertexCount);
	OutMeshData.Normals.SetNumUninitialized(VertexCount);
	OutMeshData.Tangents.SetNumUninitialized(VertexCount);
	OutMeshData.TexCoords().SetNumUninitialized(VertexCount);
	OutMeshData.Indices.SetNumUninitialized(IndexCount);
	// 按注册的 TexCoord 流决定实际有几条 UV 通道，并为每条预分配（通道 0 已在上面分配）。
	{
		int32 HighestTexCoordIndex = 0;
		for (const FCSGpuStreamDesc& Desc : ReadbackDescs)
		{
			if (Desc.CpuSemantic != ECSGpuMeshSemantic::TexCoord) continue;
			HighestTexCoordIndex = FMath::Max<int32>(HighestTexCoordIndex, Desc.TexCoordIndex);
		}
		OutMeshData.NumTexCoordChannels = FMath::Clamp(
			HighestTexCoordIndex + 1, 1, FCSGpuMeshCPUData::MaxTexCoordChannels);
		for (int32 Channel = 1; Channel < OutMeshData.NumTexCoordChannels; ++Channel)
		{
			OutMeshData.TexCoordChannels[Channel].SetNumZeroed(VertexCount);
		}
	}

	bool bMeshRead = false;
	ENQUEUE_RENDER_COMMAND(CSGpuMeshConsumeStreams)(
		[StreamReadbacks, ReadbackDescs, VertexCount, IndexCount, &OutMeshData, &bMeshRead](FRHICommandListImmediate& RHICmdList)
		{
			bool bAnyNotReady = false;
			for (FRHIGPUBufferReadback* RB : StreamReadbacks)
				if (!RB->IsReady()) bAnyNotReady = true;
			if (bAnyNotReady) RHICmdList.SubmitAndBlockUntilGPUIdle();

			bool bAllReady = true;
			for (int32 i = 0; i < ReadbackDescs.Num(); ++i)
			{
				const FCSGpuStreamDesc& D = ReadbackDescs[i];
				const uint32 Units = (D.CountSource == ECSGpuCountSource::PerIndex) ? IndexCount : VertexCount;
				const uint32 Bytes = Units * D.ElementsPerUnit * D.BytesPerElement;
				if (!StreamReadbacks[i]->IsReady() || StreamReadbacks[i]->GetGPUSizeBytes() < Bytes) { bAllReady = false; break; }
			}

			if (bAllReady)
			{
				bool bLockedOk = true;
				for (int32 i = 0; i < ReadbackDescs.Num() && bLockedOk; ++i)
				{
					const FCSGpuStreamDesc& D = ReadbackDescs[i];
					const uint32 Units = (D.CountSource == ECSGpuCountSource::PerIndex) ? IndexCount : VertexCount;
					const uint32 Bytes = Units * D.ElementsPerUnit * D.BytesPerElement;
					const void* Raw = StreamReadbacks[i]->Lock(Bytes);
					if (!Raw) { bLockedOk = false; break; }

					switch (D.CpuSemantic)
					{
					case ECSGpuMeshSemantic::Position:
					{
						const float* P = static_cast<const float*>(Raw);
						for (uint32 v = 0; v < VertexCount; ++v)
							OutMeshData.Positions[v] = FVector3f(P[v * 3 + 0], P[v * 3 + 1], P[v * 3 + 2]);
						break;
					}
					case ECSGpuMeshSemantic::TangentBasis:
					{
						const uint32* T = static_cast<const uint32*>(Raw);
						for (uint32 v = 0; v < VertexCount; ++v)
						{
							OutMeshData.Tangents[v] = UnpackSnorm8888XYZ(T[v * 2 + 0]);
							OutMeshData.Normals[v] = UnpackSnorm8888XYZ(T[v * 2 + 1]);
						}
						break;
					}
					case ECSGpuMeshSemantic::TexCoord:
					{
						// 每条 TexCoord 流按自己的 TexCoordIndex 落到对应通道，多条 UV 才能各归各位。
						const int32 Channel = FMath::Clamp<int32>(
							D.TexCoordIndex, 0, FCSGpuMeshCPUData::MaxTexCoordChannels - 1);
						if (Channel >= OutMeshData.NumTexCoordChannels) break;
						TArray<FVector2f>& ChannelUVs = OutMeshData.TexCoordChannels[Channel];
						if (ChannelUVs.Num() != int32(VertexCount)) ChannelUVs.SetNumUninitialized(VertexCount);
						const float* UV = static_cast<const float*>(Raw);
						for (uint32 v = 0; v < VertexCount; ++v)
							ChannelUVs[v] = FVector2f(UV[v * 2 + 0], UV[v * 2 + 1]);
						break;
					}
					case ECSGpuMeshSemantic::Index:
					{
						const uint32* Idx = static_cast<const uint32*>(Raw);
						for (uint32 k = 0; k < IndexCount; ++k)
							OutMeshData.Indices[k] = Idx[k];
						break;
					}
					default:
						break;
					}
					StreamReadbacks[i]->Unlock();
				}
				bMeshRead = bLockedOk;
			}

			for (FRHIGPUBufferReadback* RB : StreamReadbacks) delete RB;
		});
	FlushRenderingCommands();

	if (!bMeshRead)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSGpuMesh] Readback failed: mesh stream copies did not complete for %u verts / %u indices."), VertexCount, IndexCount);
		OutMeshData.Reset();
		return false;
	}
	return OutMeshData.IsValid();
}
