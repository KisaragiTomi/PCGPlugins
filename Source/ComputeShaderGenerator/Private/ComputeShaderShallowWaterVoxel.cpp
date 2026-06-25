#include "ComputeShaderShallowWaterVoxel.h"

ACSShallowWaterVoxelCapture::ACSShallowWaterVoxelCapture(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void ACSShallowWaterVoxelCapture::Destroyed()
{
	ReleaseTerrainVoxelGrid();
	Super::Destroyed();
}

void ACSShallowWaterVoxelCapture::BeginDestroy()
{
	ReleaseTerrainVoxelGrid();
	Super::BeginDestroy();
}

void ACSShallowWaterVoxelCapture::BuildTerrainVoxelGrid()
{
	UE_LOG(LogTemp, Warning, TEXT("[CSSW Voxel] BuildTerrainVoxelGrid: Not yet implemented"));
}

void ACSShallowWaterVoxelCapture::ReleaseTerrainVoxelGrid()
{
	VoxelColumnRunStartBuffer.SafeRelease();
	VoxelColumnRunCountBuffer.SafeRelease();
	VoxelRunsBuffer.SafeRelease();
	VoxelCoverageBuffer.SafeRelease();
	VoxelTotalRunCount = 0;
	VoxelGridSize = FIntVector::ZeroValue;
	bVoxelGridValid = false;
}

void ACSShallowWaterVoxelCapture::VisualizeVoxelRuns(float Duration)
{
	UE_LOG(LogTemp, Warning, TEXT("[CSSW Voxel] VisualizeVoxelRuns: Not yet implemented"));
}

void ACSShallowWaterVoxelCapture::DrawDebugVoxelGrid(float Duration, float Thickness)
{
	UE_LOG(LogTemp, Warning, TEXT("[CSSW Voxel] DrawDebugVoxelGrid: Not yet implemented"));
}
