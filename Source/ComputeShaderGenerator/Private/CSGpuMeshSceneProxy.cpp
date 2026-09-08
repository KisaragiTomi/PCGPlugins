#include "CSGpuMeshSceneProxy.h"
#include "CSMesh.h"

#include "Components/PrimitiveComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "MaterialDomain.h"
#include "MaterialShared.h"
#include "MeshBatch.h"
#include "SceneManagement.h"
#include "SceneView.h"   // GetDynamicMeshElementsShadowCullFrustum: the shadow-depth-view test
#include "SceneInterface.h"
#include "RHICommandList.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderUtils.h"      // VelocityIncludeStationaryPrimitives
#include "RHIGPUReadback.h"

FCSGpuMeshSceneProxy::FCSGpuMeshSceneProxy(const UPrimitiveComponent* Component, UMaterialInterface* InMaterial, const char* DebugName)
	: FPrimitiveSceneProxy(Component)
	, VertexFactoryDebugName(DebugName)
	, Material(InMaterial)
{
	if (!Material) Material = UMaterial::GetDefaultMaterial(MD_Surface);
	MaterialRelevance = Material->GetRelevance_Concurrent(GetScene().GetShaderPlatform());

	bVerifyUsedMaterials = false;
	bSupportsDistanceFieldRepresentation = false;
}

FCSGpuMeshSceneProxy::~FCSGpuMeshSceneProxy()
{
}

uint32 FCSGpuMeshSceneProxy::GetMemoryFootprint() const
{
	return sizeof(*this) + GetAllocatedSize() + Streams.GetAllocatedSize();
}

bool FCSGpuMeshSceneProxy::CanBeOccluded() const
{
	return !MaterialRelevance.bDisableDepthTest;
}

void FCSGpuMeshSceneProxy::CreateRenderThreadResources(FRHICommandListBase& RHICmdList)
{
	FPrimitiveSceneProxy::CreateRenderThreadResources(RHICmdList);
	InitGpuGeometry(RHICmdList);
}

void FCSGpuMeshSceneProxy::DestroyRenderThreadResources()
{
	// Release the vertex factory before the buffers it streams from (matches the
	// original per-proxy teardown order).
	if (VertexFactory)
	{
		VertexFactory->ReleaseResource();
		VertexFactory.Reset();
	}
	ReleaseGpuGeometry();
	FPrimitiveSceneProxy::DestroyRenderThreadResources();
}

void FCSGpuMeshSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const
{
	if (!DrawDesc.bValid || DrawDesc.IndexBuffer == nullptr || !VertexFactory) return;

	FCSGpuDrawArgs ShadowArgs;
	const bool bHaveShadowArgs = GetShadowDrawArgs(0, ShadowArgs);

	FMaterialRenderProxy* MaterialProxy = Material->GetRenderProxy();
	SubmitGpuBufferDraw(*this, Views, VisibilityMap, Collector, *VertexFactory, *MaterialProxy,
		*DrawDesc.IndexBuffer, PT_TriangleList, DrawDesc.NumPrimitives, DrawDesc.MaxVertexIndex,
		bBatchCastShadow, DrawDesc.IndirectArgsBuffer, DrawDesc.IndirectArgsOffset,
		bHaveShadowArgs ? &ShadowArgs : nullptr);
}

bool FCSGpuMeshSceneProxy::GetShadowDrawArgs(int32 ArgSetIndex, FCSGpuDrawArgs& OutArgs) const
{
	return ExternalResident.IsValid() && ExternalResident->GetDrawArgs(ArgSetIndex, OutArgs);
}

