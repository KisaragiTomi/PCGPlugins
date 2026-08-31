#include "CSHouseActor.h"

#include "CSGpuInstancedMeshComponent.h"
#include "CSGpuMeshTypes.h"
#include "CSGroundActor.h"
#include "CSHouseResize.h"
#include "CSHouseRoof.h"
#include "CSHouseSubsystem.h"
#include "Engine/StaticMesh.h"
#include "CSMesh.h"
#include "CSMeshOps.h"
#include "CSMeshRenderComponent.h"
#include "EngineUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Templates/SharedPointer.h"
#if WITH_EDITOR
// 烘焙判据要从 GetMeshDescription(0) 里读顶点色与 UV 组数（DebugBakeFrameBricksSync）。
// unity 构建下这两个恰好被邻居 TU 带进来，漏写只有 -SingleFile 才照得出来（坑表里那条）。
#include "StaticMeshAttributes.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogTinyGladeHouse, Log, All);

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSHouse_ 前缀。

constexpr float CSHouse_UVScale = 200.0f;   // 世界 cm → UV 的平铺周期
constexpr float CSHouse_MinSpring = 15.0f;  // 拱脚竖直段最低高度

// 半圆拱的段数不再写死：按弦高容差自适应，见 CSHouse_ProfileSegments（CSHouseProfile.h）。

/**
 * 按通道字典组一份逐顶点语义色（R = 构件色号, G = 洞 Tag, B = 洞形状 id, A = 保留）。
 *
 * ⚠️ **B 的初值必须是 255（= 这块面板没有洞），不能是 0** —— 字典里 0 是一个**合法**的形状 id
 * （`ECSOpeningShape::Arch`）。屋面板曾经直接 `Writer.Semantic = CSHouse_Semantic(Roof)`
 * 绕过 `SetPanel`，于是整片屋面的 B 恒为 0，按字典读出来正好是"这块屋面上有个拱洞"；
 * 屋面材质不消费这条通道，所以一路静默。默认值放在**安全**那一侧：漏调 `SetPanel` 的写法
 * 至少不会撒谎。真正该做的仍然是走 `SetPanel`（它还负责把裁剪场一起换掉，见下面）。
 */
FVector4f CSHouse_Semantic(ECSHousePart Part, uint8 Tag = 0)
{
	return FVector4f(float(uint8(Part)) / 255.0f, float(Tag) / 255.0f, 1.0f, 0.0f);
}

/** 把三角形逐个写进快照的小写手。位置按 World 变换烘成世界空间（常驻流口径），
 *  法线/切线同旋转；面法线遵守常驻流绕序 cross(B-A, C-A)（CSMeshBuild.h）。 */
struct FCSHouseMeshWriter
{
	FCSGpuMeshCPUData& S;
	FTransform World;

	/** 当前正在写的构件的逐顶点语义色（通道字典见 ACSHouseActor 类注释）。写一组几何前设一次。
	 *  初值走 `CSHouse_Semantic` 而不是零向量：B 的安全值是 255 不是 0，理由逐字见那个函数。 */
	FVector4f Semantic = CSHouse_Semantic(ECSHousePart::Wall);

	/**
	 * 当前面板所属墙面的框架 + 洞的裁剪场。AddTri 据此为每个顶点算 UV1 = q —— 洞由材质
	 * 逐像素 discard 切出来，几何上不挖（Tiny Glade 原版做法，见 CSHouseProfile.h）。
	 * 无洞面板保持 Field.bValid = false，写哨兵 (8, 8)，判据下恒保留。
	 */
	FVector ClipOrigin = FVector::ZeroVector;
	FVector ClipAlong = FVector::ForwardVector;
	FCSOpeningClipField ClipField;

	/** 顶点局部位置 → UV1。S 是沿墙弧长、Z 是墙空间高度，与剖面/判据同一套坐标。 */
	FVector2f ClipUV(const FVector& LocalP) const
	{
		return ClipField.Eval(float(FVector::DotProduct(LocalP - ClipOrigin, ClipAlong)), float(LocalP.Z));
	}

	/** 换一块面板：设墙框架与裁剪场，并把形状 id 写进语义色 B 通道。 */
	void SetPanel(const FVector& Origin, const FVector& Along, const FCSOpeningClipField& Field, ECSHousePart Part, uint8 Tag)
	{
		ClipOrigin = Origin;
		ClipAlong = Along;
		ClipField = Field;
		Semantic = CSHouse_Semantic(Part, Tag);
		// B = 形状 id / 255；255 = 这块面板没有洞（材质据此整块保留）。
		Semantic.Z = float(Field.bValid ? uint8(Field.Shape) : 255) / 255.0f;
	}

	void AddTri(const FVector& A, const FVector& B, const FVector& C, int32 Slot, const FVector2f& UVA, const FVector2f& UVB, const FVector2f& UVC)
	{
		const FVector N = FVector::CrossProduct(B - A, C - A).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
		const FVector T = (B - A).GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);
		const int32 Base = S.Positions.Num();
		const FVector P[3] = { A, B, C };
		const FVector2f UV[3] = { UVA, UVB, UVC };
		for (int32 i = 0; i < 3; ++i)
		{
			S.Positions.Add(FVector3f(World.TransformPosition(P[i])));
			S.Normals.Add(FVector3f(World.TransformVectorNoScale(N)));
			S.Tangents.Add(FVector3f(World.TransformVectorNoScale(T)));
			S.TexCoords().Add(FVector2f(UV[i]));
			// UV1 = 解析裁剪场。逐顶点写，透视校正插值精确还原（q 是 (s, z) 的仿射函数）。
			S.TexCoordChannels[1].Add(ClipUV(P[i]));
			S.Colors.Add(Semantic);
		}
		// 角点 1/2 交换：法线按外法线算（上面的 cross(B-A,C-A)），但索引要按**引擎绕序**写，
		// 否则整栋房子朝里 —— 与地面同一条口径（见 CSGroundActor::BuildSnapshotFromMirror 的注释）。
		S.Indices.Append({ uint32(Base), uint32(Base + 2), uint32(Base + 1) });
		S.TriangleMaterialSlots.Add(Slot);
	}

	/** 平行四边形面：角点 A、A+U、A+U+V、A+V；常驻流法线 = U×V。UV 取 (沿U, 沿V) / 平铺周期。 */
	void AddQuad(const FVector& A, const FVector& U, const FVector& V, int32 Slot)
	{
		const float LU = float(U.Size()) / CSHouse_UVScale;
		const float LV = float(V.Size()) / CSHouse_UVScale;
		AddTri(A, A + U, A + U + V, Slot, { 0, 0 }, { LU, 0 }, { LU, LV });
		AddTri(A, A + U + V, A + V, Slot, { 0, 0 }, { LU, LV }, { 0, LV });
	}

	/**
	 * 实心平行六面体：O 为一角，X/Y/Z 为三条边向量。六面外法线遵守常驻流口径。
	 * 只要求**右手**，即 (X×Y)·Z > 0，**不要求正交** —— 屋面板就是被剪切过的一块
	 * （沿坡向 + 竖直挤出，见 CSHouseRoof_SlabVerticalThickness）。
	 */
	void AddBox(const FVector& O, const FVector& X, const FVector& Y, const FVector& Z, int32 Slot)
	{
		AddQuad(O, Y, X, Slot);              // bottom (-Z)
		AddQuad(O + Z, X, Y, Slot);          // top (+Z)
		AddQuad(O, X, Z, Slot);              // front (-Y)
		AddQuad(O + Y, Z, X, Slot);          // back (+Y)
		AddQuad(O, Z, Y, Slot);              // left (-X)
		AddQuad(O + X, Y, Z, Slot);          // right (+X)
	}

	/**
	 * 凸多边形棱柱：Face 为前脸顶点环（绕序任意，内部按 -Extrude 校正），Extrude 指向体内。
	 *
	 * 收口以后山墙不再是三角形（顶轮廓要抬起一个咬入量、且咬入量在 footprint 边界处归零，
	 * 于是多出两个折点），檐口封口件也是同一形态的棱柱 —— 与其为每种截面各写一份绕序校正，
	 * 不如把它收在一个地方。绕序用**整圈面积向量**判而不是单个三角：截面在极端参数下会退化出
	 * 近零面积的三角（例如墙厚 ≥ 半跨时折点与脊点重合），拿它定符号会整块翻面。
	 */
	void AddPrismPoly(const TArray<FVector>& Face, const FVector& Extrude, int32 Slot)
	{
		const int32 N = Face.Num();
		if (N < 3) return;

		TArray<FVector, TInlineAllocator<8>> P(Face.GetData(), N);
		const FVector OutN = -Extrude.GetSafeNormal();
		FVector Area = FVector::ZeroVector;
		for (int32 i = 1; i + 1 < N; ++i) Area += FVector::CrossProduct(P[i] - P[0], P[i + 1] - P[0]);
		if (FVector::DotProduct(Area, OutN) < 0)
		{
			for (int32 i = 0, j = N - 1; i < j; ++i, --j) Swap(P[i], P[j]);
		}

		auto UVOf = [](const FVector& Q) { return FVector2f(float(Q.X + Q.Y) / CSHouse_UVScale, float(Q.Z) / CSHouse_UVScale); };
		for (int32 i = 1; i + 1 < N; ++i)
		{
			AddTri(P[0], P[i], P[i + 1], Slot, UVOf(P[0]), UVOf(P[i]), UVOf(P[i + 1]));                          // 前脸扇形
			AddTri(P[0] + Extrude, P[i + 1] + Extrude, P[i] + Extrude, Slot, UVOf(P[0]), UVOf(P[i + 1]), UVOf(P[i])); // 后脸（反绕）
		}
		// 侧带：前脸绕 -Extrude 为 CCW 时，边取反向才让 边×Extrude 朝外（否则侧带全朝体内）。
		for (int32 i = 0; i < N; ++i)
		{
			const FVector& E0 = P[(i + 1) % N];
			AddQuad(E0, P[i] - E0, Extrude, Slot);
		}
	}
};

// `FCSHouseEdgeFrame` / `CSHouse_GetEdge` 已上提到 `CSHouseProfile.h`：谓词（`CSHouse_QueryOpening`）
// 与单测也要问"这面墙在哪、有多长"，而墙在哪只能有一个真源。

/**
 * 往截面顶点环里推一个点，与上一个重合就不推。
 *
 * 收口截面的折点是按**参数**列出来的（footprint 边界 / 边界内一个墙厚 / 脊线），极端参数下
 * 会重合：墙厚 ≥ 半跨时"边界内一个墙厚"就落在脊线上。重合顶点会让扇形三角化吐出零面积三角，
 * 那种三角的法线是 GetSafeNormal 的兜底值 —— 不报错，但会在山墙上留一片朝向乱掉的面。
 */
void CSHouse_PushProfileVertex(TArray<FVector>& Ring, const FVector& P)
{
	if (Ring.Num() && FVector::DistSquared(Ring.Last(), P) < 1.0e-4) return;
	Ring.Add(P);
}

/** 墙空间 (沿边弧长 S, 高度 Z) → UV。内外脸、过梁带、窗台带共用同一套参数化。 */
FVector2f CSHouse_WallUV(float SAlong, float ZUp)
{
	return FVector2f(SAlong / CSHouse_UVScale, ZUp / CSHouse_UVScale);
}


uint32 CSHouse_Hash(const TArray<int32>& Values)
{
	return Values.Num() ? FCrc::MemCrc32(Values.GetData(), Values.Num() * sizeof(int32)) : 0;
}

int32 CSHouse_Q(double Value, double Quantum) { return int32(FMath::RoundToInt(Value / Quantum)); }

/**
 * 砖路**墙框架**的哈希，量在 `CSHouseFrame::Scatter` 真正写进去的那个空间（组件空间）里。
 *
 * ⚠️ **少了这一条，拉尺寸时门框砖整段不重排**（2026-08-31 实测定位，疑案 A 的几何那一半）：
 * `RebuildFrame` 的早退门原先只看 `FElement::Path` + 砖参数，而 `FPath` 全是**边局部**量
 * （`CenterS` / `LeftS` / `RightS` / `TotalLen` / `Radius` / `BaseZ` / `TopZ`）。
 * 把 Y 从 400 拉到 800 时，**X 边的长度一个字都没变** ⇒ 那条边上的洞的边局部量逐位相同
 * ⇒ 哈希不变 ⇒ 一次 `Scatter` 都不录；而那面墙本身已经沿 ±Y 走了 200 cm，
 * 砖于是留在上一代的墙面位置上。症状极具误导性：砖数（`GetFrameBrickCount` / GPU 回读）、
 * 三角形数、洞数、零阻塞四条断言**全部照绿**，只有像素能看见 —— 拱圈上一块砖都没有，
 * 而上一代的砖在屋里悬着。纯平移抓不到它（框架与组件一起走，组件空间里逐位不变），
 * 所以已有的"平移零阻塞"三条断言天生测不到这条路。
 *
 * 量在组件空间而不是世界空间，是为了保住"纯平移不重排"这条性质：世界坐标会让每一次
 * 平移都判成"变了"，白录一趟 pass（不阻塞，但也没有必要）。
 */
uint32 CSHouse_HashElementFrames(const TArray<CSHouseFrame::FElement>& Elements, const FMatrix44f& WorldToComponent)
{
	TArray<int32> H;
	H.Reserve(Elements.Num() * 9);
	for (const CSHouseFrame::FElement& E : Elements)
	{
		const FVector3f O(WorldToComponent.TransformPosition(E.Frame.Origin));
		const FVector3f U(WorldToComponent.TransformVector(E.Frame.AxisU));
		const FVector3f N(WorldToComponent.TransformVector(E.Frame.AxisN));
		H.Append({ CSHouse_Q(O.X, 1), CSHouse_Q(O.Y, 1), CSHouse_Q(O.Z, 1),
			CSHouse_Q(U.X, 0.01), CSHouse_Q(U.Y, 0.01), CSHouse_Q(U.Z, 0.01),
			CSHouse_Q(N.X, 0.01), CSHouse_Q(N.Y, 0.01), CSHouse_Q(N.Z, 0.01) });
	}
	return CSHouse_Hash(H);
}

/**
 * openings 表的全序：(边, 沿边位置, 身份)。
 *
 * **末位那个 `SourceId` 不是装饰**：`TArray::Sort` 不稳定，同一 (边, CenterS) 上的两个洞
 * 没有全序时两次重求值可以给出不同的顺序 ⇒ 形状哈希抖动 ⇒ 幂等短路失效。门的 SourceId
 * 恒为全零（子段本来就不会同位），窗的从列表槽位派生 —— 两边都是确定的。
 */
bool CSHouse_OpeningLess(const FCSWallOpening& A, const FCSWallOpening& B)
{
	if (A.EdgeIndex != B.EdgeIndex) return A.EdgeIndex < B.EdgeIndex;
	if (A.CenterS != B.CenterS) return A.CenterS < B.CenterS;
	if (A.SourceId.A != B.SourceId.A) return A.SourceId.A < B.SourceId.A;
	if (A.SourceId.B != B.SourceId.B) return A.SourceId.B < B.SourceId.B;
	if (A.SourceId.C != B.SourceId.C) return A.SourceId.C < B.SourceId.C;
	return A.SourceId.D < B.SourceId.D;
}

// --- 拖尺寸期的稳态化：把"随 FootprintSize 连续变化的量"吸成阶梯 -------------------------
//
// ⚠️ **实现已搬到 `CSShaperSteps::QuantizeUp` / `CSShaperSteps::ReserveCount`**（2026-08-31）：
// 地面那一侧的裙边摆件走的是同一条纪律，留在这个匿名命名空间里就必然被抄成第二份，
// 而两份常数分叉的症状是"房子那边零阻塞、地面这边每帧一次"——两边的断言各自都绿。
// 起因、可陈述的保证、三个常数的依据全部逐字搬过去了，本文件改成直接按全名调。
}

ACSHouseActor::ACSHouseActor()
{
	PillarMeshComponent = CreateDefaultSubobject<UCSMeshRenderComponent>(TEXT("PillarMesh"));
	PillarMeshComponent->SetupAttachment(RootComponent);
}

// -----------------------------------------------------------------------------
// Ground wiring
// -----------------------------------------------------------------------------

void ACSHouseActor::ResolveGroundAndSubscribe()
{
	if (!Ground && GetWorld())
	{
		for (TActorIterator<ACSGroundActor> It(GetWorld()); It; ++It) { Ground = *It; break; }
	}
	if (!Ground) return;
	if (GroundChangedHandle.IsValid()) return;   // 已订阅（换地面场景先 Unsubscribe 再来）
	GroundChangedHandle = Ground->OnGroundChanged.AddUObject(this, &ACSHouseActor::HandleGroundChanged);
}

void ACSHouseActor::UnsubscribeGround()
{
	if (IsValid(Ground) && GroundChangedHandle.IsValid()) Ground->OnGroundChanged.Remove(GroundChangedHandle);
	GroundChangedHandle.Reset();
}

void ACSHouseActor::HandleGroundChanged(ACSGroundActor* /*ChangedGround*/, const FBox& /*ChangedBounds*/)
{
	// v1 直推：不过滤变更盒，无条件重求值——无效唤醒由哈希短路吸收（计划 D3）。
	ReevaluateSite();
}

// -----------------------------------------------------------------------------
// Reevaluate
// -----------------------------------------------------------------------------

double ACSHouseActor::ComputeSeatZ() const
{
	const FVector Loc = GetActorLocation();
	if (!Ground) return Loc.Z;

	// 落座规则（计划 D4）：max 取 footprint 全域——正中冒起的塑形物也要能顶起房子；
	// 绝对式而非增量式，隆起与塌陷天然对称。
	const float Yaw = FMath::DegreesToRadians(GetActorRotation().Yaw);
	const FVector2D AxX(FMath::Cos(Yaw), FMath::Sin(Yaw));
	const FVector2D AxY(-FMath::Sin(Yaw), FMath::Cos(Yaw));
	const double HX = FootprintSize.X * 0.5, HY = FootprintSize.Y * 0.5;
	const double Step = 50.0;   // 与地面镜像默认格距同阶；max 采样对格距不敏感

	double MaxGround = TNumericLimits<double>::Lowest();
	const int32 NX = FMath::Max(1, int32(FMath::CeilToInt(FootprintSize.X / Step)));
	const int32 NY = FMath::Max(1, int32(FMath::CeilToInt(FootprintSize.Y / Step)));
	for (int32 ix = 0; ix <= NX; ++ix)
	{
		for (int32 iy = 0; iy <= NY; ++iy)
		{
			const double LX = -HX + FootprintSize.X * ix / NX;
			const double LY = -HY + FootprintSize.Y * iy / NY;
			const FVector2D P = FVector2D(Loc.X, Loc.Y) + AxX * LX + AxY * LY;
			MaxGround = FMath::Max(MaxGround, double(Ground->SampleHeight(P)));
		}
	}
	if (MaxGround <= TNumericLimits<double>::Lowest() * 0.5) return Loc.Z;
	return MaxGround + HeightOffset;
}

void ACSHouseActor::BuildWindowOpenings(TArray<FCSWallOpening>& OutCandidates) const
{
	OutCandidates.Reset();
	if (!bWindowsEnabled) return;

	OutCandidates.Reserve(Windows.Num());
	for (int32 Index = 0; Index < Windows.Num(); ++Index)
	{
		const FCSHouseWindow& Request = Windows[Index];
		FCSWallOpening Opening;
		Opening.Type = ECSOpeningType::Window;
		Opening.Shape = Request.Shape;
		Opening.EdgeIndex = Request.EdgeIndex;
		Opening.CenterS = Request.CenterS;
		Opening.Width = Request.Width;
		Opening.Z0 = Request.SillZ;
		Opening.Z1 = Request.SillZ + Request.Height;
		// AxisUS / Skew 一律留默认：对普通窗恒为 (0,1) 与 0，它们是给楼梯洞与将来的转角窗
		// 预留的（CSHouseProfile.h 的字段注释写明了）。现在拿来用，转角窗真做时语义会打架。

		// **身份从列表槽位派生**，不是每次重求值现掷一个 GUID：SourceId 同时是
		//   ① 谓词里"自己不与自己冲突"的键、② openings 排序的末位键（同 (边, CenterS) 时的全序）。
		// 现掷的话两次重求值会给同一扇窗不同的身份 ⇒ 排序不稳定 ⇒ 形状哈希抖动 ⇒ 幂等短路失效，
		// 而且不会有任何断言报红（同 S1 那个"槽位当随机源"的坑）。
		Opening.SourceId = FGuid(0x57494E44u /*'WIND'*/, uint32(Index), uint32(Request.EdgeIndex), 0u);
		// Tag 进顶点色 G 通道做悬停高亮。门写的是子段号，窗从 0x80 起编，两者不会撞
		// （D14 的通道字典冻结前，这只是"别互相覆盖"的最小约定，不是最终字典）。
		Opening.Tag = uint8(0x80 | (Index & 0x7F));
		OutCandidates.Add(Opening);
	}
}

FCSOpeningSite ACSHouseActor::MakeOpeningSite() const
{
	FCSOpeningSite Site;
	Site.Footprint = FootprintSize;
	Site.WallThickness = WallThickness;
	Site.WallHeight = WallHeight;
	Site.LintelBand = LintelBand;
	Site.CornerMargin = CornerMargin;
	Site.PierWidth = PierWidth;
	Site.OpeningClearance = OpeningClearance;
	Site.MinSillZ = WindowMinSillZ;
	Site.bPierStyleEnabled = bPierStyleEnabled;
	// 高阈：墩的迟回还没算（`ResolvePierSpans` 在窗过完谓词之后才跑），所以按"有没有可能
	// 被判成墩"保守判。理由完整地写在 `CSHouse_QueryOpening` 上面。
	Site.PierRestoreWidth = FMath::Max(PierStyleRestoreWidth, PierStyleMaxWidth);
	Site.Openings = CurrentOpenings;
	return Site;
}

