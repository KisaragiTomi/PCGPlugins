#include "HairMeshAsset.h"

#if WITH_EDITOR
void UHairMeshAsset::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Clamp MaxVerticesPerStrand to nearest 2^e + 1
	if (PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UHairMeshAsset, MaxVerticesPerStrand))
	{
		int32 e = FMath::FloorLog2(FMath::Max(MaxVerticesPerStrand - 1, 1));
		MaxVerticesPerStrand = (1 << e) + 1;
	}
}
#endif
