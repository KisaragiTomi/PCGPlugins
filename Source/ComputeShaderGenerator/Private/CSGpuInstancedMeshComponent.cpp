#include "CSGpuInstancedMeshComponent.h"
#include "CSGpuInstancedMeshSceneProxy.h"

#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Rendering/StaticMeshVertexBuffer.h"
#include "Rendering/PositionVertexBuffer.h"
#include "Rendering/ColorVertexBuffer.h"
#include "StaticMeshResources.h"
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

	RebuildInstanceData();
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

void UCSGpuInstancedMeshComponent::RebuildInstanceData()
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
// UObject / UPrimitiveComponent
// -----------------------------------------------------------------------------

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
	if (!BaseMeshSnapshot.IsValid()) return nullptr;

	const bool bHasInstances = GpuInstanceSource.IsValid() || GpuPointSource.IsValid() || PackedInstances.Num() > 0;
	if (!bHasInstances) return nullptr;

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

	return new FCSGpuInstancedMeshSceneProxy(this);
}
