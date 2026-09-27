#pragma once

#include "CoreMinimal.h"

#if WITH_EDITOR

class UCSMesh;
class UMaterialInstanceConstant;
class UStaticMesh;
struct FCustomPrimitiveData;

/**
 * 一个源（静态网格组件，或 ISM 的一个实例）烘焙时喂给材质的图元数据。材质里的 ObjectPosition（WorldBounds 中心）/
 * ActorPosition / 包围盒 / 自定义图元数据都取自这里（MaterialBaking 的 FPrimitiveData）—— 所以必须按源分开烘，
 * 合在一起烘这些节点全都会错。WorldPosition 不靠它：烘焙网格直接给世界空间位置、LocalToWorld 用单位矩阵
 * （原因见 CSNaniteCutBake.cpp），所以 LocalPosition / ObjectOrientation 这类局部空间节点按世界空间算。
 */
struct FCSNaniteCutBakeSource
{
	/** 截面抽取用的那个变换（组件或实例的世界变换）。目前只作记录，烘焙时 LocalToWorld 给单位矩阵。 */
	FTransform LocalToWorld = FTransform::Identity;
	FVector ActorPosition = FVector::ZeroVector;
	FBoxSphereBounds WorldBounds = FBoxSphereBounds(ForceInitToZero);
	FBoxSphereBounds LocalBounds = FBoxSphereBounds(ForceInitToZero);
	/** 组件的自定义图元数据；指针在烘焙期间必须有效。 */
	const FCustomPrimitiveData* CustomPrimitiveData = nullptr;
};

struct FCSNaniteCutBakeParams
{
	/** 输出网格烘到这个变换的局部空间（挂载它的 actor 的变换）。 */
	FTransform OutputTransform = FTransform::Identity;
	/** 资产目录（长包路径），如 /Game/Maps/AutoResult。 */
	FString AssetFolder;
	/** 资产名主干：SM_<主干>、MI_<主干>、T_<主干>_BaseColor / _Normal / _Roughness。同名资产就地覆盖。 */
	FString AssetBaseName;
	/** 三张贴图的边长。 */
	int32 TextureSize = 2048;
	bool bEnableNanite = true;
};

struct FCSNaniteCutBakeResult
{
	UStaticMesh* StaticMesh = nullptr;
	UMaterialInstanceConstant* Material = nullptr;
	int32 Triangles = 0;
	/** (源, 材质) 条目数（每条一次绘制，都画进同一套渲染目标）。 */
	int32 BakeJobs = 0;
	/** 三角覆盖到、但 GPU 烘焙没写到的纹素（落在三角边上），由周边补齐。 */
	int32 PatchedTexels = 0;
	/** 图集里被三角覆盖的纹素比例。 */
	float Coverage = 0.0f;
	double Seconds = 0.0;
};

namespace CSNaniteCutBake
{
	/**
	 * 把截面网格烘成一个带独立 UV 与三张贴图（BaseColor / Normal / Roughness）的静态网格资产。
	 *
	 *   1. 回读网格（世界空间，含源的全部 UV 组、法线、切线、副法线符号、顶点色、逐三角材质号）；
	 *   2. 按源 UV0 的岛重新打包出一套不重叠的 UV（FStaticMeshOperations::GenerateUV，引擎 MeshMerge 同款）；
	 *   3. 所有 (源, 材质) 在一次 MaterialBaking 里画进同一套整图集大小的渲染目标（单输出路径：渲染目标不进
	 *      模块的池子，烘完随 GC 释放）：位置法线切线给世界空间、图元数据给源自己的，所以用了 WorldPosition /
	 *      ObjectPosition / ActorPosition / 包围盒 / 自定义图元数据 / 顶点色 / 各组 UV 的材质都和原来一样
	 *      （LocalPosition / ObjectOrientation 这类局部空间节点除外，与引擎 MeshMerge 相同）；
	 *   4. 背景色 = 没画到，三角覆盖却没画到的边缘纹素与岛间空白外扩补齐；
	 *   5. 引擎的扁平化材质（BaseFlattenMaterial）建三张贴图 + 材质实例；
	 *   6. 输出网格保留源的法线与切线（法线贴图烘在源的切线空间里，保留切线才对得上），UV0 = 新图集，
	 *      不重算法线切线、全精度 UV；开 Nanite 时打开显式切线（否则 Nanite 按 UV 自己推切线）。
	 *
	 * Mesh：截面（可已做可见性剔除），抽取时开了 bUniqueMaterialsPerSource；MaterialSources 是抽取结果里的
	 * 同名数组；Sources 按源下标给出图元数据。资产只标脏不写盘（与 MeshBoolean 的 AutoResult 一致）。
	 * 游戏线程；阻塞；要真 RHI。
	 */
	COMPUTESHADERGENERATOR_API bool Bake(
		UCSMesh* Mesh,
		TConstArrayView<int32> MaterialSources,
		TConstArrayView<FCSNaniteCutBakeSource> Sources,
		const FCSNaniteCutBakeParams& Params,
		FCSNaniteCutBakeResult& OutResult);
}

#endif // WITH_EDITOR
