#include "CSHouseContact.h"

#include "CSHouseActor.h"

bool FCSHouseContact::IsAlive() const
{
	for (const TWeakObjectPtr<ACSHouseActor>& H : Houses)
	{
		const ACSHouseActor* House = H.Get();
		if (!IsValid(House) || !House->IsSeamParticipant()) return false;
	}
	return true;
}

bool FCSHouseContact::Involves(const ACSHouseActor* House) const
{
	return SlotOf(House) >= 0;
}

int32 FCSHouseContact::SlotOf(const ACSHouseActor* House) const
{
	if (!House) return -1;
	// 弱引用失效后按 GUID 兜底：删除路径上对端要能认出"这条涉及正在消失的那栋房"。
	for (int32 Slot = 0; Slot < 2; ++Slot)
	{
		if (Houses[Slot].GetEvenIfUnreachable() == House) return Slot;
		if (Ids[Slot].IsValid() && Ids[Slot] == House->GetHouseId()) return Slot;
	}
	return -1;
}

ACSHouseActor* FCSHouseContact::Other(const ACSHouseActor* House) const
{
	const int32 Slot = SlotOf(House);
	return Slot < 0 ? nullptr : Houses[1 - Slot].Get();
}

ACSHouseActor* FCSHouseContact::Owner() const
{
	for (const TWeakObjectPtr<ACSHouseActor>& H : Houses)
	{
		ACSHouseActor* House = H.Get();
		if (IsValid(House) && House->CanBuildSeamBricks()) return House;
	}
	return nullptr;
}

TSharedPtr<FCSHouseContact> FCSHouseContact::Classify(ACSHouseActor* A, ACSHouseActor* B)
{
	if (!IsValid(A) || !IsValid(B) || A == B) return nullptr;
	if (!A->IsSeamParticipant() || !B->IsSeamParticipant()) return nullptr;

	const CSHouseSeam::FHouse HA = A->MakeSeamHouse();
	const CSHouseSeam::FHouse HB = B->MakeSeamHouse();
	if (!HA.Id.IsValid() || !HB.Id.IsValid() || HA.Id == HB.Id) return nullptr;
	if (!CSHouseSeam::WithinReach(HA, HB)) return nullptr;

	const CSHouseSeam::FHouse* First = nullptr;
	const CSHouseSeam::FHouse* Second = nullptr;
	CSHouseSeam::Canonical(HA, HB, First, Second);
	ACSHouseActor* FirstActor = First == &HA ? A : B;
	ACSHouseActor* SecondActor = First == &HA ? B : A;

	auto Fill = [&](FCSHouseContact& C, ECSHouseContactKind Kind)
	{
		C.Kind = Kind;
		C.Houses[0] = FirstActor;
		C.Houses[1] = SecondActor;
		C.Ids[0] = First->Id;
		C.Ids[1] = Second->Id;
	};

	// 横缝：上房底 ≈ 下房檐口（容差内）且 footprint 相交 —— 不看 Z 重叠。
	// 两处都不许取决于参数顺序（谁提交谁是 A）：房底相同就没有上房；容差取两栋的较大者（见头文件）。
	const float Tolerance = FMath::Max3(A->BearingTolerance, B->BearingTolerance, 0.0f);
	if (HA.BaseZ != HB.BaseZ)
	{
		const CSHouseSeam::FHouse& Upper = HA.BaseZ > HB.BaseZ ? HA : HB;
		const CSHouseSeam::FHouse& Lower = HA.BaseZ > HB.BaseZ ? HB : HA;
		if (FMath::Abs(Upper.BaseZ - Lower.EaveZ()) <= Tolerance && CSHouseSeam::IntersectsXY(HA, HB))
		{
			TSharedPtr<FCSHouseBearingContact> Bearing = MakeShared<FCSHouseBearingContact>();
			Fill(*Bearing, ECSHouseContactKind::Bearing);
			return Bearing;
		}
	}

	// 竖缝：footprint 真重叠且 Z 区间相交。
	if (!CSHouseSeam::Intersects(HA, HB)) return nullptr;
	TSharedPtr<FCSHouseSeamContact> Seam = MakeShared<FCSHouseSeamContact>();
	Fill(*Seam, ECSHouseContactKind::Seam);
	Seam->Shapes[0] = *First;
	Seam->Shapes[1] = *Second;
	CSHouseSeam::BuildCorners(*First, *Second, Seam->Corners);
	for (int32 Slot = 0; Slot < 2; ++Slot)
	{
		const CSHouseSeam::FHouse& Self = Slot == 0 ? *First : *Second;
		const CSHouseSeam::FHouse& Other = Slot == 0 ? *Second : *First;
		for (int32 Edge = 0; Edge < Self.Footprint.NumEdges(); ++Edge)
		{
			FCSWallCut Cut;
			if (CSHouseSeam::CutOnEdge(Self, Other, Edge, Cut)) Seam->Cuts[Slot].Add(Cut);
		}
	}
	return Seam;
}

const TArray<FCSWallCut>& FCSHouseSeamContact::CutsFor(const ACSHouseActor* House) const
{
	const int32 Slot = SlotOf(House);
	static const TArray<FCSWallCut> Empty;
	return Slot < 0 ? Empty : Cuts[Slot];
}
