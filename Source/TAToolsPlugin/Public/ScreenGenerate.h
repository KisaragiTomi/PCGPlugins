// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMathLibrary.h"
#include "FoliageType_InstancedStaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "VectorTypes.h" 
#include "DynamicMeshToMeshDescription.h"
#include "Delegates/DelegateCombinations.h"
#include "Engine/SceneCapture2D.h"
#include "PolygonProcess.h"
#include "TATools_Async.h"
#include "Async/Async.h"
//#include "Public/UObject/ConstructorHelpers.h"


#if WITH_OPENCV
#include "opencv2/core.hpp"
#endif

#include "ScreenGenerate.generated.h"

//#ifdef _DEBUG
//#pragma comment(lib, "opencv_world451d.lib")
//#else
//#pragma comment(lib, "opencv_world451.lib")
//#endif

/**
 * 
 */


class ASceneCaptureContainter;
class AStaticMeshActor;
class AsyncAble;
DECLARE_MULTICAST_DELEGATE(FCalDelegate);

//DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLiveLinkTickDelegate, float, DeltaTime);


using namespace std;
using namespace cv;
using namespace UE::Geometry;


UCLASS()
class TATOOLSPLUGIN_API UScreenGenerate : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = ScreenGenerate)
	static bool TestFunction();


	UFUNCTION(BlueprintCallable, Category = ScreenGenerate)
	static bool BoxAreaFoliageGenerate(ASceneCapture2D* SceneCapture, TArray<AActor*> PickActors, TArray<UStaticMesh*> InputMeshs, float BlockGenerate, float DivideScale = 1000);
	
	UFUNCTION(BlueprintCallable, Category = ScreenGenerate)
	static bool BoxAreaOpenAssetGenerate(ASceneCapture2D* SceneCapture, TArray<AActor*> PickActors, TArray<UStaticMesh*> InputMeshs, float BlockGenerate, float DivideScale = 5000);


	UFUNCTION(BlueprintCallable, Category = ScreenGenerate)
	static bool BoxAreaLineFoliageGenerate(ASceneCapture2D* SceneCapture, TArray<AActor*> PickActors, TArray<UStaticMesh*> InputMeshs, FVector BoxCenter, FVector
	                                       BoxExtent, float BlockGenerate, float DivideScale = 2000, bool DebugImg = false);


	//UFUNCTION(BlueprintCallable, Category = ScreenGenerate)
	//static bool FoliageGenerateLineTraceClass(UTextureRenderTarget2D* Texture2D, ASceneCapture2D* SceneCapture, TArray<AActor*> PickActors, TArray<UStaticMesh*> InputMeshs, TArray<FLinearColor>& OutHDRValues, TArray<AStaticMeshActor*>& OutActors, UTextureRenderTarget2D*& OutRT);



	UFUNCTION(BlueprintCallable, Category = ScreenGenerate)
	static bool ProjectWorldToScreen_General(ASceneCapture2D* InSceneCapture, int32 Width, int32 Height, FVector WorldPos, FVector2D& ScreenPos, float& ScreenDepth);

	UFUNCTION(BlueprintCallable, Category = ScreenGenerate)
	static bool DeProjectScreenToWorld_General(ASceneCapture2D* InSceneCapture, int32 Width, int32 Height, FVector2D ScreenPos, float ScreenDepth, FVector& WorldPos, FVector& WorldDir);
};


class TATOOLSPLUGIN_API ScreenProcess
{
public:
	ScreenProcess(){}
	
	UWorld* World = GWorld;
	ASceneCapture2D* SceneCapture;
	TArray<AActor*> PickActors;
	TArray<UStaticMesh*> Meshs;
	FVector BoxExtent;
	FVector Center;
	ASceneCaptureContainter* SceneCaptureContainter = nullptr;
	int32 SceneCaptureCount;

	FCalDelegate FCalDelegate;
	struct SceneDataStruct
	{
		FLinearColor Color = FLinearColor(0, 0, 0, 0);
		bool Unchecked = true;
		bool Picking = false;
		AActor* TraceActor = nullptr;
		FVector Normal = FVector::ZeroVector;
		FVector WorldPos = FVector::ZeroVector;
		int32 Item = -1;
	};

	struct OtherSideStruct
	{
		TArray<FVector2i> OtherSidePixels;
		FVector Normal;
	};
	UStaticMesh* CurrenStaticMesh;
	Mat OutReferenceImg;//这个图在DrawTexture中会被输出 用于debug
	Mat OutReferenceImg2;//这个图在DrawTexture中会被输出 用于debug备用
	TArray<FVector2i> OutReferenceTArray;
	TMap<UStaticMesh*, TSharedPtr<FDynamicMesh3>> DynamicMeshData;
	TMap<UStaticMesh*, TArray<FVector>> MeshOpenVertexMap;
	TMap<FVector2i, float> SortIdx;
	TArray<TArray<SceneDataStruct>> SceneData;

	TArray<OtherSideStruct> OtherSides;
	TArray<FVector2i> PickPixelArray;
	TArray<FVector2i> DepthArray;
	
	FTransform CurrentTransform;
	FVector Forward, DirRight, DirUp, Root;
	FVector ObjectScale = FVector::OneVector;
	FVector2D ErodeBoundMax;
	FVector2D ErodeBoundMin;
	FVector2D ErodeBoundPixels;
	int32 RTSize;
	int32 Height = 0;
	int32 Width = 0;
	int32 ErodeNumPixels = 0;
	int32 CamIndex = -1;
	FTransform ObjectTransform;
	float PixelSize = 0;
	float RandomScale = 0;
	float Block = 0;
	float CaptureSize = 1000;
	float DivdeSize = 1000;
	bool DebugImg = false;

