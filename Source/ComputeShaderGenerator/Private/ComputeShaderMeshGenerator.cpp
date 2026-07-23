#include "ComputeShaderMeshGenerator.h"
#include "MeshGeneratorBrushCache.h"

#include "ComputeShaderBasicFunction.h"
#include "DynamicMeshActor.h"
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
#include "TimerManager.h"
#include "GameFramework/Actor.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#include "LandscapeProxy.h"
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

// Voxel cache cell key. The header unifies the cell key type on FIntVector; this alias keeps the
// legacy name used throughout the cache implementation pointing at that same type.
using FCSMeshGeneratorVoxelKey = FIntVector;

namespace
{
constexpr int32 CSGeneratorMinTextureDimension = 1;
constexpr int32 CSGeneratorDefaultTextureDimension = 1;
constexpr float CSGeneratorMinVoxelSize = 1.0e-3f;
const FName CSGeneratorDefaultRequestId(TEXT("Default"));


void ReleaseRTAndNull(TObjectPtr<UTextureRenderTarget2D>& RT)
{
	if (RT) { RT->ReleaseResource(); RT = nullptr; }
}

void InitCacheRT(UTextureRenderTarget2D* RT, int32 Width, int32 Height)
{
	if (!RT) { return; }
	RT->RenderTargetFormat = RTF_RGBA32f;
	RT->ClearColor = FLinearColor::Black;
	RT->bCanCreateUAV = true;
	RT->InitAutoFormat(Width, Height);
	RT->UpdateResourceImmediate(true);
}

bool IsValidCacheRT(const UTextureRenderTarget2D* RT)
{
	return RT && RT->bCanCreateUAV && RT->SizeX > 0 && RT->SizeY > 0;
}

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
	if (!RenderTarget || RenderTarget->SizeX <= 0 || RenderTarget->SizeY <= 0) return;

	const int32 PixelCount = RenderTarget->SizeX * RenderTarget->SizeY;
	if (Colors.Num() < PixelCount) Colors.SetNumZeroed(PixelCount);
	else if (Colors.Num() > PixelCount) Colors.SetNum(PixelCount, EAllowShrinking::No);

	UComputeShaderBasicFunction::DrawLinearColorsToRenderTarget32(RenderTarget, MoveTemp(Colors));
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
// Triangle Soup → Heightmap rasterization
// -----------------------------------------------------------------------------

class FTriangleSoupToHeightmapCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FTriangleSoupToHeightmapCS);
	SHADER_USE_PARAMETER_STRUCT(FTriangleSoupToHeightmapCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutTriangleVertices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_TriangleCounter)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, RW_HeightmapUint)
		SHADER_PARAMETER(FVector2f, HM_BoundsMin)
		SHADER_PARAMETER(FVector2f, HM_BoundsInvSize)
		SHADER_PARAMETER(float, HM_CameraHeight)
		SHADER_PARAMETER(FIntPoint, HM_TextureSize)
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

IMPLEMENT_GLOBAL_SHADER(FTriangleSoupToHeightmapCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "TriangleSoupToHeightmapCS", SF_Compute);

class FConvertHeightmapUintToFloatCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FConvertHeightmapUintToFloatCS);
	SHADER_USE_PARAMETER_STRUCT(FConvertHeightmapUintToFloatCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint>, T_HeightmapUint)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_HeightmapFloat)
		SHADER_PARAMETER(FIntPoint, HM_TextureSize)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FConvertHeightmapUintToFloatCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "ConvertHeightmapUintToFloatCS", SF_Compute);

class FLandscapeG16ToDepthCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FLandscapeG16ToDepthCS);
	SHADER_USE_PARAMETER_STRUCT(FLandscapeG16ToDepthCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, T_LandscapeRGBA)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_HeightmapFloat)
		SHADER_PARAMETER(float, LHM_CameraHeight)
		SHADER_PARAMETER(float, LHM_LandscapeScaleZ)
		SHADER_PARAMETER(float, LHM_LandscapeOriginZ)
		SHADER_PARAMETER(FIntPoint, LHM_TextureSize)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FLandscapeG16ToDepthCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "LandscapeG16ToDepthCS", SF_Compute);

class FLandscapeG16ToNormalHeightCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FLandscapeG16ToNormalHeightCS);
	SHADER_USE_PARAMETER_STRUCT(FLandscapeG16ToNormalHeightCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, T_LandscapeRGBA)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_HeightmapFloat)
		SHADER_PARAMETER(float, LHM_LandscapeScaleZ)
		SHADER_PARAMETER(float, LHM_LandscapeOriginZ)
		SHADER_PARAMETER(FIntPoint, LHM_TextureSize)
		SHADER_PARAMETER(FVector2f, LHM_TexelWorldSize)
		SHADER_PARAMETER(uint32, LHM_MergeByMaxZ)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FLandscapeG16ToNormalHeightCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "LandscapeG16ToNormalHeightCS", SF_Compute);

class FLandscapeHeightmapToTrianglesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FLandscapeHeightmapToTrianglesCS);
	SHADER_USE_PARAMETER_STRUCT(FLandscapeHeightmapToTrianglesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, T_LandscapeRGBA)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, RW_TriangleVerts)
		SHADER_PARAMETER(float, LHM_LandscapeScaleZ)
		SHADER_PARAMETER(float, LHM_LandscapeOriginZ)
		SHADER_PARAMETER(FIntPoint, LHM_TextureSize)
		SHADER_PARAMETER(FVector2f, LHM_WorldOriginXY)
		SHADER_PARAMETER(FVector2f, LHM_TexelWorldSize)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FLandscapeHeightmapToTrianglesCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "LandscapeHeightmapToTrianglesCS", SF_Compute);

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
bool ShouldExcludeStaticMeshTriangleRequest(const FCSStaticMeshTriangleRequest& Request, const AActor* ExcludedActor, const TArray<FName>& ExcludedActorTags);

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

bool ShouldExcludeStaticMeshTriangleRequest(const FCSStaticMeshTriangleRequest& Request, const AActor* ExcludedActor, const TArray<FName>& ExcludedActorTags)
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

	for (const FName& Tag : ExcludedActorTags)
	{
		if (!Tag.IsNone() && SourceActor->ActorHasTag(Tag))
		{
			return true;
		}
	}
	return false;
}

