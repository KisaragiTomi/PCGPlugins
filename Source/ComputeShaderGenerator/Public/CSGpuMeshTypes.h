#pragma once

#include "CoreMinimal.h"
#include "PixelFormat.h"
#include "RenderGraphResources.h"
#include "RenderResource.h"
#include "RHIDefinitions.h"
#include "UObject/ObjectPtr.h"

class UMaterialInterface;

// -----------------------------------------------------------------------------
// Shared GPU-mesh buffer-set types
//
// The GPU-resident mesh base (FCSGpuMeshSceneProxy / UCSGpuMeshComponent) owns a
// *descriptor-driven* buffer set: each vertex/index/indirect/aux buffer is described
// by an FCSGpuStreamDesc, and the base allocates, binds the vertex factory, and reads
// back the set by iterating the descriptors. Adding a new buffer later is one more
// AddStream(...) call in a leaf's RegisterStreams() — the alloc / VF-bind / readback /
// save code never has to change.
// -----------------------------------------------------------------------------

// Thin FRenderResource wrappers that expose a pooled buffer's RHI object as a vertex /
// index buffer for a vertex-factory stream. Shared by every GPU-resident path in the
// plugin: the descriptor-driven mesh base, and the debug geometry built by FCSGpuDebugDraw.
struct FCSPooledVertexBuffer final : public FVertexBuffer
{
	TRefCountPtr<FRDGPooledBuffer> Pooled;
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override
	{
		if (Pooled.IsValid()) VertexBufferRHI = Pooled->GetRHI();
	}
	virtual void ReleaseRHI() override
	{
		VertexBufferRHI.SafeRelease();
	}
};

struct FCSPooledIndexBuffer final : public FIndexBuffer
{
	TRefCountPtr<FRDGPooledBuffer> Pooled;
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override
	{
		if (Pooled.IsValid()) IndexBufferRHI = Pooled->GetRHI();
	}
	virtual void ReleaseRHI() override
	{
		IndexBufferRHI.SafeRelease();
	}
};

// What a stream is used for. Drives buffer usage flags and the vertex-factory binding.
enum class ECSGpuStreamRole : uint8
{
	Position,      // Buffer<float>,  ElementsPerUnit=3 (xyz)     -> VF PositionComponent
	TangentBasis,  // Buffer<uint>,   ElementsPerUnit=2 (TanX,TanZ) packed 8888 -> VF TangentBasisComponents[0/1]
	TexCoord,      // Buffer<float>,  ElementsPerUnit=2 (uv)       -> VF TextureCoordinates[TexCoordIndex]
	Color,         // Buffer<uint>,   ElementsPerUnit=1 (RGBA8)    -> VF ColorComponent
	Index,         // Buffer<uint>,   ElementsPerUnit=1            -> DrawDesc.IndexBuffer (IndexBuffer usage)
	IndirectArgs,  // Indirect buffer, ElementsPerUnit=5           -> DrawDesc.IndirectArgsBuffer (DrawIndexedIndirect)
	MeshCounters,  // Buffer<uint>,   ElementsPerUnit=2 ([0]=vertexCount, [1]=indexCount) -> readback count carrier
	AuxVertex,     // extension slot: per-vertex data with no built-in VF role
};

// How a stream's element count scales. PerVertex/PerIndex multiply the base
// VertexCapacity/IndexCapacity; Fixed uses ElementsPerUnit verbatim.
enum class ECSGpuCountSource : uint8
{
	PerVertex,
	PerIndex,
	Fixed,
	PerTriangle,   // IndexCapacity / 3. For per-face data such as the material registry id.
};

// Which FCSGpuMeshCPUData member(s) a readback stream fills. None = not part of the
// CPU mesh data (e.g. Color today, IndirectArgs, MeshCounters).
enum class ECSGpuMeshSemantic : uint8
{
	None,
	Position,      // -> Positions (float3 per vertex)
	TangentBasis,  // -> Normals + Tangents (two packed 8888 per vertex: [0]=TangentX, [1]=TangentZ/normal)
	TexCoord,      // -> TexCoords (float2 per vertex)
	Index,         // -> Indices (uint per index)
	Color,         // -> Colors (RGBA8 per vertex). Vertex colours existed on the GPU and rendered
	               //    fine, but were dropped on save because no stream carried them to the CPU.
	MaterialId,    // -> TriangleMaterialSlots (uint per triangle). Carries the registry id only;
	               //    the readback layer never resolves it to a UMaterialInterface.
};

// One buffer in the GPU-mesh set. Leaves push these in RegisterStreams(); the base
// consumes them for allocation, vertex-factory binding, and readback.
struct FCSGpuStreamDesc
{
	const TCHAR* DebugName = TEXT("CSGpuStream");
	ECSGpuStreamRole Role = ECSGpuStreamRole::AuxVertex;
	uint32 BytesPerElement = sizeof(uint32); // FRDGBufferDesc::CreateBufferDesc element stride
	uint32 ElementsPerUnit = 1;              // elements written per vertex/index (or the whole Fixed count)
	ECSGpuCountSource CountSource = ECSGpuCountSource::PerVertex;
	EPixelFormat SrvFormat = PF_Unknown;     // PF_Unknown => no manual-fetch SRV created
	EVertexElementType VfType = VET_None;    // VET_None => no vertex-factory stream
	uint8 TexCoordIndex = 0;                 // TexCoord role only (0 also drives LightMapCoordinate)
	ECSGpuMeshSemantic CpuSemantic = ECSGpuMeshSemantic::None;
	bool bReadback = false;                  // participates in the CPU mesh readback loop
};