uint32 ACSHouseActor::ComputeDoors()
{
	// 门与窗**都是派生物**，每轮从权威源重算：门来自道路推导，窗来自 `Windows` 那份显式列表。
	// 只有第三方注入的洞（楼梯穿墙等，当前无人生产）才原样留着 —— 它们没有权威源可以重导出。
	TArray<FCSWallOpening> Kept;
	for (const FCSWallOpening& O : CurrentOpenings)
	{
		if (O.Type != ECSOpeningType::Door && O.Type != ECSOpeningType::Window) Kept.Add(O);
	}
	CurrentOpenings.Reset();
	TMap<uint32, bool> NewOpen;

	const FVector Loc = GetActorLocation();
	const float YawRad = FMath::DegreesToRadians(GetActorRotation().Yaw);
	const FVector2D AxX(FMath::Cos(YawRad), FMath::Sin(YawRad));
	const FVector2D AxY(-FMath::Sin(YawRad), FMath::Cos(YawRad));
	auto ToWorld2D = [&](const FVector2D& L) { return FVector2D(Loc.X, Loc.Y) + AxX * L.X + AxY * L.Y; };

	const float MaxDoorHeight = FMath::Min(DoorHeight, WallHeight - LintelBand);
	if (Ground && MaxDoorHeight > DoorMinWidth * 0.5f)
	{
		for (int32 Edge = 0; Edge < 4; ++Edge)
		{
			const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, FootprintSize, WallThickness);
			float FirstS = 0, Pitch = 0;
			const int32 N = SplitEdgeIntoSlots(F.Len, CornerMargin, DoorPitchTarget, DoorMinWidth, FirstS, Pitch);
			if (N <= 0) continue;

			for (int32 Slot = 0; Slot < N; ++Slot)
			{
				const float S0 = FirstS + Slot * Pitch;
				const int32 Steps = FMath::Max(2, int32(Pitch / DoorSampleStep) + 1);
				int32 Covered = 0;
				float GapMax = 0;
				for (int32 K = 0; K <= Steps; ++K)
				{
					const FVector2D LP = F.Start + F.U * (S0 + Pitch * K / Steps);
					const FVector2D WP = ToWorld2D(LP);
					const FVector2D OutWorld = AxX * (-F.In.X) + AxY * (-F.In.Y);   // 世界系外法线
					const float Road = FMath::Max(
						Ground->SampleRoadWeight(WP + OutWorld * DoorProbeOffset),
						Ground->SampleRoadWeight(WP - OutWorld * DoorProbeOffset));
					if (Road >= DoorOnWeight) ++Covered;
					GapMax = FMath::Max(GapMax, float(Loc.Z - Ground->SampleHeight(WP + OutWorld * DoorProbeOffset)));
				}
				const float Coverage = float(Covered) / (Steps + 1);

				const uint32 Key = (uint32(Edge) << 24) | (uint32(N) << 16) | uint32(Slot);
				const bool bWasOpen = DoorSlotOpen.FindRef(Key);
				bool bOpen = Coverage >= (bWasOpen ? SlotOffCoverage : SlotOnCoverage);

				// 离地连续收窄（D6）：连续量无需滞回，但宽度必须量化后再进哈希。
				const float WidthScale = ComputeDoorWidthScale(GapMax, DoorGapFull, DoorGapZero);
				float Width = FMath::RoundToFloat((Pitch - PierWidth) * WidthScale / DoorWidthQuantum) * DoorWidthQuantum;
				float Height = MaxDoorHeight;
				if (Height - Width * 0.5f < CSHouse_MinSpring)
					Width = FMath::RoundToFloat(2.0f * (Height - CSHouse_MinSpring) / DoorWidthQuantum) * DoorWidthQuantum;
				if (Width < DoorMinWidth) bOpen = false;

				NewOpen.Add(Key, bOpen);
				if (bOpen)
				{
					FCSWallOpening Door;
					Door.Type = ECSOpeningType::Door;
					Door.Shape = ECSOpeningShape::Arch;
					Door.EdgeIndex = Edge;
					Door.CenterS = S0 + Pitch * 0.5f;
					Door.Width = Width;
					Door.Z0 = 0.0f;              // 门恒贴地；窗台高走 Z0 > 0（D8）
					Door.Z1 = Height;
					Door.Tag = uint8(Slot & 0xFF);
					CurrentOpenings.Add(Door);
				}
			}
		}
	}
	DoorSlotOpen = MoveTemp(NewOpen);

	// **让位规则**：门拱优先于特征标记（D6）—— 子段被点亮后，与之相交的窗判为不可行，
	// 避免拱窗互切。所以窗要在门全部落位之后再逐条过谓词。
	TArray<FCSWallOpening> WindowCandidates;
	BuildWindowOpenings(WindowCandidates);
	Kept.Append(WindowCandidates);   // 注入洞（如果有）排在前面，窗跟在后面
	CurrentWindowCount = 0;
	CurrentWindowRejectCount = 0;
	for (const FCSWallOpening& Feature : Kept)
	{
		if (!QueryFeaturePlacement(Feature))
		{
			if (Feature.Type == ECSOpeningType::Window) ++CurrentWindowRejectCount;
			continue;
		}
		// **按序插入**，不是追加到末尾：谓词里"墩跨度不接受窗"那一段靠"相邻两项即同边相邻
		// 两洞"来找跨度，一旦表乱序，下一个候选就会拿隔了一个洞的两端当跨度。
		int32 At = 0;
		while (At < CurrentOpenings.Num() && CSHouse_OpeningLess(CurrentOpenings[At], Feature)) ++At;
		CurrentOpenings.Insert(Feature, At);
		if (Feature.Type == ECSOpeningType::Window) ++CurrentWindowCount;
	}
	// 上面已经保证有序（门按边号/子段号递增加入、其余按序插入），这一下是不变量的兜底。
	// 末位键是 `SourceId`：同一 (边, CenterS) 上的两个洞若没有全序，不稳定排序会让两次
	// 重求值给出不同的顺序 ⇒ 形状哈希抖动 ⇒ 幂等短路失效（而且不会有任何断言报红）。
	CurrentOpenings.Sort([](const FCSWallOpening& A, const FCSWallOpening& B) { return CSHouse_OpeningLess(A, B); });

	// 拱间墩（D6，2026-08-30 实拍裁决）：排好序才谈得上"相邻两洞之间"。
	// 位置有两条硬约束 —— ① 在窗过完谓词之后：墩是最终洞集合的函数，先算就会把被拒的窗算进去；
	// ② 在下面那份形状哈希**之前**：样式决定几何（墩跨度不砌灰泥面板），晚一步就成了
	// "样式翻了、哈希没变 ⇒ 房体不重建"的静默失效。
	ResolvePierSpans();

	// 房体**形状**哈希：几何参数 + 门集合，**不含世界变换**（那份归 ComputePlacementHash）。
	// 纪律：desc 哈希只接受"决定顶点位置或索引的量"——材质/颜色/高亮一律走 D14 的外观通道。
	TArray<int32> H;
	H.Append({ CSHouse_Q(FootprintSize.X, 1), CSHouse_Q(FootprintSize.Y, 1), CSHouse_Q(WallHeight, 1), CSHouse_Q(WallThickness, 0.5),
		CSHouse_Q(RoofPitch, 0.1), CSHouse_Q(RoofOverhang, 1), CSHouse_Q(RoofThickness, 0.5), int32(RidgeAxis) });
	for (const FCSWallOpening& O : CurrentOpenings)
	{
		H.Append({ O.EdgeIndex, int32(O.Shape), CSHouse_Q(O.CenterS, 1), CSHouse_Q(O.Width, DoorWidthQuantum),
			CSHouse_Q(O.Z0, 1), CSHouse_Q(O.Z1, 1), CSHouse_Q(O.Skew, 0.01),
			CSHouse_Q(O.AxisUS.X, 0.01), CSHouse_Q(O.AxisUS.Y, 0.01),
			// StyleFlags 是**决定顶点位置**的量（墩侧的面板格收到洞缘、跨度只从墩顶往上砌），
			// 不是外观通道，所以它必须在这份哈希里。漏掉它 = 迟回翻了但房体不重建。
			int32(O.StyleFlags) });
	}
	H.Append({ CSHouse_Q(OpeningChordTolerance, 0.01) });   // 容差决定分段数 ⇒ 决定索引数
	return CSHouse_Hash(H);
}

void ACSHouseActor::ResolvePierSpans()
{
	// 整表重算（同 DoorSlotOpen）：判据是当前洞集合的纯函数，留着旧键只会让"这条边多开一个拱"
	// 之后的编号错位继承到别的跨度上去。
	TMap<uint32, bool> NewState;
	CurrentPierSpanCount = 0;
	for (FCSWallOpening& Opening : CurrentOpenings) Opening.StyleFlags = 0;

	FCSHousePierStyle Style;
	Style.bEnabled = bPierStyleEnabled;
	Style.MaxWidth = PierStyleMaxWidth;
	Style.RestoreWidth = PierStyleRestoreWidth;

	// CurrentOpenings 进来时已按 (边, CenterS) 排好（ComputeDoors 末尾那一下），所以同边相邻两项
	// 就是相邻两洞 —— 一趟线性扫描即可，不必再分组排序。
	int32 Begin = 0;
	while (Begin < CurrentOpenings.Num())
	{
		int32 End = Begin;
		while (End < CurrentOpenings.Num() && CurrentOpenings[End].EdgeIndex == CurrentOpenings[Begin].EdgeIndex) ++End;

		// 这条边的洞数进 key，与 DoorSlotOpen 把 N 进 key 同一个理由：多开/少开一个拱会把整条边的
		// 跨度重新编号，不把编号基准放进 key 就会把旧跨度的样式误继承给完全不同的一段墙。
		const uint32 Count = uint32(FMath::Min(End - Begin, 0xFF));
		for (int32 Index = Begin; Index + 1 < End; ++Index)
		{
			FCSWallOpening& Left = CurrentOpenings[Index];
			FCSWallOpening& Right = CurrentOpenings[Index + 1];
			float Span = 0.0f, TopZ = 0.0f;
			// 只有"两侧都是落地的拱"才谈得上墩（墩顶 = 起拱线，别的洞型没有这条线）——
			// 判据与理由都在 CSHouse_PierSpanBetween 里，这里不重写。
			if (!CSHouse_PierSpanBetween(Left, Right, Span, TopZ)) continue;

			const uint32 Key = (uint32(Left.EdgeIndex & 0xFF) << 24) | (Count << 16) | uint32((Index - Begin) & 0xFFFF);
			const bool bWasPier = PierSpanIsPier.FindRef(Key);
			const bool bIsPier = CSHouse_SpanIsPier(Style, Span, bWasPier);
			NewState.Add(Key, bIsPier);
			if (!bIsPier) continue;

			// 结论粘在洞上而不是另起一张表：铺墙板的 CSHouse_BuildBodySoup 是纯函数、只吃一份 desc，
			// 而洞是唯一同时流过两边且顺序天然一致的东西（见 FCSWallOpening::StyleFlags 的注释）。
			Left.StyleFlags |= CSHouse_StylePierAfter;
			Right.StyleFlags |= CSHouse_StylePierBefore;
			++CurrentPierSpanCount;
		}
		Begin = End;
	}

	PierSpanIsPier = MoveTemp(NewState);
}

int32 ACSHouseActor::GetOpenDoorCount() const
{
	int32 Count = 0;
	for (const FCSWallOpening& O : CurrentOpenings) if (O.Type == ECSOpeningType::Door) ++Count;
	return Count;
}

bool ACSHouseActor::QueryFeaturePlacement(const FCSWallOpening& Candidate) const
{
	return QueryFeatureReject(Candidate) == ECSFeatureReject::None;
}

ECSFeatureReject ACSHouseActor::QueryFeatureReject(const FCSWallOpening& Candidate) const
{
	// 判据本体在 `CSHouse_QueryOpening`（`CSHouseProfile.h`）—— 这里只负责把房子的属性打包。
	// **抽出去是为了让纯 CPU 单测能调同一条判据**：谓词与几何同维（C1 = 甲）这件事只有拿
	// 「谓词说能放 ⇒ 跑一趟 CSHouse_BuildBodySoup 看洞在不在」去证，才不是在证一份镜像。
	return CSHouse_QueryOpening(MakeOpeningSite(), Candidate);
}

int32 ACSHouseActor::SplitEdgeIntoSlots(float EdgeLength, float CornerMargin, float PitchTarget, float MinWidth,
	float& OutFirstS, float& OutPitch)
{
	OutFirstS = CornerMargin;
	OutPitch = 0.0f;

	const float Usable = EdgeLength - 2.0f * CornerMargin;
	if (Usable < MinWidth || PitchTarget <= 0.0f) return 0;

	// 上限 32 是防呆：拉出一面极长的墙时段数不该无界增长（每段都是一个候选拱 + 一轮采样）。
	const int32 N = FMath::Clamp(FMath::RoundToInt(Usable / PitchTarget), 1, 32);
	OutPitch = Usable / N;
	return N;
}

float ACSHouseActor::ComputeDoorWidthScale(float GapMax, float GapFull, float GapZero)
{
	const float Span = FMath::Max(GapZero - GapFull, 1.0f);   // 参数被填反/相等时退化成硬阈，不除零
	return 1.0f - FMath::Clamp((GapMax - GapFull) / Span, 0.0f, 1.0f);
}

FCSRoofDesc ACSHouseActor::GetRoofDesc() const
{
	FCSRoofDesc Desc;
	Desc.RidgeAxis = RidgeAxis;
	Desc.Footprint = FootprintSize;
	Desc.EaveZ = WallHeight;
	Desc.Pitch = RoofPitch;
	Desc.Overhang = RoofOverhang;
	Desc.Thickness = RoofThickness;
	return Desc;
}

uint32 ACSHouseActor::ComputePlacementHash() const
{
	const FVector Loc = GetActorLocation();
	const TArray<int32> H = { CSHouse_Q(Loc.X, 1), CSHouse_Q(Loc.Y, 1), CSHouse_Q(Loc.Z, 0.5), CSHouse_Q(GetActorRotation().Yaw, 0.1) };
	return CSHouse_Hash(H);
}

FTransform ACSHouseActor::GetBuildTransform() const
{
	return FTransform(FRotator(0, GetActorRotation().Yaw, 0), GetActorLocation());
}

uint32 ACSHouseActor::GetTrackingHash() const
{
	const FVector Loc = GetActorLocation();
	TArray<int32> H = {
		CSHouse_Q(Loc.X, 1), CSHouse_Q(Loc.Y, 1), CSHouse_Q(Loc.Z, 0.5), CSHouse_Q(GetActorRotation().Yaw, 0.1),
		CSHouse_Q(FootprintSize.X, 1), CSHouse_Q(FootprintSize.Y, 1), CSHouse_Q(WallHeight, 1) };

	// **邻居的摆位也算这栋房的输入**（D7 接缝）：接缝是两栋房的纯函数，邻居一动这栋房的几何
	// 就变了 —— 而邻居移动**不发任何委托给这栋房**（`PostEditMove` 只叫醒它自己）。兜底快扫比的
	// 就是这个哈希，不把邻居算进来的症状是"把 B 拖走，A 身上的接缝砖还立在原地"，且没有断言会红。
	//
	// ⚠️ 只收**外接圆够得着**的邻居，不是全部：全收的话任何一栋房动一下就叫醒全场。
	// 粗筛谓词是纯几何的（只由当前摆位决定），所以邻居从远处越过粗筛边界的那一步同样会改哈希
	// —— "靠近才唤醒"不需要任何记忆。
	//
	// ⚠️ 读的是邻居的权威属性，不是它的缓存/派生表；也**不会**调邻居的 `GetTrackingHash()`
	// （那会互相递归）。这条链只有一层深。
	TArray<CSHouseSeam::FHouse> Neighbours;
	GatherSeamNeighbours(Neighbours);
	for (const CSHouseSeam::FHouse& N : Neighbours)
	{
		H.Append({ CSHouse_Q(N.Center.X, 1), CSHouse_Q(N.Center.Y, 1), CSHouse_Q(N.BaseZ, 0.5),
			CSHouse_Q(N.Yaw, 0.1), CSHouse_Q(N.Footprint.X, 1), CSHouse_Q(N.Footprint.Y, 1),
			CSHouse_Q(N.WallHeight, 1) });
	}
	return CSHouse_Hash(H);
}

uint32 ACSHouseActor::ComputePillars(TArray<FVector>& OutPillarCenters, TArray<float>& OutPillarLengths) const
{
	OutPillarCenters.Reset();
	OutPillarLengths.Reset();
	if (!Ground) return 0;

	const FVector Loc = GetActorLocation();
	const float YawRad = FMath::DegreesToRadians(GetActorRotation().Yaw);
	const FVector2D AxX(FMath::Cos(YawRad), FMath::Sin(YawRad));
	const FVector2D AxY(-FMath::Sin(YawRad), FMath::Cos(YawRad));

	// 支撑点：周界（内缩半墙厚，柱子落在墙体正下方），四角 + 每边按间距等分。
	const double HX = FootprintSize.X * 0.5 - WallThickness * 0.5;
	const double HY = FootprintSize.Y * 0.5 - WallThickness * 0.5;
	const FVector2D Corners[4] = { { -HX, -HY }, { HX, -HY }, { HX, HY }, { -HX, HY } };
	TArray<FVector2D> Points;
	for (int32 E = 0; E < 4; ++E)
	{
		const FVector2D A = Corners[E], B = Corners[(E + 1) % 4];
		const float Len = float(FVector2D::Distance(A, B));
		const int32 Count = FMath::Max(1, int32(Len / PillarSpacing));
		for (int32 J = 0; J < Count; ++J) Points.Add(FMath::Lerp(A, B, float(J) / Count));   // 含起角，终角归下一条边
	}

	TArray<int32> H;
	for (const FVector2D& L : Points)
	{
		const FVector2D W = FVector2D(Loc.X, Loc.Y) + AxX * L.X + AxY * L.Y;
		const float Gap = float(Loc.Z - Ground->SampleHeight(W));
		if (Gap <= PillarMinGap) continue;
		const float Length = Gap + PillarEmbed;
		OutPillarCenters.Add(FVector(L.X, L.Y, 0));   // 局部：z=0 为房底
		OutPillarLengths.Add(Length);
		H.Append({ CSHouse_Q(L.X, 1), CSHouse_Q(L.Y, 1), CSHouse_Q(Length, 2) });
	}
	// 柱**形状**哈希：局部布点 + 柱长 + 截面，**不含世界变换**。柱长本来就吃了世界 Z
	// （Gap = 房底 Z − 地面高度），所以平地上纯 XY 平移不会改它 —— 那正是能走 TransformMesh 的情形。
	H.Append({ CSHouse_Q(PillarSize, 0.5), CSHouse_Q(PillarEmbed, 0.5) });
	return CSHouse_Hash(H);
}

void ACSHouseActor::ReevaluateSite()
{
	if (bInReevaluate || IsTemplate() || !GetWorld()) return;
	TGuardValue<bool> Guard(bInReevaluate, true);

	ResolveGroundAndSubscribe();

	// ① 落座：绝对式，升降对称（计划 D4）。
	const double SeatZ = ComputeSeatZ();
	FVector Loc = GetActorLocation();
	if (FMath::Abs(Loc.Z - SeatZ) > 0.5)
	{
		Loc.Z = SeatZ;
		SetActorLocation(Loc);
	}

	// 脊向滞回：拉尺寸让长短轴穿越时不原地翻面（计划 D4）。必须在 ComputeDoors 之前定下来 ——
	// 它进形状哈希，晚一步就会让同一次重求值里"用来生成的脊向"与"记进哈希的脊向"错开一代。
	RidgeAxis = CSHouseRoof_ChooseRidgeAxis(FootprintSize, RidgeAxis, RidgeSwitchRatio);

	const uint32 PlacementHash = ComputePlacementHash();

	// ② 房体（门同时依赖 Colors 与 Heights）。
	//
	// **顺序纪律**：必须先照常算完门、再比 ShapeHash，绝不能"位置没变就跳过算门" —— 门的
	// 存亡由 SampleRoadWeight(世界 XY) 决定、门宽由 GapMax = 房底 Z − SampleHeight(世界 XY)
	// 连续决定，**门集合本来就是世界摆位的函数**。ShapeHash 不是"局部量"，是已把世界采样吸收
	// 进去的派生量。算门很便宜（约 340 次镜像双线性），省它不划算还会出错。
	//
	// 接缝裁剪必须排在门**之前**只有一个理由：它要在同一次重求值里进同一份房体形状哈希。
	// 它与门互不影响 —— 接缝只吃两房的摆位，门只吃道路与落差（裁决二："其它任何内容都是独立的"）。
	const uint32 SeamCutHash = ComputeSeamCuts();
	const uint32 BodyHash = CSHouse_Hash({ int32(ComputeDoors()), int32(SeamCutHash) });
	if (bForceFullRebuild || BodyHash != BodyShapeHash || !GetTinyGladeMesh())
	{
		RebuildBodyMesh();
		BodyShapeHash = BodyHash;
		BodyPlacementHash = PlacementHash;
	}
	else if (PlacementHash != BodyPlacementHash)
	{
		// 只有真送出去了才推进哈希 —— 在途被拒时推进等于把这次移动丢掉。
		if (ApplyBodyPlacement()) BodyPlacementHash = PlacementHash;
	}

	// ③ 柱（独立组件——纯地形变化只走到这，不碰房体）。
	TArray<FVector> Centers;
	TArray<float> Lengths;
	const uint32 PillarHash = ComputePillars(Centers, Lengths);
	if (bForceFullRebuild || PillarHash != PillarShapeHash || (Centers.Num() > 0 && !PillarMesh))
	{
		RebuildPillarMesh(Centers, Lengths);
		PillarShapeHash = PillarHash;
		PillarPlacementHash = PlacementHash;
	}
	else if (PlacementHash != PillarPlacementHash)
	{
		if (ApplyPillarPlacement()) PillarPlacementHash = PlacementHash;
	}
	CurrentPillarCount = Centers.Num();

	// ④ 门框砖：clip 的配套件，洞集合一变就要重排（哈希短路吸收无效唤醒）。
	if (bForceFullRebuild) FrameDescHash = 0;
	RebuildFrame();

	// ⑤ 藤蔓：与门框砖同一档的“墙的配套件”，同样靠哈希短路吸收无效唤醒。
	//    排在门框之后是因为它读 `CurrentOpenings`（避让墙洞），那份表由 ComputeDoors 定。
	if (bForceFullRebuild) VineDescHash = 0;
	RebuildVine();

	// ⑥ 装饰摆件（D12 的**锚点那一半**）：排在最后，因为它锚在前面每一样东西上 ——
	//    门（③ 的 `CurrentOpenings`）、墙脚（②）、檐口/屋脊（屋面 desc），还要读地面镜像
	//    落高与排除道路。TG 的 `populate_autoclutter_regions` 同样排在建筑系统之后。
	if (bForceFullRebuild) DecorDescHash = 0;
	RebuildDecor();

	bForceFullRebuild = false;
}