void FCSGpuMeshSceneProxy::SubmitGpuBufferDraw(
	const FPrimitiveSceneProxy& SceneProxy,
	const TArray<const FSceneView*>& Views,
	uint32 VisibilityMap,
	FMeshElementCollector& Collector,
	const FVertexFactory& InVertexFactory,
	FMaterialRenderProxy& MaterialProxy,
	const FIndexBuffer& IndexBuffer,
	EPrimitiveType PrimitiveType,
	uint32 NumPrimitives,
	uint32 MaxVertexIndex,
	bool bCastShadow,
	FRHIBuffer* IndirectArgsBuffer,
	uint32 IndirectArgsOffset,
	const FCSGpuDrawArgs* ShadowArgs)
{

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		if ((VisibilityMap & (1 << ViewIndex)) == 0) continue;

		// A shadow-depth gather is the one caller that cannot use our indirect args, and this is
		// how it identifies itself: FProjectedShadowInfo::GatherDynamicMeshElements sets the cull
		// frustum on its view before every gather (ShadowSetup.cpp:2763/2770/2778) and nothing
		// else in the renderer ever sets it — FSceneView leaves it null (SceneView.cpp:861).
		// Preferred over asking "is this a VSM": the direct batch is equally correct for a plain
		// shadow map, and a proxy cannot see which kind of shadow map it is being gathered for.
		const bool bShadowDepthView = Views[ViewIndex]->GetDynamicMeshElementsShadowCullFrustum() != nullptr;
		// No CPU copy yet ⇒ fall back to the indirect form, which is the pre-existing behaviour:
		// right in a cascaded shadow map, empty in a virtual one. Better than drawing a guessed
		// count, because index slots past the real end hold whatever the last, larger mesh left
		// there — garbage triangles in the shadow map instead of a missing shadow.
		const bool bDirectShadowDraw = bShadowDepthView && ShadowArgs != nullptr && ShadowArgs->IsDrawable();

		FMeshBatch& Mesh = Collector.AllocateMesh();
		Mesh.VertexFactory = &InVertexFactory;
		Mesh.MaterialRenderProxy = &MaterialProxy;
		Mesh.ReverseCulling = SceneProxy.IsLocalToWorldDeterminantNegative();
		Mesh.Type = PrimitiveType;
		Mesh.DepthPriorityGroup = SDPG_World;
		Mesh.bCanApplyViewModeOverrides = false;
		Mesh.CastShadow = bCastShadow;

		FMeshBatchElement& BatchElement = Mesh.Elements[0];
		BatchElement.IndexBuffer = &IndexBuffer;
		BatchElement.FirstIndex = 0;
		BatchElement.MinVertexIndex = 0;
		BatchElement.MaxVertexIndex = MaxVertexIndex;
		// The two forms are mutually exclusive, and the engine enforces it in both directions
		// (MeshPassProcessor.cpp:930/934): an args buffer demands NumPrimitives == 0, and a
		// non-zero NumPrimitives demands no args buffer.
		if (bDirectShadowDraw)
		{
			// FirstIndex has to come across too, and not only for the direct draw: instance
			// culling copies it into the args it substitutes (AllocateIndirectArgs,
			// InstanceCullingContext.cpp:275). A section drawing from arg set i starts at that
			// set's own StartIndexLocation, so taking the count alone would cast every section's
			// shadow from the front of the mesh.
			BatchElement.NumPrimitives = ShadowArgs->IndexCount / 3u;
			BatchElement.FirstIndex = ShadowArgs->FirstIndex;
			BatchElement.BaseVertexIndex = uint32(FMath::Max(ShadowArgs->BaseVertexIndex, 0));
		}
		else if (IndirectArgsBuffer)
		{
			BatchElement.IndirectArgsBuffer = IndirectArgsBuffer;
			BatchElement.IndirectArgsOffset = IndirectArgsOffset;
			BatchElement.NumPrimitives = 0; // 0 => use IndirectArgsBuffer
		}
		else
		{
			BatchElement.NumPrimitives = NumPrimitives;
		}

		FDynamicPrimitiveUniformBuffer& DynamicPrimitiveUniformBuffer =
			Collector.AllocateOneFrameResource<FDynamicPrimitiveUniformBuffer>();
		DynamicPrimitiveUniformBuffer.Set(Collector.GetRHICommandList(), SceneProxy.GetLocalToWorld(), SceneProxy.GetLocalToWorld(),
			SceneProxy.GetBounds(), SceneProxy.GetLocalBounds(), SceneProxy.GetLocalBounds(), SceneProxy.ReceivesDecals(), false, false, nullptr);
		BatchElement.PrimitiveUniformBufferResource = &DynamicPrimitiveUniformBuffer.UniformBuffer;

		Collector.AddMesh(ViewIndex, Mesh);
	}
}

