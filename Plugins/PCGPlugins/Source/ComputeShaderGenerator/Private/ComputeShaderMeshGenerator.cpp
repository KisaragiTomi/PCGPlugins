#include "ComputeShaderMeshGenerator.h"

#include "Engine/TextureRenderTarget2D.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RenderTargetPool.h"
#include "TextureResource.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#include "MeshDescription.h"
#include "MeshDescriptionToDynamicMesh.h"
#if WITH_EDITOR
#include "LandscapeDataAccess.h"
#endif
#include "RawIndexBuffer.h"
#include "RenderGraphResources.h"
#include "RHI.h"
#include "RHIGPUReadback.h"
#include "ShaderParameterStruct.h"
#include "StaticMeshResources.h"
#include "StaticMeshAttributes.h"
#include "UObject/UObjectIterator.h"

#include <openvdb/tools/ParticlesToLevelSet.h>
#include <openvdb/tools/VolumeToMesh.h>

namespace
{
constexpr int32 CSGeneratorMinTextureDimension = 1;
constexpr int32 CSGeneratorDefaultTextureDimension = 1;
constexpr float CSGeneratorMinVoxelSize = 1.0e-3f;
const FName CSGeneratorDefaultRequestId(TEXT("Default"));


bool IsFiniteCSGeneratorVector(const FVector& Vector)
{
	return !Vector.ContainsNaN()
		&& FMath::IsFinite(Vector.X)
		&& FMath::IsFinite(Vector.Y)
		&& FMath::IsFinite(Vector.Z);
}

int32 CeilDivInt64ToInt32(int64 Numerator, int64 Denominator)
{
	if (Denominator <= 0)
	{
		return 0;
	}
	return int32((Numerator + Denominator - 1) / Denominator);
}

FRDGTextureRef RegisterRenderTargetTexture(FRDGBuilder& GraphBuilder, FTextureRenderTargetResource* RenderTargetResource, const TCHAR* DebugName)
{
	if (!RenderTargetResource || !RenderTargetResource->GetRenderTargetTexture())
	{
		return nullptr;
	}
	return RegisterExternalTexture(GraphBuilder, RenderTargetResource->GetRenderTargetTexture(), DebugName);
}

FIntPoint GetLinearDataTextureSize(int64 ElementCount, int32 MaxTextureDimension)
{
	const int32 SafeMaxDimension = FMath::Max(CSGeneratorMinTextureDimension, MaxTextureDimension);
	const int64 SafeElementCount = FMath::Max<int64>(1, ElementCount);
	const int32 Width = FMath::Min<int32>(
		SafeMaxDimension,
		FMath::Max<int64>(CSGeneratorDefaultTextureDimension, FMath::Min<int64>(SafeElementCount, SafeMaxDimension)));
	const int32 Height = FMath::Max(CSGeneratorDefaultTextureDimension, CeilDivInt64ToInt32(SafeElementCount, Width));
	return FIntPoint(Width, Height);
}

void UploadLinearColorsToRenderTarget(UTextureRenderTarget2D* RenderTarget, TArray<FLinearColor> Colors)
{
	if (!RenderTarget || RenderTarget->SizeX <= 0 || RenderTarget->SizeY <= 0)
	{
		return;
	}

	const int32 Width = RenderTarget->SizeX;
	const int32 Height = RenderTarget->SizeY;
	const int32 PixelCount = Width * Height;
	if (Colors.Num() < PixelCount)
	{
		Colors.SetNumZeroed(PixelCount);
	}
	else if (Colors.Num() > PixelCount)
	{
		Colors.SetNum(PixelCount, EAllowShrinking::No);
	}

	FTextureRenderTargetResource* TextureTarget = RenderTarget->GameThread_GetRenderTargetResource();
	ENQUEUE_RENDER_COMMAND(CSMeshGeneratorUploadLinearColorsToRenderTarget)(
		[TextureTarget, Colors = MoveTemp(Colors), Width, Height](FRHICommandListImmediate& RHICmdList)
		{
			if (!TextureTarget)
			{
				return;
			}

			FTextureRHIRef TextureRHI = TextureTarget->GetRenderTargetTexture();
			if (!TextureRHI.IsValid())
			{
				return;
			}

			uint32 DestStride = 0;
			void* DestData = RHILockTexture2D(TextureRHI, 0, RLM_WriteOnly, DestStride, false);
			if (!DestData || DestStride == 0)
			{
				if (DestData)
				{
					RHIUnlockTexture2D(TextureRHI, 0, false);
				}
				return;
			}

			constexpr uint32 PixelBytes = sizeof(FLinearColor);
			const uint32 RowBytes = uint32(Width) * PixelBytes;
			const uint8* SourceBytes = reinterpret_cast<const uint8*>(Colors.GetData());
			uint8* DestBytes = static_cast<uint8*>(DestData);
			for (int32 Y = 0; Y < Height; ++Y)
			{
				FMemory::Memcpy(DestBytes + uint64(Y) * DestStride, SourceBytes + uint64(Y) * RowBytes, RowBytes);
			}
			RHIUnlockTexture2D(TextureRHI, 0, false);
		});
	FlushRenderingCommands();
}

class FCSGeneratorVDBParticleList
{
	struct FParticle
	{
		openvdb::Vec3R Position;
		openvdb::Real Radius = 0.0;
	};

public:
	using PosType = openvdb::Vec3R;

	explicit FCSGeneratorVDBParticleList(openvdb::Real InRadiusScale = 1.0)
		: RadiusScale(InRadiusScale)
	{
	}

	void Add(const FVector& Position, float Radius)
	{
		FParticle Particle;
		Particle.Position = openvdb::Vec3R(Position.X, Position.Y, Position.Z);
		Particle.Radius = Radius;
		Particles.push_back(Particle);
	}

	size_t size() const
	{
		return Particles.size();
	}

	void getPos(size_t Index, openvdb::Vec3R& OutPosition) const
	{
		OutPosition = Particles[Index].Position;
	}

	void getPosRad(size_t Index, openvdb::Vec3R& OutPosition, openvdb::Real& OutRadius) const
	{
		OutPosition = Particles[Index].Position;
		OutRadius = RadiusScale * Particles[Index].Radius;
	}

	void getPosRadVel(size_t Index, openvdb::Vec3R& OutPosition, openvdb::Real& OutRadius, openvdb::Vec3R& OutVelocity) const
	{
		getPosRad(Index, OutPosition, OutRadius);
		OutVelocity = openvdb::Vec3R(1.0, 1.0, 1.0);
	}

private:
	openvdb::Real RadiusScale = 1.0;
	std::vector<FParticle> Particles;
};

void ConvertVDBVolumeToMeshDescription(openvdb::FloatGrid::ConstPtr SDFVolume, FMeshDescription& OutRawMesh)
{
	OutRawMesh.Empty();
	if (!SDFVolume)
	{
		return;
	}

	std::vector<openvdb::Vec3s> Points;
	std::vector<openvdb::Vec3I> Triangles;
	std::vector<openvdb::Vec4I> Quads;
	openvdb::tools::volumeToMesh(*SDFVolume, Points, Triangles, Quads, 0.001, 0.25);

	FStaticMeshAttributes Attributes(OutRawMesh);
	Attributes.Register();
	TVertexAttributesRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
	TPolygonGroupAttributesRef<FName> PolygonGroupImportedMaterialSlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
	TVertexInstanceAttributesRef<FVector3f> VertexInstanceNormals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> VertexInstanceTangents = Attributes.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<float> VertexInstanceBinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesRef<FVector4f> VertexInstanceColors = Attributes.GetVertexInstanceColors();
	TVertexInstanceAttributesRef<FVector2f> VertexInstanceUVs = Attributes.GetVertexInstanceUVs();

	if (VertexInstanceUVs.GetNumChannels() < 1)
	{
		VertexInstanceUVs.SetNumChannels(1);
	}

	const FPolygonGroupID PolygonGroupID = OutRawMesh.CreatePolygonGroup();
	PolygonGroupImportedMaterialSlotNames[PolygonGroupID] = FName(TEXT("OpenVDB_Material_0"));

	TArray<FVertexID> VertexIDs;
	VertexIDs.Reserve(static_cast<int32>(Points.size()));
	for (const openvdb::Vec3s& Point : Points)
	{
		const FVertexID NewVertexID = OutRawMesh.CreateVertex();
		VertexPositions[NewVertexID] = FVector3f(Point[0], Point[1], Point[2]);
		VertexIDs.Add(NewVertexID);
	}

	auto AppendTriangle = [&OutRawMesh, PolygonGroupID, &VertexIDs, &VertexPositions, &VertexInstanceNormals,
		&VertexInstanceTangents, &VertexInstanceBinormalSigns, &VertexInstanceColors, &VertexInstanceUVs](int32 Index0, int32 Index1, int32 Index2)
	{
		if (!VertexIDs.IsValidIndex(Index0) || !VertexIDs.IsValidIndex(Index1) || !VertexIDs.IsValidIndex(Index2))
		{
			return;
		}

		const FVector3f P0 = VertexPositions[VertexIDs[Index0]];
		const FVector3f P1 = VertexPositions[VertexIDs[Index1]];
		const FVector3f P2 = VertexPositions[VertexIDs[Index2]];
		const FVector Normal = FVector::CrossProduct(FVector(P1 - P0), FVector(P2 - P0)).GetSafeNormal();
		FVector Tangent = FVector(P1 - P0).GetSafeNormal();
		if (Tangent.IsNearlyZero())
		{
			Tangent = FVector::CrossProduct(FVector::UpVector, Normal).GetSafeNormal();
		}
		if (Tangent.IsNearlyZero())
		{
			Tangent = FVector::ForwardVector;
		}

		TArray<FVertexInstanceID> VertexInstanceIDs;
		VertexInstanceIDs.SetNum(3);
		const int32 SourceIndices[3] = {Index0, Index1, Index2};
		for (int32 Corner = 0; Corner < UE_ARRAY_COUNT(SourceIndices); ++Corner)
		{
			const FVertexID VertexID = VertexIDs[SourceIndices[Corner]];
			const FVertexInstanceID VertexInstanceID = OutRawMesh.CreateVertexInstance(VertexID);

			VertexInstanceTangents[VertexInstanceID] = FVector3f(Tangent);
			VertexInstanceNormals[VertexInstanceID] = FVector3f(Normal);
			VertexInstanceBinormalSigns[VertexInstanceID] = GetBasisDeterminantSign(
				FVector(VertexInstanceTangents[VertexInstanceID]).GetSafeNormal(),
				FVector(FVector3f(Normal) ^ VertexInstanceTangents[VertexInstanceID]).GetSafeNormal(),
				Normal);
			VertexInstanceColors[VertexInstanceID] = FVector4f(1.0f);
			VertexInstanceUVs.Set(VertexInstanceID, 0, FVector2f(0.0f, 0.0f));
			VertexInstanceIDs[Corner] = VertexInstanceID;
		}
		OutRawMesh.CreatePolygon(PolygonGroupID, VertexInstanceIDs);
	};

	for (const openvdb::Vec4I& Quad : Quads)
	{
		AppendTriangle(Quad[0], Quad[1], Quad[2]);
		AppendTriangle(Quad[2], Quad[3], Quad[0]);
	}

	for (const openvdb::Vec3I& Triangle : Triangles)
	{
		AppendTriangle(Triangle[0], Triangle[1], Triangle[2]);
	}
}
}

class FExtractStaticMeshTrianglesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FExtractStaticMeshTrianglesCS);
	SHADER_USE_PARAMETER_STRUCT(FExtractStaticMeshTrianglesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(Buffer<uint>, IndexBuffer)
		SHADER_PARAMETER_SRV(Buffer<float>, PositionBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, ReferencePoints)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutTriangleVertices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutTriangleNormals)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_TriangleCounter)
		SHADER_PARAMETER(FMatrix44f, LocalToWorld)
		SHADER_PARAMETER(FVector3f, BoundsMin)
		SHADER_PARAMETER(FVector3f, BoundsMax)
		SHADER_PARAMETER(uint32, TriangleCount)
		SHADER_PARAMETER(uint32, PositionStrideFloat)
		SHADER_PARAMETER(uint32, ReferenceCount)
		SHADER_PARAMETER(uint32, TriangleCapacity)
		SHADER_PARAMETER(uint32, bUseBounds)
		SHADER_PARAMETER(uint32, bUseReferenceFilter)
		SHADER_PARAMETER(float, ReferenceFilterDistanceSq)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

IMPLEMENT_GLOBAL_SHADER(FExtractStaticMeshTrianglesCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "ExtractStaticMeshTrianglesCS", SF_Compute);


class FFilterTriangleSoupByReferenceCS : public FGlobalShader
{
DECLARE_GLOBAL_SHADER(FFilterTriangleSoupByReferenceCS);
SHADER_USE_PARAMETER_STRUCT(FFilterTriangleSoupByReferenceCS, FGlobalShader);

BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, TriangleVertices)
SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, TriangleNormals)
SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SurfaceTriangleCounter)
SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, ReferencePoints)
SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutTriangleVertices)
SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutTriangleNormals)
SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_TriangleCounter)
SHADER_PARAMETER(uint32, TriangleCount)
SHADER_PARAMETER(uint32, ReferenceCount)
SHADER_PARAMETER(uint32, TriangleCapacity)
SHADER_PARAMETER(uint32, bUseReferenceFilter)
SHADER_PARAMETER(float, ReferenceFilterDistanceSq)
END_SHADER_PARAMETER_STRUCT()

static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
{
return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
}

static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
}
};

IMPLEMENT_GLOBAL_SHADER(FFilterTriangleSoupByReferenceCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "FilterTriangleSoupByReferenceCS", SF_Compute);

// -----------------------------------------------------------------------------
// Core System - Surface Voxel Shaders
// -----------------------------------------------------------------------------

class FTriangleSurfaceVoxelsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FTriangleSurfaceVoxelsCS);
	SHADER_USE_PARAMETER_STRUCT(FTriangleSurfaceVoxelsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, TriangleVertices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, TriangleNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SurfaceTriangleCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutVoxelPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutVoxelNormals)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_SurfaceVoxelCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_SurfaceVoxelHashSlots)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_SurfaceVoxelHashIndices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<int>, RW_SurfaceVoxelNormalSums)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_SurfaceVoxelNormalCounts)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutVoxelTargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<int>, RW_SurfaceVoxelTargetOffsetSums)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_SurfaceVoxelTargetWeightSums)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint4>, RW_OutVoxelCells)
		SHADER_PARAMETER(FVector3f, SurfaceVoxelOrigin)
		SHADER_PARAMETER(float, SurfaceVoxelSize)
		SHADER_PARAMETER(float, SurfaceThickness)
		SHADER_PARAMETER(uint32, SurfaceTriangleCount)
		SHADER_PARAMETER(uint32, SurfaceVoxelCapacity)
		SHADER_PARAMETER(uint32, SurfaceVoxelHashSlotCount)
		SHADER_PARAMETER(uint32, MaxSurfaceVoxelCellsPerTriangle)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

IMPLEMENT_GLOBAL_SHADER(FTriangleSurfaceVoxelsCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "TriangleSurfaceVoxelsCS", SF_Compute);

class FFinalizeSurfaceVoxelNormalsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFinalizeSurfaceVoxelNormalsCS);
	SHADER_USE_PARAMETER_STRUCT(FFinalizeSurfaceVoxelNormalsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<int>, SurfaceVoxelNormalSums)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SurfaceVoxelNormalCounts)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<int>, RW_SurfaceVoxelTargetOffsetSums)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_SurfaceVoxelTargetWeightSums)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutVoxelPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutVoxelNormals)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutVoxelTargetPositions)
		SHADER_PARAMETER(uint32, SurfaceVoxelCapacity)
		SHADER_PARAMETER(float, SurfaceVoxelSize)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

IMPLEMENT_GLOBAL_SHADER(FFinalizeSurfaceVoxelNormalsCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "FinalizeSurfaceVoxelNormalsCS", SF_Compute);

// Ported from ResinRattan: 3D spatial blur on surface voxel normals and target positions
class FBlurSurfaceVoxelsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FBlurSurfaceVoxelsCS);
	SHADER_USE_PARAMETER_STRUCT(FBlurSurfaceVoxelsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_SurfaceVoxelCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutVoxelNormals)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutVoxelTargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint4>, RW_OutVoxelCells)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_SurfaceVoxelHashSlots)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_SurfaceVoxelHashIndices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_BlurredVoxelNormals)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_BlurredVoxelTargetPositions)
		SHADER_PARAMETER(uint32, SurfaceVoxelCapacity)
		SHADER_PARAMETER(uint32, SurfaceVoxelHashSlotCount)
		SHADER_PARAMETER(uint32, SurfaceVoxelBlurRadius)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

IMPLEMENT_GLOBAL_SHADER(FBlurSurfaceVoxelsCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "BlurSurfaceVoxelsCS", SF_Compute);

// -----------------------------------------------------------------------------
// Dirty Cache System - Shaders
// -----------------------------------------------------------------------------

class FClearDirtyVoxelCacheCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FClearDirtyVoxelCacheCS);
	SHADER_USE_PARAMETER_STRUCT(FClearDirtyVoxelCacheCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint4>, DirtyVoxelPages)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_VoxelMetaTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_TriangleVertexTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_TriangleNormalTexture)
		SHADER_PARAMETER(uint32, DirtyVoxelCount)
		SHADER_PARAMETER(uint32, CacheGeneration)
		SHADER_PARAMETER(uint32, MaxTrianglesPerVoxel)
		SHADER_PARAMETER(uint32, MetaTextureWidth)
		SHADER_PARAMETER(uint32, MetaTextureHeight)
		SHADER_PARAMETER(uint32, TriangleVertexTextureWidth)
		SHADER_PARAMETER(uint32, TriangleVertexTextureHeight)
		SHADER_PARAMETER(uint32, TriangleNormalTextureWidth)
		SHADER_PARAMETER(uint32, TriangleNormalTextureHeight)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

IMPLEMENT_GLOBAL_SHADER(FClearDirtyVoxelCacheCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "ClearDirtyVoxelCacheCS", SF_Compute);

class FScatterTrianglesToVoxelCacheCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FScatterTrianglesToVoxelCacheCS);
	SHADER_USE_PARAMETER_STRUCT(FScatterTrianglesToVoxelCacheCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, TriangleVertices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, TriangleNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SurfaceTriangleCounter)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint4>, DirtyVoxelPages)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_VoxelMetaTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_TriangleVertexTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_TriangleNormalTexture)
		SHADER_PARAMETER(uint32, SurfaceTriangleCount)
		SHADER_PARAMETER(uint32, DirtyVoxelCount)
		SHADER_PARAMETER(uint32, CacheGeneration)
		SHADER_PARAMETER(uint32, GridSizeX)
		SHADER_PARAMETER(uint32, GridSizeY)
		SHADER_PARAMETER(uint32, GridSizeZ)
		SHADER_PARAMETER(uint32, MaxTrianglesPerVoxel)
		SHADER_PARAMETER(uint32, MetaTextureWidth)
		SHADER_PARAMETER(uint32, MetaTextureHeight)
		SHADER_PARAMETER(uint32, TriangleVertexTextureWidth)
		SHADER_PARAMETER(uint32, TriangleVertexTextureHeight)
		SHADER_PARAMETER(uint32, TriangleNormalTextureWidth)
		SHADER_PARAMETER(uint32, TriangleNormalTextureHeight)
		SHADER_PARAMETER(FVector3f, CacheWorldMin)
		SHADER_PARAMETER(float, CacheVoxelSize)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

IMPLEMENT_GLOBAL_SHADER(FScatterTrianglesToVoxelCacheCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "ScatterTrianglesToVoxelCacheCS", SF_Compute);

// -----------------------------------------------------------------------------
// Core System - Internal Helpers
// -----------------------------------------------------------------------------

namespace
{
struct FCSMeshGeneratorDirtyVoxelPage
{
	uint32 X = 0;
	uint32 Y = 0;
	uint32 Z = 0;
	uint32 PageIndex = 0;
};

static_assert(sizeof(FCSMeshGeneratorDirtyVoxelPage) == sizeof(uint32) * 4, "Dirty voxel page buffer must match shader uint4 layout.");

struct FResolvedStaticMeshTriangleRequest
{
	TRefCountPtr<const FStaticMeshLODResources> LODResource;
	FMatrix44f LocalToWorld = FMatrix44f::Identity;
	FBox3f WorldBounds = FBox3f(EForceInit::ForceInit);
	int32 TriangleCount = 0;
	int32 PositionStrideFloat = 3;
};

bool IsValidCSTriangleIndex(int32 Index, int32 VertexCount);
bool IsFiniteCSVertex(const FVector& Vertex);
bool IsDegenerateCSTriangle(const FVector& A, const FVector& B, const FVector& C);
bool ShouldExcludeStaticMeshTriangleRequest(const FCSStaticMeshTriangleRequest& Request, const AActor* ExcludedActor, FName ExcludedActorTag);

int32 GetTriangleMeshDataTriangleCount(const FCSTriangleMeshData& TriangleData)
{
	const int32 EffectiveVertexCount = TriangleData.VertexCount >= 0
		? FMath::Clamp(TriangleData.VertexCount, 0, TriangleData.Vertices.Num())
		: TriangleData.Vertices.Num();
	const int32 EffectiveIndexCount = TriangleData.IndexCount >= 0
		? FMath::Clamp(TriangleData.IndexCount, 0, TriangleData.Indices.Num())
		: TriangleData.Indices.Num();

	return EffectiveIndexCount >= 3 ? EffectiveIndexCount / 3 : EffectiveVertexCount / 3;
}

FVector ClosestPointOnTriangleForReferenceFilterCPU(const FVector& P, const FVector& A, const FVector& B, const FVector& C)
{
	const FVector AB = B - A;
	const FVector AC = C - A;
	const FVector AP = P - A;
	const double D1 = FVector::DotProduct(AB, AP);
	const double D2 = FVector::DotProduct(AC, AP);
	if (D1 <= 0.0 && D2 <= 0.0)
	{
		return A;
	}

	const FVector BP = P - B;
	const double D3 = FVector::DotProduct(AB, BP);
	const double D4 = FVector::DotProduct(AC, BP);
	if (D3 >= 0.0 && D4 <= D3)
	{
		return B;
	}

	const double VC = D1 * D4 - D3 * D2;
	if (VC <= 0.0 && D1 >= 0.0 && D3 <= 0.0)
	{
		const double V = D1 / (D1 - D3);
		return A + AB * V;
	}

	const FVector CP = P - C;
	const double D5 = FVector::DotProduct(AB, CP);
	const double D6 = FVector::DotProduct(AC, CP);
	if (D6 >= 0.0 && D5 <= D6)
	{
		return C;
	}

	const double VB = D5 * D2 - D1 * D6;
	if (VB <= 0.0 && D2 >= 0.0 && D6 <= 0.0)
	{
		const double W = D2 / (D2 - D6);
		return A + AC * W;
	}

	const double VA = D3 * D6 - D5 * D4;
	if (VA <= 0.0 && (D4 - D3) >= 0.0 && (D5 - D6) >= 0.0)
	{
		const double W = (D4 - D3) / ((D4 - D3) + (D5 - D6));
		return B + (C - B) * W;
	}

	const double Denom = 1.0 / (VA + VB + VC);
	const double V = VB * Denom;
	const double W = VC * Denom;
	return A + AB * V + AC * W;
}

bool PassTriangleReferenceFilterCPU(
	const FVector& A,
	const FVector& B,
	const FVector& C,
	const TArray<FVector>& ReferencePoints,
	float ReferenceFilterDistance)
{
	if (ReferencePoints.IsEmpty() || ReferenceFilterDistance <= 0.0f)
	{
		return true;
	}

	const double ReferenceFilterDistanceSq = FMath::Square(double(ReferenceFilterDistance));
	double BestDistSq = TNumericLimits<double>::Max();
	for (const FVector& ReferencePoint : ReferencePoints)
	{
		const FVector ClosestPoint = ClosestPointOnTriangleForReferenceFilterCPU(ReferencePoint, A, B, C);
		BestDistSq = FMath::Min(BestDistSq, FVector::DistSquared(ClosestPoint, ReferencePoint));
	}
	return BestDistSq <= ReferenceFilterDistanceSq;
}

FVector GetSafeTriangleNormal(const FVector& P0, const FVector& P1, const FVector& P2, const FVector& FallbackNormal = FVector::UpVector)
{
	const FVector Normal = FVector::CrossProduct(P1 - P0, P2 - P0).GetSafeNormal(UE_SMALL_NUMBER, FallbackNormal);
	return Normal.ContainsNaN() ? FallbackNormal : Normal;
}



bool TryAppendTriangleSoup(FCSTriangleMeshData& OutTriangleData,
	const FVector& P0,
	const FVector& P1,
	const FVector& P2,
	const FVector& Normal,
	int32 MaxTriangles)
{
	if (MaxTriangles > 0 && GetTriangleMeshDataTriangleCount(OutTriangleData) >= MaxTriangles)
	{
		return false;
	}

	if (!IsFiniteCSVertex(P0) || !IsFiniteCSVertex(P1) || !IsFiniteCSVertex(P2) || IsDegenerateCSTriangle(P0, P1, P2))
	{
		return true;
	}

	const FVector SafeNormal = Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
	OutTriangleData.Vertices.Add(P0);
	OutTriangleData.Vertices.Add(P1);
	OutTriangleData.Vertices.Add(P2);
	OutTriangleData.VertexNormals.Add(SafeNormal);
	OutTriangleData.VertexNormals.Add(SafeNormal);
	OutTriangleData.VertexNormals.Add(SafeNormal);
	OutTriangleData.VertexCount = OutTriangleData.Vertices.Num();
	OutTriangleData.IndexCount = 0;
	return true;
}

bool TryAppendTriangleSoupOrientedToNormal(FCSTriangleMeshData& OutTriangleData,
	const FVector& P0,
	const FVector& P1,
	const FVector& P2,
	const FVector& DesiredNormal,
	int32 MaxTriangles)
{
	const FVector SafeDesiredNormal = DesiredNormal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
	const FVector WindingNormal = GetSafeTriangleNormal(P0, P1, P2, SafeDesiredNormal);
	if (FVector::DotProduct(WindingNormal, SafeDesiredNormal) < 0.0)
	{
		return TryAppendTriangleSoup(OutTriangleData, P0, P2, P1, SafeDesiredNormal, MaxTriangles);
	}

	return TryAppendTriangleSoup(OutTriangleData, P0, P1, P2, SafeDesiredNormal, MaxTriangles);
}

void AppendTriangleMeshData(FCSTriangleMeshData& OutTriangleData, const FCSTriangleMeshData& SourceTriangleData, int32 MaxTriangles)
{
	const int32 ExistingTriangleCount = GetTriangleMeshDataTriangleCount(OutTriangleData);
	if (MaxTriangles > 0 && ExistingTriangleCount >= MaxTriangles)
	{
		return;
	}

	const int32 EffectiveVertexCount = SourceTriangleData.VertexCount >= 0
		? FMath::Clamp(SourceTriangleData.VertexCount, 0, SourceTriangleData.Vertices.Num())
		: SourceTriangleData.Vertices.Num();
	const int32 EffectiveIndexCount = SourceTriangleData.IndexCount >= 0
		? FMath::Clamp(SourceTriangleData.IndexCount, 0, SourceTriangleData.Indices.Num())
		: SourceTriangleData.Indices.Num();
	const bool bUseIndices = EffectiveIndexCount >= 3;
	const int32 SourceTriangleCount = bUseIndices ? EffectiveIndexCount / 3 : EffectiveVertexCount / 3;
	if (SourceTriangleCount <= 0)
	{
		return;
	}

	const bool bUseVertexNormals = SourceTriangleData.VertexNormals.Num() >= EffectiveVertexCount;
	const int32 RemainingTriangleCapacity = MaxTriangles > 0
		? FMath::Max(0, MaxTriangles - ExistingTriangleCount)
		: SourceTriangleCount;
	const int32 TrianglesToAppend = FMath::Min(SourceTriangleCount, RemainingTriangleCapacity);
	OutTriangleData.Vertices.Reserve(OutTriangleData.Vertices.Num() + TrianglesToAppend * 3);
	OutTriangleData.VertexNormals.Reserve(OutTriangleData.VertexNormals.Num() + TrianglesToAppend * 3);

	for (int32 TriangleIndex = 0; TriangleIndex < TrianglesToAppend; ++TriangleIndex)
	{
		int32 I0 = TriangleIndex * 3 + 0;
		int32 I1 = TriangleIndex * 3 + 1;
		int32 I2 = TriangleIndex * 3 + 2;
		if (bUseIndices)
		{
			I0 = SourceTriangleData.Indices[TriangleIndex * 3 + 0];
			I1 = SourceTriangleData.Indices[TriangleIndex * 3 + 1];
			I2 = SourceTriangleData.Indices[TriangleIndex * 3 + 2];
		}

		if (!IsValidCSTriangleIndex(I0, EffectiveVertexCount)
			|| !IsValidCSTriangleIndex(I1, EffectiveVertexCount)
			|| !IsValidCSTriangleIndex(I2, EffectiveVertexCount))
		{
			continue;
		}

		const FVector& P0 = SourceTriangleData.Vertices[I0];
		const FVector& P1 = SourceTriangleData.Vertices[I1];
		const FVector& P2 = SourceTriangleData.Vertices[I2];
		const FVector Normal = bUseVertexNormals
			? ((SourceTriangleData.VertexNormals[I0] + SourceTriangleData.VertexNormals[I1] + SourceTriangleData.VertexNormals[I2]) / 3.0).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector)
			: GetSafeTriangleNormal(P0, P1, P2);

		if (!TryAppendTriangleSoup(OutTriangleData, P0, P1, P2, Normal, MaxTriangles))
		{
			break;
		}
	}
}

