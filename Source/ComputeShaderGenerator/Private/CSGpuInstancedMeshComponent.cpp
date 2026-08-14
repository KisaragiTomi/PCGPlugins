#include "CSGpuInstancedMeshComponent.h"
#include "CSGpuInstancedMeshSceneProxy.h"

#include "CSMesh.h"
#include "CSMeshOps.h"

#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Rendering/StaticMeshVertexBuffer.h"
#include "Rendering/PositionVertexBuffer.h"
#include "Rendering/ColorVertexBuffer.h"
#include "StaticMeshResources.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHI.h"

DEFINE_LOG_CATEGORY_STATIC(LogCSGpuInstancedMesh, Log, All);

namespace
{
	/** Same packing as CSDirectMesh.usf / RoadBuilder.usf so the tangent stream stays one format
	 *  across every GPU mesh path (VET_PackedNormal / PF_R8G8B8A8_SNORM). */
	uint32 PackSnorm8888(const FVector4f& V)
	{
		const int32 X = FMath::RoundToInt(FMath::Clamp(V.X, -1.0f, 1.0f) * 127.0f);
		const int32 Y = FMath::RoundToInt(FMath::Clamp(V.Y, -1.0f, 1.0f) * 127.0f);
		const int32 Z = FMath::RoundToInt(FMath::Clamp(V.Z, -1.0f, 1.0f) * 127.0f);
		const int32 W = FMath::RoundToInt(FMath::Clamp(V.W, -1.0f, 1.0f) * 127.0f);
		return (uint32(X) & 0xFF) | ((uint32(Y) & 0xFF) << 8) | ((uint32(Z) & 0xFF) << 16) | ((uint32(W) & 0xFF) << 24);
	}

	/** Spreads the low 10 bits of V so three of them interleave into a 30-bit Morton code. */
	uint32 SpreadBits10(uint32 V)
	{
		V &= 0x3FF;
		V = (V | (V << 16)) & 0x030000FF;
		V = (V | (V << 8)) & 0x0300F00F;
		V = (V | (V << 4)) & 0x030C30C3;
		V = (V | (V << 2)) & 0x09249249;
		return V;
	}

	uint32 MortonCode(const FVector3f& Normalized)
	{
		const uint32 X = uint32(FMath::Clamp(Normalized.X, 0.0f, 1.0f) * 1023.0f);
		const uint32 Y = uint32(FMath::Clamp(Normalized.Y, 0.0f, 1.0f) * 1023.0f);
		const uint32 Z = uint32(FMath::Clamp(Normalized.Z, 0.0f, 1.0f) * 1023.0f);
		return SpreadBits10(X) | (SpreadBits10(Y) << 1) | (SpreadBits10(Z) << 2);
	}

	/** Deterministic 0..1 value per instance, fed to the material's PerInstanceRandom. */
	float InstanceRandom(int32 Index)
	{
		uint32 H = uint32(Index) * 747796405u + 2891336453u;
		H = ((H >> ((H >> 28) + 4u)) ^ H) * 277803737u;
		H = (H >> 22) ^ H;
		return float(H & 0xFFFFFFu) / float(0x1000000u);
	}
}

UCSGpuInstancedMeshComponent::UCSGpuInstancedMeshComponent()
{
	// GPU-Scene instance culling overrides custom indirect args in the Virtual Shadow Map passes,
	// so indirect-drawn meshes cannot cast VSM shadows (same limitation as the other GPU meshes).
	CastShadow = false;
	bUseAsOccluder = false;
}

// -----------------------------------------------------------------------------
// Base mesh
// -----------------------------------------------------------------------------

void UCSGpuInstancedMeshComponent::SetBaseMesh(UStaticMesh* InMesh)
{
	if (BaseMesh == InMesh && !bBaseMeshIsExternal) return;

	BaseMesh = InMesh;
	bBaseMeshIsExternal = false;
	RebuildBaseMeshSnapshot();
}

