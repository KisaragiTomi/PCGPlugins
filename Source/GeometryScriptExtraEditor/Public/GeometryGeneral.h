// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "DynamicMeshEditor.h"
#include "UDynamicMesh.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"

#include "GeometryGeneral.generated.h"

/**
 * 
 */
using namespace UE::Geometry;

UCLASS()
class GEOMETRYSCRIPTEXTRAEDITOR_API UGeometryGeneral : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* BlurVertexNormals(UDynamicMesh* TargetMesh, int32 Iteration = 5, bool RecomputeNormals = true);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* CreateVertexNormals(UDynamicMesh* TargetMesh);
	
	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* CreateVertexNormalFromOverlay(UDynamicMesh* TargetMesh);
	
	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* FillLine(UDynamicMesh* TargetMesh, TArray<FVector> VertexLoop);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* PrimNormal(UDynamicMesh* TargetMesh, FVector TestPos, FVector& OutVector);


	static FVector GetNearestLocationNormal(FDynamicMesh3& EditMesh, FGeometryScriptTrianglePoint NearestPoint);

	static void AppendPrimitive(
	UDynamicMesh* TargetMesh,
	FMeshShapeGenerator* Generator, 
	FTransform Transform, 
	FGeometryScriptPrimitiveOptions PrimitiveOptions,
	FVector3d PreTranslate = FVector3d::Zero(),
	TOptional<FQuaterniond> PreRotate = TOptional<FQuaterniond>());
};