FVector MakeLandscapeNormalFaceUp(const FVector& Normal)
{
	FVector SafeNormal = Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
	if (SafeNormal.Z < 0.0)
	{
		SafeNormal *= -1.0;
	}
	return SafeNormal;
}

void NormalizeTriangleMeshDataWinding(FCSTriangleMeshData& TriangleData)
{
	const int32 EffectiveVertexCount = TriangleData.VertexCount >= 0
		? FMath::Clamp(TriangleData.VertexCount, 0, TriangleData.Vertices.Num())
		: TriangleData.Vertices.Num();
	const int32 EffectiveIndexCount = TriangleData.IndexCount >= 0
		? FMath::Clamp(TriangleData.IndexCount, 0, TriangleData.Indices.Num())
		: TriangleData.Indices.Num();
	const bool bUseIndices = EffectiveIndexCount >= 3;
	const int32 TriangleCount = bUseIndices ? EffectiveIndexCount / 3 : EffectiveVertexCount / 3;
	const bool bHasVertexNormals = TriangleData.VertexNormals.Num() >= EffectiveVertexCount;

	for (int32 TriangleIndex = 0; TriangleIndex < TriangleCount; ++TriangleIndex)
	{
		int32 I0 = TriangleIndex * 3 + 0;
		int32 I1 = TriangleIndex * 3 + 1;
		int32 I2 = TriangleIndex * 3 + 2;
		if (bUseIndices)
		{
			I0 = TriangleData.Indices[TriangleIndex * 3 + 0];
			I1 = TriangleData.Indices[TriangleIndex * 3 + 1];
			I2 = TriangleData.Indices[TriangleIndex * 3 + 2];
		}

		if (!IsValidCSTriangleIndex(I0, EffectiveVertexCount)
			|| !IsValidCSTriangleIndex(I1, EffectiveVertexCount)
			|| !IsValidCSTriangleIndex(I2, EffectiveVertexCount))
		{
			continue;
		}

		const FVector& P0 = TriangleData.Vertices[I0];
		const FVector& P1 = TriangleData.Vertices[I1];
		const FVector& P2 = TriangleData.Vertices[I2];
		const FVector WindingNormal = GetSafeTriangleNormal(P0, P1, P2);
		FVector DesiredNormal = WindingNormal;
		if (bHasVertexNormals)
		{
			DesiredNormal = ((TriangleData.VertexNormals[I0] + TriangleData.VertexNormals[I1] + TriangleData.VertexNormals[I2]) / 3.0)
				.GetSafeNormal(UE_SMALL_NUMBER, WindingNormal);
		}

		if (FVector::DotProduct(WindingNormal, DesiredNormal) < 0.0)
		{
			if (bUseIndices)
			{
				Swap(TriangleData.Indices[TriangleIndex * 3 + 1], TriangleData.Indices[TriangleIndex * 3 + 2]);
			}
			else
			{
				Swap(TriangleData.Vertices[I1], TriangleData.Vertices[I2]);
				if (bHasVertexNormals)
				{
					Swap(TriangleData.VertexNormals[I1], TriangleData.VertexNormals[I2]);
				}
			}
		}
	}
}

bool TriangleIntersectsBox(const FVector& P0, const FVector& P1, const FVector& P2, const FBox& QueryBox)
{
	if (!QueryBox.IsValid)
	{
		return true;
	}

	FBox TriangleBox(ForceInit);
	TriangleBox += P0;
	TriangleBox += P1;
	TriangleBox += P2;
	return TriangleBox.Intersect(QueryBox);
}

uint32 BuildTriangleUploadData(const FCSTriangleMeshData& TriangleData,
	uint32 TriangleCapacity,
	TArray<FVector4f>& OutVertices,
	TArray<FVector4f>& OutNormals)
{
	if (TriangleCapacity == 0)
	{
		return 0;
	}

	const int32 EffectiveVertexCount = TriangleData.VertexCount >= 0
		? FMath::Clamp(TriangleData.VertexCount, 0, TriangleData.Vertices.Num())
		: TriangleData.Vertices.Num();
	const int32 EffectiveIndexCount = TriangleData.IndexCount >= 0
		? FMath::Clamp(TriangleData.IndexCount, 0, TriangleData.Indices.Num())
		: TriangleData.Indices.Num();
	const bool bUseIndices = EffectiveIndexCount >= 3;
	const int32 SourceTriangleCount = bUseIndices ? EffectiveIndexCount / 3 : EffectiveVertexCount / 3;
	const int32 UploadTriangleCount = FMath::Min<int32>(SourceTriangleCount, int32(TriangleCapacity));
	if (UploadTriangleCount <= 0)
	{
		return 0;
	}

	OutVertices.Reset(UploadTriangleCount * 3);
	OutNormals.Reset(UploadTriangleCount * 3);
	OutVertices.Reserve(UploadTriangleCount * 3);
	OutNormals.Reserve(UploadTriangleCount * 3);

	const bool bUseVertexNormals = TriangleData.VertexNormals.Num() >= EffectiveVertexCount;
	for (int32 TriangleIndex = 0; TriangleIndex < UploadTriangleCount; ++TriangleIndex)
	{
		int32 I0 = TriangleIndex * 3 + 0;
		int32 I1 = TriangleIndex * 3 + 1;
		int32 I2 = TriangleIndex * 3 + 2;
		if (bUseIndices)
		{
			I0 = TriangleData.Indices[TriangleIndex * 3 + 0];
			I1 = TriangleData.Indices[TriangleIndex * 3 + 1];
			I2 = TriangleData.Indices[TriangleIndex * 3 + 2];
		}

		if (!IsValidCSTriangleIndex(I0, EffectiveVertexCount)
			|| !IsValidCSTriangleIndex(I1, EffectiveVertexCount)
			|| !IsValidCSTriangleIndex(I2, EffectiveVertexCount))
		{
			continue;
		}

		const FVector& P0 = TriangleData.Vertices[I0];
		const FVector& P1 = TriangleData.Vertices[I1];
		const FVector& P2 = TriangleData.Vertices[I2];
		const FVector Normal = bUseVertexNormals
			? ((TriangleData.VertexNormals[I0] + TriangleData.VertexNormals[I1] + TriangleData.VertexNormals[I2]) / 3.0).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector)
			: GetSafeTriangleNormal(P0, P1, P2);

		OutVertices.Add(FVector4f(FVector3f(P0), 1.0f));
		OutVertices.Add(FVector4f(FVector3f(P1), 1.0f));
		OutVertices.Add(FVector4f(FVector3f(P2), 1.0f));
		OutNormals.Add(FVector4f(FVector3f(Normal), 0.0f));
		OutNormals.Add(FVector4f(FVector3f(Normal), 0.0f));
		OutNormals.Add(FVector4f(FVector3f(Normal), 0.0f));
	}

	return uint32(OutVertices.Num() / 3);
}

bool ResolveTriangleRequest(const FCSStaticMeshTriangleRequest& Request,
	FResolvedStaticMeshTriangleRequest& OutResolved,
	bool bNaniteOnlyFallbackMesh = true)
{
	if (!Request.StaticMesh)
	{
		return false;
	}

	FStaticMeshRenderData* RenderData = Request.StaticMesh->GetRenderData();
	if (!RenderData || RenderData->LODResources.Num() == 0)
	{
		return false;
	}

	if (bNaniteOnlyFallbackMesh
		&& Request.StaticMesh->HasValidNaniteData()
		&& !Request.StaticMesh->HasNaniteFallbackMesh(GMaxRHIShaderPlatform))
	{
		UE_LOG(LogTemp, Verbose, TEXT("[ResolveTriangleRequest] Skip Nanite mesh without fallback mesh: %s"),
			*Request.StaticMesh->GetPathName());
		return false;
	}

	const int32 LODIndex = FMath::Clamp(Request.LODIndex, 0, RenderData->LODResources.Num() - 1);
	const int32 CurrentFirstLOD = RenderData->GetCurrentFirstLODIdx(Request.StaticMesh->GetMinLODIdx());
	if (LODIndex < CurrentFirstLOD || !RenderData->LODResources.IsValidIndex(LODIndex))
	{
		return false;
	}

	const FStaticMeshLODResources* LODResource = &RenderData->LODResources[LODIndex];
	if (!LODResource || LODResource->GetNumTriangles() <= 0 || LODResource->GetNumVertices() <= 0 || LODResource->BuffersSize <= 0)
	{
		return false;
	}

	if (!LODResource->VertexBuffers.PositionVertexBuffer.GetSRV() || !LODResource->IndexBuffer.GetRHI().IsValid())
	{
		return false;
	}

	OutResolved.LODResource = LODResource;
	OutResolved.LocalToWorld = FMatrix44f(Request.LocalToWorld.ToMatrixWithScale());
	OutResolved.WorldBounds = Request.WorldBounds.IsValid
		? FBox3f(FVector3f(Request.WorldBounds.Min), FVector3f(Request.WorldBounds.Max))
		: FBox3f(EForceInit::ForceInit);
	OutResolved.TriangleCount = LODResource->GetNumTriangles();
	OutResolved.PositionStrideFloat = FMath::Max(3, int32(LODResource->VertexBuffers.PositionVertexBuffer.GetStride() / sizeof(float)));
	return true;
}

FShaderResourceViewRHIRef CreateTriangleIndexBufferSRV(FRHICommandListImmediate& RHICmdList, const FStaticMeshLODResources* LODResource)
{
	if (!LODResource || !LODResource->IndexBuffer.GetRHI().IsValid())
	{
		return nullptr;
	}

	return RHICmdList.CreateShaderResourceView(
		LODResource->IndexBuffer.GetRHI(),
		FRHIViewDesc::CreateBufferSRV()
			.SetType(FRHIViewDesc::EBufferType::Typed)
			.SetFormat(LODResource->IndexBuffer.Is32Bit() ? PF_R32_UINT : PF_R16_UINT));
}

UDynamicMesh* CreateEmptyDynamicMesh()
{
	UDynamicMesh* OutMesh = NewObject<UDynamicMesh>();
	if (OutMesh)
	{
		OutMesh->Reset();
	}
	return OutMesh;
}

uint32 GetSurfaceVoxelHashSlotCount(int32 VoxelCapacity, int32 RequestedHashSlotCount)
{
	const uint32 SafeCapacity = uint32(FMath::Max(1, VoxelCapacity));
	const uint64 TargetSlots64 = RequestedHashSlotCount > 0
		? uint64(RequestedHashSlotCount)
		: FMath::Max<uint64>(1024ull, uint64(SafeCapacity) * 2ull);
	const uint32 TargetSlots = uint32(FMath::Min<uint64>(FMath::Max<uint64>(TargetSlots64, SafeCapacity), uint64(1u << 30)));

	uint32 SlotCount = 1u;
	while (SlotCount < TargetSlots && SlotCount < (1u << 30))
	{
		SlotCount <<= 1u;
	}
	return SlotCount;
}

bool IsValidCSTriangleIndex(int32 Index, int32 VertexCount)
{
	return Index >= 0 && Index < VertexCount;
}

bool IsFiniteCSVertex(const FVector& Vertex)
{
	return !Vertex.ContainsNaN()
		&& FMath::IsFinite(Vertex.X)
		&& FMath::IsFinite(Vertex.Y)
		&& FMath::IsFinite(Vertex.Z);
}

bool IsFiniteCSVector4(const FVector4f& Vector)
{
	return FMath::IsFinite(Vector.X)
		&& FMath::IsFinite(Vector.Y)
		&& FMath::IsFinite(Vector.Z)
		&& FMath::IsFinite(Vector.W);
}

bool IsDegenerateCSTriangle(const FVector& A, const FVector& B, const FVector& C)
{
	const FVector AB = B - A;
	const FVector AC = C - A;
	const double AreaSq4 = FVector::CrossProduct(AB, AC).SizeSquared();
	return AreaSq4 <= 1.0e-8;
}

bool ShouldExcludeStaticMeshTriangleRequest(const FCSStaticMeshTriangleRequest& Request, const AActor* ExcludedActor, FName ExcludedActorTag)
{
	const AActor* SourceActor = Request.SourceActor;
	if (!SourceActor)
	{
		return false;
	}

	if (ExcludedActor && SourceActor == ExcludedActor)
	{
		return true;
	}

	return !ExcludedActorTag.IsNone() && SourceActor->ActorHasTag(ExcludedActorTag);
}

uint64 ResolveStaticMeshTriangleRequests(
	const TArray<FCSStaticMeshTriangleRequest>& Requests,
	const AActor* ExcludedActor,
	FName ExcludedActorTag,
	bool bNaniteOnlyFallbackMesh,
	TArray<FResolvedStaticMeshTriangleRequest>& OutResolvedRequests)
{
	OutResolvedRequests.Reset();
	OutResolvedRequests.Reserve(Requests.Num());

	uint64 TotalTriangleCount = 0;
	for (const FCSStaticMeshTriangleRequest& Request : Requests)
	{
		if (ShouldExcludeStaticMeshTriangleRequest(Request, ExcludedActor, ExcludedActorTag))
		{
			continue;
		}

		FResolvedStaticMeshTriangleRequest Resolved;
		if (!ResolveTriangleRequest(Request, Resolved, bNaniteOnlyFallbackMesh))
		{
			continue;
		}

		TotalTriangleCount = FMath::Min<uint64>(
			TotalTriangleCount + uint64(FMath::Max(0, Resolved.TriangleCount)),
			uint64(TNumericLimits<uint32>::Max()));
		OutResolvedRequests.Add(MoveTemp(Resolved));
	}

	return TotalTriangleCount;
}

UDynamicMesh* BuildDynamicMeshFromCSTriangleData(const TArray<FVector>& Vertices,
	const TArray<int32>& Indices,
	const TArray<FVector>& VertexNormals,
	int32 VertexCount,
	int32 IndexCount,
	bool bReverseOrientation,
	bool bSkipDegenerateTriangles,
	bool bRecomputeNormals)
{
	UDynamicMesh* OutMesh = CreateEmptyDynamicMesh();
	if (!OutMesh)
	{
		return nullptr;
	}

	const int32 EffectiveVertexCount = VertexCount >= 0
		? FMath::Clamp(VertexCount, 0, Vertices.Num())
		: Vertices.Num();
	const int32 EffectiveIndexCount = IndexCount >= 0
		? FMath::Clamp(IndexCount, 0, Indices.Num())
		: Indices.Num();

	if (EffectiveVertexCount < 3)
	{
		return OutMesh;
	}

	const bool bUseTriangleSoup = EffectiveIndexCount == 0;
	const int32 TriangleCount = bUseTriangleSoup ? EffectiveVertexCount / 3 : EffectiveIndexCount / 3;
	if (TriangleCount <= 0)
	{
		return OutMesh;
	}

	const bool bUseInputVertexNormals = !bRecomputeNormals && VertexNormals.Num() >= EffectiveVertexCount;

	UE::Geometry::FDynamicMesh3 Mesh;
	TArray<int32> VertexIDMap;
	VertexIDMap.Reserve(EffectiveVertexCount);
	for (int32 VertexIndex = 0; VertexIndex < EffectiveVertexCount; ++VertexIndex)
	{
		const FVector& Vertex = Vertices[VertexIndex];
		if (IsFiniteCSVertex(Vertex))
		{
			VertexIDMap.Add(Mesh.AppendVertex(FVector3d(Vertex)));
		}
		else
		{
			VertexIDMap.Add(INDEX_NONE);
		}
	}

	if (bUseInputVertexNormals)
	{
		Mesh.EnableVertexNormals(FVector3f::UpVector);
		for (int32 VertexIndex = 0; VertexIndex < EffectiveVertexCount; ++VertexIndex)
		{
			const int32 MeshVertexID = VertexIDMap[VertexIndex];
			if (MeshVertexID == INDEX_NONE || VertexNormals[VertexIndex].ContainsNaN())
			{
				continue;
			}

			const FVector SafeNormal = VertexNormals[VertexIndex].GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
			Mesh.SetVertexNormal(MeshVertexID, FVector3f(SafeNormal));
		}
	}

	int32 AddedTriangles = 0;
	for (int32 TriangleIndex = 0; TriangleIndex < TriangleCount; ++TriangleIndex)
	{
		int32 A = INDEX_NONE;
		int32 B = INDEX_NONE;
		int32 C = INDEX_NONE;

		if (bUseTriangleSoup)
		{
			A = TriangleIndex * 3 + 0;
			B = TriangleIndex * 3 + 1;
			C = TriangleIndex * 3 + 2;
		}
		else
		{
			A = Indices[TriangleIndex * 3 + 0];
			B = Indices[TriangleIndex * 3 + 1];
			C = Indices[TriangleIndex * 3 + 2];
		}

		if (!IsValidCSTriangleIndex(A, EffectiveVertexCount)
			|| !IsValidCSTriangleIndex(B, EffectiveVertexCount)
			|| !IsValidCSTriangleIndex(C, EffectiveVertexCount)
			|| VertexIDMap[A] == INDEX_NONE
			|| VertexIDMap[B] == INDEX_NONE
			|| VertexIDMap[C] == INDEX_NONE
			|| A == B || B == C || A == C)
		{
			continue;
		}

		if (bSkipDegenerateTriangles && IsDegenerateCSTriangle(Vertices[A], Vertices[B], Vertices[C]))
		{
			continue;
		}

		if (bReverseOrientation)
		{
			// DynamicMeshComponent front-face rendering and the collected source-normal triangle
			// buffer currently use opposite winding conventions. Keep this explicit conversion
			// at the DynamicMesh boundary instead of changing the source triangle data.
			Swap(B, C);
		}

		const int32 NewTriangleID = Mesh.AppendTriangle(UE::Geometry::FIndex3i(VertexIDMap[A], VertexIDMap[B], VertexIDMap[C]), 0);
		if (NewTriangleID >= 0)
		{
			++AddedTriangles;
		}
	}

	if (AddedTriangles == 0)
	{
		return OutMesh;
	}

	OutMesh->SetMesh(MoveTemp(Mesh));

	if (bRecomputeNormals)
	{
		FGeometryScriptCalculateNormalsOptions CalculateOptions;
		UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(OutMesh, CalculateOptions);
	}

	return OutMesh;
}

void BuildBoxSceneLandscapeTrianglesInternal(UWorld* World,
	const FBox& QueryBox,
	const TArray<FVector>& ReferencePoints,
	float ReferenceFilterDistance,
	int32 MaxTriangles,
	FCSTriangleMeshData& OutTriangleData)
{
	OutTriangleData = FCSTriangleMeshData();

#if WITH_EDITOR
	if (!World || !QueryBox.IsValid || MaxTriangles == 0)
	{
		return;
	}

	for (TObjectIterator<ULandscapeComponent> It; It; ++It)
	{
		ULandscapeComponent* LandscapeComponent = *It;
		if (!IsValid(LandscapeComponent)
			|| LandscapeComponent->IsTemplate()
			|| !LandscapeComponent->IsRegistered()
			|| LandscapeComponent->GetWorld() != World)
		{
			continue;
		}

		if (!LandscapeComponent->Bounds.GetBox().Intersect(QueryBox))
		{
			continue;
		}

		const int32 ComponentSizeQuads = LandscapeComponent->ComponentSizeQuads;
		if (ComponentSizeQuads <= 0)
		{
			continue;
		}

		FLandscapeComponentDataInterface LandscapeData(LandscapeComponent, 0, false);
		if (!LandscapeData.GetRawHeightData())
		{
			continue;
		}

		for (int32 Y = 0; Y < ComponentSizeQuads; ++Y)
		{
			for (int32 X = 0; X < ComponentSizeQuads; ++X)
			{
				FVector P00;
				FVector P10;
				FVector P01;
				FVector P11;
				FVector TangentX;
				FVector TangentY;
				FVector N00;
				FVector N10;
				FVector N01;
				FVector N11;
				LandscapeData.GetWorldPositionTangents(X, Y, P00, TangentX, TangentY, N00);
				LandscapeData.GetWorldPositionTangents(X + 1, Y, P10, TangentX, TangentY, N10);
				LandscapeData.GetWorldPositionTangents(X, Y + 1, P01, TangentX, TangentY, N01);
				LandscapeData.GetWorldPositionTangents(X + 1, Y + 1, P11, TangentX, TangentY, N11);

				if (TriangleIntersectsBox(P00, P10, P11, QueryBox) && PassTriangleReferenceFilterCPU(P00, P10, P11, ReferencePoints, ReferenceFilterDistance))
				{
					const FVector Tri0Normal = MakeLandscapeNormalFaceUp(N00 + N10 + N11);
					if (!TryAppendTriangleSoupOrientedToNormal(OutTriangleData, P00, P10, P11, Tri0Normal, MaxTriangles))
					{
						return;
					}
				}

				if (TriangleIntersectsBox(P00, P11, P01, QueryBox) && PassTriangleReferenceFilterCPU(P00, P11, P01, ReferencePoints, ReferenceFilterDistance))
				{
					const FVector Tri1Normal = MakeLandscapeNormalFaceUp(N00 + N11 + N01);
					if (!TryAppendTriangleSoupOrientedToNormal(OutTriangleData, P00, P11, P01, Tri1Normal, MaxTriangles))
					{
						return;
					}
				}
			}
		}
	}
#endif
}

void BuildBoxSceneTriangleRequestsInternal(UWorld* World,
	const FBox& QueryBox,
	int32 LODIndex,
	TArray<FCSStaticMeshTriangleRequest>& OutRequests)
{
	OutRequests.Reset();
	if (!World || !QueryBox.IsValid)
	{
		return;
	}

	for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
	{
		UStaticMeshComponent* StaticMeshComponent = *It;
		if (!IsValid(StaticMeshComponent)
			|| StaticMeshComponent->IsTemplate()
			|| !StaticMeshComponent->IsRegistered()
			|| StaticMeshComponent->GetWorld() != World)
		{
			continue;
		}

		AActor* SourceActor = StaticMeshComponent->GetOwner();
		UStaticMesh* StaticMesh = StaticMeshComponent->GetStaticMesh();
		if (!StaticMesh)
		{
			continue;
		}

		if (UInstancedStaticMeshComponent* InstancedComponent = Cast<UInstancedStaticMeshComponent>(StaticMeshComponent))
		{
			const FBox LocalMeshBounds = StaticMesh->GetBoundingBox();
			for (int32 InstanceIndex = 0; InstanceIndex < InstancedComponent->GetInstanceCount(); ++InstanceIndex)
			{
				FTransform InstanceTransform = FTransform::Identity;
				InstancedComponent->GetInstanceTransform(InstanceIndex, InstanceTransform, true);

				const FBox InstanceWorldBounds = LocalMeshBounds.TransformBy(InstanceTransform);
				if (!InstanceWorldBounds.Intersect(QueryBox))
				{
					continue;
				}

				FCSStaticMeshTriangleRequest& Request = OutRequests.AddDefaulted_GetRef();
				Request.StaticMesh = StaticMesh;
				Request.LODIndex = LODIndex;
				Request.LocalToWorld = InstanceTransform;
				Request.WorldBounds = QueryBox;
				Request.SourceActor = SourceActor;
			}
			continue;
		}

		const FBox ComponentWorldBounds = StaticMeshComponent->Bounds.GetBox();
		if (!ComponentWorldBounds.Intersect(QueryBox))
		{
			continue;
		}

		FCSStaticMeshTriangleRequest& Request = OutRequests.AddDefaulted_GetRef();
		Request.StaticMesh = StaticMesh;
		Request.LODIndex = LODIndex;
		Request.LocalToWorld = StaticMeshComponent->GetComponentTransform();
		Request.WorldBounds = QueryBox;
		Request.SourceActor = SourceActor;
	}
}
}

// -----------------------------------------------------------------------------
// Core System - Scene Requests
// -----------------------------------------------------------------------------

void AComputeShaderMeshGenerator::BuildBoxSceneTriangleRequests(UWorld* World,
	const FBox& QueryBox,
	TArray<FCSStaticMeshTriangleRequest>& OutRequests)
{
	BuildBoxSceneTriangleRequestsInternal(World, QueryBox, VoxelGridSettings.LODIndex, OutRequests);
}

void AComputeShaderMeshGenerator::BuildActorSceneTriangleRequests(TArray<AActor*> InActors,
	TArray<FCSStaticMeshTriangleRequest>& OutRequests,
	float BoundsExpand)
{
	OutRequests.Reset();

	FBox ReferenceBounds(ForceInit);
	for (const FVector& ReferencePoint : ReferencePoints)
	{
		ReferenceBounds += ReferencePoint;
	}
	if (ReferenceBounds.IsValid && BoundsExpand > 0.0f)
	{
		ReferenceBounds = ReferenceBounds.ExpandBy(BoundsExpand);
	}

	for (AActor* Actor : InActors)
	{
		if (!Actor)
		{
			continue;
		}

		if (ExcludedActorTag != NAME_None && Actor->ActorHasTag(ExcludedActorTag))
		{
			// 这里提前跳过只是减少 request 数量；真正的统一保护仍在 AddStaticMeshTrianglesToRDGInternal。
			continue;
		}

		TArray<UStaticMeshComponent*> StaticMeshComponents;
		Actor->GetComponents<UStaticMeshComponent>(StaticMeshComponents);
		for (UStaticMeshComponent* StaticMeshComponent : StaticMeshComponents)
		{
			if (!StaticMeshComponent || !StaticMeshComponent->GetStaticMesh())
			{
				continue;
			}

			if (UInstancedStaticMeshComponent* InstancedComponent = Cast<UInstancedStaticMeshComponent>(StaticMeshComponent))
			{
				UStaticMesh* InstanceMesh = InstancedComponent->GetStaticMesh();
				if (!InstanceMesh)
				{
					continue;
				}

				const FBox LocalMeshBounds = InstanceMesh->GetBoundingBox();
				for (int32 InstanceIndex = 0; InstanceIndex < InstancedComponent->GetInstanceCount(); ++InstanceIndex)
				{
					FTransform InstanceTransform = FTransform::Identity;
					InstancedComponent->GetInstanceTransform(InstanceIndex, InstanceTransform, true);
					const FBox InstanceWorldBounds = LocalMeshBounds.TransformBy(InstanceTransform);
					if (ReferenceBounds.IsValid && !InstanceWorldBounds.Intersect(ReferenceBounds))
					{
						continue;
					}

					FCSStaticMeshTriangleRequest& Request = OutRequests.AddDefaulted_GetRef();
					Request.StaticMesh = InstanceMesh;
					Request.LODIndex = VoxelGridSettings.LODIndex;
					Request.LocalToWorld = InstanceTransform;
					Request.WorldBounds = ReferenceBounds.IsValid ? ReferenceBounds : FBox(ForceInit);
					Request.SourceActor = Actor;
				}
				continue;
			}

			const FBox ComponentWorldBounds = StaticMeshComponent->Bounds.GetBox();
			if (ReferenceBounds.IsValid && !ComponentWorldBounds.Intersect(ReferenceBounds))
			{
				continue;
			}

			FCSStaticMeshTriangleRequest& Request = OutRequests.AddDefaulted_GetRef();
			Request.StaticMesh = StaticMeshComponent->GetStaticMesh();
			Request.LODIndex = VoxelGridSettings.LODIndex;
			Request.LocalToWorld = StaticMeshComponent->GetComponentTransform();
			Request.WorldBounds = ReferenceBounds.IsValid ? ReferenceBounds : FBox(ForceInit);
			Request.SourceActor = Actor;
		}
	}
}

