#pragma once

#include "CoreMinimal.h"

class UStaticMesh;

struct COMPUTESHADERGENERATOR_API FStaticMeshRenderDataPointSampleRequest
{
	UStaticMesh* StaticMesh = nullptr;
	int32 LODIndex = 0;
	FTransform LocalToWorld = FTransform::Identity;
	FBox WorldBounds = FBox(ForceInit);
	int32 MaxPoints = 0;
	float VoxelCellSize = 1.0f;
};

class COMPUTESHADERGENERATOR_API FStaticMeshRenderDataPointSampler
{
public:
	static bool SamplePointsSync(const TArray<FStaticMeshRenderDataPointSampleRequest>& Requests, TArray<FVector>& OutPoints);
	static bool SamplePointsSync(const TArray<FStaticMeshRenderDataPointSampleRequest>& Requests, TArray<FVector>& OutPoints, TArray<int32>& OutPointsPerRequest);
};
