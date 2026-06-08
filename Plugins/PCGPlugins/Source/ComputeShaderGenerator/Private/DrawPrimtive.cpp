#include "DrawPrimtive.h"

#include "ComputeShaderBasicFunction.h"
#include "GlobalShader.h"
#include "MaterialShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderGraphBuilder.h"
#include "RenderTargetPool.h"
#include "RHIResourceUtils.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#if WITH_EDITOR
#include "LandscapeDataAccess.h"
#endif
#include "RawIndexBuffer.h"
#include "RenderingThread.h"
#include "StaticMeshResources.h"
#include "TextureResource.h"
#include "UObject/UObjectIterator.h"

struct FCSVertexData
{
public:
	FVector4f Position;
	FVector2f UV;
	// FVector4f Normal;

};

struct FDrawRasterVertexData
{
	FVector4f Position;
	FVector4f Normal;
};

namespace
{
constexpr float CSBoxWorldZMinExtent = 1.0e-3f;

int32 GetCSTriangleMeshTriangleCount(const FCSTriangleMeshData& TriangleData)
{
	const int32 EffectiveVertexCount = TriangleData.VertexCount >= 0
		? FMath::Clamp(TriangleData.VertexCount, 0, TriangleData.Vertices.Num())
		: TriangleData.Vertices.Num();
	const int32 EffectiveIndexCount = TriangleData.IndexCount >= 0
		? FMath::Clamp(TriangleData.IndexCount, 0, TriangleData.Indices.Num())
		: TriangleData.Indices.Num();
	return EffectiveIndexCount >= 3 ? EffectiveIndexCount / 3 : EffectiveVertexCount / 3;
}

bool IsValidCSTriangleVertexIndex(int32 Index, int32 VertexCount)
{
	return Index >= 0 && Index < VertexCount;
}

bool IsFiniteCSPosition(const FVector& Position)
{
	return !Position.ContainsNaN()
		&& FMath::IsFinite(Position.X)
		&& FMath::IsFinite(Position.Y)
		&& FMath::IsFinite(Position.Z);
}

bool IsDegenerateCSTriangle(const FVector& A, const FVector& B, const FVector& C)
{
	return FVector::CrossProduct(B - A, C - A).SizeSquared() <= 1.0e-8;
}

FVector GetSafeCSTriangleNormal(const FVector& A, const FVector& B, const FVector& C)
{
	return FVector::CrossProduct(B - A, C - A).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
}

bool ShouldIncludeActorForDrawPrimtiveTag(const AActor* Actor, FName RequiredActorTag)
{
	return RequiredActorTag.IsNone() || (Actor && Actor->ActorHasTag(RequiredActorTag));
}

FBox BuildWorldBoxFromTransformAndSize(const FTransform& BoxTransform, const FVector& BoxSize)
{
	const FVector BoxExtent(
		FMath::Max(0.0, FMath::Abs(BoxSize.X) * 0.5),
		FMath::Max(0.0, FMath::Abs(BoxSize.Y) * 0.5),
		FMath::Max(0.0, FMath::Abs(BoxSize.Z) * 0.5));
	if (BoxExtent.IsNearlyZero())
	{
		return FBox(ForceInit);
	}

	return FBox(-BoxExtent, BoxExtent).TransformBy(BoxTransform);
}

bool TriangleIntersectsBox(const FVector& A, const FVector& B, const FVector& C, const FBox& QueryBox)
{
	if (!QueryBox.IsValid)
	{
		return true;
	}

	FBox TriangleBox(ForceInit);
	TriangleBox += A;
	TriangleBox += B;
	TriangleBox += C;
	return TriangleBox.Intersect(QueryBox);
}

bool TriangleIntersectsLocalBox(const FVector& A,
	const FVector& B,
	const FVector& C,
	const FTransform& WorldToBoxTransform,
	const FVector& BoxExtent)
{
	FBox LocalTriangleBox(ForceInit);
	LocalTriangleBox += WorldToBoxTransform.TransformPosition(A);
	LocalTriangleBox += WorldToBoxTransform.TransformPosition(B);
	LocalTriangleBox += WorldToBoxTransform.TransformPosition(C);
	return LocalTriangleBox.Intersect(FBox(-BoxExtent, BoxExtent));
}

bool TryAppendTriangleSoup(FCSTriangleMeshData& OutTriangleData,
	const FVector& A,
	const FVector& B,
	const FVector& C,
	const FVector& Normal,
	int32 MaxTriangles)
{
	if (MaxTriangles > 0 && GetCSTriangleMeshTriangleCount(OutTriangleData) >= MaxTriangles)
	{
		return false;
	}

	if (!IsFiniteCSPosition(A) || !IsFiniteCSPosition(B) || !IsFiniteCSPosition(C) || IsDegenerateCSTriangle(A, B, C))
	{
		return true;
	}

	const FVector SafeNormal = Normal.GetSafeNormal(UE_SMALL_NUMBER, GetSafeCSTriangleNormal(A, B, C));
	OutTriangleData.Vertices.Add(A);
	OutTriangleData.Vertices.Add(B);
	OutTriangleData.Vertices.Add(C);
	OutTriangleData.VertexNormals.Add(SafeNormal);
	OutTriangleData.VertexNormals.Add(SafeNormal);
	OutTriangleData.VertexNormals.Add(SafeNormal);
	OutTriangleData.VertexCount = OutTriangleData.Vertices.Num();
	OutTriangleData.IndexCount = 0;
	return true;
}

bool TryAppendTriangleSoupOrientedToNormal(FCSTriangleMeshData& OutTriangleData,
	const FVector& A,
	const FVector& B,
	const FVector& C,
	const FVector& DesiredNormal,
	int32 MaxTriangles)
{
	FVector SafeDesiredNormal = DesiredNormal.GetSafeNormal(UE_SMALL_NUMBER, GetSafeCSTriangleNormal(A, B, C));
	if (FVector::DotProduct(GetSafeCSTriangleNormal(A, B, C), SafeDesiredNormal) < 0.0)
	{
		return TryAppendTriangleSoup(OutTriangleData, A, C, B, SafeDesiredNormal, MaxTriangles);
	}

	return TryAppendTriangleSoup(OutTriangleData, A, B, C, SafeDesiredNormal, MaxTriangles);
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

void AppendTriangleMeshDataInsideLocalBox(FCSTriangleMeshData& OutTriangleData,
	const FCSTriangleMeshData& SourceTriangleData,
	int32 MaxTriangles,
	const FTransform& WorldToBoxTransform,
	const FVector& BoxExtent)
{
	const int32 EffectiveVertexCount = SourceTriangleData.VertexCount >= 0
		? FMath::Clamp(SourceTriangleData.VertexCount, 0, SourceTriangleData.Vertices.Num())
		: SourceTriangleData.Vertices.Num();
	const int32 EffectiveIndexCount = SourceTriangleData.IndexCount >= 0
		? FMath::Clamp(SourceTriangleData.IndexCount, 0, SourceTriangleData.Indices.Num())
		: SourceTriangleData.Indices.Num();
	const bool bUseIndices = EffectiveIndexCount >= 3;
	const int32 TriangleCount = bUseIndices ? EffectiveIndexCount / 3 : EffectiveVertexCount / 3;
	const bool bUseVertexNormals = SourceTriangleData.VertexNormals.Num() >= EffectiveVertexCount;

	for (int32 TriangleIndex = 0; TriangleIndex < TriangleCount; ++TriangleIndex)
	{
		if (MaxTriangles > 0 && GetCSTriangleMeshTriangleCount(OutTriangleData) >= MaxTriangles)
		{
			break;
		}

		int32 I0 = TriangleIndex * 3 + 0;
		int32 I1 = TriangleIndex * 3 + 1;
		int32 I2 = TriangleIndex * 3 + 2;
		if (bUseIndices)
		{
			I0 = SourceTriangleData.Indices[TriangleIndex * 3 + 0];
			I1 = SourceTriangleData.Indices[TriangleIndex * 3 + 1];
			I2 = SourceTriangleData.Indices[TriangleIndex * 3 + 2];
		}

		if (!IsValidCSTriangleVertexIndex(I0, EffectiveVertexCount)
			|| !IsValidCSTriangleVertexIndex(I1, EffectiveVertexCount)
			|| !IsValidCSTriangleVertexIndex(I2, EffectiveVertexCount))
		{
			continue;
		}

		const FVector& A = SourceTriangleData.Vertices[I0];
		const FVector& B = SourceTriangleData.Vertices[I1];
		const FVector& C = SourceTriangleData.Vertices[I2];
		if (!TriangleIntersectsLocalBox(A, B, C, WorldToBoxTransform, BoxExtent))
		{
			continue;
		}

		const FVector Normal = bUseVertexNormals
			? ((SourceTriangleData.VertexNormals[I0] + SourceTriangleData.VertexNormals[I1] + SourceTriangleData.VertexNormals[I2]) / 3.0).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector)
			: GetSafeCSTriangleNormal(A, B, C);
		if (!TryAppendTriangleSoup(OutTriangleData, A, B, C, Normal, MaxTriangles))
		{
			break;
		}
	}
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

		if (!IsValidCSTriangleVertexIndex(I0, EffectiveVertexCount)
			|| !IsValidCSTriangleVertexIndex(I1, EffectiveVertexCount)
			|| !IsValidCSTriangleVertexIndex(I2, EffectiveVertexCount))
		{
			continue;
		}

		const FVector& A = TriangleData.Vertices[I0];
		const FVector& B = TriangleData.Vertices[I1];
		const FVector& C = TriangleData.Vertices[I2];
		const FVector WindingNormal = GetSafeCSTriangleNormal(A, B, C);
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

void AppendStaticMeshComponentTriangles(UStaticMeshComponent* StaticMeshComponent,
	const FBox& QueryBox,
	const FTransform& WorldToBoxTransform,
	const FVector& BoxExtent,
	int32 LODIndex,
	int32 MaxTriangles,
	FCSTriangleMeshData& OutTriangleData)
{
	if (!StaticMeshComponent)
	{
		return;
	}

	UStaticMesh* StaticMesh = StaticMeshComponent->GetStaticMesh();
	if (!StaticMesh)
	{
		return;
	}

	FStaticMeshRenderData* RenderData = StaticMesh->GetRenderData();
	if (!RenderData || RenderData->LODResources.Num() == 0)
	{
		return;
	}

	const int32 SafeLODIndex = FMath::Clamp(LODIndex, 0, RenderData->LODResources.Num() - 1);
	FStaticMeshLODResources& LOD = RenderData->LODResources[SafeLODIndex];
	if (LOD.GetNumTriangles() <= 0)
	{
		return;
	}

	const FIndexArrayView Indices = LOD.IndexBuffer.GetArrayView();
	FPositionVertexBuffer& PositionBuffer = LOD.VertexBuffers.PositionVertexBuffer;
	FStaticMeshVertexBuffer& VertexBuffer = LOD.VertexBuffers.StaticMeshVertexBuffer;

	auto AppendWithTransform = [&](const FTransform& LocalToWorld)
	{
		for (int32 TriangleIndex = 0; TriangleIndex < LOD.GetNumTriangles(); ++TriangleIndex)
		{
			if (MaxTriangles > 0 && GetCSTriangleMeshTriangleCount(OutTriangleData) >= MaxTriangles)
			{
				return;
			}

			const uint32 I0 = Indices[TriangleIndex * 3 + 0];
			const uint32 I1 = Indices[TriangleIndex * 3 + 1];
			const uint32 I2 = Indices[TriangleIndex * 3 + 2];
			if (I0 >= PositionBuffer.GetNumVertices()
				|| I1 >= PositionBuffer.GetNumVertices()
				|| I2 >= PositionBuffer.GetNumVertices())
			{
				continue;
			}

			const FVector A = LocalToWorld.TransformPosition(FVector(PositionBuffer.VertexPosition(I0)));
			const FVector B = LocalToWorld.TransformPosition(FVector(PositionBuffer.VertexPosition(I1)));
			const FVector C = LocalToWorld.TransformPosition(FVector(PositionBuffer.VertexPosition(I2)));
			if (!TriangleIntersectsBox(A, B, C, QueryBox)
				|| !TriangleIntersectsLocalBox(A, B, C, WorldToBoxTransform, BoxExtent))
			{
				continue;
			}

			FVector Normal = GetSafeCSTriangleNormal(A, B, C);
			if (I0 < VertexBuffer.GetNumVertices()
				&& I1 < VertexBuffer.GetNumVertices()
				&& I2 < VertexBuffer.GetNumVertices())
			{
				const FVector N0 = LocalToWorld.TransformVector(FVector(VertexBuffer.VertexTangentZ(I0)));
				const FVector N1 = LocalToWorld.TransformVector(FVector(VertexBuffer.VertexTangentZ(I1)));
				const FVector N2 = LocalToWorld.TransformVector(FVector(VertexBuffer.VertexTangentZ(I2)));
				Normal = ((N0 + N1 + N2) / 3.0).GetSafeNormal(UE_SMALL_NUMBER, Normal);
			}

			if (!TryAppendTriangleSoupOrientedToNormal(OutTriangleData, A, B, C, Normal, MaxTriangles))
			{
				return;
			}
		}
	};

	if (UInstancedStaticMeshComponent* InstancedComponent = Cast<UInstancedStaticMeshComponent>(StaticMeshComponent))
	{
		const FBox LocalMeshBounds = StaticMesh->GetBoundingBox();
		for (int32 InstanceIndex = 0; InstanceIndex < InstancedComponent->GetInstanceCount(); ++InstanceIndex)
		{
			if (MaxTriangles > 0 && GetCSTriangleMeshTriangleCount(OutTriangleData) >= MaxTriangles)
			{
				break;
			}

			FTransform InstanceTransform = FTransform::Identity;
			InstancedComponent->GetInstanceTransform(InstanceIndex, InstanceTransform, true);
			if (!LocalMeshBounds.TransformBy(InstanceTransform).Intersect(QueryBox))
			{
				continue;
			}
			AppendWithTransform(InstanceTransform);
		}
		return;
	}

	if (!StaticMeshComponent->Bounds.GetBox().Intersect(QueryBox))
	{
		return;
	}

	AppendWithTransform(StaticMeshComponent->GetComponentTransform());
}

void AppendLandscapeTriangles(UWorld* World,
	const FBox& QueryBox,
	const FTransform& WorldToBoxTransform,
	const FVector& BoxExtent,
	FName RequiredActorTag,
	int32 MaxTriangles,
	FCSTriangleMeshData& OutTriangleData)
{
#if WITH_EDITOR
	if (!World)
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

		ALandscapeProxy* LandscapeProxy = LandscapeComponent->GetLandscapeProxy();
		if (!LandscapeProxy || !ShouldIncludeActorForDrawPrimtiveTag(LandscapeProxy, RequiredActorTag))
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
				if (MaxTriangles > 0 && GetCSTriangleMeshTriangleCount(OutTriangleData) >= MaxTriangles)
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

				if (TriangleIntersectsBox(P00, P10, P11, QueryBox)
					&& TriangleIntersectsLocalBox(P00, P10, P11, WorldToBoxTransform, BoxExtent))
				{
					if (!TryAppendTriangleSoupOrientedToNormal(OutTriangleData, P00, P10, P11, MakeLandscapeNormalFaceUp(N00 + N10 + N11), MaxTriangles))
					{
						return;
					}
				}

				if (TriangleIntersectsBox(P00, P11, P01, QueryBox)
					&& TriangleIntersectsLocalBox(P00, P11, P01, WorldToBoxTransform, BoxExtent))
				{
					if (!TryAppendTriangleSoupOrientedToNormal(OutTriangleData, P00, P11, P01, MakeLandscapeNormalFaceUp(N00 + N11 + N01), MaxTriangles))
					{
						return;
					}
				}
			}
		}
	}
