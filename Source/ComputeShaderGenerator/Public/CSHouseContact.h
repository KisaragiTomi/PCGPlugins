#pragma once

#include "CoreMinimal.h"
#include "CSHouseProfile.h"   // FCSWallCut
#include "CSHouseSeam.h"      // FCorner / FHouse / Canonical / SeamSeed
#include "Misc/Guid.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtrTemplates.h"

class ACSHouseActor;

/**
 * 两栋房之间的一条**接触记录**（TinyGladeHouse D7「接触记录 `FCSHouseContact` 与派生」，2026-09-16）。
 *
 * 用户裁决的形态：房子只知道自己跟哪些接触有关（`ACSHouseActor::Contacts`），接触只知道自己跟哪两栋房
 * 有关（`Houses[2]`）；规范序、谁出砖、随机种子都是接触自己的事，房子不再算 `Canonical`。
 * 一对房子至多一条接触，两端各持一个 `TSharedPtr` 指向**同一个对象** —— 这就是「A 写 B 的变量」里那个变量。
 *
 * 不是 UObject、不是 UPROPERTY：不存盘、不进事务、复制 / PIE 得到空表，加载后由第一次提交重建。
 * 协议（谁动了谁发、被发方只清只用、松手才提交、删除时对每个旧对端发空）全在 `ACSHouseActor` 那一侧，
 * 见 `ACSHouseActor::ReceiveContactFrom` 与 `UpdateContacts`。
 *
 * 派生按接触的**方向与位置**分（TG 从同一张 `ShapeIntersections` 派生出 stitch bricks / elevation
 * supports / inter-roof merlons 三种产物，是同一个结构）：
 * - 竖缝 `FCSHouseSeamContact`：footprint 相交且 Z 区间重叠 —— 墙对墙穿插 ⇒ 接缝砖柱 + 两端墙的 clip 段。
 * - 横缝在底 `FCSHouseBearingContact`：footprint 相交且上房底 ≈ 下房檐口 —— 上房被下房承托 / 悬挑
 *   ⇒ 横梁 + 木柱（本轮只判定、不出产物，见 `Classify` 与计划 D7）。
 */
enum class ECSHouseContactKind : uint8
{
	Seam,      // 竖缝：墙对墙穿插
	Bearing,   // 横缝在底：承托 / 悬挑
};

struct COMPUTESHADERGENERATOR_API FCSHouseContact
{
	virtual ~FCSHouseContact() = default;

	ECSHouseContactKind Kind = ECSHouseContactKind::Seam;
	/** 规范序（`CSHouseSeam::Canonical`）：槽 0 = GUID 小者。 */
	TWeakObjectPtr<ACSHouseActor> Houses[2];
	/** 同一规范序的 GUID：弱引用失效之后仍能定序、派生种子。 */
	FGuid Ids[2];

	/** 两端都有效且都参与接缝（`bSeamEnabled`）。不活的记录由持有者在下次「验」时丢弃。 */
	bool IsAlive() const;
	bool Involves(const ACSHouseActor* House) const;
	/** 0 / 1；不涉及时 −1。 */
	int32 SlotOf(const ACSHouseActor* House) const;
	ACSHouseActor* Other(const ACSHouseActor* House) const;
	/** 谁出砖：规范序里第一个能出砖的（`bFrameEnabled` 且有 `FrameBrickMesh`）。判据是当前状态，不随重建顺序变。 */
	ACSHouseActor* Owner() const;
	/** 逐实例随机数的基 = `CSHouseSeam::SeamSeed`：规范序之下两栋房得到同一个值。 */
	uint32 Seed() const { return HashCombine(GetTypeHash(Ids[0]), GetTypeHash(Ids[1])); }

	/**
	 * 纯函数：两栋房现在的接触 —— 竖缝 / 横缝 / 空。
	 *
	 * 先按容差判横缝（`abs(上房底 − 下房檐口) ≤ BearingTolerance` 且 footprint 相交），不满足再按
	 * 「footprint 相交且 Z 区间重叠」判竖缝。容差挡的是「上房嵌进下房几十厘米」这个常态：落座只看地面，
	 * 摞房靠用户拉 `HeightOffset`，拉不到正好齐平；没有容差就会在轮廓交点立几十厘米高的矮墩子。
	 * TG 没有专门判「摞」，靠「柱高不足 1.5 m 不砌」把浅重叠当成了坐在上面。
	 */
	static TSharedPtr<FCSHouseContact> Classify(ACSHouseActor* A, ACSHouseActor* B, float BearingTolerance);
};

/** 竖缝：一对房子之间的全部接缝几何，打包成一体。 */
struct COMPUTESHADERGENERATOR_API FCSHouseSeamContact final : public FCSHouseContact
{
	TArray<CSHouseSeam::FCorner> Corners;
	/** 两端各自的裁剪段，与 `Houses` 同序。 */
	TArray<FCSWallCut> Cuts[2];

	const TArray<FCSWallCut>& CutsFor(const ACSHouseActor* House) const;
};

/** 横缝在底：本轮只判定不出产物（梁 / 木柱落地时再加字段）。 */
struct COMPUTESHADERGENERATOR_API FCSHouseBearingContact final : public FCSHouseContact
{
};