void UCSGpuInstancedMeshComponent::SetBaseMeshFromGpuData(const FCSGpuMeshCPUData& InMeshData)
{
	BaseMeshSnapshot.Reset();
	bBaseMeshIsExternal = true;

	if (!InMeshData.IsValid())
	{
		UE_LOG(LogCSGpuInstancedMesh, Warning, TEXT("SetBaseMeshFromGpuData: mesh data failed IsValid(); instancing disabled."));
		RebuildInstanceData();
		MarkRenderStateDirty();
		return;
	}

	const int32 NumVerts = InMeshData.Positions.Num();
	const bool bPerCorner = InMeshData.AttrLayout == FCSGpuMeshCPUData::EAttrLayout::PerCorner;

	BaseMeshSnapshot.Positions = InMeshData.Positions;
	BaseMeshSnapshot.Indices = InMeshData.Indices;
	BaseMeshSnapshot.TangentBasis.SetNumUninitialized(NumVerts * 2);
	BaseMeshSnapshot.TexCoords.SetNumUninitialized(NumVerts);
	BaseMeshSnapshot.Colors.SetNumUninitialized(NumVerts);

	// Per-corner attributes cannot be indexed by vertex; take the first corner that references
	// each vertex rather than silently reading out of range.
	TArray<int32> AttrIndexForVertex;
	if (bPerCorner)
	{
		AttrIndexForVertex.Init(INDEX_NONE, NumVerts);
		for (int32 Corner = 0; Corner < InMeshData.Indices.Num(); ++Corner)
		{
			const int32 V = int32(InMeshData.Indices[Corner]);
			if (AttrIndexForVertex.IsValidIndex(V) && AttrIndexForVertex[V] == INDEX_NONE) AttrIndexForVertex[V] = Corner;
		}
	}

	for (int32 V = 0; V < NumVerts; ++V)
	{
		const int32 A = bPerCorner ? AttrIndexForVertex[V] : V;

		FVector3f Normal(0.0f, 0.0f, 1.0f);
		FVector3f Tangent(1.0f, 0.0f, 0.0f);
		if (InMeshData.Normals.IsValidIndex(A)) Normal = InMeshData.Normals[A].GetSafeNormal(UE_SMALL_NUMBER, FVector3f(0.0f, 0.0f, 1.0f));
		if (InMeshData.Tangents.IsValidIndex(A)) Tangent = InMeshData.Tangents[A].GetSafeNormal(UE_SMALL_NUMBER, FVector3f(1.0f, 0.0f, 0.0f));
		const float Sign = InMeshData.BinormalSigns.IsValidIndex(A) ? InMeshData.BinormalSigns[A] : 1.0f;

		BaseMeshSnapshot.TangentBasis[V * 2 + 0] = PackSnorm8888(FVector4f(Tangent, 0.0f));
		BaseMeshSnapshot.TangentBasis[V * 2 + 1] = PackSnorm8888(FVector4f(Normal, Sign >= 0.0f ? 1.0f : -1.0f));

		BaseMeshSnapshot.TexCoords[V] = InMeshData.TexCoordChannels[0].IsValidIndex(A) ? InMeshData.TexCoordChannels[0][A] : FVector2f::ZeroVector;

		FLinearColor Color = FLinearColor::White;
		if (InMeshData.Colors.IsValidIndex(A))
		{
			const FVector4f& C = InMeshData.Colors[A];
			Color = FLinearColor(C.X, C.Y, C.Z, C.W);
		}
		// ToPackedARGB reproduces FColor's own B,G,R,A memory order on little-endian, which is
		// what the manual-fetch colour path expects (FMANUALFETCH_COLOR_COMPONENT_SWIZZLE = .bgra).
		BaseMeshSnapshot.Colors[V] = Color.ToFColor(false).ToPackedARGB();
	}

	FBox MeshBounds(ForceInit);
	for (const FVector3f& P : BaseMeshSnapshot.Positions) MeshBounds += FVector(P);
	BaseMeshSnapshot.LocalBounds = MeshBounds;

	FCSGpuInstancedLODRange LOD0;
	LOD0.FirstIndex = 0;
	LOD0.NumIndices = uint32(BaseMeshSnapshot.Indices.Num());
	LOD0.BaseVertex = 0;
	LOD0.ScreenSize = 0.0f; // single LOD: always selected
	BaseMeshSnapshot.LODs.Add(LOD0);

	RebuildInstanceData();
	MarkRenderStateDirty();
	UpdateBounds();
}