// -----------------------------------------------------------------------------
// Core System - RDG Extraction
// -----------------------------------------------------------------------------

namespace
{
FCSStaticMeshTriangleRDGOutput AddResolvedStaticMeshTrianglesToRDGInternal(
	FRDGBuilder& GraphBuilder,
	FRHICommandListImmediate& RHICmdList,
	const TArray<FResolvedStaticMeshTriangleRequest>& ResolvedRequests,
	uint64 TotalTriangleCount,
	const TArray<FVector>& ReferencePoints,
	float ReferenceFilterDistance,
	int32 MaxTriangles,
	const FCSTriangleMeshData* InitialTriangleData,
	const TCHAR* DebugName)
{
	FCSStaticMeshTriangleRDGOutput Output;

	const uint64 InitialTriangleCount = InitialTriangleData ? uint64(FMath::Max(0, GetTriangleMeshDataTriangleCount(*InitialTriangleData))) : 0ull;
	const uint64 CombinedTriangleCount = FMath::Min<uint64>(TotalTriangleCount + InitialTriangleCount, uint64(TNumericLimits<int32>::Max()));
	if (CombinedTriangleCount == 0)
	{
		return Output;
	}

	const int32 CombinedTriangleCountInt = int32(CombinedTriangleCount);
	const uint32 TriangleCapacity = uint32(FMath::Clamp(MaxTriangles > 0 ? MaxTriangles : CombinedTriangleCountInt, 1, CombinedTriangleCountInt));
	const uint32 VertexCapacity = TriangleCapacity * 3u;
	Output.MaxTriangles = TriangleCapacity;
	Output.MaxVertices = VertexCapacity;

	Output.TriangleVertices = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), VertexCapacity),
		TEXT("CS.StaticMeshTriangles.Vertices"));
	Output.TriangleVerticesUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Output.TriangleVertices, PF_A32B32G32R32F));
	Output.TriangleVerticesSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Output.TriangleVertices, PF_A32B32G32R32F));
	AddClearUAVPass(GraphBuilder, Output.TriangleVerticesUAV, 0.0f);

	Output.TriangleNormals = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), VertexCapacity),
		TEXT("CS.StaticMeshTriangles.Normals"));
	Output.TriangleNormalsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Output.TriangleNormals, PF_A32B32G32R32F));
	Output.TriangleNormalsSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Output.TriangleNormals, PF_A32B32G32R32F));
	AddClearUAVPass(GraphBuilder, Output.TriangleNormalsUAV, 0.0f);

	Output.TriangleCounter = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1),
		TEXT("CS.StaticMeshTriangles.Counter"));
	Output.TriangleCounterUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Output.TriangleCounter, PF_R32_UINT));
	Output.TriangleCounterSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Output.TriangleCounter, PF_R32_UINT));
	AddClearUAVPass(GraphBuilder, Output.TriangleCounterUAV, 0u);

if (InitialTriangleData && InitialTriangleCount > 0)
{
TArray<FVector4f> InitialVertices;
TArray<FVector4f> InitialNormals;
const uint32 UploadTriangleCount = BuildTriangleUploadData(*InitialTriangleData, TriangleCapacity, InitialVertices, InitialNormals);
if (UploadTriangleCount > 0)
{
const uint32 UploadVertexCount = UploadTriangleCount * 3u;
FRDGBufferRef InitialVertexBuffer = GraphBuilder.CreateBuffer(
FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), UploadVertexCount),
TEXT("CS.StaticMeshTriangles.InitialVertices"));
FVector4f* InitialVertexUploadData = GraphBuilder.AllocPODArray<FVector4f>(UploadVertexCount);
FMemory::Memcpy(InitialVertexUploadData, InitialVertices.GetData(), UploadVertexCount * sizeof(FVector4f));
GraphBuilder.QueueBufferUpload(InitialVertexBuffer, InitialVertexUploadData, UploadVertexCount * sizeof(FVector4f));

FRDGBufferRef InitialNormalBuffer = GraphBuilder.CreateBuffer(
FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), UploadVertexCount),
TEXT("CS.StaticMeshTriangles.InitialNormals"));
FVector4f* InitialNormalUploadData = GraphBuilder.AllocPODArray<FVector4f>(UploadVertexCount);
FMemory::Memcpy(InitialNormalUploadData, InitialNormals.GetData(), UploadVertexCount * sizeof(FVector4f));
GraphBuilder.QueueBufferUpload(InitialNormalBuffer, InitialNormalUploadData, UploadVertexCount * sizeof(FVector4f));

const bool bFilterInitialTriangleSoup = ReferencePoints.Num() > 0 && ReferenceFilterDistance > 0.0f;
if (bFilterInitialTriangleSoup)
{
FRDGBufferRef InitialCounterBuffer = GraphBuilder.CreateBuffer(
FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1),
TEXT("CS.StaticMeshTriangles.InitialCounter"));
uint32* InitialCounterUploadData = GraphBuilder.AllocPODArray<uint32>(1);
*InitialCounterUploadData = UploadTriangleCount;
GraphBuilder.QueueBufferUpload(InitialCounterBuffer, InitialCounterUploadData, sizeof(uint32));

TArray<FVector4f> InitialReferencePointData;
InitialReferencePointData.Reserve(ReferencePoints.Num());
for (const FVector& ReferencePoint : ReferencePoints)
{
InitialReferencePointData.Add(FVector4f(FVector3f(ReferencePoint), 1.0f));
}
if (InitialReferencePointData.IsEmpty())
{
InitialReferencePointData.Add(FVector4f(0, 0, 0, 0));
}

FRDGBufferRef InitialReferencePointBuffer = GraphBuilder.CreateBuffer(
FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), InitialReferencePointData.Num()),
TEXT("CS.StaticMeshTriangles.InitialReferencePoints"));
FVector4f* InitialReferencePointUploadData = GraphBuilder.AllocPODArray<FVector4f>(InitialReferencePointData.Num());
FMemory::Memcpy(InitialReferencePointUploadData, InitialReferencePointData.GetData(), InitialReferencePointData.Num() * sizeof(FVector4f));
GraphBuilder.QueueBufferUpload(InitialReferencePointBuffer, InitialReferencePointUploadData, InitialReferencePointData.Num() * sizeof(FVector4f));

TShaderMapRef<FFilterTriangleSoupByReferenceCS> FilterInitialShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
FFilterTriangleSoupByReferenceCS::FParameters* FilterInitialParameters = GraphBuilder.AllocParameters<FFilterTriangleSoupByReferenceCS::FParameters>();
FilterInitialParameters->TriangleVertices = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(InitialVertexBuffer, PF_A32B32G32R32F));
FilterInitialParameters->TriangleNormals = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(InitialNormalBuffer, PF_A32B32G32R32F));
FilterInitialParameters->SurfaceTriangleCounter = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(InitialCounterBuffer, PF_R32_UINT));
FilterInitialParameters->ReferencePoints = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(InitialReferencePointBuffer, PF_A32B32G32R32F));
FilterInitialParameters->RW_OutTriangleVertices = Output.TriangleVerticesUAV;
FilterInitialParameters->RW_OutTriangleNormals = Output.TriangleNormalsUAV;
FilterInitialParameters->RW_TriangleCounter = Output.TriangleCounterUAV;
FilterInitialParameters->TriangleCount = UploadTriangleCount;
FilterInitialParameters->ReferenceCount = uint32(ReferencePoints.Num());
FilterInitialParameters->TriangleCapacity = TriangleCapacity;
FilterInitialParameters->bUseReferenceFilter = 1u;
FilterInitialParameters->ReferenceFilterDistanceSq = ReferenceFilterDistance * ReferenceFilterDistance;

GraphBuilder.AddPass(
RDG_EVENT_NAME("%s.FilterInitialTriangleSoup", DebugName),
FilterInitialParameters,
ERDGPassFlags::Compute,
[FilterInitialParameters, FilterInitialShader, UploadTriangleCount](FRHIComputeCommandList& InRHICmdList)
{
FComputeShaderUtils::Dispatch(
InRHICmdList,
FilterInitialShader,
*FilterInitialParameters,
FComputeShaderUtils::GetGroupCount(FIntVector(int32(UploadTriangleCount), 1, 1), 64));
});
}
else
{
AddCopyBufferPass(GraphBuilder, Output.TriangleVertices, 0, InitialVertexBuffer, 0, UploadVertexCount * sizeof(FVector4f));
AddCopyBufferPass(GraphBuilder, Output.TriangleNormals, 0, InitialNormalBuffer, 0, UploadVertexCount * sizeof(FVector4f));

const uint32 CounterValue = UploadTriangleCount;
FRDGBufferRef InitialCounterBuffer = GraphBuilder.CreateBuffer(
FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1),
TEXT("CS.StaticMeshTriangles.InitialCounter"));
uint32* InitialCounterUploadData = GraphBuilder.AllocPODArray<uint32>(1);
*InitialCounterUploadData = CounterValue;
GraphBuilder.QueueBufferUpload(InitialCounterBuffer, InitialCounterUploadData, sizeof(uint32));
AddCopyBufferPass(GraphBuilder, Output.TriangleCounter, 0, InitialCounterBuffer, 0, sizeof(uint32));
}
}
}


	TArray<FVector4f> ReferencePointData;
	ReferencePointData.Reserve(ReferencePoints.Num());
	for (const FVector& ReferencePoint : ReferencePoints)
	{
		ReferencePointData.Add(FVector4f(FVector3f(ReferencePoint), 1.0f));
	}
	if (ReferencePointData.IsEmpty())
	{
		ReferencePointData.Add(FVector4f(0, 0, 0, 0));
	}

	FRDGBufferRef ReferencePointBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), ReferencePointData.Num()),
		TEXT("CS.StaticMeshTriangles.ReferencePoints"));
	FVector4f* ReferencePointUploadData = GraphBuilder.AllocPODArray<FVector4f>(ReferencePointData.Num());
	FMemory::Memcpy(ReferencePointUploadData, ReferencePointData.GetData(), ReferencePointData.Num() * sizeof(FVector4f));
	GraphBuilder.QueueBufferUpload(ReferencePointBuffer, ReferencePointUploadData, ReferencePointData.Num() * sizeof(FVector4f));
	FRDGBufferSRVRef ReferencePointSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ReferencePointBuffer, PF_A32B32G32R32F));

	TShaderMapRef<FExtractStaticMeshTrianglesCS> TriangleShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	Output.ReferencedIndexBufferSRVs.Reserve(ResolvedRequests.Num());
	for (const FResolvedStaticMeshTriangleRequest& Request : ResolvedRequests)
	{
		FShaderResourceViewRHIRef IndexBufferSRV = CreateTriangleIndexBufferSRV(RHICmdList, Request.LODResource.GetReference());
		FRHIShaderResourceView* PositionBufferSRV = Request.LODResource->VertexBuffers.PositionVertexBuffer.GetSRV();
		if (!IndexBufferSRV.IsValid() || !PositionBufferSRV)
		{
			continue;
		}
		Output.ReferencedIndexBufferSRVs.Add(IndexBufferSRV);

		FExtractStaticMeshTrianglesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FExtractStaticMeshTrianglesCS::FParameters>();
		PassParameters->IndexBuffer = Output.ReferencedIndexBufferSRVs.Last().GetReference();
		PassParameters->PositionBuffer = PositionBufferSRV;
		PassParameters->ReferencePoints = ReferencePointSRV;
		PassParameters->RW_OutTriangleVertices = Output.TriangleVerticesUAV;
		PassParameters->RW_OutTriangleNormals = Output.TriangleNormalsUAV;
		PassParameters->RW_TriangleCounter = Output.TriangleCounterUAV;
		PassParameters->LocalToWorld = Request.LocalToWorld;
		PassParameters->BoundsMin = Request.WorldBounds.IsValid ? Request.WorldBounds.Min : FVector3f(-TNumericLimits<float>::Max());
		PassParameters->BoundsMax = Request.WorldBounds.IsValid ? Request.WorldBounds.Max : FVector3f(TNumericLimits<float>::Max());
		PassParameters->TriangleCount = uint32(Request.TriangleCount);
		PassParameters->PositionStrideFloat = uint32(Request.PositionStrideFloat);
		PassParameters->ReferenceCount = uint32(ReferencePoints.Num());
		PassParameters->TriangleCapacity = TriangleCapacity;
		PassParameters->bUseBounds = Request.WorldBounds.IsValid ? 1u : 0u;
		PassParameters->bUseReferenceFilter = ReferencePoints.Num() > 0 ? 1u : 0u;
		PassParameters->ReferenceFilterDistanceSq = ReferenceFilterDistance > 0.0f
			? ReferenceFilterDistance * ReferenceFilterDistance
			: TNumericLimits<float>::Max();

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("%s.Extract", DebugName),
			PassParameters,
			ERDGPassFlags::Compute,
			[PassParameters, TriangleShader, TriangleCount = Request.TriangleCount, IndexBufferSRV](FRHIComputeCommandList& InRHICmdList)
			{
				(void)IndexBufferSRV;
				FComputeShaderUtils::Dispatch(InRHICmdList, TriangleShader, *PassParameters, FComputeShaderUtils::GetGroupCount(FIntVector(TriangleCount, 1, 1), 64));
			});
	}

	return Output;
}

FCSStaticMeshTriangleRDGOutput AddStaticMeshTrianglesToRDGInternal(
	FRDGBuilder& GraphBuilder,
	FRHICommandListImmediate& RHICmdList,
	const TArray<FCSStaticMeshTriangleRequest>& Requests,
	const TArray<FVector>& ReferencePoints,
	float ReferenceFilterDistance,
	int32 MaxTriangles,
	const FCSTriangleMeshData* InitialTriangleData,
	const TCHAR* DebugName,
	const AActor* ExcludedActor,
	FName ExcludedActorTag,
	bool bNaniteOnlyFallbackMesh = true)
{
	TArray<FResolvedStaticMeshTriangleRequest> ResolvedRequests;
	const uint64 TotalTriangleCount = ResolveStaticMeshTriangleRequests(
		Requests,
		ExcludedActor,
		ExcludedActorTag,
		bNaniteOnlyFallbackMesh,
		ResolvedRequests);

	return AddResolvedStaticMeshTrianglesToRDGInternal(
		GraphBuilder,
		RHICmdList,
		ResolvedRequests,
		TotalTriangleCount,
		ReferencePoints,
		ReferenceFilterDistance,
		MaxTriangles,
		InitialTriangleData,
		DebugName);
}

bool ReadbackResolvedStaticMeshTriangleRequestSync(
	const FResolvedStaticMeshTriangleRequest& Request,
	const TArray<FVector>& ReferencePoints,
	float ReferenceFilterDistance,
	int32 MaxTriangles,
	FCSTriangleMeshData& OutTriangleData)
{
	OutTriangleData = FCSTriangleMeshData();

	const int32 RequestTriangleCount = FMath::Max(0, Request.TriangleCount);
	if (RequestTriangleCount <= 0 || MaxTriangles <= 0)
	{
		return true;
	}

	const int32 BatchTriangleCapacity = FMath::Min(RequestTriangleCount, MaxTriangles);
	const uint64 BatchVertexCapacity64 = uint64(BatchTriangleCapacity) * 3ull;
	const uint64 BatchVertexReadbackBytes64 = BatchVertexCapacity64 * sizeof(FVector4f);
	if (BatchVertexCapacity64 > uint64(TNumericLimits<int32>::Max()) || BatchVertexReadbackBytes64 > uint64(TNumericLimits<uint32>::Max()))
	{
		UE_LOG(LogTemp, Warning, TEXT("[GetBoxSceneTrianglesFromGPU] Batch readback request too large. Triangles=%d"), BatchTriangleCapacity);
		return false;
	}

	const int32 BatchVertexCapacity = int32(BatchVertexCapacity64);
	const uint32 BatchVertexReadbackBytes = uint32(BatchVertexReadbackBytes64);
	const uint32 CounterReadbackBytes = sizeof(uint32);

	FRHIGPUBufferReadback* VertexReadback = new FRHIGPUBufferReadback(TEXT("BoxSceneTriangles_BatchVertexReadback"));
	FRHIGPUBufferReadback* NormalReadback = new FRHIGPUBufferReadback(TEXT("BoxSceneTriangles_BatchNormalReadback"));
	FRHIGPUBufferReadback* CounterReadback = new FRHIGPUBufferReadback(TEXT("BoxSceneTriangles_BatchCounterReadback"));
	bool bRenderWorkQueued = false;
	bool bHasGPUOutput = false;

	ENQUEUE_RENDER_COMMAND(GetBoxSceneTrianglesBatchGPU)(
		[Request, ReferencePoints, ReferenceFilterDistance, BatchTriangleCapacity, VertexReadback, NormalReadback, CounterReadback, CounterReadbackBytes,
		 &bRenderWorkQueued, &bHasGPUOutput](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			TArray<FResolvedStaticMeshTriangleRequest> BatchRequests;
			BatchRequests.Add(Request);
			FCSStaticMeshTriangleRDGOutput TriangleOutput = AddResolvedStaticMeshTrianglesToRDGInternal(
				GraphBuilder,
				RHICmdList,
				BatchRequests,
				uint64(FMath::Max(0, Request.TriangleCount)),
				ReferencePoints,
				ReferenceFilterDistance,
				BatchTriangleCapacity,
				nullptr,
				TEXT("CS.BoxSceneTriangles.Batch"));

			if (TriangleOutput.TriangleVertices && TriangleOutput.TriangleNormals && TriangleOutput.TriangleCounter)
			{
				const uint32 VertexReadbackBytes = uint32(uint64(TriangleOutput.MaxVertices) * sizeof(FVector4f));
				AddEnqueueCopyPass(GraphBuilder, VertexReadback, TriangleOutput.TriangleVertices, VertexReadbackBytes);
				AddEnqueueCopyPass(GraphBuilder, NormalReadback, TriangleOutput.TriangleNormals, VertexReadbackBytes);
				AddEnqueueCopyPass(GraphBuilder, CounterReadback, TriangleOutput.TriangleCounter, CounterReadbackBytes);
				bHasGPUOutput = true;
			}

			GraphBuilder.Execute();
			bRenderWorkQueued = true;
		});

	FlushRenderingCommands();

	if (!bRenderWorkQueued || !bHasGPUOutput)
	{
		delete VertexReadback;
		delete NormalReadback;
		delete CounterReadback;
		return bRenderWorkQueued;
	}

	TArray<FVector4f> VertexData;
	TArray<FVector4f> NormalData;
	VertexData.SetNumZeroed(BatchVertexCapacity);
	NormalData.SetNumZeroed(BatchVertexCapacity);
	uint32 TriangleCount = 0;
	bool bReadbackSucceeded = false;

	ENQUEUE_RENDER_COMMAND(GetBoxSceneTrianglesBatchReadback)(
		[VertexReadback, NormalReadback, CounterReadback, BatchVertexReadbackBytes, CounterReadbackBytes,
		 &VertexData, &NormalData, &TriangleCount, &bReadbackSucceeded](FRHICommandListImmediate& RHICmdList)
		{
			if (!VertexReadback || !NormalReadback || !CounterReadback)
			{
				return;
			}

			if (!VertexReadback->IsReady() || !NormalReadback->IsReady() || !CounterReadback->IsReady())
			{
				RHICmdList.SubmitAndBlockUntilGPUIdle();
			}

			if (!VertexReadback->IsReady() || !NormalReadback->IsReady() || !CounterReadback->IsReady())
			{
				UE_LOG(LogTemp, Warning, TEXT("[GetBoxSceneTrianglesFromGPU] Batch GPU readback was not ready after flush."));
				delete VertexReadback;
				delete NormalReadback;
				delete CounterReadback;
				return;
			}

			if (VertexReadback->GetGPUSizeBytes() < BatchVertexReadbackBytes ||
				NormalReadback->GetGPUSizeBytes() < BatchVertexReadbackBytes ||
				CounterReadback->GetGPUSizeBytes() < CounterReadbackBytes)
			{
				UE_LOG(LogTemp, Warning, TEXT("[GetBoxSceneTrianglesFromGPU] Batch GPU readback size mismatch. Vertex=%llu/%u Normal=%llu/%u Counter=%llu/%u"),
					VertexReadback->GetGPUSizeBytes(),
					BatchVertexReadbackBytes,
					NormalReadback->GetGPUSizeBytes(),
					BatchVertexReadbackBytes,
					CounterReadback->GetGPUSizeBytes(),
					CounterReadbackBytes);
				delete VertexReadback;
				delete NormalReadback;
				delete CounterReadback;
				return;
			}

			bool bLockedAll = true;
			if (const FVector4f* VertexPtr = static_cast<const FVector4f*>(VertexReadback->Lock(BatchVertexReadbackBytes)))
			{
				FMemory::Memcpy(VertexData.GetData(), VertexPtr, BatchVertexReadbackBytes);
				VertexReadback->Unlock();
			}
			else
			{
				bLockedAll = false;
			}

			if (const FVector4f* NormalPtr = static_cast<const FVector4f*>(NormalReadback->Lock(BatchVertexReadbackBytes)))
			{
				FMemory::Memcpy(NormalData.GetData(), NormalPtr, BatchVertexReadbackBytes);
				NormalReadback->Unlock();
			}
			else
			{
				bLockedAll = false;
			}

			if (const uint32* CounterPtr = static_cast<const uint32*>(CounterReadback->Lock(CounterReadbackBytes)))
			{
				TriangleCount = *CounterPtr;
				CounterReadback->Unlock();
			}
			else
			{
				bLockedAll = false;
			}

			delete VertexReadback;
			delete NormalReadback;
			delete CounterReadback;
			bReadbackSucceeded = bLockedAll;
		});

	FlushRenderingCommands();

	if (!bReadbackSucceeded)
	{
		return false;
	}

	const int32 SafeTriangleCount = FMath::Clamp<int32>(int32(TriangleCount), 0, BatchTriangleCapacity);
	const int32 EffectiveVertexCount = FMath::Clamp(SafeTriangleCount * 3, 0, VertexData.Num());
	if (EffectiveVertexCount <= 0)
	{
		return true;
	}

	OutTriangleData.Vertices.Reserve(EffectiveVertexCount);
	OutTriangleData.VertexNormals.Reserve(EffectiveVertexCount);
	for (int32 VertexIndex = 0; VertexIndex < EffectiveVertexCount; ++VertexIndex)
	{
		const FVector4f& Vertex = VertexData[VertexIndex];
		OutTriangleData.Vertices.Add(FVector(Vertex.X, Vertex.Y, Vertex.Z));

		const FVector4f& Normal = NormalData[VertexIndex];
		OutTriangleData.VertexNormals.Add(FVector(Normal.X, Normal.Y, Normal.Z));
	}

	OutTriangleData.VertexCount = EffectiveVertexCount;
	OutTriangleData.IndexCount = 0;
	return true;
}