void ACSHouseActor::RebuildHouse()
{
	bForceFullRebuild = true;
	// 两张迟回表一起清：留着其中一张就成了"门从头判、墩却接着上一代的记忆"，
	// 同一个世界状态会因为上一次的历史给出两种房子。
	DoorSlotOpen.Empty();
	PierSpanIsPier.Empty();
	ReevaluateSite();
}

// -----------------------------------------------------------------------------
// 拉尺寸（D5）：单边推拉的机制入口
// -----------------------------------------------------------------------------

FCSHouseResizeBand ACSHouseActor::MakeResizeBand() const
{
	FCSHouseResizeBand Band;
	Band.Fraction = FootprintBandFraction;
	Band.RidgeSwitchRatio = RidgeSwitchRatio;
	return Band;
}

float ACSHouseActor::PushEdge(int32 EdgeIndex, float Offset, bool bFinished)
{
	const FCSHouseResizeBand Band = MakeResizeBand();

	// 原始诉求累加器：禁带把墙吸在外沿上的那一段里 Applied 恒 0，而生效尺寸又是下一次的起点 ——
	// 不记"手走了多远"的话墙永远跨不过带。累加器认不出当前尺寸就重新同步（别人改过
	// FootprintSize、换了一条边推、或上一次拖动早已结束）。
	if (!CSHouseResize_RawMatches(RawFootprintSize, FootprintSize, EdgeIndex, MinFootprint, Band))
	{
		RawFootprintSize = FootprintSize;
	}

	FVector2D NewSize = FootprintSize;
	FVector NewCentre = GetActorLocation();
	const float Applied = CSHouse_ApplyEdgePush(NewSize, NewCentre, EdgeIndex,
		float(GetActorRotation().Yaw), Offset, MinFootprint, Band, &RawFootprintSize);

	// 尺寸没动就一步都不走：推拉被禁带吸在外沿上时每帧都会走到这里，照常重求值的话
	// 那一整段"墙拖不动"的时间里房子仍在无谓地重算门、砖、藤、摆件。
	if (Applied == 0.0f && !bFinished) return 0.0f;

	if (Applied != 0.0f)
	{
		FootprintSize = NewSize;
		// 中心与尺寸必须**同一帧**落地：只改其中一个，画面上就是"对侧墙也跟着走"，
		// 与计划 D5 那个"拖 1 m 走 2 m"的父子回路缺陷逐像素相同，极易误诊到别处。
		SetActorLocation(NewCentre);
	}

	// 拖动期直接标脏，别等 subsystem 那 0.25 s 的兜底快扫（`MarkHouseDirty` 的第一个客户）。
	if (UWorld* World = GetWorld())
	{
		if (UCSHouseSubsystem* Subsystem = World->GetSubsystem<UCSHouseSubsystem>()) Subsystem->MarkHouseDirty(this);
	}

	// 松手 = gizmo 的 PostEditMove(bFinished=true)：把拖动期为了零阻塞留下的容量 / 包围盒
	// 余量重新收紧，并把累加器归位 —— 留着上一次没用完的诉求，下一次拖动第一帧就会自己跳。
	if (bFinished)
	{
		bForceFullRebuild = true;
		RawFootprintSize = FootprintSize;
	}
	ReevaluateSite();
	return Applied;
}

FVector2D ACSHouseActor::GetFootprintBandRange(int32 EdgeIndex) const
{
	const double Anchor = CSHouseResize_EdgeDrivesX(EdgeIndex) ? FootprintSize.Y : FootprintSize.X;
	const float F = CSHouseResize_EffectiveBandFraction(MakeResizeBand());
	return FVector2D(Anchor * (1.0 - F), Anchor * (1.0 + F));
}

// -----------------------------------------------------------------------------
// Geometry
// -----------------------------------------------------------------------------

void CSHouse_BuildBodySoup(const FCSHouseBodyDesc& Desc, FCSGpuMeshCPUData& S)
{
	FCSHouseMeshWriter Writer{ S, Desc.World };

	const float T = Desc.WallThickness, H = Desc.WallHeight;
	// 材质槽：0 墙面（Masked，按 UV1 逐像素切洞）/ 1 屋顶。
	// 洞缘不占槽位 —— 门框是独立的砖块实例，不在房体网格里。
	constexpr int32 SlotWall = 0, SlotRoof = 1;

	// ---- 四面墙：一串闭合面板。洞不在几何里，由材质按 UV1 的解析判据逐像素 discard 切出 ----
	//
	// 这是 Tiny Glade 原版的开洞方式：CPU 只提供解析参数，洞形在像素阶段成立。洞缘因此是
	// 解析精确曲线（无限分辨率），而不是受弦高容差限制的折线。代价是 discard 只丢像素、
	// 不生成表面 —— 洞缘的厚度断口由门框砖块填满，见下面的说明。
	for (int32 Edge = 0; Edge < 4; ++Edge)
	{
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Desc.Footprint, T);
		const FVector U(F.U.X, F.U.Y, 0), In(F.In.X, F.In.Y, 0), Up(0, 0, 1);
		const FVector Start(F.Start.X, F.Start.Y, 0);

		TArray<FCSWallOpening> Openings;
		for (const FCSWallOpening& O : Desc.Openings) if (O.EdgeIndex == Edge) Openings.Add(O);
		Openings.Sort([](const FCSWallOpening& A, const FCSWallOpening& B) { return A.CenterS < B.CenterS; });

		// D7 接缝在这条边上要抹掉的段。**必须先排序再并掉重叠的**：一块面板只带一个裁剪场，
		// 两刀重叠时后一刀会静默丢失（症状是三栋房挤在一起时少裁一段墙，而砖照常立着）。
		// 并的是**并集包围**，宁可多裁一点也不留一片穿进邻居房间的墙。
		TArray<FCSWallCut> Cuts;
		for (const FCSWallCut& C : Desc.SeamCuts) if (C.EdgeIndex == Edge && C.IsValid()) Cuts.Add(C);
		Cuts.Sort([](const FCSWallCut& A, const FCSWallCut& B) { return A.MinS < B.MinS; });
		for (int32 K = Cuts.Num() - 1; K > 0; --K)
		{
			FCSWallCut& Prev = Cuts[K - 1];
			if (Cuts[K].MinS > Prev.MaxS) continue;
			Prev.MaxS = FMath::Max(Prev.MaxS, Cuts[K].MaxS);
			Prev.TopZ = FMath::Max(Prev.TopZ, Cuts[K].TopZ);
			Prev.BottomZ = FMath::Min(Prev.BottomZ, Cuts[K].BottomZ);
			Cuts.RemoveAt(K);
		}

		// 一块面板：从 Z0 到墙顶（洞在其中被 clip 掉），Z0 以下那截是实心窗台盒。
		//
		// ⚠️ **任何情况下都不许靠"不生成面板"来开洞**（2026-08-30 裁决三，全项目架构不变量）：
		// 想让某一片墙消失，砌出实心盒再用裁剪场把它 discard 掉 —— 墩就是这么做的，见下面。
		auto AddPanel = [&](float SA, float SB, float Z0, const FCSOpeningClipField& Field, uint8 Tag)
		{
			// H − Z0 也要判：Z0 被参数推到墙顶以上时盒子会退化成反向挤出（面全朝里）。
			if (SB - SA < 0.5f || H - Z0 < 0.5f) return;
			Writer.SetPanel(Start, U, Field, ECSHousePart::Wall, Tag);
			Writer.AddBox(Start + U * SA + Up * Z0, U * (SB - SA), In * T, Up * (H - Z0), SlotWall);
			if (Z0 > 0.5f)
			{
				// 窗台：真几何而不是 clip 的下界 —— 判据因此只需两个 float（见 CSHouseProfile.h）。
				Writer.SetPanel(Start, U, FCSOpeningClipField(), ECSHousePart::Wall, 0);
				Writer.AddBox(Start + U * SA, U * (SB - SA), In * T, Up * Z0, SlotWall);
			}
		};

		// 洞与洞之间那段实心墙。接缝把它再切成 [实心 | 接缝裁掉 | 实心 …]，因为**一块面板只带
		// 一个裁剪场** —— 想让一段墙消失就得让它单独成为一块面板（裁决三：绝不靠"不生成面板"开洞）。
		//
		// 墩跨度与接缝落在同一段时取**接缝**那个场：接缝整条 Z 都裁，是墩场（只裁起拱线以下）
		// 的超集，反过来取就会在接缝里留一截起拱线以上的墙。
		auto AddRun = [&](float SA, float SB, bool bPierSpan, float SpanTopZ)
		{
			auto RunField = [&](float A, float B) { return bPierSpan ? CSHouse_PierClipField(A, B, SpanTopZ) : FCSOpeningClipField(); };
			float At = SA;
			for (const FCSWallCut& C : Cuts)
			{
				const float C0 = FMath::Max(C.MinS, SA);
				const float C1 = FMath::Min(C.MaxS, SB);
				if (C1 - C0 < 0.5f) continue;
				AddPanel(At, C0, 0.0f, RunField(At, C0), 0);
				AddPanel(C0, C1, 0.0f, CSHouse_SeamClipField(C0, C1, C.BottomZ, C.TopZ), 0);
				At = C1;
			}
			AddPanel(At, SB, 0.0f, RunField(At, SB), 0);
		};

		float Cursor = 0;
		// 上一个**真砌出来了**的洞。跳过某个洞（装不下）就必须清空它：否则下一轮会拿隔了
		// 一个洞的两端去配墩，把中间那一整块墙当跨度抹掉。宁可多砌灰泥，不许少砌。
		const FCSWallOpening* Prev = nullptr;
		for (const FCSWallOpening& O : Openings)
		{
			if (!O.IsValid()) { Prev = nullptr; continue; }
			float CellMin = 0, CellMax = 0;
			CSHouse_OpeningCell(O, Desc.PierWidth, CellMin, CellMax);
			// 墩侧把格**收回到洞缘再多咬 CSHouse_PierCutMargin 的十分之一**：这一侧要抹掉的
			// 灰泥是格伸进跨度的那半个墩宽（端盖），不是格与格之间那块实心段 —— 默认参数下
			// 拱宽 = 段距 − 墩宽，两格首尾相接，那块段本来就是零宽，只跳过它等于什么都没做
			// （CSHouseProfile.h 记着这条）。
			// 多咬那 0.1 是为了把端盖推到拱判据的**洞内**一侧：正好收到洞缘时 |q.x| 是浮点的
			// 1.0，保不保由 `x * (1/x)` 的舍入决定 —— 一旦被保住，跨度两端就各立起一片贯穿
			// 墙厚的灰泥薄片。被它咬掉的那 1 mm 拱缘由墩跨度那块面板顶上（起拱线以上照常是灰泥）。
			const float PierBite = CSHouse_PierCutMargin * 0.1f;
			if (O.StyleFlags & CSHouse_StylePierBefore) CellMin = O.S0() + PierBite;
			if (O.StyleFlags & CSHouse_StylePierAfter) CellMax = O.S1() - PierBite;
			// 夹进这面墙、且不吃掉前一块面板 —— 洞挨得太近时宁可让墩变窄，也不让面板反向。
			CellMin = FMath::Clamp(CellMin, Cursor, F.Len);
			CellMax = FMath::Clamp(CellMax, CellMin, F.Len);
			// 1 cm 余量：墩侧的格是 S1() − S0() 再各让一点，浮点上不会逐位等于 Width，
			// 而真正的"装不下"是厘米量级的事。
			if (CellMax - CellMin < O.Width - 1.0f) continue;   // 装不下这个洞的面板，这一洞放弃

			// 洞之间的实心段。判为墩的跨度这一块**照样砌成实心盒**，只是起拱线以下整片交给
			// 裁剪场在像素阶段裁掉（裁决三：避免所有真几何洞）—— 观感上起拱线以下就没有"墙"
			// 这个表面了，只剩两侧门樘砖自己站着，正是实拍 Docs/TinyGlade/img/TG_continuous_arches.png
			// 里"墙没了、只剩墩"的那一截。
			float SpanZ0 = 0.0f, SpanWidth = 0.0f;
			const bool bPierSpan = Prev != nullptr
				&& (Prev->StyleFlags & CSHouse_StylePierAfter) != 0
				&& (O.StyleFlags & CSHouse_StylePierBefore) != 0
				&& CSHouse_PierSpanBetween(*Prev, O, SpanWidth, SpanZ0);
			AddRun(Cursor, CellMin, bPierSpan, SpanZ0);
			// **洞面板不吃接缝裁剪**（裁决二明写的退出范围："接缝接受 openings" 不做）：
			// 它自带的裁剪场是洞形，一块面板容不下第二个场，而"两个场怎么合"正是被划出去的那件事。
			// 落进接缝里的门窗因此还是完整的门窗 —— 观感上不对，但它是**声明过的**不对。
			AddPanel(CellMin, CellMax, O.Z0, CSHouse_ComputeClipField(O), O.Tag);
			Cursor = CellMax;
			Prev = &O;
		}
		AddRun(Cursor, F.Len, false, 0.0f);

		// 洞口的厚度**不产扫掠面**（用户裁决）：断口由门框砖块填满 —— 与 TG 同构，
		// 它的拱/楣也是与墙砖并列的真实构件（flags&32：按拱高压扁贴合曲线 + 免拱裁剪），
		// 而不是一圈扫掠出来的内壁。门框沿 CSHouse_ComputeClipField 那条解析洞缘铺砖
		// （`BuildFrameArches` -> `CSHouseFrame::BuildEdgeElements`），与 clip 判据同源。
	}

	// ---- 双坡屋顶：脊沿 RidgeAxis。两块坡板 + 两端山墙 + 两条檐口封口楔形。 ----
	//
	// 三处关键高度（屋脊 / 墙顶 / 檐口外沿）**以及"墙顶该砌到哪"**一律从 CSHouseRoof.h 的
	// 求值器取，不在这里另写方程 —— 将来铺瓦、铺梁、落窗谓词都调同一个函数，脱开就穿帮（计划 D4）。
	//
	// 墙与顶之间过去有三处零余量刀口相切，这一段把三处都改成"实体互穿"：
	//  ① 檐墙那两条**根本没有封口面**：墙顶是平的 Z = H，与屋面底之间留着一条外侧 0、内侧
	//     T·tan(pitch) 的楔形空腔（24 cm 墙 / 35° ≈ 17 cm），只靠墙外棱那条**零宽度相切**封着。
	//     ⚠️ 这条缝在数学上是封住的（EvalZAcross(HalfSpan) ≡ EaveZ），逃不出一条直线射线；
	//     真正的破绽是屋面底那张大四边形**从墙顶外棱的内部横切过去**（T 型接缝），而顶点位置是
	//     float32 世界坐标 —— 封口靠的是两张面在一条线上恰好相等，不是靠实体。所以补的是实体，
	//     断言也落在"缝里有没有实体"上，见 House.EaveSealed。
	//  ② 山墙斜边与屋面底**共面**（零余量）⇒ 发丝亮线。顶轮廓抬起一个咬入量即解。
	//  ③ 两块坡板沿坡向过冲半个板厚"相接"，实为互穿，各自尖端戳出对方顶面 ≈ 7 cm。
	//     改成沿竖直挤出 + 在脊平面上对切收口。
	{
		const FCSRoofDesc Roof = Desc.Roof;
		const float LA = Roof.RidgeLength();
		const float HalfSpan = Roof.HalfSpan();
		const float EaveOut = Roof.EaveOuter();
		const float RidgeH = CSHouseRoof_RidgeZ(Roof);
		const float EaveZ = CSHouseRoof_EaveOuterZ(Roof);
		const float LAtot = LA + 2 * Roof.Overhang;
		// 咬入量的爬升宽度取墙厚，但不能超过半跨（墙厚 ≥ 半跨的退化房子）。
		const float RampW = FMath::Min(T, HalfSpan);
		auto AB = [&Roof](double A, double B, double Z) { return Roof.RidgeToLocal(A, B, Z); };

		// ③ 坡板：截面是**平行四边形**（沿坡向 + 沿竖直挤出，不再沿法线），脊线那条边因此竖直，
		//    两块板在脊平面 across = 0 上正好对切 —— 收口而不是互穿，也不留 V 形豁口。
		//    挤出量直接用 Slope 本身而不是 SlopeDir × |Slope|：Eave + Slope 的跨度分量是
		//    EaveOut + (−EaveOut)，两块板都是**逐位精确**的 0，脊缝不靠容差对齐。
		//    ⚠️ 沿脊方向要按 RidgeToLocal 的**手性**翻一次：脊沿 Y 时它是 (a,b,z) → (b,a,z)，
		//    交换 X/Y 是一次镜像，脊向坐标系里右手的一组基映射到局部就成了左手。
		//    这条过去是错的但看不出来 —— 旧写法把板厚方向取成 cross(U, Slope)，镜像下它指向**下方**，
		//    于是脊沿 Y 的房子屋面板整块挂在屋面底面**以下**（顶面恰好落在墙顶那条线上，从外面看
		//    几乎没区别，实际是板扎进阁楼、山墙斜边与屋面**顶**面共面）。演示房子一直是脊沿 X，没人撞上。
		const float SlabVert = CSHouseRoof_SlabVerticalThickness(Roof);
		const double Handed = FMath::Sign(FVector::CrossProduct(AB(1, 0, 0), AB(0, 1, 0)).Z);
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const float Sigma = Side == 0 ? 1.0f : -1.0f;
			const float Sig = Sigma * float(Handed);                       // 沿脊向的取向，保证 U×Slope 朝上外
			const FVector UDir = AB(-Sig, 0, 0);
			const FVector Eave = AB(Sig * LAtot * 0.5, Sigma * EaveOut, EaveZ);   // 盒起点在 U 的反端
			const FVector Slope = AB(0, -Sigma * EaveOut, RidgeH - EaveZ); // 檐口 → 屋脊
			// ⚠️ **必须走 SetPanel，不许直接写 `Writer.Semantic`。** 那一句既不碰 `.Z`
			// （⇒ 屋面顶点的 B 恒为 0，而 0 是**合法**的形状 id `Arch` ⇒ 按 P2 冻结的通道字典
			// 读出来是"这块屋面上有个拱洞"），也不换裁剪场（⇒ UV1 原样留着**上一块墙面板**的 q，
			// 那块墙有洞时残值是一片真的 clip 场）。屋面材质是 Opaque 常数色、不消费这两条通道，
			// 所以线上一路静默；但裁决六要求通道随网格烘进 StaticMesh，将来任何消费 B/UV1 的
			// 东西（铺瓦、雪线、屋顶天窗）都会读错整片屋面。让屋面去符合字典，字典不动。
			Writer.SetPanel(FVector::ZeroVector, FVector::ForwardVector, FCSOpeningClipField(),
				ECSHousePart::Roof, 0);
			Writer.AddBox(Eave, UDir * LAtot, Slope, FVector(0, 0, SlabVert), SlotRoof);
		}

		// ② 山墙：两端竖直多边形（墙顶线 → 顶轮廓），厚度 = 墙厚，向内挤出。
		//    顶轮廓不再是屋面底本身，而是 CSHouseRoof_SoffitTopZ —— 折点只有三个：
		//    footprint 边界（咬入 0）、边界内一个墙厚（咬满）、脊线。
		TArray<FVector> Face;
		for (int32 End = 0; End < 2; ++End)
		{
			const float SignA = End == 0 ? 1.0f : -1.0f;
			const double A = SignA * LA * 0.5;
			Face.Reset();
			Face.Add(AB(A, -HalfSpan, H));
			Face.Add(AB(A, HalfSpan, H));
			const double Knees[] = { HalfSpan - RampW, 0.0, -(HalfSpan - RampW) };
			for (double B : Knees) CSHouse_PushProfileVertex(Face, AB(A, B, CSHouseRoof_SoffitTopZ(Roof, B, RampW)));
			Writer.SetPanel(FVector::ZeroVector, FVector::ForwardVector, FCSOpeningClipField(), ECSHousePart::Gable, 0);
			Writer.AddPrismPoly(Face, AB(-SignA * T, 0, 0), SlotWall);
		}

		// ① 檐口封口楔形：沿两面檐墙把墙顶补到屋面底（截面 = 外侧高 0、内侧高 T·tan(pitch) + 咬入量
		//    的三角形，斜边**就是** SoffitTopZ 那条线，因为咬入量沿跨度是线性的）。
		//    沿脊两端各让开一个墙厚：那两段的墙顶由山墙棱柱盖着（山墙的跨度范围是整个 span，
		//    连转角那两小块也在内），再叠上去会在山墙**外表面**造出一对共面重叠的可见面（z-fight）。
		//    让开以后剩下的那条接缝是一对背靠背同面 —— 埋在实体内部，与四角那几对同型（无害）。
		//    LA ≤ 2T 时两块山墙棱柱已经首尾相接盖满整条墙顶，不必也不能再铺。
		const float CapHalfLen = LA * 0.5f - T;
		if (CapHalfLen > 0.0f && RampW > 0.0f)
		{
			for (int32 Side = 0; Side < 2; ++Side)
			{
				const float Sigma = Side == 0 ? 1.0f : -1.0f;
				const double Outer = Sigma * HalfSpan;              // 墙外表面，咬入量在这里归零
				const double Inner = Sigma * (HalfSpan - RampW);    // 墙内表面，咬满
				Face.Reset();
				Face.Add(AB(CapHalfLen, Outer, H));
				CSHouse_PushProfileVertex(Face, AB(CapHalfLen, Inner, H));
				CSHouse_PushProfileVertex(Face, AB(CapHalfLen, Inner, CSHouseRoof_SoffitTopZ(Roof, Inner, RampW)));
				// 封口件就是"被斜切的墙顶"，走 Wall 而不是新色号：ECSHousePart 是全项目唯一仲裁点，
				// 顺带：D7 落地之后 **5 号仍然空着** —— 裁决二把接缝定成"只出接缝砖"，砖是
				// `FrameComponent` 里的独立实例、不进房体三角汤，所以它一个构件色号都不消费。
				Writer.SetPanel(FVector::ZeroVector, FVector::ForwardVector, FCSOpeningClipField(), ECSHousePart::Wall, 0);
				Writer.AddPrismPoly(Face, AB(-2.0 * CapHalfLen, 0, 0), SlotWall);
			}
		}
	}

	S.SourceSpace = FCSGpuMeshCPUData::ESpace::World;
	S.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerVertex;
	S.NumTexCoordChannels = 2;   // UV0 贴图 / UV1 解析裁剪场
}