#else
	(void)World;
	(void)QueryBox;
	(void)WorldToBoxTransform;
	(void)BoxExtent;
	(void)RequiredActorTag;
	(void)MaxTriangles;
	(void)OutTriangleData;
#endif
}

uint32 BuildTriangleUploadData(const FCSTriangleMeshData& TriangleData, TArray<FVector4f>& OutVertices)
{
	const int32 EffectiveVertexCount = TriangleData.VertexCount >= 0
		? FMath::Clamp(TriangleData.VertexCount, 0, TriangleData.Vertices.Num())
		: TriangleData.Vertices.Num();
	const int32 EffectiveIndexCount = TriangleData.IndexCount >= 0
		? FMath::Clamp(TriangleData.IndexCount, 0, TriangleData.Indices.Num())
		: TriangleData.Indices.Num();
	const bool bUseIndices = EffectiveIndexCount >= 3;
	const int32 TriangleCount = bUseIndices ? EffectiveIndexCount / 3 : EffectiveVertexCount / 3;
	if (TriangleCount <= 0)
	{
		return 0;
	}

	OutVertices.Reset(TriangleCount * 3);
	OutVertices.Reserve(TriangleCount * 3);
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

		if (!IsValidCSTriangleVertexIndex(I0, EffectiveVertexCount)
			|| !IsValidCSTriangleVertexIndex(I1, EffectiveVertexCount)
			|| !IsValidCSTriangleVertexIndex(I2, EffectiveVertexCount))
		{
			continue;
		}

		const FVector& A = TriangleData.Vertices[I0];
		const FVector& B = TriangleData.Vertices[I1];
		const FVector& C = TriangleData.Vertices[I2];
		if (!IsFiniteCSPosition(A) || !IsFiniteCSPosition(B) || !IsFiniteCSPosition(C) || IsDegenerateCSTriangle(A, B, C))
		{
			continue;
		}

		OutVertices.Add(FVector4f(FVector3f(A), 1.0f));
		OutVertices.Add(FVector4f(FVector3f(B), 1.0f));
		OutVertices.Add(FVector4f(FVector3f(C), 1.0f));
	}

	return uint32(OutVertices.Num() / 3);
}