uint64 ResolveStaticMeshTriangleRequests(
	const TArray<FCSStaticMeshTriangleRequest>& Requests,
	const AActor* ExcludedActor,
	const TArray<FName>& ExcludedActorTags,
	bool bNaniteOnlyFallbackMesh,
	TArray<FResolvedStaticMeshTriangleRequest>& OutResolvedRequests)
{
	OutResolvedRequests.Reset();
	OutResolvedRequests.Reserve(Requests.Num());

	uint64 TotalTriangleCount = 0;
	for (const FCSStaticMeshTriangleRequest& Request : Requests)
	{
		if (ShouldExcludeStaticMeshTriangleRequest(Request, ExcludedActor, ExcludedActorTags))
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
	FCSTriangleMeshData& OutTriangleData,
	const FTransform* WorldToLocalBoxTransform = nullptr,
	const FVector* LocalBoxExtent = nullptr,
	FName RequiredActorTag = NAME_None,
	bool bSortComponentsByDistance = true)
{
	OutTriangleData = FCSTriangleMeshData();

#if WITH_EDITOR
	if (!World || !QueryBox.IsValid || MaxTriangles == 0)
	{
		return;
	}

	const bool bHasOBBFilter = WorldToLocalBoxTransform && LocalBoxExtent;

	struct FLandscapeComponentEntry
	{
		ULandscapeComponent* Component;
		double DistSqToCenter;
	};
	TArray<FLandscapeComponentEntry> CandidateComponents;
	const FVector QueryCenter = QueryBox.GetCenter();

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

		if (!RequiredActorTag.IsNone())
		{
			ALandscapeProxy* LandscapeProxy = LandscapeComponent->GetLandscapeProxy();
			if (!LandscapeProxy || !LandscapeProxy->ActorHasTag(RequiredActorTag))
			{
				continue;
			}
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

		const double DistSq = FVector::DistSquared(LandscapeComponent->Bounds.Origin, QueryCenter);
		CandidateComponents.Add({ LandscapeComponent, DistSq });
	}

	if (bSortComponentsByDistance)
	{
		CandidateComponents.Sort([](const FLandscapeComponentEntry& A, const FLandscapeComponentEntry& B)
		{
			return A.DistSqToCenter < B.DistSqToCenter;
		});
	}

	for (const FLandscapeComponentEntry& Entry : CandidateComponents)
	{
		FLandscapeComponentDataInterface LandscapeData(Entry.Component, 0, false);
		if (!LandscapeData.GetRawHeightData())
		{
			continue;
		}

		const int32 ComponentSizeQuads = Entry.Component->ComponentSizeQuads;
		for (int32 Y = 0; Y < ComponentSizeQuads; ++Y)
		{
			for (int32 X = 0; X < ComponentSizeQuads; ++X)
			{
				if (MaxTriangles > 0 && GetTriangleMeshDataTriangleCount(OutTriangleData) >= MaxTriangles)
				{
					return;
				}

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

				auto PassesAllFilters = [&](const FVector& A, const FVector& B, const FVector& C) -> bool
				{
					if (!TriangleIntersectsBox(A, B, C, QueryBox))
					{
						return false;
					}
					if (bHasOBBFilter)
					{
						FBox LocalTriBox(ForceInit);
						LocalTriBox += WorldToLocalBoxTransform->TransformPosition(A);
						LocalTriBox += WorldToLocalBoxTransform->TransformPosition(B);
						LocalTriBox += WorldToLocalBoxTransform->TransformPosition(C);
						if (!LocalTriBox.Intersect(FBox(-*LocalBoxExtent, *LocalBoxExtent)))
						{
							return false;
						}
					}
					if (!PassTriangleReferenceFilterCPU(A, B, C, ReferencePoints, ReferenceFilterDistance))
					{
						return false;
					}
					return true;
				};

				if (PassesAllFilters(P00, P10, P11))
				{
					const FVector Tri0Normal = MakeLandscapeNormalFaceUp(N00 + N10 + N11);
					if (!TryAppendTriangleSoupOrientedToNormal(OutTriangleData, P00, P10, P11, Tri0Normal, MaxTriangles))
					{
						return;
					}
				}

				if (PassesAllFilters(P00, P11, P01))
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
#else
	(void)World; (void)QueryBox; (void)ReferencePoints; (void)ReferenceFilterDistance;
	(void)MaxTriangles; (void)OutTriangleData; (void)WorldToLocalBoxTransform;
	(void)LocalBoxExtent; (void)RequiredActorTag; (void)bSortComponentsByDistance;
#endif
}

} // close anonymous namespace for member function definitions

// Public static wrappers
void AComputeShaderMeshGenerator::BuildBoxSceneLandscapeTriangles(UWorld* World,
	const FBox& QueryBox,
	const TArray<FVector>& InReferencePoints,
	float InReferenceFilterDistance,
	int32 MaxTriangles,
	FCSTriangleMeshData& OutTriangleData)
{
	BuildBoxSceneLandscapeTrianglesInternal(World, QueryBox, InReferencePoints,
		InReferenceFilterDistance, MaxTriangles, OutTriangleData);
}

void AComputeShaderMeshGenerator::BuildBoxSceneLandscapeTriangles(UWorld* World,
	const FBox& QueryBox,
	const TArray<FVector>& InReferencePoints,
	float InReferenceFilterDistance,
	int32 MaxTriangles,
	FCSTriangleMeshData& OutTriangleData,
	const FTransform* WorldToLocalBoxTransform,
	const FVector* LocalBoxExtent,
	FName RequiredActorTag,
	bool bSortComponentsByDistance)
{
	BuildBoxSceneLandscapeTrianglesInternal(World, QueryBox, InReferencePoints,
		InReferenceFilterDistance, MaxTriangles, OutTriangleData,
		WorldToLocalBoxTransform, LocalBoxExtent, RequiredActorTag, bSortComponentsByDistance);
}

namespace { // reopen anonymous namespace

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
	const TArray<FName>& ExcludedActorTags,
	bool bNaniteOnlyFallbackMesh = true)
{
	TArray<FResolvedStaticMeshTriangleRequest> ResolvedRequests;
	const uint64 TotalTriangleCount = ResolveStaticMeshTriangleRequests(
		Requests,
		ExcludedActor,
		ExcludedActorTags,
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
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 2),
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



// -----------------------------------------------------------------------------
// PrepareBoxSceneTriangles / AddPreparedBoxSceneTrianglesToRDG
// -----------------------------------------------------------------------------

struct FCSBoxScenePreparedDataImpl
{
	TArray<FResolvedStaticMeshTriangleRequest> ResolvedRequests;
	uint64 TotalStaticMeshTriangleCount = 0;
	FCSTriangleMeshData LandscapeTriangleData;
	TArray<FVector> ReferencePoints;
	float ReferenceFilterDistance = 0.0f;
	int32 SafeMaxTriangles = 1;
};

bool FCSBoxScenePreparedData::HasAnyTriangles() const
{
	if (!Impl) return false;
	return !Impl->ResolvedRequests.IsEmpty() || GetTriangleMeshDataTriangleCount(Impl->LandscapeTriangleData) > 0;
}

FCSBoxScenePreparedData AComputeShaderMeshGenerator::PrepareBoxSceneTriangles(
	UWorld* World,
	const FBox& QueryBox,
	int32 InMaxTriangles,
	const TArray<FVector>& InReferencePoints,
	float InReferenceFilterDistance,
	FName RequiredActorTag)
{
	FCSBoxScenePreparedData Result;
	if (!World || !QueryBox.IsValid) return Result;

	auto ImplData = MakeShared<FCSBoxScenePreparedDataImpl, ESPMode::ThreadSafe>();

	const int32 SafeMaxTriangles = InMaxTriangles > 0 ? InMaxTriangles : FMath::Max(1, MaxTriangles);
	const float SafeRefDist = InReferencePoints.IsEmpty() ? 0.0f : FMath::Max(0.0f, InReferenceFilterDistance);

	TArray<FCSStaticMeshTriangleRequest> Requests;
	BuildBoxSceneTriangleRequests(World, QueryBox, Requests);

	if (!RequiredActorTag.IsNone())
	{
		Requests.RemoveAll([RequiredActorTag](const FCSStaticMeshTriangleRequest& Req)
		{
			return Req.SourceActor && !Req.SourceActor->ActorHasTag(RequiredActorTag);
		});
	}

	BuildBoxSceneLandscapeTrianglesInternal(
		World, QueryBox, InReferencePoints, SafeRefDist, SafeMaxTriangles,
		ImplData->LandscapeTriangleData);

	ImplData->TotalStaticMeshTriangleCount = ResolveStaticMeshTriangleRequests(
		Requests, this, ExcludedActorTags, true, ImplData->ResolvedRequests);

	ImplData->ReferencePoints = InReferencePoints;
	ImplData->ReferenceFilterDistance = SafeRefDist;
	ImplData->SafeMaxTriangles = SafeMaxTriangles;

	Result.Impl = ImplData;
	return Result;
}

FCSStaticMeshTriangleRDGOutput AComputeShaderMeshGenerator::AddPreparedBoxSceneTrianglesToRDG(
	FRDGBuilder& GraphBuilder,
	FRHICommandListImmediate& RHICmdList,
	const FCSBoxScenePreparedData& Prepared,
	const TCHAR* DebugName)
{
	FCSStaticMeshTriangleRDGOutput Output;
	if (!Prepared.IsValid() || !Prepared.HasAnyTriangles()) return Output;

	const auto& D = *Prepared.Impl;
	const FCSTriangleMeshData* InitialTriangleData =
		GetTriangleMeshDataTriangleCount(D.LandscapeTriangleData) > 0 ? &D.LandscapeTriangleData : nullptr;

	Output = AddResolvedStaticMeshTrianglesToRDGInternal(
		GraphBuilder, RHICmdList, D.ResolvedRequests, D.TotalStaticMeshTriangleCount,
		D.ReferencePoints, D.ReferenceFilterDistance, D.SafeMaxTriangles,
		InitialTriangleData, DebugName);

	return Output;
}

FCSTriangleMeshData AComputeShaderMeshGenerator::GetBoxSceneTrianglesFromGPUFiltered(float ReferenceFilterDistance)
{
	FCSTriangleMeshData ResultTriangleData;
	ClearTriangleTextureData();

	UWorld* World = GetWorld();
	if (!World)
	{
		return ResultTriangleData;
	}

	const FBox QueryBox = GetGeneratorBoundsWorldBox();
	if (!QueryBox.IsValid)
	{
		return ResultTriangleData;
	}

	const TArray<FVector> ReferencePointsForRender = ReferencePoints;
	const float SafeFilterDistance = (ReferencePointsForRender.IsEmpty()) ? 0.0f : FMath::Max(0.0f, ReferenceFilterDistance);
	const int32 SafeMaxTriangles = FMath::Max(1, MaxTriangles);

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
		return ResultTriangleData;
	}

	TArray<FResolvedStaticMeshTriangleRequest> ResolvedRequests;
	const uint64 TotalStaticMeshTriangleCount = ResolveStaticMeshTriangleRequests(
		Requests,
		this,
		ExcludedActorTags,
		true,
		ResolvedRequests);
	if (ResolvedRequests.IsEmpty() && !bHasLandscapeTriangles)
	{
		return ResultTriangleData;
	}

	FRHIGPUBufferReadback* VertexReadback = new FRHIGPUBufferReadback(TEXT("BoxSceneTrianglesFiltered_VertexReadback"));
	FRHIGPUBufferReadback* NormalReadback = new FRHIGPUBufferReadback(TEXT("BoxSceneTrianglesFiltered_NormalReadback"));
	FRHIGPUBufferReadback* CounterReadback = new FRHIGPUBufferReadback(TEXT("BoxSceneTrianglesFiltered_CounterReadback"));
	const uint32 CounterReadbackBytes = sizeof(uint32);
	bool bRenderWorkQueued = false;
	bool bHasGPUOutput = false;
	int32 VertexCapacity = 0;
	uint32 ActualVertexReadbackBytes = 0;

	ENQUEUE_RENDER_COMMAND(GetBoxSceneTrianglesFromGPUFilteredGPU)(
		[ResolvedRequests = MoveTemp(ResolvedRequests), TotalStaticMeshTriangleCount, LandscapeTriangleData = MoveTemp(LandscapeTriangleData),
		 VertexReadback, NormalReadback, CounterReadback, CounterReadbackBytes,
		 ReferencePointsForRender, SafeFilterDistance, SafeMaxTriangles,
		 &bRenderWorkQueued, &bHasGPUOutput, &VertexCapacity, &ActualVertexReadbackBytes](FRHICommandListImmediate& RHICmdList)
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
				TEXT("CS.BoxSceneTrianglesFiltered"));

			if (TriangleOutput.TriangleVertices && TriangleOutput.TriangleNormals && TriangleOutput.TriangleCounter)
			{
				VertexCapacity = int32(TriangleOutput.MaxVertices);
				ActualVertexReadbackBytes = uint32(uint64(TriangleOutput.MaxVertices) * sizeof(FVector4f));
				AddEnqueueCopyPass(GraphBuilder, VertexReadback, TriangleOutput.TriangleVertices, ActualVertexReadbackBytes);
				AddEnqueueCopyPass(GraphBuilder, NormalReadback, TriangleOutput.TriangleNormals, ActualVertexReadbackBytes);
				AddEnqueueCopyPass(GraphBuilder, CounterReadback, TriangleOutput.TriangleCounter, CounterReadbackBytes);
				bHasGPUOutput = true;
			}

			GraphBuilder.Execute();
			bRenderWorkQueued = true;
		});

	FlushRenderingCommands();

	if (!bRenderWorkQueued || !bHasGPUOutput || VertexCapacity <= 0)
	{
		delete VertexReadback;
		delete NormalReadback;
		delete CounterReadback;
		return ResultTriangleData;
	}

	TArray<FVector4f> VertexData;
	TArray<FVector4f> NormalData;
	VertexData.SetNumZeroed(VertexCapacity);
	NormalData.SetNumZeroed(VertexCapacity);
	uint32 TriangleCount = 0;
	bool bReadbackSucceeded = false;

	ENQUEUE_RENDER_COMMAND(GetBoxSceneTrianglesFromGPUFilteredReadback)(
		[VertexReadback, NormalReadback, CounterReadback, ActualVertexReadbackBytes, CounterReadbackBytes,
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
				UE_LOG(LogTemp, Warning, TEXT("[GetBoxSceneTrianglesFromGPUFiltered] GPU readback was not ready after flush."));
				delete VertexReadback;
				delete NormalReadback;
				delete CounterReadback;
				return;
			}

			if (VertexReadback->GetGPUSizeBytes() < ActualVertexReadbackBytes ||
				NormalReadback->GetGPUSizeBytes() < ActualVertexReadbackBytes ||
				CounterReadback->GetGPUSizeBytes() < CounterReadbackBytes)
			{
				UE_LOG(LogTemp, Warning, TEXT("[GetBoxSceneTrianglesFromGPUFiltered] GPU readback size mismatch. Vertex=%llu/%u Normal=%llu/%u Counter=%llu/%u"),
					VertexReadback->GetGPUSizeBytes(),
					ActualVertexReadbackBytes,
					NormalReadback->GetGPUSizeBytes(),
					ActualVertexReadbackBytes,
					CounterReadback->GetGPUSizeBytes(),
					CounterReadbackBytes);
				delete VertexReadback;
				delete NormalReadback;
				delete CounterReadback;
				return;
			}

			bool bLockedAll = true;
			if (const FVector4f* VertexPtr = static_cast<const FVector4f*>(VertexReadback->Lock(ActualVertexReadbackBytes)))
			{
				FMemory::Memcpy(VertexData.GetData(), VertexPtr, ActualVertexReadbackBytes);
				VertexReadback->Unlock();
			}
			else
			{
				bLockedAll = false;
			}

			if (const FVector4f* NormalPtr = static_cast<const FVector4f*>(NormalReadback->Lock(ActualVertexReadbackBytes)))
			{
				FMemory::Memcpy(NormalData.GetData(), NormalPtr, ActualVertexReadbackBytes);
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
		return ResultTriangleData;
	}

	const int32 MaxTriangleCapacity = VertexCapacity / 3;
	const int32 SafeTriangleCount = FMath::Clamp<int32>(int32(TriangleCount), 0, MaxTriangleCapacity);
	const int32 EffectiveVertexCount = FMath::Clamp(SafeTriangleCount * 3, 0, VertexData.Num());
	if (EffectiveVertexCount <= 0)
	{
		StoreTriangleTextureData(ResultTriangleData, SafeFilterDistance, QueryBox);
		return ResultTriangleData;
	}

	ResultTriangleData.Vertices.Reserve(EffectiveVertexCount);
	ResultTriangleData.VertexNormals.Reserve(EffectiveVertexCount);
	for (int32 VertexIndex = 0; VertexIndex < EffectiveVertexCount; ++VertexIndex)
	{
		const FVector4f& Vertex = VertexData[VertexIndex];
		ResultTriangleData.Vertices.Add(FVector(Vertex.X, Vertex.Y, Vertex.Z));

		const FVector4f& Normal = NormalData[VertexIndex];
		ResultTriangleData.VertexNormals.Add(FVector(Normal.X, Normal.Y, Normal.Z));
	}
	ResultTriangleData.VertexCount = EffectiveVertexCount;
	ResultTriangleData.IndexCount = 0;

	StoreTriangleTextureData(ResultTriangleData, SafeFilterDistance, QueryBox);
	return ResultTriangleData;
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
		ExcludedActorTags,
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

	const uint32 CounterReadbackBytes = sizeof(uint32) * 2;

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
		 ReferencePointsForRender, SafeFilterDistance, SafeSurfaceVoxelBlurIterations, SafeSurfaceVoxelBlurRadius,
		 &bRenderWorkQueued, &bHasGPUOutput, &VoxelCapacity, &ActualVoxelReadbackBytes, &ActualVoxelCellReadbackBytes](FRHICommandListImmediate& RHICmdList)
		{
			const uint32 VoxCap = uint32(FMath::Max(1, SafeMaxVoxels));
			const uint32 HashSlotCount = GetSurfaceVoxelHashSlotCount(SafeMaxVoxels, 0);
			const float SafeSurfaceThickness = SafeVoxelSize * 0.5f;

			TRefCountPtr<FRDGPooledBuffer> VoxelPositionsBuf = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), VoxCap), TEXT("CS.SV.Positions"));
			TRefCountPtr<FRDGPooledBuffer> VoxelNormalsBuf = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), VoxCap), TEXT("CS.SV.Normals"));
			TRefCountPtr<FRDGPooledBuffer> VoxelCounterBuf = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 2), TEXT("CS.SV.Counter"));
			TRefCountPtr<FRDGPooledBuffer> VoxelHashSlotsBuf = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), HashSlotCount), TEXT("CS.SV.HashSlots"));
			TRefCountPtr<FRDGPooledBuffer> VoxelHashIndicesBuf = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), HashSlotCount), TEXT("CS.SV.HashIndices"));
			TRefCountPtr<FRDGPooledBuffer> VoxelNormalSumsBuf = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(int32), VoxCap * 4u), TEXT("CS.SV.NormalSums"));
			TRefCountPtr<FRDGPooledBuffer> VoxelNormalCountsBuf = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), VoxCap), TEXT("CS.SV.NormalCounts"));
			TRefCountPtr<FRDGPooledBuffer> VoxelTargetPositionsBuf = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), VoxCap), TEXT("CS.SV.TargetPositions"));
			TRefCountPtr<FRDGPooledBuffer> VoxelTargetOffsetSumsBuf = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(int32), VoxCap * 4u), TEXT("CS.SV.TargetOffsetSums"));
			TRefCountPtr<FRDGPooledBuffer> VoxelTargetWeightSumsBuf = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), VoxCap), TEXT("CS.SV.TargetWeightSums"));
			TRefCountPtr<FRDGPooledBuffer> VoxelCellsBuf = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(int32) * 4, VoxCap), TEXT("CS.SV.Cells"));
			TRefCountPtr<FRDGPooledBuffer> BlurredNormalsBuf = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), VoxCap), TEXT("CS.SV.BlurredNormals"));
			TRefCountPtr<FRDGPooledBuffer> BlurredTargetsBuf = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), VoxCap), TEXT("CS.SV.BlurredTargets"));

			{
				FRDGBuilder InitGraph(RHICmdList);
				FRDGBufferRef RDG_Positions = InitGraph.RegisterExternalBuffer(VoxelPositionsBuf);
				FRDGBufferRef RDG_Normals = InitGraph.RegisterExternalBuffer(VoxelNormalsBuf);
				FRDGBufferRef RDG_Counter = InitGraph.RegisterExternalBuffer(VoxelCounterBuf);
				FRDGBufferRef RDG_HashSlots = InitGraph.RegisterExternalBuffer(VoxelHashSlotsBuf);
				FRDGBufferRef RDG_HashIndices = InitGraph.RegisterExternalBuffer(VoxelHashIndicesBuf);
				FRDGBufferRef RDG_NormalSums = InitGraph.RegisterExternalBuffer(VoxelNormalSumsBuf);
				FRDGBufferRef RDG_NormalCounts = InitGraph.RegisterExternalBuffer(VoxelNormalCountsBuf);
				FRDGBufferRef RDG_TargetPositions = InitGraph.RegisterExternalBuffer(VoxelTargetPositionsBuf);
				FRDGBufferRef RDG_TargetOffsetSums = InitGraph.RegisterExternalBuffer(VoxelTargetOffsetSumsBuf);
				FRDGBufferRef RDG_TargetWeightSums = InitGraph.RegisterExternalBuffer(VoxelTargetWeightSumsBuf);
				FRDGBufferRef RDG_Cells = InitGraph.RegisterExternalBuffer(VoxelCellsBuf);
				FRDGBufferRef RDG_BlurredNormals = InitGraph.RegisterExternalBuffer(BlurredNormalsBuf);
				FRDGBufferRef RDG_BlurredTargets = InitGraph.RegisterExternalBuffer(BlurredTargetsBuf);
				AddClearUAVPass(InitGraph, InitGraph.CreateUAV(FRDGBufferUAVDesc(RDG_Positions, PF_A32B32G32R32F)), 0.0f);
				AddClearUAVPass(InitGraph, InitGraph.CreateUAV(FRDGBufferUAVDesc(RDG_Normals, PF_A32B32G32R32F)), 0.0f);
				AddClearUAVPass(InitGraph, InitGraph.CreateUAV(FRDGBufferUAVDesc(RDG_Counter, PF_R32_UINT)), 0u);
				AddClearUAVPass(InitGraph, InitGraph.CreateUAV(FRDGBufferUAVDesc(RDG_HashSlots, PF_R32_UINT)), 0u);
				AddClearUAVPass(InitGraph, InitGraph.CreateUAV(FRDGBufferUAVDesc(RDG_HashIndices, PF_R32_UINT)), 0u);
				AddClearUAVPass(InitGraph, InitGraph.CreateUAV(FRDGBufferUAVDesc(RDG_NormalSums, PF_R32_SINT)), 0u);
				AddClearUAVPass(InitGraph, InitGraph.CreateUAV(FRDGBufferUAVDesc(RDG_NormalCounts, PF_R32_UINT)), 0u);
				AddClearUAVPass(InitGraph, InitGraph.CreateUAV(FRDGBufferUAVDesc(RDG_TargetPositions, PF_A32B32G32R32F)), 0.0f);
				AddClearUAVPass(InitGraph, InitGraph.CreateUAV(FRDGBufferUAVDesc(RDG_TargetOffsetSums, PF_R32_SINT)), 0u);
				AddClearUAVPass(InitGraph, InitGraph.CreateUAV(FRDGBufferUAVDesc(RDG_TargetWeightSums, PF_R32_UINT)), 0u);
				AddClearUAVPass(InitGraph, InitGraph.CreateUAV(FRDGBufferUAVDesc(RDG_Cells, PF_R32G32B32A32_UINT)), 0u);
				AddClearUAVPass(InitGraph, InitGraph.CreateUAV(FRDGBufferUAVDesc(RDG_BlurredNormals, PF_A32B32G32R32F)), 0.0f);
				AddClearUAVPass(InitGraph, InitGraph.CreateUAV(FRDGBufferUAVDesc(RDG_BlurredTargets, PF_A32B32G32R32F)), 0.0f);
				InitGraph.Execute();
			}

			const int32 BatchTriangleCap = FMath::Min(SafeMaxTriangles, 500000);
			const FCSTriangleMeshData* InitialTriangleData = GetTriangleMeshDataTriangleCount(LandscapeTriangleData) > 0 ? &LandscapeTriangleData : nullptr;

			TArray<TArray<FResolvedStaticMeshTriangleRequest>> Batches;
			{
				TArray<FResolvedStaticMeshTriangleRequest> CurrentBatch;
				int64 CurrentBatchTriCount = 0;
				for (int32 i = 0; i < ResolvedRequests.Num(); ++i)
				{
					if (CurrentBatchTriCount + ResolvedRequests[i].TriangleCount > int64(BatchTriangleCap) && CurrentBatch.Num() > 0)
					{
						Batches.Add(MoveTemp(CurrentBatch));
						CurrentBatch.Reset();
						CurrentBatchTriCount = 0;
					}
					CurrentBatch.Add(ResolvedRequests[i]);
					CurrentBatchTriCount += ResolvedRequests[i].TriangleCount;
				}
				if (CurrentBatch.Num() > 0)
				{
					Batches.Add(MoveTemp(CurrentBatch));
				}
			}

			TShaderMapRef<FTriangleSurfaceVoxelsCS> VoxelShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

			for (int32 BatchIdx = 0; BatchIdx < Batches.Num(); ++BatchIdx)
			{
				const TArray<FResolvedStaticMeshTriangleRequest>& Batch = Batches[BatchIdx];
				uint64 BatchTriCount = 0;
				for (const auto& Req : Batch) BatchTriCount += Req.TriangleCount;

				FRDGBuilder BatchGraph(RHICmdList);

				const FCSTriangleMeshData* BatchInitData = (BatchIdx == 0) ? InitialTriangleData : nullptr;
				FCSStaticMeshTriangleRDGOutput TriOut = AddResolvedStaticMeshTrianglesToRDGInternal(
					BatchGraph, RHICmdList, Batch, BatchTriCount, ReferencePointsForRender, SafeFilterDistance,
					BatchTriangleCap, BatchInitData, TEXT("CS.FilteredSV.Batch.Tri"));

				if (TriOut.TriangleVertices && TriOut.TriangleNormals && TriOut.TriangleCounter && TriOut.MaxTriangles > 0)
				{
					FRDGBufferRef ExtPositions = BatchGraph.RegisterExternalBuffer(VoxelPositionsBuf);
					FRDGBufferRef ExtNormals = BatchGraph.RegisterExternalBuffer(VoxelNormalsBuf);
					FRDGBufferRef ExtCounter = BatchGraph.RegisterExternalBuffer(VoxelCounterBuf);
					FRDGBufferRef ExtHashSlots = BatchGraph.RegisterExternalBuffer(VoxelHashSlotsBuf);
					FRDGBufferRef ExtHashIndices = BatchGraph.RegisterExternalBuffer(VoxelHashIndicesBuf);
					FRDGBufferRef ExtNormalSums = BatchGraph.RegisterExternalBuffer(VoxelNormalSumsBuf);
					FRDGBufferRef ExtNormalCounts = BatchGraph.RegisterExternalBuffer(VoxelNormalCountsBuf);
					FRDGBufferRef ExtTargetPositions = BatchGraph.RegisterExternalBuffer(VoxelTargetPositionsBuf);
					FRDGBufferRef ExtTargetOffsetSums = BatchGraph.RegisterExternalBuffer(VoxelTargetOffsetSumsBuf);
					FRDGBufferRef ExtTargetWeightSums = BatchGraph.RegisterExternalBuffer(VoxelTargetWeightSumsBuf);
					FRDGBufferRef ExtCells = BatchGraph.RegisterExternalBuffer(VoxelCellsBuf);

					FRDGBufferSRVRef TriVertsSRV = TriOut.TriangleVerticesSRV ? TriOut.TriangleVerticesSRV : BatchGraph.CreateSRV(FRDGBufferSRVDesc(TriOut.TriangleVertices, PF_A32B32G32R32F));
					FRDGBufferSRVRef TriNormsSRV = TriOut.TriangleNormalsSRV ? TriOut.TriangleNormalsSRV : BatchGraph.CreateSRV(FRDGBufferSRVDesc(TriOut.TriangleNormals, PF_A32B32G32R32F));
					FRDGBufferSRVRef TriCounterSRV = TriOut.TriangleCounterSRV ? TriOut.TriangleCounterSRV : BatchGraph.CreateSRV(FRDGBufferSRVDesc(TriOut.TriangleCounter, PF_R32_UINT));

					FTriangleSurfaceVoxelsCS::FParameters* VP = BatchGraph.AllocParameters<FTriangleSurfaceVoxelsCS::FParameters>();
					VP->TriangleVertices = TriVertsSRV;
					VP->TriangleNormals = TriNormsSRV;
					VP->SurfaceTriangleCounter = TriCounterSRV;
					VP->RW_OutVoxelPositions = BatchGraph.CreateUAV(FRDGBufferUAVDesc(ExtPositions, PF_A32B32G32R32F));
					VP->RW_OutVoxelNormals = BatchGraph.CreateUAV(FRDGBufferUAVDesc(ExtNormals, PF_A32B32G32R32F));
					VP->RW_SurfaceVoxelCounter = BatchGraph.CreateUAV(FRDGBufferUAVDesc(ExtCounter, PF_R32_UINT));
					VP->RW_SurfaceVoxelHashSlots = BatchGraph.CreateUAV(FRDGBufferUAVDesc(ExtHashSlots, PF_R32_UINT));
					VP->RW_SurfaceVoxelHashIndices = BatchGraph.CreateUAV(FRDGBufferUAVDesc(ExtHashIndices, PF_R32_UINT));
					VP->RW_SurfaceVoxelNormalSums = BatchGraph.CreateUAV(FRDGBufferUAVDesc(ExtNormalSums, PF_R32_SINT));
					VP->RW_SurfaceVoxelNormalCounts = BatchGraph.CreateUAV(FRDGBufferUAVDesc(ExtNormalCounts, PF_R32_UINT));
					VP->RW_OutVoxelTargetPositions = BatchGraph.CreateUAV(FRDGBufferUAVDesc(ExtTargetPositions, PF_A32B32G32R32F));
					VP->RW_SurfaceVoxelTargetOffsetSums = BatchGraph.CreateUAV(FRDGBufferUAVDesc(ExtTargetOffsetSums, PF_R32_SINT));
					VP->RW_SurfaceVoxelTargetWeightSums = BatchGraph.CreateUAV(FRDGBufferUAVDesc(ExtTargetWeightSums, PF_R32_UINT));
					VP->RW_OutVoxelCells = BatchGraph.CreateUAV(FRDGBufferUAVDesc(ExtCells, PF_R32G32B32A32_UINT));
					VP->SurfaceVoxelOrigin = FVector3f(VoxelOrigin);
					VP->SurfaceVoxelSize = SafeVoxelSize;
					VP->SurfaceThickness = SafeSurfaceThickness;
					VP->SurfaceTriangleCount = TriOut.MaxTriangles;
					VP->SurfaceVoxelCapacity = VoxCap;
					VP->SurfaceVoxelHashSlotCount = HashSlotCount;

					BatchGraph.AddPass(
						RDG_EVENT_NAME("CS.FilteredSV.Batch%d.Voxelize", BatchIdx),
						VP, ERDGPassFlags::Compute,
						[VP, VoxelShader, TriCap = TriOut.MaxTriangles](FRHIComputeCommandList& Cmd)
						{
							FComputeShaderUtils::Dispatch(Cmd, VoxelShader, *VP, FComputeShaderUtils::GetGroupCount(FIntVector(int32(TriCap), 1, 1), 64));
						});
				}
				BatchGraph.Execute();
			}

			{
				FRDGBuilder FinalGraph(RHICmdList);

				FRDGBufferRef FPositions = FinalGraph.RegisterExternalBuffer(VoxelPositionsBuf);
				FRDGBufferRef FNormals = FinalGraph.RegisterExternalBuffer(VoxelNormalsBuf);
				FRDGBufferRef FCounter = FinalGraph.RegisterExternalBuffer(VoxelCounterBuf);
				FRDGBufferRef FNormalSums = FinalGraph.RegisterExternalBuffer(VoxelNormalSumsBuf);
				FRDGBufferRef FNormalCounts = FinalGraph.RegisterExternalBuffer(VoxelNormalCountsBuf);
				FRDGBufferRef FTargetPositions = FinalGraph.RegisterExternalBuffer(VoxelTargetPositionsBuf);
				FRDGBufferRef FTargetOffsetSums = FinalGraph.RegisterExternalBuffer(VoxelTargetOffsetSumsBuf);
				FRDGBufferRef FTargetWeightSums = FinalGraph.RegisterExternalBuffer(VoxelTargetWeightSumsBuf);
				FRDGBufferRef FCells = FinalGraph.RegisterExternalBuffer(VoxelCellsBuf);
				FRDGBufferRef FBlurNormals = FinalGraph.RegisterExternalBuffer(BlurredNormalsBuf);
				FRDGBufferRef FBlurTargets = FinalGraph.RegisterExternalBuffer(BlurredTargetsBuf);

				FRDGBufferUAVRef PositionsUAV = FinalGraph.CreateUAV(FRDGBufferUAVDesc(FPositions, PF_A32B32G32R32F));
				FRDGBufferUAVRef NormalsUAV = FinalGraph.CreateUAV(FRDGBufferUAVDesc(FNormals, PF_A32B32G32R32F));
				FRDGBufferUAVRef TargetPositionsUAV = FinalGraph.CreateUAV(FRDGBufferUAVDesc(FTargetPositions, PF_A32B32G32R32F));
				FRDGBufferSRVRef NormalSumsSRV = FinalGraph.CreateSRV(FRDGBufferSRVDesc(FNormalSums, PF_R32_SINT));
				FRDGBufferSRVRef NormalCountsSRV = FinalGraph.CreateSRV(FRDGBufferSRVDesc(FNormalCounts, PF_R32_UINT));
				FRDGBufferUAVRef TargetOffsetSumsUAV = FinalGraph.CreateUAV(FRDGBufferUAVDesc(FTargetOffsetSums, PF_R32_SINT));
				FRDGBufferUAVRef TargetWeightSumsUAV = FinalGraph.CreateUAV(FRDGBufferUAVDesc(FTargetWeightSums, PF_R32_UINT));

				TShaderMapRef<FFinalizeSurfaceVoxelNormalsCS> FinalizeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FFinalizeSurfaceVoxelNormalsCS::FParameters* FP = FinalGraph.AllocParameters<FFinalizeSurfaceVoxelNormalsCS::FParameters>();
				FP->SurfaceVoxelNormalSums = NormalSumsSRV;
				FP->SurfaceVoxelNormalCounts = NormalCountsSRV;
				FP->RW_SurfaceVoxelTargetOffsetSums = TargetOffsetSumsUAV;
				FP->RW_SurfaceVoxelTargetWeightSums = TargetWeightSumsUAV;
				FP->RW_OutVoxelPositions = PositionsUAV;
				FP->RW_OutVoxelNormals = NormalsUAV;
				FP->RW_OutVoxelTargetPositions = TargetPositionsUAV;
				FP->SurfaceVoxelCapacity = VoxCap;
				FP->SurfaceVoxelSize = SafeVoxelSize;
				FinalGraph.AddPass(
					RDG_EVENT_NAME("CS.FilteredSV.FinalizeNormals"), FP, ERDGPassFlags::Compute,
					[FP, FinalizeShader, VoxCap](FRHIComputeCommandList& Cmd)
					{
						FComputeShaderUtils::Dispatch(Cmd, FinalizeShader, *FP, FComputeShaderUtils::GetGroupCount(FIntVector(int32(VoxCap), 1, 1), 64));
					});

				const uint32 BlurIters = uint32(FMath::Max(0, SafeSurfaceVoxelBlurIterations));
				FRDGBufferUAVRef BlurNormalsUAV = FinalGraph.CreateUAV(FRDGBufferUAVDesc(FBlurNormals, PF_A32B32G32R32F));
				FRDGBufferUAVRef BlurTargetsUAV = FinalGraph.CreateUAV(FRDGBufferUAVDesc(FBlurTargets, PF_A32B32G32R32F));
				FRDGBufferUAVRef CounterUAV = FinalGraph.CreateUAV(FRDGBufferUAVDesc(FCounter, PF_R32_UINT));
				FRDGBufferUAVRef CellsUAV = FinalGraph.CreateUAV(FRDGBufferUAVDesc(FCells, PF_R32G32B32A32_UINT));
				FRDGBufferRef FHashSlots = FinalGraph.RegisterExternalBuffer(VoxelHashSlotsBuf);
				FRDGBufferRef FHashIndices = FinalGraph.RegisterExternalBuffer(VoxelHashIndicesBuf);
				FRDGBufferUAVRef HashSlotsUAV = FinalGraph.CreateUAV(FRDGBufferUAVDesc(FHashSlots, PF_R32_UINT));
				FRDGBufferUAVRef HashIndicesUAV = FinalGraph.CreateUAV(FRDGBufferUAVDesc(FHashIndices, PF_R32_UINT));

				if (BlurIters > 0u)
				{
					const uint32 BlurRadius = uint32(FMath::Max(1, SafeSurfaceVoxelBlurRadius));
					int32 CurrentNormalsIdx = 0;
					int32 CurrentTargetsIdx = 0;
					TShaderMapRef<FBlurSurfaceVoxelsCS> BlurShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					for (uint32 Iter = 0u; Iter < BlurIters; ++Iter)
					{
						const bool bReadFromOriginal = (CurrentNormalsIdx == 0);
						FRDGBufferUAVRef SrcN = bReadFromOriginal ? NormalsUAV : BlurNormalsUAV;
						FRDGBufferUAVRef SrcT = bReadFromOriginal ? TargetPositionsUAV : BlurTargetsUAV;
						FRDGBufferUAVRef DstN = bReadFromOriginal ? BlurNormalsUAV : NormalsUAV;
						FRDGBufferUAVRef DstT = bReadFromOriginal ? BlurTargetsUAV : TargetPositionsUAV;
						FBlurSurfaceVoxelsCS::FParameters* BP = FinalGraph.AllocParameters<FBlurSurfaceVoxelsCS::FParameters>();
						BP->RW_SurfaceVoxelCounter = CounterUAV;
					BP->RW_OutVoxelNormals = SrcN;
					BP->RW_BlurredVoxelNormals = DstN;
					BP->RW_OutVoxelTargetPositions = SrcT;
					BP->RW_BlurredVoxelTargetPositions = DstT;
					BP->RW_OutVoxelCells = CellsUAV;
					BP->RW_SurfaceVoxelHashSlots = HashSlotsUAV;
					BP->RW_SurfaceVoxelHashIndices = HashIndicesUAV;
					BP->SurfaceVoxelCapacity = VoxCap;
					BP->SurfaceVoxelHashSlotCount = HashSlotCount;
					BP->SurfaceVoxelBlurRadius = BlurRadius;
						FinalGraph.AddPass(
							RDG_EVENT_NAME("CS.FilteredSV.Blur%d", Iter), BP, ERDGPassFlags::Compute,
							[BP, BlurShader, VoxCap](FRHIComputeCommandList& Cmd)
							{
								FComputeShaderUtils::Dispatch(Cmd, BlurShader, *BP, FComputeShaderUtils::GetGroupCount(FIntVector(int32(VoxCap), 1, 1), 64));
							});
						CurrentNormalsIdx = 1 - CurrentNormalsIdx;
						CurrentTargetsIdx = 1 - CurrentTargetsIdx;
					}
					if (CurrentNormalsIdx != 0) { NormalsUAV = BlurNormalsUAV; }
					if (CurrentTargetsIdx != 0) { TargetPositionsUAV = BlurTargetsUAV; }
				}

				VoxelCapacity = int32(VoxCap);
				ActualVoxelReadbackBytes = uint32(uint64(VoxCap) * sizeof(FVector4f));
				ActualVoxelCellReadbackBytes = uint32(uint64(VoxCap) * sizeof(FIntVector4));
				AddEnqueueCopyPass(FinalGraph, PositionReadback, FPositions, ActualVoxelReadbackBytes);
				AddEnqueueCopyPass(FinalGraph, NormalReadback, FNormals, ActualVoxelReadbackBytes);
				AddEnqueueCopyPass(FinalGraph, TargetPositionReadback, FTargetPositions, ActualVoxelReadbackBytes);
				AddEnqueueCopyPass(FinalGraph, CellReadback, FCells, ActualVoxelCellReadbackBytes);
				AddEnqueueCopyPass(FinalGraph, CounterReadback, FCounter, CounterReadbackBytes);
				bHasGPUOutput = true;

				FinalGraph.Execute();
			}

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
	uint32 DroppedVoxelCount = 0;
	bool bReadbackSucceeded = false;

	ENQUEUE_RENDER_COMMAND(GetBoxSceneFilteredSurfaceVoxelsReadback)(
		[PositionReadback, NormalReadback, TargetPositionReadback, CellReadback, CounterReadback, ActualVoxelReadbackBytes, ActualVoxelCellReadbackBytes, CounterReadbackBytes,
		 &PositionData, &NormalData, &TargetPositionData, &CellData, &VoxelCount, &DroppedVoxelCount, &bReadbackSucceeded](FRHICommandListImmediate& RHICmdList)
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
				VoxelCount = CounterPtr[0];
				DroppedVoxelCount = CounterPtr[1];
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

	if (DroppedVoxelCount > 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[GetBoxSceneFilteredSurfaceVoxels] Voxel buffer overflow: %u cells dropped (capacity MaxVoxels=%d). Increase MaxVoxels or VoxelSize."),
			DroppedVoxelCount, SafeMaxVoxels);
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

FCSSurfaceVoxelData AComputeShaderMeshGenerator::ReadbackBoxSceneSurfaceVoxelsSync(float VoxelSize, const TCHAR* DebugName)
{
	(void)DebugName;
	TArray<FVector> UnusedPositions;
	TArray<FVector> UnusedNormals;
	GetBoxSceneFilteredSurfaceVoxels(VoxelSize, 0.0f, UnusedPositions, UnusedNormals);
	return LastSurfaceVoxelData;
}

// -----------------------------------------------------------------------------
// Core System - Generated Data Cache
// -----------------------------------------------------------------------------

FCSMeshGeneratorTriangleTextureDataHandle AComputeShaderMeshGenerator::UpdateBoxSceneTriangleTextureData(float ReferenceFilterDistance)
{
	GetBoxSceneTrianglesFromGPUFiltered(ReferenceFilterDistance);
	return LastTriangleTextureData;
}

FCSMeshGeneratorSurfaceVoxelTextureDataHandle AComputeShaderMeshGenerator::UpdateBoxSceneSurfaceVoxelTextureData(float VoxelSize)
{
	ReadbackBoxSceneSurfaceVoxelsSync(VoxelSize);
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
	const FCSDebugLastVoxelDirectionOptions& Options) const
{
	const float EffectiveDirectionLength = Options.DirectionLength > 0.0f
		? Options.DirectionLength
		: FMath::Max(LastSurfaceVoxelData.VoxelSize, UE_KINDA_SMALL_NUMBER);
	return DrawDebugDirectionArray(
		LastSurfaceVoxelData.Positions,
		LastSurfaceVoxelData.Normals,
		EffectiveDirectionLength,
		Options.DirectionColor,
		Options.Duration,
		Options.Thickness,
		Options.bPersistentLines,
		Options.bDrawPoints,
		Options.PointColor,
		Options.PointSize,
		Options.MaxDirectionsToDraw);
}

int32 AComputeShaderMeshGenerator::DrawDebugBoxSceneSurfaceVoxelDirections(
	const FCSDebugBoxVoxelDirectionOptions& Options)
{
	const FCSSurfaceVoxelData SurfaceVoxels = ReadbackBoxSceneSurfaceVoxelsSync(Options.VoxelSize);
	const float EffectiveDirectionLength = Options.DirectionLength > 0.0f
		? Options.DirectionLength
		: FMath::Max(SurfaceVoxels.VoxelSize, UE_KINDA_SMALL_NUMBER);
	return DrawDebugDirectionArray(
		SurfaceVoxels.Positions,
		SurfaceVoxels.Normals,
		EffectiveDirectionLength,
		Options.DirectionColor,
		Options.Duration,
		Options.Thickness,
		Options.bPersistentLines,
		Options.bDrawPoints,
		Options.PointColor,
		Options.PointSize,
		Options.MaxDirectionsToDraw);
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
	const FCSSurfaceVoxelData SurfaceVoxels = ReadbackBoxSceneSurfaceVoxelsSync(VoxelSize);

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
	const FCSSurfaceVoxelData SurfaceVoxels = ReadbackBoxSceneSurfaceVoxelsSync(VoxelSize);

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
	const FCSSurfaceVoxelData SurfaceVoxels = ReadbackBoxSceneSurfaceVoxelsSync(VoxelSize);
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

void AComputeShaderMeshGenerator::SpawnDebugSurfaceTrianglesDynamicMeshActor(float LifetimeSeconds)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const int32 EffectiveVertexCount = CachedSurfaceTriangles.VertexCount >= 0
		? FMath::Min(CachedSurfaceTriangles.VertexCount, CachedSurfaceTriangles.Vertices.Num())
		: CachedSurfaceTriangles.Vertices.Num();

	const int32 EffectiveIndexCount = CachedSurfaceTriangles.IndexCount >= 0
		? FMath::Min(CachedSurfaceTriangles.IndexCount, CachedSurfaceTriangles.Indices.Num())
		: CachedSurfaceTriangles.Indices.Num();

	if (EffectiveVertexCount < 3)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[SpawnDebugSurfaceTriangles] No cached surface triangles on %s. Run GenerateVines first."),
			*GetActorNameOrLabel());
		return;
	}

	// Convert cached triangles to a DynamicMesh
	UDynamicMesh* DebugMesh = BuildDynamicMeshFromCSTriangleData(
		CachedSurfaceTriangles.Vertices,
		CachedSurfaceTriangles.Indices,
		CachedSurfaceTriangles.VertexNormals,
		CachedSurfaceTriangles.VertexCount,
		CachedSurfaceTriangles.IndexCount,
		true,  // bReverseOrientation
		true,  // bSkipDegenerateTriangles
		true); // bRecomputeNormals

	if (!DebugMesh)
	{
		return;
	}

	// Spawn a temporary ADynamicMeshActor at this actor's location
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	ADynamicMeshActor* SpawnedActor = World->SpawnActor<ADynamicMeshActor>(
		ADynamicMeshActor::StaticClass(),
		FVector::ZeroVector,
		FRotator::ZeroRotator,
		SpawnParams);

	if (!SpawnedActor)
	{
		return;
	}

	// Set the debug mesh on the spawned actor
	UDynamicMeshComponent* MeshComponent = SpawnedActor->GetDynamicMeshComponent();
	if (MeshComponent)
	{
		MeshComponent->SetDynamicMesh(DebugMesh);
	}

	// Destroy the actor after LifetimeSeconds
	const float SafeLifetime = FMath::Max(0.1f, LifetimeSeconds);
	FTimerHandle DestroyTimerHandle;
	World->GetTimerManager().SetTimer(
		DestroyTimerHandle,
		[SpawnedActor]()
		{
			if (IsValid(SpawnedActor))
			{
				SpawnedActor->Destroy();
			}
		},
		SafeLifetime,
		false);
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
	SceneRoot->bEditableWhenInherited = false;
	SetRootComponent(SceneRoot);
	if (ExistingRoot && ExistingRoot != SceneRoot)
	{
		ExistingRoot->SetupAttachment(SceneRoot);
	}

	DynamicMeshComponent = CreateDefaultSubobject<UDynamicMeshComponent>(TEXT("DynamicMeshComponent"));
	DynamicMeshComponent->SetupAttachment(SceneRoot);
	DynamicMeshComponent->bUseAttachParentBound = false;
	DynamicMeshComponent->bNeverDistanceCull = true;
	DynamicMeshComponent->bAllowCullDistanceVolume = false;
	DynamicMeshComponent->SetCachedMaxDrawDistance(0.0f);
	DynamicMeshComponent->SetBoundsScale(DynamicMeshCullBoundsScale);

	GeneratorBounds = CreateDefaultSubobject<UBoxComponent>(TEXT("GeneratorBounds"));