FPrimitiveViewRelevance FCSGpuMeshSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View);
	Result.bShadowRelevance = IsShadowCast(View);
	Result.bDynamicRelevance = true;
	Result.bStaticRelevance = false;
	Result.bRenderInMainPass = ShouldRenderInMainPass();
	Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
	Result.bRenderCustomDepth = ShouldRenderCustomDepth();
	MaterialRelevance.SetPrimitiveViewRelevance(Result);
	// 必须与引擎预通道 / 速度通道的分工一致（公式抄 FStaticMeshSceneProxy::GetViewRelevance）。
	// 全深度预通道是 DDM_AllOpaqueNoVelocity 时（本工程 r.VelocityOutputPass=0 就是这一档），
	// FDepthPassMeshProcessor::AddMeshBatch 会按 DrawsVelocity() / AlwaysHasVelocity() 把"本帧有速度"
	// 的图元踢出预通道，指望速度通道替它写深度；而基通道是 DepthRead，谁都不写。这里曾硬写 false，
	// 结果材质接了 WPO（草的风动，代理构造时就带上 bHasWorldPositionOffsetVelocity）或 Movable 组件
	// 变换变化的那一帧，深度缓冲里根本没有它。实测症状（2026-09-05）：草在天空背景下被大气 / 雾按远平面
	// 合成成半透明白片，背景是地面时借地面深度"看着正常"、且后画的花把它整片盖掉；拖动 actor 时
	// 花、石阶、石子同样丢深度一帧。依赖 bOpaque，所以必须放在 SetPrimitiveViewRelevance 之后。
	// 速度着色器对两面 / WPO 材质本来就在 shader map 里（TVelocityVS::ShouldCompilePermutation），不需要新编译。
	Result.bVelocityRelevance = (VelocityIncludeStationaryPrimitives(View->GetShaderPlatform()) || DrawsVelocity())
		&& Result.bOpaque && Result.bRenderInMainPass;
	return Result;
}

// -----------------------------------------------------------------------------
// Descriptor-driven buffer set
// -----------------------------------------------------------------------------

void FCSGpuMeshSceneProxy::AddStream(const FCSGpuStreamDesc& Desc)
{
	TUniquePtr<FCSGpuStreamRuntime> Runtime = MakeUnique<FCSGpuStreamRuntime>();
	Runtime->Desc = Desc;
	Streams.Add(MoveTemp(Runtime));
}

void FCSGpuMeshSceneProxy::AddStandardTriangleStreams(uint32 NumIndirectDraws)
{
	// The descriptor list itself lives in CSGpuMeshStreams so the retained UCSMesh set and
	// this proxy-owned set cannot drift apart.
	CSGpuMeshStreams::FStandardStreamOptions Options;
	Options.NumIndirectDraws = NumIndirectDraws;

	TArray<FCSGpuStreamDesc> Descs;
	CSGpuMeshStreams::BuildStandardTriangleStreamDescs(Descs, Options);
	for (const FCSGpuStreamDesc& D : Descs) AddStream(D);
}

const FCSGpuMeshSceneProxy::FCSGpuStreamRuntime* FCSGpuMeshSceneProxy::FindStream(ECSGpuStreamRole Role, uint8 Index) const
{
	for (const TUniquePtr<FCSGpuStreamRuntime>& S : Streams)
		if (S->Desc.Role == Role && S->Desc.TexCoordIndex == Index) return S.Get();
	return nullptr;
}

TRefCountPtr<FRDGPooledBuffer> FCSGpuMeshSceneProxy::GetStreamBuffer(ECSGpuStreamRole Role, uint8 Index) const
{
	const FCSGpuStreamRuntime* S = FindStream(Role, Index);
	return S ? S->Pooled : TRefCountPtr<FRDGPooledBuffer>();
}

FRHIShaderResourceView* FCSGpuMeshSceneProxy::GetStreamSRV(ECSGpuStreamRole Role, uint8 Index) const
{
	const FCSGpuStreamRuntime* S = FindStream(Role, Index);
	return S ? S->SRV.GetReference() : nullptr;
}

TUniquePtr<FLocalVertexFactory> FCSGpuMeshSceneProxy::CreateVertexFactory(ERHIFeatureLevel::Type InFeatureLevel, const char* InDebugName) const
{
	return MakeUnique<FLocalVertexFactory>(InFeatureLevel, InDebugName);
}

void FCSGpuMeshSceneProxy::BuildResidentView(FCSMeshResident& OutResident) const
{
	OutResident.Streams.Reset(Streams.Num());
	for (const TUniquePtr<FCSGpuStreamRuntime>& S : Streams)
	{
		FCSMeshResident::FStream& Stream = OutResident.Streams.AddDefaulted_GetRef();
		Stream.Desc = S->Desc;
		Stream.Pooled = S->Pooled;
	}
	OutResident.VertexCapacity = VertexCapacity;
	OutResident.IndexCapacity = IndexCapacity;
}

void FCSGpuMeshSceneProxy::SetExternalStreams(TSharedPtr<FCSMeshResident, ESPMode::ThreadSafe> InResident)
{
	ExternalResident = MoveTemp(InResident);
}