bool EnsureR32FloatRenderTarget(UTextureRenderTarget2D* RenderTarget, float EmptyHeight)
{
	if (!RenderTarget || RenderTarget->SizeX <= 0 || RenderTarget->SizeY <= 0)
	{
		return false;
	}

	const bool bNeedsRecreate = RenderTarget->RenderTargetFormat != RTF_R32f || !RenderTarget->bSupportsUAV;
	if (!bNeedsRecreate)
	{
		return true;
	}

	const int32 Width = RenderTarget->SizeX;
	const int32 Height = RenderTarget->SizeY;
	RenderTarget->ReleaseResource();
	RenderTarget->RenderTargetFormat = RTF_R32f;
	RenderTarget->bSupportsUAV = true;
	RenderTarget->ClearColor = FLinearColor(EmptyHeight, 0.0f, 0.0f, 1.0f);
	RenderTarget->InitAutoFormat(Width, Height);
	RenderTarget->UpdateResourceImmediate(true);
	return true;
}
}

class FVertexDataDeclaration : public FRenderResource
{
public:
	FVertexDeclarationRHIRef VertexDeclaration;

	// Destructor
	virtual ~FVertexDataDeclaration() {}

	virtual void InitRHI(FRHICommandListBase& RHICmdList) override
	{
		FVertexDeclarationElementList Elements;
		
		uint16 Stride = sizeof(FCSVertexData);
		Elements.Add(FVertexElement(0, STRUCT_OFFSET(FCSVertexData, Position), VET_Float4, 0, Stride));
		Elements.Add(FVertexElement(0, STRUCT_OFFSET(FCSVertexData, UV), VET_Float2, 1, Stride));
		// Elements.Add(FVertexElement(0, STRUCT_OFFSET(FCSVertexData, Normal), VET_Float4, 2, Stride));
		VertexDeclaration = PipelineStateCache::GetOrCreateVertexDeclaration(Elements);
	}

	virtual void ReleaseRHI() override
	{
		VertexDeclaration.SafeRelease();
	}

	
};

class FDrawRasterVertexDeclaration : public FRenderResource
{
public:
	FVertexDeclarationRHIRef VertexDeclaration;

	virtual ~FDrawRasterVertexDeclaration() {}

	virtual void InitRHI(FRHICommandListBase& RHICmdList) override
	{
		FVertexDeclarationElementList Elements;
		const uint16 Stride = sizeof(FDrawRasterVertexData);
		Elements.Add(FVertexElement(0, STRUCT_OFFSET(FDrawRasterVertexData, Position), VET_Float4, 0, Stride));
		Elements.Add(FVertexElement(0, STRUCT_OFFSET(FDrawRasterVertexData, Normal), VET_Float4, 1, Stride));
		VertexDeclaration = PipelineStateCache::GetOrCreateVertexDeclaration(Elements);
	}

	virtual void ReleaseRHI() override
	{
		VertexDeclaration.SafeRelease();
	}
};

class FSimpleScreenVertexBuffer : public FVertexBuffer
{
public:
	/** Initialize the RHI for this rendering resource */
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
};
void FSimpleScreenVertexBuffer::InitRHI(FRHICommandListBase& RHICmdList)
{
	TResourceArray<FFilterVertex, VERTEXBUFFER_ALIGNMENT> Vertices;
	Vertices.SetNumUninitialized(6);

	Vertices[0].Position = FVector4f(-1, 1, 0, 1);
	Vertices[0].UV = FVector2f(0, 0);

	Vertices[1].Position = FVector4f(1, 1, 0, 1);
	Vertices[1].UV = FVector2f(1, 0);

	Vertices[2].Position = FVector4f(-1, -1, 0, 1);
	Vertices[2].UV = FVector2f(0, 1);

	Vertices[3].Position = FVector4f(1, -1, 0, 1);
	Vertices[3].UV = FVector2f(1, 1);

	FRHIBufferCreateDesc CreateDesc = FRHIBufferCreateDesc::CreateVertex(TEXT("ShaderDemoSquare"), Vertices.GetResourceDataSize())
		.DetermineInitialState()
		.SetInitActionResourceArray(&Vertices);
	VertexBufferRHI = RHICmdList.CreateBuffer(CreateDesc);
}



class FDrawInstanceHeightVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDrawInstanceHeightVS);
	SHADER_USE_PARAMETER_STRUCT(FDrawInstanceHeightVS, FGlobalShader);

