#include "CSGpuInstancedMeshSceneProxy.h"
#include "CSGpuInstancedMeshVertexFactory.h"

#include "ConvexVolume.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "Engine/Engine.h"
#include "GlobalShader.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "MaterialDomain.h"
#include "MeshBatch.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "SceneInterface.h"
#include "SceneManagement.h"
#include "SceneView.h"
#include "SceneViewExtension.h"
#include "ShaderParameterStruct.h"

DEFINE_LOG_CATEGORY_STATIC(LogCSGpuInstancedProxy, Log, All);

// -----------------------------------------------------------------------------
// Cull / LOD compute shaders (Shaders/Private/CSGpuInstancedMesh.usf)
// -----------------------------------------------------------------------------

namespace
{
	constexpr uint32 CullGroupSize = 64;
	constexpr uint32 IndirectArgsPerDraw = 5;

	bool IsSupportedPlatform(EShaderPlatform Platform)
	{
		return IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5);
	}
}

class FCSInstancedPackPointsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCSInstancedPackPointsCS);
	SHADER_USE_PARAMETER_STRUCT(FCSInstancedPackPointsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, SrcPointPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, SrcPointNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SrcInstanceCount)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RWPackedInstances)
		SHADER_PARAMETER(FMatrix44f, WorldToComponent)
		SHADER_PARAMETER(FVector3f, BaseSphereCentre)
		SHADER_PARAMETER(float, BaseSphereRadius)
		SHADER_PARAMETER(float, PointInstanceScale)
		SHADER_PARAMETER(uint32, MaxSourceInstances)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsSupportedPlatform(Parameters.Platform);
	}
};

class FCSInstancedClusterCullCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCSInstancedClusterCullCS);
	SHADER_USE_PARAMETER_STRUCT(FCSInstancedClusterCullCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, SrcClusterBounds)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWClusterVisible)
		SHADER_PARAMETER_ARRAY(FVector4f, FrustumPlanes, [6])
		SHADER_PARAMETER(FVector3f, ViewOriginLocal)
		SHADER_PARAMETER(float, ComponentScale)
		SHADER_PARAMETER(float, MaxDrawDistanceSq)
		SHADER_PARAMETER(uint32, NumClusters)
		SHADER_PARAMETER(uint32, bFrustumCull)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsSupportedPlatform(Parameters.Platform);
	}
};

class FCSInstancedInstanceCullCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCSInstancedInstanceCullCS);
	SHADER_USE_PARAMETER_STRUCT(FCSInstancedInstanceCullCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, SrcInstances)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SrcInstanceCount)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWClusterVisible)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RWVisTransforms)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RWVisOrigins)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RWVisLightmap)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWLodCounters)
		SHADER_PARAMETER_ARRAY(FVector4f, FrustumPlanes, [6])
		SHADER_PARAMETER(FVector3f, ViewOriginLocal)
		SHADER_PARAMETER(float, ComponentScale)
		SHADER_PARAMETER(float, ScreenMultiple)
		SHADER_PARAMETER(FVector4f, LodScreenSizes)
		SHADER_PARAMETER(float, MaxDrawDistanceSq)
		SHADER_PARAMETER(uint32, NumLods)
		SHADER_PARAMETER(uint32, NumClusters)
		SHADER_PARAMETER(uint32, ClusterSize)
		SHADER_PARAMETER(uint32, MaxInstancesPerLod)
		SHADER_PARAMETER(uint32, MaxSourceInstances)
		SHADER_PARAMETER(uint32, bFrustumCull)
		SHADER_PARAMETER(uint32, bLodSelect)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsSupportedPlatform(Parameters.Platform);
	}
};

class FCSInstancedBuildArgsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCSInstancedBuildArgsCS);
	SHADER_USE_PARAMETER_STRUCT(FCSInstancedBuildArgsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWLodCounters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWIndirectArgs)
		SHADER_PARAMETER(FUintVector4, LodIndexCount)
		SHADER_PARAMETER(FUintVector4, LodFirstIndex)
		SHADER_PARAMETER(FUintVector4, LodBaseVertex)
		SHADER_PARAMETER(uint32, NumLods)
		SHADER_PARAMETER(uint32, MaxInstancesPerLod)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsSupportedPlatform(Parameters.Platform);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCSInstancedPackPointsCS, "/Plugin/PCGPlugins/Shaders/Private/CSGpuInstancedMesh.usf", "PackPointInstancesCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSInstancedClusterCullCS, "/Plugin/PCGPlugins/Shaders/Private/CSGpuInstancedMesh.usf", "ClusterCullCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSInstancedInstanceCullCS, "/Plugin/PCGPlugins/Shaders/Private/CSGpuInstancedMesh.usf", "InstanceCullCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSInstancedBuildArgsCS, "/Plugin/PCGPlugins/Shaders/Private/CSGpuInstancedMesh.usf", "BuildArgsCS", SF_Compute);