#if WITH_EDITOR
	GeneratorBounds->SetIsVisualizationComponent(true);
#endif
	GeneratorBounds->bEditableWhenInherited = false;
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
	ClearGeneratedDataTextureCache();
	Super::EndPlay(EndPlayReason);
}

void AMeshGeneratorBrushCache::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearMeshGeneratorCache();
	Super::EndPlay(EndPlayReason);
}

// -----------------------------------------------------------------------------
// Brush System
// -----------------------------------------------------------------------------

void AMeshGeneratorBrushCache::StartInstanceBrush()
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

UHierarchicalInstancedStaticMeshComponent* AMeshGeneratorBrushCache::FindPaintComponent(UStaticMesh* Mesh) const
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

UHierarchicalInstancedStaticMeshComponent* AMeshGeneratorBrushCache::GetOrCreatePaintComponent(UStaticMesh* Mesh)
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

int32 AMeshGeneratorBrushCache::CommitPaintInstances(const TArray<FTransform>& WorldTransforms, UStaticMesh* Mesh)
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

bool AMeshGeneratorBrushCache::IsInstanceBrushPointAllowed(const FVector& WorldPosition) const
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

FCSMeshGeneratorTriangleCacheHandle AMeshGeneratorBrushCache::EnsureTriangleCache(const FCSMeshGeneratorTriangleCacheRequest& Request)
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