void ACSHouseActor::RebuildBodyMesh()
{
	FCSHouseBodyDesc Desc;
	Desc.Roof = GetRoofDesc();
	Desc.Footprint = FootprintSize;
	Desc.WallThickness = WallThickness;
	Desc.WallHeight = WallHeight;
	Desc.PierWidth = PierWidth;
	Desc.Openings = CurrentOpenings;
	Desc.SeamCuts = CurrentSeamCuts;
	Desc.World = GetBuildTransform();

	FCSGpuMeshCPUData S;
	CSHouse_BuildBodySoup(Desc, S);

	const int32 TriangleCount = S.Indices.Num() / 3;
	BodyBuiltAtTransform = Desc.World;
	SubmitBodyMesh(MakeShared<FCSGpuMeshCPUData, ESPMode::ThreadSafe>(MoveTemp(S)));
	UE_LOG(LogTinyGladeHouse, Log, TEXT("[TinyGladeHouse] %s body rebuilt: tris=%d openings=%d (doors=%d)"),
		*GetName(), TriangleCount, CurrentOpenings.Num(), GetOpenDoorCount());
}

void ACSHouseActor::SubmitBodyMesh(TSharedPtr<FCSGpuMeshCPUData, ESPMode::ThreadSafe> Snapshot)
{
	if (!Snapshot.IsValid() || !TinyGladeMeshComponent) return;

	if (!TinyGladeMesh) TinyGladeMesh = NewObject<UCSMesh>(this);
	UCSMesh* Body = TinyGladeMesh;

	// 材质表要在录图之前就绑好：分段数取自 Materials.Num()，排序 pass 要在同一张图里录进去。
	BindTinyGladeMaterials({ WallMaterial, RoofMaterial });

	// 房体要两组 UV（UV1 传裁剪场）。逐 mesh 的流布局变体，别人不为它付显存；
	// 同组数时这一步一点工作都不做。必须在容量与上传之前 —— 它会重建流集合。
	UCSMeshOps::EnsureTexCoordSets(Body, 2);

	// 在途 → 只留最新态。拖拽期间这条常态命中，被吸收掉的中间帧本来也没人看得见。
	if (Body->IsEditInFlight())
	{
		PendingBodySnapshot = Snapshot;
		return;
	}

	UCSMeshOps::FCSMeshUploadPayload Payload;
	if (!UCSMeshOps::BuildUploadPayload(*Snapshot, Payload, UCSMeshOps::GetTexCoordSets(Body)))
	{
		UE_LOG(LogTinyGladeHouse, Warning, TEXT("[TinyGladeHouse] %s body upload failed (tris=%d)"),
			*GetName(), Snapshot->Indices.Num() / 3);
		return;
	}

	// 两条同步扩容都带早退，稳态下一次 enqueue 都没有；且必须在异步编辑**之前** ——
	// 在途时发起的同步路径虽然 FIFO 有序、结果正确，但会一直阻塞到两者都跑完，
	// 等于把省下的 flush 又还回去。
	// 按 CSShaperSteps::ReserveCount 预留而不是按精确数要：拖尺寸时顶点数是 FootprintSize 的连续函数，
	// 精确要就等于每一帧重新分配 + 拷贝一整份房体（EnsureCapacitySync 只涨不缩，但"涨"本身
	// 就是一次阻塞刷新）。多要的容量不画任何东西 —— 画多少由计数器决定，这正是这套 GPU 网格
	// 的设计前提。
	const int32 NumSlots = FMath::Max(Body->Materials.Num(), 1);
	Body->EnsureCapacitySync(CSShaperSteps::ReserveCount(Payload.VertexCount), CSShaperSteps::ReserveCount(Payload.IndexCount));
	Body->EnsureIndirectDrawCapacitySync(NumSlots);
	TinyGladeMeshComponent->SetGpuMesh(Body);

	// 排序是否真的录进去了，只有渲染线程录图那一刻知道；游戏线程尾巴靠这个共享标志读。
	// 直接读一个 lambda 体内置位的裸 bool 会永远读到 false ⇒ 房子只画一个材质且不报错。
	TSharedPtr<bool, ESPMode::ThreadSafe> Sorted = MakeShared<bool, ESPMode::ThreadSafe>(false);
	TSharedPtr<UCSMeshOps::FCSMeshUploadPayload, ESPMode::ThreadSafe> Owned =
		MakeShared<UCSMeshOps::FCSMeshUploadPayload, ESPMode::ThreadSafe>(MoveTemp(Payload));
	TWeakObjectPtr<ACSHouseActor> WeakThis(this);

	// EditFunc 是 owned（TFunction 移入），它读到的一切也必须由它拥有 —— payload 因此走
	// 共享指针按值捕获，而不是 EditMeshSync 那种"捕获栈上快照的裸指针"（在这里是 use-after-free）。
	const bool bAccepted = Body->EditMeshAsync(
		[Owned, NumSlots, Sorted](FCSMeshEditContext& Context)
		{
			UCSMeshOps::AddCopyFromSnapshotPasses(Context, *Owned);
			*Sorted = UCSMeshOps::AddMaterialSectionPasses(Context, NumSlots);
		},
		[WeakThis, Sorted](bool /*bMeshAlive*/)
		{
			if (ACSHouseActor* House = WeakThis.Get()) House->OnBodyEditComplete(*Sorted);
		});

	if (!bAccepted)
	{
		// 走到这里说明被拒的原因不是"在途"（那条上面已经拦了），而是某种意外状态。
		// 存进 pending 会永远没人来补发（OnComplete 不会触发），所以退回同步一次 ——
		// 宁可付一次 flush，也不能让房子静默地没有几何。
		UE_LOG(LogTinyGladeHouse, Warning, TEXT("[TinyGladeHouse] %s async body edit refused; falling back to a sync upload."), *GetName());
		UCSMeshOps::CopyFromMeshSnapshot(Body, *Snapshot);
		UCSMeshOps::BuildMaterialSections(Body);
	}
}

void ACSHouseActor::OnBodyEditComplete(bool bSorted)
{
	// SetSections 必须落在这条游戏线程尾巴上（异步化时最容易静默失效的一处，详见
	// UCSMeshOps::PublishMaterialSections 的注释）。
	if (bSorted && TinyGladeMesh) UCSMeshOps::PublishMaterialSections(TinyGladeMesh);

	// 最新态合并：在途期间攒下的最后一份目标现在补发。
	if (PendingBodySnapshot.IsValid())
	{
		TSharedPtr<FCSGpuMeshCPUData, ESPMode::ThreadSafe> Next = MoveTemp(PendingBodySnapshot);
		PendingBodySnapshot.Reset();
		SubmitBodyMesh(Next);
		return;
	}

	// 在途期间被推迟的摆位增量在这里补上。没有这一步，"拖动中恰好撞上一次形状重建"的那一帧
	// 位移就永远丢了 —— 而快扫看到的变换没再变，不会来第二次唤醒。增量相对 BuiltAtTransform
	// 算，所以补送的是累计量，不是重放。
	if (ApplyBodyPlacement()) BodyPlacementHash = ComputePlacementHash();
}

bool ACSHouseActor::ApplyBodyPlacement()
{
	UCSMesh* Body = GetTinyGladeMesh();
	if (!Body) return false;

	// 常驻流是世界空间、渲染组件用绝对变换 ⇒ SetActorLocation 不会带动已生成的几何，
	// 得自己把它搬过去。变换 pass 同时改 Positions 与 Tangents（逆转置法线）并变换
	// WorldBounds，平移与 yaw 都支持。
	//
	// UE 的合成口径是 "C = A * B 先 A 后 B"，所以增量 = 先撤旧变换、再上新变换；写反了
	// 房子会在远离原点处飞走（旋转分量作用在未撤销的世界坐标上）。
	const FTransform NewWorld = GetBuildTransform();
	const FTransform Delta = BodyBuiltAtTransform.Inverse() * NewWorld;
	if (Delta.Equals(FTransform::Identity, 1.0e-4)) return true;   // 已经在位
	if (Body->IsEditInFlight()) return false;                      // 在途：留给 OnBodyEditComplete 重试

	TWeakObjectPtr<ACSHouseActor> WeakThis(this);
	const bool bAccepted = Body->EditMeshAsync(
		[Delta](FCSMeshEditContext& Context) { UCSMeshOps::AddTransformPasses(Context, Delta); },
		// 变换只改顶点、不动索引与材质分段，所以尾巴不发布分段表（bSorted = false）。
		[WeakThis](bool /*bMeshAlive*/) { if (ACSHouseActor* House = WeakThis.Get()) House->OnBodyEditComplete(false); });
	if (!bAccepted) return false;

	BodyBuiltAtTransform = NewWorld;
	return true;
}

void ACSHouseActor::RebuildPillarMesh(const TArray<FVector>& Centers, const TArray<float>& Lengths)
{
	if (!PillarMeshComponent) return;
	if (Centers.IsEmpty())
	{
		PillarMeshComponent->SetGpuMesh(nullptr);
		PillarMesh = nullptr;   // 下次有柱时重建，别让摆位快路径去搬一份空网格
		UE_LOG(LogTinyGladeHouse, Log, TEXT("[TinyGladeHouse] %s pillars cleared"), *GetName());
		return;
	}

	FCSGpuMeshCPUData S;
	const FTransform BuildTransform = GetBuildTransform();
	FCSHouseMeshWriter Writer{ S, BuildTransform };
	// 柱子也走 `SetPanel` —— 写法只留一种，下一个人就不会再把 B 通道漏成 0（= 合法形状 id
	// `Arch`）。这里的写手是新的，裁剪场本来就是无洞哨兵，所以这一句纯粹是把口径钉死。
	Writer.SetPanel(FVector::ZeroVector, FVector::ForwardVector, FCSOpeningClipField(),
		ECSHousePart::Pillar, 0);
	for (int32 i = 0; i < Centers.Num(); ++i)
	{
		const FVector C = Centers[i];
		const float Len = Lengths[i];
		Writer.AddBox(FVector(C.X - PillarSize * 0.5f, C.Y - PillarSize * 0.5f, -Len),
			FVector(PillarSize, 0, 0), FVector(0, PillarSize, 0), FVector(0, 0, Len), 0);
	}
	S.SourceSpace = FCSGpuMeshCPUData::ESpace::World;
	S.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerVertex;

	const int32 TriangleCount = S.Indices.Num() / 3;
	PillarBuiltAtTransform = BuildTransform;
	SubmitPillarMesh(MakeShared<FCSGpuMeshCPUData, ESPMode::ThreadSafe>(MoveTemp(S)));
	UE_LOG(LogTinyGladeHouse, Log, TEXT("[TinyGladeHouse] %s pillars rebuilt: count=%d tris=%d"), *GetName(), Centers.Num(), TriangleCount);
}

void ACSHouseActor::SubmitPillarMesh(TSharedPtr<FCSGpuMeshCPUData, ESPMode::ThreadSafe> Snapshot)
{
	if (!Snapshot.IsValid() || !PillarMeshComponent) return;

	// 柱子走独立网格（基类上传管道只服务主网格——参数化改造是计划 D9 的后续项，这里内联同一套步骤）。
	if (!PillarMesh) PillarMesh = NewObject<UCSMesh>(this);
	PillarMeshComponent->MeshMaterial = PillarMaterial;
	PillarMesh->SetMaterial(0, PillarMaterial);

	if (PillarMesh->IsEditInFlight())
	{
		PendingPillarSnapshot = Snapshot;
		return;
	}

	UCSMeshOps::FCSMeshUploadPayload Payload;
	if (!UCSMeshOps::BuildUploadPayload(*Snapshot, Payload)) return;
	// 同房体：柱数会随周界（=FootprintSize）跳变，按精确数要就是每次跳变一次阻塞重分配。
	PillarMesh->EnsureCapacitySync(CSShaperSteps::ReserveCount(Payload.VertexCount), CSShaperSteps::ReserveCount(Payload.IndexCount));
	PillarMeshComponent->SetGpuMesh(PillarMesh);

	// 柱子只有一个材质槽，不需要排序分段 —— 一次上传就是全部工作。
	TSharedPtr<UCSMeshOps::FCSMeshUploadPayload, ESPMode::ThreadSafe> Owned =
		MakeShared<UCSMeshOps::FCSMeshUploadPayload, ESPMode::ThreadSafe>(MoveTemp(Payload));
	TWeakObjectPtr<ACSHouseActor> WeakThis(this);

	const bool bAccepted = PillarMesh->EditMeshAsync(
		[Owned](FCSMeshEditContext& Context) { UCSMeshOps::AddCopyFromSnapshotPasses(Context, *Owned); },
		[WeakThis](bool /*bMeshAlive*/)
		{
			if (ACSHouseActor* House = WeakThis.Get()) House->OnPillarEditComplete();
		});

	if (!bAccepted)
	{
		UE_LOG(LogTinyGladeHouse, Warning, TEXT("[TinyGladeHouse] %s async pillar edit refused; falling back to a sync upload."), *GetName());
		UCSMeshOps::CopyFromMeshSnapshot(PillarMesh, *Snapshot);
	}
}

void ACSHouseActor::OnPillarEditComplete()
{
	if (PendingPillarSnapshot.IsValid())
	{
		TSharedPtr<FCSGpuMeshCPUData, ESPMode::ThreadSafe> Next = MoveTemp(PendingPillarSnapshot);
		PendingPillarSnapshot.Reset();
		SubmitPillarMesh(Next);
		return;
	}
	if (ApplyPillarPlacement()) PillarPlacementHash = ComputePlacementHash();
}

bool ACSHouseActor::ApplyPillarPlacement()
{
	if (!PillarMesh) return false;

	const FTransform NewWorld = GetBuildTransform();
	const FTransform Delta = PillarBuiltAtTransform.Inverse() * NewWorld;
	if (Delta.Equals(FTransform::Identity, 1.0e-4)) return true;
	if (PillarMesh->IsEditInFlight()) return false;

	TWeakObjectPtr<ACSHouseActor> WeakThis(this);
	const bool bAccepted = PillarMesh->EditMeshAsync(
		[Delta](FCSMeshEditContext& Context) { UCSMeshOps::AddTransformPasses(Context, Delta); },
		[WeakThis](bool /*bMeshAlive*/) { if (ACSHouseActor* House = WeakThis.Get()) House->OnPillarEditComplete(); });
	if (!bAccepted) return false;

	PillarBuiltAtTransform = NewWorld;
	return true;
}

// -----------------------------------------------------------------------------
// Frame（门框砖）
// -----------------------------------------------------------------------------

void ACSHouseActor::EnsureFrameComponent()
{
	// 蓝图 actor 重跑构造脚本会把实例组件销毁，指针会失效 —— 先判再补。
	if (!IsValid(FrameComponent))
	{
		FrameComponent = NewObject<UCSGpuInstancedMeshComponent>(this, NAME_None, RF_Transient);
		FrameComponent->SetupAttachment(RootComponent);
		FrameComponent->RegisterComponent();   // 未注册时任何变更都会释放 GPU 网格，必须先注册再喂
	}
	FrameComponent->InstanceMaterial = FrameMaterial;
	FrameComponent->SetBaseMesh(FrameBrickMesh);   // 同一张网格时内部直接早退

	if (FrameGpuBuffers.Num() != 1)
	{
		CSShaperSteps::ReleaseOnRenderThread(FrameGpuBuffers);
		FrameGpuBuffers.SetNum(1);
		FrameHandedCapacities.Reset();
	}

	// 剔除球与三轴块尺寸都从基础网格的包围盒推：TG 的 brick 是 100³ 居中盒，所以
	// BlockSize = 想要的尺寸 / 网格自身尺寸，最终实例尺寸才等于我们写的那几个 cm 值。
	const FBox Local = FrameBrickMesh ? FrameBrickMesh->GetBoundingBox() : FBox(ForceInit);
	FrameGpuBuffers[0].BaseSphereCentre = Local.IsValid ? FVector3f(Local.GetCenter()) : FVector3f::ZeroVector;
	FrameGpuBuffers[0].BaseSphereRadius = Local.IsValid ? float(Local.GetExtent().Size()) : 0.0f;

	const FVector MeshSize = Local.IsValid ? Local.GetSize() : FVector(1.0);
	const float Thickness = FrameBrickThickness > 0.5f ? FrameBrickThickness : WallThickness;
	// 长度轴乘胀大系数（TG 那一步，理由与轴的对位见 FrameBrickBloat 的字段注释）：砖心由
	// `CSHouseFrame::SolveRun` 按未胀大的 FrameBrickLength 定下、这里只放大**渲染尺寸**，
	// 相邻砖因此必然互相穿插。别改成把系数折进砖长 —— 那会连砖数和位置一起变，就不是"胀大"
	// 而是"砖变长"，块数跳变照样露缝。
	const float Bloat = FMath::Max(FrameBrickBloat, 1.0f);
	FrameGpuBuffers[0].BlockSize = FVector3f(
		float(FrameBrickDepth / FMath::Max(MeshSize.X, 1.0)),
		float(FrameBrickLength * Bloat / FMath::Max(MeshSize.Y, 1.0)),
		float(Thickness / FMath::Max(MeshSize.Z, 1.0)));

	if (!FrameBrickMesh || !FrameComponent) return;

	// **容量与实例源交接都在这里一次付清** —— 两者都是阻塞的（前者要在渲染线程分配，
	// 后者内部走 SetStreamLayoutSync + 立刻重建 render state）。留给 RebuildFrame 去做的话，
	// 它们会落在"画路的某一笔"上：那一笔恰好是第一次长出砖、或砖数第一次超过容量的那一笔，
	// 取决于用户画到哪里，既掉帧又难复现。这里做完，交互期的 RebuildFrame 就只剩录 pass。
	CSShaperSteps::ReserveCapacity(FrameGpuBuffers, uint32(FMath::Max(FrameReserveCapacity, 64)));

	// 实例源的包围盒是**组件空间**的保守盒，且必须自己把基础网格的尺寸算进去（packed 路径不代劳）。
	//
	// 量化 + 只涨不缩（理由与保证见 CSShaperSteps::QuantizeUp 上面那段）：原来这两个量直接就是
	// FootprintSize / WallHeight 的连续函数，而下面 bNeedHandover 的阈值是 1 cm ——
	// 拖尺寸时每帧都判"包围盒变了"，于是每帧重走一次阻塞的 SetInstanceSourceGPU。
	// 全量重建时不并旧盒，让它重新收紧（否则一次误拉大就永久留着保守盒）。
	const double Reach = CSShaperSteps::QuantizeUp(FMath::Max(FootprintSize.X, FootprintSize.Y) * 0.6 + FrameBrickLength);
	const double Top = CSShaperSteps::QuantizeUp(WallHeight + FrameBrickLength);
	FBox LocalBounds(FVector(-Reach, -Reach, -FrameBrickLength), FVector(Reach, Reach, Top));

	// ⚠️ 这个盒是按 footprint 在**构建空间**（`GetBuildTransform()`，只取 yaw）里量的，而实例
	// 原点存在**组件空间**里 —— 两者只有在房子没有 pitch/roll/缩放时才重合。状态文件
	// 「已知潜伏问题」那条说的就是这个不对称（裁决一要求"按同一个变换口径写死"）：
	// 盒子跟着做一次 构建空间 → 组件空间 的映射，剩下的两处口径就自洽了。
	// 只有 yaw 时这一步是恒等变换，逐位不改现状；也因此拖动/旋转期不会多出一次交接。
	const FTransform BuildToComponent = GetBuildTransform() * FrameComponent->GetComponentTransform().Inverse();
	if (!BuildToComponent.Equals(FTransform::Identity, 1.0e-4)) LocalBounds = LocalBounds.TransformBy(BuildToComponent);

	if (!bForceFullRebuild && FrameHandedLocalBounds.IsValid) LocalBounds += FrameHandedLocalBounds;

	const bool bNeedHandover = FrameHandedCapacities.Num() != 1
		|| FrameHandedCapacities[0] != FrameGpuBuffers[0].Capacity
		|| !FrameHandedLocalBounds.IsValid
		|| !FrameHandedLocalBounds.Min.Equals(LocalBounds.Min, 1.0)
		|| !FrameHandedLocalBounds.Max.Equals(LocalBounds.Max, 1.0)
		// 蓝图重跑构造脚本会销毁并重建实例组件：新组件身上没有实例源，缓存说"已交接"就会
		// 永远画不出东西 —— 拿组件自己的状态兜底。
		|| !FrameComponent->HasInstanceSourceGPU();
	if (!bNeedHandover || !FrameGpuBuffers[0].IsValid()) return;

	FCSGpuInstanceSourceGPU Source;
	Source.PackedInstances = FrameGpuBuffers[0].PackedInstances;   // 保留自己的引用，重散布还要用
	Source.Counter = FrameGpuBuffers[0].Counter;
	Source.Capacity = FrameGpuBuffers[0].Capacity;
	Source.LocalBounds = LocalBounds;
	FrameComponent->SetInstanceSourceGPU(Source);

	FrameHandedCapacities.SetNumUninitialized(1);
	FrameHandedCapacities[0] = FrameGpuBuffers[0].Capacity;
	FrameHandedLocalBounds = LocalBounds;
}

