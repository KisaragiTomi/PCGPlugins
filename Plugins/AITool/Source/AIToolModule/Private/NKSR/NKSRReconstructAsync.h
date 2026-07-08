#pragma once

// Background-thread reconstruction as a Blueprint async node.
// Work runs on a background task; delegates fire on the game thread. Cancel() is checked
// at stage boundaries (see FNKSRProgressSink), so cancellation is prompt but not instant.

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "NKSRReconstruct.h"
#include "NKSRReconstructAsync.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FNKSRReconstructAsyncEvent, const FNKSRResult&, Result, const FNKSRMeshData&, Mesh);

UCLASS()
class UNKSRReconstructAsync : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** File -> OBJ file, off the game thread. */
	UFUNCTION(BlueprintCallable, Category = "NKSR", meta = (BlueprintInternalUseOnly = "true"))
	static UNKSRReconstructAsync* ReconstructFileAsync(const FString& InputFilePath, const FString& OutputFilePath, FNKSRSettings Settings);

	/** Point arrays -> mesh data, off the game thread. */
	UFUNCTION(BlueprintCallable, Category = "NKSR", meta = (BlueprintInternalUseOnly = "true"))
	static UNKSRReconstructAsync* ReconstructPointsAsync(const TArray<FVector>& Points, const TArray<FVector>& Normals, FNKSRSettings Settings);

	UFUNCTION(BlueprintCallable, Category = "NKSR")
	void Cancel();

	virtual void Activate() override;

	UPROPERTY(BlueprintAssignable)
	FNKSRReconstructAsyncEvent OnCompleted;

	UPROPERTY(BlueprintAssignable)
	FNKSRReconstructAsyncEvent OnFailed;

private:
	FString InputPath;
	FString OutputPath;
	FNKSRSettings RunSettings;
	TArray<FVector> InPoints;
	TArray<FVector> InNormals;
	bool bFromFile = false;
	FThreadSafeBool bCancelRequested = false;
};
