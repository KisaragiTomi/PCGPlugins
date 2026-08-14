#include "CSMeshPool.h"

#include "CSGpuMemoryBudget.h"
#include "CSMesh.h"

DEFINE_LOG_CATEGORY_STATIC(LogCSMeshPool, Log, All);

namespace
{
int64 CSMeshPool_MeshBytes(const UCSMesh* Mesh)
{
	const FCSMeshResident* Resident = Mesh ? Mesh->GetResidentPtr() : nullptr;
	return Resident ? Resident->GetAllocatedBytes() : 0;
}
}

UCSMesh* UCSMeshPool::RequestMesh(int32 VertexCapacity, int32 IndexCapacity)
{
	const int32 WantVertices = FMath::Max(VertexCapacity, 3);
	const int32 WantIndices = FMath::Max(IndexCapacity, 3);

	// Prefer the tightest idle mesh that already fits: reusing an oversized allocation wastes
	// the VRAM the cap is there to protect, and growing a small one costs a realloc + copy.
	int32 BestIndex = INDEX_NONE;
	int64 BestBytes = MAX_int64;
	for (int32 Index = 0; Index < CachedMeshes.Num(); ++Index)
	{
		UCSMesh* Candidate = CachedMeshes[Index];
		if (!Candidate) continue;
		if (Candidate->GetVertexCapacity() < WantVertices || Candidate->GetIndexCapacity() < WantIndices) continue;
		const int64 Bytes = CSMeshPool_MeshBytes(Candidate);
		if (Bytes < BestBytes)
		{
			BestBytes = Bytes;
			BestIndex = Index;
		}
	}

	// Nothing fits: grow the largest idle mesh rather than allocating a second one alongside it.
	if (BestIndex == INDEX_NONE && CachedMeshes.Num() > 0)
	{
		int64 LargestBytes = -1;
		for (int32 Index = 0; Index < CachedMeshes.Num(); ++Index)
		{
			const int64 Bytes = CSMeshPool_MeshBytes(CachedMeshes[Index]);
			if (Bytes > LargestBytes)
			{
				LargestBytes = Bytes;
				BestIndex = Index;
			}
		}
	}

	UCSMesh* Mesh = nullptr;
	if (BestIndex != INDEX_NONE)
	{
		Mesh = CachedMeshes[BestIndex];
		CachedMeshes.RemoveAtSwap(BestIndex);
	}
	if (!Mesh) Mesh = NewObject<UCSMesh>(this);

	Mesh->EnsureCapacitySync(WantVertices, WantIndices);
	Mesh->Reset();
	Mesh->Materials.Reset();
	ActiveMeshes.Add(Mesh);
	return Mesh;
}

void UCSMeshPool::ReturnMesh(UCSMesh* Mesh)
{
	if (!Mesh) return;
	ActiveMeshes.RemoveSwap(Mesh);
	if (CachedMeshes.Contains(Mesh)) return;

	// Clear the contents, keep the allocation. This is the whole reason the pool exists, and
	// it is exactly where the CPU pool does the opposite.
	Mesh->Reset();
	Mesh->Materials.Reset();
	CachedMeshes.Add(Mesh);
	EnforceMemoryLimit();
}

void UCSMeshPool::ReturnAllMeshes()
{
	TArray<TObjectPtr<UCSMesh>> Handed = MoveTemp(ActiveMeshes);
	ActiveMeshes.Reset();
	for (const TObjectPtr<UCSMesh>& Mesh : Handed) ReturnMesh(Mesh);
	// Also re-check with nothing to return: the ceiling may have moved (either because the
	// caller lowered it, or because the device's available VRAM did) since the last return.
	EnforceMemoryLimit();
}

void UCSMeshPool::FreeAllMeshes()
{
	ReturnAllMeshes();
	for (const TObjectPtr<UCSMesh>& Mesh : CachedMeshes)
	{
		if (Mesh) Mesh->ReleaseSync();
	}
	CachedMeshes.Reset();
}

int64 UCSMeshPool::GetCachedBytes() const
{
	int64 Bytes = 0;
	for (const TObjectPtr<UCSMesh>& Mesh : CachedMeshes) Bytes += CSMeshPool_MeshBytes(Mesh);
	return Bytes;
}

int64 UCSMeshPool::GetTotalBytes() const
{
	int64 Bytes = GetCachedBytes();
	for (const TObjectPtr<UCSMesh>& Mesh : ActiveMeshes) Bytes += CSMeshPool_MeshBytes(Mesh);
	return Bytes;
}

int64 UCSMeshPool::GetCachedBytesLimit() const
{
	if (MaxCachedBytesOverride > 0) return MaxCachedBytesOverride;

	// No fixed default: what "too much" means depends on the device and on what else is
	// already resident, which is precisely what the budget facility measures.
	const CSGpuMemoryBudget::FMemorySnapshot Snapshot = CSGpuMemoryBudget::QueryMemorySnapshot();
	const int64 Available = Snapshot.AvailableVideoMemory > 0 ? Snapshot.AvailableVideoMemory : Snapshot.TotalVideoMemory;
	if (Available <= 0) return MAX_int64; // budget unknown: do not evict on a guess
	return int64(double(Available) * double(FMath::Clamp(MaxCachedVideoMemoryRatio, 0.0f, 1.0f)));
}

void UCSMeshPool::EnforceMemoryLimit()
{
	const int64 Limit = GetCachedBytesLimit();
	if (Limit == MAX_int64) return;

	int64 Cached = GetCachedBytes();
	while (Cached > Limit && CachedMeshes.Num() > 0)
	{
		// Evict the largest first: it frees the most for one release and is the least likely
		// to fit the next request anyway.
		int32 LargestIndex = 0;
		int64 LargestBytes = -1;
		for (int32 Index = 0; Index < CachedMeshes.Num(); ++Index)
		{
			const int64 Bytes = CSMeshPool_MeshBytes(CachedMeshes[Index]);
			if (Bytes > LargestBytes)
			{
				LargestBytes = Bytes;
				LargestIndex = Index;
			}
		}

		if (UCSMesh* Evicted = CachedMeshes[LargestIndex]) Evicted->ReleaseSync();
		CachedMeshes.RemoveAtSwap(LargestIndex);
		Cached -= FMath::Max(LargestBytes, 0ll);

		UE_LOG(LogCSMeshPool, Verbose, TEXT("[CSMeshPool] Evicted %lld bytes; cached now %lld / %lld."),
			LargestBytes, Cached, Limit);
	}
}