uint32 ACSHouseActor::BuildFrameArches(TArray<CSHouseFrame::FElement>& OutElements, int32& OutBrickCount) const
{
	OutElements.Reset();
	OutBrickCount = 0;
	if (!bFrameEnabled || !FrameBrickMesh || CurrentOpenings.IsEmpty()) return 0;

	const FTransform World = GetBuildTransform();
	const float T = WallThickness;

	CSHouseFrame::FBrickParams Params;
	Params.Length = FMath::Max(FrameBrickLength, 1.0f);
	Params.Gap = FMath::Max(FrameBrickGap, 0.0f);
	// **容量恒定**：注册期的 `ReserveCapacity` 已经一次付清，这里只按它截断、绝不扩容。
	// 门框这条路上因此没有任何一次可扩容调用 —— 那正是"交互热路径零设备同步"这条纪律要的：
	// 扩容是阻塞刷新，落在用户恰好画到的那一笔上。
	Params.MaxBricks = FMath::Max(FrameReserveCapacity, 64);

	// `CurrentOpenings` 进来时已按 (边, CenterS) 排好（`ComputeDoors` 末尾那一下），同一条边的
	// 洞因此是**连续片段**；墩的样式位（`ResolvePierSpans`）也建立在同一个顺序上。别在这里重排。
	int32 Begin = 0;
	while (Begin < CurrentOpenings.Num())
	{
		int32 End = Begin;
		while (End < CurrentOpenings.Num() && CurrentOpenings[End].EdgeIndex == CurrentOpenings[Begin].EdgeIndex) ++End;

		// **与房体面板同一份 `CSHouse_GetEdge`**：墙在哪儿只能有一个真源（同 BuildVineStrips）。
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(CurrentOpenings[Begin].EdgeIndex, FootprintSize, T);
		// 砖路走**墙厚正中**：砖的 +Z 尺寸 = 墙厚时，正好把断口两侧都封住（同旧路）。
		const FVector Mid(F.Start.X + F.In.X * (T * 0.5), F.Start.Y + F.In.Y * (T * 0.5), 0.0);

		CSHouseFrame::FWallFrame Frame;
		Frame.Origin = FVector3f(World.TransformPosition(Mid));
		Frame.AxisU = FVector3f(World.TransformVectorNoScale(FVector(F.U.X, F.U.Y, 0.0))).GetSafeNormal();
		Frame.AxisV = FVector3f(World.TransformVectorNoScale(FVector::UpVector)).GetSafeNormal();
		Frame.AxisN = (-FVector3f(World.TransformVectorNoScale(FVector(F.In.X, F.In.Y, 0.0)))).GetSafeNormal();

		OutBrickCount += CSHouseFrame::BuildEdgeElements(Frame,
			MakeArrayView(CurrentOpenings.GetData() + Begin, End - Begin), Params, OutElements);
		Begin = End;
	}

	// 截断是**有意**的（容量恒定 ⇒ 不扩容 ⇒ 不阻塞），但必须出声：不然"墙太长/砖太小"时
	// 最后几条砖路会静默地少几块砖，看着像洞缘没砌完。抬 `FrameReserveCapacity` 即可。
	if (OutBrickCount >= Params.MaxBricks)
	{
		UE_LOG(LogTinyGladeHouse, Warning,
			TEXT("[TinyGladeHouse] %s frame hit the constant capacity (%d bricks). Raise FrameReserveCapacity."),
			*GetName(), Params.MaxBricks);
	}

	// desc 哈希 = "会改变砖摆在哪儿的一切"。逐路只需要标量：砖数、弧长、铺装缩放、路的形状，
	// 逐砖的位置是它们的纯函数（正是解析推导的定义），所以不必、也无从逐砖入哈希。
	TArray<int32> Hash;
	for (const CSHouseFrame::FElement& E : OutElements)
	{
		Hash.Append({ E.BrickCount, CSHouse_Q(E.Path.TotalLen(), 1), CSHouse_Q(E.LayoutScale, 0.01),
			CSHouse_Q(E.Path.CenterS, 1), CSHouse_Q(E.Path.Radius, 1),
			CSHouse_Q(E.Path.BaseZ, 1), CSHouse_Q(E.Path.TopZ, 1),
			CSHouse_Q(E.Path.LeftS, 1), CSHouse_Q(E.Path.RightS, 1),
			int32(E.Path.MidKind) | (E.Path.bLeftJamb ? 0x10 : 0) | (E.Path.bRightJamb ? 0x20 : 0)
			// 窗台底边那一段也决定砖摆在哪（它改变 TotalLen ⇒ 改变砖数与铺装缩放），必须入哈希。
			| (E.Path.bSill ? 0x40 : 0) });
	}
	// 胀大系数与三轴尺寸只改 `BlockSize`、一条路都不改，而 `RebuildFrame` 的早退门看的就是
	// 这个哈希 —— 不把它们算进来，改了系数就只会静默无效（与 D14 开篇 FrameMaterial 同型）。
	// `FrameSeed` 在单条目 palette 下已经不影响任何东西（`SolveRun` 里没有随机），留着是为了
	// 将来加 palette 时不会静默跳过重建。
	Hash.Append({ CSHouse_Q(FrameBrickLength, 0.5), CSHouse_Q(FrameBrickDepth, 0.5),
		CSHouse_Q(FrameBrickThickness, 0.5), CSHouse_Q(FrameBrickGap, 0.1),
		CSHouse_Q(FrameBrickBloat, 0.001), FrameSeed });
	return CSHouse_Hash(Hash);
}

void ACSHouseActor::RebuildFrame()
{
	EnsureFrameComponent();

	TArray<CSHouseFrame::FElement> Elements;
	int32 BrickCount = 0;
	// 门框砖先、接缝砖后：全局砖序号跨两者连续（整栋房子一个 dispatch），而**次序是承重的** ——
	// 反过来的话每开一扇门都会把所有接缝砖的槽位推一格，从而推掉它们的逐实例随机数。
	// （接缝砖的随机数已经从接缝身份派生、与槽位无关，但门框砖那边仍然是槽位。）
	const uint32 ArchHash = BuildFrameArches(Elements, BrickCount);
	const uint32 SeamHash = BuildSeamBricks(Elements, BrickCount);
	// 世界 → 组件。⚠️ **用组件自己的变换求逆**，不用 actor 的：已删的旧路混用
	// `GetBuildTransform()`（只取 yaw）与 `GetActorTransform().ToInverseMatrixWithScale()`
	// （完整变换），正是状态文件「已知潜伏问题」里那条不对称。这里与藤蔓/摆件同一个口径，
	// 三条路从此只有一份写法。**必须在哈希之前算**：早退门要拿它把墙框架量到同一个空间里。
	const FMatrix44f WorldToComponent = IsValid(FrameComponent)
		? FMatrix44f(FrameComponent->GetComponentTransform().ToInverseMatrixWithScale())
		: FMatrix44f::Identity;
	// 墙框架也进哈希：`FPath` 全是边局部量，长边不变而房子往垂直方向拉时它一个字都不变，
	// 而砖的组件空间位置已经变了。理由与实测症状见 `CSHouse_HashElementFrames` 的注释。
	const uint32 FrameHash = CSHouse_HashElementFrames(Elements, WorldToComponent);

	// ⚠️ **三个都是 0（这栋房一块砖都没有）时合出来的必须还是 0**，不能直接 `CSHouse_Hash({0,0,0})`
	// —— 那是一个非零常数，于是"没有砖"的房子每次重求值都判成"变了"：走一趟
	// `ClearInstanceSourceGPU()`（它把 `FrameHandedCapacities` 清空），下一次 `EnsureFrameComponent`
	// 就得重走阻塞的 `SetInstanceSourceGPU`。**实测代价是每轮 2 次阻塞刷新**，四条零阻塞断言
	// （画一笔 / 拖带柱的房子 / 带藤拖 / 带摆件拖 —— 全是没有门因而没有砖的那栋）当场从 0 变成 2。
	// 有砖的房子看不见这条，因为它的哈希本来就非零。
	// （砖表空时 `FrameHash` 恒为 0 —— `CSHouse_Hash` 对空表返回 0 —— 所以只判前两个就够，
	// 但仍把它写进合并里，免得将来有人改了空表约定而这里静默失配。）
	const uint32 NewHash = (ArchHash == 0 && SeamHash == 0)
		? 0u : CSHouse_Hash({ int32(ArchHash), int32(SeamHash), int32(FrameHash) });

	bool bBuffersReady = FrameGpuBuffers.Num() == 1 && FrameHandedCapacities.Num() == 1;
	for (const CSShaperSteps::FPaletteBuffers& Buffers : FrameGpuBuffers) bBuffersReady &= Buffers.IsValid();
	if (NewHash == FrameDescHash && CurrentFrameBrickCount == BrickCount && (BrickCount == 0 || bBuffersReady)) return;

	FrameDescHash = NewHash;
	CurrentFrameBrickCount = BrickCount;
	// 越过早退门 = 这一趟砖真的会被重排（下面两条分支都会写 GPU）。判据见 GetFrameScatterCount。
	++FrameScatterCount;

	if (BrickCount == 0)
	{
		// ⚠️ **撤实例源之前必须先把 counter 清零。**
		//
		// 撤掉之后 `FrameHandedCapacities` 被清空，于是**下一轮 `EnsureFrameComponent` 会把同一批
		// buffer 重新交接回去** —— 而那批 buffer 的 counter 还留着上一次的砖数，组件照着它又画了
		// 一遍上一代的实例。症状是"砖数已经是 0 了、画面上砖还立着"，而
		// `GetFrameBrickCount()` / 三角形数 / 零阻塞**四条断言全部照绿**。
		//
		// 实测现场（2026-08-31，D7 出图抓到）：把 `bSeamEnabled` 关掉之后，两根接缝砖柱在
		// `seam_off_corner.png` 里原样立着，12 层砖一块不少。这条不是接缝引入的 —— 门框砖
		// 走的是同一段代码（画一笔路开拱、再擦掉路），只是从来没人给那个状态出过图。
		//
		// `CSHouseFrame::Scatter` 的空表分支就是干这个的，它自己的注释早就预言过这个症状
		// （"一块砖都没有时必须**显式清零**……只在从有到无那一次出现"）—— 只是从没被调到过。
		if (IsValid(FrameComponent) && FrameGpuBuffers.Num() == 1 && FrameGpuBuffers[0].IsValid())
		{
			CSHouseFrame::Scatter(Elements, FrameGpuBuffers, WorldToComponent);
		}
		if (FrameComponent) FrameComponent->ClearInstanceSourceGPU();
		FrameHandedCapacities.Reset();
		FrameHandedLocalBounds = FBox(ForceInit);
		return;
	}

	if (!IsValid(FrameComponent)) return;

	// **一个字节都不分配**：砖数已经在 BuildFrameArches 里按常驻容量截断，扩容那一次阻塞
	// 刷新在这条路上永远不会发生 —— 容量恒定正是解析推导的红利，别退化掉。
	CSHouseFrame::Scatter(Elements, FrameGpuBuffers, WorldToComponent);

	UE_LOG(LogTinyGladeHouse, Log, TEXT("[TinyGladeHouse] %s frame scattered: bricks=%d paths=%d"),
		*GetName(), BrickCount, Elements.Num());
}

// -----------------------------------------------------------------------------
// 接缝（D7，裁决二）—— 纯函数：两房 footprint → 接缝砖 + 裁剪段，零共享状态
//
// 这一段只做三件事：把自己与邻居翻译成 `CSHouseSeam::FHouse`、把纯函数的产物接到已有的两条
// 管线上（房体的裁剪场 / 门框砖的实例表）、以及把它们记进各自的哈希。**没有第四件事** ——
// 没有接缝 actor、没有交点表的生命周期、没有"谁拥有这条缝"。算法全在 `CSHouseSeam.h`。
// -----------------------------------------------------------------------------

CSHouseSeam::FHouse ACSHouseActor::MakeSeamHouse() const
{
	CSHouseSeam::FHouse H;
	H.Id = HouseId;
	const FVector Loc = GetActorLocation();
	H.Center = FVector2D(Loc.X, Loc.Y);
	// 只取 yaw，与 `GetBuildTransform()` 同口径 —— 房体面板与门框砖都建在那个变换里，
	// 接缝换一个口径就会在有 pitch/roll 的房子上与它们错开（「已知潜伏问题」那条的同族）。
	H.Yaw = float(GetActorRotation().Yaw);
	H.Footprint = FootprintSize;
	H.BaseZ = float(Loc.Z);
	H.WallHeight = WallHeight;
	H.WallThickness = WallThickness;
	return H;
}

void ACSHouseActor::GatherSeamNeighbours(TArray<CSHouseSeam::FHouse>& Out) const
{
	Out.Reset();
	if (!bSeamEnabled || !HouseId.IsValid()) return;

	const UWorld* World = GetWorld();
	const UCSHouseSubsystem* Subsystem = World ? World->GetSubsystem<UCSHouseSubsystem>() : nullptr;
	if (!Subsystem) return;

	const CSHouseSeam::FHouse Self = MakeSeamHouse();
	TArray<ACSHouseActor*> Houses;
	Subsystem->GetTrackedHouses(Houses);   // 已按 GUID 升序
	for (const ACSHouseActor* Other : Houses)
	{
		if (!IsValid(Other) || Other == this) continue;
		// ⚠️ **故意不读邻居的 `bSeamEnabled`**（第一版读了，当场被出图抓到）：那会让这栋房的
		// 几何取决于**更新顺序** —— 两栋房的开关分两句写时，先重建的那栋看到的是对方的旧值，
		// 于是 A 报 corners=0 而 B 报 corners=2，正好是本轮要证的对称性的反面。
		// 而且 `bSeamEnabled` 根本不是裁决二列的输入（那里只有 footprint / 朝向 / 高度）——
		// 它是"**我**画不画我这一份"的开关，不是世界状态。两栋房照旧各自算出同一条缝，
		// 谁的开关关着谁就不画自己那一份。
		const CSHouseSeam::FHouse Neighbour = Other->MakeSeamHouse();
		if (!Neighbour.Id.IsValid() || Neighbour.Id == HouseId) continue;
		if (!CSHouseSeam::WithinReach(Self, Neighbour)) continue;
		Out.Add(Neighbour);
	}
}

uint32 ACSHouseActor::ComputeSeamCuts()
{
	CurrentSeamCuts.Reset();
	CurrentSeamCornerCount = 0;
	if (!bSeamEnabled) return 0;

	const CSHouseSeam::FHouse Self = MakeSeamHouse();
	TArray<CSHouseSeam::FHouse> Neighbours;
	GatherSeamNeighbours(Neighbours);

	TArray<CSHouseSeam::FCorner> Corners;
	for (const CSHouseSeam::FHouse& Other : Neighbours)
	{
		if (!CSHouseSeam::Intersects(Self, Other)) continue;
		for (int32 Edge = 0; Edge < 4; ++Edge)
		{
			FCSWallCut Cut;
			if (CSHouseSeam::CutOnEdge(Self, Other, Edge, Cut)) CurrentSeamCuts.Add(Cut);
		}
		// 交点数只是统计（砖在 `BuildSeamBricks` 里才算）。放在这里是因为"有没有交汇"这件事
		// 属于房体这一轮的事实，脚本要用它区分"没相交"与"相交了但砖没画出来"。
		CurrentSeamCornerCount += CSHouseSeam::BuildCorners(Self, Other, Corners);
	}

	// 形状哈希：裁剪段决定面板怎么切 ⇒ 决定顶点位置与索引数，必须入房体哈希。
	// 交点数不入 —— 它一个顶点都不改（砖是独立实例，走门框那份哈希）。
	TArray<int32> H;
	for (const FCSWallCut& Cut : CurrentSeamCuts)
	{
		H.Append({ Cut.EdgeIndex, CSHouse_Q(Cut.MinS, 1), CSHouse_Q(Cut.MaxS, 1),
			CSHouse_Q(Cut.BottomZ, 1), CSHouse_Q(Cut.TopZ, 1) });
	}
	return CSHouse_Hash(H);
}

uint32 ACSHouseActor::BuildSeamBricks(TArray<CSHouseFrame::FElement>& InOutElements, int32& InOutBrickCount)
{
	CurrentSeamBrickCount = 0;
	if (!bSeamEnabled || !bFrameEnabled || !FrameBrickMesh) return 0;

	const CSHouseSeam::FHouse Self = MakeSeamHouse();
	TArray<CSHouseSeam::FHouse> Neighbours;
	GatherSeamNeighbours(Neighbours);
	if (Neighbours.IsEmpty()) return 0;

	CSHouseFrame::FBrickParams Params;
	Params.Length = FMath::Max(FrameBrickLength, 1.0f);
	Params.Gap = FMath::Max(FrameBrickGap, 0.0f);
	// **与门框砖共用同一份常驻容量**：接缝砖只是同一个组件里排在后面的那些行，超了一起截断。
	Params.MaxBricks = FMath::Max(FrameReserveCapacity, 64);

	TArray<int32> H;
	TArray<CSHouseSeam::FCorner> Corners;
	for (const CSHouseSeam::FHouse& Other : Neighbours)
	{
		if (CSHouseSeam::BuildCorners(Self, Other, Corners) <= 0) continue;
		const int32 Added = CSHouseSeam::BuildCornerElements(Corners, CSHouseSeam::SeamSeed(Self, Other), Params, InOutElements);
		CurrentSeamBrickCount += Added;
		InOutBrickCount += Added;
		// 哈希只记标量：交点位置 + 柱高 + 砖数。逐砖的位置是它们的纯函数（解析推导的定义）。
		H.Append({ Added });
		for (const CSHouseSeam::FCorner& Corner : Corners)
		{
			H.Append({ CSHouse_Q(Corner.Point.X, 1), CSHouse_Q(Corner.Point.Y, 1),
				CSHouse_Q(Corner.Outward.X, 0.01), CSHouse_Q(Corner.Outward.Y, 0.01),
				CSHouse_Q(Corner.BottomZ, 1), CSHouse_Q(Corner.TopZ, 1) });
		}
	}
	return CSHouse_Hash(H);
}

bool ACSHouseActor::IsSeamDrawable(FString& OutReason) const
{
	OutReason = GetSeamUndrawableReason();
	if (!OutReason.IsEmpty())
	{
		// ⚠️ **原因串到不了脚本里，必须自己打一行日志**（实测，与 IsWindowDrawable 同一条）：
		// UE Python 把"bool 返回值 + 一个 out 参数"收成单一返回值 —— 可画时拿到空串，
		// **不可画时拿到 `None`**，原因串直接丢了。脚本要拿原因请调 GetSeamUndrawableReason()。
		UE_LOG(LogTinyGladeHouse, Warning, TEXT("[TinyGladeHouse] %s seam not drawable: %s"), *GetName(), *OutReason);
	}
	return OutReason.IsEmpty();
}

FString ACSHouseActor::GetSeamUndrawableReason() const
{
	// 逐环检查渲染那一侧 —— 交点数 / 砖数 / 裁剪段数三条数值断言对这些一个字都说不了。
	if (!bSeamEnabled) return TEXT("bSeamEnabled 关着");
	if (CurrentSeamCornerCount <= 0) return TEXT("这栋房没有和任何邻居真的相交（触发条件是 footprint 真重叠，不是靠得近）");

	// ---- 洞那一半：插进邻居房间里的那截墙由墙材质逐像素 discard 抹掉 ----
	if (CurrentSeamCuts.IsEmpty()) return TEXT("相交了却一段墙都没抹掉（四条边与邻居 footprint 全无交集，不该发生）");
	if (!IsValid(TinyGladeMeshComponent)) return TEXT("房体：没有渲染组件");
	if (!TinyGladeMeshComponent->IsRegistered()) return TEXT("房体：渲染组件没注册");
	if (!TinyGladeMeshComponent->IsVisible()) return TEXT("房体：渲染组件不可见");
	if (!TinyGladeMesh) return TEXT("房体：常驻网格还没建起来");
	if (!WallMaterial) return TEXT("墙材质是空的（洞与墙都会退回引擎默认表面材质）");
	// **这一条是致命项**：接缝处那个洞不是几何，是墙材质用 OpacityMask 逐像素 discard 切出来的
	// （裁决三：避免所有真几何洞）。墙材质一旦不是 Masked，两栋房的墙就原样穿插着互相插进对方
	// 房间，而交点数 / 砖数 / 裁剪段数 / 零阻塞四条断言**全部照绿**。
	if (WallMaterial->GetBlendMode() != BLEND_Masked)
	{
		return FString::Printf(TEXT("墙材质 '%s' 不是 Masked（相交处的洞由 OpacityMask 逐像素切出，非 Masked 下那截墙照样画）"),
			*WallMaterial->GetName());
	}

	// ---- 砖那一半：轮廓交点上的砖柱（clip 断口的填充件，不是可选装饰）----
	if (!bFrameEnabled) return TEXT("bFrameEnabled 关着（接缝砖与门框砖共用一个组件，交点处会露出两条裁剪断口）");
	if (!FrameBrickMesh) return TEXT("没有 FrameBrickMesh");
	if (CurrentSeamBrickCount <= 0) return TEXT("一块接缝砖都没排出来（柱高不足半块砖，或砖数已被 FrameReserveCapacity 截断）");
	if (!IsValid(FrameComponent)) return TEXT("接缝砖：没有渲染组件");
	if (!FrameComponent->IsRegistered()) return TEXT("接缝砖：渲染组件没注册");
	if (!FrameComponent->IsVisible()) return TEXT("接缝砖：渲染组件不可见");
	if (!FrameComponent->HasInstanceSourceGPU()) return TEXT("接缝砖：实例源没交接");
	if (FrameComponent->GetBaseMeshSnapshot().Positions.Num() < 3) return TEXT("接缝砖：基础网格快照是空的");
	if (!FrameComponent->GetGpuMesh()) return TEXT("接缝砖：GPU 网格没分配");
	const UMaterialInterface* BrickMaterial = FrameComponent->InstanceMaterial;
	if (!BrickMaterial) return TEXT("接缝砖：没有绑材质（会用引擎默认表面材质画成一片灰）");
	// ⚠️ 没勾 `bUsedWithInstancedStaticMeshes` 的材质在实例路径上会被引擎**静默替换**成默认材质，
	// 症状与"没绑材质"逐像素相同。所以"材质支持实例化"必须是显式判据，不能只查材质非空。
	const UMaterial* BrickBase = BrickMaterial->GetMaterial();
	if (!BrickBase || !BrickBase->bUsedWithInstancedStaticMeshes)
	{
		return FString::Printf(
			TEXT("接缝砖：材质 '%s' 的母材质没有勾 bUsedWithInstancedStaticMeshes（引擎会静默换成默认材质）"),
			*BrickMaterial->GetName());
	}
	return FString();
}

// -----------------------------------------------------------------------------
// 窗（D8）—— 诉求走显式列表，几何全在房体与门框砖里
// -----------------------------------------------------------------------------

