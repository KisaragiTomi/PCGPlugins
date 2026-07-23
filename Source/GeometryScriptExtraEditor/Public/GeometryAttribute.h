// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "DynamicMeshEditor.h"
#include "UDynamicMesh.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"


/**
 * 
 */
using namespace UE::Geometry;


#define T_CREATE_AND_INIT_ATTR(AttrName, ValueType, Dim, MeshPtr, InitValue) \
FName N##AttrName = FName(TEXT(#AttrName)); \
TDynamicMeshTriangleAttribute<ValueType, Dim>* A##AttrName = new TDynamicMeshTriangleAttribute<ValueType, Dim>(&(MeshPtr)); \
A##AttrName->Initialize(InitValue); \
A##AttrName->SetName(N##AttrName);\
MeshPtr.Attributes()->AttachAttribute(N##AttrName,A##AttrName);


#define TI_ATTR(AttrName, MeshPtr, InitValue) \
T_CREATE_AND_INIT_ATTR(TI_##AttrName, int, 1, MeshPtr, InitValue)

#define TF_ATTR(AttrName, MeshPtr, InitValue) \
T_CREATE_AND_INIT_ATTR(TF_##AttrName, float, 1, MeshPtr, InitValue)

#define TV_ATTR(AttrName, MeshPtr, InitValue) \
T_CREATE_AND_INIT_ATTR(TV_##AttrName, FVector3f, 3, MeshPtr, InitValue)

#define TP_ATTR(AttrName, MeshPtr, InitValue) \
T_CREATE_AND_INIT_ATTR(TP_##AttrName, FVector4f, 4, MeshPtr, InitValue)


#define V_CREATE_AND_INIT_ATTR(AttrName, ValueType, Dim, MeshPtr, InitValue) \
FName N##AttrName = FName(TEXT(#AttrName)); \
TDynamicMeshVertexAttribute<ValueType, Dim>* A##AttrName = new TDynamicMeshVertexAttribute<ValueType, Dim>(&(MeshPtr)); \
A##AttrName->Initialize(InitValue);\
A##AttrName->SetName(N##AttrName);\
MeshPtr.Attributes()->AttachAttribute(N##AttrName,A##AttrName);


#define VI_ATTR(AttrName, MeshPtr, InitValue) \
V_CREATE_AND_INIT_ATTR(VI_##AttrName, int, 1, MeshPtr, InitValue)

#define VF_ATTR(AttrName, MeshPtr, InitValue) \
V_CREATE_AND_INIT_ATTR(VF_##AttrName, float, 1, MeshPtr, InitValue)

#define VV_ATTR(AttrName, MeshPtr, InitValue) \
V_CREATE_AND_INIT_ATTR(VF_##AttrName, FVector3f, 3, MeshPtr, InitValue)

#define VP_ATTR(AttrName, MeshPtr, InitValue) \
V_CREATE_AND_INIT_ATTR(VF_##AttrName, FVector3f, 4, MeshPtr, InitValue)


namespace GeometryAttribute
{
	int GetAttrib(int ID)
	{
		int IntValue = -1;
		// ATI_Class->GetValue(ID, &IntValue);
		return IntValue;
	}
#define PointInt(AttrName, ID) \
	int IntValue = -1; \
	AVI_##AttrName->GetValue(ID, &IntValue); \
	return IntValue; 
}