FCSMeshGeneratorTriangleCacheHandle AMeshGeneratorBrushCache::EnsureTriangleCacheByBox(
	FName RequestId,
	bool bForceFullRebuild)
{
	FCSMeshGeneratorTriangleCacheRequest Request;
	Request.RequestId = RequestId;
	Request.bForceFullRebuild = bForceFullRebuild;
	return EnsureTriangleCache(Request);
}

FCSMeshGeneratorTriangleCacheHandle AMeshGeneratorBrushCache::EnsureTriangleCacheByBox(
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

void AMeshGeneratorBrushCache::UpdateMeshGeneratorCacheByBox(
	bool bForceFullRebuild)
{
	EnsureTriangleCacheByBox(CSGeneratorDefaultRequestId, bForceFullRebuild);
}

void AMeshGeneratorBrushCache::ReleaseTriangleCacheRequest(FName RequestId)
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

void AMeshGeneratorBrushCache::ClearMeshGeneratorCache()
{
	ResetCacheRuntime(true);
	++CacheState.CacheGeneration;
}

void AMeshGeneratorBrushCache::MarkAllActiveVoxelsDirty()
{
	CacheState.DirtyCells.Append(CacheState.ActiveCells);
}

FCSMeshGeneratorTriangleCacheHandle AMeshGeneratorBrushCache::GetTriangleCacheHandle() const
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

// -----------------------------------------------------------------------------
// Debug System
// -----------------------------------------------------------------------------

int32 AMeshGeneratorBrushCache::DrawDebugActiveVoxels(
	const FCSDebugActiveVoxelOptions& Options) const
{
	UWorld* World = GetWorld();
	if (!World || !CacheState.CachedWorldBounds.IsValid || CacheState.CachedVoxelSize <= CSGeneratorMinVoxelSize)
	{
		return 0;
	}

	const TSet<FCSMeshGeneratorVoxelKey>* CellsToDraw = &CacheState.ActiveCells;
	if (!Options.RequestId.IsNone())
	{
		CellsToDraw = RequestActiveCells.Find(NormalizeRequestId(Options.RequestId));
		if (!CellsToDraw)
		{
			return 0;
		}
	}

	if (CellsToDraw->IsEmpty())
	{
		return 0;
	}

	const float SafeDuration = FMath::Max(0.0f, Options.Duration);
	const float SafeThickness = FMath::Max(0.0f, Options.Thickness);
	const int32 DrawLimit = Options.MaxVoxelsToDraw > 0 ? Options.MaxVoxelsToDraw : TNumericLimits<int32>::Max();
	const FColor LineColor = Options.DebugColor.ToFColor(true);

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
			Options.bPersistentLines,
			SafeDuration,
			0,
			SafeThickness);
		++DrawnCount;
	}

	if (Options.bDrawCacheBounds)
	{
		DrawDebugBox(
			World,
			CacheState.CachedWorldBounds.GetCenter(),
			CacheState.CachedWorldBounds.GetExtent(),
			FColor::White,
			Options.bPersistentLines,
			SafeDuration,
			0,
			FMath::Max(1.0f, SafeThickness));
	}

	return DrawnCount;
}