// -----------------------------------------------------------------------------
// Shared per-frame cull driver
//
// The cull passes must land in the renderer's own graph ahead of the base pass — a proxy has no
// per-frame hook of its own, so one view extension walks every live instanced proxy. Registration
// happens on the render thread from Create/DestroyRenderThreadResources, which is also where the
// callback runs, so the set needs no locking.
// -----------------------------------------------------------------------------

namespace
{
	TSet<FCSGpuInstancedMeshSceneProxy*>& GetRegisteredProxies()
	{
		static TSet<FCSGpuInstancedMeshSceneProxy*> Proxies;
		return Proxies;
	}

	class FCSGpuInstancedCullViewExtension : public FSceneViewExtensionBase
	{
	public:
		explicit FCSGpuInstancedCullViewExtension(const FAutoRegister& AutoReg)
			: FSceneViewExtensionBase(AutoReg)
		{
		}

		virtual void PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override
		{
			bCulledThisFamily = false;
		}

		virtual void PreRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView) override
		{
			if (!bLoggedFirstCall)
			{
				bLoggedFirstCall = true;
				UE_LOG(LogCSGpuInstancedProxy, Log, TEXT("[CSGpuInstanced] view extension first fired; %d proxy(s) registered"),
					GetRegisteredProxies().Num());
			}

			// One cull per family: the visible-instance buffers are per-proxy, not per-view, so a
			// second view would overwrite the first one's result before either is drawn.
			if (bCulledThisFamily) return;
			bCulledThisFamily = true;

			for (FCSGpuInstancedMeshSceneProxy* Proxy : GetRegisteredProxies()) Proxy->RunCulling(GraphBuilder, InView);
		}

	private:
		bool bCulledThisFamily = false;
		bool bLoggedFirstCall = false;
	};

	TSharedPtr<FCSGpuInstancedCullViewExtension, ESPMode::ThreadSafe> GCullViewExtension;
}

void FCSGpuInstancedMeshSceneProxy::EnsureCullServiceStarted()
{
	check(IsInGameThread());
	if (!GCullViewExtension.IsValid() && GEngine)
	{
		GCullViewExtension = FSceneViewExtensions::NewExtension<FCSGpuInstancedCullViewExtension>();
		UE_LOG(LogCSGpuInstancedProxy, Log, TEXT("[CSGpuInstanced] cull view extension registered (valid=%d)"),
			GCullViewExtension.IsValid() ? 1 : 0);
	}
}

// -----------------------------------------------------------------------------
// FCSGpuInstancedMeshSceneProxy
// -----------------------------------------------------------------------------

FCSGpuInstancedMeshSceneProxy::FCSGpuInstancedMeshSceneProxy(UCSGpuInstancedMeshComponent* Component)
	: FCSGpuMeshSceneProxy(Component, Component->InstanceMaterial, "FCSGpuInstancedMeshSceneProxy")
	, BaseMesh(Component->GetBaseMeshSnapshot())
	, PackedInstances(Component->GetPackedInstances())
	, ClusterBounds(Component->GetClusterBounds())
	, GpuSource(Component->GetInstanceSourceGPU())
	, GpuPointSource(Component->GetInstancePointSourceGPU())
	, ClusterSize(FMath::Clamp(Component->InstancesPerCluster, 1, 4096))
	, EndCullDistance(FMath::Max(Component->InstanceEndCullDistance, 0.0f))
	, LodScreenSizeScale(FMath::Max(Component->LODScreenSizeScale, 0.01f))
	, bFrustumCull(Component->bGpuFrustumCulling)
	, bLodSelect(Component->bGpuLODSelection)
{
	NumLODs = uint32(FMath::Clamp(BaseMesh.LODs.Num(), 1, CS_GPU_INSTANCED_MAX_LODS));

	if (GpuSource.IsValid() || GpuPointSource.IsValid())
	{
		// Instances live on the GPU: there is no cluster table to build from, so the coarse level
		// is skipped and every instance goes through the fine cull.
		MaxInstances = GpuSource.IsValid() ? GpuSource.Capacity : GpuPointSource.Capacity;
		NumClusters = 0;
		ClusterSize = 0;
	}
	else
	{
		MaxInstances = uint32(PackedInstances.Num() / 5);
		NumClusters = MaxInstances > 0 ? uint32(FMath::DivideAndRoundUp(int32(MaxInstances), ClusterSize)) : 0;
	}

	// Each LOD needs its own fixed-size region in the visible buffers because SV_InstanceID
	// restarts per draw, so the cost scales with LOD count as well as instance count. 80 bytes
	// per slot (3+1+1 float4). Worth knowing about before it shows up as a VRAM surprise.
	const uint64 VisibleBytes = uint64(MaxInstances) * NumLODs * 5ull * sizeof(FVector4f);
	if (VisibleBytes > 64ull * 1024ull * 1024ull)
	{
		UE_LOG(LogCSGpuInstancedProxy, Warning,
			TEXT("%s: %u instances x %u LODs need %.1f MiB of visible-instance buffers. Reduce the LOD count or split the component."),
			*GetOwnerName().ToString(), MaxInstances, NumLODs, double(VisibleBytes) / (1024.0 * 1024.0));
	}
}

