#include "CSWallBase.h"

#include "CSGpuInstancedMeshComponent.h"
#include "CSGpuMeshTypes.h"
#include "CSMesh.h"
#include "CSMeshRenderComponent.h"
#include "CSVineTube.h"   // 折线 → 管子的对外入口（unity 构建下别指望别人替你带进来）
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

DEFINE_LOG_CATEGORY_STATIC(LogTinyGladeWallBase, Log, All);

ACSWallBase::ACSWallBase()
{
}

float ACSWallBase::GetVineClock() const
{
	return GetWorld() ? float(GetWorld()->GetTimeSeconds()) : 0.0f;
}

CSHouseVine::FParams ACSWallBase::MakeVineParams(const FCSWallVineSettings& V)
{
	CSHouseVine::FParams Params;
	Params.StrandSpacing = V.StrandSpacing;
	Params.SegmentLength = V.SegmentLength;
	Params.MaxSegments = V.MaxSegments;
	Params.Wander = V.Wander;
	Params.MaxLean = V.MaxLean;
	Params.MaxTurn = V.MaxTurn;
	Params.Bloat = V.Bloat;
	Params.Thickness = V.Thickness;
	Params.StandOff = V.StandOff;
	Params.HoleClearance = V.HoleClearance;
	Params.LeafChance = V.LeafChance;
	Params.LeafSize = V.LeafSize;
	Params.LeafSizeJitter = V.LeafSizeJitter;
	// 没配花网格时把概率钉成 0：否则规划器照排记录，而 counter 指向一个没有基础网格的组件 ——
	// 症状是"实例数对得上但屏幕上什么都没有"，与"材质被静默换掉"一样查不出来。
	Params.FlowerChance = V.FlowerMesh ? V.FlowerChance : 0.0f;
	Params.FlowerFromFrac = V.FlowerFromFrac;
	Params.FlowerSize = V.FlowerSize;
	Params.JumpChance = V.JumpChance;
	Params.TipTaperLength = V.TipTaperLength;
	Params.TipTaperMin = V.TipTaperMin;
	Params.MaxGroundGap = V.MaxGroundGap;
	Params.Seed = V.Seed;
	return Params;
}

bool ACSWallBase::AreVineBuffersReady() const
{
	if (VineGpuBuffers.Num() != CSHouseVine::Palette_Num) return false;
	for (int32 Index = 0; Index < CSHouseVine::Palette_Num; ++Index)
	{
		if (!VineGpuBuffers[Index].IsValid()) return false;
	}
	return true;
}

