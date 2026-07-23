#pragma once

#include "ComputeShaderShallowWater.h"
#include "ComputeShaderShallowWaterVoxel.generated.h"

UCLASS(HideCategories=(Replication), meta=(PrioritizeCategories="SWParameter"))
class COMPUTESHADERGENERATOR_API ACSShallowWaterVoxelCapture : public ACSShallowWaterCapture
{
	GENERATED_BODY()
public:
	ACSShallowWaterVoxelCapture(const FObjectInitializer& ObjectInitializer);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter|Voxel", Meta=(Priority=1000))
	int32 VoxelMaxSceneTriangles = 2000000000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter|Voxel", Meta=(Priority=1000, ClampMin="1", ClampMax="256"))
	int32 VoxelMaxRunsPerColumn = 16;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter|Voxel", Meta=(Priority=1000, ClampMin="1", ClampMax="2048"))
	int32 VoxelTileBudgetMB = 64;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter|Voxel", Meta=(Priority=1000))
	float VoxelMaxAboveWaterSurface = 50.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter|Voxel", Meta=(Priority=1000))
	float VoxelMaxBelowWaterSurface = 500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter|Voxel", Meta=(Priority=1000))
	float VoxelMaxAboveSourceZ = 0.0f;

	TRefCountPtr<FRDGPooledBuffer> VoxelColumnRunStartBuffer;
	TRefCountPtr<FRDGPooledBuffer> VoxelColumnRunCountBuffer;
	TRefCountPtr<FRDGPooledBuffer> VoxelRunsBuffer;
	TRefCountPtr<FRDGPooledBuffer> VoxelCoverageBuffer;
	uint32 VoxelTotalRunCount = 0;
	FIntVector VoxelGridSize = FIntVector::ZeroValue;
	float VoxelBoxExtentXY = 0.0f;
	float VoxelCellSizeXY = 0.0f;
	float VoxelGridMinWorldZ = 0.0f;
	float VoxelCellSizeZ = 0.0f;
	bool bVoxelGridValid = false;
	bool bVoxelHeightMapInitialized = false;

	void BuildTerrainVoxelGrid();
	void ReleaseTerrainVoxelGrid();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "ComputeShader|Debug", Meta=(ClampMin="0", UIMin="0"))
	void VisualizeVoxelRuns(float Duration = 5.0f);

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "ComputeShader|Debug", Meta=(ClampMin="0", UIMin="0"))
	void DrawDebugVoxelGrid(float Duration = 10.0f, float Thickness = -1.0f);

	virtual void Destroyed() override;
	virtual void BeginDestroy() override;
};