FCSSurfaceVoxelRDGOutput AddTriangleSurfaceVoxelsToRDGInternal(
	FRDGBuilder& GraphBuilder,
	const FCSStaticMeshTriangleRDGOutput& TriangleOutput,
	FVector VoxelOrigin,
	float VoxelSize,
	float SurfaceThickness,
	int32 MaxVoxels,
	int32 HashSlotCount,
	int32 MaxVoxelCellsPerTriangle,
	int32 BlurIterations,
	int32 InBlurRadius,
	const TCHAR* DebugName)
{
	FCSSurfaceVoxelRDGOutput Output;
	if (!TriangleOutput.TriangleVertices
		|| !TriangleOutput.TriangleNormals
		|| !TriangleOutput.TriangleCounter
		|| TriangleOutput.MaxTriangles == 0
		|| MaxVoxels <= 0)
	{
		return Output;
	}

	const float SafeVoxelSize = FMath::Max(VoxelSize, UE_KINDA_SMALL_NUMBER);
	const float SafeSurfaceThickness = SurfaceThickness > 0.0f ? SurfaceThickness : SafeVoxelSize * 0.5f;
	const uint32 VoxelCapacity = uint32(FMath::Max(1, MaxVoxels));
	const uint32 SafeHashSlotCount = GetSurfaceVoxelHashSlotCount(MaxVoxels, HashSlotCount);
	const uint32 SafeMaxVoxelCellsPerTriangle = uint32(FMath::Max(1, MaxVoxelCellsPerTriangle));

	Output.MaxVoxels = VoxelCapacity;
	Output.HashSlotCount = SafeHashSlotCount;
	Output.VoxelSize = SafeVoxelSize;
	Output.VoxelOrigin = VoxelOrigin;

	Output.VoxelPositions = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), VoxelCapacity),
		TEXT("CS.SurfaceVoxels.Positions"));
	Output.VoxelPositionsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Output.VoxelPositions, PF_A32B32G32R32F));
	Output.VoxelPositionsSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Output.VoxelPositions, PF_A32B32G32R32F));
	AddClearUAVPass(GraphBuilder, Output.VoxelPositionsUAV, 0.0f);

	Output.VoxelNormals = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), VoxelCapacity),
		TEXT("CS.SurfaceVoxels.Normals"));
	Output.VoxelNormalsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Output.VoxelNormals, PF_A32B32G32R32F));
	Output.VoxelNormalsSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Output.VoxelNormals, PF_A32B32G32R32F));
	AddClearUAVPass(GraphBuilder, Output.VoxelNormalsUAV, 0.0f);

	Output.VoxelCounter = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1),
		TEXT("CS.SurfaceVoxels.Counter"));
	Output.VoxelCounterUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Output.VoxelCounter, PF_R32_UINT));
	Output.VoxelCounterSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Output.VoxelCounter, PF_R32_UINT));
	AddClearUAVPass(GraphBuilder, Output.VoxelCounterUAV, 0u);

	Output.VoxelHashSlots = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), SafeHashSlotCount),
		TEXT("CS.SurfaceVoxels.HashSlots"));
	Output.VoxelHashSlotsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Output.VoxelHashSlots, PF_R32_UINT));
	AddClearUAVPass(GraphBuilder, Output.VoxelHashSlotsUAV, 0u);

	Output.VoxelHashIndices = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), SafeHashSlotCount),
		TEXT("CS.SurfaceVoxels.HashIndices"));
	Output.VoxelHashIndicesUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Output.VoxelHashIndices, PF_R32_UINT));
	AddClearUAVPass(GraphBuilder, Output.VoxelHashIndicesUAV, 0u);

	Output.VoxelNormalSums = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(int32), VoxelCapacity * 4u),
		TEXT("CS.SurfaceVoxels.NormalSums"));
	Output.VoxelNormalSumsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Output.VoxelNormalSums, PF_R32_SINT));
	Output.VoxelNormalSumsSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Output.VoxelNormalSums, PF_R32_SINT));
	AddClearUAVPass(GraphBuilder, Output.VoxelNormalSumsUAV, 0u);

	Output.VoxelNormalCounts = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), VoxelCapacity),
		TEXT("CS.SurfaceVoxels.NormalCounts"));
	Output.VoxelNormalCountsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Output.VoxelNormalCounts, PF_R32_UINT));
	Output.VoxelNormalCountsSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Output.VoxelNormalCounts, PF_R32_UINT));
	AddClearUAVPass(GraphBuilder, Output.VoxelNormalCountsUAV, 0u);

	// ResinRattan port: target-position accumulation buffers
	Output.VoxelTargetPositions = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), VoxelCapacity),
		TEXT("CS.SurfaceVoxels.TargetPositions"));
	Output.VoxelTargetPositionsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Output.VoxelTargetPositions, PF_A32B32G32R32F));
	Output.VoxelTargetPositionsSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Output.VoxelTargetPositions, PF_A32B32G32R32F));
	AddClearUAVPass(GraphBuilder, Output.VoxelTargetPositionsUAV, 0.0f);

	Output.VoxelTargetOffsetSums = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(int32), VoxelCapacity * 4u),
		TEXT("CS.SurfaceVoxels.TargetOffsetSums"));
	Output.VoxelTargetOffsetSumsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Output.VoxelTargetOffsetSums, PF_R32_SINT));
	AddClearUAVPass(GraphBuilder, Output.VoxelTargetOffsetSumsUAV, 0u);

	Output.VoxelTargetWeightSums = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), VoxelCapacity),
		TEXT("CS.SurfaceVoxels.TargetWeightSums"));
	Output.VoxelTargetWeightSumsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Output.VoxelTargetWeightSums, PF_R32_UINT));
	AddClearUAVPass(GraphBuilder, Output.VoxelTargetWeightSumsUAV, 0u);

	Output.VoxelCells = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(int32) * 4, VoxelCapacity),
		TEXT("CS.SurfaceVoxels.Cells"));
	Output.VoxelCellsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Output.VoxelCells, PF_R32G32B32A32_UINT));
	AddClearUAVPass(GraphBuilder, Output.VoxelCellsUAV, 0u);

	// Blur output buffers (allocated even if blur is disabled to simplify lifetime)
	Output.BlurredVoxelNormals = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), VoxelCapacity),
		TEXT("CS.SurfaceVoxels.BlurredNormals"));
	Output.BlurredVoxelNormalsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Output.BlurredVoxelNormals, PF_A32B32G32R32F));
	Output.BlurredVoxelNormalsSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Output.BlurredVoxelNormals, PF_A32B32G32R32F));
	AddClearUAVPass(GraphBuilder, Output.BlurredVoxelNormalsUAV, 0.0f);

	Output.BlurredVoxelTargetPositions = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), VoxelCapacity),
		TEXT("CS.SurfaceVoxels.BlurredTargetPositions"));
	Output.BlurredVoxelTargetPositionsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Output.BlurredVoxelTargetPositions, PF_A32B32G32R32F));
	Output.BlurredVoxelTargetPositionsSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Output.BlurredVoxelTargetPositions, PF_A32B32G32R32F));
	AddClearUAVPass(GraphBuilder, Output.BlurredVoxelTargetPositionsUAV, 0.0f);

	FRDGBufferSRVRef TriangleVerticesSRV = TriangleOutput.TriangleVerticesSRV;
	if (!TriangleVerticesSRV)
	{
		TriangleVerticesSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(TriangleOutput.TriangleVertices, PF_A32B32G32R32F));
	}

	FRDGBufferSRVRef TriangleNormalsSRV = TriangleOutput.TriangleNormalsSRV;
	if (!TriangleNormalsSRV)
	{
		TriangleNormalsSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(TriangleOutput.TriangleNormals, PF_A32B32G32R32F));
	}

	FRDGBufferSRVRef TriangleCounterSRV = TriangleOutput.TriangleCounterSRV;
	if (!TriangleCounterSRV)
	{
		TriangleCounterSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(TriangleOutput.TriangleCounter, PF_R32_UINT));
	}

	TShaderMapRef<FTriangleSurfaceVoxelsCS> VoxelShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FTriangleSurfaceVoxelsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FTriangleSurfaceVoxelsCS::FParameters>();
	PassParameters->TriangleVertices = TriangleVerticesSRV;
	PassParameters->TriangleNormals = TriangleNormalsSRV;
	PassParameters->SurfaceTriangleCounter = TriangleCounterSRV;
	PassParameters->RW_OutVoxelPositions = Output.VoxelPositionsUAV;
	PassParameters->RW_OutVoxelNormals = Output.VoxelNormalsUAV;
	PassParameters->RW_SurfaceVoxelCounter = Output.VoxelCounterUAV;
	PassParameters->RW_SurfaceVoxelHashSlots = Output.VoxelHashSlotsUAV;
	PassParameters->RW_SurfaceVoxelHashIndices = Output.VoxelHashIndicesUAV;
	PassParameters->RW_SurfaceVoxelNormalSums = Output.VoxelNormalSumsUAV;
	PassParameters->RW_SurfaceVoxelNormalCounts = Output.VoxelNormalCountsUAV;
	PassParameters->RW_OutVoxelTargetPositions = Output.VoxelTargetPositionsUAV;
	PassParameters->RW_SurfaceVoxelTargetOffsetSums = Output.VoxelTargetOffsetSumsUAV;
	PassParameters->RW_SurfaceVoxelTargetWeightSums = Output.VoxelTargetWeightSumsUAV;
	PassParameters->RW_OutVoxelCells = Output.VoxelCellsUAV;
	PassParameters->SurfaceVoxelOrigin = FVector3f(VoxelOrigin);
	PassParameters->SurfaceVoxelSize = SafeVoxelSize;
	PassParameters->SurfaceThickness = SafeSurfaceThickness;
	PassParameters->SurfaceTriangleCount = TriangleOutput.MaxTriangles;
	PassParameters->SurfaceVoxelCapacity = VoxelCapacity;
	PassParameters->SurfaceVoxelHashSlotCount = SafeHashSlotCount;
	PassParameters->MaxSurfaceVoxelCellsPerTriangle = SafeMaxVoxelCellsPerTriangle;

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("%s.TriangleSurfaceVoxels", DebugName),
		PassParameters,
		ERDGPassFlags::Compute,
		[PassParameters, VoxelShader, TriangleCapacity = TriangleOutput.MaxTriangles](FRHIComputeCommandList& InRHICmdList)
		{
			FComputeShaderUtils::Dispatch(
				InRHICmdList,
				VoxelShader,
				*PassParameters,
				FComputeShaderUtils::GetGroupCount(FIntVector(int32(TriangleCapacity), 1, 1), 64));
		});

	TShaderMapRef<FFinalizeSurfaceVoxelNormalsCS> FinalizeNormalsShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FFinalizeSurfaceVoxelNormalsCS::FParameters* FinalizeParameters = GraphBuilder.AllocParameters<FFinalizeSurfaceVoxelNormalsCS::FParameters>();
	FinalizeParameters->SurfaceVoxelNormalSums = Output.VoxelNormalSumsSRV;
	FinalizeParameters->SurfaceVoxelNormalCounts = Output.VoxelNormalCountsSRV;
	FinalizeParameters->RW_SurfaceVoxelTargetOffsetSums = Output.VoxelTargetOffsetSumsUAV;
	FinalizeParameters->RW_SurfaceVoxelTargetWeightSums = Output.VoxelTargetWeightSumsUAV;
	FinalizeParameters->RW_OutVoxelPositions = Output.VoxelPositionsUAV;
	FinalizeParameters->RW_OutVoxelNormals = Output.VoxelNormalsUAV;
	FinalizeParameters->RW_OutVoxelTargetPositions = Output.VoxelTargetPositionsUAV;
	FinalizeParameters->SurfaceVoxelCapacity = VoxelCapacity;
	FinalizeParameters->SurfaceVoxelSize = SafeVoxelSize;
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("%s.FinalizeSurfaceVoxelNormals", DebugName),
		FinalizeParameters,
		ERDGPassFlags::Compute,
		[FinalizeParameters, FinalizeNormalsShader, VoxelCapacity](FRHIComputeCommandList& InRHICmdList)
		{
			FComputeShaderUtils::Dispatch(
				InRHICmdList,
				FinalizeNormalsShader,
				*FinalizeParameters,
				FComputeShaderUtils::GetGroupCount(FIntVector(int32(VoxelCapacity), 1, 1), 64));
		});

	// Blur pass (ported from ResinRattan) — only executes when BlurIterations > 0
	{
		const uint32 BlurIters = uint32(FMath::Max(0, BlurIterations));
		if (BlurIters > 0u)
		{
			const uint32 BlurRadius = uint32(FMath::Max(1, InBlurRadius));
			int32 CurrentNormalsIdx = 0; // 0 = VoxelNormals, 1 = BlurredVoxelNormals
			int32 CurrentTargetsIdx = 0; // 0 = VoxelTargetPositions, 1 = BlurredVoxelTargetPositions

			TShaderMapRef<FBlurSurfaceVoxelsCS> BlurShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

			for (uint32 Iter = 0u; Iter < BlurIters; ++Iter)
			{
				const bool bReadFromOriginal = (CurrentNormalsIdx == 0);
				FRDGBufferUAVRef SrcNormalsUAV = bReadFromOriginal ? Output.VoxelNormalsUAV : Output.BlurredVoxelNormalsUAV;
				FRDGBufferUAVRef SrcTargetsUAV = bReadFromOriginal ? Output.VoxelTargetPositionsUAV : Output.BlurredVoxelTargetPositionsUAV;
				FRDGBufferUAVRef DstNormalsUAV = bReadFromOriginal ? Output.BlurredVoxelNormalsUAV : Output.VoxelNormalsUAV;
				FRDGBufferUAVRef DstTargetsUAV = bReadFromOriginal ? Output.BlurredVoxelTargetPositionsUAV : Output.VoxelTargetPositionsUAV;

				FBlurSurfaceVoxelsCS::FParameters* BlurParameters = GraphBuilder.AllocParameters<FBlurSurfaceVoxelsCS::FParameters>();
				BlurParameters->RW_SurfaceVoxelCounter = Output.VoxelCounterUAV;
				BlurParameters->RW_OutVoxelNormals = SrcNormalsUAV;
				BlurParameters->RW_OutVoxelTargetPositions = SrcTargetsUAV;
				BlurParameters->RW_OutVoxelCells = Output.VoxelCellsUAV;
				BlurParameters->RW_SurfaceVoxelHashSlots = Output.VoxelHashSlotsUAV;
				BlurParameters->RW_SurfaceVoxelHashIndices = Output.VoxelHashIndicesUAV;
				BlurParameters->RW_BlurredVoxelNormals = DstNormalsUAV;
				BlurParameters->RW_BlurredVoxelTargetPositions = DstTargetsUAV;
				BlurParameters->SurfaceVoxelCapacity = VoxelCapacity;
				BlurParameters->SurfaceVoxelHashSlotCount = SafeHashSlotCount;
				BlurParameters->SurfaceVoxelBlurRadius = BlurRadius;

				GraphBuilder.AddPass(
					RDG_EVENT_NAME("%s.BlurSurfaceVoxels_Iter_%u", DebugName, Iter),
					BlurParameters,
					ERDGPassFlags::Compute,
					[BlurParameters, BlurShader, VoxelCapacity](FRHIComputeCommandList& InRHICmdList)
					{
						FComputeShaderUtils::Dispatch(
							InRHICmdList,
							BlurShader,
							*BlurParameters,
							FComputeShaderUtils::GetGroupCount(FIntVector(int32(VoxelCapacity), 1, 1), 64));
					});

				// Flip ping-pong for next iteration
				CurrentNormalsIdx = 1 - CurrentNormalsIdx;
				CurrentTargetsIdx = 1 - CurrentTargetsIdx;
			}

			// After the last blur, the "current" buffers hold the final result.
			// Make VoxelNormals/VoxelTargetPositions point to the final output so
			// downstream readback uses the blurred data.
			{
				const bool bFinalIsOriginal = (CurrentNormalsIdx == 0);
				if (!bFinalIsOriginal)
				{
					Output.VoxelNormals = Output.BlurredVoxelNormals;
					Output.VoxelNormalsSRV = Output.BlurredVoxelNormalsSRV;
					Output.VoxelNormalsUAV = Output.BlurredVoxelNormalsUAV;
					Output.VoxelTargetPositions = Output.BlurredVoxelTargetPositions;
					Output.VoxelTargetPositionsSRV = Output.BlurredVoxelTargetPositionsSRV;
					Output.VoxelTargetPositionsUAV = Output.BlurredVoxelTargetPositionsUAV;
				}
			}
		}
	}

	return Output;
}
}

// -----------------------------------------------------------------------------
// Core System - Public GPU Extraction
// -----------------------------------------------------------------------------

FCSStaticMeshTriangleRDGOutput AComputeShaderMeshGenerator::AddStaticMeshTrianglesToRDG(
	FRDGBuilder& GraphBuilder,
	FRHICommandListImmediate& RHICmdList,
	const TArray<FCSStaticMeshTriangleRequest>& Requests,
	float ReferenceFilterDistance,
	const TCHAR* DebugName,
	bool bNaniteOnlyFallbackMesh)
{
	return AddStaticMeshTrianglesToRDGInternal(
		GraphBuilder,
		RHICmdList,
		Requests,
		ReferencePoints,
		ReferenceFilterDistance,
		FMath::Max(1, MaxTriangles),
		nullptr,
		DebugName,
		this,
		ExcludedActorTag,
		bNaniteOnlyFallbackMesh);
}

FCSSurfaceVoxelRDGOutput AComputeShaderMeshGenerator::AddTriangleSurfaceVoxelsToRDG(
	FRDGBuilder& GraphBuilder,
	const FCSStaticMeshTriangleRDGOutput& TriangleOutput,
	FVector VoxelOrigin,
	float VoxelSize,
	int32 HashSlotCount,
	int32 BlurIterations,
	int32 BlurRadius,
	const TCHAR* DebugName)
{
	return AddTriangleSurfaceVoxelsToRDGInternal(
		GraphBuilder,
		TriangleOutput,
		VoxelOrigin,
		VoxelSize,
		0.0f,
		FMath::Max(1, MaxVoxels),
		HashSlotCount,
		FMath::Max(1, MaxVoxelCellsPerTriangle),
		BlurIterations,
		BlurRadius,
		DebugName);
}

FCSTriangleMeshData AComputeShaderMeshGenerator::GetBoxSceneTrianglesFromGPU()
{
	return GetBoxSceneTrianglesFromGPUFiltered(0.0f);
}

FCSTriangleMeshData AComputeShaderMeshGenerator::GetBoxSceneTrianglesFromGPUFiltered(float ReferenceFilterDistance)
{
	const TArray<FVector> ReferencePointsForRender = ReferencePoints;
	return ReadBoxSceneTrianglesFromGPUFilteredInternal(
		GetGeneratorBoundsWorldBox(),
		ReferencePointsForRender,
		ReferenceFilterDistance,
		TEXT("[GetBoxSceneTrianglesFromGPU]"),
		this,
		ExcludedActorTag);
}

FCSTriangleMeshData AComputeShaderMeshGenerator::ReadBoxSceneTrianglesFromGPUFilteredInternal(const FBox& QueryBox,
	const TArray<FVector>& ReferencePointsForRender,
	float ReferenceFilterDistance,
	const TCHAR* LogPrefix,
	const AActor* ExcludedActor,
	FName ExcludedActorTagForResolve)
{
	FCSTriangleMeshData OutTriangleData;
	LastTriangleTextureData.bValid = false;
	LastTriangleTextureData.VertexCount = 0;
	LastTriangleTextureData.TriangleCount = 0;
	LastTriangleTextureData.IndexCount = 0;

	UWorld* World = GetWorld();
	if (!World)
	{
		return OutTriangleData;
	}

	if (!QueryBox.IsValid)
	{
		return OutTriangleData;
	}

	const int32 SafeMaxTriangles = FMath::Max(1, MaxTriangles);
	TArray<FCSStaticMeshTriangleRequest> Requests;
	BuildBoxSceneTriangleRequestsInternal(World, QueryBox, VoxelGridSettings.LODIndex, Requests);

	FCSTriangleMeshData LandscapeTriangleData;
	BuildBoxSceneLandscapeTrianglesInternal(
		World,
		QueryBox,
		ReferencePointsForRender,
		ReferenceFilterDistance,
		SafeMaxTriangles,
		LandscapeTriangleData);
	const bool bHasLandscapeTriangles = GetTriangleMeshDataTriangleCount(LandscapeTriangleData) > 0;
	if (Requests.IsEmpty() && !bHasLandscapeTriangles)
	{
		return OutTriangleData;
	}

	TArray<FResolvedStaticMeshTriangleRequest> ResolvedRequests;
	ResolveStaticMeshTriangleRequests(
		Requests,
		ExcludedActor,
		ExcludedActorTagForResolve,
		true,
		ResolvedRequests);
	if (ResolvedRequests.IsEmpty() && !bHasLandscapeTriangles)
	{
		return OutTriangleData;
	}

	AppendTriangleMeshData(OutTriangleData, LandscapeTriangleData, SafeMaxTriangles);

	for (const FResolvedStaticMeshTriangleRequest& Request : ResolvedRequests)
	{
		const int32 CurrentTriangleCount = GetTriangleMeshDataTriangleCount(OutTriangleData);
		const int32 RemainingTriangles = SafeMaxTriangles - CurrentTriangleCount;
		if (RemainingTriangles <= 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("%s MaxTriangles reached; remaining scene triangles are skipped. MaxTriangles=%d"), LogPrefix, SafeMaxTriangles);
			break;
		}

		FCSTriangleMeshData RequestTriangleData;
		if (!ReadbackResolvedStaticMeshTriangleRequestSync(
			Request,
			ReferencePointsForRender,
			ReferenceFilterDistance,
			RemainingTriangles,
			RequestTriangleData))
		{
			UE_LOG(LogTemp, Warning, TEXT("%s Failed to read back a static mesh triangle batch."), LogPrefix);
			break;
		}

		AppendTriangleMeshData(OutTriangleData, RequestTriangleData, SafeMaxTriangles);
	}

	const int32 EffectiveVertexCount = OutTriangleData.Vertices.Num();
	if (EffectiveVertexCount <= 0)
	{
		return OutTriangleData;
	}

	OutTriangleData.VertexCount = EffectiveVertexCount;
	OutTriangleData.IndexCount = 0;
	NormalizeTriangleMeshDataWinding(OutTriangleData);
	StoreTriangleTextureData(OutTriangleData, ReferenceFilterDistance, QueryBox);
	return OutTriangleData;
}

FCSSurfaceVoxelData AComputeShaderMeshGenerator::GetBoxSceneSurfaceVoxelsFromGPU(float VoxelSize)
{
	FCSSurfaceVoxelData OutVoxelData;
	OutVoxelData.VoxelSize = FMath::Max(VoxelSize, UE_KINDA_SMALL_NUMBER);
	LastSurfaceVoxelData = FCSSurfaceVoxelData();
	LastSurfaceVoxelData.VoxelSize = OutVoxelData.VoxelSize;
	LastSurfaceVoxelTextureData.bValid = false;
	LastSurfaceVoxelTextureData.VoxelCount = 0;
	LastSurfaceVoxelTextureData.VoxelSize = OutVoxelData.VoxelSize;

	UWorld* World = GetWorld();
	if (!World)
	{
		return OutVoxelData;
	}

	const FBox QueryBox = GetGeneratorBoundsWorldBox();
	if (!QueryBox.IsValid)
	{
		return OutVoxelData;
	}

	// Voxel cell 坐标系原点 — 存入 OutVoxelData 用于后续体素采样
	OutVoxelData.VoxelOrigin = QueryBox.Min;

	const int32 SafeMaxTriangles = FMath::Max(1, MaxTriangles);
	const int32 SafeMaxVoxels = FMath::Max(1, MaxVoxels);
	const int32 SafeSurfaceVoxelBlurIterations = FMath::Max(0, SurfaceVoxelBlurIterations);
	const int32 SafeSurfaceVoxelBlurRadius = FMath::Max(1, SurfaceVoxelBlurRadius);
	TArray<FCSStaticMeshTriangleRequest> Requests;
	BuildBoxSceneTriangleRequests(World, QueryBox, Requests);

	FCSTriangleMeshData LandscapeTriangleData;
	const TArray<FVector> EmptyReferencePoints;
	BuildBoxSceneLandscapeTrianglesInternal(
		World,
		QueryBox,
		EmptyReferencePoints,
		0.0f,
		SafeMaxTriangles,
		LandscapeTriangleData);

	const bool bHasLandscapeTriangles = GetTriangleMeshDataTriangleCount(LandscapeTriangleData) > 0;
	if (Requests.IsEmpty() && !bHasLandscapeTriangles)
	{
		return OutVoxelData;
	}

	TArray<FResolvedStaticMeshTriangleRequest> ResolvedRequests;
	const uint64 TotalStaticMeshTriangleCount = ResolveStaticMeshTriangleRequests(
		Requests,
		this,
		ExcludedActorTag,
		true,
		ResolvedRequests);
	if (ResolvedRequests.IsEmpty() && !bHasLandscapeTriangles)
	{
		return OutVoxelData;
	}

	const uint64 MaxVoxelReadbackBytes64 = uint64(SafeMaxVoxels) * sizeof(FVector4f);
	if (MaxVoxelReadbackBytes64 > uint64(TNumericLimits<uint32>::Max()))
	{
		UE_LOG(LogTemp, Warning, TEXT("[GetBoxSceneSurfaceVoxelsFromGPU] Readback request too large. MaxVoxels=%d"), SafeMaxVoxels);
		return OutVoxelData;
	}

	const uint32 CounterReadbackBytes = sizeof(uint32);

	FRHIGPUBufferReadback* PositionReadback = new FRHIGPUBufferReadback(TEXT("BoxSceneSurfaceVoxels_PositionReadback"));
	FRHIGPUBufferReadback* NormalReadback = new FRHIGPUBufferReadback(TEXT("BoxSceneSurfaceVoxels_NormalReadback"));
	FRHIGPUBufferReadback* TargetPositionReadback = new FRHIGPUBufferReadback(TEXT("BoxSceneSurfaceVoxels_TargetPositionReadback"));
	FRHIGPUBufferReadback* CellReadback = new FRHIGPUBufferReadback(TEXT("BoxSceneSurfaceVoxels_CellReadback"));
	FRHIGPUBufferReadback* CounterReadback = new FRHIGPUBufferReadback(TEXT("BoxSceneSurfaceVoxels_CounterReadback"));
	bool bRenderWorkQueued = false;
	bool bHasGPUOutput = false;
	int32 VoxelCapacity = 0;
	uint32 ActualVoxelReadbackBytes = 0;
	uint32 ActualVoxelCellReadbackBytes = 0;

	ENQUEUE_RENDER_COMMAND(GetBoxSceneSurfaceVoxelsFromGPUGPU)(
		[ResolvedRequests = MoveTemp(ResolvedRequests), TotalStaticMeshTriangleCount, LandscapeTriangleData = MoveTemp(LandscapeTriangleData), PositionReadback, NormalReadback, TargetPositionReadback, CellReadback, CounterReadback, CounterReadbackBytes,
		 SafeMaxTriangles, SafeMaxVoxels, VoxelOrigin = QueryBox.Min, SafeVoxelSize = OutVoxelData.VoxelSize,
		 SafeMaxVoxelCellsPerTriangle = FMath::Max(1, MaxVoxelCellsPerTriangle), SafeSurfaceVoxelBlurIterations, SafeSurfaceVoxelBlurRadius,
		 &bRenderWorkQueued, &bHasGPUOutput, &VoxelCapacity, &ActualVoxelReadbackBytes, &ActualVoxelCellReadbackBytes](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			const TArray<FVector> EmptyReferencePoints;
			const FCSTriangleMeshData* InitialTriangleData = GetTriangleMeshDataTriangleCount(LandscapeTriangleData) > 0 ? &LandscapeTriangleData : nullptr;

			FCSStaticMeshTriangleRDGOutput TriangleOutput = AddResolvedStaticMeshTrianglesToRDGInternal(
				GraphBuilder,
				RHICmdList,
				ResolvedRequests,
				TotalStaticMeshTriangleCount,
				EmptyReferencePoints,
				0.0f,
				SafeMaxTriangles,
				InitialTriangleData,
				TEXT("CS.BoxSceneSurfaceVoxels.Triangles"));

			FCSSurfaceVoxelRDGOutput VoxelOutput = AddTriangleSurfaceVoxelsToRDGInternal(
				GraphBuilder,
				TriangleOutput,
				VoxelOrigin,
				SafeVoxelSize,
				0.0f,
				SafeMaxVoxels,
				0,
				SafeMaxVoxelCellsPerTriangle,
				SafeSurfaceVoxelBlurIterations,
				SafeSurfaceVoxelBlurRadius,
				TEXT("CS.BoxSceneSurfaceVoxels"));

			if (VoxelOutput.VoxelPositions
				&& VoxelOutput.VoxelNormals
				&& VoxelOutput.VoxelTargetPositions
				&& VoxelOutput.VoxelCells
				&& VoxelOutput.VoxelCounter)
			{
				VoxelCapacity = int32(VoxelOutput.MaxVoxels);
				ActualVoxelReadbackBytes = uint32(uint64(VoxelOutput.MaxVoxels) * sizeof(FVector4f));
				ActualVoxelCellReadbackBytes = uint32(uint64(VoxelOutput.MaxVoxels) * sizeof(FIntVector4));
				AddEnqueueCopyPass(GraphBuilder, PositionReadback, VoxelOutput.VoxelPositions, ActualVoxelReadbackBytes);
				AddEnqueueCopyPass(GraphBuilder, NormalReadback, VoxelOutput.VoxelNormals, ActualVoxelReadbackBytes);
				AddEnqueueCopyPass(GraphBuilder, TargetPositionReadback, VoxelOutput.VoxelTargetPositions, ActualVoxelReadbackBytes);
				AddEnqueueCopyPass(GraphBuilder, CellReadback, VoxelOutput.VoxelCells, ActualVoxelCellReadbackBytes);
				AddEnqueueCopyPass(GraphBuilder, CounterReadback, VoxelOutput.VoxelCounter, CounterReadbackBytes);
				bHasGPUOutput = true;
			}

			GraphBuilder.Execute();
			bRenderWorkQueued = true;
		});

	FlushRenderingCommands();

	if (!bRenderWorkQueued || !bHasGPUOutput)
	{
		delete PositionReadback;
		delete NormalReadback;
		delete TargetPositionReadback;
		delete CellReadback;
		delete CounterReadback;
		return OutVoxelData;
	}

	TArray<FVector4f> PositionData;
	TArray<FVector4f> NormalData;
	TArray<FVector4f> TargetPositionData;
	TArray<FIntVector4> CellData;
	PositionData.SetNumZeroed(VoxelCapacity);
	NormalData.SetNumZeroed(VoxelCapacity);
	TargetPositionData.SetNumZeroed(VoxelCapacity);
	CellData.SetNumZeroed(VoxelCapacity);
	uint32 VoxelCount = 0;
	bool bReadbackSucceeded = false;

	ENQUEUE_RENDER_COMMAND(GetBoxSceneSurfaceVoxelsFromGPUReadback)(
		[PositionReadback, NormalReadback, TargetPositionReadback, CellReadback, CounterReadback, ActualVoxelReadbackBytes, ActualVoxelCellReadbackBytes, CounterReadbackBytes,
		 &PositionData, &NormalData, &TargetPositionData, &CellData, &VoxelCount, &bReadbackSucceeded](FRHICommandListImmediate& RHICmdList)
		{
			if (!PositionReadback || !NormalReadback || !TargetPositionReadback || !CellReadback || !CounterReadback)
			{
				return;
			}

			if (!PositionReadback->IsReady()
				|| !NormalReadback->IsReady()
				|| !TargetPositionReadback->IsReady()
				|| !CellReadback->IsReady()
				|| !CounterReadback->IsReady())
			{
				RHICmdList.SubmitAndBlockUntilGPUIdle();
			}

			if (!PositionReadback->IsReady()
				|| !NormalReadback->IsReady()
				|| !TargetPositionReadback->IsReady()
				|| !CellReadback->IsReady()
				|| !CounterReadback->IsReady())
			{
				UE_LOG(LogTemp, Warning, TEXT("[GetBoxSceneSurfaceVoxelsFromGPU] GPU readback was not ready after flush."));
				delete PositionReadback;
				delete NormalReadback;
				delete TargetPositionReadback;
				delete CellReadback;
				delete CounterReadback;
				return;
			}

			if (PositionReadback->GetGPUSizeBytes() < ActualVoxelReadbackBytes ||
				NormalReadback->GetGPUSizeBytes() < ActualVoxelReadbackBytes ||
				TargetPositionReadback->GetGPUSizeBytes() < ActualVoxelReadbackBytes ||
				CellReadback->GetGPUSizeBytes() < ActualVoxelCellReadbackBytes ||
				CounterReadback->GetGPUSizeBytes() < CounterReadbackBytes)
			{
				UE_LOG(LogTemp, Warning, TEXT("[GetBoxSceneSurfaceVoxelsFromGPU] GPU readback size mismatch. Position=%llu/%u Normal=%llu/%u Target=%llu/%u Cell=%llu/%u Counter=%llu/%u"),
					PositionReadback->GetGPUSizeBytes(),
					ActualVoxelReadbackBytes,
					NormalReadback->GetGPUSizeBytes(),
					ActualVoxelReadbackBytes,
					TargetPositionReadback->GetGPUSizeBytes(),
					ActualVoxelReadbackBytes,
					CellReadback->GetGPUSizeBytes(),
					ActualVoxelCellReadbackBytes,
					CounterReadback->GetGPUSizeBytes(),
					CounterReadbackBytes);
				delete PositionReadback;
				delete NormalReadback;
				delete TargetPositionReadback;
				delete CellReadback;
				delete CounterReadback;
				return;
			}

			bool bLockedAll = true;
			if (const FVector4f* PositionPtr = static_cast<const FVector4f*>(PositionReadback->Lock(ActualVoxelReadbackBytes)))
			{
				FMemory::Memcpy(PositionData.GetData(), PositionPtr, ActualVoxelReadbackBytes);
				PositionReadback->Unlock();
			}
			else
			{
				bLockedAll = false;
			}

			if (const FVector4f* NormalPtr = static_cast<const FVector4f*>(NormalReadback->Lock(ActualVoxelReadbackBytes)))
			{
				FMemory::Memcpy(NormalData.GetData(), NormalPtr, ActualVoxelReadbackBytes);
				NormalReadback->Unlock();
			}
			else
			{
				bLockedAll = false;
			}

			if (const FVector4f* TargetPositionPtr = static_cast<const FVector4f*>(TargetPositionReadback->Lock(ActualVoxelReadbackBytes)))
			{
				FMemory::Memcpy(TargetPositionData.GetData(), TargetPositionPtr, ActualVoxelReadbackBytes);
				TargetPositionReadback->Unlock();
			}
			else
			{
				bLockedAll = false;
			}

			if (const FIntVector4* CellPtr = static_cast<const FIntVector4*>(CellReadback->Lock(ActualVoxelCellReadbackBytes)))
			{
				FMemory::Memcpy(CellData.GetData(), CellPtr, ActualVoxelCellReadbackBytes);
				CellReadback->Unlock();
			}
			else
			{
				bLockedAll = false;
			}

			if (const uint32* CounterPtr = static_cast<const uint32*>(CounterReadback->Lock(CounterReadbackBytes)))
			{
				VoxelCount = *CounterPtr;
				CounterReadback->Unlock();
			}
			else
			{
				bLockedAll = false;
			}

			delete PositionReadback;
			delete NormalReadback;
			delete TargetPositionReadback;
			delete CellReadback;
			delete CounterReadback;
			bReadbackSucceeded = bLockedAll;
		});

	FlushRenderingCommands();

	if (!bReadbackSucceeded)
	{
		return OutVoxelData;
	}

	const int32 EffectiveVoxelCount = FMath::Clamp<int32>(int32(VoxelCount), 0, PositionData.Num());
	if (EffectiveVoxelCount <= 0)
	{
		OutVoxelData.VoxelCount = 0;
		return OutVoxelData;
	}

	OutVoxelData.Positions.Reserve(EffectiveVoxelCount);
	OutVoxelData.Normals.Reserve(EffectiveVoxelCount);
	OutVoxelData.TargetPositions.Reserve(EffectiveVoxelCount);
	OutVoxelData.Cells.Reserve(EffectiveVoxelCount);
	int32 InvalidPositionCount = 0;
	int32 InvalidNormalCount = 0;
	int32 InvalidTargetCount = 0;
	int32 InvalidTargetVectorCount = 0;
	int32 InvalidTargetWCount = 0;
	int32 FarTargetCount = 0;
	const double MaxTargetDistanceSq = FMath::Square(double(FMath::Max(OutVoxelData.VoxelSize * 2.0f, UE_KINDA_SMALL_NUMBER)));
	for (int32 VoxelIndex = 0; VoxelIndex < EffectiveVoxelCount; ++VoxelIndex)
	{
		const FVector4f& Position = PositionData[VoxelIndex];
		const FVector4f& Normal = NormalData[VoxelIndex];
		const FVector4f& TargetPosition = TargetPositionData[VoxelIndex];
		const FIntVector4& Cell = CellData[VoxelIndex];

		if (!IsFiniteCSVector4(Position) || Position.W <= 0.0f)
		{
			++InvalidPositionCount;
			continue;
		}

		const FVector VoxelCenter(Position.X, Position.Y, Position.Z);
		FVector SafeNormal(Normal.X, Normal.Y, Normal.Z);
		if (!IsFiniteCSVector4(Normal) || !SafeNormal.Normalize())
		{
			SafeNormal = FVector::UpVector;
			++InvalidNormalCount;
		}

		FVector SafeTarget(TargetPosition.X, TargetPosition.Y, TargetPosition.Z);
		const bool bFiniteTarget = IsFiniteCSVector4(TargetPosition);
		const bool bPositiveTargetW = TargetPosition.W > 0.0f;
		const bool bTargetWithinVoxel = bFiniteTarget
			&& FVector::DistSquared(SafeTarget, VoxelCenter) <= MaxTargetDistanceSq;
		const bool bValidTarget = bFiniteTarget && bPositiveTargetW && bTargetWithinVoxel;
		if (!bValidTarget)
		{
			if (!bFiniteTarget)
			{
				++InvalidTargetVectorCount;
			}
			else if (!bPositiveTargetW)
			{
				++InvalidTargetWCount;
			}
			else
			{
				++FarTargetCount;
			}
			SafeTarget = VoxelCenter;
			++InvalidTargetCount;
		}

		OutVoxelData.Positions.Add(VoxelCenter);
		OutVoxelData.Normals.Add(SafeNormal);
		OutVoxelData.TargetPositions.Add(SafeTarget);
		OutVoxelData.Cells.Add(FIntVector(Cell.X, Cell.Y, Cell.Z));
	}
	OutVoxelData.VoxelCount = OutVoxelData.Positions.Num();
	if (InvalidPositionCount > 0 || InvalidNormalCount > 0 || InvalidTargetCount > 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[GetBoxSceneSurfaceVoxelsFromGPU] Sanitized surface voxel readback. Input=%d Output=%d InvalidPositions=%d InvalidNormals=%d InvalidTargets=%d InvalidTargetVectors=%d InvalidTargetW=%d FarTargets=%d"),
			EffectiveVoxelCount,
			OutVoxelData.VoxelCount,
			InvalidPositionCount,
			InvalidNormalCount,
			InvalidTargetCount,
			InvalidTargetVectorCount,
			InvalidTargetWCount,
			FarTargetCount);
	}
	LastSurfaceVoxelData = OutVoxelData;
	StoreSurfaceVoxelTextureData(OutVoxelData, QueryBox.Min);
	return OutVoxelData;
}