// -----------------------------------------------------------------------------
// Dirty Cache System - Internals
// -----------------------------------------------------------------------------

bool AMeshGeneratorBrushCache::DoesInputRequireFullRebuild(const FBox& InputWorldBounds) const
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

void AMeshGeneratorBrushCache::RebuildCacheResources(const FBox& InputWorldBounds)
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

void AMeshGeneratorBrushCache::BuildActiveCellsFromReferencePoints(
	float ActivationRadius,
	TSet<FCSMeshGeneratorVoxelKey>& OutCells) const
{
	BuildActiveCellsFromReferencePoints(ReferencePoints, ActivationRadius, OutCells);
}

void AMeshGeneratorBrushCache::BuildActiveCellsFromReferencePoints(
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

		const FIntVector CenterCell = WorldPositionToCell(Point);
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

void AMeshGeneratorBrushCache::BuildUnionActiveCells(TSet<FCSMeshGeneratorVoxelKey>& OutCells) const
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

void AMeshGeneratorBrushCache::DiffActiveCells(const TSet<FCSMeshGeneratorVoxelKey>& NewActiveCells)
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

void AMeshGeneratorBrushCache::AllocatePagesForCells(const TSet<FCSMeshGeneratorVoxelKey>& Cells)
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

void AMeshGeneratorBrushCache::ReleasePagesForCells(const TSet<FCSMeshGeneratorVoxelKey>& Cells)
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

void AMeshGeneratorBrushCache::DispatchDirtyVoxelTriangleCacheUpdate()
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
		ExcludedActorTags,
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

FIntVector AMeshGeneratorBrushCache::ComputeGridSize(const FBox& InputWorldBounds) const
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

FCSMeshGeneratorVoxelKey AMeshGeneratorBrushCache::WorldPositionToCell(FVector WorldPosition) const
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

FBox AMeshGeneratorBrushCache::GetCellWorldBounds(const FCSMeshGeneratorVoxelKey& Cell) const
{
	const float SafeVoxelSize = FMath::Max(CacheState.CachedVoxelSize, CSGeneratorMinVoxelSize);
	const FVector Min = CacheState.CachedWorldBounds.Min + FVector(Cell.X, Cell.Y, Cell.Z) * SafeVoxelSize;
	return FBox(Min, Min + FVector(SafeVoxelSize));
}

void AMeshGeneratorBrushCache::ReleaseCacheResources()
{
	ReleaseRTAndNull(VoxelMetaRT);
	ReleaseRTAndNull(TriangleVertexRT);
	ReleaseRTAndNull(TriangleNormalRT);
}

void AMeshGeneratorBrushCache::ResetCacheRuntime(bool bClearRequests)
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

void AMeshGeneratorBrushCache::InitializeFreePages()
{
	CacheState.FreePages.Reset();
	CacheState.FreePages.Reserve(CacheState.CachedMaxActiveVoxels);
	for (int32 PageIndex = CacheState.CachedMaxActiveVoxels - 1; PageIndex >= 0; --PageIndex)
	{
		CacheState.FreePages.Add(PageIndex);
	}
}

void AMeshGeneratorBrushCache::CreateCacheRenderTargets()
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

	InitCacheRT(VoxelMetaRT, MetaWidth, MetaHeight);
	InitCacheRT(TriangleVertexRT, VertexWidth, VertexHeight);
	InitCacheRT(TriangleNormalRT, NormalWidth, NormalHeight);
}

