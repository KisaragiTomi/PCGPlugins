// Fill out your copyright notice in the Description page of Project Settings.


#include "GeometryEditorFunction.h"

#include "GeometryGeneral.h"

UStaticMesh* UGeometryEditorFunction::CreateStaticMeshAsset(UDynamicMesh* TargetMesh, FString AssetPathAndName, TArray<UMaterialInterface*> Materials)
{
	// 这里以前自己拼一份 UE::AssetUtils::FStaticMeshAssetOptions，与 UGeometryGeneral 那份逐字
	// 重复（同样的 NumSourceModels / bEnableRecomputeNormals=false / bEnableRecomputeTangents /
	// CTF_UseComplexAsSimple），却少了建目录、覆盖已有资产、标脏和可选落盘，另外还带三个缺陷：
	//   - TargetMesh 为空时直接解引用
	//   - 不检查 CreateStaticMeshAsset 的返回码就用 ResultData.StaticMesh
	//   - 一次没有配对 BeginTransaction 的 GEditor->EndTransaction()
	// 保留本入口只为不打断已有蓝图连线，实现统一到公用那条。
	return UGeometryGeneral::SaveDynamicMeshToStaticMeshWithMaterials(TargetMesh, AssetPathAndName, Materials);
}