void AComputeShaderMeshGenerator::GetBoxSceneFilteredSurfaceVoxels(float VoxelSize,
	float ReferenceFilterDistance,
	TArray<FVector>& OutPositions,
	TArray<FVector>& OutNormals)
{
	OutPositions.Reset();
	OutNormals.Reset();

	const float SafeVoxelSize = FMath::Max(VoxelSize, UE_KINDA_SMALL_NUMBER);
	const TArray<FVector> ReferencePointsForRender = ReferencePoints;
	const float SafeFilterDistance = (ReferencePointsForRender.IsEmpty()) ? 0.0f : FMath::Max(0.0f, ReferenceFilterDistance);
	LastSurfaceVoxelData = FCSSurfaceVoxelData();
	LastSurfaceVoxelData.VoxelSize = SafeVoxelSize;
	LastSurfaceVoxelTextureData.bValid = false;
	LastSurfaceVoxelTextureData.VoxelCount = 0;
	LastSurfaceVoxelTextureData.VoxelSize = SafeVoxelSize;

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FBox QueryBox = GetGeneratorBoundsWorldBox();
	if (!QueryBox.IsValid)
	{
		return;
	}

	const int32 SafeMaxTriangles = FMath::Max(1, MaxTriangles);
	const int32 SafeMaxVoxels = FMath::Max(1, MaxVoxels);
	const int32 SafeSurfaceVoxelBlurIterations = FMath::Max(0, SurfaceVoxelBlurIterations);
	const int32 SafeSurfaceVoxelBlurRadius = FMath::Max(1, SurfaceVoxelBlurRadius);
	TArray<FCSStaticMeshTriangleRequest> Requests;
	BuildBoxSceneTriangleRequests(World, QueryBox, Requests);

	FCSTriangleMeshData LandscapeTriangleData;
	BuildBoxSceneLandscapeTrianglesInternal(
		World,
		QueryBox,
		ReferencePointsForRender,
		SafeFilterDistance,
		SafeMaxTriangles,
		LandscapeTriangleData);

	const bool bHasLandscapeTriangles = GetTriangleMeshDataTriangleCount(LandscapeTriangleData) > 0;
	if (Requests.IsEmpty() && !bHasLandscapeTriangles)
	{
		return;
	}

	TArray<FResolvedStaticMeshTriangleRequest> ResolvedRequests;
	const uint64 TotalStaticMeshTriangleCount = ResolveStaticMeshTriangleRequests(
		Requests,
		this,
		ExcludedActorTag,
		true,
		ResolvedRequests);
	if (ResolvedRequests.IsEmpty() && !bHasLandscapeTriangles)
	{
		return;
	}

	const uint64 MaxVoxelReadbackBytes64 = uint64(SafeMaxVoxels) * sizeof(FVector4f);
	if (MaxVoxelReadbackBytes64 > uint64(TNumericLimits<uint32>::Max()))
	{
		UE_LOG(LogTemp, Warning, TEXT("[GetBoxSceneFilteredSurfaceVoxels] Readback request too large. MaxVoxels=%d"), SafeMaxVoxels);
		return;
	}

	const uint32 CounterReadbackBytes = sizeof(uint32);

	FRHIGPUBufferReadback* PositionReadback = new FRHIGPUBufferReadback(TEXT("FilteredSurfaceVoxels_PositionReadback"));
	FRHIGPUBufferReadback* NormalReadback = new FRHIGPUBufferReadback(TEXT("FilteredSurfaceVoxels_NormalReadback"));
	FRHIGPUBufferReadback* TargetPositionReadback = new FRHIGPUBufferReadback(TEXT("FilteredSurfaceVoxels_TargetPositionReadback"));
	FRHIGPUBufferReadback* CellReadback = new FRHIGPUBufferReadback(TEXT("FilteredSurfaceVoxels_CellReadback"));
	FRHIGPUBufferReadback* CounterReadback = new FRHIGPUBufferReadback(TEXT("FilteredSurfaceVoxels_CounterReadback"));
	bool bRenderWorkQueued = false;
	bool bHasGPUOutput = false;
	int32 VoxelCapacity = 0;
	uint32 ActualVoxelReadbackBytes = 0;
	uint32 ActualVoxelCellReadbackBytes = 0;

	ENQUEUE_RENDER_COMMAND(GetBoxSceneFilteredSurfaceVoxelsGPU)(
		[ResolvedRequests = MoveTemp(ResolvedRequests), TotalStaticMeshTriangleCount, LandscapeTriangleData = MoveTemp(LandscapeTriangleData),
		 PositionReadback, NormalReadback, TargetPositionReadback, CellReadback, CounterReadback, CounterReadbackBytes,
		 SafeMaxTriangles, SafeMaxVoxels, VoxelOrigin = QueryBox.Min, SafeVoxelSize,
		 SafeMaxVoxelCellsPerTriangle = FMath::Max(1, MaxVoxelCellsPerTriangle),
		 ReferencePointsForRender, SafeFilterDistance, SafeSurfaceVoxelBlurIterations, SafeSurfaceVoxelBlurRadius,
		 &bRenderWorkQueued, &bHasGPUOutput, &VoxelCapacity, &ActualVoxelReadbackBytes, &ActualVoxelCellReadbackBytes](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			const FCSTriangleMeshData* InitialTriangleData = GetTriangleMeshDataTriangleCount(LandscapeTriangleData) > 0 ? &LandscapeTriangleData : nullptr;

			FCSStaticMeshTriangleRDGOutput TriangleOutput = AddResolvedStaticMeshTrianglesToRDGInternal(
				GraphBuilder,
				RHICmdList,
				ResolvedRequests,
				TotalStaticMeshTriangleCount,
				ReferencePointsForRender,
				SafeFilterDistance,
				SafeMaxTriangles,
				InitialTriangleData,
				TEXT("CS.FilteredSurfaceVoxels.Triangles"));

			FCSSurfaceVoxelRDGOutput VoxelOutput = AddTriangleSurfaceVoxelsToRDGInternal(
				GraphBuilder,
				TriangleOutput,
				VoxelOrigin,
				SafeVoxelSize,
				0.0f,
				SafeMaxVoxels,
				0,
				SafeMaxVoxelCellsPerTriangle,
				SafeSurfaceVoxelBlurIterations,
				SafeSurfaceVoxelBlurRadius,
				TEXT("CS.FilteredSurfaceVoxels"));

			if (VoxelOutput.VoxelPositions
				&& VoxelOutput.VoxelNormals
				&& VoxelOutput.VoxelTargetPositions
				&& VoxelOutput.VoxelCells
				&& VoxelOutput.VoxelCounter)
			{
				VoxelCapacity = int32(VoxelOutput.MaxVoxels);
				ActualVoxelReadbackBytes = uint32(uint64(VoxelOutput.MaxVoxels) * sizeof(FVector4f));
				ActualVoxelCellReadbackBytes = uint32(uint64(VoxelOutput.MaxVoxels) * sizeof(FIntVector4));
				AddEnqueueCopyPass(GraphBuilder, PositionReadback, VoxelOutput.VoxelPositions, ActualVoxelReadbackBytes);
				AddEnqueueCopyPass(GraphBuilder, NormalReadback, VoxelOutput.VoxelNormals, ActualVoxelReadbackBytes);
				AddEnqueueCopyPass(GraphBuilder, TargetPositionReadback, VoxelOutput.VoxelTargetPositions, ActualVoxelReadbackBytes);
				AddEnqueueCopyPass(GraphBuilder, CellReadback, VoxelOutput.VoxelCells, ActualVoxelCellReadbackBytes);
				AddEnqueueCopyPass(GraphBuilder, CounterReadback, VoxelOutput.VoxelCounter, CounterReadbackBytes);
				bHasGPUOutput = true;
			}

			GraphBuilder.Execute();
			bRenderWorkQueued = true;
		});

	FlushRenderingCommands();

	if (!bRenderWorkQueued || !bHasGPUOutput)
	{
		delete PositionReadback;
		delete NormalReadback;
		delete TargetPositionReadback;
		delete CellReadback;
		delete CounterReadback;
		return;
	}

	TArray<FVector4f> PositionData;
	TArray<FVector4f> NormalData;
	TArray<FVector4f> TargetPositionData;
	TArray<FIntVector4> CellData;
	PositionData.SetNumZeroed(VoxelCapacity);
	NormalData.SetNumZeroed(VoxelCapacity);
	TargetPositionData.SetNumZeroed(VoxelCapacity);
	CellData.SetNumZeroed(VoxelCapacity);
	uint32 VoxelCount = 0;
	bool bReadbackSucceeded = false;

	ENQUEUE_RENDER_COMMAND(GetBoxSceneFilteredSurfaceVoxelsReadback)(
		[PositionReadback, NormalReadback, TargetPositionReadback, CellReadback, CounterReadback, ActualVoxelReadbackBytes, ActualVoxelCellReadbackBytes, CounterReadbackBytes,
		 &PositionData, &NormalData, &TargetPositionData, &CellData, &VoxelCount, &bReadbackSucceeded](FRHICommandListImmediate& RHICmdList)
		{
			if (!PositionReadback || !NormalReadback || !TargetPositionReadback || !CellReadback || !CounterReadback)
			{
				return;
			}

			if (!PositionReadback->IsReady()
				|| !NormalReadback->IsReady()
				|| !TargetPositionReadback->IsReady()
				|| !CellReadback->IsReady()
				|| !CounterReadback->IsReady())
			{
				RHICmdList.SubmitAndBlockUntilGPUIdle();
			}

			if (!PositionReadback->IsReady()
				|| !NormalReadback->IsReady()
				|| !TargetPositionReadback->IsReady()
				|| !CellReadback->IsReady()
				|| !CounterReadback->IsReady())
			{
				UE_LOG(LogTemp, Warning, TEXT("[GetBoxSceneFilteredSurfaceVoxels] GPU readback was not ready after flush."));
				delete PositionReadback;
				delete NormalReadback;
				delete TargetPositionReadback;
				delete CellReadback;
				delete CounterReadback;
				return;
			}

			if (PositionReadback->GetGPUSizeBytes() < ActualVoxelReadbackBytes ||
				NormalReadback->GetGPUSizeBytes() < ActualVoxelReadbackBytes ||
				TargetPositionReadback->GetGPUSizeBytes() < ActualVoxelReadbackBytes ||
				CellReadback->GetGPUSizeBytes() < ActualVoxelCellReadbackBytes ||
				CounterReadback->GetGPUSizeBytes() < CounterReadbackBytes)
			{
				UE_LOG(LogTemp, Warning, TEXT("[GetBoxSceneFilteredSurfaceVoxels] GPU readback size mismatch. Position=%llu/%u Normal=%llu/%u Target=%llu/%u Cell=%llu/%u Counter=%llu/%u"),
					PositionReadback->GetGPUSizeBytes(),
					ActualVoxelReadbackBytes,
					NormalReadback->GetGPUSizeBytes(),
					ActualVoxelReadbackBytes,
					TargetPositionReadback->GetGPUSizeBytes(),
					ActualVoxelReadbackBytes,
					CellReadback->GetGPUSizeBytes(),
					ActualVoxelCellReadbackBytes,
					CounterReadback->GetGPUSizeBytes(),
					CounterReadbackBytes);
				delete PositionReadback;
				delete NormalReadback;
				delete TargetPositionReadback;
				delete CellReadback;
				delete CounterReadback;
				return;
			}

			bool bLockedAll = true;
			if (const FVector4f* PositionPtr = static_cast<const FVector4f*>(PositionReadback->Lock(ActualVoxelReadbackBytes)))
			{
				FMemory::Memcpy(PositionData.GetData(), PositionPtr, ActualVoxelReadbackBytes);
				PositionReadback->Unlock();
			}
			else
			{
				bLockedAll = false;
			}

			if (const FVector4f* NormalPtr = static_cast<const FVector4f*>(NormalReadback->Lock(ActualVoxelReadbackBytes)))
			{
				FMemory::Memcpy(NormalData.GetData(), NormalPtr, ActualVoxelReadbackBytes);
				NormalReadback->Unlock();
			}
			else
			{
				bLockedAll = false;
			}

			if (const FVector4f* TargetPositionPtr = static_cast<const FVector4f*>(TargetPositionReadback->Lock(ActualVoxelReadbackBytes)))
			{
				FMemory::Memcpy(TargetPositionData.GetData(), TargetPositionPtr, ActualVoxelReadbackBytes);
				TargetPositionReadback->Unlock();
			}
			else
			{
				bLockedAll = false;
			}

			if (const FIntVector4* CellPtr = static_cast<const FIntVector4*>(CellReadback->Lock(ActualVoxelCellReadbackBytes)))
			{
				FMemory::Memcpy(CellData.GetData(), CellPtr, ActualVoxelCellReadbackBytes);
				CellReadback->Unlock();
			}
			else
			{
				bLockedAll = false;
			}

			if (const uint32* CounterPtr = static_cast<const uint32*>(CounterReadback->Lock(CounterReadbackBytes)))
			{
				VoxelCount = *CounterPtr;
				CounterReadback->Unlock();
			}
			else
			{
				bLockedAll = false;
			}

			delete PositionReadback;
			delete NormalReadback;
			delete TargetPositionReadback;
			delete CellReadback;
			delete CounterReadback;
			bReadbackSucceeded = bLockedAll;
		});

	FlushRenderingCommands();

	if (!bReadbackSucceeded)
	{
		return;
	}

	const int32 EffectiveVoxelCount = FMath::Clamp<int32>(int32(VoxelCount), 0, PositionData.Num());
	if (EffectiveVoxelCount <= 0)
	{
		return;
	}

	OutPositions.Reserve(EffectiveVoxelCount);
	OutNormals.Reserve(EffectiveVoxelCount);
	FCSSurfaceVoxelData FilteredVoxelData;
	FilteredVoxelData.Positions.Reserve(EffectiveVoxelCount);
	FilteredVoxelData.Normals.Reserve(EffectiveVoxelCount);
	FilteredVoxelData.TargetPositions.Reserve(EffectiveVoxelCount);
	FilteredVoxelData.Cells.Reserve(EffectiveVoxelCount);
	FilteredVoxelData.VoxelSize = SafeVoxelSize;
	FilteredVoxelData.VoxelOrigin = QueryBox.Min;
	int32 InvalidPositionCount = 0;
	int32 InvalidNormalCount = 0;
	int32 InvalidTargetCount = 0;
	int32 InvalidTargetVectorCount = 0;
	int32 InvalidTargetWCount = 0;
	int32 FarTargetCount = 0;
	const double MaxTargetDistanceSq = FMath::Square(double(FMath::Max(SafeVoxelSize * 2.0f, UE_KINDA_SMALL_NUMBER)));
	for (int32 VoxelIndex = 0; VoxelIndex < EffectiveVoxelCount; ++VoxelIndex)
	{
		const FVector4f& Position = PositionData[VoxelIndex];
		const FVector4f& Normal = NormalData[VoxelIndex];
		const FVector4f& TargetPosition = TargetPositionData[VoxelIndex];
		const FIntVector4& Cell = CellData[VoxelIndex];

		if (!IsFiniteCSVector4(Position) || Position.W <= 0.0f)
		{
			++InvalidPositionCount;
			continue;
		}

		const FVector VoxelCenter(Position.X, Position.Y, Position.Z);
		FVector SafeNormal(Normal.X, Normal.Y, Normal.Z);
		if (!IsFiniteCSVector4(Normal) || !SafeNormal.Normalize())
		{
			SafeNormal = FVector::UpVector;
			++InvalidNormalCount;
		}

		FVector SafeTarget(TargetPosition.X, TargetPosition.Y, TargetPosition.Z);
		const bool bFiniteTarget = IsFiniteCSVector4(TargetPosition);
		const bool bPositiveTargetW = TargetPosition.W > 0.0f;
		const bool bTargetWithinVoxel = bFiniteTarget
			&& FVector::DistSquared(SafeTarget, VoxelCenter) <= MaxTargetDistanceSq;
		const bool bValidTarget = bFiniteTarget && bPositiveTargetW && bTargetWithinVoxel;
		if (!bValidTarget)
		{
			if (!bFiniteTarget)
			{
				++InvalidTargetVectorCount;
			}
			else if (!bPositiveTargetW)
			{
				++InvalidTargetWCount;
			}
			else
			{
				++FarTargetCount;
			}
			SafeTarget = VoxelCenter;
			++InvalidTargetCount;
		}

		FilteredVoxelData.Positions.Add(VoxelCenter);
		FilteredVoxelData.Normals.Add(SafeNormal);
		FilteredVoxelData.TargetPositions.Add(SafeTarget);
		FilteredVoxelData.Cells.Add(FIntVector(Cell.X, Cell.Y, Cell.Z));
		OutPositions.Add(VoxelCenter);
		OutNormals.Add(SafeNormal);
	}
	FilteredVoxelData.VoxelCount = FilteredVoxelData.Positions.Num();
	if (InvalidPositionCount > 0 || InvalidNormalCount > 0 || InvalidTargetCount > 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[GetBoxSceneFilteredSurfaceVoxels] Sanitized surface voxel readback. Input=%d Output=%d InvalidPositions=%d InvalidNormals=%d InvalidTargets=%d InvalidTargetVectors=%d InvalidTargetW=%d FarTargets=%d"),
			EffectiveVoxelCount,
			FilteredVoxelData.VoxelCount,
			InvalidPositionCount,
			InvalidNormalCount,
			InvalidTargetCount,
			InvalidTargetVectorCount,
			InvalidTargetWCount,
			FarTargetCount);
	}

	// Update cached data
	LastSurfaceVoxelData = FilteredVoxelData;
	StoreSurfaceVoxelTextureData(LastSurfaceVoxelData, QueryBox.Min);
}

// -----------------------------------------------------------------------------
// Core System - Generated Data Cache
// -----------------------------------------------------------------------------

FCSMeshGeneratorTriangleTextureDataHandle AComputeShaderMeshGenerator::GetLastTriangleTextureData() const
{
	return LastTriangleTextureData;
}

FCSMeshGeneratorSurfaceVoxelTextureDataHandle AComputeShaderMeshGenerator::GetLastSurfaceVoxelTextureData() const
{
	return LastSurfaceVoxelTextureData;
}

FCSMeshGeneratorTriangleTextureDataHandle AComputeShaderMeshGenerator::UpdateBoxSceneTriangleTextureData(float ReferenceFilterDistance)
{
	GetBoxSceneTrianglesFromGPUFiltered(ReferenceFilterDistance);
	return LastTriangleTextureData;
}

FCSMeshGeneratorSurfaceVoxelTextureDataHandle AComputeShaderMeshGenerator::UpdateBoxSceneSurfaceVoxelTextureData(float VoxelSize)
{
	GetBoxSceneSurfaceVoxelsFromGPU(VoxelSize);
	return LastSurfaceVoxelTextureData;
}

void AComputeShaderMeshGenerator::ClearGeneratedDataTextureCache()
{
	ClearTriangleTextureData();
	ClearSurfaceVoxelTextureData();
}

int32 AComputeShaderMeshGenerator::DrawDebugDirectionArray(
	const TArray<FVector>& Positions,
	const TArray<FVector>& Directions,
	float DirectionLength,
	FLinearColor DirectionColor,
	float Duration,
	float Thickness,
	bool bPersistentLines,
	bool bDrawPoints,
	FLinearColor PointColor,
	float PointSize,
	int32 MaxDirectionsToDraw) const
{
	UWorld* World = GetWorld();
	if (!World || Positions.IsEmpty() || Directions.IsEmpty())
	{
		return 0;
	}

	const int32 AvailableCount = FMath::Min(Positions.Num(), Directions.Num());
	const int32 DrawLimit = MaxDirectionsToDraw > 0
		? FMath::Min(MaxDirectionsToDraw, AvailableCount)
		: AvailableCount;
	if (DrawLimit <= 0)
	{
		return 0;
	}

	const float SafeDirectionLength = FMath::Max(0.0f, DirectionLength);
	const float SafeDuration = FMath::Max(0.0f, Duration);
	const float SafeThickness = FMath::Max(0.0f, Thickness);
	const float SafePointSize = FMath::Max(0.0f, PointSize);
	const float ArrowHeadSize = FMath::Max(SafeDirectionLength * 0.15f, SafeThickness * 4.0f);
	const FColor DirectionDrawColor = DirectionColor.ToFColor(true);
	const FColor PointDrawColor = PointColor.ToFColor(true);

	int32 DrawnCount = 0;
	for (int32 Index = 0; Index < DrawLimit; ++Index)
	{
		const FVector& Position = Positions[Index];
		if (!IsFiniteCSVertex(Position))
		{
			continue;
		}

		FVector Direction = Directions[Index];
		if (!Direction.Normalize())
		{
			continue;
		}

		const FVector EndPosition = Position + Direction * SafeDirectionLength;
		DrawDebugDirectionalArrow(
			World,
			Position,
			EndPosition,
			ArrowHeadSize,
			DirectionDrawColor,
			bPersistentLines,
			SafeDuration,
			0,
			SafeThickness);

		if (bDrawPoints && SafePointSize > 0.0f)
		{
			DrawDebugPoint(
				World,
				Position,
				SafePointSize,
				PointDrawColor,
				bPersistentLines,
				SafeDuration,
				0);
		}

		++DrawnCount;
	}

	return DrawnCount;
}



