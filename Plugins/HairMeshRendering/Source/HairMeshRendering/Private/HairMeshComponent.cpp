#include "HairMeshComponent.h"
#include "HairMeshAsset.h"
#include "HairMeshSceneProxy.h"

UHairMeshComponent::UHairMeshComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	bUseAsOccluder = false;
}

void UHairMeshComponent::OnRegister()
{
	Super::OnRegister();
	RebuildTextures();
}

void UHairMeshComponent::OnUnregister()
{
	Super::OnUnregister();
}

#if WITH_EDITOR
void UHairMeshComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	RebuildTextures();
}
#endif

void UHairMeshComponent::RebuildTextures()
{
	TextureSet = FHairMeshTextureSet();

	if (HairMeshAsset && HairMeshAsset->Bundles.Num() > 0)
	{
		TextureSet = FHairMeshTextureBuilder::Build(HairMeshAsset);
	}

	MarkRenderStateDirty();
}

FPrimitiveSceneProxy* UHairMeshComponent::CreateSceneProxy()
{
	if (!TextureSet.IsValid())
	{
		return nullptr;
	}
	return new FHairMeshSceneProxy(this);
}

FBoxSphereBounds UHairMeshComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	if (!HairMeshAsset || HairMeshAsset->Vertices.Num() == 0)
	{
		return FBoxSphereBounds(ForceInit);
	}

	FBox BoundingBox(ForceInit);
	for (const FHairMeshVertex& V : HairMeshAsset->Vertices)
	{
		BoundingBox += V.Position;
	}
	return FBoxSphereBounds(BoundingBox).TransformBy(LocalToWorld);
}