void AMeshGeneratorBrushCache::RebuildRequestActiveCellsFromLastRequests()
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

bool AMeshGeneratorBrushCache::HasValidCacheResources() const
{
	return IsValidCacheRT(VoxelMetaRT) && IsValidCacheRT(TriangleVertexRT) && IsValidCacheRT(TriangleNormalRT);
}

bool AMeshGeneratorBrushCache::AreBoundsCompatible(const FBox& A, const FBox& B) const
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

FName AMeshGeneratorBrushCache::NormalizeRequestId(FName RequestId) const
{
	return RequestId.IsNone() ? CSGeneratorDefaultRequestId : RequestId;
}

// -----------------------------------------------------------------------------
// Triangle Soup → Heightmap RDG pass
// -----------------------------------------------------------------------------

void AComputeShaderMeshGenerator::RasterizeTriangleSoupToHeightmapRDG(
	FRDGBuilder& GraphBuilder,
	const FCSStaticMeshTriangleRDGOutput& TriangleOutput,
	FRDGTextureRef OutputHeightmap,
	const FBox& WorldBounds,
	float CameraHeight)
{
	if (!TriangleOutput.TriangleVertices || !TriangleOutput.TriangleCounter || TriangleOutput.MaxTriangles == 0)
	{
		return;
	}

	FIntPoint TexSize;
	{
		FRDGTextureDesc Desc = OutputHeightmap->Desc;
		TexSize = FIntPoint(Desc.Extent.X, Desc.Extent.Y);
	}

	FVector2f BoundsMin(WorldBounds.Min.X, WorldBounds.Min.Y);
	FVector2f BoundsSize(WorldBounds.Max.X - WorldBounds.Min.X, WorldBounds.Max.Y - WorldBounds.Min.Y);
	FVector2f BoundsInvSize(
		BoundsSize.X > 0.01f ? 1.0f / BoundsSize.X : 0.0f,
		BoundsSize.Y > 0.01f ? 1.0f / BoundsSize.Y : 0.0f);

	FRDGTextureDesc UintDesc = FRDGTextureDesc::Create2D(
		FIntPoint(TexSize.X, TexSize.Y),
		PF_R32_UINT,
		FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_UAV);
	FRDGTextureRef HeightmapUint = GraphBuilder.CreateTexture(UintDesc, TEXT("CS.HeightmapUint"));
	FRDGTextureUAVRef HeightmapUintUAV = GraphBuilder.CreateUAV(HeightmapUint);

	AddClearUAVPass(GraphBuilder, HeightmapUintUAV, 0xFFFFFFFFu);

	{
		TShaderMapRef<FTriangleSoupToHeightmapCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		auto* PassParams = GraphBuilder.AllocParameters<FTriangleSoupToHeightmapCS::FParameters>();
		PassParams->RW_OutTriangleVertices = TriangleOutput.TriangleVerticesUAV;
		PassParams->RW_TriangleCounter = TriangleOutput.TriangleCounterUAV;
		PassParams->RW_HeightmapUint = HeightmapUintUAV;
		PassParams->HM_BoundsMin = BoundsMin;
		PassParams->HM_BoundsInvSize = BoundsInvSize;
		PassParams->HM_CameraHeight = CameraHeight;
		PassParams->HM_TextureSize = TexSize;

		FIntVector GroupCount(FMath::DivideAndRoundUp(int32(TriangleOutput.MaxTriangles), 64), 1, 1);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("TriangleSoupToHeightmap"),
			ERDGPassFlags::Compute,
			CS,
			PassParams,
			GroupCount);
	}

	{
		TShaderMapRef<FConvertHeightmapUintToFloatCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		auto* ConvertParams = GraphBuilder.AllocParameters<FConvertHeightmapUintToFloatCS::FParameters>();
		ConvertParams->T_HeightmapUint = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(HeightmapUint));
		ConvertParams->RW_HeightmapFloat = GraphBuilder.CreateUAV(OutputHeightmap);
		ConvertParams->HM_TextureSize = TexSize;

		FIntVector GroupCount(
			FMath::DivideAndRoundUp(TexSize.X, 8),
			FMath::DivideAndRoundUp(TexSize.Y, 8),
			1);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("ConvertHeightmapUintToFloat"),
			ERDGPassFlags::Compute,
			CS,
			ConvertParams,
			GroupCount);
	}
}