public:

	enum class EDrawTypeVS : uint8
	{
		DIVS_Test,
		DIVS_DrawHeight,
		MAX
	};

	class FDrawType : SHADER_PERMUTATION_ENUM_CLASS("DIVS", EDrawTypeVS);
	using FPermutationDomain = TShaderPermutationDomain<FDrawType>;
	static TShaderMapRef<FDrawInstanceHeightVS> CreatePermutation(EDrawTypeVS Permutation)
	{
		typename FPermutationDomain PermutationVector;
		PermutationVector.Set<FDrawType>(Permutation);
		TShaderMapRef<FDrawInstanceHeightVS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
		return ComputeShader;
	}
	
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return FDataDrivenShaderPlatformInfo::GetSupportsVertexShaderSRVs(Parameters.Platform);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, InstanceTransform)

		// SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GPUSceneInstancePayloadData)
		// SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GPUScenePrimitiveSceneData)
		SHADER_PARAMETER(FMatrix44f, V2P)
		SHADER_PARAMETER(FMatrix44f, L2WTest)
		
	END_SHADER_PARAMETER_STRUCT()

	static inline void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		static const TCHAR* ShaderSourceModeDefineName[] =
		{
			TEXT("DIVS_TEST"),
			TEXT("DIVS_DRAWHEIGHT"),
		}; 
		static_assert(UE_ARRAY_COUNT(ShaderSourceModeDefineName) == (uint32)EDrawTypeVS::MAX, "Enum doesn't match define table.");

		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const uint32 SourceModeIndex = static_cast<uint32>(PermutationVector.Get<FDrawType>());
		OutEnvironment.SetDefine(ShaderSourceModeDefineName[SourceModeIndex], 1u);

	}
};



class FDrawInstanceHeightPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDrawInstanceHeightPS);
	SHADER_USE_PARAMETER_STRUCT(FDrawInstanceHeightPS, FGlobalShader);

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return FDataDrivenShaderPlatformInfo::GetSupportsVertexShaderSRVs(Parameters.Platform);
	}


	enum class EDrawTypePS : uint8
	{
		DIPS_PreDepth,
		DIPS_DrawHeight,
		MAX
	};

	class FDrawType : SHADER_PERMUTATION_ENUM_CLASS("DIPS", EDrawTypePS);
	using FPermutationDomain = TShaderPermutationDomain<FDrawType>;
	static TShaderMapRef<FDrawInstanceHeightPS> CreatePermutation(EDrawTypePS Permutation)
	{
		typename FPermutationDomain PermutationVector;
		PermutationVector.Set<FDrawType>(Permutation);
		TShaderMapRef<FDrawInstanceHeightPS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
		return ComputeShader;
	}
	
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_DebugView)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_Normal)
	END_SHADER_PARAMETER_STRUCT()


	static inline void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		static const TCHAR* ShaderSourceModeDefineName[] =
		{
			TEXT("DIPS_PREDEPTH"),
			TEXT("DIPS_DRAWHEIGHT"),
		}; 
		static_assert(UE_ARRAY_COUNT(ShaderSourceModeDefineName) == (uint32)EDrawTypePS::MAX, "Enum doesn't match define table.");

		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const uint32 SourceModeIndex = static_cast<uint32>(PermutationVector.Get<FDrawType>());
		OutEnvironment.SetDefine(ShaderSourceModeDefineName[SourceModeIndex], 1u);

	}
};

class FDrawInstancesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDrawInstancesCS);
	SHADER_USE_PARAMETER_STRUCT(FDrawInstancesCS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, TriangleData)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_OutputTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_DepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_DebugTexture)
		SHADER_PARAMETER(uint32, TriangleCount)
		SHADER_PARAMETER(float, CaptureWidth)
		SHADER_PARAMETER(float, MinDepth)
		SHADER_PARAMETER(float, DepthRange)
		SHADER_PARAMETER(float, MaxDepth)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static inline void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 8);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Y"), 8);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Z"), 1);
	}
};

class FDrawInstancesRasterVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDrawInstancesRasterVS);
	SHADER_USE_PARAMETER_STRUCT(FDrawInstancesRasterVS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(float, InvHalfCaptureWidth)
		SHADER_PARAMETER(float, InvHalfCaptureHeight)
		SHADER_PARAMETER(float, RasterMinDepth)
		SHADER_PARAMETER(float, RasterInvDepthRange)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}
};

class FDrawInstancesRasterPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDrawInstancesRasterPS);
	SHADER_USE_PARAMETER_STRUCT(FDrawInstancesRasterPS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}
};

class FDrawBoxWorldZHeightCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDrawBoxWorldZHeightCS);
	SHADER_USE_PARAMETER_STRUCT(FDrawBoxWorldZHeightCS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, BoxHeightTriangleData)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, BoxHeightTriangleCounter)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RW_HeightTexture)
		SHADER_PARAMETER(uint32, BoxHeightTriangleCount)
		SHADER_PARAMETER(uint32, bUseTriangleCounter)
		SHADER_PARAMETER(FMatrix44f, WorldToBox)
		SHADER_PARAMETER(FVector3f, BoxExtent)
		SHADER_PARAMETER(FIntPoint, OutputSize)
		SHADER_PARAMETER(float, EmptyHeight)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static inline void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 8);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Y"), 8);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Z"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FDrawInstanceHeightVS, "/Plugin/PCGPlugins/Shaders/Private/DrawPrimtive.usf", "MainVertexShader", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FDrawInstanceHeightPS, "/Plugin/PCGPlugins/Shaders/Private/DrawPrimtive.usf", "MainPixelShader", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FDrawInstancesCS, "/Plugin/PCGPlugins/Shaders/Private/DrawPrimtive.usf", "DrawInstancesCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FDrawInstancesRasterVS, "/Plugin/PCGPlugins/Shaders/Private/DrawPrimtive.usf", "DrawInstancesRasterVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FDrawInstancesRasterPS, "/Plugin/PCGPlugins/Shaders/Private/DrawPrimtive.usf", "DrawInstancesRasterPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FDrawBoxWorldZHeightCS, "/Plugin/PCGPlugins/Shaders/Private/DrawPrimtive.usf", "DrawBoxWorldZHeightCS", SF_Compute);


BEGIN_SHADER_PARAMETER_STRUCT(FDrawInstanceHeight, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FDrawInstanceHeightVS::FParameters, VS)
	SHADER_PARAMETER_STRUCT_INCLUDE(FDrawInstanceHeightPS::FParameters, PS)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FDrawInstancesRasterPassParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FDrawInstancesRasterVS::FParameters, VS)
	SHADER_PARAMETER_STRUCT_INCLUDE(FDrawInstancesRasterPS::FParameters, PS)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()


TGlobalResource<FVertexDataDeclaration> VertexDataResource;
TGlobalResource<FDrawRasterVertexDeclaration> DrawRasterVertexDataResource;
TGlobalResource<FSimpleScreenVertexBuffer> GSimpleScreenVertexBuffer;

