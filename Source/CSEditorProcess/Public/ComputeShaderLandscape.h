#pragma once
//
#include "CoreMinimal.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ComputeShaderMeshFill.h"
#include "ComputeShaderSceneCapture.h"
#include "ComputeShaderShallowWater.h"
#include "LandscapeBlueprintBrush.h"
#include "LandscapeExtra.h"
//
#include "ComputeShaderLandscape.generated.h"
//
//
// //This struct act as a container for all the parameters that the client needs to pass to the Compute Shader Manager.

class USplineComponent;

UCLASS()
class CSEDITORPROCESS_API ACSLandscape : public ALandscapeBlueprintBrush
{
GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LandscapeData")
	UTextureRenderTarget2D* RT_DebugView;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LandscapeData")
	UTextureRenderTarget2D* RT_Result;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LandscapeData")
	UTextureRenderTarget2D* RT_LandscapeData;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LandscapeData")
	UTextureRenderTarget2D* RT_CopyLandscapeData;


	USceneComponent* SceneComponent;
	UPROPERTY(BlueprintReadWrite, Category = "LandscapeData")
	UBoxComponent* Box;

	FReadLandscapeData Orig_LandscapeData;

	FReadLandscapeData Copy_LandscapeData;

	FVector BoxMin = FVector::ZeroVector;
	FVector BoxMax = FVector::ZeroVector;

	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	FVector LandscapeTexMinUV = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	FVector LandscapeTexUVRange = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	FVector MapMin = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	FVector MapMax = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LandscapeData")
	float BlurRange = .25;

	UPROPERTY( BlueprintReadWrite, Category = "LandscapeData")
	UDynamicMeshComponent* VisMesh;
	ACSLandscape();

	virtual void OnConstruction(const FTransform& Transform) override;

	virtual void InitRT();
	
	virtual bool IsParameterValidMult()
	{
		bool Check = true;
		if (RT_LandscapeData == nullptr) Check = false;
		if (RT_DebugView == nullptr) Check = false;
		if (RT_Result == nullptr) Check = false;
		return Check;
	}
	
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void ReadLandscapeDataToTexture();


	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void CopyLandscapeData();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void PasteLandscapeData();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void BP_InitRT();

};


UCLASS()
class CSEDITORPROCESS_API ACSLandscapeRiver : public ACSLandscape
{
GENERATED_BODY()
public:

	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "LandscapeData")
	USplineComponent* SplineComponent;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LandscapeData")
	UTextureRenderTarget2D* RT_SplineRotateDist;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LandscapeData")
	UTextureRenderTarget2D* RT_SplineGradientHeight;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LandscapeData", Meta=(Priority=1000))
	float TargetRiverWidth = 200;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LandscapeData", Meta=(Priority=1000))
	float RiverDepth = 200;
	
	ACSLandscapeRiver();

	virtual void OnConstruction(const FTransform& Transform) override;

	virtual void InitRT() override; 

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void ProjectLineToLandscape();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void RecenterSpline();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void GenerateRiverBed();
	
	void BoxMatchSpline();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void SimRiver(TSubclassOf<AActor> ActorClass, int32 SimIteration, FVector SourcePoint, float Size = .05);


};