int32 AComputeShaderMeshGenerator::DrawDebugLastSurfaceVoxelDirections(
	float DirectionLength,
	FLinearColor DirectionColor,
	float Duration,
	float Thickness,
	bool bPersistentLines,
	bool bDrawPoints,
	FLinearColor PointColor,
	float PointSize,
	int32 MaxDirectionsToDraw) const
{
	const float EffectiveDirectionLength = DirectionLength > 0.0f
		? DirectionLength
		: FMath::Max(LastSurfaceVoxelData.VoxelSize, UE_KINDA_SMALL_NUMBER);
	return DrawDebugDirectionArray(
		LastSurfaceVoxelData.Positions,
		LastSurfaceVoxelData.Normals,
		EffectiveDirectionLength,
		DirectionColor,
		Duration,
		Thickness,
		bPersistentLines,
		bDrawPoints,
		PointColor,
		PointSize,
		MaxDirectionsToDraw);
}

int32 AComputeShaderMeshGenerator::DrawDebugLastSurfaceVoxelArrows(
	float ArrowLength,
	FLinearColor ArrowColor,
	float Duration,
	float Thickness,
	bool bPersistentLines,
	bool bDrawPoints,
	FLinearColor PointColor,
	float PointSize,
	int32 MaxArrowsToDraw) const
{
	return DrawDebugLastSurfaceVoxelDirections(
		ArrowLength,
		ArrowColor,
		Duration,
		Thickness,
		bPersistentLines,
		bDrawPoints,
		PointColor,
		PointSize,
		MaxArrowsToDraw);
}

int32 AComputeShaderMeshGenerator::DrawDebugBoxSceneSurfaceVoxelDirections(
	float VoxelSize,
	float DirectionLength,
	FLinearColor DirectionColor,
	float Duration,
	float Thickness,
	bool bPersistentLines,
	bool bDrawPoints,
	FLinearColor PointColor,
	float PointSize,
	int32 MaxDirectionsToDraw)
{
	const FCSSurfaceVoxelData SurfaceVoxels = GetBoxSceneSurfaceVoxelsFromGPU(VoxelSize);
	const float EffectiveDirectionLength = DirectionLength > 0.0f
		? DirectionLength
		: FMath::Max(SurfaceVoxels.VoxelSize, UE_KINDA_SMALL_NUMBER);
	return DrawDebugDirectionArray(
		SurfaceVoxels.Positions,
		SurfaceVoxels.Normals,
		EffectiveDirectionLength,
		DirectionColor,
		Duration,
		Thickness,
		bPersistentLines,
		bDrawPoints,
		PointColor,
		PointSize,
		MaxDirectionsToDraw);
}

int32 AComputeShaderMeshGenerator::DrawDebugBoxSceneSurfaceVoxelArrows(
	float VoxelSize,
	float ArrowLength,
	FLinearColor ArrowColor,
	float Duration,
	float Thickness,
	bool bPersistentLines,
	bool bDrawPoints,
	FLinearColor PointColor,
	float PointSize,
	int32 MaxArrowsToDraw)
{
	return DrawDebugBoxSceneSurfaceVoxelDirections(
		VoxelSize,
		ArrowLength,
		ArrowColor,
		Duration,
		Thickness,
		bPersistentLines,
		bDrawPoints,
		PointColor,
		PointSize,
		MaxArrowsToDraw);
}

UTextureRenderTarget2D* AComputeShaderMeshGenerator::GetOrCreateGeneratedDataRenderTarget(
	TObjectPtr<UTextureRenderTarget2D>& RenderTarget,
	const TCHAR* BaseName,
	int32 Width,
	int32 Height)
{
	const int32 SafeWidth = FMath::Max(CSGeneratorMinTextureDimension, Width);
	const int32 SafeHeight = FMath::Max(CSGeneratorMinTextureDimension, Height);
	const bool bNeedsCreate = !RenderTarget
		|| RenderTarget->SizeX != SafeWidth
		|| RenderTarget->SizeY != SafeHeight
		|| RenderTarget->RenderTargetFormat != RTF_RGBA32f
		|| !RenderTarget->bCanCreateUAV;

	if (!bNeedsCreate)
	{
		return RenderTarget.Get();
	}

	if (RenderTarget)
	{
		RenderTarget->ReleaseResource();
		RenderTarget = nullptr;
	}

	RenderTarget = NewObject<UTextureRenderTarget2D>(
		this,
		MakeUniqueObjectName(this, UTextureRenderTarget2D::StaticClass(), BaseName),
		RF_Transient);
	if (!RenderTarget)
	{
		return nullptr;
	}

	RenderTarget->RenderTargetFormat = RTF_RGBA32f;
	RenderTarget->ClearColor = FLinearColor::Black;
	RenderTarget->bCanCreateUAV = true;
	RenderTarget->InitAutoFormat(SafeWidth, SafeHeight);
	RenderTarget->UpdateResourceImmediate(true);
	return RenderTarget.Get();
}

void AComputeShaderMeshGenerator::StoreTriangleTextureData(const FCSTriangleMeshData& TriangleData, float ReferenceFilterDistance, FBox SourceWorldBounds)
{
	const int32 EffectiveVertexCount = TriangleData.VertexCount >= 0
		? FMath::Clamp(TriangleData.VertexCount, 0, TriangleData.Vertices.Num())
		: TriangleData.Vertices.Num();
	const int32 EffectiveIndexCount = TriangleData.IndexCount >= 0
		? FMath::Clamp(TriangleData.IndexCount, 0, TriangleData.Indices.Num())
		: TriangleData.Indices.Num();
	const int32 TriangleCount = EffectiveIndexCount >= 3
		? EffectiveIndexCount / 3
		: EffectiveVertexCount / 3;
	if (EffectiveVertexCount <= 0 || TriangleCount <= 0)
	{
		ClearTriangleTextureData();
		return;
	}

	const int32 MaxTextureDimension = FMath::Max(CSGeneratorMinTextureDimension, VoxelGridSettings.MaxCacheTextureDimension);
	const FIntPoint VertexTextureSize = GetLinearDataTextureSize(EffectiveVertexCount, MaxTextureDimension);
	UTextureRenderTarget2D* VertexRT = GetOrCreateGeneratedDataRenderTarget(
		LastTriangleTextureData.TriangleVertexRT,
		TEXT("CSMeshGenerator_LastTriangleVertexRT"),
		VertexTextureSize.X,
		VertexTextureSize.Y);
	UTextureRenderTarget2D* NormalRT = GetOrCreateGeneratedDataRenderTarget(
		LastTriangleTextureData.TriangleNormalRT,
		TEXT("CSMeshGenerator_LastTriangleNormalRT"),
		VertexTextureSize.X,
		VertexTextureSize.Y);
	UTextureRenderTarget2D* MetaRT = GetOrCreateGeneratedDataRenderTarget(
		LastTriangleTextureData.TriangleMetaRT,
		TEXT("CSMeshGenerator_LastTriangleMetaRT"),
		8,
		1);
	if (!VertexRT || !NormalRT || !MetaRT)
	{
		ClearTriangleTextureData();
		return;
	}

	const int32 TexturePixelCount = VertexTextureSize.X * VertexTextureSize.Y;
	TArray<FLinearColor> VertexPixels;
	VertexPixels.SetNumZeroed(TexturePixelCount);
	TArray<FLinearColor> NormalPixels;
	NormalPixels.SetNumZeroed(TexturePixelCount);

	for (int32 VertexIndex = 0; VertexIndex < EffectiveVertexCount; ++VertexIndex)
	{
		const FVector& Vertex = TriangleData.Vertices[VertexIndex];
		VertexPixels[VertexIndex] = FLinearColor(float(Vertex.X), float(Vertex.Y), float(Vertex.Z), 1.0f);

		FVector Normal = TriangleData.VertexNormals.IsValidIndex(VertexIndex)
			? TriangleData.VertexNormals[VertexIndex]
			: FVector::ZeroVector;
		if (!Normal.Normalize())
		{
			const int32 TriVertexBase = (VertexIndex / 3) * 3;
			if (TriangleData.Vertices.IsValidIndex(TriVertexBase + 2))
			{
				Normal = FVector::CrossProduct(
					TriangleData.Vertices[TriVertexBase + 1] - TriangleData.Vertices[TriVertexBase + 0],
					TriangleData.Vertices[TriVertexBase + 2] - TriangleData.Vertices[TriVertexBase + 0]).GetSafeNormal();
			}
		}
		NormalPixels[VertexIndex] = FLinearColor(float(Normal.X), float(Normal.Y), float(Normal.Z), 0.0f);
	}

	const FBox SourceBounds = SourceWorldBounds.IsValid ? SourceWorldBounds : GetGeneratorBoundsWorldBox();
	TArray<FLinearColor> MetaPixels;
	MetaPixels.SetNumZeroed(8);
	MetaPixels[0] = FLinearColor(float(TriangleCount), float(EffectiveVertexCount), float(EffectiveIndexCount), ReferenceFilterDistance);
	MetaPixels[1] = SourceBounds.IsValid
		? FLinearColor(float(SourceBounds.Min.X), float(SourceBounds.Min.Y), float(SourceBounds.Min.Z), 1.0f)
		: FLinearColor::Black;
	MetaPixels[2] = SourceBounds.IsValid
		? FLinearColor(float(SourceBounds.Max.X), float(SourceBounds.Max.Y), float(SourceBounds.Max.Z), 1.0f)
		: FLinearColor::Black;
	MetaPixels[3] = FLinearColor(float(VertexTextureSize.X), float(VertexTextureSize.Y), float(MaxTextureDimension), 0.0f);

	UploadLinearColorsToRenderTarget(VertexRT, MoveTemp(VertexPixels));
	UploadLinearColorsToRenderTarget(NormalRT, MoveTemp(NormalPixels));
	UploadLinearColorsToRenderTarget(MetaRT, MoveTemp(MetaPixels));

	LastTriangleTextureData.bValid = true;
	LastTriangleTextureData.VertexCount = EffectiveVertexCount;
	LastTriangleTextureData.TriangleCount = TriangleCount;
	LastTriangleTextureData.IndexCount = EffectiveIndexCount;
	LastTriangleTextureData.ReferenceFilterDistance = ReferenceFilterDistance;
	LastTriangleTextureData.SourceWorldBounds = SourceBounds;
}

void AComputeShaderMeshGenerator::StoreSurfaceVoxelTextureData(const FCSSurfaceVoxelData& SurfaceVoxelData, FVector VoxelOrigin)
{
	const int32 EffectiveVoxelCount = SurfaceVoxelData.VoxelCount >= 0
		? FMath::Clamp(SurfaceVoxelData.VoxelCount, 0, SurfaceVoxelData.Positions.Num())
		: SurfaceVoxelData.Positions.Num();
	if (EffectiveVoxelCount <= 0)
	{
		ClearSurfaceVoxelTextureData();
		LastSurfaceVoxelData.VoxelSize = SurfaceVoxelData.VoxelSize;
		LastSurfaceVoxelTextureData.VoxelSize = SurfaceVoxelData.VoxelSize;
		return;
	}

	const int32 MaxTextureDimension = FMath::Max(CSGeneratorMinTextureDimension, VoxelGridSettings.MaxCacheTextureDimension);
	const FIntPoint VoxelTextureSize = GetLinearDataTextureSize(EffectiveVoxelCount, MaxTextureDimension);
	UTextureRenderTarget2D* PositionRT = GetOrCreateGeneratedDataRenderTarget(
		LastSurfaceVoxelTextureData.VoxelPositionRT,
		TEXT("CSMeshGenerator_LastSurfaceVoxelPositionRT"),
		VoxelTextureSize.X,
		VoxelTextureSize.Y);
	UTextureRenderTarget2D* NormalRT = GetOrCreateGeneratedDataRenderTarget(
		LastSurfaceVoxelTextureData.VoxelNormalRT,
		TEXT("CSMeshGenerator_LastSurfaceVoxelNormalRT"),
		VoxelTextureSize.X,
		VoxelTextureSize.Y);
	UTextureRenderTarget2D* TargetRT = GetOrCreateGeneratedDataRenderTarget(
		LastSurfaceVoxelTextureData.VoxelTargetRT,
		TEXT("CSMeshGenerator_LastSurfaceVoxelTargetRT"),
		VoxelTextureSize.X,
		VoxelTextureSize.Y);
	UTextureRenderTarget2D* CellRT = GetOrCreateGeneratedDataRenderTarget(
		LastSurfaceVoxelTextureData.VoxelCellRT,
		TEXT("CSMeshGenerator_LastSurfaceVoxelCellRT"),
		VoxelTextureSize.X,
		VoxelTextureSize.Y);
	UTextureRenderTarget2D* MetaRT = GetOrCreateGeneratedDataRenderTarget(
		LastSurfaceVoxelTextureData.VoxelMetaRT,
		TEXT("CSMeshGenerator_LastSurfaceVoxelMetaRT"),
		8,
		1);
	if (!PositionRT || !NormalRT || !TargetRT || !CellRT || !MetaRT)
	{
		ClearSurfaceVoxelTextureData();
		return;
	}

	const int32 TexturePixelCount = VoxelTextureSize.X * VoxelTextureSize.Y;
	TArray<FLinearColor> PositionPixels;
	PositionPixels.SetNumZeroed(TexturePixelCount);
	TArray<FLinearColor> NormalPixels;
	NormalPixels.SetNumZeroed(TexturePixelCount);
	TArray<FLinearColor> TargetPixels;
	TargetPixels.SetNumZeroed(TexturePixelCount);
	TArray<FLinearColor> CellPixels;
	CellPixels.SetNumZeroed(TexturePixelCount);
	for (int32 VoxelIndex = 0; VoxelIndex < EffectiveVoxelCount; ++VoxelIndex)
	{
		const FVector& Position = SurfaceVoxelData.Positions[VoxelIndex];
		PositionPixels[VoxelIndex] = FLinearColor(float(Position.X), float(Position.Y), float(Position.Z), 1.0f);

		FVector Normal = SurfaceVoxelData.Normals.IsValidIndex(VoxelIndex)
			? SurfaceVoxelData.Normals[VoxelIndex]
			: FVector::UpVector;
		Normal = Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
		NormalPixels[VoxelIndex] = FLinearColor(float(Normal.X), float(Normal.Y), float(Normal.Z), 0.0f);

		FVector Target = SurfaceVoxelData.TargetPositions.IsValidIndex(VoxelIndex)
			? SurfaceVoxelData.TargetPositions[VoxelIndex]
			: Position;
		if (!IsFiniteCSVertex(Target))
		{
			Target = Position;
		}
		TargetPixels[VoxelIndex] = FLinearColor(float(Target.X), float(Target.Y), float(Target.Z), 1.0f);

		FIntVector Cell = SurfaceVoxelData.Cells.IsValidIndex(VoxelIndex)
			? SurfaceVoxelData.Cells[VoxelIndex]
			: FIntVector(
				FMath::FloorToInt((Position.X - VoxelOrigin.X) / FMath::Max(SurfaceVoxelData.VoxelSize, UE_KINDA_SMALL_NUMBER)),
				FMath::FloorToInt((Position.Y - VoxelOrigin.Y) / FMath::Max(SurfaceVoxelData.VoxelSize, UE_KINDA_SMALL_NUMBER)),
				FMath::FloorToInt((Position.Z - VoxelOrigin.Z) / FMath::Max(SurfaceVoxelData.VoxelSize, UE_KINDA_SMALL_NUMBER)));
		CellPixels[VoxelIndex] = FLinearColor(float(Cell.X), float(Cell.Y), float(Cell.Z), 0.0f);
	}

	const FBox SourceBounds = GetGeneratorBoundsWorldBox();
	TArray<FLinearColor> MetaPixels;
	MetaPixels.SetNumZeroed(8);
	MetaPixels[0] = FLinearColor(float(EffectiveVoxelCount), SurfaceVoxelData.VoxelSize, float(VoxelTextureSize.X), float(VoxelTextureSize.Y));
	MetaPixels[1] = FLinearColor(float(VoxelOrigin.X), float(VoxelOrigin.Y), float(VoxelOrigin.Z), 1.0f);
	MetaPixels[2] = SourceBounds.IsValid
		? FLinearColor(float(SourceBounds.Min.X), float(SourceBounds.Min.Y), float(SourceBounds.Min.Z), 1.0f)
		: FLinearColor::Black;
	MetaPixels[3] = SourceBounds.IsValid
		? FLinearColor(float(SourceBounds.Max.X), float(SourceBounds.Max.Y), float(SourceBounds.Max.Z), 1.0f)
		: FLinearColor::Black;
	MetaPixels[4] = FLinearColor(float(VoxelTextureSize.X), float(VoxelTextureSize.Y), 1.0f, 0.0f);

	UploadLinearColorsToRenderTarget(PositionRT, MoveTemp(PositionPixels));
	UploadLinearColorsToRenderTarget(NormalRT, MoveTemp(NormalPixels));
	UploadLinearColorsToRenderTarget(TargetRT, MoveTemp(TargetPixels));
	UploadLinearColorsToRenderTarget(CellRT, MoveTemp(CellPixels));
	UploadLinearColorsToRenderTarget(MetaRT, MoveTemp(MetaPixels));

	LastSurfaceVoxelTextureData.bValid = true;
	LastSurfaceVoxelTextureData.VoxelCount = EffectiveVoxelCount;
	LastSurfaceVoxelTextureData.VoxelSize = SurfaceVoxelData.VoxelSize;
	LastSurfaceVoxelTextureData.VoxelOrigin = VoxelOrigin;
	LastSurfaceVoxelTextureData.SourceWorldBounds = SourceBounds;
}

void AComputeShaderMeshGenerator::ClearTriangleTextureData()
{
	if (LastTriangleTextureData.TriangleVertexRT)
	{
		LastTriangleTextureData.TriangleVertexRT->ReleaseResource();
	}
	if (LastTriangleTextureData.TriangleNormalRT)
	{
		LastTriangleTextureData.TriangleNormalRT->ReleaseResource();
	}
	if (LastTriangleTextureData.TriangleMetaRT)
	{
		LastTriangleTextureData.TriangleMetaRT->ReleaseResource();
	}
	LastTriangleTextureData = FCSMeshGeneratorTriangleTextureDataHandle();
}

void AComputeShaderMeshGenerator::ClearSurfaceVoxelTextureData()
{
	if (LastSurfaceVoxelTextureData.VoxelPositionRT)
	{
		LastSurfaceVoxelTextureData.VoxelPositionRT->ReleaseResource();
	}
	if (LastSurfaceVoxelTextureData.VoxelNormalRT)
	{
		LastSurfaceVoxelTextureData.VoxelNormalRT->ReleaseResource();
	}
	if (LastSurfaceVoxelTextureData.VoxelTargetRT)
	{
		LastSurfaceVoxelTextureData.VoxelTargetRT->ReleaseResource();
	}
	if (LastSurfaceVoxelTextureData.VoxelCellRT)
	{
		LastSurfaceVoxelTextureData.VoxelCellRT->ReleaseResource();
	}
	if (LastSurfaceVoxelTextureData.VoxelMetaRT)
	{
		LastSurfaceVoxelTextureData.VoxelMetaRT->ReleaseResource();
	}
	LastSurfaceVoxelTextureData = FCSMeshGeneratorSurfaceVoxelTextureDataHandle();
	LastSurfaceVoxelData = FCSSurfaceVoxelData();
}

// -----------------------------------------------------------------------------
// Debug System - Dynamic Mesh Output
// -----------------------------------------------------------------------------

UDynamicMesh* AComputeShaderMeshGenerator::SurfaceVoxelsToIsolatedQuadsDebug(float VoxelSize,
	bool bReverseOrientation)
{
	const FCSSurfaceVoxelData SurfaceVoxels = GetBoxSceneSurfaceVoxelsFromGPU(VoxelSize);

	UDynamicMesh* OutMesh = CreateEmptyDynamicMesh();
	if (!OutMesh)
	{
		return nullptr;
	}

	const int32 EffectiveVoxelCount = SurfaceVoxels.VoxelCount >= 0
		? FMath::Clamp(SurfaceVoxels.VoxelCount, 0, SurfaceVoxels.Positions.Num())
		: SurfaceVoxels.Positions.Num();
	if (EffectiveVoxelCount <= 0)
	{
		UE_LOG(LogTemp, Log, TEXT("[SurfaceVoxelDebug] IsolatedQuads: Empty EffectiveVoxelCount=%d"), EffectiveVoxelCount);
		return OutMesh;
	}

	const float BaseVoxelSize = VoxelSize > 0.0f ? VoxelSize : SurfaceVoxels.VoxelSize;
	const float SafeVoxelSize = FMath::Max(BaseVoxelSize, UE_KINDA_SMALL_NUMBER);
	const float HalfQuadSize = SafeVoxelSize * FMath::Max(QuadScale, UE_KINDA_SMALL_NUMBER) * 0.5f;

	UE::Geometry::FDynamicMesh3 Mesh;
	Mesh.EnableVertexNormals(FVector3f::UpVector);

	int32 AddedPatches = 0;
	int32 AddedTriangles = 0;
	int32 InvalidPositionCount = 0;
	int32 InvalidNormalCount = 0;

	for (int32 VoxelIndex = 0; VoxelIndex < EffectiveVoxelCount; ++VoxelIndex)
	{
		const FVector& Position = SurfaceVoxels.Positions[VoxelIndex];
		if (!IsFiniteCSVertex(Position))
		{
			++InvalidPositionCount;
			continue;
		}

		FVector Normal = SurfaceVoxels.Normals.IsValidIndex(VoxelIndex)
			? SurfaceVoxels.Normals[VoxelIndex]
			: FVector::UpVector;
		if (Normal.ContainsNaN())
		{
			++InvalidNormalCount;
			continue;
		}
		Normal = Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);

		const FVector HelperAxis = FMath::Abs(Normal.Z) < 0.99 ? FVector::UpVector : FVector::RightVector;
		const FVector AxisX = FVector::CrossProduct(HelperAxis, Normal).GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);
		const FVector AxisY = FVector::CrossProduct(Normal, AxisX).GetSafeNormal(UE_SMALL_NUMBER, FVector::RightVector);
		const FVector Center = Position + Normal * (SafeVoxelSize * NormalOffsetScale);
		const FVector DX = AxisX * HalfQuadSize;
		const FVector DY = AxisY * HalfQuadSize;

		const int32 A = Mesh.AppendVertex(FVector3d(Center - DX - DY));
		const int32 B = Mesh.AppendVertex(FVector3d(Center + DX - DY));
		const int32 C = Mesh.AppendVertex(FVector3d(Center + DX + DY));
		const int32 D = Mesh.AppendVertex(FVector3d(Center - DX + DY));
		Mesh.SetVertexNormal(A, FVector3f(Normal));
		Mesh.SetVertexNormal(B, FVector3f(Normal));
		Mesh.SetVertexNormal(C, FVector3f(Normal));
		Mesh.SetVertexNormal(D, FVector3f(Normal));

		const int32 T0 = bReverseOrientation
			? Mesh.AppendTriangle(UE::Geometry::FIndex3i(A, C, B), 0)
			: Mesh.AppendTriangle(UE::Geometry::FIndex3i(A, B, C), 0);
		const int32 T1 = bReverseOrientation
			? Mesh.AppendTriangle(UE::Geometry::FIndex3i(A, D, C), 0)
			: Mesh.AppendTriangle(UE::Geometry::FIndex3i(A, C, D), 0);
		if (T0 >= 0)
		{
			++AddedTriangles;
		}
		if (T1 >= 0)
		{
			++AddedTriangles;
		}
		++AddedPatches;
	}

	UE_LOG(LogTemp, Log, TEXT("[SurfaceVoxelDebug] IsolatedQuads: Input=%d AddedPatches=%d AddedTriangles=%d InvalidPositions=%d InvalidNormals=%d MeshVertices=%d MeshTriangles=%d"),
		EffectiveVoxelCount,
		AddedPatches,
		AddedTriangles,
		InvalidPositionCount,
		InvalidNormalCount,
		Mesh.VertexCount(),
		Mesh.TriangleCount());

	if (AddedTriangles <= 0)
	{
		return OutMesh;
	}

	OutMesh->SetMesh(MoveTemp(Mesh));
	FGeometryScriptCalculateNormalsOptions CalculateOptions;
	UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(OutMesh, CalculateOptions);
	SetGeneratedDynamicMesh(OutMesh);
	return OutMesh;
}

// -----------------------------------------------------------------------------
// Core System - Dynamic Mesh Output
// -----------------------------------------------------------------------------

UDynamicMesh* AComputeShaderMeshGenerator::SurfaceVoxelsToOpenDynamicMesh(float VoxelSize,
	bool bReverseOrientation,
	bool bRecomputeNormals)
{
	const FCSSurfaceVoxelData SurfaceVoxels = GetBoxSceneSurfaceVoxelsFromGPU(VoxelSize);

	UDynamicMesh* OutMesh = CreateEmptyDynamicMesh();
	if (!OutMesh)
	{
		return nullptr;
	}

	const int32 EffectiveVoxelCount = SurfaceVoxels.VoxelCount >= 0
		? FMath::Clamp(SurfaceVoxels.VoxelCount, 0, SurfaceVoxels.Positions.Num())
		: SurfaceVoxels.Positions.Num();
	if (EffectiveVoxelCount <= 0)
	{
		return OutMesh;
	}

	const float BaseVoxelSize = VoxelSize > 0.0f ? VoxelSize : SurfaceVoxels.VoxelSize;
	const float SafeVoxelSize = FMath::Max(BaseVoxelSize, UE_KINDA_SMALL_NUMBER);
	const float HalfQuadSize = SafeVoxelSize * FMath::Max(QuadScale, UE_KINDA_SMALL_NUMBER) * 0.5f;

	UE::Geometry::FDynamicMesh3 Mesh;
	Mesh.EnableVertexNormals(FVector3f::UpVector);

	int32 AddedTriangles = 0;
	for (int32 VoxelIndex = 0; VoxelIndex < EffectiveVoxelCount; ++VoxelIndex)
	{
		const FVector& Position = SurfaceVoxels.Positions[VoxelIndex];
		if (!IsFiniteCSVertex(Position))
		{
			continue;
		}

		FVector Normal = SurfaceVoxels.Normals.IsValidIndex(VoxelIndex)
			? SurfaceVoxels.Normals[VoxelIndex]
			: FVector::UpVector;
		if (Normal.ContainsNaN())
		{
			continue;
		}
		Normal = Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);

		const FVector HelperAxis = FMath::Abs(Normal.Z) < 0.99 ? FVector::UpVector : FVector::RightVector;
		const FVector AxisX = FVector::CrossProduct(HelperAxis, Normal).GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);
		const FVector AxisY = FVector::CrossProduct(Normal, AxisX).GetSafeNormal(UE_SMALL_NUMBER, FVector::RightVector);
		const FVector Center = Position + Normal * (SafeVoxelSize * NormalOffsetScale);
		const FVector DX = AxisX * HalfQuadSize;
		const FVector DY = AxisY * HalfQuadSize;

		const int32 A = Mesh.AppendVertex(FVector3d(Center - DX - DY));
		const int32 B = Mesh.AppendVertex(FVector3d(Center + DX - DY));
		const int32 C = Mesh.AppendVertex(FVector3d(Center + DX + DY));
		const int32 D = Mesh.AppendVertex(FVector3d(Center - DX + DY));
		Mesh.SetVertexNormal(A, FVector3f(Normal));
		Mesh.SetVertexNormal(B, FVector3f(Normal));
		Mesh.SetVertexNormal(C, FVector3f(Normal));
		Mesh.SetVertexNormal(D, FVector3f(Normal));

		const int32 T0 = bReverseOrientation
			? Mesh.AppendTriangle(UE::Geometry::FIndex3i(A, C, B), 0)
			: Mesh.AppendTriangle(UE::Geometry::FIndex3i(A, B, C), 0);
		const int32 T1 = bReverseOrientation
			? Mesh.AppendTriangle(UE::Geometry::FIndex3i(A, D, C), 0)
			: Mesh.AppendTriangle(UE::Geometry::FIndex3i(A, C, D), 0);
		if (T0 >= 0)
		{
			++AddedTriangles;
		}
		if (T1 >= 0)
		{
			++AddedTriangles;
		}
	}

	if (AddedTriangles <= 0)
	{
		return OutMesh;
	}

	OutMesh->SetMesh(MoveTemp(Mesh));
	if (bRecomputeNormals)
	{
		FGeometryScriptCalculateNormalsOptions CalculateOptions;
		UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(OutMesh, CalculateOptions);
	}
	SetGeneratedDynamicMesh(OutMesh);
	return OutMesh;
}

