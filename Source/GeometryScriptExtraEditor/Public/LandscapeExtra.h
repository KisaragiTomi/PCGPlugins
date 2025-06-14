// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "LandscapeComponent.h"
#include "UDynamicMesh.h"
#include "LandscapeExtra.generated.h"




USTRUCT()
struct GEOMETRYSCRIPTEXTRAEDITOR_API FReadLandscapeData
{
GENERATED_BODY()
	TArray<FLinearColor> Colors;
	TArray<FFloat16Color> Colors16;
	TArray<FLinearColor> ValidColors;
	FIntVector4 ReadRange = FIntVector4(0, 0, 0, 0);
	FIntVector2 TextureSize = FIntVector2(0, 0);
	FIntVector2 TextureVaildSize = FIntVector2(0, 0);
	FVector2f ValidUVRange = FVector2f::Zero();
	FVector MapMin = FVector::ZeroVector;
	FVector MapMax = FVector::ZeroVector;
	FVector ValidMapMin = FVector::ZeroVector;
	FVector ValidMapMax = FVector::ZeroVector;
	FTransform Transform = FTransform::Identity;
	FBoxSphereBounds ValidTextureBounds;
	FBoxSphereBounds TextureBounds;
};

UCLASS()
class GEOMETRYSCRIPTEXTRAEDITOR_API ULandscapeExtra : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = Landscape)
	static UDynamicMesh* CreateProjectPlane(UDynamicMesh* Mesh, FVector Center, FVector Extent, int32 ExtentPlus = 1);

	UFUNCTION(BlueprintCallable, Category = Landscape)
	static bool ProjectPoint(FVector SourceLocation, FVector& OutLocation, FVector& OutNormal);

	UFUNCTION(BlueprintCallable, Category = Landscape)
	static TArray<FLinearColor> GetLandscapeData(FVector Center, FVector Extent, int32 ExtentPlus = 1);
	

	static void CreateLandscapeTextureData(FReadLandscapeData& LandscapeData, FVector Center, FVector Extent, int32 ExtentPlus = 1);
	                                                       



	UFUNCTION(BlueprintCallable, Category = Landscape)
	static TArray<FLinearColor> CreateLandscapeMeshTextureData(FVector& MapMin, FVector& MapMax, FVector Center, FVector Extent, int32 TextureSize = 256, int32 ExtentPlus = 1);

};