CSShaperSteps::EHandoverResult ACSWallBase::EnsureVineRig(const FCSWallVineSettings& V,
	TFunctionRef<void(uint32& OutMaxRecords, FBox& OutLocalBounds)> Budget, bool bForceFullRebuild)
{
	// 新组件身上没有基础网格快照。三条按下标对齐，任一条是新的就整批重建。已有的（样条墙构造函数里预建的默认子对象）直接用。
	auto EnsureOne = [this](TObjectPtr<UCSGpuInstancedMeshComponent>& Component)
	{
		if (CSShaperSteps::EnsureInstancedComponent(this, Component)) bVineBaseMeshReady = false;
	};
	EnsureOne(VineBranchComponent);
	EnsureOne(VineLeafComponent);
	EnsureOne(VineFlowerComponent);
	if (!IsValid(VineBranchComponent) || !IsValid(VineLeafComponent) || !IsValid(VineFlowerComponent))
	{
		return CSShaperSteps::EHandoverResult::NotReady;
	}
	// ⚠️ 换材质走 `SetInstanceMaterial`（变了才写、变了才让渲染状态重建）：经典路的代理构造时把材质抄走了，直接写属性看不出变化。
	VineBranchComponent->SetInstanceMaterial(V.BranchMaterial);

	// 三季叶：只写母材质上的 `Season` 标量，**不换材质资产**（理由见 `ECSVineSeason`）。
	// ⚠️ MID 的父换了必须重建 —— 在细节面板里换掉叶材质时旧 MID 仍然有效，
	// 于是"换了材质但画面没变"，与 `VineBranchMeshBuiltFrom` 那条是同一个失败模式。
	if (V.LeafMaterial)
	{
		if (!VineLeafSeasonMID || VineLeafSeasonMID->Parent != V.LeafMaterial)
		{
			VineLeafSeasonMID = UMaterialInstanceDynamic::Create(V.LeafMaterial, this);
		}
		if (VineLeafSeasonMID) VineLeafSeasonMID->SetScalarParameterValue(TEXT("Season"), float(uint8(V.Season)));
	}
	else
	{
		VineLeafSeasonMID = nullptr;
	}
	// 退回母材质而不是留空：留空的话组件会画成引擎默认灰，而 `Get*UndrawableReason`
	// 只查"材质非空"就放行了 —— 那正是石阶那个坑的形状。
	VineLeafComponent->SetInstanceMaterial(VineLeafSeasonMID
		? static_cast<UMaterialInterface*>(VineLeafSeasonMID) : ToRawPtr(V.LeafMaterial));

	// 枝 / 花的 MID：只为把 `VineGrowSpeed` 下推过去。父换了必须重建 —— 在细节面板里
	// 换掉材质资产时旧 MID 仍然有效，于是"换了材质但画面没变"（同 VineLeafSeasonMID 那条）。
	// 秒 → 弧长 cm。材质侧的前沿是按弧长推的，而面板上给的是秒（"延迟"问的就是时间）。
	// ⚠️ 这三行是**唯一**的换算点：别在材质里再折算一次，两处各算一份的症状是
	// "改速度时延迟莫名其妙地平方级变化"，而两边各自都自洽。
	const float Speed = FMath::Max(V.GrowSpeed, 1.0f);
	const float FadeCm = FMath::Max(V.GrowFadeSeconds, 0.01f) * Speed;
	const float LeafLagCm = FMath::Max(V.LeafGrowDelay, 0.0f) * Speed;
	const float FlowerLagCm = FMath::Max(V.FlowerGrowDelay, 0.0f) * Speed;

	auto EnsureGrowMID = [this, Speed, FadeCm](TObjectPtr<UMaterialInstanceDynamic>& MID, UMaterialInterface* Parent)
	{
		if (!Parent) { MID = nullptr; return; }
		if (!MID || MID->Parent != Parent) MID = UMaterialInstanceDynamic::Create(Parent, this);
		if (!MID) return;
		MID->SetScalarParameterValue(TEXT("VineGrowSpeed"), Speed);
		MID->SetScalarParameterValue(TEXT("VineGrowFade"), FadeCm);
	};
	EnsureGrowMID(VineBranchGrowMID, V.BranchMaterial);
	EnsureGrowMID(VineFlowerGrowMID, V.FlowerMaterial);
	// 叶子那张复用季节 MID —— 同一张上再写一个标量即可，不必多建一个。
	if (VineLeafSeasonMID)
	{
		VineLeafSeasonMID->SetScalarParameterValue(TEXT("VineGrowSpeed"), Speed);
		VineLeafSeasonMID->SetScalarParameterValue(TEXT("VineGrowFade"), FadeCm);
	}

	// 叶 / 花相对枝的延时。**参数名两张材质是同一个**（`instance_growth_nodes` 建的那个），
	// 但它们是两张不同的材质、两个不同的 MID，所以可以各写各的值。
	// ⚠️ 枝那张**不要**写：它没有这个参数（枝的前沿就是基准），写进去只是个没人读的标量，
	// 但会让"这个数到底影响谁"变得不好查。
	if (VineLeafSeasonMID) VineLeafSeasonMID->SetScalarParameterValue(TEXT("VineLeafLag"), LeafLagCm);
	if (VineFlowerGrowMID) VineFlowerGrowMID->SetScalarParameterValue(TEXT("VineLeafLag"), FlowerLagCm);

	VineFlowerComponent->SetInstanceMaterial(VineFlowerGrowMID
		? static_cast<UMaterialInterface*>(VineFlowerGrowMID) : ToRawPtr(V.FlowerMaterial));

	if (VineGpuBuffers.Num() != CSHouseVine::Palette_Num)
	{
		CSShaperSteps::ReleaseOnRenderThread(VineGpuBuffers);
		VineGpuBuffers.SetNum(CSHouseVine::Palette_Num);
		VineHandover.Capacities.Reset();
		bVineBaseMeshReady = false;
	}

	// 基础网格快照只在第一次（或组件被重建后）建一次：它要读 LOD0 顶点、补法线与 UV，
	// 不是每帧该做的事。⚠️ **不能改用 `SetBaseMesh`** —— `ivy_branch` 的切线流是空的，
	// 那条路会把一份零长法线原样搬进快照，画出来是一条黑剪影（见 CSHouseVine::BuildBaseMesh）。
	// 换过网格资产就必须重建快照（见 VineBranchMeshBuiltFrom 的字段注释）。
	if (VineBranchMeshBuiltFrom != V.BranchMesh || VineLeafMeshBuiltFrom != V.LeafMesh || VineFlowerMeshBuiltFrom != V.FlowerMesh)
	{
		bVineBaseMeshReady = false;
	}

	if (!bVineBaseMeshReady)
	{
		FCSGpuMeshCPUData BranchData;
		FCSGpuMeshCPUData LeafData;
		FCSGpuMeshCPUData FlowerData;
		// 长度轴：`ivy_branch` 实测 min=(-50,-86.6,0) max=(100,86.6,100) ⇒ 长度在 +Z；
		// `ivy_leaf` 实测 min=(-33.3,0,-13) max=(33.3,70,8.5) ⇒ 长度在 +Y；
		// `ivy_flower` 实测 min=(-36,-39,0) max=(40,39,38.4) ⇒ 底面在 Z=0、簇沿 +Z 张开，换轴取恒等。
		const bool bBranchOk = CSHouseVine::BuildBaseMesh(V.BranchMesh, 2, BranchData);
		const bool bLeafOk = CSHouseVine::BuildBaseMesh(V.LeafMesh, 1, LeafData);
		const bool bFlowerOk = CSHouseVine::BuildBaseMesh(V.FlowerMesh, 2, FlowerData);
		if (bBranchOk) VineBranchComponent->SetBaseMeshFromGpuData(BranchData);
		if (bLeafOk) VineLeafComponent->SetBaseMeshFromGpuData(LeafData);
		if (bFlowerOk) VineFlowerComponent->SetBaseMeshFromGpuData(FlowerData);
		// 花是**可选**的（网格留空 = 不长花），所以它不进 `bVineBaseMeshReady` 的与 ——
		// 进了的话没配花的墙会连枝带叶一起消失。
		// 管子模式下枝不进实例路，它的基础网格快照建不出来也无所谓。
		bVineBaseMeshReady = (V.bUseTube || bBranchOk) && bLeafOk;
		VineBranchMeshBuiltFrom = bBranchOk ? V.BranchMesh : nullptr;
		VineLeafMeshBuiltFrom = bLeafOk ? V.LeafMesh : nullptr;
		VineFlowerMeshBuiltFrom = bFlowerOk ? V.FlowerMesh : nullptr;

		// 块尺寸从**换轴之后**的快照量，不从资产的包围盒量 —— 两者的轴是不同的。
		auto SetBlock = [](CSShaperSteps::FPaletteBuffers& Buffers, const FCSGpuMeshCPUData& Data, float CrossSection)
		{
			FBox3f Local(ForceInit);
			for (const FVector3f& P : Data.Positions) Local += P;
			const FVector3f Size = Local.IsValid ? Local.GetSize() : FVector3f(1.0f, 1.0f, 1.0f);
			Buffers.BaseSphereCentre = Local.IsValid ? Local.GetCenter() : FVector3f::ZeroVector;
			Buffers.BaseSphereRadius = Local.IsValid ? Local.GetExtent().Size() : 0.0f;
			// z 只放 1/网格长度：记录里的 LengthScale 直接就是**想要的世界长度**（cm），
			// 这样每一段可以有不同的长度，而不必回头改 BlockSize。
			Buffers.BlockSize = FVector3f(
				CrossSection / FMath::Max(Size.X, 1.0f),
				CrossSection / FMath::Max(Size.Y, 1.0f),
				1.0f / FMath::Max(Size.Z, 1.0f));
		};
		if (bBranchOk) SetBlock(VineGpuBuffers[CSHouseVine::Palette_Branch], BranchData, FMath::Max(V.Thickness, 1.0f));
		// 叶片按"长度 = 宽度"等比放：记录的 LengthScale 与 SizeScale 带同一个抖动系数。
		if (bLeafOk) SetBlock(VineGpuBuffers[CSHouseVine::Palette_Leaf], LeafData, FMath::Max(V.LeafSize, 2.0f));
		// 花的截面是**宽度**、长度轴是**高度**，两者由 `FParams::FlowerAspect` 联系起来。
		if (bFlowerOk) SetBlock(VineGpuBuffers[CSHouseVine::Palette_Flower], FlowerData, FMath::Max(V.FlowerSize, 2.0f));
	}

	if (!bVineBaseMeshReady) return CSShaperSteps::EHandoverResult::UpToDate;

	// 容量与交接包围盒归各家算（墙长 / footprint 周长、屋脊高 …）。容量按**配置上限**一次付清、之后永不扩容（零阻塞纪律）：
	// 规划真的排超了就在 kernel 里截断 —— 少画几段藤，远好过在拖动的某一帧上付一次设备同步。
	uint32 MaxRecords = 64;
	FBox LocalBounds(ForceInit);
	Budget(MaxRecords, LocalBounds);
	// 三个调色板同容量。花远少于枝，但 `ReserveCapacity` 是逐调色板同一个下限的接口，
	// 而多留的那一份是 4096 行 × 80 B = 320 KB —— 为省它去开一条"逐调色板容量"的口子，
	// 换来的是一处只在花特别多时才会显形的截断，不划算。
	CSShaperSteps::ReserveCapacity(VineGpuBuffers, MaxRecords);
	LocalBounds = CSShaperSteps::MergeHandoverBounds(LocalBounds, VineHandover, bForceFullRebuild);

	UCSGpuInstancedMeshComponent* Components[CSHouseVine::Palette_Num] =
		{ VineBranchComponent, VineLeafComponent, VineFlowerComponent };
	TArray<CSShaperSteps::FHandoverSource, TInlineAllocator<CSHouseVine::Palette_Num>> Sources;
	for (int32 Index = 0; Index < CSHouseVine::Palette_Num; ++Index)
	{
		// 叶/花的生长动画靠 custom data（枝走管子，不吃这条，但三条一起交省一个分支）。
		// 漏传的症状是材质里的 `Per Instance Custom Data` 恒读 0 ⇒ 叶子一出现就是长成的，
		// 而不报任何错。
		Sources.Add(CSShaperSteps::MakeHandoverSource(Components[Index], VineGpuBuffers[Index], /*bWithCustomData*/ true));
	}
	return CSShaperSteps::HandOverInstanceSources(Sources, LocalBounds, VineHandover);
}

