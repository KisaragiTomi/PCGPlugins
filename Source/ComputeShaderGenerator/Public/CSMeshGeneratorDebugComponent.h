#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "RenderGraphResources.h"
#include "CSMeshGeneratorDebugComponent.generated.h"

struct FCSGpuDebugPooledSource;
struct FCSSurfaceVoxelGPUBuffers;

enum class ECSMeshGeneratorDebugMode : uint8
{
	Directions,
	IsolatedQuads,
};

/** Render-thread-safe snapshot of a surface-voxel debug request. */
struct FCSMeshGeneratorDebugData
{
	TRefCountPtr<FRDGPooledBuffer> Positions;
	TRefCountPtr<FRDGPooledBuffer> Normals;
	TRefCountPtr<FRDGPooledBuffer> Counter;
	int32 VoxelCapacity = 0;
	int32 MaxVoxelsToDraw = 0;
	float VoxelSize = 0.0f;
	float DirectionLength = 0.0f;
	float QuadScale = 1.0f;
	float NormalOffsetScale = 0.0f;
	FLinearColor DirectionColor = FLinearColor::Blue;
	FLinearColor PointColor = FLinearColor::Yellow;
	FBox WorldBounds = FBox(ForceInit);
	ECSMeshGeneratorDebugMode Mode = ECSMeshGeneratorDebugMode::Directions;
	bool bDrawPoints = true;
	bool bReverseOrientation = false;

	bool IsValid() const
	{
		return Positions.IsValid() && Normals.IsValid() && Counter.IsValid() && VoxelCapacity > 0;
	}
};

/**
 * Draws surface-voxel diagnostics directly from retained GPU buffers. The proxy reads
 * the valid voxel count from Counter[0], generates render geometry with a compute shader,
 * and issues indirect draws. No GPU resource is mapped or copied to CPU memory.
 */
UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
class COMPUTESHADERGENERATOR_API UCSMeshGeneratorDebugComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UCSMeshGeneratorDebugComponent();

	/** Submits GPU normal lines and optional point primitives from any retained pooled source.
	 *  Returns the submitted capacity. Pass a tiny DirectionLength for a points-only visual. */
	int32 SetDirectionSource(
		const FCSGpuDebugPooledSource& Source,
		float DirectionLength,
		FLinearColor DirectionColor,
		bool bDrawPoints,
		FLinearColor PointColor,
		int32 MaxDirectionsToDraw,
		float Duration,
		bool bPersistent);

	/** Surface-voxel entry point; forwards to the pooled-source overload above. */
	int32 SetDirectionSource(
		const FCSSurfaceVoxelGPUBuffers& Source,
		float DirectionLength,
		FLinearColor DirectionColor,
		bool bDrawPoints,
		FLinearColor PointColor,
		int32 MaxDirectionsToDraw,
		float Duration,
		bool bPersistent);

	/** Submits one GPU-generated isolated quad per valid surface voxel. */
	bool SetIsolatedQuadSource(
		const FCSSurfaceVoxelGPUBuffers& Source,
		float QuadScale,
		float NormalOffsetScale,
		bool bReverseOrientation);

	/** Releases the retained voxel buffers and removes the debug proxy. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Debug")
	void ClearDebug();

	//~ UPrimitiveComponent interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;

private:
	void SubmitData(FCSMeshGeneratorDebugData&& InData);
	void ScheduleClear(float Duration, bool bPersistent);

	FCSMeshGeneratorDebugData PendingData;
	FTimerHandle ClearTimerHandle;
};
