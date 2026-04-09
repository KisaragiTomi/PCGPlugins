#pragma once
//
#include "CoreMinimal.h"
#include "ComputeShaderGenerateHepler.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/BoxComponent.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Components/DecalComponent.h"
#include "RenderGraphBuilder.h"

//
#include "ComputeShaderShallowWater.generated.h"

UENUM(BlueprintType)
enum class EWaterfallExpansion : uint8
{
	Expansion_5  = 0 UMETA(DisplayName = "5"),
	Expansion_7  = 1 UMETA(DisplayName = "7"),
	Expansion_10 = 2 UMETA(DisplayName = "10"),
};



UCLASS()
class COMPUTESHADERGENERATOR_API ACSShallowWaterCapture : public AActor
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_DebugView;
	UPROPERTY(BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_VelocityHeight;
	UPROPERTY(BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_ResultVelHeight;
	UPROPERTY(BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_ResultDepthWet;
	UPROPERTY(BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_Source;
	UPROPERTY(BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_SceneDepth;
	UPROPERTY(BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_SmoothHeight;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", Meta=(Priority=1000))
	int32 SWUniqueID = -99999;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", Meta=(Priority=1000))
	bool CleanDepthWet = true;


	USceneComponent* SceneComponent;
	UBoxComponent* Box;
	UPROPERTY(BlueprintReadWrite, Category = "Debug")
	USceneCaptureComponent2D* CaptureSceneDepth;
	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "Debug")
	UStaticMeshComponent* VisualizeMesh;
	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "Debug")
	UDecalComponent* CausticsDecal;

	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	UMaterialInterface* WaterMaterial;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	UMaterialInterface* DecalMaterial;

	UPROPERTY(BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	UMaterialInterface* VisWaterMaterial;
	UPROPERTY(BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	UMaterialInterface* VisDecalMaterial;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	bool CloseBound = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	int32 Iteration = 1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	int32 HeightSmoothIteration = 1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	EWaterfallExpansion WaterfallExpansionIterations = EWaterfallExpansion::Expansion_5;
	UPROPERTY(BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	float DT = .1;
	UPROPERTY(BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	float Friction = 0.005;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	float AdvectFoam = 1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	float FoamFadeSpeed = 0.001;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	float WorldPixelSize = 40;
	UPROPERTY(BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	float SeaLevel = -1000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	FName SWCaptureTag = FName("CSSW");

	UPROPERTY(BlueprintReadWrite, Category = "Capturer")
	FName SWTag = FName("CSSW_Bake");

	UPROPERTY(BlueprintReadWrite, Category = "Capturer", Meta=(Priority=1000))
	float MaxHeight = 10000;
	UPROPERTY(BlueprintReadWrite, Category = "Capturer", Meta=(Priority=1000))
	float Scale3DZ = 100;
	// UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capturer")
	float TextureSize = 256;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capturer", Meta=(Priority=1000))
	float CaptureSize = 2000;


	

	ACSShallowWaterCapture();

	virtual void OnConstruction(const FTransform& Transform) override;
	
	bool IsParameterValidMult_SourceTexture()
	{
		// TextureSize = CSHepler::GenerateTextureSize(TextureSize);
		bool Check = true;
		if (RT_SceneDepth == nullptr) RT_SceneDepth = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA32f, FLinearColor::Black, true, false);
		CaptureSceneDepth->TextureTarget = RT_SceneDepth;
		if (RT_DebugView == nullptr) RT_DebugView = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
		if (RT_VelocityHeight == nullptr) RT_VelocityHeight = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA32f, FLinearColor::Black, true, false);
		if (RT_ResultVelHeight == nullptr) RT_ResultVelHeight = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
		if (RT_Source == nullptr) RT_Source = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
		if (RT_ResultDepthWet == nullptr) RT_ResultDepthWet = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
		SetMaterialParameter();
		return Check;
	}
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	bool CheckAndCreateTexture_SWSourcePoint()
	{
		// TextureSize = CSHepler::GenerateTextureSize(TextureSize);
		bool Check = true;
		if (RT_SceneDepth == nullptr) RT_SceneDepth = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA32f, FLinearColor::Black, true, false);
		CaptureSceneDepth->TextureTarget = RT_SceneDepth;
		if (RT_DebugView == nullptr) RT_DebugView = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
		if (RT_VelocityHeight == nullptr) RT_VelocityHeight = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA32f, FLinearColor(0, 0, -9999, 1), true, false);
		if (RT_ResultVelHeight == nullptr) RT_ResultVelHeight = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor(0, 0, -9999, 1), true, false);
		if (RT_Source == nullptr) RT_Source = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor(MaxHeight, 0, 0, 0), true, false);
		if (RT_SmoothHeight == nullptr) RT_SmoothHeight = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA32f,FLinearColor(-9999, -9999, -9999, -9999), true, false);
		
		if (RT_ResultDepthWet == nullptr)
		{
			RT_ResultDepthWet = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor(-9999, -9999, -9999, -9999), true, false);
			UKismetRenderingLibrary::ClearRenderTarget2D(this, RT_ResultDepthWet,  FLinearColor(-9999, -9999, -9999, -9999));
			CleanDepthWet = true;
		}
		else
		{
			if (CleanDepthWet)
			{
				UKismetRenderingLibrary::ClearRenderTarget2D(this, RT_ResultDepthWet,  FLinearColor(-9999, -9999, -9999, -9999));
				CleanDepthWet = false;
			}
		}
		SetMaterialParameter();
		return Check;
	}
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void ConstructionComponent();
	
	UFUNCTION(BlueprintPure, Category = "SWParameter")
	int32 GetWaterfallExpansionCount() const
	{
		constexpr int32 LUT[] = { 5, 7, 10 };
		const int32 Idx = FMath::Clamp(static_cast<int32>(WaterfallExpansionIterations), 0, 2);
		return LUT[Idx];
	}

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void ShallowWaterSolverSoucePoint(int32 InIteration);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void ShallowWaterSolverSplineRange(UTextureRenderTarget2D* RT_SplineScaleDist, UTextureRenderTarget2D* RT_CopyLandscape, FVector SourceLocation, FVector2f ValidUV, int32 TarIteration = 1, float SourceSize = .05);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void ShallowWaterSolverSouceTexture();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void SetHeight();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void HeightSmooth();
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void Clean();
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void CleanDepthWet_Construct();
	
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	TArray<FVector4> GetSources();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void CleanupAttachedActors();
	
	UFUNCTION(BlueprintNativeEvent)
	void SetMaterialParameter();
	virtual void SetMaterialParameter_Implementation();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capturer", Meta=(Priority=1000))
	bool bAutoCapture = false;

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void CaptureSceneDepthNow();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|RenderDoc", Meta=(Priority=999))
	bool bCaptureNextSolverFrame = false;

	UFUNCTION(BlueprintCallable, Category = "Debug|RenderDoc")
	void RequestRenderDocCapture();

	UFUNCTION(BlueprintCallable, Category = "Debug|RenderDoc")
	void ShallowWaterSolverSoucePointWithCapture(int32 InIteration);
};




UCLASS()
class COMPUTESHADERGENERATOR_API ACSSHallowWaterSource : public AActor
{
	GENERATED_BODY()
public:
	ACSSHallowWaterSource();
	
};

UCLASS()
class COMPUTESHADERGENERATOR_API ACSSHallowWaterContainer : public AActor
{
	GENERATED_BODY()
public:
	ACSSHallowWaterContainer();

	USceneComponent * SceneComponent;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capturer")
	UStaticMeshComponent* VisualizeMesh;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capturer")
	UDecalComponent* CausticsDecal;
};