void AComputeShaderMeshGenerator::ConvertLandscapeHeightmapToDepthRDG(
	FRDGBuilder& GraphBuilder,
	FRDGTextureRef LandscapeG16Texture,
	FRDGTextureRef OutputHeightmap,
	float CameraHeight,
	float LandscapeScaleZ,
	float LandscapeOriginZ)
{
	if (!LandscapeG16Texture || !OutputHeightmap) return;

	FIntPoint TexSize(OutputHeightmap->Desc.Extent.X, OutputHeightmap->Desc.Extent.Y);

	TShaderMapRef<FLandscapeG16ToDepthCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	auto* PassParams = GraphBuilder.AllocParameters<FLandscapeG16ToDepthCS::FParameters>();
	PassParams->T_LandscapeRGBA = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(LandscapeG16Texture));
	PassParams->RW_HeightmapFloat = GraphBuilder.CreateUAV(OutputHeightmap);
	PassParams->LHM_CameraHeight = CameraHeight;
	PassParams->LHM_LandscapeScaleZ = LandscapeScaleZ;
	PassParams->LHM_LandscapeOriginZ = LandscapeOriginZ;
	PassParams->LHM_TextureSize = TexSize;

	FIntVector GroupCount(
		FMath::DivideAndRoundUp(TexSize.X, 8),
		FMath::DivideAndRoundUp(TexSize.Y, 8),
		1);
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("LandscapeG16ToDepth"),
		ERDGPassFlags::Compute,
		CS,
		PassParams,
		GroupCount);
}

bool AComputeShaderMeshGenerator::CaptureLandscapeHeightmap(UTextureRenderTarget2D* OutRT, bool bOutputWorldHeight)
{
	const FBox Box = GetGeneratorBoundsWorldBox();
	if (!Box.IsValid) return false;
	const FVector Center = Box.GetCenter();
	const FVector Extent = Box.GetExtent();
	const float CaptureExtent = FMath::Max(Extent.X, Extent.Y);
	const float CameraHeight = Center.Z + Extent.Z;

	if (OutRT == nullptr)
	{
		UWorld* World = GetWorld();
		if (!World) return false;

		constexpr int32 GridSize = 32;
		const float WorldSize = CaptureExtent * 2.0f;
		const float StepSize = WorldSize / GridSize;

		const float TraceTop    = CameraHeight + 100000.0f;
		const float TraceBottom = Center.Z - Extent.Z - 100000.0f;

		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(LandscapeHeightDD), false);
		QueryParams.AddIgnoredActor(this);

		int32 HitCount = 0;
		const double TraceStart = FPlatformTime::Seconds();
		for (int32 Y = 0; Y < GridSize; ++Y)
		{
			for (int32 X = 0; X < GridSize; ++X)
			{
				const float WorldX = Center.X - CaptureExtent + (X + 0.5f) * StepSize;
				const float WorldY = Center.Y - CaptureExtent + (Y + 0.5f) * StepSize;

				FHitResult Hit;
				if (World->LineTraceSingleByChannel(Hit,
					FVector(WorldX, WorldY, TraceTop),
					FVector(WorldX, WorldY, TraceBottom),
					ECC_WorldStatic, QueryParams)
					&& Hit.GetActor() && Hit.GetActor()->IsA<ALandscapeProxy>())
				{
					DrawDebugPoint(World, Hit.ImpactPoint + FVector(0,0,10), 8.0f, FColor::Green, false, 15.0f);
					++HitCount;
				}
			}
		}
		const double TraceMs = (FPlatformTime::Seconds() - TraceStart) * 1000.0;
		UE_LOG(LogTemp, Log, TEXT("[CaptureLandscapeHeightmap] DD mode: %d/%d landscape hits, %.2f ms, Center=(%.0f,%.0f,%.0f) Extent=%.0f"),
			HitCount, GridSize * GridSize, TraceMs, Center.X, Center.Y, Center.Z, CaptureExtent);
		return HitCount > 0;
	}

	const bool bResult = bOutputWorldHeight
		? CaptureLandscapeHeightmapGPU(Center, CaptureExtent, OutRT)
		: CaptureLandscapeHeightmapToDepth(Center, CaptureExtent, CameraHeight, OutRT);

	return bResult;
}

bool AComputeShaderMeshGenerator::CaptureLandscapeHeightmapToDepth(
	FVector WorldCenter,
	float CaptureExtent,
	float CameraHeight,
	UTextureRenderTarget2D* OutDepthRT)
{
	if (!OutDepthRT) return false;

	UWorld* World = GetWorld();
	if (!World) return false;

	TArray<ALandscape*> Landscapes;
	for (TActorIterator<ALandscape> It(World); It; ++It)
	{
		if (IsValid(*It))
		{
			Landscapes.Add(*It);
		}
	}
	if (Landscapes.IsEmpty()) return false;

	const int32 TexSize = OutDepthRT->SizeX;

	// Pre-clear output to very large depth so min-merge works across multiple landscapes.
	// The shader writes min(existing, newDepth), so existing must start high.
	FTextureRenderTargetResource* R_Depth = OutDepthRT->GameThread_GetRenderTargetResource();
	ENQUEUE_RENDER_COMMAND(ClearDepthToMax)(
	[R_Depth](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		FRDGTextureRef RDG = RegisterExternalTexture(
			GraphBuilder, R_Depth->GetRenderTargetTexture(), TEXT("DepthClear"));
		AddClearRenderTargetPass(GraphBuilder, RDG, FLinearColor(1e10f, 0, 0, 1));
		GraphBuilder.Execute();
	});

	FTransform AreaTransform(FQuat::Identity, WorldCenter, FVector::OneVector);
	FBox2D Extents(FVector2D(-CaptureExtent, -CaptureExtent), FVector2D(CaptureExtent, CaptureExtent));

	TArray<UTextureRenderTarget2D*> TempRTs;
	bool bAnySuccess = false;

	for (ALandscape* Landscape : Landscapes)
	{
		UTextureRenderTarget2D* TempRT = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
		TempRT->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA8;
		TempRT->bCanCreateUAV = false;
		TempRT->ClearColor = FLinearColor(0.5f, 0, 0, 0);
		TempRT->InitAutoFormat(TexSize, TexSize);
		TempRT->UpdateResourceImmediate(true);

		if (!Landscape->RenderHeightmap(AreaTransform, Extents, TempRT))
		{
			TempRT->MarkAsGarbage();
			continue;
		}

		const float LandscapeScaleZ = Landscape->GetActorScale3D().Z;
		const float LandscapeOriginZ = Landscape->GetActorLocation().Z;

		FTextureRenderTargetResource* R_RGBA = TempRT->GameThread_GetRenderTargetResource();

		ENQUEUE_RENDER_COMMAND(LandscapeRGBAToDepth)(
		[this, R_RGBA, R_Depth, CameraHeight, LandscapeScaleZ, LandscapeOriginZ, TexSize](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			FRDGTextureRef RDG_RGBA = RegisterExternalTexture(GraphBuilder, R_RGBA->GetRenderTargetTexture(), TEXT("LandscapeRGBA_RT"));
			FRDGTextureRef RDG_Depth = RegisterExternalTexture(GraphBuilder, R_Depth->GetRenderTargetTexture(), TEXT("DepthOutput_RT"));

			ConvertLandscapeHeightmapToDepthRDG(
				GraphBuilder, RDG_RGBA, RDG_Depth,
				CameraHeight, LandscapeScaleZ, LandscapeOriginZ);

			GraphBuilder.Execute();
		});

		TempRTs.Add(TempRT);
		bAnySuccess = true;
	}

	FlushRenderingCommands();

	for (UTextureRenderTarget2D* TempRT : TempRTs)
	{
		TempRT->MarkAsGarbage();
	}

	return bAnySuccess;
}

void AComputeShaderMeshGenerator::ConvertLandscapeHeightmapToNormalHeightRDG(
	FRDGBuilder& GraphBuilder,
	FRDGTextureRef LandscapeG16Texture,
	FRDGTextureRef OutputNormalHeight,
	float LandscapeScaleZ,
	float LandscapeOriginZ,
	FVector2f TexelWorldSize,
	bool bMergeByMaxZ)
{
	if (!LandscapeG16Texture || !OutputNormalHeight) return;

	FIntPoint TexSize(OutputNormalHeight->Desc.Extent.X, OutputNormalHeight->Desc.Extent.Y);

	TShaderMapRef<FLandscapeG16ToNormalHeightCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	auto* PassParams = GraphBuilder.AllocParameters<FLandscapeG16ToNormalHeightCS::FParameters>();
	PassParams->T_LandscapeRGBA = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(LandscapeG16Texture));
	PassParams->RW_HeightmapFloat = GraphBuilder.CreateUAV(OutputNormalHeight);
	PassParams->LHM_LandscapeScaleZ = LandscapeScaleZ;
	PassParams->LHM_LandscapeOriginZ = LandscapeOriginZ;
	PassParams->LHM_TextureSize = TexSize;
	PassParams->LHM_TexelWorldSize = TexelWorldSize;
	PassParams->LHM_MergeByMaxZ = bMergeByMaxZ ? 1u : 0u;

	FIntVector GroupCount(
		FMath::DivideAndRoundUp(TexSize.X, 8),
		FMath::DivideAndRoundUp(TexSize.Y, 8),
		1);
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("LandscapeG16ToNormalHeight"),
		ERDGPassFlags::Compute,
		CS,
		PassParams,
		GroupCount);
}

bool AComputeShaderMeshGenerator::CaptureLandscapeHeightmapGPU(
	FVector WorldCenter,
	float CaptureExtent,
	UTextureRenderTarget2D* OutNormalHeightRT)
{
	if (!OutNormalHeightRT) return false;

	UWorld* World = GetWorld();
	if (!World) return false;

	TArray<ALandscape*> Landscapes;
	for (TActorIterator<ALandscape> It(World); It; ++It)
	{
		if (IsValid(*It))
		{
			Landscapes.Add(*It);
		}
	}
	if (Landscapes.IsEmpty()) return false;

	const int32 TexSize = OutNormalHeightRT->SizeX;
	const bool bMultipleLandscapes = Landscapes.Num() > 1;

	// Pre-clear output: Normal=(0,0,1) up, Height=-1e10 (very low → any real terrain wins merge)
	FTextureRenderTargetResource* R_Out = OutNormalHeightRT->GameThread_GetRenderTargetResource();
	if (bMultipleLandscapes)
	{
		ENQUEUE_RENDER_COMMAND(ClearNormalHeightToMin)(
		[R_Out](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			FRDGTextureRef RDG = RegisterExternalTexture(
				GraphBuilder, R_Out->GetRenderTargetTexture(), TEXT("NHClear"));
			AddClearRenderTargetPass(GraphBuilder, RDG, FLinearColor(0, 0, 1, -1e10f));
			GraphBuilder.Execute();
		});
	}

	FTransform AreaTransform(FQuat::Identity, WorldCenter, FVector::OneVector);
	FBox2D Extents(FVector2D(-CaptureExtent, -CaptureExtent), FVector2D(CaptureExtent, CaptureExtent));
	const FVector2f CapturedTexelWorldSize(
		(CaptureExtent * 2.0f) / TexSize,
		(CaptureExtent * 2.0f) / TexSize);

	TArray<UTextureRenderTarget2D*> TempRTs;
	bool bAnySuccess = false;

	for (ALandscape* Landscape : Landscapes)
	{
		UTextureRenderTarget2D* TempRT = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
		TempRT->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA8;
		TempRT->bCanCreateUAV = false;
		TempRT->ClearColor = FLinearColor(0.5f, 0, 0, 0);
		TempRT->InitAutoFormat(TexSize, TexSize);
		TempRT->UpdateResourceImmediate(true);

		if (!Landscape->RenderHeightmap(AreaTransform, Extents, TempRT))
		{
			TempRT->MarkAsGarbage();
			continue;
		}

		const float LandscapeScaleZ = Landscape->GetActorScale3D().Z;
		const float LandscapeOriginZ = Landscape->GetActorLocation().Z;
		const bool bMerge = bMultipleLandscapes;

		FTextureRenderTargetResource* R_RGBA = TempRT->GameThread_GetRenderTargetResource();

		ENQUEUE_RENDER_COMMAND(LandscapeRGBAToNormalHeight)(
		[this, R_RGBA, R_Out, LandscapeScaleZ, LandscapeOriginZ, CapturedTexelWorldSize, TexSize, bMerge](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			FRDGTextureRef RDG_RGBA = RegisterExternalTexture(GraphBuilder, R_RGBA->GetRenderTargetTexture(), TEXT("LandscapeRGBA_RT"));
			FRDGTextureRef RDG_Out = RegisterExternalTexture(GraphBuilder, R_Out->GetRenderTargetTexture(), TEXT("NormalHeightOutput_RT"));

			ConvertLandscapeHeightmapToNormalHeightRDG(
				GraphBuilder, RDG_RGBA, RDG_Out,
				LandscapeScaleZ, LandscapeOriginZ, CapturedTexelWorldSize, bMerge);

			GraphBuilder.Execute();
		});

		TempRTs.Add(TempRT);
		bAnySuccess = true;
	}

	FlushRenderingCommands();

	for (UTextureRenderTarget2D* TempRT : TempRTs)
	{
		TempRT->MarkAsGarbage();
	}

	return bAnySuccess;
}

