// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "DynamicMeshActor.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "UDynamicMesh.h"
#include "Curves/CurveLinearColor.h"

#include "GeometryVisualization.generated.h"


/**
 * 
 */


UCLASS()
class GEOMETRYSCRIPTEXTRA_API UGeometryVisualization : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = Visualize)
	static void VisualizingVertexNormal(UDynamicMesh* TargetMesh, FTransform Transform, float Length = 100);
};



