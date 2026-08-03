#pragma once

#include "CoreMinimal.h"
#include "CSGpuMeshTypes.h"
#include "Engine/EngineTypes.h"

class AActor;
class UMaterialInterface;
class UStaticMesh;
struct FMeshDescription;

/**
 * 单一的「GPU 三角内容 → StaticMesh」转换入口。
 *
 * 此前每条产出路径（MeshBoolean / Road / Vine / DirectMesh / VDB / ShallowWater）都自带一套
 * 装配代码，绕序约定、退化面阈值、切线正交化、材质槽处理各不相同，改一处修不到其余几处。
 * 这里把整条链切成职责不重叠的两段：
 *
 *   属性段 BuildMeshDescription —— 只做几何与属性装配，不认识 package / AssetRegistry。
 *   落盘段 BuildStaticMesh      —— 只负责建资产或 transient 网格，不碰几何属性。
 *
 * 回读段仍归各组件自己（它们只搬 registry id，不认识 UObject 材质）。
 *
 * 绕序约定在属性段集中实现且只此一份：UE 是左手坐标系 + 逆时针绕序，StaticMesh 对角点序
 * (0,1,2) 的正面法线取的是**反向**叉积 cross(P2-P0, P1-P0)，见引擎
 * StaticMeshOperations.cpp 的 ComputeTriangleTangentsAndNormals。
 */
namespace CSGpuMeshConvert
{
	/** 属性装配选项。退化面阈值等判据统一在此，避免各路径各自为政。 */
	struct FConvertOptions
	{
		/** 目标 actor/组件变换。bBakeToLocalSpace 为真时把世界空间数据烘到它的局部空间。 */
		FTransform TargetTransform = FTransform::Identity;

		/** 源数据是世界空间、需要烘到 TargetTransform 的局部空间。源数据本就是局部空间时置 false。 */
		bool bBakeToLocalSpace = true;

		/** 忽略源法线切线，按几何重算。 */
		bool bRecomputeNormals = false;

		/** 三角面积平方低于该值视为退化并丢弃。 */
		float DegenerateAreaThresholdSq = 1.0e-8f;

		/** 材质槽为空时填入引擎默认表面材质，避免输出网格渲染成默认灰且无法在编辑器里区分槽位。 */
		bool bFillEmptySlotsWithDefaultMaterial = true;
	};

	/** 落盘选项。空 AssetPath 表示用 OwnerActor 所在 level 旁的 result 目录。 */
	struct FAssetOptions
	{
		FString AssetPath;
		bool bTransient = false;
		bool bReplaceExisting = true;
		bool bSaveToDisk = false;
	};

	/** 返回引擎默认表面材质，供空槽兜底。 */
	COMPUTESHADERGENERATOR_API UMaterialInterface* GetDefaultSurfaceMaterial();

	/**
	 * 由 GPU 回读快照装配 LOD0 的 FMeshDescription。支持索引网格（顶点数 != 角点数）、
	 * per-corner 属性与 per-triangle 材质槽。不触碰任何资产系统。
	 */
	COMPUTESHADERGENERATOR_API bool BuildMeshDescription(
		const FCSGpuMeshCPUData& MeshData,
		const FConvertOptions& Options,
		FMeshDescription& OutMeshDescription);

	/**
	 * 由 GPU 回读快照产出 UStaticMesh。Materials 为空槽时按 Options 兜底默认材质。
	 * bTransient 时在 Outer 下建临时网格（不提交 MeshDescription，省下每网格约 1.5 GiB 常驻）；
	 * 否则建成资产并标脏，bSaveToDisk 决定是否立即写盘。
	 */
	COMPUTESHADERGENERATOR_API UStaticMesh* BuildStaticMesh(
		UObject* Outer,
		const AActor* OwnerActor,
		const FCSGpuMeshCPUData& MeshData,
		const TArray<UMaterialInterface*>& Materials,
		const FConvertOptions& Options,
		const FAssetOptions& AssetOptions);
}