bool AComputeShaderMeshGenerator::RenderLandscapeToNormalHeightRT(
	ALandscape* Landscape,
	FVector WorldCenter,
	FVector WorldExtentXY,
	UTextureRenderTarget2D* OutNormalHeightRT)
{
	if (!Landscape || !OutNormalHeightRT) return false;

	const int32 TexSizeX = OutNormalHeightRT->SizeX;
	const int32 TexSizeY = OutNormalHeightRT->SizeY;
	if (TexSizeX < 4 || TexSizeY < 4) return false;

	const float ExtX = FMath::Abs(WorldExtentXY.X);
	const float ExtY = FMath::Abs(WorldExtentXY.Y);
	if (ExtX < 1.0f || ExtY < 1.0f) return false;

	UTextureRenderTarget2D* TempRT = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
	TempRT->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA8;
	TempRT->bCanCreateUAV = false;
	TempRT->ClearColor = FLinearColor(0.5f, 0, 0, 0);
	TempRT->InitAutoFormat(TexSizeX, TexSizeY);
	TempRT->UpdateResourceImmediate(true);

	FTransform AreaTransform(FQuat::Identity, WorldCenter, FVector::OneVector);
	FBox2D Extents(FVector2D(-ExtX, -ExtY), FVector2D(ExtX, ExtY));

	if (!Landscape->RenderHeightmap(AreaTransform, Extents, TempRT))
	{
		TempRT->MarkAsGarbage();
		return false;
	}

	const float LandscapeScaleZ = Landscape->GetActorScale3D().Z;
	const float LandscapeOriginZ = Landscape->GetActorLocation().Z;
	const FVector2f TexelWorldSize(
		(ExtX * 2.0f) / TexSizeX,
		(ExtY * 2.0f) / TexSizeY);

	FTextureRenderTargetResource* R_RGBA = TempRT->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Out = OutNormalHeightRT->GameThread_GetRenderTargetResource();

	ENQUEUE_RENDER_COMMAND(LandscapeRGBAToNormalHeight_Static)(
	[R_RGBA, R_Out, LandscapeScaleZ, LandscapeOriginZ, TexelWorldSize, TexSizeX, TexSizeY](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);

		FRDGTextureRef RDG_RGBA = RegisterExternalTexture(GraphBuilder, R_RGBA->GetRenderTargetTexture(), TEXT("LandscapeRGBA_RT"));
		FRDGTextureRef RDG_Out = RegisterExternalTexture(GraphBuilder, R_Out->GetRenderTargetTexture(), TEXT("NormalHeight_RT"));

		FIntPoint TexSize(TexSizeX, TexSizeY);
		TShaderMapRef<FLandscapeG16ToNormalHeightCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		auto* PassParams = GraphBuilder.AllocParameters<FLandscapeG16ToNormalHeightCS::FParameters>();
		PassParams->T_LandscapeRGBA = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(RDG_RGBA));
		PassParams->RW_HeightmapFloat = GraphBuilder.CreateUAV(RDG_Out);
		PassParams->LHM_LandscapeScaleZ = LandscapeScaleZ;
		PassParams->LHM_LandscapeOriginZ = LandscapeOriginZ;
		PassParams->LHM_TextureSize = TexSize;
		PassParams->LHM_TexelWorldSize = TexelWorldSize;
		PassParams->LHM_MergeByMaxZ = 0u;

		FIntVector GroupCount(
			FMath::DivideAndRoundUp(TexSize.X, 8),
			FMath::DivideAndRoundUp(TexSize.Y, 8),
			1);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("LandscapeG16ToNormalHeight"),
			ERDGPassFlags::Compute,
			CS,
			PassParams,
			GroupCount);

		GraphBuilder.Execute();
	});

	FlushRenderingCommands();

	TempRT->MarkAsGarbage();
	return true;
}

FCSTriangleMeshData AComputeShaderMeshGenerator::CaptureLandscapeTrianglesGPU(int32 TextureSize)
{
	FCSTriangleMeshData Result;

	const FBox Box = GetGeneratorBoundsWorldBox();
	if (!Box.IsValid) return Result;

	UWorld* World = GetWorld();
	if (!World) return Result;

	const FVector Center = Box.GetCenter();
	const FVector Extent = Box.GetExtent();
	const float CaptureExtent = FMath::Max(Extent.X, Extent.Y);
	TextureSize = FMath::Clamp(TextureSize, 4, 2048);

	TArray<ALandscape*> Landscapes;
	for (TActorIterator<ALandscape> It(World); It; ++It)
	{
		if (IsValid(*It))
			Landscapes.Add(*It);
	}
	if (Landscapes.IsEmpty()) return Result;

	ALandscape* Landscape = Landscapes[0];

	UTextureRenderTarget2D* TempRT = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
	TempRT->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA8;
	TempRT->bCanCreateUAV = false;
	TempRT->ClearColor = FLinearColor(0.5f, 0, 0, 0);
	TempRT->InitAutoFormat(TextureSize, TextureSize);
	TempRT->UpdateResourceImmediate(true);

	FTransform AreaTransform(FQuat::Identity, Center, FVector::OneVector);
	FBox2D AreaExtents(FVector2D(-CaptureExtent, -CaptureExtent), FVector2D(CaptureExtent, CaptureExtent));

	if (!Landscape->RenderHeightmap(AreaTransform, AreaExtents, TempRT))
	{
		TempRT->MarkAsGarbage();
		return Result;
	}

	const float LandscapeScaleZ = Landscape->GetActorScale3D().Z;
	const float LandscapeOriginZ = Landscape->GetActorLocation().Z;
	const FVector2f TexelWorldSize(
		(CaptureExtent * 2.0f) / TextureSize,
		(CaptureExtent * 2.0f) / TextureSize);
	const FVector2f WorldOriginXY(
		float(Center.X - CaptureExtent),
		float(Center.Y - CaptureExtent));

	const int32 GridCells = TextureSize - 1;
	const int32 TotalVerts = GridCells * GridCells * 6;
	const uint32 ReadbackBytes = uint32(int64(TotalVerts) * sizeof(FVector4f));

	FTextureRenderTargetResource* R_RGBA = TempRT->GameThread_GetRenderTargetResource();

	FRHIGPUBufferReadback* VertReadback = new FRHIGPUBufferReadback(TEXT("LandscapeTriangles_VertReadback"));
	bool bRenderWorkQueued = false;

	ENQUEUE_RENDER_COMMAND(LandscapeToTriangles)(
	[R_RGBA, LandscapeScaleZ, LandscapeOriginZ, TexelWorldSize, WorldOriginXY,
	 TextureSize, GridCells, TotalVerts, ReadbackBytes, VertReadback,
	 &bRenderWorkQueued](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);

		FRDGTextureRef RDG_RGBA = RegisterExternalTexture(
			GraphBuilder, R_RGBA->GetRenderTargetTexture(), TEXT("LandscapeRGBA_Tri"));

		FRDGBufferRef TriBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), TotalVerts),
			TEXT("LandscapeTriVerts"));

		TShaderMapRef<FLandscapeHeightmapToTrianglesCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		auto* Params = GraphBuilder.AllocParameters<FLandscapeHeightmapToTrianglesCS::FParameters>();
		Params->T_LandscapeRGBA = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(RDG_RGBA));
		Params->RW_TriangleVerts = GraphBuilder.CreateUAV(TriBuffer);
		Params->LHM_LandscapeScaleZ = LandscapeScaleZ;
		Params->LHM_LandscapeOriginZ = LandscapeOriginZ;
		Params->LHM_TextureSize = FIntPoint(TextureSize, TextureSize);
		Params->LHM_WorldOriginXY = WorldOriginXY;
		Params->LHM_TexelWorldSize = TexelWorldSize;

		FIntVector GroupCount(
			FMath::DivideAndRoundUp(GridCells, 8),
			FMath::DivideAndRoundUp(GridCells, 8),
			1);
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("LandscapeHeightmapToTriangles"),
			ERDGPassFlags::Compute, CS, Params, GroupCount);

		AddEnqueueCopyPass(GraphBuilder, VertReadback, TriBuffer, ReadbackBytes);

		GraphBuilder.Execute();
		bRenderWorkQueued = true;
	});

	FlushRenderingCommands();

	if (bRenderWorkQueued)
	{
		ENQUEUE_RENDER_COMMAND(LandscapeTrianglesReadback)(
		[VertReadback, ReadbackBytes, TotalVerts, &Result](FRHICommandListImmediate& RHICmdList)
		{
			if (!VertReadback->IsReady())
				RHICmdList.SubmitAndBlockUntilGPUIdle();

			if (VertReadback->IsReady() && VertReadback->GetGPUSizeBytes() >= ReadbackBytes)
			{
				if (const FVector4f* SrcData = static_cast<const FVector4f*>(VertReadback->Lock(ReadbackBytes)))
				{
					Result.Vertices.SetNumUninitialized(TotalVerts);
					for (int32 i = 0; i < TotalVerts; ++i)
						Result.Vertices[i] = FVector(SrcData[i].X, SrcData[i].Y, SrcData[i].Z);
					Result.VertexCount = TotalVerts;
					VertReadback->Unlock();
				}
			}
			delete VertReadback;
		});
		FlushRenderingCommands();
	}
	else
	{
		delete VertReadback;
	}

	TempRT->MarkAsGarbage();

	UE_LOG(LogTemp, Log, TEXT("[CaptureLandscapeTrianglesGPU] %d verts (%d tris), TexSize=%d, Center=(%.0f,%.0f) Extent=%.0f"),
		Result.VertexCount, Result.VertexCount / 3, TextureSize, Center.X, Center.Y, CaptureExtent);
	return Result;
}
