// Fill out your copyright notice in the Description page of Project Settings.


#include "GeometryEditorFunction.h"

#include "PackageTools.h"
#include "UDynamicMesh.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetUtils/CreateStaticMeshUtil.h"


using namespace UE::Geometry;

UStaticMesh* UGeometryEditorFunction::CreateStaticMeshAsset(UDynamicMesh* TargetMesh, FString AssetPathAndName, TArray<UMaterialInterface*> Materials)
{
	if (TargetMesh->GetTriangleCount() == 0) return nullptr;
	
	UE::AssetUtils::FStaticMeshAssetOptions AssetOptions;
	AssetPathAndName = UPackageTools::SanitizePackageName(AssetPathAndName);
	AssetOptions.NewAssetPath = AssetPathAndName;
	AssetOptions.NumSourceModels = 1;
	AssetOptions.AssetMaterials = Materials;
	AssetOptions.bEnableRecomputeNormals = false;
	AssetOptions.bEnableRecomputeTangents = true;
	AssetOptions.CollisionType = ECollisionTraceFlag::CTF_UseComplexAsSimple;
	AssetOptions.NumMaterialSlots = Materials.Num();
	
	FDynamicMesh3 CopyMesh;
	TargetMesh->ProcessMesh([&](const FDynamicMesh3& ReadMesh)
	{
		CopyMesh = ReadMesh;
	});
	AssetOptions.SourceMeshes.DynamicMeshes.Add(&CopyMesh);

	UE::AssetUtils::FStaticMeshResults ResultData;
	UE::AssetUtils::ECreateStaticMeshResult AssetResult = UE::AssetUtils::CreateStaticMeshAsset(AssetOptions, ResultData);
	
	UStaticMesh* NewStaticMesh = ResultData.StaticMesh;
	NewStaticMesh->PostEditChange();
	GEditor->EndTransaction();
	FAssetRegistryModule::AssetCreated(NewStaticMesh);
	return NewStaticMesh;
}