FCSBoxWorldZHeightRDGOutput UCSDrawPrimtive::AddBoxWorldZHeightToRDG(
	FRDGBuilder& GraphBuilder,
	const FCSBoxWorldZHeightRDGInput& Input)
{
	FCSBoxWorldZHeightRDGOutput Output;

	FRDGTextureRef HeightTexture = Input.OutputTexture;
	if (HeightTexture && HeightTexture->Desc.Format != PF_R32_FLOAT)
	{
		UE_LOG(LogTemp, Warning, TEXT("[UCSDrawPrimtive::AddBoxWorldZHeightToRDG] OutputTexture must be PF_R32_FLOAT. A new R32 texture will be created."));
		HeightTexture = nullptr;
	}

	FIntPoint OutputSize = HeightTexture ? HeightTexture->Desc.Extent : Input.OutputSize;
	OutputSize.X = FMath::Max(1, OutputSize.X);
	OutputSize.Y = FMath::Max(1, OutputSize.Y);
	Output.OutputSize = OutputSize;

	if (!HeightTexture)
	{
		const FRDGTextureDesc HeightDesc = FRDGTextureDesc::Create2D(
			OutputSize,
			PF_R32_FLOAT,
			FClearValueBinding(FLinearColor(Input.EmptyHeight, 0.0f, 0.0f, 1.0f)),
			TexCreate_ShaderResource | TexCreate_UAV | TexCreate_RenderTargetable);
		HeightTexture = GraphBuilder.CreateTexture(HeightDesc, TEXT("CS.BoxWorldZHeight.Texture"));
	}
	Output.HeightTexture = HeightTexture;

	FRDGTextureUAVRef HeightUAV = GraphBuilder.CreateUAV(HeightTexture);
	AddClearUAVPass(GraphBuilder, HeightUAV, Input.EmptyHeight);

	const FVector BoxExtent(
		FMath::Abs(Input.BoxSize.X) * 0.5,
		FMath::Abs(Input.BoxSize.Y) * 0.5,
		FMath::Abs(Input.BoxSize.Z) * 0.5);
	if (!Input.TriangleVerticesSRV
		|| BoxExtent.X <= CSBoxWorldZMinExtent
		|| BoxExtent.Y <= CSBoxWorldZMinExtent
		|| BoxExtent.Z <= CSBoxWorldZMinExtent
		|| (Input.TriangleCount == 0 && !Input.TriangleCounterSRV))
	{
		return Output;
	}

	FRDGBufferSRVRef TriangleCounterSRV = Input.TriangleCounterSRV;
	if (!TriangleCounterSRV)
	{
		FRDGBufferRef DummyCounterBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1),
			TEXT("CS.BoxWorldZHeight.DummyCounter"));
		uint32* DummyCounterUploadData = GraphBuilder.AllocPODArray<uint32>(1);
		*DummyCounterUploadData = Input.TriangleCount;
		GraphBuilder.QueueBufferUpload(DummyCounterBuffer, DummyCounterUploadData, sizeof(uint32));
		TriangleCounterSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(DummyCounterBuffer, PF_R32_UINT));
	}

	FDrawBoxWorldZHeightCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FDrawBoxWorldZHeightCS::FParameters>();
	PassParameters->BoxHeightTriangleData = Input.TriangleVerticesSRV;
	PassParameters->BoxHeightTriangleCounter = TriangleCounterSRV;
	PassParameters->RW_HeightTexture = HeightUAV;
	PassParameters->BoxHeightTriangleCount = Input.TriangleCount;
	PassParameters->bUseTriangleCounter = Input.TriangleCounterSRV ? 1u : 0u;
	PassParameters->WorldToBox = FMatrix44f(Input.BoxTransform.Inverse().ToMatrixWithScale());
	PassParameters->BoxExtent = FVector3f(BoxExtent);
	PassParameters->OutputSize = OutputSize;
	PassParameters->EmptyHeight = Input.EmptyHeight;

	TShaderMapRef<FDrawBoxWorldZHeightCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("%s", Input.DebugName ? Input.DebugName : TEXT("CS.BoxWorldZHeight")),
		ERDGPassFlags::Compute,
		ComputeShader,
		PassParameters,
		FComputeShaderUtils::GetGroupCount(FIntVector(OutputSize.X, OutputSize.Y, 1), FIntVector(8, 8, 1)));

	return Output;
}

FCSBoxWorldZHeightRDGOutput UCSDrawPrimtive::AddStaticMeshTrianglesWorldZHeightToRDG(
	FRDGBuilder& GraphBuilder,
	const FCSStaticMeshTriangleRDGOutput& TriangleOutput,
	FTransform BoxTransform,
	FVector BoxSize,
	FIntPoint OutputSize,
	FRDGTextureRef OutputTexture,
	float EmptyHeight,
	const TCHAR* DebugName)
{
	FCSBoxWorldZHeightRDGInput Input;
	Input.TriangleVerticesSRV = TriangleOutput.TriangleVerticesSRV;
	Input.TriangleCounterSRV = TriangleOutput.TriangleCounterSRV;
	Input.TriangleCount = TriangleOutput.MaxTriangles;
	Input.BoxTransform = BoxTransform;
	Input.BoxSize = BoxSize;
	Input.OutputSize = OutputSize;
	Input.OutputTexture = OutputTexture;
	Input.EmptyHeight = EmptyHeight;
	Input.DebugName = DebugName;
	return AddBoxWorldZHeightToRDG(GraphBuilder, Input);
}

FCSBoxWorldZHeightRDGOutput UCSDrawPrimtive::AddTriangleMeshWorldZHeightToRDG(
	FRDGBuilder& GraphBuilder,
	const FCSTriangleMeshData& TriangleData,
	FTransform BoxTransform,
	FVector BoxSize,
	FIntPoint OutputSize,
	FRDGTextureRef OutputTexture,
	float EmptyHeight,
	const TCHAR* DebugName)
{
	FCSBoxWorldZHeightRDGInput Input;
	Input.BoxTransform = BoxTransform;
	Input.BoxSize = BoxSize;
	Input.OutputSize = OutputSize;
	Input.OutputTexture = OutputTexture;
	Input.EmptyHeight = EmptyHeight;
	Input.DebugName = DebugName;

	TArray<FVector4f> TriangleVertices;
	const uint32 TriangleCount = BuildTriangleUploadData(TriangleData, TriangleVertices);
	Input.TriangleCount = TriangleCount;
	if (TriangleCount > 0)
	{
		FVector4f* UploadData = GraphBuilder.AllocPODArray<FVector4f>(TriangleVertices.Num());
		FMemory::Memcpy(UploadData, TriangleVertices.GetData(), TriangleVertices.Num() * sizeof(FVector4f));

		FRDGBufferRef TriangleBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), TriangleVertices.Num()),
			TEXT("CS.BoxWorldZHeight.UploadedTriangles"));
		GraphBuilder.QueueBufferUpload(TriangleBuffer, UploadData, TriangleVertices.Num() * sizeof(FVector4f));
		Input.TriangleVerticesSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(TriangleBuffer, PF_A32B32G32R32F));
	}

	return AddBoxWorldZHeightToRDG(GraphBuilder, Input);
}