void UCSGpuInstancedMeshComponent::RebuildBaseMeshSnapshot()
{
	if (bBaseMeshIsExternal) return;

	BaseMeshSnapshot.Reset();

	const FStaticMeshRenderData* RenderData = BaseMesh ? BaseMesh->GetRenderData() : nullptr;
	if (!RenderData || RenderData->LODResources.Num() == 0)
	{
		RebuildInstanceData();
		MarkRenderStateDirty();
		UpdateBounds();
		return;
	}

	const int32 NumLODs = FMath::Min(RenderData->LODResources.Num(), CS_GPU_INSTANCED_MAX_LODS);
	for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
	{
		const FStaticMeshLODResources& LOD = RenderData->LODResources[LODIndex];
		const uint32 NumVerts = LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();
		const uint32 NumTangentVerts = LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumVertices();
		if (NumVerts == 0 || NumTangentVerts != NumVerts) break;

		TArray<uint32> LODIndices;
		LOD.IndexBuffer.GetCopy(LODIndices);
		if (LODIndices.Num() < 3) break;

		FCSGpuInstancedLODRange Range;
		Range.BaseVertex = uint32(BaseMeshSnapshot.Positions.Num());
		Range.FirstIndex = uint32(BaseMeshSnapshot.Indices.Num());
		Range.NumIndices = uint32(LODIndices.Num());
		// Screen size at which this LOD takes over. LOD0's threshold is never tested (it is the
		// fallback when nothing smaller matches), so only 1..N-1 matter.
		Range.ScreenSize = (LODIndex < MAX_STATIC_MESH_LODS) ? RenderData->ScreenSize[LODIndex].Default : 0.0f;

		const bool bHasColors = LOD.VertexBuffers.ColorVertexBuffer.GetNumVertices() == NumVerts;
		const bool bHasUVs = LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords() > 0;

		BaseMeshSnapshot.Positions.Reserve(BaseMeshSnapshot.Positions.Num() + int32(NumVerts));
		BaseMeshSnapshot.TangentBasis.Reserve(BaseMeshSnapshot.TangentBasis.Num() + int32(NumVerts) * 2);
		BaseMeshSnapshot.TexCoords.Reserve(BaseMeshSnapshot.TexCoords.Num() + int32(NumVerts));
		BaseMeshSnapshot.Colors.Reserve(BaseMeshSnapshot.Colors.Num() + int32(NumVerts));

		for (uint32 V = 0; V < NumVerts; ++V)
		{
			BaseMeshSnapshot.Positions.Add(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(V));

			const FVector3f TangentX = LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentX(V);
			const FVector4f TangentZ = LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(V);
			BaseMeshSnapshot.TangentBasis.Add(PackSnorm8888(FVector4f(TangentX, 0.0f)));
			BaseMeshSnapshot.TangentBasis.Add(PackSnorm8888(TangentZ));

			BaseMeshSnapshot.TexCoords.Add(bHasUVs ? LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(V, 0) : FVector2f::ZeroVector);
			BaseMeshSnapshot.Colors.Add(bHasColors ? LOD.VertexBuffers.ColorVertexBuffer.VertexColor(V).ToPackedARGB() : 0xFFFFFFFFu);
		}

		// Indices are stored relative to the LOD's own vertex block; DrawIndexedIndirect adds
		// BaseVertexLocation, so they go in unmodified.
		BaseMeshSnapshot.Indices.Append(LODIndices);
		BaseMeshSnapshot.LODs.Add(Range);
	}

	if (BaseMeshSnapshot.LODs.Num() == 0)
	{
		UE_LOG(LogCSGpuInstancedMesh, Warning,
			TEXT("%s: could not read vertex data from '%s'. Enable 'Allow CPU Access' on the mesh (required outside the editor)."),
			*GetPathName(), *GetNameSafe(BaseMesh));
	}
	else
	{
		BaseMeshSnapshot.LocalBounds = BaseMesh->GetBoundingBox();
	}

	RebuildInstanceData();
	MarkRenderStateDirty();
	UpdateBounds();
}

// -----------------------------------------------------------------------------
// Instances — CPU source
// -----------------------------------------------------------------------------

int32 UCSGpuInstancedMeshComponent::AddInstance(const FTransform& InstanceTransform, bool bWorldSpace)
{
	const FTransform Local = bWorldSpace ? InstanceTransform.GetRelativeTransform(GetComponentTransform()) : InstanceTransform;
	const int32 Index = PerInstanceTransforms.Add(Local);

	RebuildInstanceData();
	MarkRenderStateDirty();
	UpdateBounds();
	return Index;
}

TArray<int32> UCSGpuInstancedMeshComponent::AddInstances(const TArray<FTransform>& InstanceTransforms, bool bWorldSpace)
{
	TArray<int32> Indices;
	Indices.Reserve(InstanceTransforms.Num());

	const FTransform ComponentTransform = GetComponentTransform();
	for (const FTransform& T : InstanceTransforms) Indices.Add(PerInstanceTransforms.Add(bWorldSpace ? T.GetRelativeTransform(ComponentTransform) : T));

	RebuildInstanceData();
	MarkRenderStateDirty();
	UpdateBounds();
	return Indices;
}

void UCSGpuInstancedMeshComponent::SetInstances(const TArray<FTransform>& InstanceTransforms, bool bWorldSpace)
{
	PerInstanceTransforms.Reset(InstanceTransforms.Num());

	const FTransform ComponentTransform = GetComponentTransform();
	for (const FTransform& T : InstanceTransforms) PerInstanceTransforms.Add(bWorldSpace ? T.GetRelativeTransform(ComponentTransform) : T);

	RebuildInstanceData();
	MarkRenderStateDirty();
	UpdateBounds();
}

bool UCSGpuInstancedMeshComponent::RemoveInstance(int32 InstanceIndex)
{
	if (!PerInstanceTransforms.IsValidIndex(InstanceIndex)) return false;

	PerInstanceTransforms.RemoveAtSwap(InstanceIndex);
	RebuildInstanceData();
	MarkRenderStateDirty();
	UpdateBounds();
	return true;
}

bool UCSGpuInstancedMeshComponent::UpdateInstanceTransform(int32 InstanceIndex, const FTransform& NewInstanceTransform, bool bWorldSpace, bool bMarkRenderStateDirty)
{
	if (!PerInstanceTransforms.IsValidIndex(InstanceIndex)) return false;

	PerInstanceTransforms[InstanceIndex] = bWorldSpace ? NewInstanceTransform.GetRelativeTransform(GetComponentTransform()) : NewInstanceTransform;

	// The GPU upload is what bMarkRenderStateDirty now really gates: it blocks on a render flush, so
	// a caller updating a run of instances must be able to pay for it once rather than per instance.
	RebuildInstanceData(bMarkRenderStateDirty);
	if (bMarkRenderStateDirty)
	{
		MarkRenderStateDirty();
		UpdateBounds();
	}
	return true;
}