FCSGpuInstancedMeshSceneProxy::~FCSGpuInstancedMeshSceneProxy()
{
}

SIZE_T FCSGpuInstancedMeshSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

TUniquePtr<FLocalVertexFactory> FCSGpuInstancedMeshSceneProxy::CreateVertexFactory(ERHIFeatureLevel::Type InFeatureLevel, const char* InDebugName) const
{
	return MakeUnique<FCSGpuInstancedMeshVertexFactory>(InFeatureLevel, InDebugName);
}

void FCSGpuInstancedMeshSceneProxy::CreateRenderThreadResources(FRHICommandListBase& RHICmdList)
{
	// Constant for the proxy's lifetime: no fade, nothing selected, no dithered LOD transition —
	// the cull pass moves an instance between LODs outright. Without this the vertex shader would
	// read an unbound uniform buffer and collapse every instance to the origin.
	FInstancedStaticMeshVFLooseUniformShaderParameters LooseParameters;
	LooseParameters.InstancingViewZCompareZero = FVector4f(ForceInit);
	LooseParameters.InstancingViewZCompareOne = FVector4f(ForceInit);
	LooseParameters.InstancingViewZConstant = FVector4f(ForceInit);
	LooseParameters.InstancingTranslatedWorldViewOriginZero = FVector4f(ForceInit);
	LooseParameters.InstancingTranslatedWorldViewOriginOne = FVector4f(ForceInit);
	// x = fade start distance, y = 1 / fade range, z = render-selected, w = render-deselected.
	LooseParameters.InstancingFadeOutParams = FVector4f(UE_BIG_NUMBER, 0.0f, 1.0f, 1.0f);
	InstancedLooseUniformBuffer = FInstancedStaticMeshVFLooseUniformShaderParametersRef::CreateUniformBufferImmediate(
		LooseParameters, UniformBuffer_MultiFrame);

	FCSGpuMeshSceneProxy::CreateRenderThreadResources(RHICmdList);

	if (!bRegisteredForCulling)
	{
		GetRegisteredProxies().Add(this);
		bRegisteredForCulling = true;
	}
}

void FCSGpuInstancedMeshSceneProxy::DestroyRenderThreadResources()
{
	if (bRegisteredForCulling)
	{
		GetRegisteredProxies().Remove(this);
		bRegisteredForCulling = false;
	}

	if (DiagnosticReadback)
	{
		delete DiagnosticReadback;
		DiagnosticReadback = nullptr;
	}

	InstancedLooseUniformBuffer.SafeRelease();
	FCSGpuMeshSceneProxy::DestroyRenderThreadResources();
}

// -----------------------------------------------------------------------------
// Streams
// -----------------------------------------------------------------------------

uint32 FCSGpuInstancedMeshSceneProxy::GetAuxElementCount(ECSGpuInstancedAuxSlot Slot) const
{
	// Every LOD gets its own full-capacity region in the visible buffers: SV_InstanceID restarts
	// at 0 for each indirect draw, so the per-LOD start offset has to be a CPU-side constant and
	// cannot depend on the GPU-decided counts.
	const uint32 VisibleSlots = MaxInstances * NumLODs;

	switch (Slot)
	{
	// The packed rows are supplied directly by a packed GPU source; for a point source the proxy
	// builds them itself each frame, so it needs the room.
	case ECSGpuInstancedAuxSlot::SourceInstances:   return GpuSource.IsValid() ? 1u : MaxInstances * 5u;
	case ECSGpuInstancedAuxSlot::ClusterBounds:     return FMath::Max(NumClusters, 1u);
	case ECSGpuInstancedAuxSlot::ClusterVisible:    return FMath::Max(NumClusters, 1u);
	case ECSGpuInstancedAuxSlot::VisibleTransforms: return VisibleSlots * 3u;
	case ECSGpuInstancedAuxSlot::VisibleOrigins:    return VisibleSlots;
	case ECSGpuInstancedAuxSlot::VisibleLightmap:   return VisibleSlots;
	case ECSGpuInstancedAuxSlot::LodCounters:       return CS_GPU_INSTANCED_MAX_LODS;
	default:                                        return 1u;
	}
}