void ACSWallBase::PackVine(const FCSWallVineSettings& V, const CSHouseVine::FParams& Params,
	const TArray<CSHouseVine::FWallStrip>& Strips, CSHouseVine::FPlan& Plan)
{
	// 世界 → 组件。⚠️ **用组件自己的变换求逆**，不用 actor 的：已删的门框旧路混用
	// `GetBuildTransform()`（只取 yaw）与 actor 的完整逆变换，正是状态文件「已知潜伏问题」
	// 里那条 —— 房子一旦被 pitch/roll 或缩放就错位。这里不重复它。
	const FMatrix44f WorldToComponent = FMatrix44f(
		VineBranchComponent->GetComponentTransform().ToInverseMatrixWithScale());

	// SpawnTime：老藤沿用、新藤记当前时刻、消失的藤从表里删掉（整表替换）。
	// ⚠️ 位置在 `Pack` **之前**、且在管子分支**之外** —— 枝（管子）与叶花（实例）
	// 两条路共用同一份相位，同一根藤的枝与叶必须同时长出来。放进管子分支里的后果是
	// 叶子的 SpawnTime 恒为 0（一出现就长成），而管子长得好好的。
	TArray<float> SpawnTimes;
	CSHouseVine::ResolveSpawnTimes(VineStrandHistory, Plan, GetVineClock(), V.GrowSpeed, V.bGrowOnLoad, SpawnTimes);

	// 回填叶/花记录的相位。**必须在 Pack 之前** —— Pack 会把记录拍平上传，之后再改
	// 只是改了一份没人读的 CPU 副本，而画面上叶子的相位恒为 0。
	auto FillSpawn = [&SpawnTimes](TArray<CSHouseVine::FRecord>& Records)
	{
		for (CSHouseVine::FRecord& R : Records)
		{
			R.SpawnTime = SpawnTimes.IsValidIndex(R.StrandIndex) ? SpawnTimes[R.StrandIndex] : 0.0f;
		}
	};
	FillSpawn(Plan.Leaf);
	FillSpawn(Plan.Flower);

	// ⚠️ **管子模式下枝不能再走实例**，否则管子与分段实例同时画（用户实测："连续的管子和
	// 分段似乎同时存在"）。摘掉记录而不是"不画"：`Pack` 的空表分支会 `AddClearUAVPass`
	// 把 counter 清零，而单纯跳过打包会让 counter **停在上一次的值** —— 那正是重影的成因。
	// `PackTubePath` 只读 `Plan.Strands`，不受影响。
	if (V.bUseTube) Plan.Branch.Reset();
	CSHouseVine::Pack(Plan, VineGpuBuffers, WorldToComponent);

	// 枝：折线 → 管子。叶与花仍走上面那两个调色板（2026-09-06 裁决 5）。
	if (V.bUseTube)
	{
		TSharedPtr<CSHouseVine::FTubePath, ESPMode::ThreadSafe> Path =
			MakeShared<CSHouseVine::FTubePath, ESPMode::ThreadSafe>();
		CSHouseVine::PackTubePath(Strips, Plan, Params, V.TubeSubdivide,
			CSHouseVine::TubeCircleScale, SpawnTimes, *Path);
		if (Path->IsEmpty())
		{
			// 一根藤都没有（悬空、或墙太矮）：把组件上的网格撤掉，别让上一次的管子留在画面上。
			// 这与 `Pack` 那边"空表也要走一趟清零 counter"是同一条纪律的两半。
			if (VineTubeComponent) VineTubeComponent->SetGpuMesh(nullptr);
			PendingVineTubePath.Reset();
		}
		else
		{
			SubmitVineTube(Path);
		}
	}
	else
	{
		// 关掉管子模式：撤掉上一次的管子（否则旧管子与分段实例同时画）。
		ClearMeshSlot(VineTubeComponent, VineTubeMesh, PendingVineTubePath);
	}
}