bool UCSGpuInstancedMeshComponent::GetInstanceTransform(int32 InstanceIndex, FTransform& OutInstanceTransform, bool bWorldSpace) const
{
	if (!PerInstanceTransforms.IsValidIndex(InstanceIndex)) return false;

	OutInstanceTransform = PerInstanceTransforms[InstanceIndex];
	if (bWorldSpace) OutInstanceTransform *= GetComponentTransform();
	return true;
}

void UCSGpuInstancedMeshComponent::ClearInstances()
{
	PerInstanceTransforms.Reset();
	RebuildInstanceData();
	MarkRenderStateDirty();
	UpdateBounds();
}

// -----------------------------------------------------------------------------
// Instances — GPU source
// -----------------------------------------------------------------------------

void UCSGpuInstancedMeshComponent::SetInstanceSourceFromPoints(const FCSGpuInstancePointSourceGPU& InSource)
{
	GpuInstanceSource.Reset();
	GpuPointSource = InSource;
	RebuildInstanceData();
	RecreateRenderState_Concurrent();
	UpdateBounds();
}

void UCSGpuInstancedMeshComponent::SetInstanceSourceGPU(const FCSGpuInstanceSourceGPU& InSource)
{
	GpuPointSource.Reset();
	GpuInstanceSource = InSource;
	RebuildInstanceData();

	// A GPU source is normally handed over right before a save or a readback, so the new proxy
	// has to exist immediately — MarkRenderStateDirty alone defers to the end-of-frame update.
	RecreateRenderState_Concurrent();
	UpdateBounds();
}

void UCSGpuInstancedMeshComponent::ClearInstanceSourceGPU()
{
	if (!GpuInstanceSource.IsValid() && !GpuPointSource.IsValid()) return;

	GpuInstanceSource.Reset();
	GpuPointSource.Reset();
	RebuildInstanceData();
	RecreateRenderState_Concurrent();
	UpdateBounds();
}

// -----------------------------------------------------------------------------
// Cluster build
// -----------------------------------------------------------------------------

void UCSGpuInstancedMeshComponent::RebuildInstanceData(bool bRebuildGpuMesh)
{
	RebuildInstancePacking();
	if (bRebuildGpuMesh) RebuildGpuMesh();
}

void UCSGpuInstancedMeshComponent::RebuildInstancePacking()
{
	PackedInstances.Reset();
	ClusterBounds.Reset();

	if (GpuInstanceSource.IsValid())
	{
		// The instance set lives on the GPU; only the bounds are the component's business.
		LocalBounds = GpuInstanceSource.LocalBounds;
		return;
	}

	if (GpuPointSource.IsValid())
	{
		// Point positions are world-space, so the bounds arrive world-space too; the base mesh's
		// own extent has to be added because an instance stands out of its point.
		const FBox WorldBounds = GpuPointSource.WorldBounds;
		LocalBounds = WorldBounds.IsValid ? WorldBounds.InverseTransformBy(GetComponentTransform()) : FBox(ForceInit);
		if (LocalBounds.IsValid && BaseMeshSnapshot.LocalBounds.IsValid)
		{
			const double Reach = BaseMeshSnapshot.LocalBounds.GetExtent().Size() * FMath::Max(GpuPointSource.InstanceScale, UE_KINDA_SMALL_NUMBER);
			LocalBounds = LocalBounds.ExpandBy(Reach);
		}
		return;
	}

	const int32 NumInstances = PerInstanceTransforms.Num();
	if (NumInstances == 0 || !BaseMeshSnapshot.IsValid())
	{
		LocalBounds = FBox(ForceInit);
		return;
	}

	// Per-instance culling sphere: the base mesh's LOD0 sphere pushed through the instance
	// transform. Non-uniform scale is handled conservatively by the largest axis.
	const FVector BaseCentre = BaseMeshSnapshot.LocalBounds.GetCenter();
	const float BaseRadius = float(BaseMeshSnapshot.LocalBounds.GetExtent().Size());

	TArray<FVector3f> Centres;
	TArray<float> Radii;
	Centres.SetNumUninitialized(NumInstances);
	Radii.SetNumUninitialized(NumInstances);

	FBox TotalBounds(ForceInit);
	for (int32 i = 0; i < NumInstances; ++i)
	{
		const FTransform& T = PerInstanceTransforms[i];
		const FVector Centre = T.TransformPosition(BaseCentre);
		const FVector AbsScale = T.GetScale3D().GetAbs();
		const float Radius = BaseRadius * float(FMath::Max3(AbsScale.X, AbsScale.Y, AbsScale.Z));

		Centres[i] = FVector3f(Centre);
		Radii[i] = Radius;
		TotalBounds += FBox(Centre - FVector(Radius), Centre + FVector(Radius));
	}
	LocalBounds = TotalBounds;

	// Morton order gives the clusters spatial locality, which is what makes the coarse cull
	// level worth running at all. A real BVH would reject more; this costs one sort.
	const FVector3f Origin = FVector3f(TotalBounds.Min);
	const FVector3f Size = FVector3f(TotalBounds.GetSize()).ComponentMax(FVector3f(UE_KINDA_SMALL_NUMBER));

	TArray<int32> Order;
	Order.SetNumUninitialized(NumInstances);
	TArray<uint32> Codes;
	Codes.SetNumUninitialized(NumInstances);
	for (int32 i = 0; i < NumInstances; ++i)
	{
		Order[i] = i;
		Codes[i] = MortonCode((Centres[i] - Origin) / Size);
	}
	Order.Sort([&Codes](int32 A, int32 B) { return Codes[A] < Codes[B]; });

	const int32 ClusterSize = FMath::Clamp(InstancesPerCluster, 1, 4096);
	const int32 NumClusters = FMath::DivideAndRoundUp(NumInstances, ClusterSize);

	PackedInstances.SetNumUninitialized(NumInstances * 5);
	ClusterBounds.SetNumUninitialized(NumClusters);

	for (int32 Cluster = 0; Cluster < NumClusters; ++Cluster)
	{
		const int32 First = Cluster * ClusterSize;
		const int32 Count = FMath::Min(ClusterSize, NumInstances - First);

		FVector3f Min(UE_BIG_NUMBER), Max(-UE_BIG_NUMBER);
		for (int32 Slot = 0; Slot < Count; ++Slot)
		{
			const int32 Src = Order[First + Slot];
			const FTransform& T = PerInstanceTransforms[Src];
			const FMatrix44f M = FMatrix44f(T.ToMatrixWithScale());

			// Rows of the instance-to-component 3x3 + the origin, exactly the layout
			// LocalVertexFactory.ush's GetInstanceTransform() reconstructs.
			const int32 Dst = (First + Slot) * 5;
			PackedInstances[Dst + 0] = FVector4f(M.M[0][0], M.M[0][1], M.M[0][2], 0.0f);
			PackedInstances[Dst + 1] = FVector4f(M.M[1][0], M.M[1][1], M.M[1][2], 0.0f);
			PackedInstances[Dst + 2] = FVector4f(M.M[2][0], M.M[2][1], M.M[2][2], 0.0f);
			PackedInstances[Dst + 3] = FVector4f(M.M[3][0], M.M[3][1], M.M[3][2], InstanceRandom(Src));
			PackedInstances[Dst + 4] = FVector4f(Centres[Src], Radii[Src]);

			Min = Min.ComponentMin(Centres[Src] - FVector3f(Radii[Src]));
			Max = Max.ComponentMax(Centres[Src] + FVector3f(Radii[Src]));
		}

		const FVector3f Centre = (Min + Max) * 0.5f;
		ClusterBounds[Cluster] = FVector4f(Centre, (Max - Centre).Size());
	}
}

