#include "CSGpuInstancedMeshComponent.h"
#include "CSGpuInstancedMeshSceneProxy.h"
// 诊断入口 DebugGetDrawnAssetMismatchSync 要问"材质为本顶点工厂编出着色器了没有"，
// 所以这里必须自己带上工厂类型与材质着色器映射那几个头 —— unity 构建下它们恰好被邻居带进来，
// 漏写只有 -SingleFile 才照得出来（坑表里那条）。
#include "CSGpuInstancedMeshVertexFactory.h"

#include "CSMesh.h"
#include "CSMeshOps.h"

#include "Engine/StaticMesh.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialRenderProxy.h"
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

	/**
	 * 基础网格上传前把顶点色 alpha 清零 —— 类注释那份「实例路顶点色通道字典」的执行面。
	 *
	 * 这一步不是可选的美化：材质两条路读的是 `PerInstanceRandom + VertexColor.A`，而这条等式
	 * 只有在「GPU 路的 A 恒为 0」时才成立。资产里 alpha 通常是 255，漏掉这一步的症状是
	 * 实例版比烘焙版整体亮一档（tint 多加了 1.0），**不报错**。
	 *
	 * 打包顺序照 `ToPackedARGB` 的字面意思：最高 8 位是 A（小端内存里就是 FColor 的 .A）。
	 */
	uint32 ClearPackedColorAlpha(uint32 PackedARGB)
	{
		return PackedARGB & 0x00FFFFFFu;
	}
}

