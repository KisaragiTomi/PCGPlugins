#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;
class UPackage;
class UStaticMesh;
struct FMeshDescription;

/**
 * 「把一份网格描述落成 StaticMesh」的唯一一条实现。
 *
 * 在此之前，这件事在仓库里有三份各自为政的实现：
 *   - UCSGpuMeshComponent::SaveGpuMeshDataToStaticMesh（GPU 回读快照，最完整的一份）
 *   - UGeometryGeneral::SaveDynamicMeshToStaticMesh（DynamicMesh，无条件 DeleteAsset）
 *   - UGeometryEditorFunction::CreateStaticMeshAsset（DynamicMesh，不建目录、不处理重名）
 * 三份的路径清洗、重名处理、注册与写盘各写各的，行为并不一致——最要命的一处是后两份用
 * DeleteAsset 覆盖同名资产，而那会打断所有已有引用，并且只要还有人在内存里引用它就直接失败。
 *
 * 这里保留的是三者中正确的那套语义（源自 CSMesh 那份）：同名资产**就地重建**，保留同一个
 * UObject，引用自然跟着更新；只有同名的东西不是 StaticMesh 时才退回删除。
 *
 * 分成四步而不是一个大函数，是为了让「破坏性操作」晚于「可能失败的几何构建」发生：
 *
 *     ResolveTarget()        只读探测：清洗路径、校验、建目录、看同名资产能否复用
 *     <调用方构建 FMeshDescription —— 失败可以在这里直接退出，资产毫发无损>
 *     PrepareMesh()          破坏性：删旧 / 建包 / 新建对象，或就地清空材质槽与 section 表
 *     PopulateFromDescription()
 *     Finalize()             注册 + 标脏 + 可选写盘
 *
 * 合成一步就会回到旧行为：几何构建失败时资产已经被删掉或清空，而调用方只拿到一个 nullptr。
 */

/** 一次资产落地的目标。ResolveTarget 填好它，PrepareMesh / Finalize 消费它。 */
struct COMPUTESHADERGENERATOR_API FCSStaticMeshAssetTarget
{
	/** 清洗并校验过的完整包路径（/Game/.../SM_Foo）。 */
	FString SanitizedPath;

	/** 包内对象名（SM_Foo）。 */
	FString AssetName;

	/** 同名且可复用的既有资产。非空 = 就地重建，空 = 新建。 */
	UStaticMesh* ExistingMesh = nullptr;

	/** 同名的东西存在但不是 StaticMesh，PrepareMesh 必须先把它删掉。 */
	bool bMustDeleteConflicting = false;

	bool IsValid() const { return !SanitizedPath.IsEmpty() && !AssetName.IsEmpty(); }
};

namespace CSStaticMeshAsset
{
	/**
	 * 材质槽命名。MeshDescription 的 polygon group 名与 StaticMesh 的材质槽名必须逐字一致，
	 * 否则 BuildFromMeshDescriptions 对不上槽、三角会落到错误的材质上。两边都从这里取名，
	 * 就不存在"改了一边忘了另一边"。
	 */
	COMPUTESHADERGENERATOR_API FName MaterialSlotName(int32 Slot);

	/**
	 * 把 FMeshDescription 写进 StaticMesh：Nanite 设置、材质槽表、BuildFromMeshDescriptions。
	 * 落盘资产与 transient 预览共用，保证两者材质槽与构建设置一致。
	 *
	 * TriangleMaterialSlots 只用来把槽位数撑到实际用到的最大槽号——缺槽会在落盘时丢面。传空即可，
	 * 此时槽数取 max(1, Materials.Num())。
	 *
	 * bCommitMeshDescription=true 走完整构建（按属性相等合并顶点，且保留可编辑源数据），
	 * false 走 fast build（每三角三顶点、不合并），供一次性预览使用。
	 *
	 * 非编辑器构建下同样可用——它不碰资产系统。
	 */
	COMPUTESHADERGENERATOR_API bool PopulateFromDescription(
		UStaticMesh* StaticMesh,
		FMeshDescription& MeshDescription,
		const TArray<UMaterialInterface*>& Materials,
		TArrayView<const int32> TriangleMaterialSlots,
		bool bCommitMeshDescription,
		bool bEnableNanite);

#if WITH_EDITOR
	/**
	 * 只读探测，不改动任何资产：清洗路径 → 校验 → 确保目录存在 → 判断同名资产能否就地复用。
	 * 失败时落一条带 LogPrefix 的日志并返回 false。
	 * bReplaceExisting=false 且同名资产已存在时同样返回 false（视为"跳过"）。
	 */
	COMPUTESHADERGENERATOR_API bool ResolveTarget(
		const FString& AssetPath,
		bool bReplaceExisting,
		const TCHAR* LogPrefix,
		FCSStaticMeshAssetTarget& OutTarget);

	/**
	 * 破坏性的一步：需要时删掉冲突资产、建包、新建 UStaticMesh；可复用时就地 Modify 并清空
	 * 材质槽与 section 表（它们会按本次网格重新装配，残留会串到前面）。
	 * 只在几何已经准备好、确定要写入时才调用。
	 */
	COMPUTESHADERGENERATOR_API UStaticMesh* PrepareMesh(
		const FCSStaticMeshAssetTarget& Target,
		const TCHAR* LogPrefix);

	/** 注册（仅新建时）+ 标脏 + 可选写盘。 */
	COMPUTESHADERGENERATOR_API void Finalize(
		UStaticMesh* StaticMesh,
		const FCSStaticMeshAssetTarget& Target,
		bool bSaveToDisk,
		const TCHAR* LogPrefix);
#endif // WITH_EDITOR
}