// -----------------------------------------------------------------------------
// The retained buffer set
// -----------------------------------------------------------------------------

uint32 UCSGpuInstancedMeshComponent::ResolveInstanceCapacity(uint32 LiveInstanceCount) const
{
	// A floor, so a handful of instances still gets a buffer worth having and the first few
	// AddInstance calls change nothing.
	constexpr uint32 MinCapacity = 64u;

	const uint32 Held = GpuLayout.InstanceCapacity;
	const bool bTooSmall = LiveInstanceCount > Held;
	// Hysteresis: only hand capacity back once three quarters of it are idle. Mirrors what
	// UCSMesh::ShrinkSlackRatio does for the geometry streams, and for the same reason — an
	// instance set that oscillates must not churn its buffers on every swing.
	const bool bMostlyIdle = LiveInstanceCount * 4u < Held;
	if (!bTooSmall && !bMostlyIdle) return FMath::Max(Held, LiveInstanceCount);

	return FMath::Max(MinCapacity, LiveInstanceCount + LiveInstanceCount / 2u);
}

void UCSGpuInstancedMeshComponent::ReleaseGpuMesh()
{
	GpuLayout = FCSGpuInstancedGpuLayout();
	if (!InstancedGpuMesh) return;

	// Not merely unbound: the buffers were sized for an instance set that no longer exists, and a
	// component with nothing to draw is exactly when the VRAM is worth handing back. The live proxy
	// keeps its own references to the pooled buffers, so it goes on drawing from valid memory until
	// the render-state recreation its caller is about to trigger replaces it.
	const FCSMeshResident* Resident = InstancedGpuMesh->GetResidentPtr();
	if (Resident && Resident->IsAllocated()) InstancedGpuMesh->ReleaseSync();
}