bool ACSHouseActor::IsWindowDrawable(FString& OutReason) const
{
	OutReason = GetWindowUndrawableReason();
	if (!OutReason.IsEmpty())
	{
		// ⚠️ **原因串到不了脚本里，必须自己打一行日志**（实测，与 IsVineDrawable 同一条）：
		// UE Python 把"bool 返回值 + 一个 out 参数"收成单一返回值 —— 可画时拿到空串，
		// **不可画时拿到 `None`**，原因串直接丢了。脚本要拿原因请调 GetWindowUndrawableReason()。
		UE_LOG(LogTinyGladeHouse, Warning, TEXT("[TinyGladeHouse] %s window not drawable: %s"), *GetName(), *OutReason);
	}
	return OutReason.IsEmpty();
}

FString ACSHouseActor::GetWindowUndrawableReason() const
{
	// 逐环检查渲染那一侧 —— readback 断言对这些一个字都说不了（同 IsVineDrawable / IsDecorDrawable）。
	if (!bWindowsEnabled) return TEXT("bWindowsEnabled 关着");
	if (Windows.IsEmpty()) return TEXT("Windows 列表是空的（窗只从这份显式列表来）");
	if (CurrentWindowCount <= 0)
	{
		return FString::Printf(TEXT("%d 个窗诉求全被谓词拒了（原因逐条调 QueryFeatureReject；门拱优先于窗）"),
			CurrentWindowRejectCount);
	}

	// ---- 洞那一半：房体网格 + 墙材质 ----
	if (!IsValid(TinyGladeMeshComponent)) return TEXT("房体：没有渲染组件");
	if (!TinyGladeMeshComponent->IsRegistered()) return TEXT("房体：渲染组件没注册");
	if (!TinyGladeMeshComponent->IsVisible()) return TEXT("房体：渲染组件不可见");
	if (!TinyGladeMesh) return TEXT("房体：常驻网格还没建起来");
	if (!WallMaterial) return TEXT("墙材质是空的（洞与墙都会退回引擎默认表面材质）");
	// **这一条是窗独有的致命项**：洞不是几何，是墙材质用 OpacityMask 逐像素 discard 切出来的
	// （裁决三：避免所有真几何洞）。墙材质一旦不是 Masked，画面上一个洞都没有，而洞数 / 砖数 /
	// 三角形数 / 零阻塞四条断言**全部照绿** —— 与"没绑材质"那一枪同型，只是更隐蔽。
	if (WallMaterial->GetBlendMode() != BLEND_Masked)
	{
		return FString::Printf(TEXT("墙材质 '%s' 不是 Masked（洞由 OpacityMask 逐像素切出，非 Masked 下窗洞根本不存在）"),
			*WallMaterial->GetName());
	}

	// ---- 砖那一半：窗台与窗楣那圈砖（clip 断口的填充件，不是可选装饰）----
	if (!bFrameEnabled) return TEXT("bFrameEnabled 关着（窗洞四边会露出裁剪断口）");
	if (!FrameBrickMesh) return TEXT("没有 FrameBrickMesh");
	if (CurrentFrameBrickCount <= 0) return TEXT("一块门框砖都没排出来");
	if (!IsValid(FrameComponent)) return TEXT("门框砖：没有渲染组件");
	if (!FrameComponent->IsRegistered()) return TEXT("门框砖：渲染组件没注册");
	if (!FrameComponent->IsVisible()) return TEXT("门框砖：渲染组件不可见");
	if (!FrameComponent->HasInstanceSourceGPU()) return TEXT("门框砖：实例源没交接");
	if (FrameComponent->GetBaseMeshSnapshot().Positions.Num() < 3) return TEXT("门框砖：基础网格快照是空的");
	if (!FrameComponent->GetGpuMesh()) return TEXT("门框砖：GPU 网格没分配");
	const UMaterialInterface* BrickMaterial = FrameComponent->InstanceMaterial;
	if (!BrickMaterial) return TEXT("门框砖：没有绑材质（会用引擎默认表面材质画成一片灰）");
	// ⚠️ 没勾 `bUsedWithInstancedStaticMeshes` 的材质在实例路径上会被引擎**静默替换**成默认材质，
	// 症状与"没绑材质"逐像素相同。所以"材质支持实例化"必须是显式判据，不能只查材质非空。
	const UMaterial* BrickBase = BrickMaterial->GetMaterial();
	if (!BrickBase || !BrickBase->bUsedWithInstancedStaticMeshes)
	{
		return FString::Printf(
			TEXT("门框砖：材质 '%s' 的母材质没有勾 bUsedWithInstancedStaticMeshes（引擎会静默换成默认材质）"),
			*BrickMaterial->GetName());
	}
	return FString();
}

// -----------------------------------------------------------------------------
// 藤蔓（D13）
// -----------------------------------------------------------------------------

void ACSHouseActor::BuildVineStrips(TArray<CSHouseVine::FWallStrip>& OutStrips) const
{
	OutStrips.Reset();
	// **与房体面板同一份 `CSHouse_GetEdge`**：墙在哪儿只能有一个真源。各抄一份的症状是
	// "藤悬在离墙半个墙厚的空中"，而且只在改过 WallThickness 之后才显形。
	const FTransform World = GetBuildTransform();
	const FCSRoofDesc Roof = GetRoofDesc();
	for (int32 EdgeIndex = 0; EdgeIndex < 4; ++EdgeIndex)
	{
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(EdgeIndex, FootprintSize, WallThickness);
		if (F.Len <= UE_KINDA_SMALL_NUMBER) continue;

		CSHouseVine::FWallStrip Strip;
		Strip.EdgeIndex = EdgeIndex;
		Strip.Origin = World.TransformPosition(FVector(F.Start.X, F.Start.Y, 0.0));
		Strip.U = World.TransformVectorNoScale(FVector(F.U.X, F.U.Y, 0.0)).GetSafeNormal();
		Strip.Up = World.TransformVectorNoScale(FVector::UpVector).GetSafeNormal();
		// `In` 指向体内，藤长在**外**皮上。
		Strip.N = -World.TransformVectorNoScale(FVector(F.In.X, F.In.Y, 0.0)).GetSafeNormal();
		Strip.Length = F.Len;
		Strip.Height = WallHeight;

		// 山墙三角。**屋面方程仍然只有 `CSHouseRoof.h` 那一份真源** —— 这里只是把它在这面墙上
		// 的一维剖面折算成三个标量交给纯函数的规划器（`FWallStrip::TopAt`）。
		//
		// 判据是"这面墙的走向平不平行于脊"：脊沿 X 时，沿 Y 走的那两面（edge 1/3）是山墙，
		// 跨度坐标沿 S **线性**，山尖落在跨度 = 0（脊线）那一点上；沿 X 走的那两面是檐墙，
		// 跨度坐标沿 S 恒定 ⇒ 墙顶是平的，三个标量留 0。
		// ⚠️ 上界取 `CSHouseRoof_EvalZAcross`（屋面**底**）而不是 `SoffitTopZ`（再加咬入量）：
		// 咬入量是墙**扎进**屋面板的那一截，藤爬到那里就已经在板子里了。
		if (bVineClimbGable)
		{
			const double AcrossAtStart = Roof.LocalToAcross(F.Start);
			const double AcrossAtEnd = Roof.LocalToAcross(FVector2D(F.Start) + FVector2D(F.U) * double(F.Len));
			const double AcrossSpan = AcrossAtEnd - AcrossAtStart;
			if (FMath::Abs(AcrossSpan) > UE_DOUBLE_KINDA_SMALL_NUMBER)
			{
				const double DAcrossDS = AcrossSpan / double(F.Len);   // ±1（矩形 footprint）
				Strip.GableTan = Roof.TanPitch();
				Strip.GablePeakS = float(-AcrossAtStart / DAcrossDS);   // across(S) = 0 的那个 S
				Strip.GableHalfSpan = Roof.HalfSpan();
			}
		}
		OutStrips.Add(Strip);
	}
}

void ACSHouseActor::EnsureVineComponents()
{
	// 蓝图 actor 重跑构造脚本会把实例组件销毁，指针会失效 —— 先判再补（同 EnsureFrameComponent）。
	auto EnsureOne = [this](TObjectPtr<UCSGpuInstancedMeshComponent>& Component)
	{
		if (!IsValid(Component))
		{
			Component = NewObject<UCSGpuInstancedMeshComponent>(this, NAME_None, RF_Transient);
			Component->SetupAttachment(RootComponent);
			Component->RegisterComponent();   // 未注册时任何变更都会释放 GPU 网格，必须先注册再喂
			bVineBaseMeshReady = false;       // 新组件身上没有基础网格快照
		}
	};
	EnsureOne(VineBranchComponent);
	EnsureOne(VineLeafComponent);
	EnsureOne(VineFlowerComponent);
	VineBranchComponent->InstanceMaterial = VineBranchMaterial;
	VineFlowerComponent->InstanceMaterial = VineFlowerMaterial;

	// 三季叶：只写母材质上的 `Season` 标量，**不换材质资产**（理由见 `ECSVineSeason`）。
	// ⚠️ MID 的父换了必须重建 —— 在细节面板里换掉 `VineLeafMaterial` 时旧 MID 仍然有效，
	// 于是"换了材质但画面没变"，与 `VineBranchMeshBuiltFrom` 那条是同一个失败模式。
	if (VineLeafMaterial)
	{
		if (!VineLeafSeasonMID || VineLeafSeasonMID->Parent != VineLeafMaterial)
		{
			VineLeafSeasonMID = UMaterialInstanceDynamic::Create(VineLeafMaterial, this);
		}
		if (VineLeafSeasonMID) VineLeafSeasonMID->SetScalarParameterValue(TEXT("Season"), float(uint8(VineSeason)));
	}
	else
	{
		VineLeafSeasonMID = nullptr;
	}
	// 退回母材质而不是留空：留空的话组件会画成引擎默认灰，而 `GetVineUndrawableReason`
	// 只查"材质非空"就放行了 —— 那正是石阶那个坑的形状。
	VineLeafComponent->InstanceMaterial = VineLeafSeasonMID
		? static_cast<UMaterialInterface*>(VineLeafSeasonMID) : ToRawPtr(VineLeafMaterial);

	if (VineGpuBuffers.Num() != CSHouseVine::Palette_Num)
	{
		CSShaperSteps::ReleaseOnRenderThread(VineGpuBuffers);
		VineGpuBuffers.SetNum(CSHouseVine::Palette_Num);
		VineHandedCapacities.Reset();
		bVineBaseMeshReady = false;
	}

	// 基础网格快照只在第一次（或组件被重建后）建一次：它要读 LOD0 顶点、补法线与 UV，
	// 不是每帧该做的事。⚠️ **不能改用 `SetBaseMesh`** —— `ivy_branch` 的切线流是空的，
	// 那条路会把一份零长法线原样搬进快照，画出来是一条黑剪影（见 CSHouseVine::BuildBaseMesh）。
	// 换过网格资产就必须重建快照（见 VineBranchMeshBuiltFrom 的字段注释）。
	if (VineBranchMeshBuiltFrom != VineBranchMesh || VineLeafMeshBuiltFrom != VineLeafMesh
		|| VineFlowerMeshBuiltFrom != VineFlowerMesh)
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
		const bool bBranchOk = CSHouseVine::BuildBaseMesh(VineBranchMesh, 2, BranchData);
		const bool bLeafOk = CSHouseVine::BuildBaseMesh(VineLeafMesh, 1, LeafData);
		const bool bFlowerOk = CSHouseVine::BuildBaseMesh(VineFlowerMesh, 2, FlowerData);
		if (bBranchOk) VineBranchComponent->SetBaseMeshFromGpuData(BranchData);
		if (bLeafOk) VineLeafComponent->SetBaseMeshFromGpuData(LeafData);
		if (bFlowerOk) VineFlowerComponent->SetBaseMeshFromGpuData(FlowerData);
		// 花是**可选**的（网格留空 = 不长花），所以它不进 `bVineBaseMeshReady` 的与 ——
		// 进了的话没配花的房子会连枝带叶一起消失。
		bVineBaseMeshReady = bBranchOk && bLeafOk;
		VineBranchMeshBuiltFrom = bBranchOk ? VineBranchMesh : nullptr;
		VineLeafMeshBuiltFrom = bLeafOk ? VineLeafMesh : nullptr;
		VineFlowerMeshBuiltFrom = bFlowerOk ? VineFlowerMesh : nullptr;

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
		if (bBranchOk) SetBlock(VineGpuBuffers[CSHouseVine::Palette_Branch], BranchData, FMath::Max(VineThickness, 1.0f));
		// 叶片按"长度 = 宽度"等比放：记录的 LengthScale 与 SizeScale 带同一个抖动系数。
		if (bLeafOk) SetBlock(VineGpuBuffers[CSHouseVine::Palette_Leaf], LeafData, FMath::Max(VineLeafSize, 2.0f));
		// 花的截面是**宽度**、长度轴是**高度**，两者由 `FParams::FlowerAspect` 联系起来。
		if (bFlowerOk) SetBlock(VineGpuBuffers[CSHouseVine::Palette_Flower], FlowerData, FMath::Max(VineFlowerSize, 2.0f));
	}

	if (!bVineBaseMeshReady) return;

	// 容量按**配置上限**一次付清，之后永不扩容（零阻塞纪律）。上限是纯配置量：
	// 四面墙的总周长 / 间距 × 每根最多几段。规划真的排超了就在 kernel 里截断 ——
	// 少画几段藤，远好过在拖动的某一帧上付一次设备同步。
	const double Perimeter = 2.0 * (FootprintSize.X + FootprintSize.Y);
	const int32 MaxStrands = FMath::CeilToInt(Perimeter / FMath::Max(VineStrandSpacing, 20.0f)) + 4;
	// ⚠️ **必须再走一次 `CSShaperSteps::ReserveCount` 的台阶**，不能把这个数直接喂给 ReserveCapacity。
	// 上限本身是 FootprintSize 的**连续函数**（周长 / 间距），而 ReserveCapacity 只对齐到 64 ——
	// 拖尺寸时每涨过一根藤的间距就重新分配一次，实测每次 **5 次阻塞刷新**，一段拖动累计 21 次。
	// 这与门框砖包围盒那条是同一个失败模式（"值连续变 ⇒ 资源每帧重来"），解法也同一个：
	// 留 50% 余量 + 对齐到 4096。代价是显存多留一点（一行 80 B，4096 行 = 320 KB）。
	const uint32 MaxRecords = uint32(FMath::Clamp(
		CSShaperSteps::ReserveCount(MaxStrands * FMath::Clamp(VineMaxSegments, 1, 128)), 64, 1 << 16));
	// 三个调色板同容量。花远少于枝，但 `ReserveCapacity` 是逐调色板同一个下限的接口，
	// 而多留的那一份是 4096 行 × 80 B = 320 KB —— 为省它去开一条"逐调色板容量"的口子，
	// 换来的是一处只在花特别多时才会显形的截断，不划算。
	CSShaperSteps::ReserveCapacity(VineGpuBuffers, MaxRecords);

	// 交接包围盒：量化 + 只涨不缩，理由与门框砖那段逐字相同（拖尺寸时 1 cm 阈值会让
	// 每帧都重走一次阻塞的 SetInstanceSourceGPU）。藤爬满整面墙，所以取整个 footprint。
	const double Reach = CSShaperSteps::QuantizeUp(FMath::Max(FootprintSize.X, FootprintSize.Y) * 0.6
		+ FMath::Max(VineLeafSize, VineFlowerSize) + VineStandOff);
	// ⚠️ 上界要含**山墙尖**：藤能爬到脊高，包围盒还停在墙高的话整片山墙藤会在斜看时被剔掉
	// （症状是"转个视角藤就成片闪没"，最难复现的那一类）。`CSShaperSteps::QuantizeUp` 只涨不缩，
	// 所以这一项即使 bVineClimbGable 关着也照留 —— 它不是每帧变的量。
	const double Top = CSShaperSteps::QuantizeUp(CSHouseRoof_RidgeZ(GetRoofDesc())
		+ FMath::Max(VineLeafSize, VineFlowerSize));
	FBox LocalBounds(FVector(-Reach, -Reach, -VineLeafSize), FVector(Reach, Reach, Top));
	if (!bForceFullRebuild && VineHandedLocalBounds.IsValid) LocalBounds += VineHandedLocalBounds;

	UCSGpuInstancedMeshComponent* Components[CSHouseVine::Palette_Num] =
		{ VineBranchComponent, VineLeafComponent, VineFlowerComponent };

	bool bNeedHandover = VineHandedCapacities.Num() != CSHouseVine::Palette_Num
		|| !VineHandedLocalBounds.IsValid
		|| !VineHandedLocalBounds.Min.Equals(LocalBounds.Min, 1.0)
		|| !VineHandedLocalBounds.Max.Equals(LocalBounds.Max, 1.0);
	for (int32 Index = 0; !bNeedHandover && Index < CSHouseVine::Palette_Num; ++Index)
	{
		// 蓝图重跑构造脚本会销毁并重建实例组件：新组件身上没有实例源，缓存说"已交接"就会
		// 永远画不出东西 —— 拿组件自己的状态兜底（同 EnsureFrameComponent）。
		bNeedHandover = VineHandedCapacities[Index] != VineGpuBuffers[Index].Capacity
			|| !Components[Index]->HasInstanceSourceGPU();
	}
	if (!bNeedHandover) return;
	for (int32 Index = 0; Index < CSHouseVine::Palette_Num; ++Index)
	{
		if (!VineGpuBuffers[Index].IsValid()) return;
	}

	for (int32 Index = 0; Index < CSHouseVine::Palette_Num; ++Index)
	{
		FCSGpuInstanceSourceGPU Source;
		Source.PackedInstances = VineGpuBuffers[Index].PackedInstances;   // 保留自己的引用，重打包还要用
		Source.Counter = VineGpuBuffers[Index].Counter;
		Source.Capacity = VineGpuBuffers[Index].Capacity;
		Source.LocalBounds = LocalBounds;
		Components[Index]->SetInstanceSourceGPU(Source);
	}

	VineHandedCapacities.SetNumUninitialized(CSHouseVine::Palette_Num);
	for (int32 Index = 0; Index < CSHouseVine::Palette_Num; ++Index)
	{
		VineHandedCapacities[Index] = VineGpuBuffers[Index].Capacity;
	}
	VineHandedLocalBounds = LocalBounds;
}

void ACSHouseActor::RebuildVine()
{
	if (!bVineEnabled || !VineBranchMesh || !VineLeafMesh)
	{
		if (CurrentVineSegmentCount != 0 || CurrentVineLeafCount != 0 || CurrentVineFlowerCount != 0)
		{
			// ⚠️ **撤实例源之前必须先把 counter 清零**（与 `RebuildFrame` 那条同源，
			// 门框砖已经因此在画面上留过 12 层砖）：撤掉之后 `VineHandedCapacities` 被清空，
			// 下一次 `EnsureVineComponents` 会把同一批 buffer 交回组件。这条路上的窗口是
			// **重新打开藤之后 `bVineBaseMeshReady` 为假**那一次 —— 那时交接已经发生，
			// 而 `CSHouseVine::Pack`（它自己的空表分支会清零）根本走不到。
			CSShaperSteps::ZeroCounters(VineGpuBuffers);
			if (VineBranchComponent) VineBranchComponent->ClearInstanceSourceGPU();
			if (VineLeafComponent) VineLeafComponent->ClearInstanceSourceGPU();
			if (VineFlowerComponent) VineFlowerComponent->ClearInstanceSourceGPU();
			VineHandedCapacities.Reset();
			VineHandedLocalBounds = FBox(ForceInit);
			CurrentVineSegmentCount = 0;
			CurrentVineLeafCount = 0;
			CurrentVineFlowerCount = 0;
			VineDescHash = 0;
		}
		return;
	}

	EnsureVineComponents();
	if (!bVineBaseMeshReady) return;

	TArray<CSHouseVine::FWallStrip> Strips;
	BuildVineStrips(Strips);

	CSHouseVine::FParams Params;
	Params.StrandSpacing = VineStrandSpacing;
	Params.SegmentLength = VineSegmentLength;
	Params.MaxSegments = VineMaxSegments;
	Params.Wander = VineWander;
	Params.MaxLean = VineMaxLean;
	Params.Bloat = VineBloat;
	Params.Thickness = VineThickness;
	Params.StandOff = VineStandOff;
	Params.HoleClearance = VineHoleClearance;
	Params.LeafChance = VineLeafChance;
	Params.LeafSize = VineLeafSize;
	Params.LeafSizeJitter = VineLeafSizeJitter;
	// 没配花网格时把概率钉成 0：否则规划器照排记录，而 counter 指向一个没有基础网格的组件 ——
	// 症状是"实例数对得上但屏幕上什么都没有"，与"材质被静默换掉"一样查不出来。
	Params.FlowerChance = VineFlowerMesh ? VineFlowerChance : 0.0f;
	Params.FlowerFromFrac = VineFlowerFromFrac;
	Params.FlowerSize = VineFlowerSize;
	Params.JumpChance = VineJumpChance;
	Params.Seed = VineSeed;

	CSHouseVine::FPlan Plan;
	CSHouseVine::BuildPlan(Strips, CurrentOpenings, Params, Plan);

	// 幂等短路。哈希覆盖"会改变藤的形态的一切"：摆位 + 尺寸 + 洞集合 + 参数。
	// ⚠️ 短路点在 BuildPlan **之后**是有意的：规划是纯 CPU、微秒量级，而它同时是哈希的
	// 唯一诚实来源 —— 拿参数拼一个哈希而不跑规划，会在"参数没变但洞变了"时静默漏更新。
	const TArray<int32> HashInput = {
		int32(Plan.Branch.Num()), int32(Plan.Leaf.Num()), int32(Plan.Flower.Num()),
		CSHouse_Q(FootprintSize.X, 1), CSHouse_Q(FootprintSize.Y, 1), CSHouse_Q(WallHeight, 1),
		CSHouse_Q(VineStrandSpacing, 0.5), CSHouse_Q(VineSegmentLength, 0.5), VineMaxSegments,
		CSHouse_Q(VineWander, 0.01), CSHouse_Q(VineMaxLean, 0.01), CSHouse_Q(VineBloat, 0.01),
		CSHouse_Q(VineThickness, 0.1), CSHouse_Q(VineStandOff, 0.1), CSHouse_Q(VineLeafSize, 0.1),
		CSHouse_Q(VineLeafChance, 0.01), CSHouse_Q(VineLeafSizeJitter, 0.01), VineSeed,
		CSHouse_Q(VineFlowerChance, 0.01), CSHouse_Q(VineFlowerFromFrac, 0.01), CSHouse_Q(VineFlowerSize, 0.1),
		CSHouse_Q(VineJumpChance, 0.01), int32(bVineClimbGable),
		// 山墙顶随屋面走 ⇒ 屋面参数也进哈希，否则只改坡度时藤不重排（山墙那两面会穿帮）。
		CSHouse_Q(RoofPitch, 0.1), int32(RidgeAxis),
		int32(ComputePlacementHash() & 0x7FFFFFFF) };
	const uint32 NewHash = CSHouse_Hash(HashInput);

	bool bBuffersReady = VineGpuBuffers.Num() == CSHouseVine::Palette_Num;
	for (int32 Index = 0; bBuffersReady && Index < CSHouseVine::Palette_Num; ++Index)
	{
		bBuffersReady = VineGpuBuffers[Index].IsValid();
	}
	if (NewHash == VineDescHash && VineHandedCapacities.Num() == CSHouseVine::Palette_Num && bBuffersReady) return;

	VineDescHash = NewHash;
	CurrentVineSegmentCount = Plan.Branch.Num();
	CurrentVineLeafCount = Plan.Leaf.Num();
	CurrentVineFlowerCount = Plan.Flower.Num();

	// 世界 → 组件。⚠️ **用组件自己的变换求逆**，不用 actor 的：已删的门框旧路混用
	// `GetBuildTransform()`（只取 yaw）与 actor 的完整逆变换，正是状态文件「已知潜伏问题」
	// 里那条 —— 房子一旦被 pitch/roll 或缩放就错位。这里不重复它。
	const FMatrix44f WorldToComponent = FMatrix44f(
		VineBranchComponent->GetComponentTransform().ToInverseMatrixWithScale());
	CSHouseVine::Pack(Plan, VineGpuBuffers, WorldToComponent);

	UE_LOG(LogTinyGladeHouse, Log, TEXT("[TinyGladeHouse] %s vine packed: strips=%d branches=%d leaves=%d flowers=%d"),
		*GetName(), Strips.Num(), CurrentVineSegmentCount, CurrentVineLeafCount, CurrentVineFlowerCount);
}