UDynamicMesh* AComputeShaderMeshGenerator::SurfaceVoxelsToVDBMesh(float VoxelSize,
	float RadiusMult,
	bool bRecomputeNormals)
{
	const FCSSurfaceVoxelData SurfaceVoxels = GetBoxSceneSurfaceVoxelsFromGPU(VoxelSize);
	UDynamicMesh* OutMesh = CreateEmptyDynamicMesh();
	if (!OutMesh)
	{
		return nullptr;
	}

	const int32 EffectiveVoxelCount = SurfaceVoxels.VoxelCount >= 0
		? FMath::Clamp(SurfaceVoxels.VoxelCount, 0, SurfaceVoxels.Positions.Num())
		: SurfaceVoxels.Positions.Num();
	if (EffectiveVoxelCount <= 0)
	{
		return OutMesh;
	}

	const float SafeVoxelSize = FMath::Max(VoxelSize > 0.0f ? VoxelSize : SurfaceVoxels.VoxelSize, UE_KINDA_SMALL_NUMBER);
	const float Rmin = 1.5f;
	const float Radius = FMath::Max((Rmin + 0.1f) * SafeVoxelSize * RadiusMult, (Rmin + 0.1f) * SafeVoxelSize);

	FCSGeneratorVDBParticleList Particles;
	for (int32 Index = 0; Index < EffectiveVoxelCount; ++Index)
	{
		const FVector& Position = SurfaceVoxels.Positions[Index];
		if (!IsFiniteCSVertex(Position))
		{
			continue;
		}
		Particles.Add(Position, Radius);
	}

	if (Particles.size() == 0)
	{
		return OutMesh;
	}

	const float Rmax = 100.0f;
	openvdb::FloatGrid::Ptr LevelSet = openvdb::createLevelSet<openvdb::FloatGrid>(SafeVoxelSize, 2.0);
	openvdb::tools::ParticlesToLevelSet<openvdb::FloatGrid> Raster(*LevelSet);
	Raster.setRmax(Rmax);
	Raster.setRmin(Rmin);
	Raster.rasterizeTrails(Particles, 0.75);
	Raster.finalize(true);
	LevelSet->setTransform(openvdb::math::Transform::createLinearTransform(SafeVoxelSize));

	FMeshDescription MeshDescription;
	ConvertVDBVolumeToMeshDescription(LevelSet, MeshDescription);

	FDynamicMesh3 ConvertedMesh;
	FMeshDescriptionToDynamicMesh Converter;
	Converter.Convert(&MeshDescription, ConvertedMesh);
	OutMesh->SetMesh(MoveTemp(ConvertedMesh));

	if (bRecomputeNormals)
	{
		FGeometryScriptCalculateNormalsOptions CalculateOptions;
		UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(OutMesh, CalculateOptions);
	}

	SetGeneratedDynamicMesh(OutMesh);
	return OutMesh;
}

UDynamicMesh* AComputeShaderMeshGenerator::GetBoxSceneTrianglesFilteredToDynamicMesh(float ReferenceFilterDistance,
	bool bReverseOrientation,
	bool bSkipDegenerateTriangles,
	bool bRecomputeNormals)
{
	FCSTriangleMeshData TriangleData = GetBoxSceneTrianglesFromGPUFiltered(ReferenceFilterDistance);
	NormalizeTriangleMeshDataWinding(TriangleData);
	UDynamicMesh* OutMesh = BuildDynamicMeshFromCSTriangleData(
		TriangleData.Vertices,
		TriangleData.Indices,
		TriangleData.VertexNormals,
		TriangleData.VertexCount,
		TriangleData.IndexCount,
		bReverseOrientation,
		bSkipDegenerateTriangles,
		bRecomputeNormals);
	SetGeneratedDynamicMesh(OutMesh);
	return OutMesh;
}

bool AComputeShaderMeshGenerator::SetGeneratedDynamicMesh(UDynamicMesh* NewMesh, float BoundsScale)
{
	UDynamicMeshComponent* MeshComponent = GetDynamicMeshComponent();
	if (!MeshComponent || !NewMesh)
	{
		return false;
	}

	MeshComponent->SetDynamicMesh(NewMesh);
	RefreshDynamicMeshComponentCullingBounds(BoundsScale);
	return true;
}

void AComputeShaderMeshGenerator::RefreshDynamicMeshComponentCullingBounds(float BoundsScale)
{
	UDynamicMeshComponent* MeshComponent = GetDynamicMeshComponent();
	if (!MeshComponent)
	{
		return;
	}

	const float SafeBoundsScale = FMath::Max(
		BoundsScale > 0.0f ? BoundsScale : DynamicMeshCullBoundsScale,
		1.0f);

	// DynamicMeshComponent is attached under SceneRoot in this actor.  If it uses
	// the attach parent's tiny bounds, the mesh is culled when the actor/root
	// origin leaves the view, even if the generated DynamicMesh is still visible.
	MeshComponent->bUseAttachParentBound = false;
	MeshComponent->bNeverDistanceCull = true;
	MeshComponent->bAllowCullDistanceVolume = false;
	MeshComponent->SetCachedMaxDrawDistance(0.0f);
	MeshComponent->SetBoundsScale(SafeBoundsScale);

	// Rebuild render proxy + recompute DynamicMeshComponent LocalBounds from the
	// actual mesh. This fixes stale bounds after replacing or editing UDynamicMesh
	// data through Blueprint/GeometryScript paths.
	MeshComponent->NotifyMeshUpdated();
	MeshComponent->UpdateBounds();
	MeshComponent->MarkRenderTransformDirty();
	MeshComponent->MarkRenderStateDirty();
}

// -----------------------------------------------------------------------------
// Core System - Lifecycle
// -----------------------------------------------------------------------------

AComputeShaderMeshGenerator::AComputeShaderMeshGenerator(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = false;

	if (GeneratorTimeCode == -1)
	{
		const FDateTime Now = FDateTime::Now();
		GeneratorTimeCode =
			int64(Now.GetYear() % 100) * 100000000LL +
			int64(Now.GetMonth()) * 1000000LL +
			int64(Now.GetDay()) * 10000LL +
			int64(Now.GetHour()) * 100LL +
			int64(Now.GetMinute());
	}

	USceneComponent* ExistingRoot = GetRootComponent();
	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);
	if (ExistingRoot && ExistingRoot != SceneRoot)
	{
		ExistingRoot->SetupAttachment(SceneRoot);
	}
	if (UDynamicMeshComponent* MeshComponent = GetDynamicMeshComponent())
	{
		MeshComponent->bUseAttachParentBound = false;
		MeshComponent->bNeverDistanceCull = true;
		MeshComponent->bAllowCullDistanceVolume = false;
		MeshComponent->SetCachedMaxDrawDistance(0.0f);
		MeshComponent->SetBoundsScale(DynamicMeshCullBoundsScale);
	}

	GeneratorBounds = CreateDefaultSubobject<UBoxComponent>(TEXT("GeneratorBounds"));
	GeneratorBounds->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	GeneratorBounds->SetCollisionObjectType(ECollisionChannel::ECC_WorldDynamic);
	GeneratorBounds->SetCollisionResponseToAllChannels(ECR_Overlap);
	GeneratorBounds->SetHiddenInGame(true);
	GeneratorBounds->SetBoxExtent(FVector(500.0, 500.0, 500.0));
	GeneratorBounds->SetupAttachment(SceneRoot);
}

void AComputeShaderMeshGenerator::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	RefreshDynamicMeshComponentCullingBounds();
}

void AComputeShaderMeshGenerator::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	RefreshDynamicMeshComponentCullingBounds();
}

void AComputeShaderMeshGenerator::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearMeshGeneratorCache();
	ClearGeneratedDataTextureCache();
	Super::EndPlay(EndPlayReason);
}

// -----------------------------------------------------------------------------
// Brush System
// -----------------------------------------------------------------------------

FCSInstanceBrushEditorRequest AComputeShaderMeshGenerator::OnInstanceBrushEditorRequest;

void AComputeShaderMeshGenerator::StartInstanceBrush()
{
#if WITH_EDITOR
	if (!InstanceBrushMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("[AComputeShaderMeshGenerator::StartInstanceBrush] InstanceBrushMesh is not set. Actor=%s"),
			*GetNameSafe(this));
	}

	if (OnInstanceBrushEditorRequest.IsBound())
	{
		OnInstanceBrushEditorRequest.Broadcast(this);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[AComputeShaderMeshGenerator::StartInstanceBrush] No editor brush handler is registered. Actor=%s"),
			*GetNameSafe(this));
	}
#endif
}

UHierarchicalInstancedStaticMeshComponent* AComputeShaderMeshGenerator::FindPaintComponent(UStaticMesh* Mesh) const
{
	if (!Mesh)
	{
		return nullptr;
	}

	for (const FCSInstancePaintComponentSlot& Slot : PaintedInstanceComponents)
	{
		if (Slot.Mesh == Mesh && Slot.Component)
		{
			return Slot.Component;
		}
	}

	return nullptr;
}

UHierarchicalInstancedStaticMeshComponent* AComputeShaderMeshGenerator::GetOrCreatePaintComponent(UStaticMesh* Mesh)
{
	if (!Mesh)
	{
		return nullptr;
	}

	if (UHierarchicalInstancedStaticMeshComponent* ExistingComponent = FindPaintComponent(Mesh))
	{
		return ExistingComponent;
	}

	const FName BaseName(*FString::Printf(TEXT("PaintedInstances_%s"), *Mesh->GetName()));
	const FName ComponentName = MakeUniqueObjectName(this, UHierarchicalInstancedStaticMeshComponent::StaticClass(), BaseName);
	UHierarchicalInstancedStaticMeshComponent* NewComponent = NewObject<UHierarchicalInstancedStaticMeshComponent>(this, ComponentName, RF_Transactional);
	if (!NewComponent)
	{
		return nullptr;
	}

	NewComponent->SetStaticMesh(Mesh);
	NewComponent->SetMobility(EComponentMobility::Static);
	NewComponent->SetupAttachment(SceneRoot ? SceneRoot.Get() : GetRootComponent());
	AddInstanceComponent(NewComponent);
	NewComponent->RegisterComponent();

	FCSInstancePaintComponentSlot& NewSlot = PaintedInstanceComponents.AddDefaulted_GetRef();
	NewSlot.Mesh = Mesh;
	NewSlot.Component = NewComponent;

#if WITH_EDITOR
	MarkPackageDirty();
#endif

	return NewComponent;
}

int32 AComputeShaderMeshGenerator::CommitPaintInstances(const TArray<FTransform>& WorldTransforms, UStaticMesh* Mesh)
{
	if (!Mesh || WorldTransforms.IsEmpty())
	{
		return 0;
	}

	UHierarchicalInstancedStaticMeshComponent* PaintComponent = GetOrCreatePaintComponent(Mesh);
	if (!PaintComponent)
	{
		return 0;
	}

	const int32 PreviousInstanceCount = PaintComponent->GetInstanceCount();
	PaintComponent->AddInstances(WorldTransforms, false, true, false);
	PaintComponent->MarkRenderStateDirty();

#if WITH_EDITOR
	MarkPackageDirty();
#endif

	return PaintComponent->GetInstanceCount() - PreviousInstanceCount;
}

bool AComputeShaderMeshGenerator::IsInstanceBrushPointAllowed(const FVector& WorldPosition) const
{
	if (!bInstanceBrushUseGeneratorBounds)
	{
		return true;
	}

	const FBox Bounds = GetGeneratorBoundsWorldBox();
	return Bounds.IsValid && Bounds.IsInsideOrOn(WorldPosition);
}

// -----------------------------------------------------------------------------
// Dirty Cache System - Public API
// -----------------------------------------------------------------------------

FCSMeshGeneratorTriangleCacheHandle AComputeShaderMeshGenerator::EnsureTriangleCache(const FCSMeshGeneratorTriangleCacheRequest& Request)
{
	FCSMeshGeneratorTriangleCacheRequest NormalizedRequest = Request;
	NormalizedRequest.RequestId = NormalizeRequestId(Request.RequestId);
	NormalizedRequest.CachedReferencePoints = ReferencePoints;

	const FBox InputWorldBounds = GetGeneratorBoundsWorldBox();
	if (!InputWorldBounds.IsValid || !FMath::IsFinite(VoxelGridSettings.VoxelSize) || VoxelGridSettings.VoxelSize <= CSGeneratorMinVoxelSize)
	{
		UE_LOG(LogTemp, Warning, TEXT("[AComputeShaderMeshGenerator::EnsureTriangleCache] Invalid bounds or voxel size. Actor=%s Request=%s"),
			*GetNameSafe(this),
			*NormalizedRequest.RequestId.ToString());
		return GetTriangleCacheHandle();
	}

	const bool bNeedsFullRebuild = NormalizedRequest.bForceFullRebuild || DoesInputRequireFullRebuild(InputWorldBounds) || !HasValidCacheResources();
	if (bNeedsFullRebuild)
	{
		ResetCacheRuntime(false);
		RebuildCacheResources(InputWorldBounds);
		++CacheState.CacheGeneration;
		RebuildRequestActiveCellsFromLastRequests();
	}

	if (!HasValidCacheResources())
	{
		UE_LOG(LogTemp, Warning, TEXT("[AComputeShaderMeshGenerator::EnsureTriangleCache] Cache resources are invalid after rebuild. Actor=%s Request=%s"),
			*GetNameSafe(this),
			*NormalizedRequest.RequestId.ToString());
		return GetTriangleCacheHandle();
	}

	TSet<FCSMeshGeneratorVoxelKey> RequestCells;
	const float RequestActivationRadius = NormalizedRequest.ActivationRadiusOverride > 0.0f
		? NormalizedRequest.ActivationRadiusOverride
		: VoxelGridSettings.ActivationRadius;
	BuildActiveCellsFromReferencePoints(NormalizedRequest.CachedReferencePoints, RequestActivationRadius, RequestCells);

	const FName SafeRequestId = NormalizedRequest.RequestId;
	const bool bPersistentInterest = NormalizedRequest.bPersistentInterest;
	if (bPersistentInterest)
	{
		RequestActiveCells.FindOrAdd(SafeRequestId) = MoveTemp(RequestCells);
		LastRequests.FindOrAdd(SafeRequestId) = MoveTemp(NormalizedRequest);
	}
	else
	{
		RequestActiveCells.FindOrAdd(SafeRequestId) = RequestCells;
	}

	TSet<FCSMeshGeneratorVoxelKey> NewUnionActiveCells;
	BuildUnionActiveCells(NewUnionActiveCells);

	DiffActiveCells(NewUnionActiveCells);
	ReleasePagesForCells(CacheState.CellsToDeactivate);
	AllocatePagesForCells(CacheState.CellsToActivate);
	CacheState.DirtyCells.Append(CacheState.CellsToActivate);

	DispatchDirtyVoxelTriangleCacheUpdate();

	CacheState.ActiveCells = MoveTemp(NewUnionActiveCells);

	if (!bPersistentInterest)
	{
		RequestActiveCells.Remove(SafeRequestId);
	}

	return GetTriangleCacheHandle();
}

FCSMeshGeneratorTriangleCacheHandle AComputeShaderMeshGenerator::EnsureTriangleCacheByBox(
	FName RequestId,
	bool bForceFullRebuild)
{
	FCSMeshGeneratorTriangleCacheRequest Request;
	Request.RequestId = RequestId;
	Request.bForceFullRebuild = bForceFullRebuild;
	return EnsureTriangleCache(Request);
}

FCSMeshGeneratorTriangleCacheHandle AComputeShaderMeshGenerator::EnsureTriangleCacheByBox(
	FName RequestId,
	const FVector& BoxCenter,
	const FVector& BoxExtent,
	bool bForceFullRebuild)
{
	if (GeneratorBounds)
	{
		GeneratorBounds->SetWorldLocation(BoxCenter);

		const FVector SafeWorldExtent(
			FMath::Max(0.0, BoxExtent.X),
			FMath::Max(0.0, BoxExtent.Y),
			FMath::Max(0.0, BoxExtent.Z));
		const FVector ComponentScale = GeneratorBounds->GetComponentTransform().GetScale3D().GetAbs();
		const FVector SafeComponentScale(
			FMath::Max(UE_KINDA_SMALL_NUMBER, ComponentScale.X),
			FMath::Max(UE_KINDA_SMALL_NUMBER, ComponentScale.Y),
			FMath::Max(UE_KINDA_SMALL_NUMBER, ComponentScale.Z));
		GeneratorBounds->SetBoxExtent(
			FVector(
				SafeWorldExtent.X / SafeComponentScale.X,
				SafeWorldExtent.Y / SafeComponentScale.Y,
				SafeWorldExtent.Z / SafeComponentScale.Z),
			true);
	}

	return EnsureTriangleCacheByBox(RequestId, bForceFullRebuild);
}

void AComputeShaderMeshGenerator::UpdateMeshGeneratorCacheByBox(
	bool bForceFullRebuild)
{
	EnsureTriangleCacheByBox(CSGeneratorDefaultRequestId, bForceFullRebuild);
}

void AComputeShaderMeshGenerator::ReleaseTriangleCacheRequest(FName RequestId)
{
	const FName SafeRequestId = NormalizeRequestId(RequestId);
	if (!RequestActiveCells.Contains(SafeRequestId) && !LastRequests.Contains(SafeRequestId))
	{
		return;
	}

	RequestActiveCells.Remove(SafeRequestId);
	LastRequests.Remove(SafeRequestId);

	TSet<FCSMeshGeneratorVoxelKey> NewUnionActiveCells;
	BuildUnionActiveCells(NewUnionActiveCells);

	DiffActiveCells(NewUnionActiveCells);
	ReleasePagesForCells(CacheState.CellsToDeactivate);
	CacheState.ActiveCells = MoveTemp(NewUnionActiveCells);
}

void AComputeShaderMeshGenerator::ClearMeshGeneratorCache()
{
	ResetCacheRuntime(true);
	++CacheState.CacheGeneration;
}

void AComputeShaderMeshGenerator::MarkAllActiveVoxelsDirty()
{
	CacheState.DirtyCells.Append(CacheState.ActiveCells);
}

FCSMeshGeneratorTriangleCacheHandle AComputeShaderMeshGenerator::GetTriangleCacheHandle() const
{
	FCSMeshGeneratorTriangleCacheHandle Handle;
	Handle.bValid = HasValidCacheResources() && CacheState.CachedWorldBounds.IsValid;
	Handle.CacheGeneration = int32(FMath::Min<uint32>(CacheState.CacheGeneration, uint32(TNumericLimits<int32>::Max())));
	Handle.CachedWorldBounds = CacheState.CachedWorldBounds;
	Handle.GridSize = CacheState.GridSize;
	Handle.VoxelSize = CacheState.CachedVoxelSize;
	Handle.ActiveVoxelCount = CacheState.ActiveCells.Num();
	Handle.DirtyVoxelCount = CacheState.DirtyCells.Num();
	Handle.VoxelMetaRT = VoxelMetaRT;
	Handle.TriangleVertexRT = TriangleVertexRT;
	Handle.TriangleNormalRT = TriangleNormalRT;
	return Handle;
}

FBox AComputeShaderMeshGenerator::GetCachedWorldBounds() const
{
	return CacheState.CachedWorldBounds;
}

// -----------------------------------------------------------------------------
// Debug System
// -----------------------------------------------------------------------------

int32 AComputeShaderMeshGenerator::DrawDebugActiveVoxels(
	FName RequestId,
	FLinearColor DebugColor,
	float Duration,
	float Thickness,
	bool bPersistentLines,
	bool bDrawCacheBounds,
	int32 MaxVoxelsToDraw) const
{
	UWorld* World = GetWorld();
	if (!World || !CacheState.CachedWorldBounds.IsValid || CacheState.CachedVoxelSize <= CSGeneratorMinVoxelSize)
	{
		return 0;
	}

	const TSet<FCSMeshGeneratorVoxelKey>* CellsToDraw = &CacheState.ActiveCells;
	if (!RequestId.IsNone())
	{
		CellsToDraw = RequestActiveCells.Find(NormalizeRequestId(RequestId));
		if (!CellsToDraw)
		{
			return 0;
		}
	}

	if (CellsToDraw->IsEmpty())
	{
		return 0;
	}

	const float SafeDuration = FMath::Max(0.0f, Duration);
	const float SafeThickness = FMath::Max(0.0f, Thickness);
	const int32 DrawLimit = MaxVoxelsToDraw > 0 ? MaxVoxelsToDraw : TNumericLimits<int32>::Max();
	const FColor LineColor = DebugColor.ToFColor(true);

	int32 DrawnCount = 0;
	for (const FCSMeshGeneratorVoxelKey& Cell : *CellsToDraw)
	{
		if (DrawnCount >= DrawLimit)
		{
			break;
		}

		const FBox CellBounds = GetCellWorldBounds(Cell);
		if (!CellBounds.IsValid)
		{
			continue;
		}

		DrawDebugBox(
			World,
			CellBounds.GetCenter(),
			CellBounds.GetExtent(),
			LineColor,
			bPersistentLines,
			SafeDuration,
			0,
			SafeThickness);
		++DrawnCount;
	}

	if (bDrawCacheBounds)
	{
		DrawDebugBox(
			World,
			CacheState.CachedWorldBounds.GetCenter(),
			CacheState.CachedWorldBounds.GetExtent(),
			FColor::White,
			bPersistentLines,
			SafeDuration,
			0,
			FMath::Max(1.0f, SafeThickness));
	}

	return DrawnCount;
}

// -----------------------------------------------------------------------------
// Dirty Cache System - Internals
// -----------------------------------------------------------------------------

bool AComputeShaderMeshGenerator::DoesInputRequireFullRebuild(const FBox& InputWorldBounds) const
{
	const float SafeVoxelSize = FMath::Max(VoxelGridSettings.VoxelSize, CSGeneratorMinVoxelSize);
	const int32 SafeMaxActiveVoxels = FMath::Max(1, VoxelGridSettings.MaxActiveVoxels);
	const int32 SafeMaxTrianglesPerVoxel = FMath::Max(1, VoxelGridSettings.MaxTrianglesPerVoxel);
	const int32 SafeMaxTextureDimension = FMath::Max(CSGeneratorMinTextureDimension, VoxelGridSettings.MaxCacheTextureDimension);

	if (!CacheState.CachedWorldBounds.IsValid)
	{
		return true;
	}

	if (!AreBoundsCompatible(CacheState.CachedWorldBounds, InputWorldBounds))
	{
		return true;
	}

	if (!FMath::IsNearlyEqual(CacheState.CachedVoxelSize, SafeVoxelSize, KINDA_SMALL_NUMBER))
	{
		return true;
	}

	if (CacheState.GridSize != ComputeGridSize(InputWorldBounds))
	{
		return true;
	}

	if (CacheState.CachedMaxActiveVoxels != SafeMaxActiveVoxels ||
		CacheState.CachedMaxTrianglesPerVoxel != SafeMaxTrianglesPerVoxel ||
		CacheState.CachedLODIndex != VoxelGridSettings.LODIndex ||
		CacheState.CachedMaxTextureDimension != SafeMaxTextureDimension)
	{
		return true;
	}

	return false;
}

void AComputeShaderMeshGenerator::RebuildCacheResources(const FBox& InputWorldBounds)
{
	CacheState.CachedWorldBounds = InputWorldBounds;
	CacheState.CachedVoxelSize = FMath::Max(VoxelGridSettings.VoxelSize, CSGeneratorMinVoxelSize);
	CacheState.GridSize = ComputeGridSize(InputWorldBounds);
	CacheState.CachedMaxActiveVoxels = FMath::Max(1, VoxelGridSettings.MaxActiveVoxels);
	CacheState.CachedMaxTrianglesPerVoxel = FMath::Max(1, VoxelGridSettings.MaxTrianglesPerVoxel);
	CacheState.CachedLODIndex = VoxelGridSettings.LODIndex;
	CacheState.CachedMaxTextureDimension = FMath::Max(CSGeneratorMinTextureDimension, VoxelGridSettings.MaxCacheTextureDimension);

	InitializeFreePages();
	CreateCacheRenderTargets();
}

void AComputeShaderMeshGenerator::BuildActiveCellsFromReferencePoints(
	float ActivationRadius,
	TSet<FCSMeshGeneratorVoxelKey>& OutCells) const
{
	BuildActiveCellsFromReferencePoints(ReferencePoints, ActivationRadius, OutCells);
}

void AComputeShaderMeshGenerator::BuildActiveCellsFromReferencePoints(
	const TArray<FVector>& InReferencePoints,
	float ActivationRadius,
	TSet<FCSMeshGeneratorVoxelKey>& OutCells) const
{
	OutCells.Reset();
	if (!CacheState.CachedWorldBounds.IsValid || CacheState.GridSize.X <= 0 || CacheState.GridSize.Y <= 0 || CacheState.GridSize.Z <= 0)
	{
		return;
	}

	const float SafeVoxelSize = FMath::Max(CacheState.CachedVoxelSize, CSGeneratorMinVoxelSize);
	const float SafeActivationRadius = FMath::Max(ActivationRadius, 0.0f);
	const int32 RadiusInCells = FMath::Max(0, FMath::CeilToInt(SafeActivationRadius / SafeVoxelSize));
	const double ActivationRadiusSq = double(SafeActivationRadius) * double(SafeActivationRadius);

	for (const FVector& Point : InReferencePoints)
	{
		if (!IsFiniteCSGeneratorVector(Point) || !CacheState.CachedWorldBounds.IsInsideOrOn(Point))
		{
			continue;
		}

		const FIntVector CenterCell = WorldPositionToCell(Point).ToIntVector();
		for (int32 Z = CenterCell.Z - RadiusInCells; Z <= CenterCell.Z + RadiusInCells; ++Z)
		{
			if (Z < 0 || Z >= CacheState.GridSize.Z)
			{
				continue;
			}

			for (int32 Y = CenterCell.Y - RadiusInCells; Y <= CenterCell.Y + RadiusInCells; ++Y)
			{
				if (Y < 0 || Y >= CacheState.GridSize.Y)
				{
					continue;
				}

				for (int32 X = CenterCell.X - RadiusInCells; X <= CenterCell.X + RadiusInCells; ++X)
				{
					if (X < 0 || X >= CacheState.GridSize.X)
					{
						continue;
					}

					const FCSMeshGeneratorVoxelKey Cell(X, Y, Z);
					const FBox CellBounds = GetCellWorldBounds(Cell);
					const FVector ClosestPoint = CellBounds.GetClosestPointTo(Point);
					const double DistSq = FVector::DistSquared(ClosestPoint, Point);
					if (RadiusInCells == 0 || DistSq <= ActivationRadiusSq)
					{
						OutCells.Add(Cell);
						if (OutCells.Num() >= FMath::Max(1, VoxelGridSettings.MaxActiveVoxels))
						{
							return;
						}
					}
				}
			}
		}
	}
}