void FCSGpuInstancedMeshSceneProxy::RegisterStreams()
{
	VertexCapacity = uint32(BaseMesh.Positions.Num());
	IndexCapacity = uint32(BaseMesh.Indices.Num());
	AddStandardTriangleStreams(NumLODs); // one DrawIndexedIndirect arg set per LOD

	auto AddAux = [this](const TCHAR* DebugName, ECSGpuInstancedAuxSlot Slot, uint32 BytesPerElement, EPixelFormat Format)
	{
		FCSGpuStreamDesc D;
		D.DebugName = DebugName;
		D.Role = ECSGpuStreamRole::AuxVertex;
		D.BytesPerElement = BytesPerElement;
		D.ElementsPerUnit = FMath::Max(GetAuxElementCount(Slot), 1u);
		D.CountSource = ECSGpuCountSource::Fixed;
		D.SrvFormat = Format;
		D.VfType = VET_None;
		D.TexCoordIndex = uint8(Slot);
		AddStream(D);
	};

	AddAux(TEXT("CSGpuInstanced.SourceInstances"), ECSGpuInstancedAuxSlot::SourceInstances, sizeof(FVector4f), PF_A32B32G32R32F);
	AddAux(TEXT("CSGpuInstanced.ClusterBounds"), ECSGpuInstancedAuxSlot::ClusterBounds, sizeof(FVector4f), PF_A32B32G32R32F);
	AddAux(TEXT("CSGpuInstanced.ClusterVisible"), ECSGpuInstancedAuxSlot::ClusterVisible, sizeof(uint32), PF_R32_UINT);
	AddAux(TEXT("CSGpuInstanced.VisibleTransforms"), ECSGpuInstancedAuxSlot::VisibleTransforms, sizeof(FVector4f), PF_A32B32G32R32F);
	AddAux(TEXT("CSGpuInstanced.VisibleOrigins"), ECSGpuInstancedAuxSlot::VisibleOrigins, sizeof(FVector4f), PF_A32B32G32R32F);
	AddAux(TEXT("CSGpuInstanced.VisibleLightmap"), ECSGpuInstancedAuxSlot::VisibleLightmap, sizeof(FVector4f), PF_A32B32G32R32F);
	AddAux(TEXT("CSGpuInstanced.LodCounters"), ECSGpuInstancedAuxSlot::LodCounters, sizeof(uint32), PF_R32_UINT);
}

void FCSGpuInstancedMeshSceneProxy::OnStreamsAllocated(FRHICommandListBase& RHICmdList)
{
	auto* InstancedVF = static_cast<FCSGpuInstancedMeshVertexFactory*>(VertexFactory.Get());
	if (!InstancedVF) return;

	InstancedVF->SetInstanceStreams(
		GetStreamSRV(ECSGpuStreamRole::AuxVertex, uint8(ECSGpuInstancedAuxSlot::VisibleOrigins)),
		GetStreamSRV(ECSGpuStreamRole::AuxVertex, uint8(ECSGpuInstancedAuxSlot::VisibleTransforms)),
		GetStreamSRV(ECSGpuStreamRole::AuxVertex, uint8(ECSGpuInstancedAuxSlot::VisibleLightmap)));
}

// -----------------------------------------------------------------------------
// One-off upload of the base mesh + instance source
// -----------------------------------------------------------------------------