bool ACSHouseActor::IsVineDrawable(FString& OutReason) const
{
	OutReason = GetVineUndrawableReason();
	if (!OutReason.IsEmpty())
	{
		// ⚠️ **原因串到不了脚本里，必须自己打一行日志**（实测，与 IsDecorDrawable 同一条）：
		// UE Python 把"bool 返回值 + 一个 out 参数"收成单一返回值 —— 可画时拿到空串，
		// **不可画时拿到 `None`**，原因串直接丢了。脚本要拿原因请调 GetVineUndrawableReason()。
		UE_LOG(LogTinyGladeHouse, Warning, TEXT("[TinyGladeHouse] %s vine not drawable: %s"), *GetName(), *OutReason);
	}
	return OutReason.IsEmpty();
}

FString ACSHouseActor::GetVineUndrawableReason() const
{
	FString OutReason;
	// 逐环检查渲染那一侧 —— readback 断言对这些一个字都说不了（见头文件里那段教训）。
	if (!bVineEnabled) { OutReason = TEXT("bVineEnabled 关着"); return OutReason; }
	if (!VineBranchMesh) { OutReason = TEXT("没有 VineBranchMesh"); return OutReason; }
	if (!VineLeafMesh) { OutReason = TEXT("没有 VineLeafMesh"); return OutReason; }
	if (!bVineBaseMeshReady) { OutReason = TEXT("基础网格快照没建起来（读不到 LOD0 顶点？）"); return OutReason; }
	if (CurrentVineSegmentCount <= 0) { OutReason = TEXT("一段藤都没排出来"); return OutReason; }

	const UCSGpuInstancedMeshComponent* Components[CSHouseVine::Palette_Num] =
		{ VineBranchComponent, VineLeafComponent, VineFlowerComponent };
	const TCHAR* Names[CSHouseVine::Palette_Num] = { TEXT("枝"), TEXT("叶"), TEXT("花") };
	for (int32 Index = 0; Index < CSHouseVine::Palette_Num; ++Index)
	{
		// 花是可选的：没配网格就整条跳过，而不是报"画不出来"—— 那会把一栋**正确**的
		// 无花房子判成红灯，判据一旦有假红，下一次真红就没人信了。
		if (Index == CSHouseVine::Palette_Flower && !VineFlowerMesh) continue;
		const UCSGpuInstancedMeshComponent* Component = Components[Index];
		if (!IsValid(Component)) { OutReason = FString::Printf(TEXT("%s：没有渲染组件"), Names[Index]); return OutReason; }
		if (!Component->IsRegistered()) { OutReason = FString::Printf(TEXT("%s：渲染组件没注册"), Names[Index]); return OutReason; }
		if (!Component->IsVisible()) { OutReason = FString::Printf(TEXT("%s：渲染组件不可见"), Names[Index]); return OutReason; }
		if (!Component->HasInstanceSourceGPU()) { OutReason = FString::Printf(TEXT("%s：实例源没交接"), Names[Index]); return OutReason; }
		if (Component->GetBaseMeshSnapshot().Positions.Num() < 3)
		{
			OutReason = FString::Printf(TEXT("%s：基础网格快照是空的"), Names[Index]);
			return OutReason;
		}
		if (!Component->GetGpuMesh()) { OutReason = FString::Printf(TEXT("%s：GPU 网格没分配"), Names[Index]); return OutReason; }

		// **这一条就是石阶那个坑**：材质为空时组件仍然会画，只是退回引擎默认表面材质 ——
		// 画面上是一片灰，而所有 readback 断言照绿。
		const UMaterialInterface* Material = Component->InstanceMaterial;
		if (!Material)
		{
			OutReason = FString::Printf(TEXT("%s：没有绑材质（会用引擎默认表面材质画成一片灰）"), Names[Index]);
			return OutReason;
		}
		// ⚠️ 比石阶那条**多一环**：没勾 `bUsedWithInstancedStaticMeshes` 的材质在实例路径上
		// 会被引擎**静默替换**成默认材质，症状与"没绑材质"逐像素相同。现成的 `MI_ivy_*`
		// 全都挂在 `M_TG_Texture` 下，而它恰恰没勾 —— 这是本模块最容易中的一枪。
		const UMaterial* Base = Material->GetMaterial();
		if (!Base || !Base->bUsedWithInstancedStaticMeshes)
		{
			OutReason = FString::Printf(
				TEXT("%s：材质 '%s' 的母材质没有勾 bUsedWithInstancedStaticMeshes（引擎会静默换成默认材质）"),
				Names[Index], *Material->GetName());
			return OutReason;
		}
		// 🆕 **第三条"静默换默认材质"**（2026-08-31 现场踩到）：母材质**编译失败**时引擎同样
		// 静默退回默认材质，与上面两条逐像素同症状 —— 叶子整片暗蓝黑，而上面那两条全绿。
		// 现场是三季混合里 `MaterialExpressionClamp` 的针脚接不上（MEL 的连线函数**返回 false
		// 不抛异常**），材质带着 `Missing Clamp input` 存了盘，日志里只有一条 LogMaterial Warning。
		// ⚠️ 这一环也会在**异步编译还没完**时为真。措辞因此把两种情况一起说 —— 对调用方来说
		// 两者的动作是同一个（等一下再看 / 去看 LogMaterial），把它们拆开反而会诱导人忽略前者。
		if (const_cast<UMaterial*>(Base)->IsCompilingOrHadCompileError(GMaxRHIShaderPlatform))
		{
			OutReason = FString::Printf(
				TEXT("%s：母材质 '%s' 还在编译、或者编译失败（引擎会静默换成默认材质；去看 LogMaterial）"),
				Names[Index], *Base->GetName());
			return OutReason;
		}
	}
	return FString();
}

// -----------------------------------------------------------------------------
// 装饰摆件（D12 的锚点那一半）
//
// ⚠️ 计划 D12 的「复杂度场 `RT_DecorField` + tile-argmax」**不在这里**，也不在别处 ——
// 它在 TG 里没有对位物，取舍是挂起的决策 **C2**。本节只做锚点：围着已经存在的构件长。
// 详见 `CSHouseDecor.h` 的文件头。
// -----------------------------------------------------------------------------

void ACSHouseActor::BuildDecorSite(CSHouseDecor::FSite& OutSite) const
{
	// 墙矩形与藤蔓共用同一份 `BuildVineStrips`（它自己又走 `CSHouse_GetEdge`）——
	// 墙在哪儿只能有一个真源，各抄一份的症状是"摆件悬在离墙半个墙厚的空中"，
	// 而且只在改过 WallThickness 之后才显形。
	BuildVineStrips(OutSite.Strips);
	OutSite.Openings = CurrentOpenings;
	// 屋面 desc 也只有一个真源（计划 D4）：檐口/屋脊的高度由 `CSHouseRoof_*` 求值，
	// 这里一条屋顶方程都不写。
	OutSite.Roof = GetRoofDesc();
	OutSite.World = GetBuildTransform();
	OutSite.BaseZ = GetActorLocation().Z;

	// 落高与道路排除走地面镜像的 CPU 采样器 —— 与 `ComputeDoors` 同一条路，是权威层，
	// 而且**纯 CPU**：装饰这条链一次回读都不会有（计划 D12 的异步回读是给场准备的，
	// 锚点这一半根本不需要）。
	if (const ACSGroundActor* G = Ground)
	{
		OutSite.SampleGroundZ = [G](const FVector2D& XY) { return G->SampleHeight(XY); };
		OutSite.SampleRoadWeight = [G](const FVector2D& XY) { return G->SampleRoadWeight(XY); };
	}
}

CSHouseDecor::FParams ACSHouseActor::MakeDecorParams() const
{
	CSHouseDecor::FParams Params;
	Params.WallFootSpacing = DecorWallFootSpacing;
	Params.EaveSpacing = DecorEaveSpacing;
	// 屋脊比檐口稀一档：脊线只有**一条**而檐口有两条，按同一密度摆会在屋顶正中排成一串珠子。
	Params.RidgeSpacing = DecorEaveSpacing * 1.13f;
	Params.MinSpacing = DecorMinSpacing;
	Params.RoadReject = DecorRoadReject;
	Params.BaseScale = DecorScale;
	Params.ScaleJitter = DecorScaleJitter;
	Params.Seed = DecorSeed;
	return Params;
}

void ACSHouseActor::EnsureDecorComponents()
{
	// palette 的排布恒为「门 → 墙脚 → 屋顶」，三家各占一段；檐口与屋脊**共用**屋顶那一段
	// （TG 的 `add_birdnests` 本来就是一家两处）。窗户那家留空 ⇒ `Count == 0` ⇒ 一件都不长。
	TArray<UStaticMesh*> Wanted;
	DecorPaletteRanges.SetNum(int32(CSHouseDecor::EFamily::Count));
	for (CSHouseDecor::FPaletteRange& Range : DecorPaletteRanges) Range = CSHouseDecor::FPaletteRange();

	auto AppendFamily = [&Wanted](const TArray<TObjectPtr<UStaticMesh>>& Source, CSHouseDecor::FPaletteRange& OutRange)
	{
		OutRange.First = Wanted.Num();
		for (const TObjectPtr<UStaticMesh>& Mesh : Source)
		{
			if (Mesh) Wanted.Add(Mesh);
		}
		OutRange.Count = Wanted.Num() - OutRange.First;
	};
	AppendFamily(DecorGateMeshes, DecorPaletteRanges[int32(CSHouseDecor::EFamily::Gate)]);
	AppendFamily(DecorWallFootMeshes, DecorPaletteRanges[int32(CSHouseDecor::EFamily::WallFoot)]);
	AppendFamily(DecorRoofMeshes, DecorPaletteRanges[int32(CSHouseDecor::EFamily::Eave)]);
	DecorPaletteRanges[int32(CSHouseDecor::EFamily::Ridge)] = DecorPaletteRanges[int32(CSHouseDecor::EFamily::Eave)];

	// 蓝图 actor 重跑构造脚本会把实例组件销毁，指针会失效 —— 先判再补（同 EnsureVineComponents）。
	// ⚠️ 组件数一变就必须重建基础网格快照：palette 与组件是**按下标**对齐的，少一个就全体错位。
	bool bComponentsChanged = false;
	while (DecorComponents.Num() > Wanted.Num())
	{
		TObjectPtr<UCSGpuInstancedMeshComponent> Extra = DecorComponents.Pop();
		if (IsValid(Extra)) Extra->DestroyComponent();
		bComponentsChanged = true;
	}
	while (DecorComponents.Num() < Wanted.Num())
	{
		DecorComponents.Add(nullptr);
		bComponentsChanged = true;
	}
	for (int32 Index = 0; Index < Wanted.Num(); ++Index)
	{
		if (!IsValid(DecorComponents[Index]))
		{
			UCSGpuInstancedMeshComponent* Component = NewObject<UCSGpuInstancedMeshComponent>(this, NAME_None, RF_Transient);
			Component->SetupAttachment(RootComponent);
			Component->RegisterComponent();   // 未注册时任何变更都会释放 GPU 网格，必须先注册再喂
			DecorComponents[Index] = Component;
			bComponentsChanged = true;
		}
		DecorComponents[Index]->InstanceMaterial = DecorMaterial;
	}
	if (bComponentsChanged) bDecorBaseMeshReady = false;

	if (DecorGpuBuffers.Num() != Wanted.Num())
	{
		CSShaperSteps::ReleaseOnRenderThread(DecorGpuBuffers);
		DecorGpuBuffers.SetNum(Wanted.Num());
		DecorHandedCapacities.Reset();
		bDecorBaseMeshReady = false;
	}
	if (Wanted.Num() == 0) return;

	// 换过网格资产就必须重建快照（同 `VineBranchMeshBuiltFrom` 的字段注释）。
	bool bMeshesChanged = DecorMeshesBuiltFrom.Num() != Wanted.Num();
	for (int32 Index = 0; !bMeshesChanged && Index < Wanted.Num(); ++Index)
	{
		bMeshesChanged = DecorMeshesBuiltFrom[Index] != Wanted[Index];
	}
	if (bMeshesChanged) bDecorBaseMeshReady = false;

	if (!bDecorBaseMeshReady)
	{
		DecorMeshesBuiltFrom.SetNum(Wanted.Num());
		bool bAllOk = true;
		for (int32 Index = 0; Index < Wanted.Num(); ++Index)
		{
			FCSGpuMeshCPUData Data;
			// ⚠️ **复用藤蔓那份读取器，不是图省事**：它做的事对 clutter 同样必要 ——
			// 判"法线/UV 读出来合不合法"（有流不等于有数据）、缺了就现补、并把**顶点色**搬进快照。
			// 顶点色这一条对杂物是决定性的：`Content/TinyGlade/Textures/` 里**没有一张 clutter 贴图**
			// （459 张贴图与 459 个 MI 一一对应，clutter 一个都不在其中），它们的颜色全烘在顶点流里。
			// 长度轴传 2（不换轴）：摆件本来就以 +Z 为上，藤那两张才需要换。
			const bool bOk = CSHouseVine::BuildBaseMesh(Wanted[Index], 2, Data);
			if (bOk)
			{
				DecorComponents[Index]->SetBaseMeshFromGpuData(Data);

				FBox3f Local(ForceInit);
				for (const FVector3f& P : Data.Positions) Local += P;
				CSShaperSteps::FPaletteBuffers& Buffers = DecorGpuBuffers[Index];
				Buffers.BaseSphereCentre = Local.IsValid ? Local.GetCenter() : FVector3f::ZeroVector;
				Buffers.BaseSphereRadius = Local.IsValid ? Local.GetExtent().Size() : 0.0f;
				// clutter 网格自带真实尺寸（不是石阶那种单位立方体字典 mesh），所以块尺寸留 1 ——
				// 缩放全部活在记录的 Scale/ScaleZ 里。
				Buffers.BlockSize = FVector3f(1.0f, 1.0f, 1.0f);
			}
			DecorMeshesBuiltFrom[Index] = bOk ? Wanted[Index] : nullptr;
			bAllOk &= bOk;
		}
		bDecorBaseMeshReady = bAllOk;
	}
	if (!bDecorBaseMeshReady) return;

	// 容量按**配置上限**一次付清，之后永不扩容（零阻塞纪律）。
	// ⚠️ **必须再走一次 `CSShaperSteps::ReserveCount` 的台阶**，不能把上限直接喂给 ReserveCapacity：
	// 上限是 FootprintSize 的**连续函数**（周长 / 间距），而 ReserveCapacity 只对齐到 64 ——
	// 拖尺寸时每涨过一个间距就重新分配一次。藤蔓那轮正是漏了这一步，实测一段拖动 21 次阻塞刷新。
	const CSHouseDecor::FParams Params = MakeDecorParams();
	const int32 Bound = CSHouseDecor::MaxRecordsBound(FootprintSize, RoofOverhang, PierWidth, Params);
	const uint32 MaxRecords = uint32(FMath::Clamp(CSShaperSteps::ReserveCount(Bound), 64, 1 << 16));
	CSShaperSteps::ReserveCapacity(DecorGpuBuffers, MaxRecords);

	// 交接包围盒：量化 + 只涨不缩，理由与门框砖 / 藤蔓那两段逐字相同（1 cm 阈值会让拖尺寸时
	// 每帧都重走一次阻塞的 SetInstanceSourceGPU）。摆件最远伸到门前引道，最高到屋脊上的鸟窝。
	double MeshReach = 0.0;
	for (const CSShaperSteps::FPaletteBuffers& Buffers : DecorGpuBuffers)
	{
		MeshReach = FMath::Max(MeshReach, double(Buffers.BaseSphereRadius));
	}
	MeshReach *= FMath::Max(double(DecorScale) * (1.0 + double(DecorScaleJitter)), 0.05);
	const double Reach = CSShaperSteps::QuantizeUp(FMath::Max(FootprintSize.X, FootprintSize.Y) * 0.6
		+ Params.GateApproachDistance + Params.GateApproachSpread + MeshReach);
	const double Top = CSShaperSteps::QuantizeUp(CSHouseRoof_RidgeZ(GetRoofDesc()) + Params.RoofStandOff + MeshReach);
	// 下界也取 -Reach：摆件落在**地面**上，而房子坐在 footprint 内的最高点，
	// 所以墙脚那一圈可以比房底低不少（低多少由地形说了算，不是常数）。
	FBox LocalBounds(FVector(-Reach, -Reach, -Reach), FVector(Reach, Reach, Top));
	if (!bForceFullRebuild && DecorHandedLocalBounds.IsValid) LocalBounds += DecorHandedLocalBounds;

	bool bNeedHandover = DecorHandedCapacities.Num() != DecorGpuBuffers.Num()
		|| !DecorHandedLocalBounds.IsValid
		|| !DecorHandedLocalBounds.Min.Equals(LocalBounds.Min, 1.0)
		|| !DecorHandedLocalBounds.Max.Equals(LocalBounds.Max, 1.0);
	for (int32 Index = 0; !bNeedHandover && Index < DecorGpuBuffers.Num(); ++Index)
	{
		// 蓝图重跑构造脚本会销毁并重建实例组件：新组件身上没有实例源，缓存说"已交接"就会
		// 永远画不出东西 —— 拿组件自己的状态兜底（同 EnsureVineComponents）。
		bNeedHandover = DecorHandedCapacities[Index] != DecorGpuBuffers[Index].Capacity
			|| !IsValid(DecorComponents[Index])
			|| !DecorComponents[Index]->HasInstanceSourceGPU();
	}
	if (!bNeedHandover) return;

	for (int32 Index = 0; Index < DecorGpuBuffers.Num(); ++Index)
	{
		if (!DecorGpuBuffers[Index].IsValid() || !IsValid(DecorComponents[Index])) return;
	}

	for (int32 Index = 0; Index < DecorGpuBuffers.Num(); ++Index)
	{
		FCSGpuInstanceSourceGPU Source;
		Source.PackedInstances = DecorGpuBuffers[Index].PackedInstances;   // 保留自己的引用，重打包还要用
		Source.Counter = DecorGpuBuffers[Index].Counter;
		Source.Capacity = DecorGpuBuffers[Index].Capacity;
		Source.LocalBounds = LocalBounds;
		DecorComponents[Index]->SetInstanceSourceGPU(Source);
	}

	DecorHandedCapacities.SetNumUninitialized(DecorGpuBuffers.Num());
	for (int32 Index = 0; Index < DecorGpuBuffers.Num(); ++Index)
	{
		DecorHandedCapacities[Index] = DecorGpuBuffers[Index].Capacity;
	}
	DecorHandedLocalBounds = LocalBounds;
}

