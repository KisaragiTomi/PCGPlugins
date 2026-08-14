#pragma once

// -----------------------------------------------------------------------------
// FCSBoxScenePreparedData 的 PImpl 定义（Private 头，不导出模块 API）。
//
// 这个结构不能住在 CSBoxSceneCollection.h 里：它的成员是 CSMeshGenInternal 的内部提取类型
// （resolved render 资源引用、Nanite 源三角），公开出去就等于把整条提取管线的私有细节
// 钉进公共 API。反过来也不能只留在某一个 .cpp 里 —— 生产端（CSBoxSceneCollection.cpp 的
// game-thread 收集）与消费端（ComputeShaderMeshGenerator.cpp 的 render-thread 上传）分属
// 两个 TU，两边都要看到完整定义，故收进这个共享私有头。
// -----------------------------------------------------------------------------

#include "CoreMinimal.h"
#include "CSBoxSceneCollection.h"
#include "MeshGeneratorInternal.h"

struct FCSBoxScenePreparedDataImpl
{
	TArray<CSMeshGenInternal::FResolvedStaticMeshTriangleRequest> ResolvedRequests;
	uint64 TotalStaticMeshTriangleCount = 0;
	FCSTriangleMeshData LandscapeTriangleData;
	TArray<FVector> ReferencePoints;
	float ReferenceFilterDistance = 0.0f;
	int32 SafeMaxTriangles = 1;
	// 去重材质表：soup 材质 buffer 里的 id 索引进本表。TObjectPtr 持有强引用，保证 readback 前不被 GC。
	TArray<TObjectPtr<UMaterialInterface>> MaterialRegistry;
	// Nanite 网格的全细节源三角（editor MeshDescription 提取，world-space + UV0 + 材质 id）。
	// game thread 在 CollectBoxSceneTriangles 里填充，render thread 在 AddPreparedBoxSceneTrianglesToRDG 追加进 soup。
	CSMeshGenInternal::FCSNaniteSourceTriangleData NaniteTriangles;
};