FCSTriangleMeshData UCSDrawPrimtive::GetTaggedBoxSceneTriangles(
	UObject* WorldContextObject,
	FTransform BoxTransform,
	FVector BoxSize,
	FName RequiredActorTag,
	int32 LODIndex,
	int32 MaxTriangles)
{
	FCSTriangleMeshData OutTriangleData;

	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	if (!World)
	{
		return OutTriangleData;
	}

	const FBox QueryBox = BuildWorldBoxFromTransformAndSize(BoxTransform, BoxSize);
	const FVector BoxExtent(
		FMath::Abs(BoxSize.X) * 0.5,
		FMath::Abs(BoxSize.Y) * 0.5,
		FMath::Abs(BoxSize.Z) * 0.5);
	if (!QueryBox.IsValid
		|| BoxExtent.X <= CSBoxWorldZMinExtent
		|| BoxExtent.Y <= CSBoxWorldZMinExtent
		|| BoxExtent.Z <= CSBoxWorldZMinExtent)
	{
		return OutTriangleData;
	}

	const FTransform WorldToBoxTransform = BoxTransform.Inverse();
	const int32 SafeMaxTriangles = FMath::Max(1, MaxTriangles);

	FCSTriangleMeshData LandscapeTriangleData;
	AppendLandscapeTriangles(World, QueryBox, WorldToBoxTransform, BoxExtent, RequiredActorTag, SafeMaxTriangles, LandscapeTriangleData);
	AppendTriangleMeshDataInsideLocalBox(OutTriangleData, LandscapeTriangleData, SafeMaxTriangles, WorldToBoxTransform, BoxExtent);

	for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
	{
		if (GetCSTriangleMeshTriangleCount(OutTriangleData) >= SafeMaxTriangles)
		{
			break;
		}

		UStaticMeshComponent* StaticMeshComponent = *It;
		if (!IsValid(StaticMeshComponent)
			|| StaticMeshComponent->IsTemplate()
			|| !StaticMeshComponent->IsRegistered()
			|| StaticMeshComponent->GetWorld() != World)
		{
			continue;
		}

		AActor* SourceActor = StaticMeshComponent->GetOwner();
		if (!ShouldIncludeActorForDrawPrimtiveTag(SourceActor, RequiredActorTag))
		{
			continue;
		}

		AppendStaticMeshComponentTriangles(
			StaticMeshComponent,
			QueryBox,
			WorldToBoxTransform,
			BoxExtent,
			LODIndex,
			SafeMaxTriangles,
			OutTriangleData);
	}

	OutTriangleData.VertexCount = OutTriangleData.Vertices.Num();
	OutTriangleData.IndexCount = 0;
	NormalizeTriangleMeshDataWinding(OutTriangleData);
	return OutTriangleData;
}

bool UCSDrawPrimtive::DrawTaggedBoxSceneWorldZHeight(
	UObject* WorldContextObject,
	FTransform BoxTransform,
	FVector BoxSize,
	UTextureRenderTarget2D* HeightRenderTarget,
	FName RequiredActorTag,
	int32 LODIndex,
	int32 MaxTriangles,
	float EmptyHeight)
{
	if (!EnsureR32FloatRenderTarget(HeightRenderTarget, EmptyHeight))
	{
		return false;
	}

	FCSTriangleMeshData TriangleData = GetTaggedBoxSceneTriangles(
		WorldContextObject,
		BoxTransform,
		BoxSize,
		RequiredActorTag,
		LODIndex,
		MaxTriangles);

	FTextureRenderTargetResource* HeightResource = HeightRenderTarget->GameThread_GetRenderTargetResource();
	const FIntPoint OutputSize(HeightRenderTarget->SizeX, HeightRenderTarget->SizeY);

	ENQUEUE_RENDER_COMMAND(CSDrawTaggedBoxSceneWorldZHeight)(
		[TriangleData = MoveTemp(TriangleData), HeightResource, BoxTransform, BoxSize, OutputSize, EmptyHeight](FRHICommandListImmediate& RHICmdList)
		{
			if (!HeightResource || !HeightResource->GetRenderTargetTexture().IsValid())
			{
				return;
			}

			FRDGBuilder GraphBuilder(RHICmdList);
			FRDGTextureRef ExternalHeightTexture = RegisterExternalTexture(GraphBuilder, HeightResource->GetRenderTargetTexture(), TEXT("CS.BoxWorldZHeight.ExternalRT"));
			UCSDrawPrimtive::AddTriangleMeshWorldZHeightToRDG(
				GraphBuilder,
				TriangleData,
				BoxTransform,
				BoxSize,
				OutputSize,
				ExternalHeightTexture,
				EmptyHeight,
				TEXT("CS.DrawTaggedBoxSceneWorldZHeight"));
			GraphBuilder.Execute();
		});

	FlushRenderingCommands();
	return true;
}