void FCSGpuInstancedMeshSceneProxy::BuildGeometry(FRHICommandListBase& RHICmdList)
{
	FRHICommandListImmediate& RHICmdListImmediate = FRHICommandListExecutor::GetImmediateCommandList();
	FRDGBuilder GraphBuilder(RHICmdListImmediate, RDG_EVENT_NAME("CSGpuInstanced.Upload"));

	auto Register = [this, &GraphBuilder](ECSGpuStreamRole Role, uint8 Index = 0)
	{
		return GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(Role, Index));
	};
	auto RegisterAux = [&Register](ECSGpuInstancedAuxSlot Slot)
	{
		return Register(ECSGpuStreamRole::AuxVertex, uint8(Slot));
	};

	FRDGBufferRef Positions = Register(ECSGpuStreamRole::Position);
	FRDGBufferRef Tangents = Register(ECSGpuStreamRole::TangentBasis);
	FRDGBufferRef TexCoords = Register(ECSGpuStreamRole::TexCoord);
	FRDGBufferRef Colors = Register(ECSGpuStreamRole::Color);
	FRDGBufferRef Indices = Register(ECSGpuStreamRole::Index);
	FRDGBufferRef IndirectArgs = Register(ECSGpuStreamRole::IndirectArgs);
	FRDGBufferRef MeshCounters = Register(ECSGpuStreamRole::MeshCounters);

	GraphBuilder.QueueBufferUpload(Positions, BaseMesh.Positions.GetData(), BaseMesh.Positions.Num() * sizeof(FVector3f), ERDGInitialDataFlags::None);
	GraphBuilder.QueueBufferUpload(Tangents, BaseMesh.TangentBasis.GetData(), BaseMesh.TangentBasis.Num() * sizeof(uint32), ERDGInitialDataFlags::None);
	GraphBuilder.QueueBufferUpload(TexCoords, BaseMesh.TexCoords.GetData(), BaseMesh.TexCoords.Num() * sizeof(FVector2f), ERDGInitialDataFlags::None);
	GraphBuilder.QueueBufferUpload(Colors, BaseMesh.Colors.GetData(), BaseMesh.Colors.Num() * sizeof(uint32), ERDGInitialDataFlags::None);
	GraphBuilder.QueueBufferUpload(Indices, BaseMesh.Indices.GetData(), BaseMesh.Indices.Num() * sizeof(uint32), ERDGInitialDataFlags::None);

	// The readback path (ReadbackMeshSync / save-to-StaticMesh) sees the single base-mesh copy,
	// not the instanced result — the instances only ever exist as transforms.
	const uint32 Counters[2] = { uint32(BaseMesh.Positions.Num()), uint32(BaseMesh.Indices.Num()) };
	GraphBuilder.QueueBufferUpload(MeshCounters, Counters, sizeof(Counters), ERDGInitialDataFlags::None);

	// Zeroed until the first cull pass runs, so an unculled frame draws nothing rather than garbage.
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgs, PF_R32_UINT)), 0u);

	FRDGBufferRef SourceInstances = RegisterAux(ECSGpuInstancedAuxSlot::SourceInstances);
	FRDGBufferRef ClusterBoundsBuffer = RegisterAux(ECSGpuInstancedAuxSlot::ClusterBounds);

	if (!GpuSource.IsValid() && PackedInstances.Num() > 0) GraphBuilder.QueueBufferUpload(SourceInstances, PackedInstances.GetData(), PackedInstances.Num() * sizeof(FVector4f), ERDGInitialDataFlags::None);
	if (ClusterBounds.Num() > 0) GraphBuilder.QueueBufferUpload(ClusterBoundsBuffer, ClusterBounds.GetData(), ClusterBounds.Num() * sizeof(FVector4f), ERDGInitialDataFlags::None);

	// RDG's default epilogue state (SRVMask) is illegal for index / indirect usage.
	GraphBuilder.SetBufferAccessFinal(Positions, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
	GraphBuilder.SetBufferAccessFinal(Tangents, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
	GraphBuilder.SetBufferAccessFinal(TexCoords, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
	GraphBuilder.SetBufferAccessFinal(Colors, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
	GraphBuilder.SetBufferAccessFinal(Indices, ERHIAccess::VertexOrIndexBuffer);
	GraphBuilder.SetBufferAccessFinal(IndirectArgs, ERHIAccess::IndirectArgs);
	GraphBuilder.SetBufferAccessFinal(MeshCounters, ERHIAccess::CopySrc);
	GraphBuilder.SetBufferAccessFinal(SourceInstances, ERHIAccess::SRVMask);
	GraphBuilder.SetBufferAccessFinal(ClusterBoundsBuffer, ERHIAccess::SRVMask);

	GraphBuilder.Execute();
}

// -----------------------------------------------------------------------------
// Per-frame culling
// -----------------------------------------------------------------------------

void FCSGpuInstancedMeshSceneProxy::RunCulling(FRDGBuilder& GraphBuilder, const FSceneView& View)
{
	if (DiagnosticState == EDiagnosticState::Pending)
	{
		UE_LOG(LogCSGpuInstancedProxy, Log, TEXT("[CSGpuInstanced] RunCulling entered: DrawValid=%d MaxInstances=%u NumClusters=%u"),
			DrawDesc.bValid ? 1 : 0, MaxInstances, NumClusters);
	}

	if (!DrawDesc.bValid || MaxInstances == 0) return;

	RDG_EVENT_SCOPE(GraphBuilder, "CSGpuInstanced.Cull");

	// Cull in component-local space: the frustum and the view origin come across in double
	// precision and land as plain floats, which keeps LWC out of the shader entirely.
	const FMatrix ComponentToWorld = GetLocalToWorld();
	const FMatrix WorldToLocal = ComponentToWorld.Inverse();

	FVector4f FrustumPlanes[6];
	const FConvexVolume& Frustum = View.GetCullingFrustum();
	for (int32 i = 0; i < 6; ++i)
	{
		// Fewer than six planes means an unbounded side; a plane that rejects nothing keeps the
		// shader loop uniform.
		FPlane Plane = Frustum.Planes.IsValidIndex(i) ? Frustum.Planes[i] : FPlane(0.0, 0.0, 1.0, -UE_BIG_NUMBER);
		Plane = Plane.TransformBy(WorldToLocal);

		const FVector Normal(Plane.X, Plane.Y, Plane.Z);
		const double Length = Normal.Size();
		if (Length > UE_DOUBLE_SMALL_NUMBER) FrustumPlanes[i] = FVector4f(FVector3f(Normal / Length), float(Plane.W / Length));
		else FrustumPlanes[i] = FVector4f(0.0f, 0.0f, 1.0f, -UE_BIG_NUMBER);
	}

	const FVector4 ViewOriginLocal4 = WorldToLocal.TransformPosition(View.ViewMatrices.GetViewOrigin());
	const FVector3f ViewOriginLocal = FVector3f(float(ViewOriginLocal4.X), float(ViewOriginLocal4.Y), float(ViewOriginLocal4.Z));
	const FVector Scale = ComponentToWorld.GetScaleVector();
	const float ComponentScale = FMath::Max(float(FMath::Max3(Scale.X, Scale.Y, Scale.Z)), UE_KINDA_SMALL_NUMBER);

	const FMatrix& ProjMatrix = View.ViewMatrices.GetProjectionMatrix();
	const float ScreenMultiple = FMath::Max(0.5f * float(ProjMatrix.M[0][0]), 0.5f * float(ProjMatrix.M[1][1]));

	FVector4f LodScreenSizes(ForceInit);
	FUintVector4 LodIndexCount(ForceInit);
	FUintVector4 LodFirstIndex(ForceInit);
	FUintVector4 LodBaseVertex(ForceInit);
	for (int32 Lod = 0; Lod < int32(NumLODs); ++Lod)
	{
		const FCSGpuInstancedLODRange& Range = BaseMesh.LODs[Lod];
		LodScreenSizes[Lod] = Range.ScreenSize * LodScreenSizeScale;
		LodIndexCount[Lod] = Range.NumIndices;
		LodFirstIndex[Lod] = Range.FirstIndex;
		LodBaseVertex[Lod] = Range.BaseVertex;
	}

	auto RegisterAux = [this, &GraphBuilder](ECSGpuInstancedAuxSlot Slot)
	{
		return GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::AuxVertex, uint8(Slot)));
	};

	FRDGBufferRef ClusterBoundsBuffer = RegisterAux(ECSGpuInstancedAuxSlot::ClusterBounds);
	FRDGBufferRef ClusterVisible = RegisterAux(ECSGpuInstancedAuxSlot::ClusterVisible);
	FRDGBufferRef VisTransforms = RegisterAux(ECSGpuInstancedAuxSlot::VisibleTransforms);
	FRDGBufferRef VisOrigins = RegisterAux(ECSGpuInstancedAuxSlot::VisibleOrigins);
	FRDGBufferRef VisLightmap = RegisterAux(ECSGpuInstancedAuxSlot::VisibleLightmap);
	FRDGBufferRef LodCounters = RegisterAux(ECSGpuInstancedAuxSlot::LodCounters);
	FRDGBufferRef IndirectArgs = GraphBuilder.RegisterExternalBuffer(GetStreamBuffer(ECSGpuStreamRole::IndirectArgs));

	// The visible buffers and the indirect args were left in external access mode by last frame's
	// pass so the draw could read them; hand them back to RDG before writing them again.
	GraphBuilder.UseInternalAccessMode(VisTransforms);
	GraphBuilder.UseInternalAccessMode(VisOrigins);
	GraphBuilder.UseInternalAccessMode(VisLightmap);
	GraphBuilder.UseInternalAccessMode(IndirectArgs);

	// Instance count: a plain uniform for the CPU array, the producer's GPU counter otherwise.
	FRDGBufferRef InstanceCount;
	if (GpuSource.IsValid()) InstanceCount = GraphBuilder.RegisterExternalBuffer(GpuSource.Counter);
	else if (GpuPointSource.IsValid()) InstanceCount = GraphBuilder.RegisterExternalBuffer(GpuPointSource.Counter);
	else
	{
		InstanceCount = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("CSGpuInstanced.InstanceCount"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(InstanceCount, PF_R32_UINT)), MaxInstances);
	}

	FRDGBufferRef SourceInstances = GpuSource.IsValid()
		? GraphBuilder.RegisterExternalBuffer(GpuSource.PackedInstances)
		: RegisterAux(ECSGpuInstancedAuxSlot::SourceInstances);

	FRDGBufferUAVRef ClusterVisibleUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(ClusterVisible, PF_R32_UINT));
	FRDGBufferUAVRef LodCountersUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(LodCounters, PF_R32_UINT));
	FRDGBufferUAVRef IndirectArgsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgs, PF_R32_UINT));

	AddClearUAVPass(GraphBuilder, LodCountersUAV, 0u);

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GetScene().GetFeatureLevel());
	const float MaxDrawDistanceSq = EndCullDistance > 0.0f ? EndCullDistance * EndCullDistance : 0.0f;

	// Point source: build this frame's instance rows before anything reads them. Running it here
	// rather than when the producer appends is what removes the need for any notification — the
	// rows are always rebuilt from whatever the point buffer holds right now.
	if (GpuPointSource.IsValid())
	{
		FCSInstancedPackPointsCS::FParameters* Params = GraphBuilder.AllocParameters<FCSInstancedPackPointsCS::FParameters>();
		Params->SrcPointPositions = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(GraphBuilder.RegisterExternalBuffer(GpuPointSource.Positions), PF_A32B32G32R32F));
		Params->SrcPointNormals = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(GraphBuilder.RegisterExternalBuffer(GpuPointSource.Normals), PF_A32B32G32R32F));
		Params->SrcInstanceCount = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(InstanceCount, PF_R32_UINT));
		Params->RWPackedInstances = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(SourceInstances, PF_A32B32G32R32F));
		// Point positions are absolute world space. The component sits at ordinary level
		// coordinates, so a float matrix keeps sub-millimetre accuracy over the level.
		Params->WorldToComponent = FMatrix44f(WorldToLocal);
		Params->BaseSphereCentre = FVector3f(BaseMesh.LocalBounds.GetCenter());
		Params->BaseSphereRadius = float(BaseMesh.LocalBounds.GetExtent().Size());
		Params->PointInstanceScale = GpuPointSource.InstanceScale;
		Params->MaxSourceInstances = MaxInstances;

		TShaderMapRef<FCSInstancedPackPointsCS> Shader(ShaderMap);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("PackPointInstances"), Shader, Params,
			FComputeShaderUtils::GetGroupCount(MaxInstances, CullGroupSize));
	}

	// Coarse level.
	if (NumClusters > 0)
	{
		FCSInstancedClusterCullCS::FParameters* Params = GraphBuilder.AllocParameters<FCSInstancedClusterCullCS::FParameters>();
		Params->SrcClusterBounds = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ClusterBoundsBuffer, PF_A32B32G32R32F));
		Params->RWClusterVisible = ClusterVisibleUAV;
		for (int32 i = 0; i < 6; ++i) Params->FrustumPlanes[i] = FrustumPlanes[i];
		Params->ViewOriginLocal = ViewOriginLocal;
		Params->ComponentScale = ComponentScale;
		Params->MaxDrawDistanceSq = MaxDrawDistanceSq;
		Params->NumClusters = NumClusters;
		Params->bFrustumCull = bFrustumCull ? 1u : 0u;

		TShaderMapRef<FCSInstancedClusterCullCS> Shader(ShaderMap);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("ClusterCull"), Shader, Params,
			FComputeShaderUtils::GetGroupCount(NumClusters, CullGroupSize));
	}

	// Fine level + compaction.
	{
		FCSInstancedInstanceCullCS::FParameters* Params = GraphBuilder.AllocParameters<FCSInstancedInstanceCullCS::FParameters>();
		Params->SrcInstances = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SourceInstances, PF_A32B32G32R32F));
		Params->SrcInstanceCount = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(InstanceCount, PF_R32_UINT));
		Params->RWClusterVisible = ClusterVisibleUAV;
		Params->RWVisTransforms = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(VisTransforms, PF_A32B32G32R32F));
		Params->RWVisOrigins = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(VisOrigins, PF_A32B32G32R32F));
		Params->RWVisLightmap = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(VisLightmap, PF_A32B32G32R32F));
		Params->RWLodCounters = LodCountersUAV;
		for (int32 i = 0; i < 6; ++i) Params->FrustumPlanes[i] = FrustumPlanes[i];
		Params->ViewOriginLocal = ViewOriginLocal;
		Params->ComponentScale = ComponentScale;
		Params->ScreenMultiple = ScreenMultiple;
		Params->LodScreenSizes = LodScreenSizes;
		Params->MaxDrawDistanceSq = MaxDrawDistanceSq;
		Params->NumLods = NumLODs;
		Params->NumClusters = NumClusters;
		Params->ClusterSize = NumClusters > 0 ? uint32(ClusterSize) : 0u;
		Params->MaxInstancesPerLod = MaxInstances;
		Params->MaxSourceInstances = MaxInstances;
		Params->bFrustumCull = bFrustumCull ? 1u : 0u;
		Params->bLodSelect = bLodSelect ? 1u : 0u;

		TShaderMapRef<FCSInstancedInstanceCullCS> Shader(ShaderMap);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstanceCull"), Shader, Params,
			FComputeShaderUtils::GetGroupCount(MaxInstances, CullGroupSize));
	}

	// Indirect args, one set per LOD.
	{
		FCSInstancedBuildArgsCS::FParameters* Params = GraphBuilder.AllocParameters<FCSInstancedBuildArgsCS::FParameters>();
		Params->RWLodCounters = LodCountersUAV;
		Params->RWIndirectArgs = IndirectArgsUAV;
		Params->LodIndexCount = LodIndexCount;
		Params->LodFirstIndex = LodFirstIndex;
		Params->LodBaseVertex = LodBaseVertex;
		Params->NumLods = NumLODs;
		Params->MaxInstancesPerLod = MaxInstances;

		TShaderMapRef<FCSInstancedBuildArgsCS> Shader(ShaderMap);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("BuildArgs"), Shader, Params, FIntVector(1, 1, 1));
	}

	// One-shot diagnostics: copy the args the cull just produced and log them next frame.
	if (DiagnosticState == EDiagnosticState::Pending)
	{
		DiagnosticReadback = new FRHIGPUBufferReadback(TEXT("CSGpuInstanced.DiagArgs"));
		AddEnqueueCopyPass(GraphBuilder, DiagnosticReadback, IndirectArgs, sizeof(uint32) * IndirectArgsPerDraw * NumLODs);
		DiagnosticState = EDiagnosticState::Waiting;
	}
	else if (DiagnosticState == EDiagnosticState::Waiting && DiagnosticReadback && DiagnosticReadback->IsReady())
	{
		const uint32 NumArgs = IndirectArgsPerDraw * NumLODs;
		if (const uint32* Args = static_cast<const uint32*>(DiagnosticReadback->Lock(sizeof(uint32) * NumArgs)))
		{
			for (uint32 Lod = 0; Lod < NumLODs; ++Lod)
			{
				const uint32* A = Args + Lod * IndirectArgsPerDraw;
				UE_LOG(LogCSGpuInstancedProxy, Log,
					TEXT("[CSGpuInstanced] %s LOD%u args: IndexCount=%u Instances=%u FirstIndex=%u BaseVertex=%u (capacity %u instances, %u clusters, verts %u, indices %u)"),
					*GetOwnerName().ToString(), Lod, A[0], A[1], A[2], A[3], MaxInstances, NumClusters, VertexCapacity, IndexCapacity);
			}
			DiagnosticReadback->Unlock();
		}
		delete DiagnosticReadback;
		DiagnosticReadback = nullptr;
		DiagnosticState = EDiagnosticState::Done;
	}

	// The draw reads these through raw SRVs and an indirect-args binding that RDG knows nothing
	// about, and it happens later in this same graph — SetBufferAccessFinal would only transition
	// them at the very end, far too late. External access mode transitions them here and holds it.
	GraphBuilder.UseExternalAccessMode(VisTransforms, ERHIAccess::SRVMask);
	GraphBuilder.UseExternalAccessMode(VisOrigins, ERHIAccess::SRVMask);
	GraphBuilder.UseExternalAccessMode(VisLightmap, ERHIAccess::SRVMask);
	GraphBuilder.UseExternalAccessMode(IndirectArgs, ERHIAccess::IndirectArgs);
}