	virtual bool ProcessImg() = 0;
	void DoCalculate()
	{
		FCalDelegate.Broadcast();
	}
	
	bool Setup();

	void CacheImg();
	
	bool CheckNormalImg(FVector2i SearchIndex, float NormalThreshold);

	void CheckCollision(AStaticMeshActor* StaticActor);

	FVector2i PixelBox(FVector2i& Max, FVector2i& Min);

	FTransform CreateObjectTransform(FVector2i PickPixel, int32 Type);

	TArray<FVector2i> ScreenErode();

	bool OverlapImgCheck(Mat UnLayoutImg, Mat ObjectImg, float OverlapThreashold = 0);

	bool OverlapCheck(Mat Img, FVector2i PickPixel);

	TArray<FVector2i> MatBinarizationToVector2D(Mat Img);

	Mat Array2DToMatBinarization(TArray<FVector2i> ConvertTArray);

	bool RasterizeMesh(UStaticMesh* InMesh, FTransform& Transform, Mat& OutImg);

	void CalculateStaticMesh();

	void SpawnStaticMesh();

	void LineDetection();

	void DrawDebugTexture(Mat Img, int32 index);

};

class TATOOLSPLUGIN_API ProcessFoliage : public ScreenProcess
{
public:
	virtual bool ProcessImg() override;
	virtual ~ProcessFoliage(){};
};

class TATOOLSPLUGIN_API ProcessLineFoliage : public ScreenProcess
{
public:
	virtual bool ProcessImg() override;
	virtual ~ProcessLineFoliage(){};
};

class TATOOLSPLUGIN_API ProcessOpenAsset : public ScreenProcess
{
public:
	virtual bool ProcessImg() override;
	void FixOpenAssetTransform(FTransform& InTransform);
	TArray<AActor*> IgnoreActors;
	virtual ~ProcessOpenAsset(){};
};

class TATOOLSPLUGIN_API FConsiderMesh
{
public:
	FConsiderMesh(UStaticMesh* StaticMesh);

	FVector ConvexCenter;
	FVector2D ErodeBoundMax;
	FVector2D ErodeBoundMin;
	FVector2i ErodeBoundPixels;
	TArray<UStaticMesh*> InputMeshs;
	UStaticMesh* StaticMesh;
	TSharedPtr<FDynamicMesh3> OriginalMesh;
	
	virtual ~FConsiderMesh() {}

	virtual bool ProcessMesh() { return false; };
};

class TATOOLSPLUGIN_API FConsiderMeshZ : public FConsiderMesh
{
	virtual bool ProcessMesh() override;
};


class TATOOLSPLUGIN_API FConsiderMeshY : public FConsiderMesh
{
	virtual bool ProcessMesh() override;
};


USTRUCT()
struct FMeshData
{
	GENERATED_USTRUCT_BODY()
public:
	UPROPERTY()
	AActor* Actor = nullptr;
	UPROPERTY()
	UStaticMesh* Mesh;
	UPROPERTY()
	FTransform Transform;
	UPROPERTY()
	int32 Count;

	FORCEINLINE bool operator==(const FMeshData& Other) const
	{
		return (Mesh == Other.Mesh) && (UKismetMathLibrary::EqualEqual_TransformTransform(Transform, Other.Transform)) && (Count == Other.Count);
	}
};

UCLASS()
class TATOOLSPLUGIN_API ASceneCaptureContainter : public AActor
{
	GENERATED_BODY()
	ASceneCaptureContainter()
	{
		Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
		InstanceBlockMesh = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("BlockInstanceMesh"));
		InstanceBlockMesh->SetupAttachment(Root, TEXT("BlockInstanceMesh"));
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		InstanceBlockMesh->SetStaticMesh(Mesh);
		InstanceBlockMesh->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Overlap);
		InstanceBlockMesh->SetCollisionObjectType(ECollisionChannel::ECC_WorldDynamic);
		InstanceBlockMesh->SetVisibility(false, false);
		InstanceBlockMesh->SetHiddenInGame(true);
	}
	
public:
	UPROPERTY(BlueprintReadWrite, Category = "CubeAttrib")
	USceneComponent* Root;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CubeAttrib")
	TArray<FTransform> StoreCaptureTransforms;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CubeAttrib")
	UInstancedStaticMeshComponent* InstanceBlockMesh;

	TArray<FMeshData> MeshData;
	TArray<UStaticMesh*> InputMeshs;
	TMap<UStaticMesh*, TArray<FVector>> MeshOpenVertexMap;
	TMap<UStaticMesh*, TSharedPtr<FDynamicMesh3>> DynamicMeshData;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CubeAttrib")
	FVector SourceCenter;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CubeAttrib")
	FVector SourceExtent;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CubeAttrib")
	TArray<AActor*> PickActors;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CubeAttrib")
	int32 CurrentTransformIndex = 0;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CubeAttrib")
	float DivdeSize = 0;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CubeAttrib")
	float Block = 0;

	float ExtentMult = 1.2;
	float ScreenDepthMax = 100000;
	
	Mat ReferenceImg1;
	Mat ReferenceImg2;

	UFUNCTION(BlueprintCallable, Category = SceneCaptureContainter)
	void GenerateTransforms();

	UFUNCTION(BlueprintCallable, Category = SceneCaptureContainter)
	void GenerateBlockBox();



};