// CPU snapshot produced only when explicitly saving a GPU mesh. Vertex arrays are sized
// to the GPU-decided vertex count; Indices to the index count (they differ for an indexed
// mesh such as a road, and are equal for a triangle soup). Extensible: extra readback
// streams whose CpuSemantic maps to a known member fill it here; unmapped aux streams can
// be surfaced later without breaking this contract.
struct FCSGpuMeshCPUData
{
	// Which space Positions/Normals/Tangents are expressed in. A single "convert to local"
	// bool could not express this: one leaf defaulted it to false because its data was already
	// local, another defaulted it to false because its data was world - opposite reasons, same
	// flag, and no way for the converter to tell them apart.
	enum class ESpace : uint8 { World, ComponentLocal };

	// Whether vertex-instance attributes are indexed by position or by index corner. GPU render
	// components normally use per-position data; boolean output uses per-corner data so welded
	// positions can still retain source UV/normal/colour seams. For a triangle soup both counts
	// are equal, which is exactly when inferring the layout from array sizes is ambiguous.
	enum class EAttrLayout : uint8 { PerVertex, PerCorner };

	/** StaticMesh 支持 8 条 UV，这里按实际用量留 4 条；不够时改这一个常量即可。 */
	static constexpr int32 MaxTexCoordChannels = 4;

	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	TArray<FVector3f> Tangents;

	/**
	 * 多条 UV 通道。通道 0 是主 UV（用 TexCoords() 取，与旧代码等价）；lightmap / 遮罩 /
	 * 世界投影等额外通道填 1..NumTexCoordChannels-1。只有前 NumTexCoordChannels 条会被
	 * 装配进 MeshDescription，后面的即使非空也会被忽略。
	 */
	TArray<FVector2f> TexCoordChannels[MaxTexCoordChannels];
	int32 NumTexCoordChannels = 1;

	TArray<FVector2f>& TexCoords() { return TexCoordChannels[0]; }
	const TArray<FVector2f>& TexCoords() const { return TexCoordChannels[0]; }

	TArray<FVector4f> Colors;
	TArray<float> BinormalSigns;
	TArray<uint32> Indices;
	TArray<int32> TriangleMaterialSlots;

	// Material table indexed by TriangleMaterialSlots. Kept with the data so producers do not
	// have to pass a parallel array alongside it to every conversion call. Empty slots are the
	// converter's problem (it substitutes the engine default surface material), not the
	// producer's.
	TArray<TObjectPtr<UMaterialInterface>> Materials;

	ESpace SourceSpace = ESpace::World;
	EAttrLayout AttrLayout = EAttrLayout::PerVertex;

	void Reset()
	{
		Positions.Reset();
		Normals.Reset();
		Tangents.Reset();
		for (TArray<FVector2f>& Channel : TexCoordChannels) Channel.Reset();
		NumTexCoordChannels = 1;
		Colors.Reset();
		BinormalSigns.Reset();
		Indices.Reset();
		TriangleMaterialSlots.Reset();
		Materials.Reset();
		SourceSpace = ESpace::World;
		AttrLayout = EAttrLayout::PerVertex;
	}

	// Vertex-instance attributes may be supplied per position or per index corner. GPU render
	// components normally use per-position data; boolean output uses per-corner data so welded
	// positions can still retain source UV/normal/color seams.
	bool IsValid() const
	{
		auto HasValidAttributeCount = [this](int32 Count)
		{
			return Count == Positions.Num() || Count == Indices.Num();
		};

		// 已声明的每条 UV 通道都必须与主通道等长，否则装配时会逐角点越界。
		const int32 ActiveChannels = FMath::Clamp(NumTexCoordChannels, 1, MaxTexCoordChannels);
		for (int32 Channel = 1; Channel < ActiveChannels; ++Channel)
		{
			if (TexCoordChannels[Channel].Num() != TexCoordChannels[0].Num()) return false;
		}

		return Positions.Num() >= 3
			&& Indices.Num() >= 3
			&& Indices.Num() % 3 == 0
			&& HasValidAttributeCount(Normals.Num())
			&& HasValidAttributeCount(Tangents.Num())
			&& HasValidAttributeCount(TexCoords().Num())
			&& (Colors.IsEmpty() || HasValidAttributeCount(Colors.Num()))
			&& (BinormalSigns.IsEmpty() || HasValidAttributeCount(BinormalSigns.Num()))
			&& (TriangleMaterialSlots.IsEmpty() || TriangleMaterialSlots.Num() == Indices.Num() / 3);
	}
};