// -----------------------------------------------------------------------------
// Draw
// -----------------------------------------------------------------------------

void FCSGpuInstancedMeshSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const
{
	if (!DrawDesc.bValid || DrawDesc.IndexBuffer == nullptr || !VertexFactory) return;
	if (DrawDesc.IndirectArgsBuffer == nullptr || MaxInstances == 0) return;

	FMaterialRenderProxy* MaterialProxy = Material->GetRenderProxy();

	if (!bLoggedFirstDraw)
	{
		bLoggedFirstDraw = true;
		UE_LOG(LogCSGpuInstancedProxy, Log,
			TEXT("[CSGpuInstanced] %s first draw: %u LODs, material '%s', bounds radius %.1f"),
			*GetOwnerName().ToString(), NumLODs, *Material->GetName(), GetBounds().SphereRadius);
	}

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		if ((VisibilityMap & (1 << ViewIndex)) == 0) continue;

		FDynamicPrimitiveUniformBuffer& DynamicPrimitiveUniformBuffer =
			Collector.AllocateOneFrameResource<FDynamicPrimitiveUniformBuffer>();
		DynamicPrimitiveUniformBuffer.Set(Collector.GetRHICommandList(), GetLocalToWorld(), GetLocalToWorld(),
			GetBounds(), GetLocalBounds(), GetLocalBounds(), ReceivesDecals(), false, false, nullptr);

		// One indirect draw per LOD. The instance count sits in the args the cull pass wrote, so a
		// LOD nobody selected costs an empty draw call and nothing else.
		for (uint32 Lod = 0; Lod < NumLODs; ++Lod)
		{
			FMeshBatch& Mesh = Collector.AllocateMesh();
			Mesh.VertexFactory = VertexFactory.Get();
			Mesh.MaterialRenderProxy = MaterialProxy;
			Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
			Mesh.Type = PT_TriangleList;
			Mesh.DepthPriorityGroup = SDPG_World;
			Mesh.bCanApplyViewModeOverrides = false;
			Mesh.CastShadow = bBatchCastShadow;

			FMeshBatchElement& BatchElement = Mesh.Elements[0];
			BatchElement.IndexBuffer = DrawDesc.IndexBuffer;
			BatchElement.FirstIndex = 0;
			BatchElement.MinVertexIndex = 0;
			BatchElement.MaxVertexIndex = DrawDesc.MaxVertexIndex;
			BatchElement.NumPrimitives = 0; // 0 => read the count from IndirectArgsBuffer
			BatchElement.IndirectArgsBuffer = DrawDesc.IndirectArgsBuffer;
			BatchElement.IndirectArgsOffset = Lod * IndirectArgsPerDraw * sizeof(uint32);
			// Start of this LOD's region in the visible-instance buffers; the vertex factory adds
			// SV_InstanceID to it.
			BatchElement.UserIndex = int32(Lod * MaxInstances);
			BatchElement.LooseParametersUniformBuffer = InstancedLooseUniformBuffer;
			BatchElement.PrimitiveUniformBufferResource = &DynamicPrimitiveUniformBuffer.UniformBuffer;
			// This factory has no primitive-id stream (that is what puts the shader on the
			// manual-fetch instancing path), so FMeshElementCollector::AddMesh skips the GPU-Scene
			// dynamic-primitive registration that would otherwise set PrimitiveIdMode. Left at its
			// PrimID_FromPrimitiveSceneInfo default the renderer ensures on the primitive uniform
			// buffer above and drops the whole batch. The shader never reads a primitive id — it
			// reads the Primitive uniform buffer — so force it to zero.
			BatchElement.PrimitiveIdMode = PrimID_ForceZero;

			Collector.AddMesh(ViewIndex, Mesh);
		}
	}
}