void UCSGpuInstancedMeshComponent::RebuildGpuMesh()
{
	// Deferred to OnRegister while unregistered. Nothing can draw an unregistered component, and
	// this blocks on a render flush — PostLoad rebuilds every one of these in the level, and a
	// render round trip each would show up as load time for no visible result. Released rather than
	// left alone, because what is in the buffers has already stopped matching the instance set.
	if (!IsRegistered())
	{
		ReleaseGpuMesh();
		return;
	}

	const bool bPackedGpuSource = GpuInstanceSource.IsValid();
	const bool bPointGpuSource = GpuPointSource.IsValid();
	const bool bHasInstances = bPackedGpuSource || bPointGpuSource || PackedInstances.Num() > 0;
	if (!BaseMeshSnapshot.IsValid() || !bHasInstances)
	{
		ReleaseGpuMesh();
		return;
	}

	// --- what the buffers have to be sized for
	FCSGpuInstancedGpuLayout NewLayout;
	NewLayout.NumLODs = uint32(FMath::Clamp(BaseMeshSnapshot.LODs.Num(), 1, CS_GPU_INSTANCED_MAX_LODS));
	if (bPackedGpuSource || bPointGpuSource)
	{
		// Instances live on the GPU: there is no cluster table to build from, so the coarse level is
		// skipped and every instance goes through the fine cull. The producer's buffer capacity is
		// already a capacity, so it needs no ratchet of ours; its counter carries the live count.
		NewLayout.InstanceCapacity = bPackedGpuSource ? GpuInstanceSource.Capacity : GpuPointSource.Capacity;
		NewLayout.NumSourceInstances = NewLayout.InstanceCapacity;
	}
	else
	{
		NewLayout.NumSourceInstances = uint32(PackedInstances.Num() / 5);
		NewLayout.ClusterSize = uint32(FMath::Clamp(InstancesPerCluster, 1, 4096));
		NewLayout.InstanceCapacity = ResolveInstanceCapacity(NewLayout.NumSourceInstances);
		NewLayout.NumClusters = uint32(FMath::DivideAndRoundUp(NewLayout.NumSourceInstances, NewLayout.ClusterSize));
	}
	if (!NewLayout.IsValid())
	{
		ReleaseGpuMesh();
		return;
	}

	if (!InstancedGpuMesh) InstancedGpuMesh = NewObject<UCSMesh>(this);
	// One entry, and not the material anything draws with — the proxy draws every LOD with
	// InstanceMaterial directly. This is where a save of the base mesh gets its single slot from,
	// which is the same table the material-id stream is cleared to index.
	InstancedGpuMesh->SetMaterial(0, InstanceMaterial);

	// --- declare the stream set: one indirect arg set per LOD, plus this leaf's seven aux streams.
	//
	// Declared at the instance capacity the mesh ALREADY holds, not at the new one. A re-declaration
	// reallocates and copies every resident stream, the base-mesh geometry included, and the only
	// thing another instance changes is how many instances the aux streams have room for — which is
	// a per-stream resize, applied right below. So the declaration only moves when the shape of the
	// set moves: a LOD gained or lost, a different cluster size, or a packed GPU source appearing
	// and taking the source-row stream down to a placeholder.
	FCSGpuInstancedGpuLayout DeclaredLayout = NewLayout;
	if (GpuLayout.IsValid()) DeclaredLayout.InstanceCapacity = GpuLayout.InstanceCapacity;

	FCSMeshStreamLayout StreamLayout;
	StreamLayout.NumIndirectDraws = DeclaredLayout.NumLODs;
	CSGpuInstancedBuildAuxStreamDescs(StreamLayout.ExtraStreams, DeclaredLayout, bPackedGpuSource);

	// Refused rather than partially applied, and the return value is the only signal: a slot that
	// collides with a stream the standard set already owns is dropped, and a dropped stream is a
	// null buffer bound at draw time with nothing in the log. See ECSGpuInstancedAuxSlot.
	if (!InstancedGpuMesh->SetStreamLayoutSync(StreamLayout))
	{
		UE_LOG(LogCSGpuInstancedMesh, Error,
			TEXT("%s: the GPU mesh refused this leaf's stream layout (%u LODs, %u instance slots). Nothing will be drawn."),
			*GetPathName(), DeclaredLayout.NumLODs, DeclaredLayout.InstanceCapacity);
		ReleaseGpuMesh();
		return;
	}

	// --- bring the instance-sized streams to the capacity this rebuild wants, one buffer at a time.
	//
	// Built from the same descriptor builder the declaration uses, so the two can never disagree
	// about what a stream at a given capacity looks like; entries already at their target size are
	// no-ops, which is what makes a rebuild that changed nothing free. Resized streams come back
	// zeroed — the rows and the cluster spheres are re-uploaded by the edit below, and every other
	// one of them is rewritten in full by the cull before anything reads it.
	TArray<FCSGpuStreamDesc> WantedAux;
	CSGpuInstancedBuildAuxStreamDescs(WantedAux, NewLayout, bPackedGpuSource);

	TArray<FCSMeshStreamResize> Resizes;
	Resizes.Reserve(WantedAux.Num());
	for (const FCSGpuStreamDesc& Desc : WantedAux)
	{
		FCSMeshStreamResize& Resize = Resizes.AddDefaulted_GetRef();
		Resize.Role = Desc.Role;
		Resize.SlotIndex = Desc.TexCoordIndex;
		Resize.ElementCount = Desc.ElementsPerUnit;
	}
	if (!InstancedGpuMesh->ResizeStreamsSync(Resizes))
	{
		UE_LOG(LogCSGpuInstancedMesh, Error,
			TEXT("%s: the GPU mesh refused to resize this leaf's aux streams to %u instance slots. Nothing will be drawn."),
			*GetPathName(), NewLayout.InstanceCapacity);
		ReleaseGpuMesh();
		return;
	}

	// --- size it for the base mesh. Both counts are exact here (unlike the road, whose emitted size
	// only the GPU knows), which is what lets the shrink below run without a counter readback.
	const int32 NumVertices = BaseMeshSnapshot.Positions.Num();
	const int32 NumIndices = BaseMeshSnapshot.Indices.Num();
	if (!InstancedGpuMesh->EnsureCapacitySync(NumVertices, NumIndices))
	{
		UE_LOG(LogCSGpuInstancedMesh, Warning,
			TEXT("%s: capacity for %d vertices / %d indices was refused; the instanced mesh will not be drawn."),
			*GetPathName(), NumVertices, NumIndices);
		ReleaseGpuMesh();
		return;
	}

	// --- upload
	const FCSGpuInstancedBaseMesh& Mesh = BaseMeshSnapshot;
	const TArray<FVector4f>& Rows = PackedInstances;
	const TArray<FVector4f>& Clusters = ClusterBounds;
	const FBox DrawnWorldBounds = LocalBounds.IsValid ? LocalBounds.TransformBy(GetComponentTransform()) : FBox(ForceInit);
	const bool bUploadRows = !bPackedGpuSource && Rows.Num() > 0;

	bool bUploaded = false;
	InstancedGpuMesh->EditMeshSync([&Mesh, &Rows, &Clusters, &DrawnWorldBounds, bUploadRows, &bUploaded](FCSMeshEditContext& Context)
	{
		FRDGBuilder& GraphBuilder = Context.GraphBuilder;

		FRDGBufferRef Positions = Context.Positions();
		FRDGBufferRef Tangents = Context.Tangents();
		FRDGBufferRef TexCoords = Context.TexCoords();
		FRDGBufferRef Colors = Context.Colors();
		FRDGBufferRef Indices = Context.Indices();
		FRDGBufferRef IndirectArgs = Context.IndirectArgs();
		FRDGBufferRef MeshCounters = Context.Counters();
		if (!Positions || !Tangents || !TexCoords || !Colors) return;
		if (!Indices || !IndirectArgs || !MeshCounters) return;

		// ERDGInitialDataFlags::None makes RDG take its own copy, so these arrays only have to
		// outlive the call — which EditMeshSync's flush guarantees anyway.
		GraphBuilder.QueueBufferUpload(Positions, Mesh.Positions.GetData(), Mesh.Positions.Num() * sizeof(FVector3f), ERDGInitialDataFlags::None);
		GraphBuilder.QueueBufferUpload(Tangents, Mesh.TangentBasis.GetData(), Mesh.TangentBasis.Num() * sizeof(uint32), ERDGInitialDataFlags::None);
		GraphBuilder.QueueBufferUpload(TexCoords, Mesh.TexCoords.GetData(), Mesh.TexCoords.Num() * sizeof(FVector2f), ERDGInitialDataFlags::None);
		GraphBuilder.QueueBufferUpload(Colors, Mesh.Colors.GetData(), Mesh.Colors.Num() * sizeof(uint32), ERDGInitialDataFlags::None);
		GraphBuilder.QueueBufferUpload(Indices, Mesh.Indices.GetData(), Mesh.Indices.Num() * sizeof(uint32), ERDGInitialDataFlags::None);

		// The readback path (ReadbackMeshSync / save-to-StaticMesh) sees the single base-mesh copy,
		// not the instanced result — the instances only ever exist as transforms.
		const uint32 Counters[2] = { uint32(Mesh.Positions.Num()), uint32(Mesh.Indices.Num()) };
		GraphBuilder.QueueBufferUpload(MeshCounters, Counters, sizeof(Counters), ERDGInitialDataFlags::None);

		// Zeroed until the first cull pass runs, so an unculled frame draws nothing rather than
		// whatever the buffer pool's previous tenant left in those five uints per LOD.
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgs, PF_R32_UINT)), 0u);

		// This leaf writes no material ids, but every UCSMesh carries that stream and the save path
		// reads it back as the per-triangle slot. Left holding the pool's previous tenant, a
		// one-material base mesh would come out of the saver scattered across dozens of slots that
		// have no materials behind them. Zero is this mesh's only slot.
		FRDGBufferRef MaterialIds = Context.MaterialIds();
		if (MaterialIds) AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(MaterialIds, PF_R32_UINT)), 0u);

		FRDGBufferRef SourceInstances = Context.Find(ECSGpuStreamRole::AuxVertex, uint8(ECSGpuInstancedAuxSlot::SourceInstances));
		FRDGBufferRef ClusterBoundsBuffer = Context.Find(ECSGpuStreamRole::AuxVertex, uint8(ECSGpuInstancedAuxSlot::ClusterBounds));
		if (!SourceInstances || !ClusterBoundsBuffer) return;

		if (bUploadRows) GraphBuilder.QueueBufferUpload(SourceInstances, Rows.GetData(), Rows.Num() * sizeof(FVector4f), ERDGInitialDataFlags::None);
		if (Clusters.Num() > 0) GraphBuilder.QueueBufferUpload(ClusterBoundsBuffer, Clusters.GetData(), Clusters.Num() * sizeof(FVector4f), ERDGInitialDataFlags::None);

		// No SetStandardStreamAccessFinal and no GraphBuilder.Execute() here: both belong to
		// EditMeshSync, which restores every resident stream's access state for the whole edit.
		// Doing either by hand is the failure that has no symptom but "it stopped drawing".

		// The arg sets are per LOD here, not per material run. A section table would make the render
		// side draw arg set i with material i, which for this mesh is LOD i's draw.
		UCSMeshOps::InvalidateSections(Context);
		// Stated exactly, which is what lets the shrink below skip the counter readback that a
		// GPU-decided size would force (a full stall) before it even reaches its own hysteresis.
		Context.SetKnownCounts(Mesh.Positions.Num(), Mesh.Indices.Num());
		// The streams hold one component-local copy of the base mesh; this box is where the drawn
		// instances are. They deliberately disagree — the instanced result has no vertices anywhere
		// — and this is the answer anything asking "where is this mesh" wants.
		Context.Resident.WorldBounds = DrawnWorldBounds;
		bUploaded = true;
	});

	if (!bUploaded)
	{
		UE_LOG(LogCSGpuInstancedMesh, Error, TEXT("%s: the base-mesh upload found the GPU mesh missing streams; nothing will be drawn."), *GetPathName());
		ReleaseGpuMesh();
		return;
	}

	// Retention introduces a ratchet the proxy-owned path never had: the buffers used to be
	// reallocated from scratch on every proxy rebuild, and now only EnsureCapacitySync moves them,
	// which never goes down. Without this, swapping a 100k-vertex base mesh for a 500-vertex one
	// keeps the larger allocation for the rest of the session.
	//
	// After the upload, not before it: the shrink refuses to go below the mesh's live counts, and
	// until the edit above ran those were the *previous* base mesh's — which is exactly the case
	// worth shrinking. The surviving contents are copied across, so the freshly uploaded mesh
	// arrives intact on the other side.
	if (NumVertices < InstancedGpuMesh->GetVertexCapacity() || NumIndices < InstancedGpuMesh->GetIndexCapacity())
	{
		InstancedGpuMesh->ShrinkCapacitySync(NumVertices, NumIndices);
	}

	GpuLayout = NewLayout;
}

