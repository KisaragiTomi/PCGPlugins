#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "NKSRReconstruct.generated.h"

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
};

USTRUCT(BlueprintType)
struct FNKSRSettings
{
	GENERATED_BODY()

	/** Leave empty to auto-detect a local Python runtime, then fallback to bundled Python if present. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NKSR",
		meta = (ToolTip = "Python executable path. Empty = auto-detect local Python first, then bundled Python."))
	FString PythonExePath;

	/** Leave empty to auto-detect an external NKSR package workspace (preferred) or a bundled package. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NKSR",
		meta = (ToolTip = "NKSR package path. Should contain nksr/__init__.py and nksr/_C.pyd. Empty = auto-detect external or bundled package."))
	FString NKSRPackagePath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NKSR", meta = (ClampMin = "0.1", ClampMax = "5.0"))
	float DetailLevel = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NKSR")
	FString Device = TEXT("cuda");
};

UCLASS()
class AITOOLMODULE_API UNKSRReconstructLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Run NKSR point-cloud-to-mesh reconstruction.
	 * Blocks the calling thread until Python finishes.
	 * Supports PLY / OBJ / FBX / NPY / XYZ input formats.
	 *
	 * @param InputFilePath  Absolute path to the input point cloud file.
	 * @param OutputFilePath Absolute path for the output OBJ. Empty = auto-generate next to input.
	 * @param Settings       Python/NKSR paths (empty = bundled), detail level, device.
	 * @return               Result struct with success flag, vertex/face counts, and output path.
	 */
	UFUNCTION(BlueprintCallable, Category = "NKSR")
	static FNKSRResult RunNKSRReconstruction(
		const FString& InputFilePath,
		const FString& OutputFilePath,
		const FNKSRSettings& Settings);

	/** Get absolute path to the bundled reconstruct.py script. */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "NKSR")
	static FString GetNKSRScriptPath();

	/** Get absolute path to the bundled Python executable (ThirdParty/Python/python.exe). */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "NKSR")
	static FString GetBundledPythonPath();

	/** Resolve the preferred Python executable for NKSR on the current machine. */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "NKSR")
	static FString GetDefaultPythonPath();

	/** Resolve the preferred external or bundled NKSR package path for the current machine. */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "NKSR")
	static FString GetDefaultNKSRPackagePath();

	/** Get the AITool plugin root directory. */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "NKSR")
	static FString GetAIToolPluginDir();
};