void AComputeShaderMeshGenerator::BuildUnionActiveCells(TSet<FCSMeshGeneratorVoxelKey>& OutCells) const
{
	OutCells.Reset();
	const int32 MaxActiveVoxels = FMath::Max(1, VoxelGridSettings.MaxActiveVoxels);
	for (const TPair<FName, TSet<FCSMeshGeneratorVoxelKey>>& Pair : RequestActiveCells)
	{
		for (const FCSMeshGeneratorVoxelKey& Cell : Pair.Value)
		{
			OutCells.Add(Cell);
			if (OutCells.Num() >= MaxActiveVoxels)
			{
				return;
			}
		}
	}
}

void AComputeShaderMeshGenerator::DiffActiveCells(const TSet<FCSMeshGeneratorVoxelKey>& NewActiveCells)
{
	CacheState.CellsToActivate.Reset();
	CacheState.CellsToDeactivate.Reset();
	CacheState.DirtyCells.Reset();

	for (const FCSMeshGeneratorVoxelKey& Cell : NewActiveCells)
	{
		if (!CacheState.ActiveCells.Contains(Cell))
		{
			CacheState.CellsToActivate.Add(Cell);
		}
	}

	for (const FCSMeshGeneratorVoxelKey& Cell : CacheState.ActiveCells)
	{
		if (!NewActiveCells.Contains(Cell))
		{
			CacheState.CellsToDeactivate.Add(Cell);
		}
	}
}

void AComputeShaderMeshGenerator::AllocatePagesForCells(const TSet<FCSMeshGeneratorVoxelKey>& Cells)
{
	for (const FCSMeshGeneratorVoxelKey& Cell : Cells)
	{
		if (CacheState.CellToPage.Contains(Cell))
		{
			continue;
		}

		if (CacheState.FreePages.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("[AComputeShaderMeshGenerator] Triangle cache page capacity exhausted. Actor=%s MaxActiveVoxels=%d"),
				*GetNameSafe(this),
				CacheState.CachedMaxActiveVoxels);
			break;
		}

		const int32 PageIndex = CacheState.FreePages.Pop(EAllowShrinking::No);
		CacheState.CellToPage.Add(Cell, PageIndex);
	}
}

void AComputeShaderMeshGenerator::ReleasePagesForCells(const TSet<FCSMeshGeneratorVoxelKey>& Cells)
{
	for (const FCSMeshGeneratorVoxelKey& Cell : Cells)
	{
		int32 PageIndex = INDEX_NONE;
		if (CacheState.CellToPage.RemoveAndCopyValue(Cell, PageIndex) && PageIndex != INDEX_NONE)
		{
			if (!CacheState.FreePages.Contains(PageIndex))
			{
				CacheState.FreePages.Add(PageIndex);
			}
		}
	}
}

void AComputeShaderMeshGenerator::DispatchDirtyVoxelTriangleCacheUpdate()
{
	if (CacheState.DirtyCells.IsEmpty())
	{
		return;
	}

	FBox DirtyWorldBounds(ForceInit);
	for (const FCSMeshGeneratorVoxelKey& DirtyCell : CacheState.DirtyCells)
	{
		DirtyWorldBounds += GetCellWorldBounds(DirtyCell);
	}

	if (!DirtyWorldBounds.IsValid)
	{
		CacheState.DirtyCells.Reset();
		return;
	}

	DirtyWorldBounds = DirtyWorldBounds.ExpandBy(FMath::Max(CacheState.CachedVoxelSize, CSGeneratorMinVoxelSize) * 0.5f);

	TArray<FCSStaticMeshTriangleRequest> SceneTriangleRequests;
	BuildBoxSceneTriangleRequestsInternal(
		GetWorld(),
		DirtyWorldBounds,
		CacheState.CachedLODIndex,
		SceneTriangleRequests);

	TArray<FVector> CacheReferencePoints;
	for (const TPair<FName, FCSMeshGeneratorTriangleCacheRequest>& Pair : LastRequests)
	{
		CacheReferencePoints.Append(Pair.Value.CachedReferencePoints);
	}

	const int32 DirtyCellCount = CacheState.DirtyCells.Num();
	TArray<FCSMeshGeneratorDirtyVoxelPage> DirtyPageData;
	DirtyPageData.Reserve(DirtyCellCount);
	for (const FCSMeshGeneratorVoxelKey& DirtyCell : CacheState.DirtyCells)
	{
		const int32* PageIndex = CacheState.CellToPage.Find(DirtyCell);
		if (!PageIndex || *PageIndex < 0)
		{
			continue;
		}

		FCSMeshGeneratorDirtyVoxelPage& DirtyPage = DirtyPageData.AddDefaulted_GetRef();
		DirtyPage.X = uint32(FMath::Max(0, DirtyCell.X));
		DirtyPage.Y = uint32(FMath::Max(0, DirtyCell.Y));
		DirtyPage.Z = uint32(FMath::Max(0, DirtyCell.Z));
		DirtyPage.PageIndex = uint32(*PageIndex);
	}

	if (DirtyPageData.IsEmpty() || !VoxelMetaRT || !TriangleVertexRT || !TriangleNormalRT)
	{
		CacheState.DirtyCells.Reset();
		return;
	}
	const int32 DirtyPageCountForLog = DirtyPageData.Num();

	const int64 RequestedTriangleCapacity = int64(FMath::Max(1, DirtyCellCount)) * int64(FMath::Max(1, CacheState.CachedMaxTrianglesPerVoxel));
	const int32 MaxTrianglesForDirtyCells = int32(FMath::Clamp<int64>(RequestedTriangleCapacity, 1, int64(TNumericLimits<int32>::Max())));
	const float ReferenceFilterDistance = FMath::Max(0.0f, VoxelGridSettings.ActivationRadius);
	TArray<FResolvedStaticMeshTriangleRequest> ResolvedRequests;
	const uint64 TotalStaticMeshTriangleCount = ResolveStaticMeshTriangleRequests(
		SceneTriangleRequests,
		this,
		ExcludedActorTag,
		true,
		ResolvedRequests);

	FCSTriangleMeshData LandscapeTriangleData;
	BuildBoxSceneLandscapeTrianglesInternal(
		GetWorld(),
		DirtyWorldBounds,
		CacheReferencePoints,
		ReferenceFilterDistance,
		MaxTrianglesForDirtyCells,
		LandscapeTriangleData);

	const uint32 CacheGeneration = CacheState.CacheGeneration;
	const FIntVector GridSize = CacheState.GridSize;
	const FVector CacheWorldMin = CacheState.CachedWorldBounds.Min;
	const float CachedVoxelSize = FMath::Max(CacheState.CachedVoxelSize, CSGeneratorMinVoxelSize);
	const uint32 MaxTrianglesPerVoxel = uint32(FMath::Max(1, CacheState.CachedMaxTrianglesPerVoxel));
	const uint32 MetaTextureWidth = uint32(FMath::Max(1, VoxelMetaRT->SizeX));
	const uint32 MetaTextureHeight = uint32(FMath::Max(1, VoxelMetaRT->SizeY));
	const uint32 TriangleVertexTextureWidth = uint32(FMath::Max(1, TriangleVertexRT->SizeX));
	const uint32 TriangleVertexTextureHeight = uint32(FMath::Max(1, TriangleVertexRT->SizeY));
	const uint32 TriangleNormalTextureWidth = uint32(FMath::Max(1, TriangleNormalRT->SizeX));
	const uint32 TriangleNormalTextureHeight = uint32(FMath::Max(1, TriangleNormalRT->SizeY));
	FTextureRenderTargetResource* VoxelMetaResource = VoxelMetaRT->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* TriangleVertexResource = TriangleVertexRT->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* TriangleNormalResource = TriangleNormalRT->GameThread_GetRenderTargetResource();

	ENQUEUE_RENDER_COMMAND(CSMeshGeneratorUpdateDirtyVoxelTriangleCache)(
		[ResolvedRequests = MoveTemp(ResolvedRequests),
		 TotalStaticMeshTriangleCount,
		 ReferencePoints = MoveTemp(CacheReferencePoints),
		 LandscapeTriangleData = MoveTemp(LandscapeTriangleData),
		 DirtyPageData = MoveTemp(DirtyPageData),
		 VoxelMetaResource,
		 TriangleVertexResource,
		 TriangleNormalResource,
		 ReferenceFilterDistance,
		 MaxTrianglesForDirtyCells,
		 CacheGeneration,
		 GridSize,
		 CacheWorldMin,
		 CachedVoxelSize,
		 MaxTrianglesPerVoxel,
		 MetaTextureWidth,
		 MetaTextureHeight,
		 TriangleVertexTextureWidth,
		 TriangleVertexTextureHeight,
		 TriangleNormalTextureWidth,
		 TriangleNormalTextureHeight](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			FRDGTextureRef VoxelMetaTexture = RegisterRenderTargetTexture(GraphBuilder, VoxelMetaResource, TEXT("CS.MeshGenerator.VoxelMetaRT"));
			FRDGTextureRef TriangleVertexTexture = RegisterRenderTargetTexture(GraphBuilder, TriangleVertexResource, TEXT("CS.MeshGenerator.TriangleVertexRT"));
			FRDGTextureRef TriangleNormalTexture = RegisterRenderTargetTexture(GraphBuilder, TriangleNormalResource, TEXT("CS.MeshGenerator.TriangleNormalRT"));
			if (!VoxelMetaTexture || !TriangleVertexTexture || !TriangleNormalTexture || DirtyPageData.IsEmpty())
			{
				GraphBuilder.Execute();
				return;
			}

			FRDGTextureUAVRef VoxelMetaUAV = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(VoxelMetaTexture));
			FRDGTextureUAVRef TriangleVertexUAV = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(TriangleVertexTexture));
			FRDGTextureUAVRef TriangleNormalUAV = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(TriangleNormalTexture));

			const uint32 DirtyPageCount = uint32(DirtyPageData.Num());
			FRDGBufferRef DirtyPageBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateBufferDesc(sizeof(FCSMeshGeneratorDirtyVoxelPage), DirtyPageData.Num()),
				TEXT("CS.MeshGenerator.DirtyVoxelPages"));
			FCSMeshGeneratorDirtyVoxelPage* DirtyPageUploadData = GraphBuilder.AllocPODArray<FCSMeshGeneratorDirtyVoxelPage>(DirtyPageData.Num());
			FMemory::Memcpy(DirtyPageUploadData, DirtyPageData.GetData(), DirtyPageData.Num() * sizeof(FCSMeshGeneratorDirtyVoxelPage));
			GraphBuilder.QueueBufferUpload(DirtyPageBuffer, DirtyPageUploadData, DirtyPageData.Num() * sizeof(FCSMeshGeneratorDirtyVoxelPage));
			FRDGBufferSRVRef DirtyPageSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(DirtyPageBuffer, PF_R32G32B32A32_UINT));

			TShaderMapRef<FClearDirtyVoxelCacheCS> ClearShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FClearDirtyVoxelCacheCS::FParameters* ClearParameters = GraphBuilder.AllocParameters<FClearDirtyVoxelCacheCS::FParameters>();
			ClearParameters->DirtyVoxelPages = DirtyPageSRV;
			ClearParameters->RW_VoxelMetaTexture = VoxelMetaUAV;
			ClearParameters->RW_TriangleVertexTexture = TriangleVertexUAV;
			ClearParameters->RW_TriangleNormalTexture = TriangleNormalUAV;
			ClearParameters->DirtyVoxelCount = DirtyPageCount;
			ClearParameters->CacheGeneration = CacheGeneration;
			ClearParameters->MaxTrianglesPerVoxel = MaxTrianglesPerVoxel;
			ClearParameters->MetaTextureWidth = MetaTextureWidth;
			ClearParameters->MetaTextureHeight = MetaTextureHeight;
			ClearParameters->TriangleVertexTextureWidth = TriangleVertexTextureWidth;
			ClearParameters->TriangleVertexTextureHeight = TriangleVertexTextureHeight;
			ClearParameters->TriangleNormalTextureWidth = TriangleNormalTextureWidth;
			ClearParameters->TriangleNormalTextureHeight = TriangleNormalTextureHeight;
			GraphBuilder.AddPass(
				RDG_EVENT_NAME("CS.MeshGenerator.ClearDirtyVoxelCache"),
				ClearParameters,
				ERDGPassFlags::Compute,
				[ClearParameters, ClearShader, DirtyPageCount](FRHIComputeCommandList& InRHICmdList)
				{
					FComputeShaderUtils::Dispatch(InRHICmdList, ClearShader, *ClearParameters, FComputeShaderUtils::GetGroupCount(FIntVector(int32(DirtyPageCount), 1, 1), 64));
				});

			const FCSTriangleMeshData* InitialTriangleData = GetTriangleMeshDataTriangleCount(LandscapeTriangleData) > 0 ? &LandscapeTriangleData : nullptr;
			if (!ResolvedRequests.IsEmpty() || InitialTriangleData)
			{
				FCSStaticMeshTriangleRDGOutput TriangleOutput = AddResolvedStaticMeshTrianglesToRDGInternal(
					GraphBuilder,
					RHICmdList,
					ResolvedRequests,
					TotalStaticMeshTriangleCount,
					ReferencePoints,
					ReferenceFilterDistance,
					MaxTrianglesForDirtyCells,
					InitialTriangleData,
					TEXT("CS.MeshGenerator.DirtyStaticMeshTriangles"));

				if (TriangleOutput.TriangleVertices && TriangleOutput.TriangleNormals && TriangleOutput.TriangleCounter && TriangleOutput.MaxTriangles > 0)
				{
					TShaderMapRef<FScatterTrianglesToVoxelCacheCS> ScatterShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					FScatterTrianglesToVoxelCacheCS::FParameters* ScatterParameters = GraphBuilder.AllocParameters<FScatterTrianglesToVoxelCacheCS::FParameters>();
					ScatterParameters->TriangleVertices = TriangleOutput.TriangleVerticesSRV
						? TriangleOutput.TriangleVerticesSRV
						: GraphBuilder.CreateSRV(FRDGBufferSRVDesc(TriangleOutput.TriangleVertices, PF_A32B32G32R32F));
					ScatterParameters->TriangleNormals = TriangleOutput.TriangleNormalsSRV
						? TriangleOutput.TriangleNormalsSRV
						: GraphBuilder.CreateSRV(FRDGBufferSRVDesc(TriangleOutput.TriangleNormals, PF_A32B32G32R32F));
					ScatterParameters->SurfaceTriangleCounter = TriangleOutput.TriangleCounterSRV
						? TriangleOutput.TriangleCounterSRV
						: GraphBuilder.CreateSRV(FRDGBufferSRVDesc(TriangleOutput.TriangleCounter, PF_R32_UINT));
					ScatterParameters->DirtyVoxelPages = DirtyPageSRV;
					ScatterParameters->RW_VoxelMetaTexture = VoxelMetaUAV;
					ScatterParameters->RW_TriangleVertexTexture = TriangleVertexUAV;
					ScatterParameters->RW_TriangleNormalTexture = TriangleNormalUAV;
					ScatterParameters->SurfaceTriangleCount = TriangleOutput.MaxTriangles;
					ScatterParameters->DirtyVoxelCount = DirtyPageCount;
					ScatterParameters->CacheGeneration = CacheGeneration;
					ScatterParameters->GridSizeX = uint32(FMath::Max(0, GridSize.X));
					ScatterParameters->GridSizeY = uint32(FMath::Max(0, GridSize.Y));
					ScatterParameters->GridSizeZ = uint32(FMath::Max(0, GridSize.Z));
					ScatterParameters->MaxTrianglesPerVoxel = MaxTrianglesPerVoxel;
					ScatterParameters->MetaTextureWidth = MetaTextureWidth;
					ScatterParameters->MetaTextureHeight = MetaTextureHeight;
					ScatterParameters->TriangleVertexTextureWidth = TriangleVertexTextureWidth;
					ScatterParameters->TriangleVertexTextureHeight = TriangleVertexTextureHeight;
					ScatterParameters->TriangleNormalTextureWidth = TriangleNormalTextureWidth;
					ScatterParameters->TriangleNormalTextureHeight = TriangleNormalTextureHeight;
					ScatterParameters->CacheWorldMin = FVector3f(CacheWorldMin);
					ScatterParameters->CacheVoxelSize = CachedVoxelSize;
					GraphBuilder.AddPass(
						RDG_EVENT_NAME("CS.MeshGenerator.ScatterTrianglesToVoxelCache"),
						ScatterParameters,
						ERDGPassFlags::Compute,
						[ScatterParameters, ScatterShader, DirtyPageCount](FRHIComputeCommandList& InRHICmdList)
						{
							FComputeShaderUtils::Dispatch(InRHICmdList, ScatterShader, *ScatterParameters, FComputeShaderUtils::GetGroupCount(FIntVector(int32(DirtyPageCount), 1, 1), 64));
						});
				}
			}

			GraphBuilder.Execute();
		});

	UE_LOG(LogTemp, Verbose, TEXT("[AComputeShaderMeshGenerator] Dirty voxel triangle cache update queued. Actor=%s Dirty=%d Pages=%d Generation=%u"),
		*GetNameSafe(this),
		DirtyCellCount,
		DirtyPageCountForLog,
		CacheState.CacheGeneration);

	CacheState.DirtyCells.Reset();
}

// -----------------------------------------------------------------------------
// Dirty Cache System - Utilities
// -----------------------------------------------------------------------------

FBox AComputeShaderMeshGenerator::GetGeneratorBoundsWorldBox() const
{
	if (!GeneratorBounds)
	{
		return FBox(ForceInit);
	}

	const FBox BoundsBox = GeneratorBounds->Bounds.GetBox();
	if (BoundsBox.IsValid)
	{
		return BoundsBox;
	}

	const FVector SafeExtent = GeneratorBounds->GetScaledBoxExtent().ComponentMax(FVector::ZeroVector);
	return FBox(GeneratorBounds->GetComponentLocation() - SafeExtent, GeneratorBounds->GetComponentLocation() + SafeExtent);
}

FIntVector AComputeShaderMeshGenerator::ComputeGridSize(const FBox& InputWorldBounds) const
{
	if (!InputWorldBounds.IsValid)
	{
		return FIntVector::ZeroValue;
	}

	const FVector BoundsSize = InputWorldBounds.GetSize();
	const float SafeVoxelSize = FMath::Max(VoxelGridSettings.VoxelSize, CSGeneratorMinVoxelSize);
	return FIntVector(
		FMath::Max(1, FMath::CeilToInt(BoundsSize.X / SafeVoxelSize)),
		FMath::Max(1, FMath::CeilToInt(BoundsSize.Y / SafeVoxelSize)),
		FMath::Max(1, FMath::CeilToInt(BoundsSize.Z / SafeVoxelSize)));
}

FCSMeshGeneratorVoxelKey AComputeShaderMeshGenerator::WorldPositionToCell(FVector WorldPosition) const
{
	const float SafeVoxelSize = FMath::Max(CacheState.CachedVoxelSize, CSGeneratorMinVoxelSize);
	const FVector Local = (WorldPosition - CacheState.CachedWorldBounds.Min) / SafeVoxelSize;
	const FIntVector RawCell(
		FMath::FloorToInt(Local.X),
		FMath::FloorToInt(Local.Y),
		FMath::FloorToInt(Local.Z));

	return FCSMeshGeneratorVoxelKey(
		FMath::Clamp(RawCell.X, 0, FMath::Max(0, CacheState.GridSize.X - 1)),
		FMath::Clamp(RawCell.Y, 0, FMath::Max(0, CacheState.GridSize.Y - 1)),
		FMath::Clamp(RawCell.Z, 0, FMath::Max(0, CacheState.GridSize.Z - 1)));
}

FBox AComputeShaderMeshGenerator::GetCellWorldBounds(const FCSMeshGeneratorVoxelKey& Cell) const
{
	const float SafeVoxelSize = FMath::Max(CacheState.CachedVoxelSize, CSGeneratorMinVoxelSize);
	const FVector Min = CacheState.CachedWorldBounds.Min + FVector(Cell.X, Cell.Y, Cell.Z) * SafeVoxelSize;
	return FBox(Min, Min + FVector(SafeVoxelSize));
}

void AComputeShaderMeshGenerator::ReleaseCacheResources()
{
	if (VoxelMetaRT)
	{
		VoxelMetaRT->ReleaseResource();
		VoxelMetaRT = nullptr;
	}
	if (TriangleVertexRT)
	{
		TriangleVertexRT->ReleaseResource();
		TriangleVertexRT = nullptr;
	}
	if (TriangleNormalRT)
	{
		TriangleNormalRT->ReleaseResource();
		TriangleNormalRT = nullptr;
	}
}

void AComputeShaderMeshGenerator::ResetCacheRuntime(bool bClearRequests)
{
	ReleaseCacheResources();

	CacheState.CachedWorldBounds = FBox(ForceInit);
	CacheState.GridSize = FIntVector::ZeroValue;
	CacheState.CachedVoxelSize = 0.0f;
	CacheState.CachedMaxActiveVoxels = 0;
	CacheState.CachedMaxTrianglesPerVoxel = 0;
	CacheState.CachedLODIndex = 0;
	CacheState.CachedMaxTextureDimension = 0;
	CacheState.ActiveCells.Empty();
	CacheState.CellsToActivate.Empty();
	CacheState.CellsToDeactivate.Empty();
	CacheState.DirtyCells.Empty();
	CacheState.CellToPage.Empty();
	CacheState.FreePages.Empty();

	if (bClearRequests)
	{
		RequestActiveCells.Empty();
		LastRequests.Empty();
	}
	else
	{
		RequestActiveCells.Empty();
	}
}

void AComputeShaderMeshGenerator::InitializeFreePages()
{
	CacheState.FreePages.Reset();
	CacheState.FreePages.Reserve(CacheState.CachedMaxActiveVoxels);
	for (int32 PageIndex = CacheState.CachedMaxActiveVoxels - 1; PageIndex >= 0; --PageIndex)
	{
		CacheState.FreePages.Add(PageIndex);
	}
}

void AComputeShaderMeshGenerator::CreateCacheRenderTargets()
{
	ReleaseCacheResources();

	const int32 MaxActiveVoxels = FMath::Max(1, CacheState.CachedMaxActiveVoxels);
	const int32 MaxTrianglesPerVoxel = FMath::Max(1, CacheState.CachedMaxTrianglesPerVoxel);
	const int32 MaxDimension = FMath::Max(CSGeneratorMinTextureDimension, CacheState.CachedMaxTextureDimension);
	const int64 TotalTriangleSlots = int64(MaxActiveVoxels) * int64(MaxTrianglesPerVoxel);
	const int64 TotalVertexPixels = TotalTriangleSlots * 3ll;
	const int64 TotalNormalPixels = TotalTriangleSlots;

	const int32 MetaWidth = FMath::Min(MaxDimension, FMath::Max(CSGeneratorDefaultTextureDimension, MaxActiveVoxels));
	const int32 MetaHeight = FMath::Max(CSGeneratorDefaultTextureDimension, CeilDivInt64ToInt32(MaxActiveVoxels, MetaWidth));

	const int32 VertexWidth = FMath::Min<int32>(MaxDimension, FMath::Max<int64>(CSGeneratorDefaultTextureDimension, FMath::Min<int64>(TotalVertexPixels, MaxDimension)));
	const int32 VertexHeight = FMath::Max(CSGeneratorDefaultTextureDimension, CeilDivInt64ToInt32(TotalVertexPixels, VertexWidth));

	const int32 NormalWidth = FMath::Min<int32>(MaxDimension, FMath::Max<int64>(CSGeneratorDefaultTextureDimension, FMath::Min<int64>(TotalNormalPixels, MaxDimension)));
	const int32 NormalHeight = FMath::Max(CSGeneratorDefaultTextureDimension, CeilDivInt64ToInt32(TotalNormalPixels, NormalWidth));

	VoxelMetaRT = NewObject<UTextureRenderTarget2D>(
		this,
		MakeUniqueObjectName(this, UTextureRenderTarget2D::StaticClass(), TEXT("CSMeshGenerator_VoxelMetaRT")),
		RF_Transient);
	TriangleVertexRT = NewObject<UTextureRenderTarget2D>(
		this,
		MakeUniqueObjectName(this, UTextureRenderTarget2D::StaticClass(), TEXT("CSMeshGenerator_TriangleVertexRT")),
		RF_Transient);
	TriangleNormalRT = NewObject<UTextureRenderTarget2D>(
		this,
		MakeUniqueObjectName(this, UTextureRenderTarget2D::StaticClass(), TEXT("CSMeshGenerator_TriangleNormalRT")),
		RF_Transient);

	if (VoxelMetaRT)
	{
		VoxelMetaRT->RenderTargetFormat = RTF_RGBA32f;
		VoxelMetaRT->ClearColor = FLinearColor::Black;
		VoxelMetaRT->bCanCreateUAV = true;
		VoxelMetaRT->InitAutoFormat(MetaWidth, MetaHeight);
		VoxelMetaRT->UpdateResourceImmediate(true);
	}

	if (TriangleVertexRT)
	{
		TriangleVertexRT->RenderTargetFormat = RTF_RGBA32f;
		TriangleVertexRT->ClearColor = FLinearColor::Black;
		TriangleVertexRT->bCanCreateUAV = true;
		TriangleVertexRT->InitAutoFormat(VertexWidth, VertexHeight);
		TriangleVertexRT->UpdateResourceImmediate(true);
	}

	if (TriangleNormalRT)
	{
		TriangleNormalRT->RenderTargetFormat = RTF_RGBA32f;
		TriangleNormalRT->ClearColor = FLinearColor::Black;
		TriangleNormalRT->bCanCreateUAV = true;
		TriangleNormalRT->InitAutoFormat(NormalWidth, NormalHeight);
		TriangleNormalRT->UpdateResourceImmediate(true);
	}
}

void AComputeShaderMeshGenerator::RebuildRequestActiveCellsFromLastRequests()
{
	RequestActiveCells.Empty();
	for (const TPair<FName, FCSMeshGeneratorTriangleCacheRequest>& Pair : LastRequests)
	{
		const FCSMeshGeneratorTriangleCacheRequest& Request = Pair.Value;
		if (!Request.bPersistentInterest)
		{
			continue;
		}

		const float RequestActivationRadius = Request.ActivationRadiusOverride > 0.0f
			? Request.ActivationRadiusOverride
			: VoxelGridSettings.ActivationRadius;

		TSet<FCSMeshGeneratorVoxelKey> RebuiltCells;
		BuildActiveCellsFromReferencePoints(Request.CachedReferencePoints, RequestActivationRadius, RebuiltCells);
		RequestActiveCells.Add(Pair.Key, MoveTemp(RebuiltCells));
	}
}

bool AComputeShaderMeshGenerator::HasValidCacheResources() const
{
	return VoxelMetaRT != nullptr
		&& TriangleVertexRT != nullptr
		&& TriangleNormalRT != nullptr
		&& VoxelMetaRT->bCanCreateUAV
		&& TriangleVertexRT->bCanCreateUAV
		&& TriangleNormalRT->bCanCreateUAV
		&& VoxelMetaRT->SizeX > 0
		&& VoxelMetaRT->SizeY > 0
		&& TriangleVertexRT->SizeX > 0
		&& TriangleVertexRT->SizeY > 0
		&& TriangleNormalRT->SizeX > 0
		&& TriangleNormalRT->SizeY > 0;
}

bool AComputeShaderMeshGenerator::AreBoundsCompatible(const FBox& A, const FBox& B) const
{
	if (!A.IsValid || !B.IsValid)
	{
		return false;
	}

	const float Tolerance = FMath::Max(0.0f, VoxelGridSettings.BoundsTolerance);
	const double ToleranceSq = double(Tolerance) * double(Tolerance);
	return FVector::DistSquared(A.GetCenter(), B.GetCenter()) <= ToleranceSq
		&& FVector::DistSquared(A.GetExtent(), B.GetExtent()) <= ToleranceSq;
}

FName AComputeShaderMeshGenerator::NormalizeRequestId(FName RequestId) const
{
	return RequestId.IsNone() ? CSGeneratorDefaultRequestId : RequestId;
}