UCSGpuInstancedMeshComponent::UCSGpuInstancedMeshComponent()
{
	// 2026-08-30 实测（出图对照，不是推断）：
	//   · r.Shadow.Virtual.Enable 0（CSM）—— **这条路会投影**，门框砖的影子清清楚楚。
	//   · r.Shadow.Virtual.Enable 1（VSM，本工程 Config/DefaultEngine.ini 的项目级设置）
	//     —— 一点影子都没有，且与 r.Shadow.Virtual.Cache / NonNanite.Batch / NonNanite.UseHZB
	//     三个开关**逐字节无关**（三张对照图 md5 完全相同，都不是它们的锅）。
	// 所以旧注释里那句"GPU-Scene 覆盖 indirect args"在这条路上根本不成立
	// （MeshPassProcessor.cpp:1304 的 bDoOverrideArgs 要求 PrimitiveIdStreamIndex >= 0，
	//  而本工厂注册时就没有 SupportsPrimitiveIdStream），拦住它的是另一件事：
	// VSM 的非 Nanite 光栅整条走 GPU-Scene 实例剔除
	// （VirtualShadowMapArray.cpp 的 AddRasterPass → FParallelMeshDrawCommandPass::Draw
	//  带 InstanceCullingDrawParams；InstanceCullingContext.cpp:1509/1590 只为带
	//  HasPrimitiveIdStreamIndex 的命令追加实例），而我们是 dynamic relevance +
	// FDynamicPrimitiveUniformBuffer，压根没有 GPU-Scene 实例可剔。【机制系源码阅读，标推测；
	// "VSM 不画 / CSM 画"是实测】
	//
	// 留 true 而不是退回 false：它在 CSM 下确凿正确，在 VSM 下**一个影子像素都画不出来**
	// （改前/改后同机位逐像素比过，差异只有纹理采样噪声，没有任何影子形状），
	// 而退回 false 只会把上面这条错误归因再钉一遍。
	// ⚠️ 另一条已知代价（CSM 下才看得见）：可见实例集是 RunCulling 按**主视锥**压出来的
	//    （一族只跑一次），主视锥外的实例不进阴影图。要视锥外也投影得另备一份不做视锥剔除的 args。
	CastShadow = true;
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
		// alpha 归本组件所有（逐实例随机的载体，见类注释那份通道字典），一律清零。
		BaseMeshSnapshot.Colors[V] = ClearPackedColorAlpha(Color.ToFColor(false).ToPackedARGB());
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
			// alpha 归本组件所有（逐实例随机的载体，见类注释那份通道字典），一律清零；
			// 没有色流的资产退白也一样要清，否则烘焙路与实例路差一个常数 1.0。
			BaseMeshSnapshot.Colors.Add(ClearPackedColorAlpha(
				bHasColors ? LOD.VertexBuffers.ColorVertexBuffer.VertexColor(V).ToPackedARGB() : 0xFFFFFFFFu));
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
// 诊断 / 验收（**阻塞**）—— 声明处那段注释是这两条为什么存在
// -----------------------------------------------------------------------------

int32 UCSGpuInstancedMeshComponent::DebugReadDrawnInstanceCountSync() const
{
	// 读的必须是**组件手上这一份**实例源，不是生产者自己那批 buffer：门框砖那个既有 bug 的
	// 现场就是两者不一致 —— 生产者以为自己清干净了，而组件被重新交接回同一批带着陈旧计数器
	// 的 buffer，剔除 pass 照着它又画了一遍上一代的实例。
	// 按值持一份引用而不是绑常量引用：回读期间生产者可能换掉成员，而三元表达式的结果本来
	// 就是个 prvalue —— 写成引用只是把"这里有一次拷贝"藏起来，读的人容易误以为它跟着成员走。
	TRefCountPtr<FRDGPooledBuffer> Counter;
	if (GpuInstanceSource.IsValid()) Counter = GpuInstanceSource.Counter;
	else if (GpuPointSource.IsValid()) Counter = GpuPointSource.Counter;
	if (!Counter.IsValid())
	{
		// 没有 GPU 源时剔除 pass 用的是常量 `NumSourceInstances`（CSGpuInstancedMeshSceneProxy
		// 那条 `AddClearUAVPass(..., Layout.NumSourceInstances)`），如实返回它。没有分配 =
		// 组件根本不会建代理，一个实例都画不出来，那才是真的 0。
		const FCSMeshResidentRef Resident = InstancedGpuMesh ? InstancedGpuMesh->GetResident() : FCSMeshResidentRef();
		if (!Resident.IsValid() || !Resident->IsAllocated() || !GpuLayout.IsValid()) return 0;
		return int32(GpuLayout.NumSourceInstances);
	}

	TArray<uint32> Values;
	// AuxVertex 的常态访问是 SRVMask —— 实例计数器平时就停在那里（生产者的
	// `SetBufferAccessFinal(CounterRef, ERHIAccess::SRVMask)`），读完必须放回去。
	if (!CSMeshReadback::ReadUintBufferSync(Counter, 1u, ECSGpuStreamRole::AuxVertex, Values) || Values.IsEmpty())
	{
		// **读不到不能当 0**：那会让"擦掉之后 GPU 计数器必须归零"这条断言在回读管线自己坏掉时
		// 假绿 —— 正是这一族 bug 最典型的失效方式。
		return -1;
	}
	const uint32 Capacity = GpuInstanceSource.IsValid() ? GpuInstanceSource.Capacity : GpuPointSource.Capacity;
	// GPU 的 counter 会数到越界丢弃的那些（InterlockedAdd 先加后判），按容量钳 —— 同石阶回读。
	return int32(FMath::Min(Values[0], FMath::Max(Capacity, 1u)));
}

FString UCSGpuInstancedMeshComponent::DebugGetDrawnAssetMismatchSync() const
{
	const FCSMeshResidentRef Resident = InstancedGpuMesh ? InstancedGpuMesh->GetResident() : FCSMeshResidentRef();
	if (!Resident.IsValid() || !Resident->IsAllocated())
	{
		return TEXT("GPU 网格没分配（组件不会建场景代理，一个实例都画不出来）");
	}

	// --- 基础网格：拿 GPU 上真的那份跟我们以为的那张对 ---
	//
	// CPU 侧的 `BaseMeshSnapshot` 只是"我们打算上传什么"。中间隔着 EnsureCapacitySync /
	// SetStreamLayoutSync / ShrinkCapacitySync 三道会失败且**只写日志不报错**的关口，
	// 任何一道漏掉，画面上就是上一张网格（或什么都没有），而所有计数断言照绿。
	uint32 GpuVerts = 0;
	uint32 GpuIndices = 0;
	if (!CSMeshReadback::ReadCountersSync(*Resident, GpuVerts, GpuIndices))
	{
		return TEXT("读不到 GPU 上的网格计数器（回读没完成）");
	}
	if (GpuVerts != uint32(BaseMeshSnapshot.Positions.Num()) || GpuIndices != uint32(BaseMeshSnapshot.Indices.Num()))
	{
		return FString::Printf(
			TEXT("GPU 上的基础网格不是快照那一张（GPU %u 顶点 / %u 索引，快照 %d / %d）"),
			GpuVerts, GpuIndices, BaseMeshSnapshot.Positions.Num(), BaseMeshSnapshot.Indices.Num());
	}
	if (GpuVerts < 3u || GpuIndices < 3u)
	{
		return TEXT("GPU 上的基础网格是空的（不足一个三角）");
	}

	// 快照又是从哪张资产建的：换掉 `BaseMesh` 时组件不一定重建，那时候画面上还是旧网格 ——
	// 与坑表里"CDO 默认值不传播到已存在实例"同一族的静默失效。外部喂进来的快照没有资产可对。
	if (!bBaseMeshIsExternal)
	{
		if (!BaseMesh) return TEXT("没有基础网格资产（BaseMesh 是 NULL）");
		const FStaticMeshRenderData* RenderData = BaseMesh->GetRenderData();
		if (!RenderData || RenderData->LODResources.Num() == 0)
		{
			return FString::Printf(TEXT("基础网格 '%s' 没有渲染数据（CPU 访问没开？）"), *GetNameSafe(BaseMesh));
		}
		const uint32 AssetLod0Verts = RenderData->LODResources[0].VertexBuffers.PositionVertexBuffer.GetNumVertices();
		const uint32 SnapshotLod0Verts = BaseMeshSnapshot.LODs.Num() > 0
			? (BaseMeshSnapshot.LODs.Num() > 1 ? BaseMeshSnapshot.LODs[1].BaseVertex : uint32(BaseMeshSnapshot.Positions.Num()))
			: 0u;
		if (AssetLod0Verts != SnapshotLod0Verts)
		{
			return FString::Printf(
				TEXT("GPU 上的基础网格与资产 '%s' 对不上（LOD0：GPU %u 顶点，资产 %u）——多半是换了资产但组件没重建"),
				*GetNameSafe(BaseMesh), SnapshotLod0Verts, AssetLod0Verts);
		}
	}

	// --- 材质：引擎会不会把它静默换掉 ---
	if (!InstanceMaterial)
	{
		// ⚠️ 这一条**同时**覆盖"本来就没绑"和"绑了但被换掉了"：`CreateSceneProxy` 在
		// `CheckMaterialUsage_Concurrent` 不过时会把 `InstanceMaterial` 就地置空，
		// 所以建过一次代理之后，被换掉的材质在这里表现为"没绑"。
		return TEXT("没有绑材质（会用引擎默认表面材质画成一片灰）");
	}
	const UMaterial* Base = InstanceMaterial->GetMaterial();
	if (!Base || !Base->bUsedWithInstancedStaticMeshes)
	{
		return FString::Printf(
			TEXT("材质 '%s' 的母材质没有勾 bUsedWithInstancedStaticMeshes（引擎会静默换成默认材质）"),
			*GetNameSafe(InstanceMaterial));
	}

	// 比"勾没勾"再进一步：**编译产物**里到底有没有本顶点工厂那一份。勾是输入，着色器映射
	// 是输出 —— 母材质勾了但那一份没编出来（平台裁剪、编译失败）时，渲染器一样退默认材质。
	//
	// ⚠️ 三态，不是两态：无头进程里材质的渲染线程着色器映射可能压根还没建起来，那时候
	// **判不了**。判不了就说判不了，不许当成通过 —— 本项目已经吃过"把判不了当绿"的亏。
	bool bShaderMapKnown = false;
	bool bHasFactoryShaders = false;
	const UMaterialInterface* MaterialForProbe = InstanceMaterial;
	const ERHIFeatureLevel::Type FeatureLevel = GMaxRHIFeatureLevel;
	ENQUEUE_RENDER_COMMAND(CSGpuInstancedProbeMaterialShaderMap)(
		[MaterialForProbe, FeatureLevel, &bShaderMapKnown, &bHasFactoryShaders](FRHICommandListImmediate&)
		{
			const FMaterialRenderProxy* Proxy = MaterialForProbe->GetRenderProxy();
			if (!Proxy) return;
			const FMaterialRenderProxy* Fallback = nullptr;
			const FMaterial& Material = Proxy->GetMaterialWithFallback(FeatureLevel, Fallback);
			const FMaterialShaderMap* ShaderMap = Material.GetRenderingThreadShaderMap();
			if (!ShaderMap) return;
			bShaderMapKnown = true;
			bHasFactoryShaders = ShaderMap->GetMeshShaderMap(&FCSGpuInstancedMeshVertexFactory::StaticType) != nullptr;
		});
	UCSMesh::CountedBlockingFlush();
	if (bShaderMapKnown && !bHasFactoryShaders)
	{
		return FString::Printf(
			TEXT("材质 '%s' 没有为本实例顶点工厂编出着色器（渲染器会退回默认材质）"),
			*GetNameSafe(InstanceMaterial));
	}
	if (!bShaderMapKnown)
	{
		// **判不了要说出来**：空串在调用方那里读作"通过"，而这一路其实什么都没证明。
		// 不打这行日志的话，这条判据会在无头环境里静默退化成"永远绿"，
		// 而那正是本项目已经吃过三次亏的失效方式。
		UE_LOG(LogCSGpuInstancedMesh, Log,
			TEXT("%s: 材质 '%s' 的渲染线程着色器映射还没建起来，「引擎会不会静默换材质」这一条判不了（不算通过）。"),
			*GetPathName(), *GetNameSafe(InstanceMaterial));
	}

	return FString();
}

// -----------------------------------------------------------------------------
// 烘焙出口（裁决六 ①）—— 声明处那段注释是它为什么存在与覆盖到哪
// -----------------------------------------------------------------------------

namespace
{
	// unity 构建共享 TU，file-local 一律加模块前缀（上面那组老 helper 是同一条纪律的历史遗留，
	// 名字太泛，新加的一律用 CSGpuInstanced_ 开头）。

	/** PackSnorm8888 的逆。烘焙要把常驻的切线基还原成 float，不能靠 GPU 那份。 */
	FVector4f CSGpuInstanced_UnpackSnorm8888(uint32 Packed)
	{
		auto Component = [](uint32 Byte) { return float(int8(Byte & 0xFFu)) / 127.0f; };
		return FVector4f(
			Component(Packed), Component(Packed >> 8), Component(Packed >> 16), Component(Packed >> 24));
	}

	/** ToPackedARGB 的逆，只取线性域下的 0..1 分量（顶点色在常驻流里就是 8 位量化的）。 */
	FLinearColor CSGpuInstanced_UnpackColorARGB(uint32 Packed)
	{
		const FColor Color(uint8((Packed >> 16) & 0xFFu), uint8((Packed >> 8) & 0xFFu),
			uint8(Packed & 0xFFu), uint8((Packed >> 24) & 0xFFu));
		// bSRGB=false：上传时走的是 ToFColor(false) / 资产色流原样搬，两头必须同一口径，
		// 否则烘出来的顶点色会被 sRGB 曲线拧过一道，画面上是"摆件整体变亮"而不报错。
		return FLinearColor(Color.R / 255.0f, Color.G / 255.0f, Color.B / 255.0f, Color.A / 255.0f);
	}
}

bool UCSGpuInstancedMeshComponent::ReadLiveInstanceRowsSync(TArray<FVector4f>& OutRows) const
{
	OutRows.Reset();

	// --- CPU 实例数组：行早就打包好躺在内存里，一次回读都不用付 ---
	if (!GpuInstanceSource.IsValid() && !GpuPointSource.IsValid())
	{
		OutRows = PackedInstances;
		return true;
	}

	// --- 点云源：这条烘不出来，理由逐字见 SaveToStaticMesh 的声明注释 ---
	if (GpuPointSource.IsValid())
	{
		UE_LOG(LogCSGpuInstancedMesh, Warning,
			TEXT("%s: 点云实例源没有 CPU 侧的 packed 行（代理每帧在渲染图里现打），烘不出来。"),
			*GetPathName());
		return false;
	}

	// --- GPU 实例源：先读计数器再按计数取行 ---
	//
	// 先读计数器而不是一口气把整段容量拉回来，是因为容量与活着的实例数差着数量级（门框砖
	// 512 : 152，藤更悬殊），而这段是**逐 float 走 PCIe** 的。多付一次阻塞换少读几十倍的字节，
	// 在离线烘焙上是划算的；两次阻塞都会被计数器数到，也正是想要的。
	const int32 LiveCount = DebugReadDrawnInstanceCountSync();
	if (LiveCount < 0) return false;                       // 读不到 ≠ 0 个实例
	if (LiveCount == 0) return true;                       // 真的没有实例，空集是正确答案

	const uint32 Rows = uint32(LiveCount) * 5u;
	TArray<uint32> Raw;
	// AuxVertex：packed 行平时停在 SRVMask（生产者的 SetBufferAccessFinal(PackedRef, SRVMask)），
	// 读完必须放回去，否则下一帧的剔除 pass 在错误的访问状态上撞见它。
	if (!CSMeshReadback::ReadUintBufferSync(GpuInstanceSource.PackedInstances, Rows * 4u,
		ECSGpuStreamRole::AuxVertex, Raw) || Raw.Num() < int32(Rows * 4u))
	{
		return false;
	}

	OutRows.SetNumUninitialized(int32(Rows));
	// buffer 是 Buffer<float4>，回读通路只认 uint32 —— 逐位重解释，不做任何数值转换。
	FMemory::Memcpy(OutRows.GetData(), Raw.GetData(), Rows * 4u * sizeof(uint32));
	return true;
}

bool UCSGpuInstancedMeshComponent::DebugReadInstanceRandomsSync(TArray<float>& OutRandoms) const
{
	OutRandoms.Reset();

	TArray<FVector4f> Row;
	if (!ReadLiveInstanceRowsSync(Row)) return false;

	OutRandoms.Reserve(Row.Num() / 5);
	// 第 4 行的 .w 就是 PerInstanceRandom（LocalVertexFactory.ush 的 OriginBuffer 契约）。
	for (int32 Base = 0; Base + 4 < Row.Num(); Base += 5) OutRandoms.Add(Row[Base + 3].W);
	return true;
}

#if WITH_EDITOR

UStaticMesh* UCSGpuInstancedMeshComponent::SaveToStaticMesh(const FTransform& BakeSpace,
	const FString& AssetPathAndName, bool bReplaceExistingAsset, bool bSaveAsset, bool bEnableNanite)
{
	if (!BaseMeshSnapshot.IsValid() || BaseMeshSnapshot.LODs.Num() == 0)
	{
		UE_LOG(LogCSGpuInstancedMesh, Warning, TEXT("%s: 没有可用的基础网格快照，烘不出东西。"), *GetPathName());
		return nullptr;
	}

	TArray<FVector4f> Row;
	if (!ReadLiveInstanceRowsSync(Row)) return nullptr;

	const int32 NumInstances = Row.Num() / 5;
	if (NumInstances <= 0)
	{
		UE_LOG(LogCSGpuInstancedMesh, Warning, TEXT("%s: 这一族一个实例都没有，不产资产。"), *GetPathName());
		return nullptr;
	}

	// **只烘 LOD0**。LOD 是"离远了画什么"，而资产该带的是最全的那一份；把 GPU 当前选中的 LOD
	// 烘下来会让烘焙结果取决于烘那一刻相机在哪 —— 那是最难复现的一类差异。
	const FCSGpuInstancedLODRange& LOD0 = BaseMeshSnapshot.LODs[0];
	const int32 LodVertexCount = (BaseMeshSnapshot.LODs.Num() > 1)
		? int32(BaseMeshSnapshot.LODs[1].BaseVertex - LOD0.BaseVertex)
		: (BaseMeshSnapshot.Positions.Num() - int32(LOD0.BaseVertex));
	if (LodVertexCount < 3 || LOD0.NumIndices < 3)
	{
		UE_LOG(LogCSGpuInstancedMesh, Warning, TEXT("%s: LOD0 不足一个三角，烘不出东西。"), *GetPathName());
		return nullptr;
	}

	// 不叫 ComponentToWorld：USceneComponent 有一个同名成员，遮蔽会被 /WX 当错误。
	const FTransform BakeComponentToWorld = GetComponentTransform();
	const int32 TotalVerts = NumInstances * LodVertexCount;
	const int32 TotalIndices = NumInstances * int32(LOD0.NumIndices);

	// 裁决六 ②「多组 UV 必须随网格保住」：**常驻流只有 UV0**（FCSGpuInstancedBaseMesh::TexCoords
	// 是每顶点一条，实例路从来没上传过第二组），所以额外的 UV 直接从基础网格资产的 LOD0 取。
	// 快照的 LOD0 顶点与资产 LOD0 顶点是 1:1 同序拷贝（见 RebuildBaseMeshSnapshot），下标可以直用。
	// 外部喂进来的快照（SetBaseMeshFromGpuData）没有资产可取，只能停在一组 —— 如实退化，不假装。
	const FStaticMeshLODResources* AssetLod0 = nullptr;
	if (!bBaseMeshIsExternal && BaseMesh)
	{
		const FStaticMeshRenderData* RenderData = BaseMesh->GetRenderData();
		if (RenderData && RenderData->LODResources.Num() > 0
			&& int32(RenderData->LODResources[0].VertexBuffers.PositionVertexBuffer.GetNumVertices()) == LodVertexCount)
		{
			AssetLod0 = &RenderData->LODResources[0];
		}
	}
	const int32 NumUVChannels = AssetLod0
		? FMath::Clamp(int32(AssetLod0->VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords()),
			1, FCSGpuMeshCPUData::MaxTexCoordChannels)
		: 1;

	FCSGpuMeshCPUData MeshData;
	// 世界空间 + 逐顶点属性：落盘层按 SourceSpace 决定要不要烘回 BakeSpace 的局部空间，
	// 与岩壳/道路那两条出口同一条判据。
	MeshData.SourceSpace = FCSGpuMeshCPUData::ESpace::World;
	MeshData.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerVertex;
	MeshData.Positions.SetNumUninitialized(TotalVerts);
	MeshData.Normals.SetNumUninitialized(TotalVerts);
	MeshData.Tangents.SetNumUninitialized(TotalVerts);
	MeshData.BinormalSigns.SetNumUninitialized(TotalVerts);
	MeshData.NumTexCoordChannels = NumUVChannels;
	for (int32 Channel = 0; Channel < NumUVChannels; ++Channel) MeshData.TexCoordChannels[Channel].SetNumUninitialized(TotalVerts);
	MeshData.Colors.SetNumUninitialized(TotalVerts);
	MeshData.Indices.SetNumUninitialized(TotalIndices);

	for (int32 Instance = 0; Instance < NumInstances; ++Instance)
	{
		const int32 Base = Instance * 5;
		const FVector4f R0 = Row[Base + 0];
		const FVector4f R1 = Row[Base + 1];
		const FVector4f R2 = Row[Base + 2];
		const FVector4f Origin = Row[Base + 3];

		// 行向量约定，与 LocalVertexFactory.ush 的 GetInstanceTransform() 逐字一致：
		// P' = P.x*R0 + P.y*R1 + P.z*R2 + Origin。写成列向量会把每个非对称的旋转烘成它的转置 ——
		// 画面上是"砖全都歪了但还都在拱上"，看着像排布算错，其实是这里错了。
		const FMatrix44f InstanceToComponent(
			FPlane4f(R0.X, R0.Y, R0.Z, 0.0f),
			FPlane4f(R1.X, R1.Y, R1.Z, 0.0f),
			FPlane4f(R2.X, R2.Y, R2.Z, 0.0f),
			FPlane4f(Origin.X, Origin.Y, Origin.Z, 1.0f));
		// 法线/切线走 3x3 的**逆转置**：逐实例非均匀缩放（石阶被压成 60×100×45）下直接乘
		// 会把法线拧歪，而歪掉的法线只表现为"这一批的明暗不太对"，没有任何断言看得见。
		const FMatrix44f NormalMatrix = InstanceToComponent.Inverse().GetTransposed();
		// **逐实例随机进顶点色 alpha** —— 类注释那份通道字典的另一半（GPU 路那一半在
		// ClearPackedColorAlpha）。烘完之后 PerInstanceRandom 恒为 0，不写这一条的症状是
		// 整片同色，而且一条断言都不会红。
		const float InstanceRandom01 = FMath::Clamp(Origin.W, 0.0f, 1.0f);

		const int32 DstVertexBase = Instance * LodVertexCount;
		for (int32 V = 0; V < LodVertexCount; ++V)
		{
			const int32 Src = int32(LOD0.BaseVertex) + V;
			const int32 Dst = DstVertexBase + V;

			const FVector4f Local4 = InstanceToComponent.TransformPosition(BaseMeshSnapshot.Positions[Src]);
			MeshData.Positions[Dst] = FVector3f(
				BakeComponentToWorld.TransformPosition(FVector(Local4.X, Local4.Y, Local4.Z)));

			const FVector4f TangentX = CSGpuInstanced_UnpackSnorm8888(BaseMeshSnapshot.TangentBasis[Src * 2 + 0]);
			const FVector4f TangentZ = CSGpuInstanced_UnpackSnorm8888(BaseMeshSnapshot.TangentBasis[Src * 2 + 1]);
			const FVector4f Normal4 = NormalMatrix.TransformVector(FVector3f(TangentZ.X, TangentZ.Y, TangentZ.Z));
			const FVector4f Tangent4 = InstanceToComponent.TransformVector(FVector3f(TangentX.X, TangentX.Y, TangentX.Z));
			// 组件变换那一段只取旋转：组件本身带非均匀缩放的话法线同样要走逆转置，但实例组件
			// 一律以父 actor 的变换挂着（缩放为 1），为一个不存在的情形多算一次逆矩阵不划算。
			MeshData.Normals[Dst] = FVector3f(BakeComponentToWorld
				.TransformVectorNoScale(FVector(Normal4.X, Normal4.Y, Normal4.Z)).GetSafeNormal());
			MeshData.Tangents[Dst] = FVector3f(BakeComponentToWorld
				.TransformVectorNoScale(FVector(Tangent4.X, Tangent4.Y, Tangent4.Z)).GetSafeNormal());
			MeshData.BinormalSigns[Dst] = TangentZ.W >= 0.0f ? 1.0f : -1.0f;

			MeshData.TexCoordChannels[0][Dst] = BaseMeshSnapshot.TexCoords[Src];
			for (int32 Channel = 1; Channel < NumUVChannels; ++Channel)
			{
				MeshData.TexCoordChannels[Channel][Dst] =
					AssetLod0->VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(uint32(V), uint32(Channel));
			}

			const FLinearColor Color = CSGpuInstanced_UnpackColorARGB(BaseMeshSnapshot.Colors[Src]);
			MeshData.Colors[Dst] = FVector4f(Color.R, Color.G, Color.B, InstanceRandom01);
		}

		const int32 DstIndexBase = Instance * int32(LOD0.NumIndices);
		for (uint32 I = 0; I < LOD0.NumIndices; ++I)
		{
			// 索引在快照里是**相对本 LOD 顶点块**的（DrawIndexedIndirect 加 BaseVertexLocation），
			// 展开时既要去掉那个基址、又要加上本实例在输出数组里的基址。
			const uint32 Local = BaseMeshSnapshot.Indices[int32(LOD0.FirstIndex + I)];
			MeshData.Indices[DstIndexBase + int32(I)] = uint32(DstVertexBase) + Local;
		}
	}

	// 材质表只有一格：实例路整族共用一份 InstanceMaterial（组件级契约，不是简化）。
	TArray<UMaterialInterface*> Materials;
	Materials.Add(InstanceMaterial ? InstanceMaterial.Get() : nullptr);

	FCSGpuMeshConvertOptions ConvertOptions;
	ConvertOptions.TargetTransform = BakeSpace;
	ConvertOptions.bBakeToLocalSpace = true;

	FCSGpuMeshAssetOptions AssetOptions;
	AssetOptions.AssetPath = AssetPathAndName.TrimStartAndEnd();
	AssetOptions.bTransient = false;   // 这条出口存在的全部意义就是产资产
	AssetOptions.bReplaceExisting = bReplaceExistingAsset;
	AssetOptions.bSaveToDisk = bSaveAsset;
	AssetOptions.bEnableNanite = bEnableNanite;

	UE_LOG(LogCSGpuInstancedMesh, Log, TEXT("%s: 烘焙 %d 个实例 × %d 顶点 = %d 顶点 / %d 三角。"),
		*GetPathName(), NumInstances, LodVertexCount, TotalVerts, TotalIndices / 3);

	return UCSGpuMeshComponent::BuildStaticMesh(this, GetOwner(), MeshData, Materials, ConvertOptions, AssetOptions);
}

#endif // WITH_EDITOR

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
	// render round trip each would show up as load time for no visible result.
	//
	// ⚠️ 这里以前是 ReleaseGpuMesh()，理由写的是"缓冲已经与实例集不符"。但 Release 本身就是
	// **一次阻塞刷新**（正是这段注释想省掉的那种），而且把常驻缓冲整套还回去，重注册时只能
	// AllocateSync + EditMeshSync 从头再来。编辑器改一个属性 ⇒ RerunConstructionScripts ⇒
	// 卸载全部组件 → 跑构造脚本 → 重注册，于是**拖尺寸的每一帧**都白付 3 次阻塞刷新
	// （实测 12 帧 36 次，与容量/包围盒/实例数全都无关）。
	// 正确做法是只记脏：不许画的东西不需要马上正确，OnRegister 会在建代理**之前**补上。
	if (!IsRegistered())
	{
		bGpuMeshDirty = true;
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
	bGpuMeshDirty = false;
}

// -----------------------------------------------------------------------------
// UObject / UPrimitiveComponent
// -----------------------------------------------------------------------------

void UCSGpuInstancedMeshComponent::OnRegister()
{
	Super::OnRegister();

	// 只在**真有东西被跳过**时重建。GPU 网格是常驻的（InstancedGpuMesh 是组件自己的 UObject，
	// 卸载不动它），代理只是重新从常驻集取视图 —— 布局没变、缓冲还在的话一个字节都不用重传。
	// 无条件重建的代价是 RebuildGpuMesh 尾部那次**无条件的** EditMeshSync：一次阻塞刷新，
	// 每次重注册都付，而编辑器改任一属性都会走一遍卸载/重注册。
	if (bGpuMeshDirty || !GpuLayout.IsValid()) RebuildGpuMesh();
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
