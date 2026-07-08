#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "NKSRMeshData.h"
#include "NKSRReconstruct.generated.h"

class UDynamicMesh;

/**
 * NKSR point-cloud surface reconstruction — pure C++ implementation (no Python, no external deps).
 * Network weights ship with the plugin (Resources/nksr_ks.nkw).
 */
USTRUCT(BlueprintType)
struct FNKSRSettings
{
	GENERATED_BODY()

	/** 0..1. Higher = more detail, slower. Ignored when VoxelSize > 0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NKSR", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DetailLevel = 1.0f;

	/** > 0: force the working voxel size (input units) instead of DetailLevel-based density scaling. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NKSR", meta = (ClampMin = "0.0"))
	float VoxelSize = 0.0f;

	/** Iterations of iso-surface refinement (higher = denser mesh). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NKSR", meta = (ClampMin = "0", ClampMax = "3"))
	int32 MiseIter = 1;

	/** Estimate normals (PCA + consistent orientation) when the input has none. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NKSR")
	bool bEstimateNormalsIfMissing = true;

	/** k-nearest-neighbor count for normal estimation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NKSR", meta = (ClampMin = "3", ClampMax = "128"))
	int32 NormalKnn = 30;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NKSR|Advanced", meta = (ClampMin = "1"))
	int32 SolverMaxIter = 2000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NKSR|Advanced")
	float SolverTol = 1e-5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NKSR|Advanced")
	bool bVerboseLog = false;
};

USTRUCT(BlueprintType)
struct FNKSRResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "NKSR")
	bool bSuccess = false;

	UPROPERTY(BlueprintReadOnly, Category = "NKSR")
	FString OutputFilePath;

	UPROPERTY(BlueprintReadOnly, Category = "NKSR")
	int32 VertexCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "NKSR")
	int32 FaceCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "NKSR")
	FString ErrorMessage;

	UPROPERTY(BlueprintReadOnly, Category = "NKSR")
	float ElapsedSeconds = 0.f;
};

UCLASS()
class AITOOLMODULE_API UNKSRReconstructLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * File -> file reconstruction (blocks the calling thread; prefer UNKSRReconstructAsync in game code).
	 * Input: PLY / OBJ / XYZ / CSV / NPY. Output: OBJ (empty path = "<input>_nksr.obj").
	 */
	UFUNCTION(BlueprintCallable, Category = "NKSR")
	static FNKSRResult RunNKSRReconstruction(
		const FString& InputFilePath,
		const FString& OutputFilePath,
		const FNKSRSettings& Settings);

	/** Array -> mesh reconstruction. Normals may be empty when Settings.bEstimateNormalsIfMissing. */
	UFUNCTION(BlueprintCallable, Category = "NKSR")
	static bool ReconstructPointCloud(
		const TArray<FVector>& Points,
		const TArray<FVector>& Normals,
		const FNKSRSettings& Settings,
		FNKSRMeshData& OutMesh,
		FString& OutError);

	/** Copies reconstruction output into a UDynamicMesh (e.g. a DynamicMeshActor's mesh). */
	UFUNCTION(BlueprintCallable, Category = "NKSR")
	static bool MeshDataToDynamicMesh(const FNKSRMeshData& MeshData, UDynamicMesh* TargetMesh);

	/** Absolute AITool plugin root directory. */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "NKSR")
	static FString GetAIToolPluginDir();
};