void FCSGpuMeshSceneProxy::InitGpuGeometry(FRHICommandListBase& RHICmdList)
{
	Streams.Reset();
	VertexCapacity = 0;
	IndexCapacity = 0;
	DrawDesc = FDrawDesc();

	if (ExternalResident.IsValid())
	{
		// Adopt: the buffers already exist and already hold geometry, so there is nothing to
		// allocate and nothing to generate. This is what turns a render-state recreation from
		// "re-run the generation compute" into "rebind the vertex factory".
		for (const FCSMeshResident::FStream& Stream : ExternalResident->Streams)
		{
			AddStream(Stream.Desc);
			Streams.Last()->Pooled = Stream.Pooled;
		}
		VertexCapacity = ExternalResident->VertexCapacity;
		IndexCapacity = ExternalResident->IndexCapacity;
		AllocateStreamsAndBindVF(RHICmdList, /*bAllocateBuffers*/ false);
		return;
	}

	RegisterStreams();                    // leaf: push descriptors + set capacities
	AllocateStreamsAndBindVF(RHICmdList); // base: alloc pooled buffers + SRVs + VF + DrawDesc handles
	BuildGeometry(RHICmdList);            // leaf: run compute into the base-owned buffers
}

void FCSGpuMeshSceneProxy::AllocateStreamsAndBindVF(FRHICommandListBase& RHICmdList, bool bAllocateBuffers)
{
	const uint32 VertUnits = FMath::Max(VertexCapacity, 1u);
	const uint32 IdxUnits = FMath::Max(IndexCapacity, 1u);

	// Leaf-selected vertex factory; created here (not in the ctor) so the virtual dispatch works.
	if (!VertexFactory) VertexFactory = CreateVertexFactory(GetScene().GetFeatureLevel(), VertexFactoryDebugName);

	FLocalVertexFactory::FDataType Data;

	for (TUniquePtr<FCSGpuStreamRuntime>& SPtr : Streams)
	{
		FCSGpuStreamRuntime& S = *SPtr;
		const FCSGpuStreamDesc& D = S.Desc;

		// --- allocate the pooled buffer (adopt mode arrives with one already attached)
		if (bAllocateBuffers)
		{
			FRDGBufferDesc Desc;
			if (D.Role == ECSGpuStreamRole::IndirectArgs)
			{
				Desc = FRDGBufferDesc::CreateIndirectDesc(D.BytesPerElement, D.ElementsPerUnit);
			}
			else
			{
				const uint32 Units = FMath::Max(CSGpuMeshStreams::UnitsForCountSource(D.CountSource, VertUnits, IdxUnits), 1u);
				Desc = FRDGBufferDesc::CreateBufferDesc(D.BytesPerElement, D.ElementsPerUnit * Units);
				if (D.Role == ECSGpuStreamRole::Index)
					Desc.Usage = (Desc.Usage & ~EBufferUsageFlags::VertexBuffer) | EBufferUsageFlags::IndexBuffer;
			}
			S.Pooled = AllocatePooledBuffer(Desc, D.DebugName);
		}
		if (!S.Pooled.IsValid()) continue;

		// --- manual-fetch SRV
		if (D.SrvFormat != PF_Unknown)
			S.SRV = RHICmdList.CreateShaderResourceView(S.Pooled->GetRHI(),
				FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(D.SrvFormat));

		// --- render-resource wrappers for the VF streams / index buffer
		if (D.Role == ECSGpuStreamRole::Index)
		{
			S.IB.Pooled = S.Pooled;
			S.IB.InitResource(RHICmdList);
		}
		else if (D.VfType != VET_None)
		{
			S.VB.Pooled = S.Pooled;
			S.VB.InitResource(RHICmdList);
		}

		// --- vertex-factory binding
		const uint32 VertexStride = D.BytesPerElement * D.ElementsPerUnit;
		switch (D.Role)
		{
		case ECSGpuStreamRole::Position:
			Data.PositionComponent = FVertexStreamComponent(&S.VB, 0, VertexStride, D.VfType);
			Data.PositionComponentSRV = S.SRV;
			break;
		case ECSGpuStreamRole::TangentBasis:
			Data.TangentBasisComponents[0] = FVertexStreamComponent(&S.VB, 0, VertexStride, VET_PackedNormal);
			Data.TangentBasisComponents[1] = FVertexStreamComponent(&S.VB, sizeof(uint32), VertexStride, VET_PackedNormal);
			Data.TangentsSRV = S.SRV;
			break;
		case ECSGpuStreamRole::TexCoord:
		{
			// 一条流可以承载多组 UV（交错，每组 2 个 float）—— 见 FStandardStreamOptions::NumTexCoordSets。
			// 逐组各挂一个 stream component（偏移 8×组号、步长 = 整条 unit），SRV 只有一个槽位、
			// 设一次即可：引擎的 manual fetch 正是按 NumTexCoords 做交错索引的。
			//
			// ⚠️ **stream component 最多只挂 4 个，而 NumTexCoords 可以到 8**，两个数不是一回事：
			// `FStaticMeshDataType::TextureCoordinates` 是 `TFixedAllocator<MAX_STATIC_TEXCOORDS / 2>`
			// （引擎 `Components.h:46`）—— 第 5 个 Add 当场撑爆固定分配器；顶点声明那边也只排属性
			// 4..7（`LocalVertexFactory.cpp:530-549`，不足 4 个还拿最后一个补齐），引擎自己同样把
			// 声明侧钳在 `MAX_TEXCOORDS = 4`（`StaticMeshVertexBuffer.cpp:626`）。
			// 第 5..8 组只经 manual fetch 的 SRV 到达 shader：`MANUAL_VERTEX_FETCH` 在
			// `RHISupportsManualVertexFetch()` 成立时恒开（`LocalVertexFactory.cpp:306-308`），而本插件
			// 只面向 SM5+（`CSGpuInstancedMeshVertexFactory.h:18-19`）⇒ 恒成立。要往非 SM5 平台移植
			// 时，UV4..7 会静默读不到，得先把这条钳位的前提重新论证。
			const int32 NumSets = FMath::Max(int32(D.ElementsPerUnit) / 2, 1);
			const int32 NumDeclaredSets = FMath::Min(NumSets, int32(MAX_STATIC_TEXCOORDS) / 2);
			for (int32 Set = 0; Set < NumDeclaredSets; ++Set)
			{
				Data.TextureCoordinates.Add(FVertexStreamComponent(
					&S.VB, uint32(Set) * 2u * sizeof(float), VertexStride, VET_Float2));
			}
			Data.TextureCoordinatesSRV = S.SRV;
			const int32 DesiredTexCoords = int32(D.TexCoordIndex) + NumSets;
			if (int32(Data.NumTexCoords) < DesiredTexCoords) Data.NumTexCoords = DesiredTexCoords;
			if (D.TexCoordIndex == 0)
			{
				Data.LightMapCoordinateIndex = 0;
				Data.LightMapCoordinateComponent = FVertexStreamComponent(&S.VB, 0, VertexStride, VET_Float2);
			}
			break;
		}
		case ECSGpuStreamRole::Color:
			Data.ColorComponent = FVertexStreamComponent(&S.VB, 0, VertexStride, VET_Color);
			Data.ColorComponentsSRV = S.SRV;
			Data.ColorIndexMask = ~0u;
			break;
		case ECSGpuStreamRole::Index:
			DrawDesc.IndexBuffer = &S.IB;
			break;
		case ECSGpuStreamRole::IndirectArgs:
			DrawDesc.IndirectArgsBuffer = S.Pooled->GetRHI();
			DrawDesc.IndirectArgsOffset = 0;
			break;
		default:
			break; // MeshCounters, AuxVertex: no VF / draw binding
		}
	}

	OnStreamsAllocated(RHICmdList);

	VertexFactory->SetData(RHICmdList, Data);
	VertexFactory->InitResource(RHICmdList);

	DrawDesc.FirstIndex = 0;
	DrawDesc.MinVertexIndex = 0;
	DrawDesc.MaxVertexIndex = VertUnits - 1;
	DrawDesc.bValid = (DrawDesc.IndexBuffer != nullptr);
}

void FCSGpuMeshSceneProxy::ReleaseGpuGeometry()
{
	DrawDesc = FDrawDesc();
	for (TUniquePtr<FCSGpuStreamRuntime>& SPtr : Streams)
	{
		FCSGpuStreamRuntime& S = *SPtr;
		// Release exactly the wrappers that were initialised in AllocateStreamsAndBindVF.
		if (S.Desc.Role == ECSGpuStreamRole::Index) S.IB.ReleaseResource();
		else if (S.Desc.VfType != VET_None) S.VB.ReleaseResource();
		S.SRV.SafeRelease();
		S.Pooled.SafeRelease();
	}
	Streams.Reset();
}