void ACSWallBase::ClearVineRig()
{
	// ⚠️ **撤实例源之前必须先把 counter 清零**（与门框砖那条同源，门框砖已经因此在画面上留过 12 层砖）：
	// 撤掉之后交接缓存被清空，下一次会把同一批 buffer 交回组件。这条路上的窗口是**重新打开藤之后
	// `bVineBaseMeshReady` 为假**那一次 —— 那时交接已经发生，而 `CSHouseVine::Pack`（它自己的空表分支会清零）根本走不到。
	CSShaperSteps::ZeroCounters(VineGpuBuffers);
	ClearMeshSlot(VineTubeComponent, VineTubeMesh, PendingVineTubePath);   // 下次有藤时重建，别让空网格留在组件上
	if (VineBranchComponent) VineBranchComponent->ClearInstanceSourceGPU();
	if (VineLeafComponent) VineLeafComponent->ClearInstanceSourceGPU();
	if (VineFlowerComponent) VineFlowerComponent->ClearInstanceSourceGPU();
	VineHandover.Reset();
}

void ACSWallBase::SubmitVineTube(TSharedPtr<CSHouseVine::FTubePath, ESPMode::ThreadSafe> Path)
{
	if (!Path.IsValid() || !VineTubeComponent) return;
	const FCSWallVineSettings V = GetVineSettings();

	EnsureSlotMesh(VineTubeComponent, VineTubeMesh);   // 归藤管组件所有：组件销毁时它自己还显存
	// 走 MID 而不是材质资产本身：`VineGrowSpeed` 要下推（见 VineBranchGrowMID 的注释）。
	UMaterialInterface* TubeMaterial = VineBranchGrowMID
		? static_cast<UMaterialInterface*>(VineBranchGrowMID) : ToRawPtr(V.BranchMaterial);
	BindMeshSlotMaterials(VineTubeComponent, VineTubeMesh, { TubeMaterial });

	// 在途被拒 ⇒ 只留**最新**那一份（不是排队）。拖尺寸时每 tick 都会来一次，排队的话
	// 松手后要把整段拖动重放一遍；只留最新则最多落后一帧。同基类的网格槽。
	if (ParkIfInFlight(VineTubeMesh, PendingVineTubePath, Path)) return;

	VineTubeComponent->SetGpuMesh(VineTubeMesh);

	CSVineTube::FParams TubeParams;
	TubeParams.ProfileCount = uint32(FMath::Clamp(V.TubeSegments, 3, 24));
	// ⚠️ 与 `PackVine` 里 PackTubePath 传的必须是**同一个** CircleScale：环半径 =
	// 10 * CircleScale * Points[i].w，而那个 .w 正是按这个式子反解出来的。
	TubeParams.CircleScale = CSHouseVine::TubeCircleScale;

	TWeakObjectPtr<ACSWallBase> WeakThis(this);
	const bool bIssued = CSVineTube::BuildTubeIntoMesh(
		VineTubeMesh, Path->Points, Path->Axes, Path->PointMeta, Path->SegmentMeta, Path->Growth, TubeParams,
		[WeakThis](bool /*bBuilt*/)
		{
			if (ACSWallBase* Wall = WeakThis.Get()) Wall->OnVineTubeEditComplete();
		});

	if (!bIssued)
	{
		// 递交失败**不重试**：多半是折线自相矛盾或容量被拒，重试只会每帧再失败一次。
		// 留一行日志，让"有折线却没有藤"这件事在日志里看得见（而不是画面上一片空白）。
		UE_LOG(LogTinyGladeWallBase, Warning,
			TEXT("[TinyGladeWall] %s vine tube submit refused: points=%d segs=%d"),
			*GetName(), Path->Points.Num(), Path->SegmentMeta.Num());
	}
}

void ACSWallBase::OnVineTubeEditComplete()
{
	if (const TSharedPtr<CSHouseVine::FTubePath, ESPMode::ThreadSafe> Next = TakePending(PendingVineTubePath)) SubmitVineTube(Next);
}