void UCSDrawPrimtive::DrawInstances(UTextureRenderTarget2D* RT_TextureTarget, UTextureRenderTarget2D* RT_Depth, UTextureRenderTarget2D* RT_DebugView, FTransform CameraTransform, float CaptureWidth, TArray<FTransform> InstanceTransforms, UStaticMesh* InStaticMesh)
{
	constexpr float EmptyDepthValue = 100000.0f;

	if (RT_TextureTarget == nullptr || RT_Depth == nullptr || InStaticMesh == nullptr)
	{
		return;
	}
	if (CaptureWidth <= KINDA_SMALL_NUMBER || InstanceTransforms.Num() == 0)
	{
		UKismetRenderingLibrary::ClearRenderTarget2D(GWorld, RT_TextureTarget);
		UKismetRenderingLibrary::ClearRenderTarget2D(GWorld, RT_Depth, FLinearColor(EmptyDepthValue, EmptyDepthValue, EmptyDepthValue, 1.0f));
		if (RT_DebugView != nullptr)
		{
			UKismetRenderingLibrary::ClearRenderTarget2D(GWorld, RT_DebugView);
		}
		return;
	}

	FStaticMeshRenderData* RenderData = InStaticMesh->GetRenderData();
	if (RenderData == nullptr || RenderData->LODResources.Num() == 0)
	{
		return;
	}

	FStaticMeshLODResources& LOD = RenderData->LODResources[0];
	if (LOD.GetNumTriangles() == 0)
	{
		return;
	}

	if (RT_TextureTarget->SizeX != RT_Depth->SizeX || RT_TextureTarget->SizeY != RT_Depth->SizeY)
	{
		RT_TextureTarget->ResizeTarget(RT_Depth->SizeX, RT_Depth->SizeY);
	}
	if (RT_DebugView != nullptr && (RT_DebugView->SizeX != RT_Depth->SizeX || RT_DebugView->SizeY != RT_Depth->SizeY))
	{
		RT_DebugView->ResizeTarget(RT_Depth->SizeX, RT_Depth->SizeY);
	}

	FTextureRenderTargetResource* R_Output = RT_TextureTarget->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Depth = RT_Depth->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Debug = RT_DebugView != nullptr ? RT_DebugView->GameThread_GetRenderTargetResource() : nullptr;

	const FIndexArrayView Indices = LOD.IndexBuffer.GetArrayView();
	FPositionVertexBuffer& PositionBuffer = LOD.VertexBuffers.PositionVertexBuffer;
	const FTransform CameraActorTransform(CameraTransform.GetRotation(), CameraTransform.GetLocation(), FVector::OneVector);
	const FTransform CaptureRelativeTransform(FRotator(-90.0f, -90.0f, 0.0f), FVector::ZeroVector, FVector::OneVector);
	const FTransform DirectCaptureTransform = CameraActorTransform;
	const FTransform DerivedCaptureTransform = CaptureRelativeTransform * CameraActorTransform;
	const FBox LocalBounds = InStaticMesh->GetBoundingBox();
	const FVector BoundsMin = LocalBounds.Min;
	const FVector BoundsMax = LocalBounds.Max;
	const FVector BoundsCorners[8] =
	{
		FVector(BoundsMin.X, BoundsMin.Y, BoundsMin.Z),
		FVector(BoundsMin.X, BoundsMin.Y, BoundsMax.Z),
		FVector(BoundsMin.X, BoundsMax.Y, BoundsMin.Z),
		FVector(BoundsMin.X, BoundsMax.Y, BoundsMax.Z),
		FVector(BoundsMax.X, BoundsMin.Y, BoundsMin.Z),
		FVector(BoundsMax.X, BoundsMin.Y, BoundsMax.Z),
		FVector(BoundsMax.X, BoundsMax.Y, BoundsMin.Z),
		FVector(BoundsMax.X, BoundsMax.Y, BoundsMax.Z)
	};
	const float HalfCaptureWidth = CaptureWidth * 0.5f;
	const float HalfCaptureHeight = HalfCaptureWidth * (float(R_Depth->GetSizeXY().Y) / FMath::Max((float)R_Depth->GetSizeXY().X, 1.0f));

	auto ScoreCaptureTransform = [&](const FTransform& CandidateTransform)
	{
		int32 Score = 0;
		for (const FTransform& InstanceTransform : InstanceTransforms)
		{
			for (const FVector& LocalCorner : BoundsCorners)
			{
				const FVector WorldCorner = InstanceTransform.TransformPosition(LocalCorner);
				const FVector CaptureCorner = CandidateTransform.InverseTransformPosition(WorldCorner);
				if (CaptureCorner.X >= 0.0f &&
					FMath::Abs(CaptureCorner.Y) <= HalfCaptureWidth &&
					FMath::Abs(CaptureCorner.Z) <= HalfCaptureHeight)
				{
					++Score;
				}
			}
		}
		return Score;
	};

	const int32 DirectTransformScore = ScoreCaptureTransform(DirectCaptureTransform);
	const int32 DerivedTransformScore = ScoreCaptureTransform(DerivedCaptureTransform);
	const FTransform CaptureTransform = DerivedTransformScore >= DirectTransformScore ? DerivedCaptureTransform : DirectCaptureTransform;

	struct FCaptureTriangleData
	{
		FVector3f P0;
		FVector3f P1;
		FVector3f P2;
		float MinDepth;
	};

	TArray<FCaptureTriangleData> CaptureTriangles;
	CaptureTriangles.Reserve(LOD.GetNumTriangles() * InstanceTransforms.Num());

	float MinVisibleDepth = TNumericLimits<float>::Max();
	float MaxVisibleDepth = 0.0f;
	bool bHasVisibleDepth = false;
	for (const FTransform& InstanceTransform : InstanceTransforms)
	{
		float InstanceMinX = TNumericLimits<float>::Max();
		float InstanceMaxX = -TNumericLimits<float>::Max();
		float InstanceMinY = TNumericLimits<float>::Max();
		float InstanceMaxY = -TNumericLimits<float>::Max();
		float InstanceMinZ = TNumericLimits<float>::Max();
		float InstanceMaxZ = -TNumericLimits<float>::Max();
		for (const FVector& LocalCorner : BoundsCorners)
		{
			const FVector WorldCorner = InstanceTransform.TransformPosition(LocalCorner);
			const FVector CaptureCorner = CaptureTransform.InverseTransformPosition(WorldCorner);
			InstanceMinX = FMath::Min(InstanceMinX, (float)CaptureCorner.X);
			InstanceMaxX = FMath::Max(InstanceMaxX, (float)CaptureCorner.X);
			InstanceMinY = FMath::Min(InstanceMinY, (float)CaptureCorner.Y);
			InstanceMaxY = FMath::Max(InstanceMaxY, (float)CaptureCorner.Y);
			InstanceMinZ = FMath::Min(InstanceMinZ, (float)CaptureCorner.Z);
			InstanceMaxZ = FMath::Max(InstanceMaxZ, (float)CaptureCorner.Z);
		}

		const bool bInstanceCulled =
			InstanceMaxX < 0.0f ||
			InstanceMaxY < -HalfCaptureWidth ||
			InstanceMinY > HalfCaptureWidth ||
			InstanceMaxZ < -HalfCaptureHeight ||
			InstanceMinZ > HalfCaptureHeight;

		if (bInstanceCulled)
		{
			continue;
		}

		for (int32 TriangleIndex = 0; TriangleIndex < LOD.GetNumTriangles(); ++TriangleIndex)
		{
			const uint32 I0 = Indices[TriangleIndex * 3 + 0];
			const uint32 I1 = Indices[TriangleIndex * 3 + 1];
			const uint32 I2 = Indices[TriangleIndex * 3 + 2];

			const FVector WorldP0 = InstanceTransform.TransformPosition(FVector(PositionBuffer.VertexPosition(I0)));
			const FVector WorldP1 = InstanceTransform.TransformPosition(FVector(PositionBuffer.VertexPosition(I1)));
			const FVector WorldP2 = InstanceTransform.TransformPosition(FVector(PositionBuffer.VertexPosition(I2)));

			const FVector CaptureP0 = CaptureTransform.InverseTransformPosition(WorldP0);
			const FVector CaptureP1 = CaptureTransform.InverseTransformPosition(WorldP1);
			const FVector CaptureP2 = CaptureTransform.InverseTransformPosition(WorldP2);
			FCaptureTriangleData Triangle;
			Triangle.P0 = FVector3f(CaptureP0);
			Triangle.P1 = FVector3f(CaptureP1);
			Triangle.P2 = FVector3f(CaptureP2);
			Triangle.MinDepth = FMath::Min3((float)CaptureP0.X, (float)CaptureP1.X, (float)CaptureP2.X);
			CaptureTriangles.Add(Triangle);

			if (CaptureP0.X >= 0.0f)
			{
				bHasVisibleDepth = true;
				MinVisibleDepth = FMath::Min(MinVisibleDepth, (float)CaptureP0.X);
				MaxVisibleDepth = FMath::Max(MaxVisibleDepth, (float)CaptureP0.X);
			}
			if (CaptureP1.X >= 0.0f)
			{
				bHasVisibleDepth = true;
				MinVisibleDepth = FMath::Min(MinVisibleDepth, (float)CaptureP1.X);
				MaxVisibleDepth = FMath::Max(MaxVisibleDepth, (float)CaptureP1.X);
			}
			if (CaptureP2.X >= 0.0f)
			{
				bHasVisibleDepth = true;
				MinVisibleDepth = FMath::Min(MinVisibleDepth, (float)CaptureP2.X);
				MaxVisibleDepth = FMath::Max(MaxVisibleDepth, (float)CaptureP2.X);
			}
		}
	}

	if (!bHasVisibleDepth)
	{
		MinVisibleDepth = 0.0f;
		MaxVisibleDepth = 1.0f;
	}

	if (CaptureTriangles.Num() == 0)
	{
		UKismetRenderingLibrary::ClearRenderTarget2D(GWorld, RT_TextureTarget);
		UKismetRenderingLibrary::ClearRenderTarget2D(GWorld, RT_Depth, FLinearColor(EmptyDepthValue, EmptyDepthValue, EmptyDepthValue, 1.0f));
		if (RT_DebugView != nullptr)
		{
			UKismetRenderingLibrary::ClearRenderTarget2D(GWorld, RT_DebugView);
		}
		return;
	}

	CaptureTriangles.Sort([](const FCaptureTriangleData& A, const FCaptureTriangleData& B)
	{
		return A.MinDepth < B.MinDepth;
	});

	TArray<FDrawRasterVertexData> RasterVertices;
	RasterVertices.Reserve(CaptureTriangles.Num() * 3);
	for (const FCaptureTriangleData& Triangle : CaptureTriangles)
	{
		FVector3f TriangleNormal = FVector3f::CrossProduct(Triangle.P1 - Triangle.P0, Triangle.P2 - Triangle.P0);
		if (!TriangleNormal.Normalize())
		{
			TriangleNormal = FVector3f(1.0f, 0.0f, 0.0f);
		}

		FDrawRasterVertexData V0;
		V0.Position = FVector4f(Triangle.P0, 1.0f);
		V0.Normal = FVector4f(TriangleNormal, 0.0f);
		RasterVertices.Add(V0);

		FDrawRasterVertexData V1;
		V1.Position = FVector4f(Triangle.P1, 1.0f);
		V1.Normal = FVector4f(TriangleNormal, 0.0f);
		RasterVertices.Add(V1);

		FDrawRasterVertexData V2;
		V2.Position = FVector4f(Triangle.P2, 1.0f);
		V2.Normal = FVector4f(TriangleNormal, 0.0f);
		RasterVertices.Add(V2);
	}

	const float DepthRange = FMath::Max(MaxVisibleDepth - MinVisibleDepth, 1.0f);


	ENQUEUE_RENDER_COMMAND(FRPCSRunner)(
		[RasterVertices = MoveTemp(RasterVertices), R_Output, R_Depth, R_Debug, CaptureWidth, MinVisibleDepth, DepthRange](FRHICommandListImmediate& RHICmdList)
		{
			if (RasterVertices.Num() == 0)
			{
				return;
			}

			TResourceArray<FDrawRasterVertexData, VERTEXBUFFER_ALIGNMENT> VertexResourceArray;
			VertexResourceArray.SetNumUninitialized(RasterVertices.Num());
			FMemory::Memcpy(VertexResourceArray.GetData(), RasterVertices.GetData(), RasterVertices.Num() * sizeof(FDrawRasterVertexData));

			FRHIBufferCreateDesc VertexCreateDesc = FRHIBufferCreateDesc::CreateVertex(TEXT("DrawInstanceRasterVB"), VertexResourceArray.GetResourceDataSize())
				.DetermineInitialState()
				.SetInitActionResourceArray(&VertexResourceArray);
			FBufferRHIRef VertexBufferRHI = RHICmdList.CreateBuffer(VertexCreateDesc);

			FRDGBuilder GraphBuilder(RHICmdList);
			{
				const float MaxDepth = EmptyDepthValue;
				const FIntPoint TextureSize = R_Depth->GetSizeXY();
				const float HalfCaptureWidth = CaptureWidth * 0.5f;
				const float HalfCaptureHeight = HalfCaptureWidth * ((float)TextureSize.Y / FMath::Max((float)TextureSize.X, 1.0f));
				const EPixelFormat OutputFormat = R_Output->GetRenderTargetTexture()->GetFormat();
				const EPixelFormat DepthFormat = R_Depth->GetRenderTargetTexture()->GetFormat();
				const EPixelFormat DebugFormat = R_Debug != nullptr ? R_Debug->GetRenderTargetTexture()->GetFormat() : PF_A32B32G32R32F;

				FRDGTextureDesc OutputDesc = FRDGTextureDesc::Create2D(TextureSize, OutputFormat, FClearValueBinding::Black, TexCreate_RenderTargetable | TexCreate_ShaderResource);
				FRDGTextureDesc DepthPreviewDesc = FRDGTextureDesc::Create2D(TextureSize, DepthFormat, FClearValueBinding::Black, TexCreate_RenderTargetable | TexCreate_ShaderResource);
				FRDGTextureDesc DebugDesc = FRDGTextureDesc::Create2D(TextureSize, DebugFormat, FClearValueBinding::Black, TexCreate_RenderTargetable | TexCreate_ShaderResource);
				FRDGTextureRef TmpOutputTexture = GraphBuilder.CreateTexture(OutputDesc, TEXT("DrawInstance_Output"));
				FRDGTextureRef TmpDepthTexture = GraphBuilder.CreateTexture(DepthPreviewDesc, TEXT("DrawInstance_Depth"));
				FRDGTextureRef TmpDebugTexture = GraphBuilder.CreateTexture(DebugDesc, TEXT("DrawInstance_Debug"));
				FRDGTextureDesc RasterDepthDesc = FRDGTextureDesc::Create2D(
					TextureSize,
					PF_DepthStencil,
					FClearValueBinding::DepthFar,
					TexCreate_DepthStencilTargetable | TexCreate_ShaderResource);
				FRDGTextureRef RasterDepthTexture = GraphBuilder.CreateTexture(RasterDepthDesc, TEXT("DrawInstance_RasterDepth"));

				FRDGTextureRef OutputTexture = RegisterExternalTexture(GraphBuilder, R_Output->GetRenderTargetTexture(), TEXT("DrawInstance_OutputRT"));
				FRDGTextureRef DepthTexture = RegisterExternalTexture(GraphBuilder, R_Depth->GetRenderTargetTexture(), TEXT("DrawInstance_DepthRT"));
				AddClearRenderTargetPass(GraphBuilder, TmpOutputTexture, FLinearColor::Black);
				AddClearRenderTargetPass(GraphBuilder, TmpDepthTexture, FLinearColor(MaxDepth, MaxDepth, MaxDepth, 1.0f));
				AddClearRenderTargetPass(GraphBuilder, TmpDebugTexture, FLinearColor::Black);

				FDrawInstancesRasterPassParameters* PassParameters = GraphBuilder.AllocParameters<FDrawInstancesRasterPassParameters>();
				PassParameters->VS.InvHalfCaptureWidth = 1.0f / FMath::Max(HalfCaptureWidth, KINDA_SMALL_NUMBER);
				PassParameters->VS.InvHalfCaptureHeight = 1.0f / FMath::Max(HalfCaptureHeight, KINDA_SMALL_NUMBER);
				PassParameters->VS.RasterMinDepth = MinVisibleDepth;
				PassParameters->VS.RasterInvDepthRange = 1.0f / FMath::Max(DepthRange, KINDA_SMALL_NUMBER);
				PassParameters->RenderTargets[0] = FRenderTargetBinding(TmpOutputTexture, ERenderTargetLoadAction::ELoad);
				PassParameters->RenderTargets[1] = FRenderTargetBinding(TmpDepthTexture, ERenderTargetLoadAction::ELoad);
				PassParameters->RenderTargets[2] = FRenderTargetBinding(TmpDebugTexture, ERenderTargetLoadAction::ELoad);
				PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
					RasterDepthTexture,
					ERenderTargetLoadAction::EClear,
					ERenderTargetLoadAction::ENoAction,
					FExclusiveDepthStencil::DepthWrite_StencilNop);

				TShaderMapRef<FDrawInstancesRasterVS> VertexShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				TShaderMapRef<FDrawInstancesRasterPS> PixelShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

				GraphBuilder.AddPass(
					RDG_EVENT_NAME("DrawInstancesRaster"),
					PassParameters,
					ERDGPassFlags::Raster,
					[PassParameters, VertexShader, PixelShader, VertexBufferRHI, TextureSize, PrimitiveCount = RasterVertices.Num() / 3](FRHICommandList& RHICmdList)
					{
						FGraphicsPipelineStateInitializer GraphicsPSOInit;
						RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
						GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
						GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
						GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<true, CF_LessEqual>::GetRHI();
						GraphicsPSOInit.PrimitiveType = PT_TriangleList;
						GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = DrawRasterVertexDataResource.VertexDeclaration;
						GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
						GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

						SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
						SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), PassParameters->VS);
						SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PassParameters->PS);

						RHICmdList.SetViewport(0.0f, 0.0f, 0.0f, (float)TextureSize.X, (float)TextureSize.Y, 1.0f);
						RHICmdList.SetStreamSource(0, VertexBufferRHI, 0);
						RHICmdList.DrawPrimitive(0, PrimitiveCount, 1);
					});

				AddCopyTexturePass(GraphBuilder, TmpOutputTexture, OutputTexture, FRHICopyTextureInfo());
				AddCopyTexturePass(GraphBuilder, TmpDepthTexture, DepthTexture, FRHICopyTextureInfo());
				if (R_Debug != nullptr)
				{
					FRDGTextureRef DebugTexture = RegisterExternalTexture(GraphBuilder, R_Debug->GetRenderTargetTexture(), TEXT("DrawInstance_DebugRT"));
					AddCopyTexturePass(GraphBuilder, TmpDebugTexture, DebugTexture, FRHICopyTextureInfo());
				}
			}
			GraphBuilder.Execute();
		});

	FlushRenderingCommands();
}