// -----------------------------------------------------------------------------
// UObject / UPrimitiveComponent
// -----------------------------------------------------------------------------

void UCSGpuInstancedMeshComponent::OnRegister()
{
	Super::OnRegister();
	RebuildGpuMesh();
}

void UCSGpuInstancedMeshComponent::PostLoad()
{
	Super::PostLoad();

	if (BaseMesh) BaseMesh->ConditionalPostLoad();
	RebuildBaseMeshSnapshot();
}

#if WITH_EDITOR
void UCSGpuInstancedMeshComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	const FName PropertyName = PropertyChangedEvent.GetPropertyName();

	if (PropertyName == GET_MEMBER_NAME_CHECKED(UCSGpuInstancedMeshComponent, BaseMesh))
	{
		bBaseMeshIsExternal = false;
		RebuildBaseMeshSnapshot();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UCSGpuInstancedMeshComponent, InstancesPerCluster))
	{
		RebuildInstanceData();
		MarkRenderStateDirty();
	}

	Super::PostEditChangeProperty(PropertyChangedEvent);
}
#endif

FPrimitiveSceneProxy* UCSGpuInstancedMeshComponent::CreateSceneProxy()
{
	if (GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5) return nullptr;

	// The one gate now, and it stands for all the old ones: RebuildGpuMesh releases the buffers
	// unless there is a valid base mesh, a non-empty instance set and a layout the mesh accepted, so
	// an allocation existing is the same statement as "there is something to draw". Note this is
	// only a *read* — a proxy creation must never build geometry, since it can run off the game
	// thread during the end-of-frame update where a blocking flush is not allowed.
	const FCSMeshResidentRef Resident = InstancedGpuMesh ? InstancedGpuMesh->GetResident() : FCSMeshResidentRef();
	if (!Resident.IsValid() || !Resident->IsAllocated() || !GpuLayout.IsValid()) return nullptr;

	// The vertex factory only compiles for materials flagged for instancing; this both flags the
	// material in the editor and warns when it cannot be flagged (same call ISM makes).
	if (InstanceMaterial && !InstanceMaterial->CheckMaterialUsage_Concurrent(MATUSAGE_InstancedStaticMeshes))
	{
		UE_LOG(LogCSGpuInstancedMesh, Warning,
			TEXT("%s: material '%s' is not usable with instanced static meshes; instances will draw with the default material."),
			*GetPathName(), *GetNameSafe(InstanceMaterial));
		InstanceMaterial = nullptr;
	}

	// The per-frame cull passes are driven by a shared view extension; create it here rather than
	// from the proxy so registration stays on the game thread.
	FCSGpuInstancedMeshSceneProxy::EnsureCullServiceStarted();

	return new FCSGpuInstancedMeshSceneProxy(this, Resident);
}