void ACSHouseActor::RebuildDecor()
{
	auto HasAny = [](const TArray<TObjectPtr<UStaticMesh>>& Meshes)
	{
		for (const TObjectPtr<UStaticMesh>& Mesh : Meshes)
		{
			if (Mesh) return true;
		}
		return false;
	};
	const bool bAnyMesh = HasAny(DecorGateMeshes) || HasAny(DecorWallFootMeshes) || HasAny(DecorRoofMeshes);

	if (!bDecorEnabled || !bAnyMesh)
	{
		if (CurrentDecorInstanceCount != 0 || CurrentDecorAnchorCount != 0)
		{
			// ⚠️ 同 `RebuildVine` / `RebuildFrame`：撤实例源之前先清 counter，
			// 否则下一次 `EnsureDecorComponents` 把同一批带陈旧计数器的 buffer 交回组件。
			CSShaperSteps::ZeroCounters(DecorGpuBuffers);
			for (const TObjectPtr<UCSGpuInstancedMeshComponent>& Component : DecorComponents)
			{
				if (IsValid(Component)) Component->ClearInstanceSourceGPU();
			}
			DecorHandedCapacities.Reset();
			DecorHandedLocalBounds = FBox(ForceInit);
			CurrentDecorInstanceCount = 0;
			CurrentDecorAnchorCount = 0;
			CurrentDecorGateAnchorCount = 0;
			DecorDescHash = 0;
		}
		return;
	}

	EnsureDecorComponents();
	if (!bDecorBaseMeshReady || DecorGpuBuffers.Num() == 0) return;

	CSHouseDecor::FSite Site;
	BuildDecorSite(Site);
	const CSHouseDecor::FParams Params = MakeDecorParams();

	TArray<CSHouseDecor::FAnchor> Anchors;
	CSHouseDecor::BuildAnchors(Site, Params, Anchors);

	CSHouseDecor::FPlan Plan;
	CSHouseDecor::BuildPlan(Anchors, Params, DecorPaletteRanges, DecorGpuBuffers.Num(), Plan);

	// 幂等短路。⚠️ 短路点在生产 + 规划**之后**是有意的（同藤蔓）：两步都是纯 CPU、微秒量级，
	// 而锚点表是哈希的**唯一诚实来源** —— 拿参数拼哈希而不跑生产，会在"参数没动但门开了 /
	// 地面被塑高了"时静默漏更新，而这两件事恰恰都会改锚点（门那一家的存亡、每个锚的落高）。
	// 所以哈希直接盖住锚点的身份与量化后的位置，地形抬高一厘米也逃不掉。
	TArray<int32> HashInput;
	HashInput.Reserve(Anchors.Num() * 5 + 20);
	HashInput.Append({ int32(ComputePlacementHash() & 0x7FFFFFFF), Anchors.Num(), Plan.TotalRecords(),
		CSHouse_Q(FootprintSize.X, 1), CSHouse_Q(FootprintSize.Y, 1), CSHouse_Q(WallHeight, 1),
		CSHouse_Q(RoofPitch, 0.1), CSHouse_Q(RoofOverhang, 1), int32(RidgeAxis),
		CSHouse_Q(DecorWallFootSpacing, 0.5), CSHouse_Q(DecorEaveSpacing, 0.5),
		CSHouse_Q(DecorMinSpacing, 0.5), CSHouse_Q(DecorRoadReject, 0.01),
		CSHouse_Q(DecorScale, 0.01), CSHouse_Q(DecorScaleJitter, 0.01), DecorSeed });
	for (const CSHouseDecor::FAnchor& Anchor : Anchors)
	{
		HashInput.Append({ int32(Anchor.Family), Anchor.AnchorId,
			CSHouse_Q(Anchor.Location.X, 1), CSHouse_Q(Anchor.Location.Y, 1), CSHouse_Q(Anchor.Location.Z, 1) });
	}
	const uint32 NewHash = CSHouse_Hash(HashInput);

	bool bBuffersReady = DecorGpuBuffers.Num() == DecorComponents.Num();
	for (const CSShaperSteps::FPaletteBuffers& Buffers : DecorGpuBuffers) bBuffersReady &= Buffers.IsValid();
	if (NewHash == DecorDescHash && DecorHandedCapacities.Num() == DecorGpuBuffers.Num() && bBuffersReady) return;

	DecorDescHash = NewHash;
	CurrentDecorAnchorCount = Anchors.Num();
	CurrentDecorGateAnchorCount = 0;
	for (const CSHouseDecor::FAnchor& Anchor : Anchors)
	{
		if (Anchor.Family == CSHouseDecor::EFamily::Gate) ++CurrentDecorGateAnchorCount;
	}
	CurrentDecorInstanceCount = Plan.TotalRecords();

	// 世界 → 组件。⚠️ **用组件自己的变换求逆**，不用 actor 的：已删的门框旧路混用
	// `GetBuildTransform()`（只取 yaw）与 actor 的完整逆变换，正是状态文件「已知潜伏问题」
	// 里那条（房子一旦被 pitch/roll 或缩放就错位）。这里不重复它。
	// 各 palette 的组件是同一棵挂接树上的兄弟、变换相同，取第 0 个即可。
	const FMatrix44f WorldToComponent = FMatrix44f(
		DecorComponents[0]->GetComponentTransform().ToInverseMatrixWithScale());
	CSHouseDecor::Pack(Plan, DecorGpuBuffers, WorldToComponent);

	UE_LOG(LogTinyGladeHouse, Log, TEXT("[TinyGladeHouse] %s decor packed: anchors=%d instances=%d palettes=%d"),
		*GetName(), CurrentDecorAnchorCount, CurrentDecorInstanceCount, DecorGpuBuffers.Num());
}

bool ACSHouseActor::IsDecorDrawable(FString& OutReason) const
{
	OutReason = GetDecorUndrawableReason();
	if (!OutReason.IsEmpty())
	{
		// ⚠️ **原因串到不了脚本里，必须自己打一行日志**（实测）：UE Python 把
		// "bool 返回值 + 一个 out 参数"的调用收成单一返回值 —— 可画时拿到空串，
		// **不可画时拿到的是 `None`，原因串直接丢了**。于是出图/回归的红灯只能说
		// "画不出来"而说不出为什么，而这个函数存在的全部价值就在那句原因上。
		// `IsVineDrawable` / `IsRockShellDrawable` 同形、同样盲，2026-08-30 已一并补上
		// `Get*UndrawableReason()`：三处的脚本一律调那一版，原因才真的到得了日志与断言里。
		UE_LOG(LogTinyGladeHouse, Warning, TEXT("[TinyGladeHouse] %s decor not drawable: %s"), *GetName(), *OutReason);
	}
	return OutReason.IsEmpty();
}

FString ACSHouseActor::GetDecorUndrawableReason() const
{
	FString OutReason;
	// 逐环检查渲染那一侧 —— readback 断言对这些一个字都说不了（见头文件里那段教训）。
	if (!bDecorEnabled) { OutReason = TEXT("bDecorEnabled 关着"); return OutReason; }
	if (DecorComponents.Num() == 0) { OutReason = TEXT("一个 palette 都没有（三家的网格都空着）"); return OutReason; }
	if (!bDecorBaseMeshReady) { OutReason = TEXT("基础网格快照没建起来（读不到 LOD0 顶点？）"); return OutReason; }
	if (CurrentDecorAnchorCount <= 0) { OutReason = TEXT("一个锚点都没生产出来"); return OutReason; }
	if (CurrentDecorInstanceCount <= 0) { OutReason = TEXT("锚点全被填充概率/间距球筛掉了"); return OutReason; }

	for (int32 Index = 0; Index < DecorComponents.Num(); ++Index)
	{
		const UCSGpuInstancedMeshComponent* Component = DecorComponents[Index];
		if (!IsValid(Component)) { OutReason = FString::Printf(TEXT("palette %d：没有渲染组件"), Index); return OutReason; }
		if (!Component->IsRegistered()) { OutReason = FString::Printf(TEXT("palette %d：渲染组件没注册"), Index); return OutReason; }
		if (!Component->IsVisible()) { OutReason = FString::Printf(TEXT("palette %d：渲染组件不可见"), Index); return OutReason; }
		if (!Component->HasInstanceSourceGPU()) { OutReason = FString::Printf(TEXT("palette %d：实例源没交接"), Index); return OutReason; }
		if (Component->GetBaseMeshSnapshot().Positions.Num() < 3)
		{
			OutReason = FString::Printf(TEXT("palette %d：基础网格快照是空的"), Index);
			return OutReason;
		}
		if (!Component->GetGpuMesh()) { OutReason = FString::Printf(TEXT("palette %d：GPU 网格没分配"), Index); return OutReason; }

		// **这一条就是石阶那个坑**：材质为空时组件仍然会画，只是退回引擎默认表面材质 ——
		// 画面上是一片灰，而所有 readback 断言照绿。
		const UMaterialInterface* Material = Component->InstanceMaterial;
		if (!Material)
		{
			OutReason = FString::Printf(TEXT("palette %d：没有绑材质（会用引擎默认表面材质画成一片灰）"), Index);
			return OutReason;
		}
		// ⚠️ 比石阶那条**多一环**：没勾 `bUsedWithInstancedStaticMeshes` 的材质在实例路径上
		// 会被引擎**静默替换**成默认材质，症状与"没绑材质"逐像素相同。藤蔓那轮就是被
		// `M_TG_Texture`（459 个 MI 的母材质，没勾）绊过 —— clutter 的材质同样别去挂它。
		const UMaterial* Base = Material->GetMaterial();
		if (!Base || !Base->bUsedWithInstancedStaticMeshes)
		{
			// ⚠️ 编辑器里引擎会在第一次实例化使用时**自己把这个标志勾回去**并重编（实测：
			// 手动摩掉它再出图，画面逐像素不变）。所以这一条在编辑器里很难抵到东西，
			// 真正会发作的是烘培/非编辑器路径。留着不代表它白写 —— 但也别拿它当唯一的门。
			OutReason = FString::Printf(
				TEXT("palette %d：材质 '%s' 的母材质没有勾 bUsedWithInstancedStaticMeshes（引擎会静默换成默认材质）"),
				Index, *Material->GetName());
			return OutReason;
		}
	}
	return FString();
}

// -----------------------------------------------------------------------------
// GPU 侧真值（诊断 / 验收专用，阻塞）—— 声明处那段注释是这几条为什么存在
// -----------------------------------------------------------------------------

namespace
{
/** 组件不在（还没建 / 被构造脚本销毁）时读到的就是 0：那一路确实一个实例都不画。 */
int32 CSHouse_ReadGpuInstanceCount(const UCSGpuInstancedMeshComponent* Component)
{
	return IsValid(Component) ? Component->DebugReadDrawnInstanceCountSync() : 0;
}
} // namespace

int32 ACSHouseActor::DebugReadFrameBrickCountGpuSync() const
{
	return CSHouse_ReadGpuInstanceCount(FrameComponent);
}

int32 ACSHouseActor::DebugReadVineBranchCountGpuSync() const
{
	return CSHouse_ReadGpuInstanceCount(VineBranchComponent);
}

int32 ACSHouseActor::DebugReadVineLeafCountGpuSync() const
{
	return CSHouse_ReadGpuInstanceCount(VineLeafComponent);
}

int32 ACSHouseActor::DebugReadVineFlowerCountGpuSync() const
{
	return CSHouse_ReadGpuInstanceCount(VineFlowerComponent);
}

int32 ACSHouseActor::DebugReadDecorInstanceCountGpuSync() const
{
	int32 Total = 0;
	for (const TObjectPtr<UCSGpuInstancedMeshComponent>& Component : DecorComponents)
	{
		const int32 Count = CSHouse_ReadGpuInstanceCount(Component);
		// 一路读不到就整体作废：把它当 0 加进去会让总数看着"少了一点"而不是"坏了"，
		// 而这一族 bug 最典型的失效方式就是被当成小偏差放过去。
		if (Count < 0) return -1;
		Total += Count;
	}
	return Total;
}

FString ACSHouseActor::DebugGetGpuAssetMismatchSync() const
{
	auto Check = [](const UCSGpuInstancedMeshComponent* Component, const FString& Label) -> FString
	{
		// 没有组件不算"画错了"——那是 `IsXxxDrawable` 那一族的职责范围，这里只答"画的是不是那个"。
		if (!IsValid(Component)) return FString();
		const FString Reason = Component->DebugGetDrawnAssetMismatchSync();
		return Reason.IsEmpty() ? FString() : FString::Printf(TEXT("%s：%s"), *Label, *Reason);
	};

	if (CurrentFrameBrickCount > 0)
	{
		const FString Reason = Check(FrameComponent, TEXT("门框砖"));
		if (!Reason.IsEmpty()) return Reason;
	}
	if (CurrentVineSegmentCount > 0)
	{
		FString Reason = Check(VineBranchComponent, TEXT("藤枝"));
		if (!Reason.IsEmpty()) return Reason;
		Reason = Check(VineLeafComponent, TEXT("藤叶"));
		if (!Reason.IsEmpty()) return Reason;
	}
	// 花单独判：一栋不长花的房子（`VineFlowerMesh` 留空）是**合法**的，拿枝数当门会把它误判。
	if (CurrentVineFlowerCount > 0)
	{
		const FString Reason = Check(VineFlowerComponent, TEXT("藤花"));
		if (!Reason.IsEmpty()) return Reason;
	}
	if (CurrentDecorInstanceCount > 0)
	{
		for (int32 Index = 0; Index < DecorComponents.Num(); ++Index)
		{
			const FString Reason = Check(DecorComponents[Index], FString::Printf(TEXT("摆件[%d]"), Index));
			if (!Reason.IsEmpty()) return Reason;
		}
	}
	return FString();
}

#if WITH_EDITOR

bool ACSHouseActor::DebugBakeFrameBricksSync(const FString& AssetPath, int32& OutTriangles,
	int32& OutVertexInstances, int32& OutUVChannels, int32& OutDistinctBakedRandoms,
	int32& OutGpuInstanceCount, bool& bOutRandomsMatchGpu)
{
	OutTriangles = 0;
	OutVertexInstances = 0;
	OutUVChannels = 0;
	OutDistinctBakedRandoms = 0;
	OutGpuInstanceCount = 0;
	bOutRandomsMatchGpu = false;
	if (!IsValid(FrameComponent)) return false;

	// 先把 GPU 那一侧的真值取到手，再烘 —— 顺序反过来的话，烘焙本身若不小心动了实例源，
	// 后读到的就是被自己改过的值，断言等于自证。
	TArray<float> GpuRandoms;
	if (!FrameComponent->DebugReadInstanceRandomsSync(GpuRandoms)) return false;
	OutGpuInstanceCount = GpuRandoms.Num();

	// 走组件自己的出口，不另拼一条：要证的正是"**那条**出口带不带得走通道"。
	// BakeSpace 取本 actor 的变换 —— 实例原点是组件空间的，出口内部先升世界再烘回这里的局部，
	// 资产摆在同一个变换上就能复现画面（同 RockShell / RoadMesh 的口径）。
	UStaticMesh* Baked = FrameComponent->SaveToStaticMesh(
		GetActorTransform(), AssetPath, /*bReplaceExistingAsset*/ true, /*bSaveAsset*/ false);
	if (!Baked) return false;

	const FMeshDescription* Description = Baked->GetMeshDescription(0);
	if (!Description) return false;

	FStaticMeshConstAttributes Attributes(*Description);
	TVertexInstanceAttributesConstRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
	TVertexInstanceAttributesConstRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
	if (!Colors.IsValid() || !UVs.IsValid()) return false;

	OutTriangles = Description->Triangles().Num();
	OutVertexInstances = Description->VertexInstances().Num();
	OutUVChannels = UVs.GetNumChannels();

	// 8 位量化后去重：顶点色最终就存成 FColor，比"浮点相等"更贴近资产里真正留下的东西，
	// 也让下面与 GPU 值的比对有一条明确的容差（±1/255），不必猜浮点误差。
	auto Quantize = [](float Value) { return int32(FMath::RoundToInt(FMath::Clamp(Value, 0.0f, 1.0f) * 255.0f)); };
	TSet<int32> BakedRandoms;
	for (const FVertexInstanceID InstanceID : Description->VertexInstances().GetElementIDs())
	{
		BakedRandoms.Add(Quantize(Colors[InstanceID].W));
	}
	OutDistinctBakedRandoms = BakedRandoms.Num();

	// 逐个对：GPU 上每一行的 `Origin.w` 都必须在烘焙件的 alpha 集合里找得到。
	// 反向不查 —— 同一个 8 位桶可能被两个实例共用（512 个实例挤 256 个桶），
	// 那不是缺陷；"GPU 有而烘焙件没有"才是通道被丢掉的证据。
	bOutRandomsMatchGpu = GpuRandoms.Num() > 0;
	for (const float Random : GpuRandoms)
	{
		const int32 Bucket = Quantize(Random);
		if (!BakedRandoms.Contains(Bucket) && !BakedRandoms.Contains(Bucket - 1) && !BakedRandoms.Contains(Bucket + 1))
		{
			bOutRandomsMatchGpu = false;
			break;
		}
	}
	return true;
}

int32 ACSHouseActor::SaveInstancedToStaticMeshes(const FString& BakeFolder, bool bSaveAssets)
{
	const FString Folder = BakeFolder.TrimStartAndEnd().IsEmpty()
		? FString::Printf(TEXT("/Game/TinyGladeBake/%s"), *GetName())
		: BakeFolder.TrimStartAndEnd();

	int32 Saved = 0;
	auto BakeOne = [this, &Folder, bSaveAssets, &Saved](UCSGpuInstancedMeshComponent* Component, const TCHAR* Family)
	{
		if (!IsValid(Component)) return;
		const FString Path = FString::Printf(TEXT("%s/SM_%s_%s"), *Folder, *GetName(), Family);
		// 烘回本 actor 的局部空间：资产摆在同一个变换上就复现画面（同岩壳 / 道路那两条出口）。
		if (Component->SaveToStaticMesh(GetActorTransform(), Path, /*bReplaceExistingAsset*/ true, bSaveAssets))
		{
			++Saved;
		}
	};

	BakeOne(FrameComponent, TEXT("FrameBricks"));   // 门框砖 + 接缝砖共用这一个组件
	BakeOne(VineBranchComponent, TEXT("VineBranch"));
	BakeOne(VineLeafComponent, TEXT("VineLeaf"));
	BakeOne(VineFlowerComponent, TEXT("VineFlower"));
	for (int32 Index = 0; Index < DecorComponents.Num(); ++Index)
	{
		BakeOne(DecorComponents[Index], *FString::Printf(TEXT("Decor%d"), Index));
	}

	UE_LOG(LogTinyGladeHouse, Log, TEXT("[TinyGladeHouse] %s 实例路烘焙：%d 张资产 -> %s"),
		*GetName(), Saved, *Folder);
	return Saved;
}

UStaticMesh* ACSHouseActor::DebugBakeVineBranchesSync(const FString& AssetPath)
{
	if (!IsValid(VineBranchComponent)) return nullptr;
	return VineBranchComponent->SaveToStaticMesh(
		GetActorTransform(), AssetPath, /*bReplaceExistingAsset*/ true, /*bSaveAsset*/ false);
}

void ACSHouseActor::DebugSetVineBranchInstancesHidden(bool bHideInstances)
{
	// SetHiddenInGame 在编辑器视口/离屏捕获里不起作用（那两条走的是编辑器可见性），
	// 所以用 SetVisibility —— 它两边都算数。
	if (IsValid(VineBranchComponent)) VineBranchComponent->SetVisibility(!bHideInstances, /*bPropagateToChildren*/ true);
}

#endif // WITH_EDITOR

// -----------------------------------------------------------------------------
// AActor
// -----------------------------------------------------------------------------

void ACSHouseActor::BindHouseMaterials()
{
	BindTinyGladeMaterials({ WallMaterial, RoofMaterial });
	if (PillarMesh)
	{
		if (PillarMeshComponent) PillarMeshComponent->MeshMaterial = PillarMaterial;
		PillarMesh->Materials.SetNum(FMath::Max(PillarMesh->Materials.Num(), 1));
		PillarMesh->Materials[0] = PillarMaterial;
		PillarMesh->NotifyMaterialsChanged();
	}

	// 门框砖走另一条组件（实例化），漏了它的症状与 D14 开篇描述的一模一样：在细节面板里改
	// FrameMaterial 静默无效，必须手点 RebuildHouse()。这里补上，重绑不重建的纪律才算完整。
	// 代理在构造时就把 InstanceMaterial 抄走了（FCSGpuInstancedMeshSceneProxy 的初始化列表），
	// 光写属性不重建代理是看不出变化的 —— 必须自己脏一次渲染状态。
	if (IsValid(FrameComponent) && FrameComponent->InstanceMaterial != FrameMaterial)
	{
		FrameComponent->InstanceMaterial = FrameMaterial;
		FrameComponent->MarkRenderStateDirty();
	}
}

void ACSHouseActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	ReevaluateSite();
}

void ACSHouseActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	if (IsTemplate() || !GetWorld()) return;

	// 先重求值再登记：登记时基线取当前变换，而落座就在重求值里发生 —— 顺序反了会让
	// subsystem 在下一帧凭空唤醒一次（幂等所以无害，但那正是要避免的空转）。
	if (!HouseId.IsValid()) HouseId = FGuid::NewGuid();
	ReevaluateSite();
	if (UCSHouseSubsystem* Subsystem = GetWorld()->GetSubsystem<UCSHouseSubsystem>()) Subsystem->RegisterHouse(this);
}

void ACSHouseActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnsubscribeGround();
	if (UWorld* World = GetWorld())
	{
		if (UCSHouseSubsystem* Subsystem = World->GetSubsystem<UCSHouseSubsystem>()) Subsystem->UnregisterHouse(this);
	}
	Super::EndPlay(EndPlayReason);
}

void ACSHouseActor::BeginDestroy()
{
	UnsubscribeGround();
	Super::BeginDestroy();
}

#if WITH_EDITOR
void ACSHouseActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// 材质是纯外观量：只重绑，不重建（计划 D14）。走 ReevaluateSite 的话 BodyDescHash 不含
	// 任何材质 ⇒ 哈希不变 ⇒ 跳过重建 ⇒ 材质从未被重新绑定，画面零变化。
	const FName Name = PropertyChangedEvent.GetPropertyName();
	if (Name == GET_MEMBER_NAME_CHECKED(ACSHouseActor, WallMaterial)
		|| Name == GET_MEMBER_NAME_CHECKED(ACSHouseActor, RoofMaterial)
		|| Name == GET_MEMBER_NAME_CHECKED(ACSHouseActor, PillarMaterial)
		|| Name == GET_MEMBER_NAME_CHECKED(ACSHouseActor, FrameMaterial))
	{
		BindHouseMaterials();
	}
}

void ACSHouseActor::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);

	// 连续 N 次增量 TransformMesh 会攒浮点误差：松手做一次全量重建对齐并清零
	// （ACSGroundActor::PostEditMove 已经是这个模式，照抄）。拖动中走摆位快路径。
	if (bFinished) bForceFullRebuild = true;
	ReevaluateSite();
}

void ACSHouseActor::PostEditUndo()
{
	Super::PostEditUndo();

	// AActor::PostEditUndo（ActorEditor.cpp）只做 InternalPostEditUndo + 一条
	// UpdateAllPrimitiveSceneInfos —— 不调 PostEditMove、不保证跑构造脚本，所以重建必须放这。
	// BodyDescHash 含量化世界变换，撤销一次移动后不重求值就会画在旧位置。
	ReevaluateSite();
}
#endif
