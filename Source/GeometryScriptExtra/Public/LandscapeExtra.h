// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "LandscapeComponent.h"
#include "UDynamicMesh.h"
#include "LandscapeExtra.generated.h"




/**
 * 
 */
UCLASS()
class GEOMETRYSCRIPTEXTRA_API ULandscapeExtra : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = Landscape)
	static UDynamicMesh* CreateProjectPlane(UDynamicMesh* Mesh, FVector Center, FVector Extent, int32 ExtentPlus = 1);

	UFUNCTION(BlueprintCallable, Category = Landscape)
	static bool ProjectPoint(FVector SourceLocation, FVector& OutLocation, FVector& OutNormal);

};


