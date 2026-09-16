#include "CSHouseActor.h"

#include "CSGpuInstancedMeshComponent.h"
#include "CSGpuMeshTypes.h"
#include "CSGroundActor.h"
#include "CSHouseQuoin.h"
#include "CSHouseResize.h"
#include "CSHouseHeightHandleActor.h"   // D5 高度框（unity 构建下别指望别人替你带进来）
#include "CSHouseResizeHandleActor.h"   // D5 水平锥子（同上）
#include "CSHouseFeatureMarker.h"       // D8 附属物：重建后要回推裁决 + 按锚点吸附
#include "CSHouseBrickWall.h"           // D4 两层之 A：砖层 = 一摞包边带
#include "CSHouseSeam.h"   // IdLess —— 标记登记表的确定次序（unity 构建下别指望别人替你带进来）
#include "CSHouseRoof.h"
#include "CSHouseTrim.h"
#include "CSHouseSubsystem.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "CSMesh.h"
#include "CSMeshOps.h"
#include "CSMeshRenderComponent.h"
#include "CSHousePillar.h"   // 砖石柱（unity 构建下别指望别人替你带进来）
#include "CSVineTube.h"   // 折线 → 管子的对外入口（unity 构建下别指望别人替你带进来）
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
	 * 只要求**右手**，即 (X×Y)·Z > 0，**不要求正交**（剪切过的块也吃得下）。
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
	 * 任意**平面**四边形：角点按环序 P0 → P1 → P2 → P3，法线 = (P1−P0)×(P3−P0) 的方向。
	 * 与 `AddQuad(A, U, V)` 同一套三角划分与 UV 约定（取 P0 处两条邻边的长度），
	 * 平行四边形时两者产出相同的角点。
	 */
	void AddQuad4(const FVector& P0, const FVector& P1, const FVector& P2, const FVector& P3, int32 Slot)
	{
		const float LU = float((P1 - P0).Size()) / CSHouse_UVScale;
		const float LV = float((P3 - P0).Size()) / CSHouse_UVScale;
		AddTri(P0, P1, P2, Slot, { 0, 0 }, { LU, 0 }, { LU, LV });
		AddTri(P0, P2, P3, Slot, { 0, 0 }, { LU, LV }, { 0, LV });
	}

	/**
	 * 一段墙板：外皮 S 区间 `[SA, SB]`、内皮 S 区间 `[SAin, SBin]`、高度 `[ZLo, ZHi]`、厚 `T`。
	 *
	 * 这就是斜接之后的面板形状：外皮与内皮都是矩形，只有两个端面可能是斜的（沿角平分线切）。
	 * 六个面的角点顺序与 `AddBox(Start + U·SA + Up·ZLo, U·(SB−SA), In·T, Up·(ZHi−ZLo))` 逐面对应，
	 * 所以 `SAin == SA && SBin == SB` 时它就是原来那个盒子。
	 */
	void AddWallPrism(const FVector& Start, const FVector& U, const FVector& In, const FVector& Up,
		float SA, float SB, float SAin, float SBin, float T, float ZLo, float ZHi, int32 Slot)
	{
		const FVector Lo = Up * ZLo, Hi = Up * ZHi, Deep = In * T;
		const FVector O0 = Start + U * SA + Lo,           O1 = Start + U * SB + Lo;
		const FVector I0 = Start + U * SAin + Deep + Lo,  I1 = Start + U * SBin + Deep + Lo;
		const FVector O0t = Start + U * SA + Hi,          O1t = Start + U * SB + Hi;
		const FVector I0t = Start + U * SAin + Deep + Hi, I1t = Start + U * SBin + Deep + Hi;

		AddQuad4(O0, I0, I1, O1, Slot);        // bottom (-Z)
		AddQuad4(O0t, O1t, I1t, I0t, Slot);    // top (+Z)
		AddQuad4(O0, O1, O1t, O0t, Slot);      // 外皮（背离房内）
		AddQuad4(I0, I0t, I1t, I1, Slot);      // 内皮（朝向房内）
		AddQuad4(O0, O0t, I0t, I0, Slot);      // 起点端面（斜接时是斜的）
		AddQuad4(O1, I1, I1t, O1t, Slot);      // 终点端面
	}

};

// `FCSHouseEdgeFrame` / `CSHouse_GetEdge` 已上提到 `CSHouseProfile.h`：谓词（`CSHouse_QueryOpening`）
// 与单测也要问"这面墙在哪、有多长"，而墙在哪只能有一个真源。

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
 * footprint 进哈希：顶点数 + 逐顶点量化（口径与理由见 `FCSHouseFootprint::AppendQuantizedHash`）。
 *
 * 原来各家哈希写的是 `CSHouse_Q(FootprintSize.X, 1), CSHouse_Q(FootprintSize.Y, 1)` —— 两个数只能
 * 描述居中矩形。⚠️ 2026-09-14 起顶点量子从 1 cm 改成 0.5 cm：矩形上 1 cm 的顶点量子等于 2 cm 的尺寸量子，
 * 比折线化之前粗了一倍。量化只留一份，单测直接钉那个成员函数。
 */
void CSHouse_AppendFootprintHash(TArray<int32>& H, const FCSHouseFootprint& Footprint)
{
	Footprint.AppendQuantizedHash(H);
}

/**
 * 藤蔓管子的环半径系数。**两个调用点必须用同一个值**：`PackTubePath` 按
 * `w = 想要的半径 / (10 * CircleScale)` 反解逐点缩放，Pass C 再按
 * `半径 = 10 * CircleScale * w` 还原。取值本身是任意的（两边约掉了），
 * 所以它是个常量而不是属性 —— 暴露出去只会让人以为调它能改粗细（那是 `VineThickness`），
 * 而真正的后果是两边取不同值时管子整体差一个常数倍、且两边各自都自洽。
 */
constexpr float CSHouseVine_TubeCircleScale = 0.2f;

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
	// 合批唤醒的兑现点（见 `RequestReevaluate`）。基类默认 `bCanEverTick = false`，这里自己打开；
	// 平时关着、有欠账才开。编辑器里要真的 tick，还得配 `ShouldTickIfViewportsOnly`（同特征标记）。
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	PillarMeshComponent = CreateDefaultSubobject<UCSMeshRenderComponent>(TEXT("PillarMesh"));
	PillarMeshComponent->SetupAttachment(RootComponent);

	// 藤蔓管子。几何在世界空间 ⇒ 相对变换必须钉成恒等（组件的变换已被构造函数标成绝对，
	// 所以"相对"在这里就是世界）。与 `CSVineTube::BuildTubeIntoMesh` 里的
	// `VineWorldToLocal = Identity` 是同一条约定的两半，必须一起动。
	VineTubeComponent = CreateDefaultSubobject<UCSMeshRenderComponent>(TEXT("VineTubeMesh"));
	VineTubeComponent->SetupAttachment(RootComponent);
	VineTubeComponent->SetRelativeTransform(FTransform::Identity);
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
	// 走合批：一次落笔会让地面把这条广播给每一栋受影响的房子，而画笔是按住不放的
	// —— 同一栋房子一帧里收到多次是常态。
	RequestReevaluate();
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
	const FCSHouseFootprint Footprint = GetFootprint();
	const FBox2D Box = Footprint.GetBounds();
	if (!Box.bIsValid) return Loc.Z;
	const FVector2D Size = Box.GetSize();
	const double Step = 50.0;   // 与地面镜像默认格距同阶；max 采样对格距不敏感

	double MaxGround = TNumericLimits<double>::Lowest();
	auto SampleAt = [&](const FVector2D& L)
	{
		const FVector2D P = FVector2D(Loc.X, Loc.Y) + AxX * L.X + AxY * L.Y;
		MaxGround = FMath::Max(MaxGround, double(Ground->SampleHeight(P)));
	};
	const int32 NX = FMath::Max(1, int32(FMath::CeilToInt(Size.X / Step)));
	const int32 NY = FMath::Max(1, int32(FMath::CeilToInt(Size.Y / Step)));
	for (int32 ix = 0; ix <= NX; ++ix)
	{
		for (int32 iy = 0; iy <= NY; ++iy)
		{
			const FVector2D L(Box.Min.X + Size.X * ix / NX, Box.Min.Y + Size.Y * iy / NY);
			// 包围盒网格只采落在折线内（含边界）的点：非矩形折线的包围盒角上是房外的空地，
			// 采了会把房子顶到那块地的高度上去。矩形的网格点全在内部或边上，一个都不会被跳过。
			if (Footprint.ContainsPoint(L, 0.5)) SampleAt(L);
		}
	}
	// 顶点单独补采：锐角顶点可能夹在两个网格点之间，漏掉它就漏掉角上那块凸起。
	// 矩形的四个角本来就是网格点，补采不改最大值。
	for (const FVector2D& V : Footprint.Verts) SampleAt(V);
	if (MaxGround <= TNumericLimits<double>::Lowest() * 0.5) return Loc.Z;
	return MaxGround + HeightOffset;
}

void ACSHouseActor::ReanchorMarkersToPreserveWorld()
{
	const FTransform Build = GetBuildTransform();
	const FCSHouseFootprint Footprint = GetFootprint();

	// **只在墙几何变了的时候做。** 纯平移 / 旋转不做 —— 标记 attach 在房子下，场景图已经带着
	// 它一起走了；这时再"守恒世界位置"等于把窗从房子上扯下来留在原地。
	const bool bGeomChanged = !MarkerRefFootprint.EqualsApprox(Footprint, 0.01)
		|| !FMath::IsNearlyEqual(MarkerRefThickness, WallThickness, 0.01f);

	if (bMarkerRefValid && bGeomChanged)
	{
		for (FCSMarkerWindow& Entry : MarkerWindows)
		{
			FCSWallAnchor A = Entry.Anchor;
			if (!A.IsValidAnchor()) continue;

			// ① 旧墙上那个点，取到世界里（旧 footprint + 旧构建变换）。
			const FCSHouseEdgeFrame OldF = CSHouse_GetEdge(A.EdgeIndex, MarkerRefFootprint, MarkerRefThickness);
			const float OldS = CSHouse_AnchorS(A, MarkerRefFootprint, MarkerRefThickness);
			const FVector2D OldP2 = OldF.Start + OldF.U * double(OldS);
			const FVector OldWorld = MarkerRefBuild.TransformPosition(FVector(OldP2.X, OldP2.Y, 0.0));

			// ② 投到新墙上，只取**沿墙**分量。法向那一维故意丢掉：墙沿自己的法线挪时窗必须跟着走
			//    （否则窗会留在半空），要守恒的只有"窗在这面墙上的哪个位置"。
			const FCSHouseEdgeFrame NewF = CSHouse_GetEdge(A.EdgeIndex, Footprint, WallThickness);
			const FVector NewStart = Build.TransformPosition(FVector(NewF.Start.X, NewF.Start.Y, 0.0));
			const FVector NewU = Build.TransformVectorNoScale(FVector(NewF.U.X, NewF.U.Y, 0.0)).GetSafeNormal();
			const float NewS = FMath::Clamp(float(FVector::DotProduct(OldWorld - NewStart, NewU)), 0.0f, NewF.Len);

			// ③ 重新表达。近角约定照旧（存哪一端只是度量方式，物理点没变）。
			A.bFromEndCorner = NewS > NewF.Len * 0.5f;
			A.DistFromCorner = A.bFromEndCorner ? (NewF.Len - NewS) : NewS;
			// ⚠️ **口径必须一起改写成斜接**：`NewS` 是在斜接框架上量的。旧锚点（口径 0，斜接之前存的档）
			// 留着 0 的话，`CSHouse_AnchorS` 在奇数边上会把这个新距离**再补一个 T** ——
			// 每改一次墙几何（拖边的每一帧），东西墙上的窗就沿墙滑一个墙厚，而且越滑越远。
			// `OldS` 那一步已经按旧口径换算过了，所以这里恒写 1。单测 `House.LegacyAnchorSurvivesResize`。
			A.SConvention = 1;

			Entry.Anchor = A;
			if (ACSHouseFeatureMarker* M = Entry.Marker.Get()) M->Reanchor(A);
		}
	}

	MarkerRefFootprint = Footprint;
	MarkerRefThickness = WallThickness;
	MarkerRefBuild = Build;
	bMarkerRefValid = true;
}

void ACSHouseActor::BuildWindowOpenings(TArray<FCSWallOpening>& OutCandidates) const
{
	OutCandidates.Reset();
	if (!bWindowsEnabled) return;

	// 循环外取一次：标记那半要逐条把锚点解成弧长，每条都重建一遍折线没有意义。
	const FCSHouseFootprint Footprint = GetFootprint();

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

	// 标记登记的那一半（D8）。与上面那半**完全同构**，只有身份的来源不同：
	// 属性面板那份没有别的稳定身份，只能拿槽位；标记自己带 GUID，直接用它 ——
	// 计划 D8 明写 `SourceId = 标记 GUID`。用槽位会让"删掉列表中间一个标记"把后面每一扇窗的
	// 身份都平移一格 ⇒ 谓词的"自己不与自己冲突"错位、排序全序翻转，而且不会有断言报红。
	OutCandidates.Reserve(OutCandidates.Num() + MarkerWindows.Num());
	for (int32 Index = 0; Index < MarkerWindows.Num(); ++Index)
	{
		const FCSMarkerWindow& Entry = MarkerWindows[Index];
		FCSWallOpening Opening;
		Opening.Type = ECSOpeningType::Window;
		Opening.Shape = Entry.Window.Shape;
		Opening.EdgeIndex = Entry.Window.EdgeIndex;
		// ⚠️ **弧长从锚点现算，不用登记时缓存的那一份**（2026-09-06 修）。`CenterS` 是 footprint
		// 的函数，缓存下来一改尺寸就过期，而没有任何东西会去刷新它 —— 症状是框跑到新位置、洞
		// 留在原地，再复评多少次都不会自愈。宽/高/形状/窗台高与 footprint 无关，照旧用缓存的。
		Opening.CenterS = Entry.Anchor.IsValidAnchor()
			? CSHouse_AnchorS(Entry.Anchor, Footprint, WallThickness)
			: Entry.Window.CenterS;
		Opening.Width = Entry.Window.Width;
		Opening.Z0 = Entry.Window.SillZ;
		Opening.Z1 = Entry.Window.SillZ + Entry.Window.Height;
		Opening.SourceId = Entry.MarkerId;
		// Tag 与属性面板那半共用同一段（0x80 起），从列表尾部往回编，两半在 128 扇窗以内不会撞。
		Opening.Tag = uint8(0x80 | ((0x7F - Index) & 0x7F));
		OutCandidates.Add(Opening);
	}
}

FCSWallHit ACSHouseActor::RayHitWall(const FVector& WorldOrigin, const FVector& WorldDir, float MaxDistance) const
{
	const FTransform Build = GetBuildTransform();
	return CSHouse_RayHitWall(
		Build.InverseTransformPosition(WorldOrigin),
		Build.InverseTransformVectorNoScale(WorldDir.GetSafeNormal()),
		GetFootprint(), WallThickness, WallHeight, MaxDistance);
}

FCSWallHit ACSHouseActor::NearestWall(const FVector& WorldPoint, float MaxDistance) const
{
	return CSHouse_NearestWall(
		GetBuildTransform().InverseTransformPosition(WorldPoint),
		GetFootprint(), WallThickness, WallHeight, MaxDistance);
}

FTransform ACSHouseActor::AnchorToWorld(const FCSWallAnchor& InAnchor, float HalfHeight, float Standoff) const
{
	// `A * B` = 先 A 后 B ⇒ 局部到世界是 `Local * Build`。写反了房子一旦离开原点，窗就飞了。
	return CSHouse_AnchorToLocal(InAnchor, GetFootprint(), WallThickness, HalfHeight, Standoff)
		* GetBuildTransform();
}

void ACSHouseActor::RegisterFeatureMarker(const FGuid& MarkerId, const FCSHouseWindow& Demand,
	ACSHouseFeatureMarker* Marker)
{
	if (!MarkerId.IsValid()) return;

	// **按 MarkerId 升序保序插入**，不是追加：这张表喂进 openings，而 openings 的次序进形状
	// 哈希。按登记先后排的话，同一份世界状态换个加载顺序就换一份哈希 ⇒ 幂等短路时灵时不灵。
	int32 Index = 0;
	while (Index < MarkerWindows.Num() && CSHouseSeam::IdLess(MarkerWindows[Index].MarkerId, MarkerId)) ++Index;

	if (MarkerWindows.IsValidIndex(Index) && MarkerWindows[Index].MarkerId == MarkerId)
	{
		// 反引每次都刷新：同一个 MarkerId 的 actor 可能被撤销/重做换成新实例，
		// 攥着旧指针的话重建时就推不回去了（弱引用只会静静地失效，不报红）。
		if (Marker)
		{
			MarkerWindows[Index].Marker = Marker;
			MarkerWindows[Index].Anchor = Marker->GetAnchor();
		}
		if (MarkerWindows[Index].Window == Demand) return;   // 诉求没变：连标脏都不必
		MarkerWindows[Index].Window = Demand;
	}
	else
	{
		FCSMarkerWindow Entry;
		Entry.MarkerId = MarkerId;
		Entry.Window = Demand;
		Entry.Marker = Marker;
		if (Marker) Entry.Anchor = Marker->GetAnchor();
		MarkerWindows.Insert(MoveTemp(Entry), Index);
	}
	// **合批**：gizmo 多选拖 N 个窗时，这里一帧会被叫 N 次（每个标记的 Tick 各一次），
	// 而前 N−1 次的结果都会被后一次盖掉。见 `RequestReevaluate` 的注释。
	RequestReevaluate();
}

void ACSHouseActor::UnregisterFeatureMarker(const FGuid& MarkerId)
{
	const int32 Removed = MarkerWindows.RemoveAll(
		[&MarkerId](const FCSMarkerWindow& Entry) { return Entry.MarkerId == MarkerId; });
	// 合批：一次删多个窗（框选 + Delete）会一帧内连来 N 次，同 RegisterFeatureMarker。
	if (Removed > 0) RequestReevaluate();
}

FCSOpeningSite ACSHouseActor::MakeOpeningSite() const
{
	FCSOpeningSite Site;
	Site.Footprint = GetFootprint();
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
	TArray<FCSDoorRunMemory> NewMemory;

	const FVector Loc = GetActorLocation();
	const float YawRad = FMath::DegreesToRadians(GetActorRotation().Yaw);
	const FVector2D AxX(FMath::Cos(YawRad), FMath::Sin(YawRad));
	const FVector2D AxY(-FMath::Sin(YawRad), FMath::Cos(YawRad));
	auto ToWorld2D = [&](const FVector2D& L) { return FVector2D(Loc.X, Loc.Y) + AxX * L.X + AxY * L.Y; };

	const float MaxDoorHeight = FMath::Min(DoorHeight, WallHeight - LintelBand);
	if (Ground && MaxDoorHeight > DoorMinWidth * 0.5f)
	{
		// 2026-09-04 重做：**门宽 = 路在墙上截出的弦长**（TG 的
		// `ArchSegment (*)(WallPathSegment)`，逐条证据见 `CSHouseDoorRuns.h` 文件头）。
		// 旧口径是"等分槽 + 覆盖率二值投票"，路只能决定这一格开不开，宽度与位置都与路无关。
		// **四条边接成一条闭合周界**（2026-09-04 下午，用户："根据 4 条边组合一个环形边"）。
		// TG 那边墙本来就是一条闭合曲线（`Rectangle2d::circular_slice` 是环形切片），所以
		// 一条压过转角的路在那边得到的是**一条跨角的段**；四条边各自求解只会把它切成两个
		// 互不相干、还各自被护角推开的洞。转角实拍见 `img/tiny-glade-ref-corner-arch-passage.png`：
		// **两道拱共用一根角柱**，角柱不是障碍，它就是中墩。
		const FCSHouseFootprint Footprint = GetFootprint();
		const int32 NumEdges = Footprint.NumEdges();
		TArray<FCSHouseEdgeFrame, TInlineAllocator<8>> Frames;
		TArray<float, TInlineAllocator<8>> EdgeStart;
		Frames.SetNum(NumEdges);
		EdgeStart.SetNumZeroed(NumEdges);
		float Perimeter = 0.0f;
		for (int32 Edge = 0; Edge < NumEdges; ++Edge)
		{
			Frames[Edge] = CSHouse_GetEdge(Edge, Footprint, WallThickness);
			EdgeStart[Edge] = Perimeter;
			Perimeter += Frames[Edge].Len;
		}

		if (Perimeter > DoorMinWidth)
		{
			// 闭环采样：覆盖 [0, Perimeter)，**末点不重复首点**（求解器的闭环口径）。
			const int32 Steps = FMath::Max(8, FMath::CeilToInt(Perimeter / FMath::Max(DoorSampleStep, 1.0f)));
			const float Step = Perimeter / Steps;

			// 环参数 → 哪条边 + 边内弧长。转角恰好落在边界上时归**后**一条边（半开区间）。
			auto RingToEdge = [&](float Ring, int32& OutEdge, float& OutLocal)
			{
				float Wrapped = FMath::Fmod(Ring, Perimeter);
				if (Wrapped < 0.0f) Wrapped += Perimeter;
				OutEdge = NumEdges - 1;
				for (int32 Edge = 0; Edge < NumEdges; ++Edge)
				{
					if (Wrapped < EdgeStart[Edge] + Frames[Edge].Len) { OutEdge = Edge; break; }
				}
				OutLocal = Wrapped - EdgeStart[OutEdge];
			};

			TArray<float> Weights;
			TArray<float> Gaps;
			Weights.Reserve(Steps);
			Gaps.Reserve(Steps);
			for (int32 K = 0; K < Steps; ++K)
			{
				int32 Edge = 0;
				float Local = 0.0f;
				RingToEdge(Step * K, Edge, Local);
				const FCSHouseEdgeFrame& F = Frames[Edge];
				const FVector2D OutWorld = AxX * (-F.In.X) + AxY * (-F.In.Y);   // 这一段的世界系外法线
				const FVector2D WP = ToWorld2D(F.Start + F.U * Local);
				// 内外两条探测线取较大者：路铺到墙根就算经过，不要求压过墙心。
				Weights.Add(FMath::Max(
					Ground->SampleRoadWeight(WP + OutWorld * DoorProbeOffset),
					Ground->SampleRoadWeight(WP - OutWorld * DoorProbeOffset)));
				Gaps.Add(float(Loc.Z - Ground->SampleHeight(WP + OutWorld * DoorProbeOffset)));
			}

			// 上一帧的区间（滞回靠交叠继承，不靠编号）。记忆现在存的是**环参数**，
			// 所以 `EdgeIndex` 恒 -1；旧的逐边记忆读进来会全部失配 ⇒ 换版本那一帧滞回失效一次，
			// 与"拉尺寸跨 round 边界"那条老坑同型但只发生一次，可接受。
			TArray<FCSDoorRun> Prev;
			for (const FCSDoorRunMemory& M : DoorRunMemory) Prev.Add(FCSDoorRun{ M.S0, M.S1 });

			FCSDoorRunParams P;
			P.OnWeight = DoorOnWeight;
			P.MinWidth = DoorMinWidth;
			P.KeepWidth = DoorMinWidth * DoorKeepWidthRatio;
			P.MaxWidth = 0.0f;      // 过宽的切分**放到按边切完之后**做，免得一个拱骑在转角上
			P.PierWidth = PierWidth;
			// 碎环段并掉。放在环上做才对：转角两边各一条窄路，在环上是"中间夹一小段墙"，
			// 并掉之后成为一条跨角段 ⇒ 按边切回去就是共用角柱的两道拱。
			P.MinWallSegment = DoorMinWallSegment;
			P.Hi = Perimeter;
			P.bClosed = true;

			TArray<FCSDoorRun> RingRuns;
			CSHouse_SolveRoadRuns(Weights, 0.0f, Step, P, Prev, RingRuns);

			// 环上的段 → 逐边的洞。跨角的段在这里被拆成两片，两片都打上转角标记。
			struct FEdgePiece { int32 Edge; FCSDoorRun Run; };
			TArray<FEdgePiece> Pieces;
			for (const FCSDoorRun& Ring : RingRuns)
			{
				NewMemory.Add([&] { FCSDoorRunMemory M; M.EdgeIndex = -1; M.S0 = Ring.S0; M.S1 = Ring.S1; return M; }());

				// 段可能跨越 0 点，所以按"环上 [S0, S1] 与每条边的 [start, start+len] 求交"来切，
				// 边的区间同时试 −Perimeter / 0 / +Perimeter 三个副本，覆盖绕回的情况。
				for (int32 Edge = 0; Edge < NumEdges; ++Edge)
				{
					for (int32 Rep = -1; Rep <= 1; ++Rep)
					{
						const float A = EdgeStart[Edge] + Rep * Perimeter;
						const float B = A + Frames[Edge].Len;
						const float C0 = FMath::Max(Ring.S0, A);
						const float C1 = FMath::Min(Ring.S1, B);
						if (C1 - C0 < 1.0f) continue;
						FEdgePiece Piece;
						Piece.Edge = Edge;
						Piece.Run = FCSDoorRun{ C0 - A, C1 - A };   // 转回边内弧长
						// 顶到边界的那一片不再单独打标记（2026-09-06 删掉了转角位）：它靠"顶着墙端"
						// 这个几何事实在 `ResolvePierSpans` 里与对面那片配成墩，与拱廊的墩同一套口径。
						Pieces.Add(Piece);
					}
				}
			}

			for (int32 PieceIndex = 0; PieceIndex < Pieces.Num(); ++PieceIndex)
			{
				const int32 Edge = Pieces[PieceIndex].Edge;
				const FCSHouseEdgeFrame& F = Frames[Edge];

				// 过宽的切分放在这里：切出来的一排拱因此**不会骑在转角上**。
				TArray<FCSDoorRun> Runs;
				CSHouse_SplitRun(Pieces[PieceIndex].Run, DoorMaxWidth, PierWidth,
					DoorMinWidth * DoorKeepWidthRatio, Runs);

			for (int32 RunIndex = 0; RunIndex < Runs.Num(); ++RunIndex)
			{
				const FCSDoorRun& Run = Runs[RunIndex];
				float Width = Run.Width();

				// 离地收窄降级成一个乘数（旧口径里它是唯一的宽度源）。平地上恒 1。
				if (bDoorGroundNarrowing)
				{
					// 段内最大落差：整条门取最坏的一处，免得门一半悬空一半贴地。
					float GapMax = 0.0f;
					// 环参数下标：段是边内弧长，先加回这条边的环起点，再按环采样步长取样本区间。
					const float RingA = EdgeStart[Edge] + Run.S0;
					const float RingB = EdgeStart[Edge] + Run.S1;
					const int32 First = FMath::FloorToInt(RingA / Step);
					const int32 Last = FMath::CeilToInt(RingB / Step);
					for (int32 K = First; K <= Last; ++K)
					{
						GapMax = FMath::Max(GapMax, Gaps[((K % Steps) + Steps) % Steps]);
					}
					Width *= ComputeDoorWidthScale(GapMax, DoorGapFull, DoorGapZero);
				}

				const float Height = MaxDoorHeight;

				// 量化后再进哈希：路是连续场，端点每帧亚厘米地抖，不量化则哈希永不相等 ⇒ 每帧全量重建。
				Width = FMath::RoundToFloat(Width / DoorWidthQuantum) * DoorWidthQuantum;
				float CenterS = FMath::RoundToFloat(Run.Center() / DoorCenterQuantum) * DoorCenterQuantum;

				// ⚠️ **量化之后必须夹回边内**（2026-09-05，实测缺陷）。洞只存中心与宽度，两者是
				// **各自**四舍五入的，端点由 `CenterS ∓ Width/2` 反算 ⇒ 端点可以被推出去
				// (中心步长 + 宽度步长)/2（默认 2 cm）。中间的门无所谓，但**转角片有一端本来就是
				// 被墙端切出来的**：推出去那一截落在墙外，任何面板都盖不到，而 `CSHouse_BuildBodySoup`
				// 的"装不下"判据拿的是名义 `Width` ⇒ 差一点点就 `continue`，症状是**砖拱砌得好好的、
				// 墙却没挖洞**，而且一声不吭（L_HouseGroundDemo 的转角上实测差 0.10 cm）。
				{
					const float ClampedS0 = FMath::Max(CenterS - Width * 0.5f, 0.0f);
					const float ClampedS1 = FMath::Min(CenterS + Width * 0.5f, F.Len);
					Width = ClampedS1 - ClampedS0;
					CenterS = (ClampedS0 + ClampedS1) * 0.5f;
				}
				if (Width < DoorMinWidth) continue;

				// 拱高（2026-09-04 起与洞宽解耦）：`DoorMaxArchRise` 为 0 时退回半宽 = 正半圆。
				// 起拱段至少留 `CSHouse_MinSpring`，所以拱高不能吃掉全部门高。
				// **用夹过之后的宽度算**：夹之前算，转角片的拱会比它自己的洞还宽一点。
				const float FullRise = FMath::Max(Height - CSHouse_MinSpring, 1.0f);
				float Rise = (DoorMaxArchRise > UE_KINDA_SMALL_NUMBER)
					? FMath::Min(DoorMaxArchRise, Width * 0.5f)   // 比半圆还高没有意义，拱只会更扁不会更尖
					: Width * 0.5f;
				Rise = FMath::Clamp(Rise, 1.0f, FullRise);
				// ⚠️ 拱高解耦之后，**洞宽不再被门高限制**（半圆时"半宽 ≤ 门高 − 起拱段"那条约束
				// 是半径与半宽相等带来的，现在半径分成了两根半轴）。宽度只受路与 DoorMaxWidth 管。

				FCSWallOpening Door;
				Door.Type = ECSOpeningType::Door;
				Door.Shape = ECSOpeningShape::Arch;
				Door.EdgeIndex = Edge;
				Door.CenterS = CenterS;
				Door.Width = Width;
				Door.Z0 = 0.0f;              // 门恒贴地；窗台高走 Z0 > 0（D8）
				Door.Z1 = Height;
				Door.ArchRise = Rise;
				Door.Tag = uint8(RunIndex & 0xFF);
				// 转角片不打任何位：它的墩位由 `ResolvePierSpans` 跨角配对时打上，不装门扇也从墩位读。
				// 拱廊：一条路被切成多拱时，每个子拱都是敞开的（用户实测："两道门并排时只有拱没有木门"）。
				if (Runs.Num() > 1) Door.StyleFlags |= CSHouse_StyleArcade;
				CurrentOpenings.Add(Door);
			}
			}
		}
	}
	DoorRunMemory = MoveTemp(NewMemory);

	// **让位规则**：门拱优先于特征标记（D6）—— 子段被点亮后，与之相交的窗判为不可行，
	// 避免拱窗互切。所以窗要在门全部落位之后再逐条过谓词。
	TArray<FCSWallOpening> WindowCandidates;
	BuildWindowOpenings(WindowCandidates);
	Kept.Append(WindowCandidates);   // 注入洞（如果有）排在前面，窗跟在后面
	CurrentWindowCount = 0;
	CurrentWindowRejectCount = 0;
	CurrentFeatureVerdicts.Reset();
	for (const FCSWallOpening& Feature : Kept)
	{
		// 裁决**就在这里**记下来，别事后再问一遍谓词：这一刻的 `CurrentOpenings` 才是它当时
		// 面对的那一份，事后问会拿"已经把自己放进去了"的表去判自己。
		const ECSFeatureReject Reason = QueryFeatureReject(Feature);
		if (Feature.SourceId.IsValid()) CurrentFeatureVerdicts.Add(Feature.SourceId, Reason);
		if (Reason != ECSFeatureReject::None)
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
	CSHouse_AppendFootprintHash(H, GetFootprint());
	H.Append({ CSHouse_Q(WallHeight, 1), CSHouse_Q(WallThickness, 0.5),
		CSHouse_Q(RoofPitch, 0.1), CSHouse_Q(RoofOverhang, 1) });
	for (const FCSWallOpening& O : CurrentOpenings)
	{
		H.Append({ O.EdgeIndex, int32(O.Shape), CSHouse_Q(O.CenterS, 1), CSHouse_Q(O.Width, DoorWidthQuantum),
			CSHouse_Q(O.Z0, 1), CSHouse_Q(O.Z1, 1), CSHouse_Q(O.Skew, 0.01),
			// 拱高进哈希：漏掉它 = 改了 `DoorMaxArchRise` 洞形变了、房体却不重建（静默失效）。
			CSHouse_Q(O.ArchRise, 0.5), CSHouse_Q(O.Rise(), 0.5),
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
	// 整表重算（同 DoorRunMemory）：判据是当前洞集合的纯函数，留着旧键只会让"这条边多开一个拱"
	// 之后的编号错位继承到别的跨度上去。
	TMap<uint32, bool> NewState;
	CurrentPierSpanCount = 0;
	// ⚠️ **只清墩那两位，别整份清零**：`CSHouse_StyleArcade` 是 `ComputeDoors` 在更早一步
	// 定的，而本函数在它之后跑 —— 整份清零会把它擦掉，症状是拱廊照样装门扇而且**没有任何报错**
	// （2026-09-04 转角位被这么擦掉过一次，判据 ⑥ 抓到的）。
	constexpr uint8 PierBits = CSHouse_StylePierBefore | CSHouse_StylePierAfter;
	for (FCSWallOpening& Opening : CurrentOpenings) Opening.StyleFlags &= ~PierBits;
	// 转角数 = 边数：闭合折线上每个顶点恰好是一个角。
	const FCSHouseFootprint Footprint = GetFootprint();
	const int32 NumCorners = Footprint.NumEdges();
	CornerPierTopZ.Init(0.0f, NumCorners);

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

		// 这条边的洞数进 key：多开/少开一个拱会把整条边的
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

	// 跨角配对（2026-09-06 用户裁决：**转角就是一个墩**，两道拱只是不在同一条边上）。
	// k 号边最后一洞顶着 `Len`、k+1 号边第一洞顶着 0 ⇒ 它们之间只隔那块转角方块，跨度 = 墙厚，
	// 走同一套双阈迟回。配上就打墩位，下游一个字都不用为转角单写：门樘在转角侧自动关掉
	// （`BuildEdgeElements`）、门扇不装（`CSHouse_StyleNoLeafMask`）、墙板的格咬到洞缘
	// （`CSHouse_BuildBodySoup`）；只有"中间那根柱子"是转角特有的 —— 同边的墩由
	// `BuildEdgeElements` 沿边出，转角的墩立在角点上，由 `BuildCornerPierBricks` 读
	// `CornerPierTopZ` 出，角石（`BuildQuoinBricks`）读同一份数值在起拱线以下让路。
	for (int32 Corner = 0; Corner < NumCorners; ++Corner)
	{
		// 角 k 夹在「k 号边的远端」与「k+1 号边的近端」之间，与 `CSHouse_GetCorner` 同号。
		const int32 FarEdge = Corner;
		const int32 NearEdge = (Corner + 1) % NumCorners;
		const float FarLen = CSHouse_GetEdge(FarEdge, Footprint, WallThickness).Len;
		FCSWallOpening* Far = nullptr;
		FCSWallOpening* Near = nullptr;
		for (FCSWallOpening& O : CurrentOpenings)
		{
			// 与 `CSHouse_PierSpanBetween` 同一条前置："两侧都是落地的拱"才谈得上墩。
			if (!O.IsValid() || O.Shape != ECSOpeningShape::Arch || O.Z0 > UE_KINDA_SMALL_NUMBER) continue;
			// 2 cm 容差：`ComputeDoors` 已把转角片夹回墙端，这里只是防浮点。
			if (O.EdgeIndex == FarEdge && FMath::Abs(O.S1() - FarLen) <= 2.0f) Far = &O;
			if (O.EdgeIndex == NearEdge && FMath::Abs(O.S0()) <= 2.0f) Near = &O;
		}
		if (!Far || !Near) continue;

		// key 的顶字节 0xC0 与边号（< 0xC0）不共域：跨角跨度与同边跨度永远不互相继承样式。
		const uint32 Key = (0xC0u << 24) | uint32(Corner);
		const bool bWasPier = PierSpanIsPier.FindRef(Key);
		const bool bIsPier = CSHouse_SpanIsPier(Style, WallThickness, bWasPier);
		NewState.Add(Key, bIsPier);
		if (!bIsPier) continue;

		Far->StyleFlags |= CSHouse_StylePierAfter;
		Near->StyleFlags |= CSHouse_StylePierBefore;
		// 墩顶 = 两条起拱线的较低者（与 `CSHouse_PierSpanBetween` 同一个取法）。起拱线用
		// `Z1 − Rise()`：2026-09-04 拱高解耦之后它才是墙上裁剪场真正的那条线，半宽只在正半圆时相等。
		CornerPierTopZ[Corner] = FMath::Max(FMath::Min(Far->Z1 - Far->Rise(), Near->Z1 - Near->Rise()), 0.0f);
		++CurrentPierSpanCount;
	}

	PierSpanIsPier = MoveTemp(NewState);
}

int32 ACSHouseActor::GetOpenDoorCount() const
{
	FlushPendingReevaluate();
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

float ACSHouseActor::ComputeDoorWidthScale(float GapMax, float GapFull, float GapZero)
{
	const float Span = FMath::Max(GapZero - GapFull, 1.0f);   // 参数被填反/相等时退化成硬阈，不除零
	return 1.0f - FMath::Clamp((GapMax - GapFull) / Span, 0.0f, 1.0f);
}

FCSRoofDesc ACSHouseActor::GetRoofDesc() const
{
	FCSRoofDesc Desc;
	Desc.Footprint = GetFootprint();
	Desc.EaveZ = WallHeight + RoofHeightOffset;
	Desc.Pitch = RoofPitch;
	Desc.Overhang = RoofOverhang;
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
		CSHouse_Q(WallHeight, 1) };
	CSHouse_AppendFootprintHash(H, GetFootprint());

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
			CSHouse_Q(N.Yaw, 0.1), CSHouse_Q(N.WallHeight, 1) });
		CSHouse_AppendFootprintHash(H, N.Footprint);
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

	// 支撑点：墙厚中线那一圈（柱子落在墙体正下方），每个角 + 每边按间距等分。
	//
	// 中线的角点就是深度 T/2 处的斜接点：外角点沿本边 U 让出 `InsetStart / 2`、再沿 In 进 T/2。
	// 矩形上它逐位等于原来的「四角各内缩半个墙厚」—— 直角让出量恰好是 T。
	const FCSHouseFootprint Footprint = GetFootprint();
	const int32 NumEdges = Footprint.NumEdges();
	TArray<FVector2D, TInlineAllocator<8>> Corners;
	for (int32 E = 0; E < NumEdges; ++E)
	{
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(E, Footprint, WallThickness);
		Corners.Add(F.Start + F.U * (F.InsetStart * 0.5) + F.In * (WallThickness * 0.5));
	}
	TArray<FVector2D> Points;
	for (int32 E = 0; E < NumEdges; ++E)
	{
		const FVector2D A = Corners[E], B = Corners[(E + 1) % NumEdges];
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

void ACSHouseActor::RequestReevaluate()
{
	// 只标脏。一帧里来多少次都只是同一个布尔，兑现只在 Tick 里发生一次（N vs N² 的账见头文件）。
	bReevaluatePending = true;
	if (!IsTemplate()) SetActorTickEnabled(true);
}

void ACSHouseActor::FlushPendingReevaluate() const
{
	if (!bReevaluatePending) return;
	// `const_cast`：本函数做的是"把已经欠下的那一次重求值当场补上"，补完之后对外可见的状态
	// 与"合批没生效、当场就跑了"逐位相同 —— 那正是 const 想保证的东西。而 getter 必须是
	// const（`BlueprintPure` 要求），补票只能在这里做。
	const_cast<ACSHouseActor*>(this)->ReevaluateSite();
}

void ACSHouseActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	// **先关再跑**：重建途中若又来了通知，`RequestReevaluate` 会把 tick 重新打开、留给下一帧。
	// 顺序反过来，那次打开就被这一行关掉，新诉求要等到下一次别的唤醒才兑现。
	SetActorTickEnabled(false);
	FlushPendingReevaluate();
}

void ACSHouseActor::ReevaluateSite()
{
	if (bInReevaluate || IsTemplate() || !GetWorld()) return;
	TGuardValue<bool> Guard(bInReevaluate, true);
	// 欠账**在开头**清：重建途中新到的通知会重新置位，由下一次 Tick 兑现（见 bReevaluatePending）。
	bReevaluatePending = false;

	ResolveGroundAndSubscribe();

	// 形状不是严格凸时按凸包使用（`GetFootprint`），这里只负责"出声一次"。
	WarnIfFootprintShapeFixed();

	// ① 落座：绝对式，升降对称（计划 D4）。
	const double SeatZ = ComputeSeatZ();
	FVector Loc = GetActorLocation();
	if (FMath::Abs(Loc.Z - SeatZ) > 0.5)
	{
		Loc.Z = SeatZ;
		SetActorLocation(Loc);
	}

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
	// ⚠️ **必须排在 `ComputeDoors()` 之前**：洞的弧长是从锚点现解的（见 `FCSMarkerWindow::Anchor`），
	// 排在后面的话洞会用上一轮的锚点算，而框已经按新锚点摆好了 —— 又是一次框洞分家。
	ReanchorMarkersToPreserveWorld();

	const uint32 SeamCutHash = ComputeSeamCuts();
	const uint32 BodyHash = CSHouse_Hash({ int32(ComputeDoors()), int32(SeamCutHash) });
	// "网格在不在"不够：删除后撤销，复活的网格对象还在、显存却已被渲染组件还掉了（IsSlotMeshLive）。
	ReconcileMeshSlot(BodySlot, BodyHash, PlacementHash, bForceFullRebuild || !IsSlotMeshLive(TinyGladeMesh),
		[this]() { RebuildBodyMesh(); }, [this]() { return ApplyBodyPlacement(); });

	// ③ 柱（独立组件——纯地形变化只走到这，不碰房体）。
	TArray<FVector> Centers;
	TArray<float> Lengths;
	const uint32 PillarHash = ComputePillars(Centers, Lengths);
	ReconcileMeshSlot(PillarSlot, PillarHash, PlacementHash, bForceFullRebuild || (Centers.Num() > 0 && !IsSlotMeshLive(PillarMesh)),
		[&]() { RebuildPillarMesh(Centers, Lengths); }, [this]() { return ApplyPillarPlacement(); });
	CurrentPillarCount = Centers.Num();

	// ④ 门框砖：clip 的配套件，洞集合一变就要重排（哈希短路吸收无效唤醒）。
	if (bForceFullRebuild) FrameDescHash = 0;
	RebuildFrame();

	// ⑤ 藤蔓：与门框砖同一档的“墙的配套件”，同样靠哈希短路吸收无效唤醒。
	//    排在门框之后是因为它读 `CurrentOpenings`（避让墙洞），那份表由 ComputeDoors 定。
	if (bForceFullRebuild) VineDescHash = 0;
	RebuildVine();

	// ⑥ 屋面瓦：四坡的屋面**全部**由它铺成，房体三角汤里一片屋面都没有。
	//    只吃屋面 desc + 摆位，不读门、不读地面 —— 所以排在哪儿都对，放这里只是顺着"墙 → 屋面"读。
	if (bForceFullRebuild) RoofTileDescHash = 0;
	RebuildRoofTiles();

	//    尖顶跟在瓦后面：它要盖住的正是瓦在脊端点上的破口，读的也是同一份屋面 desc。
	if (bForceFullRebuild) RoofFinialDescHash = 0;
	RebuildRoofFinials();

	//    门扇：读的是 ③ 定下来的 `CurrentOpenings`，所以必须排在 ComputeDoors 之后。
	//    与门框砖并列（都是"洞的配套件"），但走普通静态网格组件而不是实例化路径。
	if (bForceFullRebuild) DoorLeafDescHash = 0;
	RebuildDoorLeaves();

	// ⑦ 装饰摆件（D12 的**锚点那一半**）：排在最后，因为它锚在前面每一样东西上 ——
	//    门（③ 的 `CurrentOpenings`）、墙脚（②）、檐口/屋脊（屋面 desc），还要读地面镜像
	//    落高与排除道路。TG 的 `populate_autoclutter_regions` 同样排在建筑系统之后。
	if (bForceFullRebuild) DecorDescHash = 0;
	RebuildDecor();

	// ⑧ 附属物（D8）：回推裁决 + 按锚点吸附。**必须排在最后** —— 它要读 ⑦ 之前定下来的
	//    `CurrentFeatureVerdicts`，而那份表由 ② 的落位循环产出。
	NotifyMarkersRebuilt();

	// ⑨ 拉尺寸模式里边数变了（改 `FootprintShape`、撤销……）：锥子按新边数对齐。不在模式里 / 已经对上时零成本。
	//    放在最后是因为它只读 footprint、会摆放抓手，不影响上面任何一样派生物。
	SyncEdgeHandlesToFootprint();

	bForceFullRebuild = false;
	++ReevaluateCount;   // 合批的观测量（`GetReevaluateCount`），只数真正跑完的
}

void ACSHouseActor::WarnIfFootprintShapeFixed()
{
	if (FootprintShape.Num() < 3)
	{
		FootprintShapeWarnedHash = 0;
		return;
	}

	ECSFootprintShapeFix Fix = ECSFootprintShapeFix::Kept;
	const FCSHouseFootprint Used = FCSHouseFootprint::FromShape(FootprintShape, FootprintSize, &Fix);
	if (Fix == ECSFootprintShapeFix::Kept || Fix == ECSFootprintShapeFix::Rect)
	{
		FootprintShapeWarnedHash = 0;
		return;
	}

	// 只哈希**形状本身**（不含尺寸）：正缩放保凸、保共线，拖 FootprintSize 不改变"要不要取凸包"，
	// 所以拖尺寸的每一帧都不该重复告警。处置方式也拼进去：同一个形状从"凸包"变成"退化"要再说一次。
	uint32 Hash = FCrc::MemCrc32(FootprintShape.GetData(), FootprintShape.Num() * sizeof(FVector2D));
	Hash = HashCombine(Hash, uint32(Fix) + 1u);
	if (Hash == FootprintShapeWarnedHash) return;
	FootprintShapeWarnedHash = Hash;

	if (Fix == ECSFootprintShapeFix::Hulled)
	{
		UE_LOG(LogTinyGladeHouse, Warning,
			TEXT("[TinyGladeHouse] %s 的 FootprintShape 不是严格凸的（凹角 / 共线点 / 重复点 / 乱序）：按凸包使用，顶点数 %d → %d。")
			TEXT("属性本身不改写；凹 footprint 不支持（2026-09-14 裁决：取凸包）。"),
			*GetName(), FootprintShape.Num(), Used.NumEdges());
	}
	else
	{
		UE_LOG(LogTinyGladeHouse, Warning,
			TEXT("[TinyGladeHouse] %s 的 FootprintShape 退化（%d 个点的凸包不足 3 个顶点或面积为零）：按矩形使用。"),
			*GetName(), FootprintShape.Num());
	}
}

FCSHouseWindowBrushRequest ACSHouseActor::OnWindowBrushRequest;

void ACSHouseActor::StartWindowBrush()
{
	OnWindowBrushRequest.Broadcast(this);
}

void ACSHouseActor::NotifyMarkersRebuilt()
{
	// 反引失效的顺手清掉：标记被删时会自己 `UnregisterFeatureMarker`，但"actor 没了却没走
	// 注销"这条路是存在的（关卡卸载的销毁次序不保证），留着会让 `GetFeatureMarkerCount`
	// 报出一个根本不存在的窗。
	MarkerWindows.RemoveAll([](const FCSMarkerWindow& Entry) {
		return Entry.Marker.IsStale();
	});

	for (const FCSMarkerWindow& Entry : MarkerWindows)
	{
		ACSHouseFeatureMarker* Marker = Entry.Marker.Get();
		if (!Marker) continue;

		// ① 裁决回执。没有它，拉尺寸/改墙高把窗挤掉之后标记还在说"我切出洞了" ——
		//    这两条路都不经过 `OnHandleDrag`，而回执原本只在那里写。
		const ECSFeatureReject* Reason = CurrentFeatureVerdicts.Find(Marker->GetMarkerId());
		Marker->ApplyHostVerdict(Reason ? *Reason : ECSFeatureReject::None);

		// ② 按锚点吸附（TG `move_decorators_following_anchors`）。
		//    ⚠️ 正在被 gizmo 拖的那一个**不写** —— 标记不 attach 在房子下，写它纯粹是和 gizmo
		//    抢方向盘。（拉尺寸抓手 attach 着，2026-09-06 改成每次都重摆，成因见 SnapResizeHandles。）
		if (!Marker->IsBeingDragged()) Marker->SnapToAnchor();
	}
}

void ACSHouseActor::RebuildHouse()
{
	bForceFullRebuild = true;
	// 两张迟回表一起清：留着其中一张就成了"门从头判、墩却接着上一代的记忆"，
	// 同一个世界状态会因为上一次的历史给出两种房子。
	DoorRunMemory.Empty();
	PierSpanIsPier.Empty();
	ReevaluateSite();
}

// -----------------------------------------------------------------------------
// 拉尺寸（D5）：单边推拉的机制入口
// -----------------------------------------------------------------------------

float ACSHouseActor::PushEdge(int32 EdgeIndex, float Offset, bool bFinished)
{
	FVector2D NewSize = FootprintSize;
	FVector NewCentre = GetActorLocation();
	TArray<FVector2D> NewShape;
	float Applied = 0.0f;

	// 矩形（形状为空或退回矩形）走矩形版：那条的算术被 `House.EdgePush` 按位钉着，旧存档一位都不许变。
	const FCSHouseFootprint Current = GetFootprint();
	const bool bShaped = FootprintShape.Num() >= 3
		&& !Current.EqualsApprox(FCSHouseFootprint::MakeRect(FootprintSize), 0.0);
	if (!bShaped)
	{
		Applied = CSHouse_ApplyEdgePush(NewSize, NewCentre, EdgeIndex,
			float(GetActorRotation().Yaw), Offset, MinFootprint);
	}
	else
	{
		// 异形：推完的折线已经按包围盒居中 ⇒ 它的包围盒就是新尺寸、它本身就是新形状
		// （`FromShape` 会再拉伸一次，比例恰好是 1）。
		// ⚠️ `Current` 是**凸包之后**的折线：输入里被凸包丢掉的点（凹角处 / 共线 / 重复）在推边之后不再回来 ——
		// 推边是显式的几何编辑，写回的就是被推的那个凸包。与"凸包只在使用时取、不回写"不矛盾：
		// 那条挡的是细节面板逐个加顶点的中间态，拖锥子不经过那条路。
		FCSHouseFootprint Local = Current;
		Applied = CSHouse_ApplyEdgePushPolyline(Local, NewCentre, EdgeIndex,
			float(GetActorRotation().Yaw), Offset, MinFootprint);
		NewSize = Local.GetBounds().GetSize();
		NewShape = MoveTemp(Local.Verts);
	}

	// 尺寸没动就一步都不走：推到 MinFootprint 下限之后每帧都会走到这里，照常重求值的话
	// 那一整段"墙拖不动"的时间里房子仍在无谓地重算门、砖、藤、摆件。
	if (Applied == 0.0f && !bFinished) return 0.0f;

	if (Applied != 0.0f)
	{
		FootprintSize = NewSize;
		if (bShaped) FootprintShape = MoveTemp(NewShape);
		// 中心与尺寸必须**同一帧**落地：只改其中一个，画面上就是"对侧墙也跟着走"，
		// 与计划 D5 那个"拖 1 m 走 2 m"的父子回路缺陷逐像素相同，极易误诊到别处。
		SetActorLocation(NewCentre);
	}

	// 拖动期直接标脏，别等 subsystem 那 0.25 s 的兜底快扫（`MarkHouseDirty` 的第一个客户）。
	if (UWorld* World = GetWorld())
	{
		if (UCSHouseSubsystem* Subsystem = World->GetSubsystem<UCSHouseSubsystem>()) Subsystem->MarkHouseDirty(this);
	}

	// 松手 = gizmo 的 PostEditMove(bFinished=true)：把拖动期为了零阻塞留下的容量 / 包围盒余量重新收紧。
	if (bFinished) bForceFullRebuild = true;
	ReevaluateSite();
	return Applied;
}

float ACSHouseActor::PushHeight(float Offset, bool bFinished)
{
	const float Desired = FMath::Max(WallHeight + Offset, FMath::Max(MinWallHeight, 1.0f));
	const float Applied = Desired - WallHeight;

	// 高度没动就一步都不走：压到下限之后每帧都会走到这里，照常重求值的话那一整段
	// "墙压不下去"的时间里房子仍在无谓地重算门、砖、瓦、藤、摆件。
	if (Applied == 0.0f && !bFinished) return 0.0f;

	WallHeight = Desired;

	// 拖动期直接标脏，别等 subsystem 那 0.25 s 的兜底快扫（与 PushEdge 同一条路）。
	if (UWorld* World = GetWorld())
	{
		if (UCSHouseSubsystem* Subsystem = World->GetSubsystem<UCSHouseSubsystem>()) Subsystem->MarkHouseDirty(this);
	}

	// 墙高波及檐口、屋面、门的离地收窄、窗的 AboveEave 谓词、藤与摆件的锚点 —— 只重建墙板
	// 是不够的，必须走完整的重求值。
	if (bFinished) bForceFullRebuild = true;
	ReevaluateSite();
	return Applied;
}

float ACSHouseActor::PushBase(float Offset, bool bFinished)
{
	// 底动顶不动（用户裁决 2026-09-14）：房底抬 Δ ⇔ `HeightOffset += Δ`、`WallHeight -= Δ`。
	// 两条下限同时夹，生效量取两者都允许的那一段：
	//  · 往上受墙高下限管：墙最多矮到 `MinWallHeight`；已经矮于下限的旧存档往上一步都不给（不许再矮），往下照常。
	//  · 往下受贴地管：`HeightOffset` 最低到 `min(0, 当前值)` —— 旧存档里已经是负数的以当前值为界，
	//    **不许一抓就跳到 0**（那一跳在画面上是房子自己蹦起来）。
	const float WallFloor = FMath::Max(MinWallHeight, 1.0f);
	const float MaxRaise = FMath::Max(WallHeight - WallFloor, 0.0f);
	const float OffsetFloor = FMath::Min(HeightOffset, 0.0f);
	const float MaxLower = FMath::Max(HeightOffset - OffsetFloor, 0.0f);
	const float Applied = FMath::Clamp(Offset, -MaxLower, MaxRaise);

	// 顶在下限上每帧都会走到这里：没动就一步都不走（同 PushHeight）。
	if (Applied == 0.0f && !bFinished) return 0.0f;

	// **同一次调用里两个量一起改完**再重求值：分两次改的话中间那一刻檐口会先掉 Δ、再回来，
	// 重求值若恰好插在中间（合批 tick、快扫），屋顶会闪一帧。
	HeightOffset += Applied;
	WallHeight -= Applied;

	// 波及面是 PushHeight 的超集（墙高 + 落座 Z ⇒ 柱、门的离地收窄、藤的悬空判据、摆件落高），走完整重求值。
	if (bFinished) bForceFullRebuild = true;
	ReevaluateSite();
	return Applied;
}

// -----------------------------------------------------------------------------
// 拉尺寸模式（D5 交互层）：抓手 actor 的生成 / 回位 / 销毁
// -----------------------------------------------------------------------------

FCSHouseResizeModeChanged ACSHouseActor::OnResizeModeChanged;

namespace
{
/**
 * 抓手的生成参数（锥子与高度框共用一份）。
 *
 * `RF_Transient` 不存盘（计划 D5）。**顺带丢掉 `RF_Transactional`**：抓手的位移本身没有
 * 撤销语义——真正该被撤销的是 FootprintSize / WallHeight / HeightOffset，而它们由 Push* 直接写。
 * 两者各记一半的话 Ctrl+Z 会撤回抓手却留下尺寸，下一次拖动从一个自相矛盾的状态起步。
 */
FActorSpawnParameters CSHouse_HandleSpawnParams(AActor* Owner)
{
	FActorSpawnParameters SpawnParams;
	SpawnParams.ObjectFlags = RF_Transient;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.Owner = Owner;
	return SpawnParams;
}
}

ACSHouseResizeHandleActor* ACSHouseActor::SpawnEdgeHandle(int32 EdgeIndex)
{
	UWorld* World = GetWorld();
	if (!World) return nullptr;

	ACSHouseResizeHandleActor* Handle = World->SpawnActor<ACSHouseResizeHandleActor>(
		GetActorLocation(), GetActorRotation(), CSHouse_HandleSpawnParams(this));
	if (!Handle) return nullptr;

	// ⚠️ 顺序在两种抓手里都一样：**先 attach 再 Initialize**。`InitializeHandle` 末尾的
	// `SnapToCanonical` 写的是世界位置，attach 会把它换算成相对量；反过来的话抓手的相对位置
	// 会被算成"世界原点到规范位置"，房子一移动抓手就飞了。
	Handle->AttachToActor(this, FAttachmentTransformRules::KeepWorldTransform);
	Handle->InitializeHandle(this, EdgeIndex);
#if WITH_EDITOR
	Handle->SetActorLabel(FString::Printf(TEXT("%s_ResizeHandle_%d"), *GetActorLabel(), EdgeIndex));
#endif
	ResizeHandles.Add(Handle);
	return Handle;
}

ACSHouseHeightHandleActor* ACSHouseActor::SpawnHeightHandle(ECSHouseHeightHandleSide Side)
{
	UWorld* World = GetWorld();
	if (!World) return nullptr;

	ACSHouseHeightHandleActor* Handle = World->SpawnActor<ACSHouseHeightHandleActor>(
		GetActorLocation(), GetActorRotation(), CSHouse_HandleSpawnParams(this));
	if (!Handle) return nullptr;

	Handle->AttachToActor(this, FAttachmentTransformRules::KeepWorldTransform);
	Handle->InitializeHandle(this, Side);
#if WITH_EDITOR
	Handle->SetActorLabel(FString::Printf(TEXT("%s_%s"), *GetActorLabel(),
		Side == ECSHouseHeightHandleSide::Base ? TEXT("BaseHandle") : TEXT("HeightHandle")));
#endif
	ResizeHandles.Add(Handle);
	return Handle;
}

void ACSHouseActor::EnterResizeMode()
{
	if (!GetWorld()) return;

	// 幂等（计划 D5）：先把已经失效的格子摘掉，还剩抓手就只对齐边数、归位，不再生一组。
	// 用户在详情面板上连点两下这个按钮是常态，不挡的话场景里会攒出两组锥子。
	ResizeHandles.RemoveAll([](const TObjectPtr<ACSHouseHandleActor>& H) { return !IsValid(H); });
	if (ResizeHandles.Num() > 0)
	{
		SyncEdgeHandlesToFootprint();
		SnapResizeHandles();
		return;
	}

	// 边数 = **取完凸包之后**的 footprint 边数（用户裁决 2026-09-14："折线有多少折手柄就有多少个"）。
	const int32 NumEdges = GetFootprint().NumEdges();
	ResizeHandles.Reserve(NumEdges + 2);

	// ① 每面墙一个锥子：水平推拉，每条边一个独立自由度 ⇒ 每条边一个 actor（矩形四个）。
	for (int32 Edge = 0; Edge < NumEdges; ++Edge) SpawnEdgeHandle(Edge);

	// ② 两个套在房子外面的矩形框：檐口那个上下拖改墙高，房底那个上下拖改房底（底动顶不动）。
	//    **一个框只有一个自由度 ⇒ 一个框一个 actor**（拆成四根的话用户抓哪根都在改同一个量，
	//    四个 gizmo 互相打架）。将来再加竖直抓手也只是在这里多调一次 `SpawnHeightHandle`。
	SpawnHeightHandle(ECSHouseHeightHandleSide::Eave);
	SpawnHeightHandle(ECSHouseHeightHandleSide::Base);

	if (ResizeHandles.Num() > 0)
	{
		OnResizeModeChanged.Broadcast(this, true);
	}
}

void ACSHouseActor::SyncEdgeHandlesToFootprint()
{
	if (ResizeHandles.Num() == 0 || bSyncingResizeHandles) return;
	TGuardValue<bool> Guard(bSyncingResizeHandles, true);

	const FCSHouseFootprint Footprint = GetFootprint();
	const int32 NumEdges = Footprint.NumEdges();
	TArray<ACSHouseResizeHandleActor*> Cones;
	for (const TObjectPtr<ACSHouseHandleActor>& Handle : ResizeHandles)
	{
		ACSHouseResizeHandleActor* Cone = Cast<ACSHouseResizeHandleActor>(Handle);
		if (IsValid(Cone)) Cones.Add(Cone);
	}

	// 抓手摆位吃的几何：footprint 顶点 + 墙厚 + 墙高（锥子挂在墙外皮中点、`WallHeight × HandleHeightFraction`，
	// 框吃包围盒与檐口）。边数没变而它们变了（细节面板里挪一个顶点、改墙高、撤销……）只需重摆，不需要重建。
	TArray<int32> GeometryInput;
	Footprint.AppendQuantizedHash(GeometryInput);
	GeometryInput.Append({ int32(FMath::RoundToInt(WallThickness * 2.0f)), int32(FMath::RoundToInt(WallHeight * 2.0f)) });
	const uint32 GeometryHash = FCrc::MemCrc32(GeometryInput.GetData(), GeometryInput.Num() * sizeof(int32));

	// 稳态：锥子数 = 边数，且边号恰好铺满 0..N-1。每次重求值都会走到这里，稳态下不许有任何动作。
	if (Cones.Num() == NumEdges)
	{
		TBitArray<> Seen(false, NumEdges);
		bool bMatched = true;
		for (const ACSHouseResizeHandleActor* Cone : Cones)
		{
			const int32 Edge = Cone->GetEdgeIndex();
			if (Edge < 0 || Edge >= NumEdges || Seen[Edge]) { bMatched = false; break; }
			Seen[Edge] = true;
		}
		if (bMatched)
		{
			if (GeometryHash != ResizeHandleGeometryHash)
			{
				ResizeHandleGeometryHash = GeometryHash;
				SnapResizeHandles();
			}
			return;
		}
	}
	ResizeHandleGeometryHash = GeometryHash;

	// 按边号排好，**复用**前 min(旧, 新) 个（改认 0..k-1 号边），多的销毁、缺的补生。
	// 复用是为了让被选中的锥子留得住：销毁一个选中的 actor 会触发选中集变化，编辑器侧的失选监听
	// 在选中集里找不到归属者时就把整个模式退掉 —— 用户只是改了一下形状，锥子却全没了。
	// （`TArray<T*>::Sort` 会解引用，谓词拿到的是对象引用。）
	Cones.Sort([](const ACSHouseResizeHandleActor& A, const ACSHouseResizeHandleActor& B)
	{
		return A.GetEdgeIndex() < B.GetEdgeIndex();
	});
	const int32 Keep = FMath::Min(Cones.Num(), NumEdges);
	for (int32 Edge = 0; Edge < Keep; ++Edge)
	{
		Cones[Edge]->InitializeHandle(this, Edge);
#if WITH_EDITOR
		Cones[Edge]->SetActorLabel(FString::Printf(TEXT("%s_ResizeHandle_%d"), *GetActorLabel(), Edge));
#endif
	}
	for (int32 Index = Keep; Index < Cones.Num(); ++Index)
	{
		// **先摘表再销毁**：`Destroy` 会同步走到抓手的 `Destroyed` → `NotifyResizeHandleDestroyed`，
		// 表里还有它的话会被当成"模式里最后一个抓手没了"去判退出。
		ResizeHandles.Remove(Cones[Index]);
		Cones[Index]->Destroy();
	}
	for (int32 Edge = Keep; Edge < NumEdges; ++Edge) SpawnEdgeHandle(Edge);

	UE_LOG(LogTinyGladeHouse, Log, TEXT("[TinyGladeHouse] %s resize cones re-synced to %d edges (had %d)"),
		*GetName(), NumEdges, Cones.Num());

	// 边数变了，框的包围盒、其余锥子的规范位置多半也变了。
	SnapResizeHandles();
}

void ACSHouseActor::ExitResizeMode()
{
	if (ResizeHandles.Num() == 0) return;

	// 先把表清空再销毁：`Destroy()` 会同步走到抓手的 `Destroyed()`，那边回调
	// `NotifyResizeHandleDestroyed` —— 表还在的话会在循环中途被改，且退出事件要广播四次。
	TArray<TObjectPtr<ACSHouseHandleActor>> Doomed = MoveTemp(ResizeHandles);
	ResizeHandles.Reset();

	for (const TObjectPtr<ACSHouseHandleActor>& Handle : Doomed)
	{
		if (IsValid(Handle)) Handle->Destroy();
	}

	OnResizeModeChanged.Broadcast(this, false);
}

void ACSHouseActor::SnapResizeHandles()
{
	for (const TObjectPtr<ACSHouseHandleActor>& Handle : ResizeHandles)
	{
		// `SnapToCanonical` 是基类虚函数：锥子摆回墙外、框摆回檐口并重算四条边。
		// 房子在这里**不区分**是哪一种抓手 —— 那正是公共基类的意义。
		if (!IsValid(Handle)) continue;
		Handle->SnapToCanonical();
	}
}

void ACSHouseActor::NotifyResizeHandleDestroyed(ACSHouseHandleActor* Handle)
{
	// 表里没有它 = `ExitResizeMode` 已经摘干净并广播过了，这里再广播就是第二次。
	if (ResizeHandles.Remove(Handle) == 0) return;

	if (ResizeHandles.Num() == 0)
	{
		OnResizeModeChanged.Broadcast(this, false);
	}
}

TArray<ACSHouseHandleActor*> ACSHouseActor::GetResizeHandles() const
{
	TArray<ACSHouseHandleActor*> Out;
	Out.Reserve(ResizeHandles.Num());
	for (const TObjectPtr<ACSHouseHandleActor>& Handle : ResizeHandles)
	{
		if (IsValid(Handle)) Out.Add(Handle);
	}
	return Out;
}

TArray<ACSHouseResizeHandleActor*> ACSHouseActor::GetEdgeHandles() const
{
	TArray<ACSHouseResizeHandleActor*> Out;
	Out.Reserve(ResizeHandles.Num());
	for (const TObjectPtr<ACSHouseHandleActor>& Handle : ResizeHandles)
	{
		if (ACSHouseResizeHandleActor* Edge = Cast<ACSHouseResizeHandleActor>(Handle)) Out.Add(Edge);
	}
	return Out;
}

ACSHouseHeightHandleActor* ACSHouseActor::GetHeightHandle() const
{
	for (const TObjectPtr<ACSHouseHandleActor>& Handle : ResizeHandles)
	{
		ACSHouseHeightHandleActor* Height = Cast<ACSHouseHeightHandleActor>(Handle);
		if (IsValid(Height) && Height->GetSide() == ECSHouseHeightHandleSide::Eave) return Height;
	}
	return nullptr;
}

ACSHouseHeightHandleActor* ACSHouseActor::GetBaseHandle() const
{
	for (const TObjectPtr<ACSHouseHandleActor>& Handle : ResizeHandles)
	{
		ACSHouseHeightHandleActor* Base = Cast<ACSHouseHeightHandleActor>(Handle);
		if (IsValid(Base) && Base->GetSide() == ECSHouseHeightHandleSide::Base) return Base;
	}
	return nullptr;
}

// -----------------------------------------------------------------------------
// Geometry
// -----------------------------------------------------------------------------

void CSHouse_BuildBodySoup(const FCSHouseBodyDesc& Desc, FCSGpuMeshCPUData& S)
{
	FCSHouseMeshWriter Writer{ S, Desc.World };

	const float T = Desc.WallThickness, H = Desc.WallHeight;
	// 材质槽：0 墙面（Masked，按 UV1 逐像素切洞）。槽 1 留给屋顶，但四坡改瓦以后房体里
	// 一片屋面三角都没有 —— 瓦是独立的实例组件。洞缘同样不占槽位（门框是砖块实例）。
	constexpr int32 SlotWall = 0;

	// ---- 四面墙：一串闭合面板。洞不在几何里，由材质按 UV1 的解析判据逐像素 discard 切出 ----
	//
	// 这是 Tiny Glade 原版的开洞方式：CPU 只提供解析参数，洞形在像素阶段成立。洞缘因此是
	// 解析精确曲线（无限分辨率），而不是受弦高容差限制的折线。代价是 discard 只丢像素、
	// 不生成表面 —— 洞缘的厚度断口由门框砖块填满，见下面的说明。
	for (int32 Edge = 0; Edge < Desc.Footprint.NumEdges(); ++Edge)
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
		// 斜接：面板贴着转角的那一端，内皮要沿角平分线让出 `Inset`（凹角为负，内皮反而伸出去）。
		// 只有真正**顶到墙端**的面板才吃这个量；从墙中间起止的面板两端都是直的。被接缝切在斜接区
		// 里面的那种少见情况取 max / min，保证内皮区间不越过斜接面、不与邻边的面板重叠。
		auto InnerSpan = [&F](float SA, float SB, float& OutSAin, float& OutSBin)
		{
			OutSAin = (SA <= 0.5f) ? F.InsetStart : FMath::Max(SA, F.InsetStart);
			OutSBin = (SB >= F.Len - 0.5f) ? (F.Len - F.InsetEnd) : FMath::Min(SB, F.Len - F.InsetEnd);
			// 整块面板都落在斜接区里时内皮会反向：压成零长，棱台退化成三棱柱，面不翻。
			OutSBin = FMath::Max(OutSBin, OutSAin);
		};

		auto AddPanel = [&](float SA, float SB, float Z0, const FCSOpeningClipField& Field, uint8 Tag)
		{
			// H − Z0 也要判：Z0 被参数推到墙顶以上时盒子会退化成反向挤出（面全朝里）。
			if (SB - SA < 0.5f || H - Z0 < 0.5f) return;
			float SAin = SA, SBin = SB;
			InnerSpan(SA, SB, SAin, SBin);
			Writer.SetPanel(Start, U, Field, ECSHousePart::Wall, Tag);
			Writer.AddWallPrism(Start, U, In, Up, SA, SB, SAin, SBin, T, Z0, H, SlotWall);
			if (Z0 > 0.5f)
			{
				// 窗台：真几何而不是 clip 的下界 —— 判据因此只需两个 float（见 CSHouseProfile.h）。
				Writer.SetPanel(Start, U, FCSOpeningClipField(), ECSHousePart::Wall, 0);
				Writer.AddWallPrism(Start, U, In, Up, SA, SB, SAin, SBin, T, 0.0f, Z0, SlotWall);
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
			// 要求按**洞落在这面墙里的那一截**算，不是名义 `O.Width`：端点可能被量化推到墙外
			// （`ComputeDoors` 已经夹过一次，这里是第二道闸），墙外那一截任何面板都盖不到，
			// 拿名义宽去比就会把整个洞判成"装不下" ⇒ 砖拱砌好了、墙没挖洞，一声不吭。
			const float VisibleWidth = FMath::Min(O.S1(), F.Len) - FMath::Max(O.S0(), 0.0f);
			if (CellMax - CellMin < VisibleWidth - 1.0f) continue;   // 装不下这个洞的面板，这一洞放弃

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

	// ---- 屋顶：四坡 + 全瓦片。房体三角汤里**一片屋面都不产**（2026-08-31）。 ----
	//
	// 这里原本是「两块实体坡板 + 两端山墙棱柱 + 两条檐口封口楔形」那一整套双坡结构，整段删除：
	// TG 的屋顶是**四个坡面**、整面**由瓦铺成**（实拍俯视 + `roof_shape::ridge_length_01_from_
	// rectangle_ratio`），既没有山墙这个构件，屋面也不是实体板。连带作废的还有"墙顶该砌到哪"
	// 那条纪律（咬入量 / `SoffitTopZ` / 封口楔形）—— 没有板底可咬，四面墙顶一律平在 WallHeight，
	// 墙顶与屋面之间那条缝在 TG 里本来就是露着的（室内实拍可见漏光）。
	//
	// 屋面几何改由**瓦片实例**承担（`Content/HouseTest/TinyGladeAsset/Meshes/roof_tile`），走 GPU 实例组件那条路
	// （与藤蔓 / 门框砖同构），不进这份三角汤。屋面方程仍然只有 `CSHouseRoof.h` 一份真源：
	// 铺瓦、铺梁、尖顶、雪、以及 D8「落屋顶 → 不生成」谓词全部调它。

	S.SourceSpace = FCSGpuMeshCPUData::ESpace::World;
	S.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerVertex;
	S.NumTexCoordChannels = 2;   // UV0 贴图 / UV1 解析裁剪场
}

void ACSHouseActor::RebuildBodyMesh()
{
	FCSHouseBodyDesc Desc;
	Desc.Footprint = GetFootprint();
	Desc.WallThickness = WallThickness;
	Desc.WallHeight = WallHeight;
	Desc.PierWidth = PierWidth;
	Desc.Openings = CurrentOpenings;
	Desc.SeamCuts = CurrentSeamCuts;
	Desc.World = GetBuildTransform();

	FCSGpuMeshCPUData S;
	CSHouse_BuildBodySoup(Desc, S);

	const int32 TriangleCount = S.Indices.Num() / 3;
	BodySlot.BuiltAt = Desc.World;
	SubmitBodyMesh(MakeShared<FCSGpuMeshCPUData, ESPMode::ThreadSafe>(MoveTemp(S)));
	UE_LOG(LogTinyGladeHouse, Log, TEXT("[TinyGladeHouse] %s body rebuilt: tris=%d openings=%d (doors=%d)"),
		*GetName(), TriangleCount, CurrentOpenings.Num(), GetOpenDoorCount());
}

void ACSHouseActor::SubmitBodyMesh(TSharedPtr<FCSGpuMeshCPUData, ESPMode::ThreadSafe> Snapshot)
{
	FCSMeshSlotUpload Upload;
	// ⚠️ 槽 1（屋顶）现在**一个三角都没有**（四坡改瓦片实例）—— 空分段只是画不出东西，无害；
	// 槽位留着是为了不动这张 P2 冻结的槽表，`RoofMaterial` 本身归瓦片那条路用。
	Upload.Materials = { WallMaterial, RoofMaterial };
	Upload.NumTexCoordSets = 2;   // UV1 传裁剪场
	Upload.bSortSections = true;
	SubmitMeshSlotAsync(TinyGladeMeshComponent, TinyGladeMesh, BodySlot, Snapshot, Upload, [this]() { OnBodyEditComplete(); });
}

void ACSHouseActor::OnBodyEditComplete()
{
	// 最新态合并：在途期间攒下的最后一份目标现在补发。
	if (const TSharedPtr<FCSGpuMeshCPUData, ESPMode::ThreadSafe> Next = TakePending(BodySlot.Pending))
	{
		SubmitBodyMesh(Next);
		return;
	}

	// 在途期间被推迟的摆位增量在这里补上。没有这一步，"拖动中恰好撞上一次形状重建"的那一帧
	// 位移就永远丢了 —— 而快扫看到的变换没再变，不会来第二次唤醒。增量相对 BodySlot.BuiltAt
	// 算，所以补送的是累计量，不是重放。
	if (ApplyBodyPlacement()) BodySlot.PlacementHash = ComputePlacementHash();
}

bool ACSHouseActor::ApplyBodyPlacement()
{
	return ApplyMeshSlotPlacement(GetTinyGladeMesh(), BodySlot, GetBuildTransform(), [this]() { OnBodyEditComplete(); });
}

namespace
{
/**
 * 交接包围盒从**构建空间**（`GetBuildTransform()`，只取 yaw）映射到**组件空间**（实例原点所在的空间）。
 * 房子没有 pitch / roll / 缩放时是恒等变换，逐位不改现状；有的话不映射，剔除盒就偏、视锥边缘闪
 * （2026-09-07 审查 B5：五家里以前只有门框砖做了这一步）。只有 yaw 时也因此不会多出一次交接。
 */
FBox CSHouse_BuildBoundsToComponent(const FTransform& BuildTransform, const USceneComponent* Component, const FBox& Bounds)
{
	if (!Component || !Bounds.IsValid) return Bounds;
	const FTransform BuildToComponent = BuildTransform * Component->GetComponentTransform().Inverse();
	return BuildToComponent.Equals(FTransform::Identity, 1.0e-4) ? Bounds : Bounds.TransformBy(BuildToComponent);
}
}

void ACSHouseActor::EnsurePillarBrickComponent()
{
	// 新组件身上没有实例源，交接缓存必须一起作废 —— 缓存说"交接过了"而组件是空的，
	// 下一趟就会被判成稳态而跳过，画面永远空白且不报错。
	if (CSShaperSteps::EnsureInstancedComponent(this, PillarBrickComponent)) PillarHandover.Capacities.Reset();
	PillarBrickComponent->InstanceMaterial = PillarMaterial;
	PillarBrickComponent->SetBaseMesh(PillarBrickMesh);   // 同一张网格时内部直接早退

	if (PillarGpuBuffers.Num() != 1)
	{
		CSShaperSteps::ReleaseOnRenderThread(PillarGpuBuffers);
		PillarGpuBuffers.SetNum(1);
		PillarHandover.Capacities.Reset();
	}

	// 剔除球与三轴块尺寸都从基础网格的包围盒推。⚠️ `brick` 是 1×1×1 的**居中**字典 mesh，
	// 所以 BlockSize 恒等于 (1,1,1) —— 记录里的轴已经带着真实尺寸了（见 CSHousePillar.usf）。
	// 留着这一步是为了换资产时不用改 kernel。
	const FBox Local = PillarBrickMesh ? PillarBrickMesh->GetBoundingBox() : FBox(ForceInit);
	PillarGpuBuffers[0].BaseSphereCentre = Local.IsValid ? FVector3f(Local.GetCenter()) : FVector3f::ZeroVector;
	PillarGpuBuffers[0].BaseSphereRadius = Local.IsValid ? float(Local.GetExtent().Size()) : 0.0f;
	const FVector MeshSize = Local.IsValid ? Local.GetSize() : FVector(1.0);
	PillarGpuBuffers[0].BlockSize = FVector3f(
		float(1.0 / FMath::Max(MeshSize.X, UE_KINDA_SMALL_NUMBER)),
		float(1.0 / FMath::Max(MeshSize.Y, UE_KINDA_SMALL_NUMBER)),
		float(1.0 / FMath::Max(MeshSize.Z, UE_KINDA_SMALL_NUMBER)));

	if (!PillarBrickMesh || !PillarBrickComponent) return;

	// 容量按**配置上限**一次付清，之后永不扩容（零阻塞纪律，同门框砖 / 藤蔓）。
	// 上限 = 周界柱位数 × 每根最多几层。柱长由地面空隙定，没有上界，所以层数要自己钉一个：
	// 500 cm 的悬空已经远超任何合理场景，再高就截断 —— 少砌几块砖远好过在拖动的某一帧
	// 付一次设备同步。
	const FCSHouseFootprint Footprint = GetFootprint();
	const double Perimeter = Footprint.GetPerimeter();
	const int32 MaxPillars = FMath::CeilToInt(Perimeter / FMath::Max(PillarSpacing, 20.0f)) + Footprint.NumEdges();
	const int32 MaxCourses = FMath::CeilToInt(500.0f / FMath::Max(PillarCourseHeight, 4.0f));
	const uint32 MaxBricks = uint32(FMath::Clamp(
		CSShaperSteps::ReserveCount(MaxPillars * MaxCourses), 64, 1 << 16));
	CSShaperSteps::ReserveCapacity(PillarGpuBuffers, MaxBricks);

	// 交接包围盒：量化 + 只涨不缩（理由同门框砖那段 —— 拖尺寸时 1 cm 阈值会让每帧重走一次
	// 阻塞的 SetInstanceSourceGPU）。柱子往**下**长，所以下界要给足。
	const double Reach = CSShaperSteps::QuantizeUp(Footprint.GetCenteredSpan() * 0.6 + PillarSize);
	const double Depth = CSShaperSteps::QuantizeUp(500.0 + PillarEmbed);
	FBox LocalBounds(FVector(-Reach, -Reach, -Depth), FVector(Reach, Reach, PillarSize));
	LocalBounds = CSHouse_BuildBoundsToComponent(GetBuildTransform(), PillarBrickComponent, LocalBounds);
	LocalBounds = CSShaperSteps::MergeHandoverBounds(LocalBounds, PillarHandover, bForceFullRebuild);
	// 柱砖的 kernel（CSHousePillar.usf）不写 custom data，柱材质也不读：不交，省掉剔除 pass 里逐可见槽
	// 抄一遍全零的那一趟（2026-09-07 审查 B1 的柱子那条：以前交的是一块从未被 kernel 写过的 buffer）。
	CSShaperSteps::HandOverInstanceSource(
		CSShaperSteps::MakeHandoverSource(PillarBrickComponent, PillarGpuBuffers[0], /*bWithCustomData*/ false),
		LocalBounds, PillarHandover);
}

void ACSHouseActor::RebuildPillarMesh(const TArray<FVector>& Centers, const TArray<float>& Lengths)
{
	// ── 砖石柱（2026-09-06 裁决）────────────────────────────────────────────────
	if (bPillarUseBricks && PillarBrickMesh)
	{
		// ⚠️ 另一条必须显式清掉，否则方盒与砖同时画 —— 藤蔓换管子时正是漏了这一条。
		// 连在途待发的那份方盒一起作废（ClearMeshSlot），否则完成回调会把它补发回来。
		ClearMeshSlot(PillarMeshComponent, PillarMesh, PillarSlot.Pending);

		EnsurePillarBrickComponent();
		if (!PillarGpuBuffers.Num() || !PillarGpuBuffers[0].IsValid()) return;

		CSHousePillar::FParams PillarParams;
		PillarParams.BrickWidth = PillarSize;
		PillarParams.CourseHeight = PillarCourseHeight;
		PillarParams.YawJitter = PillarYawJitter;
		PillarParams.BracketCourses = PillarBracketCourses;
		PillarParams.BracketOverhang = PillarBracketOverhang;
		PillarParams.Seed = VineSeed;   // 与藤共用用户种子：同一栋房子的随机看得出同源

		TArray<CSHousePillar::FBrick> Bricks;
		CSHousePillar::BuildBricks(Centers, Lengths, GetBuildTransform(), PillarParams, Bricks);

		// ⚠️ 用**组件自己的变换**求逆，不用 actor 的（同藤蔓那条：房子被 pitch/roll
		// 或缩放时 actor 的完整逆变换与 GetBuildTransform 的只取 yaw 会打架）。
		const FMatrix44f WorldToComponent = FMatrix44f(
			PillarBrickComponent->GetComponentTransform().ToInverseMatrixWithScale());
		CSHousePillar::Pack(Bricks, PillarGpuBuffers[0], WorldToComponent);

		UE_LOG(LogTinyGladeHouse, Log, TEXT("[TinyGladeHouse] %s pillars rebuilt: count=%d bricks=%d"),
			*GetName(), Centers.Num(), Bricks.Num());
		return;
	}

	// ── 旧路：摞方盒（占位，砖那条定型后连同开关一起删）──────────────────────
	// 反向也要清：从砖切回方盒时砖的 counter 得归零，否则两套并存。
	if (PillarGpuBuffers.Num() && PillarGpuBuffers[0].IsValid()) CSShaperSteps::ZeroCounters(PillarGpuBuffers);

	if (!PillarMeshComponent) return;
	if (Centers.IsEmpty())
	{
		// 下次有柱时重建，别让摆位快路径去搬一份空网格；在途待发的旧柱一并作废。
		ClearMeshSlot(PillarMeshComponent, PillarMesh, PillarSlot.Pending);
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
	PillarSlot.BuiltAt = BuildTransform;
	SubmitPillarMesh(MakeShared<FCSGpuMeshCPUData, ESPMode::ThreadSafe>(MoveTemp(S)));
	UE_LOG(LogTinyGladeHouse, Log, TEXT("[TinyGladeHouse] %s pillars rebuilt: count=%d tris=%d"), *GetName(), Centers.Num(), TriangleCount);
}

void ACSHouseActor::SubmitPillarMesh(TSharedPtr<FCSGpuMeshCPUData, ESPMode::ThreadSafe> Snapshot)
{
	// 柱子只有一个材质槽，不需要排序分段 —— 一次上传就是全部工作。容量同房体按台阶预留：
	// 柱数会随周界（=FootprintSize）跳变，按精确数要就是每次跳变一次阻塞重分配。
	FCSMeshSlotUpload Upload;
	Upload.Materials = { PillarMaterial };
	SubmitMeshSlotAsync(PillarMeshComponent, PillarMesh, PillarSlot, Snapshot, Upload, [this]() { OnPillarEditComplete(); });
}

void ACSHouseActor::OnPillarEditComplete()
{
	if (const TSharedPtr<FCSGpuMeshCPUData, ESPMode::ThreadSafe> Next = TakePending(PillarSlot.Pending))
	{
		SubmitPillarMesh(Next);
		return;
	}
	if (ApplyPillarPlacement()) PillarSlot.PlacementHash = ComputePlacementHash();
}

void ACSHouseActor::ResolveVineSpawnTimes(const CSHouseVine::FPlan& Plan, TArray<float>& OutSpawnTimes)
{
	OutSpawnTimes.Reset(Plan.Strands.Num());

	const float Now = GetWorld() ? float(GetWorld()->GetTimeSeconds()) : 0.0f;
	const float Speed = FMath::Max(VineGrowSpeed, 1.0f);
	// 从没长过的哨兵：足够早，材质算出来的前沿远超任何弧长 ⇒ 第一帧就是长成的。
	const float GrownSentinel = -1.0e6f;

	TMap<uint32, FVineStrandHistory> NextHistory;
	NextHistory.Reserve(Plan.Strands.Num());

	for (const CSHouseVine::FStrand& Strand : Plan.Strands)
	{
		const FVineStrandHistory* Prev = VineStrandHistory.Find(Strand.RootKey);
		float Spawn;

		if (!Prev)
		{
			// 全新的一根（第一次生成、或跨过藤位间距新增的那根）。
			Spawn = bVineGrowOnLoad ? Now : GrownSentinel;
		}
		else
		{
			// 第一个不同的点。⚠️ 逐点比较用**墙面参数坐标 + 所在墙**，容差取 0.5 cm ——
			// 比它更严会把浮点噪声判成"变了"，于是每次重求值都重新长一遍。
			int32 Diverge = 0;
			const int32 Common = FMath::Min(Prev->PointsSZ.Num(), Strand.Points.Num());
			while (Diverge < Common)
			{
				const CSHouseVine::FStrandPoint& P = Strand.Points[Diverge];
				const bool bSame = Prev->Edges.IsValidIndex(Diverge)
					&& Prev->Edges[Diverge] == P.EdgeIndex
					&& (Prev->PointsSZ[Diverge] - P.WallSZ).IsNearlyZero(0.5f);
				if (!bSame) break;
				++Diverge;
			}

			const bool bIdentical = (Diverge == Common)
				&& Prev->PointsSZ.Num() == Strand.Points.Num();
			if (bIdentical)
			{
				Spawn = Prev->SpawnTime;
			}
			else
			{
				// 变化点的弧长。`Diverge` 是"第一个不同的点"，它之前那一截与上一轮逐点相同，
				// 所以那一截已经长出来的部分应当留着。
				const int32 ArcIndex = FMath::Clamp(Diverge, 0, Strand.Arc.Num() - 1);
				const float DivergeArc = Strand.Arc.IsValidIndex(ArcIndex) ? Strand.Arc[ArcIndex] : 0.0f;
				const float Front = (Now - Prev->SpawnTime) * Speed;

				// 前沿还没长到变化点 ⇒ 那段变化对它不可见，相位不用动。
				// 越过了 ⇒ 把前沿拉回变化点（等价于 SpawnTime 往后挪），从那里接着长。
				Spawn = (Front > DivergeArc) ? (Now - DivergeArc / Speed) : Prev->SpawnTime;
			}
		}

		OutSpawnTimes.Add(Spawn);

		FVineStrandHistory Entry;
		Entry.SpawnTime = Spawn;
		Entry.PointsSZ.Reserve(Strand.Points.Num());
		Entry.Edges.Reserve(Strand.Points.Num());
		for (const CSHouseVine::FStrandPoint& P : Strand.Points)
		{
			Entry.PointsSZ.Add(P.WallSZ);
			Entry.Edges.Add(P.EdgeIndex);
		}
		NextHistory.Add(Strand.RootKey, MoveTemp(Entry));
	}

	// ⚠️ **整表替换而不是往里塞**：房子反复改尺寸会让键不断变化，只加不删的话这张表
	// 会随编辑次数无界增长，而且泄漏得毫无症状（每根藤还带着一份折线副本）。
	VineStrandHistory = MoveTemp(NextHistory);
}

void ACSHouseActor::SubmitVineTube(TSharedPtr<CSHouseVine::FTubePath, ESPMode::ThreadSafe> Path)
{
	if (!Path.IsValid() || !VineTubeComponent) return;

	EnsureSlotMesh(VineTubeComponent, VineTubeMesh);   // 归藤管组件所有：组件销毁时它自己还显存
	// 走 MID 而不是材质资产本身：`VineGrowSpeed` 要下推（见 VineBranchGrowMID 的注释）。
	UMaterialInterface* TubeMaterial = VineBranchGrowMID
		? static_cast<UMaterialInterface*>(VineBranchGrowMID) : ToRawPtr(VineBranchMaterial);
	BindMeshSlotMaterials(VineTubeComponent, VineTubeMesh, { TubeMaterial });

	// 在途被拒 ⇒ 只留**最新**那一份（不是排队）。拖尺寸时每 tick 都会来一次，排队的话
	// 松手后要把整段拖动重放一遍；只留最新则最多落后一帧。同基类的网格槽。
	if (ParkIfInFlight(VineTubeMesh, PendingVineTubePath, Path)) return;

	VineTubeComponent->SetGpuMesh(VineTubeMesh);

	CSVineTube::FParams TubeParams;
	TubeParams.ProfileCount = uint32(FMath::Clamp(VineTubeSegments, 3, 24));
	// ⚠️ 与下面 PackTubePath 传的必须是**同一个** CircleScale：环半径 =
	// 10 * CircleScale * Points[i].w，而那个 .w 正是按这个式子反解出来的。
	TubeParams.CircleScale = CSHouseVine_TubeCircleScale;

	TWeakObjectPtr<ACSHouseActor> WeakThis(this);
	const bool bIssued = CSVineTube::BuildTubeIntoMesh(
		VineTubeMesh, Path->Points, Path->Axes, Path->PointMeta, Path->SegmentMeta, Path->Growth, TubeParams,
		[WeakThis](bool /*bBuilt*/)
		{
			if (ACSHouseActor* House = WeakThis.Get()) House->OnVineTubeEditComplete();
		});

	if (!bIssued)
	{
		// 递交失败**不重试**：多半是折线自相矛盾或容量被拒，重试只会每帧再失败一次。
		// 留一行日志，让"有折线却没有藤"这件事在日志里看得见（而不是画面上一片空白）。
		UE_LOG(LogTinyGladeHouse, Warning,
			TEXT("[TinyGladeHouse] %s vine tube submit refused: points=%d segs=%d"),
			*GetName(), Path->Points.Num(), Path->SegmentMeta.Num());
	}
}

void ACSHouseActor::OnVineTubeEditComplete()
{
	if (const TSharedPtr<CSHouseVine::FTubePath, ESPMode::ThreadSafe> Next = TakePending(PendingVineTubePath)) SubmitVineTube(Next);
}

bool ACSHouseActor::ApplyPillarPlacement()
{
	return ApplyMeshSlotPlacement(PillarMesh, PillarSlot, GetBuildTransform(), [this]() { OnPillarEditComplete(); });
}

// -----------------------------------------------------------------------------
// Frame（门框砖）
// -----------------------------------------------------------------------------

CSShaperSteps::EHandoverResult ACSHouseActor::EnsureFrameComponent()
{
	CSShaperSteps::EnsureInstancedComponent(this, FrameComponent);
	FrameComponent->InstanceMaterial = FrameMaterial;
	FrameComponent->SetBaseMesh(FrameBrickMesh);   // 同一张网格时内部直接早退

	if (FrameGpuBuffers.Num() != 1)
	{
		CSShaperSteps::ReleaseOnRenderThread(FrameGpuBuffers);
		FrameGpuBuffers.SetNum(1);
		FrameHandover.Capacities.Reset();
	}

	// 剔除球与三轴块尺寸都从基础网格的包围盒推：TG 的 brick 是 100³ 居中盒，所以
	// BlockSize = 想要的尺寸 / 网格自身尺寸，最终实例尺寸才等于我们写的那几个 cm 值。
	const FBox Local = FrameBrickMesh ? FrameBrickMesh->GetBoundingBox() : FBox(ForceInit);
	FrameGpuBuffers[0].BaseSphereCentre = Local.IsValid ? FVector3f(Local.GetCenter()) : FVector3f::ZeroVector;
	FrameGpuBuffers[0].BaseSphereRadius = Local.IsValid ? float(Local.GetExtent().Size()) : 0.0f;

	const FVector MeshSize = Local.IsValid ? Local.GetSize() : FVector(1.0);
	// 自动档：墙厚 + 两面各凸 `FrameBrickProtrude`（理由见那个字段的注释）。显式给了厚度就照给的用。
	const float Thickness = FrameBrickThickness > 0.5f ? FrameBrickThickness : WallThickness + 2.0f * FrameBrickProtrude;
	// 长度轴乘胀大系数（TG 那一步，理由与轴的对位见 FrameBrickBloat 的字段注释）：砖心由
	// `CSHouseFrame::SolveRun` 按未胀大的 FrameBrickLength 定下、这里只放大**渲染尺寸**，
	// 相邻砖因此必然互相穿插。别改成把系数折进砖长 —— 那会连砖数和位置一起变，就不是"胀大"
	// 而是"砖变长"，块数跳变照样露缝。
	const float Bloat = FMath::Max(FrameBrickBloat, 1.0f);
	FrameGpuBuffers[0].BlockSize = FVector3f(
		float(FrameBrickDepth / FMath::Max(MeshSize.X, 1.0)),
		float(FrameBrickLength * Bloat / FMath::Max(MeshSize.Y, 1.0)),
		float(Thickness / FMath::Max(MeshSize.Z, 1.0)));

	if (!FrameBrickMesh || !FrameComponent) return CSShaperSteps::EHandoverResult::UpToDate;

	// **容量与实例源交接都在这里一次付清** —— 两者都是阻塞的（前者要在渲染线程分配，
	// 后者内部走 SetStreamLayoutSync + 立刻重建 render state）。留给 RebuildFrame 去做的话，
	// 它们会落在"画路的某一笔"上：那一笔恰好是第一次长出砖、或砖数第一次超过容量的那一笔，
	// 取决于用户画到哪里，既掉帧又难复现。这里做完，交互期的 RebuildFrame 就只剩录 pass。
	CSShaperSteps::ReserveCapacity(FrameGpuBuffers, uint32(EffectiveFrameCapacity()));

	// 实例源的包围盒是**组件空间**的保守盒，且必须自己把基础网格的尺寸算进去（packed 路径不代劳）。
	//
	// 量化 + 只涨不缩（理由与保证见 CSShaperSteps::QuantizeUp 上面那段）：原来这两个量直接就是
	// FootprintSize / WallHeight 的连续函数，而下面 bNeedHandover 的阈值是 1 cm ——
	// 拖尺寸时每帧都判"包围盒变了"，于是每帧重走一次阻塞的 SetInstanceSourceGPU。
	// 全量重建时不并旧盒，让它重新收紧（否则一次误拉大就永久留着保守盒）。
	const double Reach = CSShaperSteps::QuantizeUp(GetFootprint().GetCenteredSpan() * 0.6 + FrameBrickLength);
	const double Top = CSShaperSteps::QuantizeUp(WallHeight + FrameBrickLength);
	FBox LocalBounds(FVector(-Reach, -Reach, -FrameBrickLength), FVector(Reach, Reach, Top));

	// ⚠️ 这个盒是按 footprint 在**构建空间**（`GetBuildTransform()`，只取 yaw）里量的，而实例
	// 原点存在**组件空间**里 —— 两者只有在房子没有 pitch/roll/缩放时才重合。状态文件
	// 「已知潜伏问题」那条说的就是这个不对称（裁决一要求"按同一个变换口径写死"）：
	// 盒子跟着做一次 构建空间 → 组件空间 的映射，剩下的两处口径就自洽了（五家同一个 helper）。
	LocalBounds = CSHouse_BuildBoundsToComponent(GetBuildTransform(), FrameComponent, LocalBounds);

	LocalBounds = CSShaperSteps::MergeHandoverBounds(LocalBounds, FrameHandover, bForceFullRebuild);
	// 门框砖没有生长动画，不吃逐实例 custom data —— 交了只会让剔除 pass 多抄一遍全零。
	return CSShaperSteps::HandOverInstanceSource(
		CSShaperSteps::MakeHandoverSource(FrameComponent, FrameGpuBuffers[0], /*bWithCustomData*/ false),
		LocalBounds, FrameHandover);
}

uint32 ACSHouseActor::BuildFrameArches(TArray<CSHouseFrame::FElement>& OutElements, int32& OutBrickCount) const
{
	OutElements.Reset();
	OutBrickCount = 0;
	if (!bFrameEnabled || !FrameBrickMesh || CurrentOpenings.IsEmpty()) return 0;

	const FTransform World = GetBuildTransform();
	const FCSHouseFootprint Footprint = GetFootprint();
	const float T = WallThickness;

	CSHouseFrame::FBrickParams Params;
	Params.Length = FMath::Max(FrameBrickLength, 1.0f);
	Params.Gap = FMath::Max(FrameBrickGap, 0.0f);
	Params.CapitalScale = PierCapitalScale;     // 只有门框这条路出柱头；接缝柱 / 角石沿用默认 0
	Params.CapitalHeight = PierCapitalHeight;
	// **容量恒定**：注册期的 `ReserveCapacity` 已经一次付清，这里只按它截断、绝不扩容。
	// 门框这条路上因此没有任何一次可扩容调用 —— 那正是"交互热路径零设备同步"这条纪律要的：
	// 扩容是阻塞刷新，落在用户恰好画到的那一笔上。
	Params.MaxBricks = EffectiveFrameCapacity();

	// `CurrentOpenings` 进来时已按 (边, CenterS) 排好（`ComputeDoors` 末尾那一下），同一条边的
	// 洞因此是**连续片段**；墩的样式位（`ResolvePierSpans`）也建立在同一个顺序上。别在这里重排。
	int32 Begin = 0;
	while (Begin < CurrentOpenings.Num())
	{
		int32 End = Begin;
		while (End < CurrentOpenings.Num() && CurrentOpenings[End].EdgeIndex == CurrentOpenings[Begin].EdgeIndex) ++End;

		// **与房体面板同一份 `CSHouse_GetEdge`**：墙在哪儿只能有一个真源（同 BuildVineStrips）。
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(CurrentOpenings[Begin].EdgeIndex, Footprint, T);
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
			int32(E.Path.MidKind) | (E.Path.bLeftJamb ? 0x10 : 0) | (E.Path.bRightJamb ? 0x20 : 0) });
	}
	// 胀大系数与三轴尺寸只改 `BlockSize`、一条路都不改，而 `RebuildFrame` 的早退门看的就是
	// 这个哈希 —— 不把它们算进来，改了系数就只会静默无效（与 D14 开篇 FrameMaterial 同型）。
	// `FrameSeed` 在单条目 palette 下已经不影响任何东西（`SolveRun` 里没有随机），留着是为了
	// 将来加 palette 时不会静默跳过重建。
	Hash.Append({ CSHouse_Q(PierCapitalScale, 0.01), CSHouse_Q(PierCapitalHeight, 0.5),
		CSHouse_Q(FrameBrickProtrude, 0.5),
		CSHouse_Q(FrameBrickLength, 0.5), CSHouse_Q(FrameBrickDepth, 0.5),
		CSHouse_Q(FrameBrickThickness, 0.5), CSHouse_Q(FrameBrickGap, 0.1),
		CSHouse_Q(FrameBrickBloat, 0.001), FrameSeed });
	return CSHouse_Hash(Hash);
}

void ACSHouseActor::RebuildFrame()
{
	const CSShaperSteps::EHandoverResult FrameHandoverResult = EnsureFrameComponent();

	TArray<CSHouseFrame::FElement> Elements;
	int32 BrickCount = 0;
	// 门框砖先、接缝砖后：全局砖序号跨两者连续（整栋房子一个 dispatch），而**次序是承重的** ——
	// 反过来的话每开一扇门都会把所有接缝砖的槽位推一格，从而推掉它们的逐实例随机数。
	// （接缝砖的随机数已经从接缝身份派生、与槽位无关，但门框砖那边仍然是槽位。）
	const uint32 ArchHash = BuildFrameArches(Elements, BrickCount);
	const uint32 SeamHash = BuildSeamBricks(Elements, BrickCount);
	// 转角墩（转角配成墩的角上那根柱础/柱身/柱头）：随洞表变，随机数从房子身份派生，
	// 排在门框砖之后任何位置都无害；放在角石之前是因为角石要在它的墩顶以下让路，读同一份高度。
	const uint32 CornerPierHash = BuildCornerPierBricks(Elements, BrickCount);
	// 角石最后：它的砖数只随 footprint / 墙高变，排在最后就不会因为开一扇门或来一个邻居
	// 而把别人的槽位推走（反过来它自己被推走无害 —— 随机数已从房子身份派生）。
	const uint32 QuoinHash = BuildQuoinBricks(Elements, BrickCount);
	// 包边最后：它随 footprint / 墙高 / 洞表变，排在最后就不会推走别人的槽位。
	const uint32 TrimHash = BuildTrimBricks(Elements, BrickCount);
	// 砖层**真的**最后：它是砖数最多的一家（一栋 6×4 m 的房约 1400 块），排在最后
	// ⇒ 撞容量上限时先截断的是它自己，门框 / 接缝 / 角石 / 包边一块都不少。
	const uint32 BrickWallHash = BuildBrickWallBricks(Elements, BrickCount);

	// **五家加起来**撞上限也要出声。单家那条警告（`BuildFrameArches` 里）只看得见自己，
	// 而砖层排在最后 ⇒ 真正被截掉的总是它，症状是"墙砌到一半"而所有断言全绿。
	if (BrickCount >= EffectiveFrameCapacity())
	{
		UE_LOG(LogTinyGladeHouse, Warning,
			TEXT("[TinyGladeHouse] %s 砖数撞上常驻容量（%d 块，其中砖层 %d）。容量注册期一次付清、"
				"**只截断不扩容** —— 把 FrameReserveCapacity 调大（上限 65536）再重开关卡。"),
			*GetName(), EffectiveFrameCapacity(), CurrentBrickWallBrickCount);
	}
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
	// `ClearInstanceSourceGPU()`（它把 `FrameHandover` 清空），下一次 `EnsureFrameComponent`
	// 就得重走阻塞的 `SetInstanceSourceGPU`。**实测代价是每轮 2 次阻塞刷新**，四条零阻塞断言
	// （画一笔 / 拖带柱的房子 / 带藤拖 / 带摆件拖 —— 全是没有门因而没有砖的那栋）当场从 0 变成 2。
	// 有砖的房子看不见这条，因为它的哈希本来就非零。
	// （砖表空时 `FrameHash` 恒为 0 —— `CSHouse_Hash` 对空表返回 0 —— 所以只判前两个就够，
	// 但仍把它写进合并里，免得将来有人改了空表约定而这里静默失配。）
	const uint32 NewHash = (ArchHash == 0 && SeamHash == 0 && CornerPierHash == 0 && QuoinHash == 0 && TrimHash == 0 && BrickWallHash == 0)
		? 0u : CSHouse_Hash({ int32(ArchHash), int32(SeamHash), int32(CornerPierHash), int32(QuoinHash), int32(TrimHash), int32(BrickWallHash), int32(FrameHash) });

	bool bBuffersReady = FrameGpuBuffers.Num() == 1 && FrameHandover.Capacities.Num() == 1;
	for (const CSShaperSteps::FPaletteBuffers& Buffers : FrameGpuBuffers) bBuffersReady &= Buffers.IsValid();
	// 刚交接过（扩容换了清零的新 buffer，或包围盒变了）就必须重排一次：哈希没变也不能拿它当"砖还在"
	// （2026-09-07 审查 B1：以前这道门只看哈希与数量，扩容那一轮画的是池子残值）。有砖才强制 ——
	// 没砖时 counter 本来就该是 0，强制反而会走进下面的撤源分支，把交接缓存清空，下一轮再交、再撤，
	// 每轮白付两次阻塞（正是上面那段注释里那种失败形状）。
	const bool bMustRescatter = FrameHandoverResult == CSShaperSteps::EHandoverResult::HandedOver && BrickCount > 0;
	if (!bMustRescatter && NewHash == FrameDescHash && CurrentFrameBrickCount == BrickCount && (BrickCount == 0 || bBuffersReady)) return;

	FrameDescHash = NewHash;
	CurrentFrameBrickCount = BrickCount;
	// 越过早退门 = 这一趟砖真的会被重排（下面两条分支都会写 GPU）。判据见 GetFrameScatterCount。
	++FrameScatterCount;

	if (BrickCount == 0)
	{
		// ⚠️ **撤实例源之前必须先把 counter 清零。**
		//
		// 撤掉之后 `FrameHandover` 被清空，于是**下一轮 `EnsureFrameComponent` 会把同一批
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
		FrameHandover.Reset();
		return;
	}

	if (!IsValid(FrameComponent)) return;

	// **一个字节都不分配**：砖数已经在 BuildFrameArches 里按常驻容量截断，扩容那一次阻塞
	// 刷新在这条路上永远不会发生 —— 容量恒定正是解析推导的红利，别退化掉。
	CSHouseFrame::Scatter(Elements, FrameGpuBuffers, WorldToComponent);

	UE_LOG(LogTinyGladeHouse, Log, TEXT("[TinyGladeHouse] %s frame scattered: bricks=%d paths=%d seam=%d quoin=%d trim=%d"),
		*GetName(), BrickCount, Elements.Num(), CurrentSeamBrickCount, CurrentQuoinBrickCount, CurrentTrimBrickCount);
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
	H.Footprint = GetFootprint();
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
		for (int32 Edge = 0; Edge < Self.Footprint.NumEdges(); ++Edge)
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
	Params.MaxBricks = EffectiveFrameCapacity();

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

// -----------------------------------------------------------------------------
// 转角墩（2026-09-06）—— 转角配成墩的角上那根柱础 / 柱身 / 柱头，与拱廊的墩同一副样子
// -----------------------------------------------------------------------------

uint32 ACSHouseActor::BuildCornerPierBricks(TArray<CSHouseFrame::FElement>& InOutElements, int32& InOutBrickCount)
{
	CurrentCornerPierCount = 0;
	if (!bFrameEnabled || !FrameBrickMesh) return 0;
	bool bAny = false;
	for (const float TopZ : CornerPierTopZ) bAny |= TopZ > UE_KINDA_SMALL_NUMBER;
	if (!bAny) return 0;

	CSHouseFrame::FBrickParams Params;
	Params.Length = FMath::Max(FrameBrickLength, 1.0f);
	Params.Gap = FMath::Max(FrameBrickGap, 0.0f);
	Params.CapitalScale = PierCapitalScale;     // 与拱廊的墩同一对参数：转角墩就是那种墩，只是立在角上
	Params.CapitalHeight = PierCapitalHeight;
	Params.MaxBricks = EffectiveFrameCapacity();

	// 墩心放在两面墙**墙厚中线的交点**（`PointAtDepth(T/2)`，直角上是沿平分线内缩 T/√2）：
	// 门樘砖走墙厚正中（`BuildFrameArches` 的 Mid），拱廊的墩也在那条线上。角石（Inset 0）留在外角当面层。
	// 角点与角平分线与角石同一个真源 `CSHouse_GetCorner`。墩不看 `IsQuoinCorner`：凹角 / 锐角上
	// 不包角石，但两道拱照样可以在那里配成墩。
	const float BaseZ = float(GetActorLocation().Z);
	const FTransform World = GetBuildTransform();
	const FCSHouseFootprint Footprint = GetFootprint();

	const uint32 Seed = HouseId.IsValid() ? GetTypeHash(HouseId) : uint32(GetUniqueID());
	int32 Cursor = CSHouseFrame::NextBrickSlot(InOutElements);
	TArray<int32> H;
	for (int32 Corner = 0; Corner < CornerPierTopZ.Num() && Corner < Footprint.NumEdges(); ++Corner)
	{
		const float TopZ = CornerPierTopZ[Corner];
		if (TopZ <= UE_KINDA_SMALL_NUMBER) continue;
		const FCSHouseCornerFrame CF = CSHouse_GetCorner(Corner, Footprint);
		const FVector2D Local = CF.PointAtDepth(WallThickness * 0.5);
		const FVector WorldPoint = World.TransformPosition(FVector(Local.X, Local.Y, 0.0));
		const FVector WorldOut = World.TransformVectorNoScale(FVector(CF.Outward.X, CF.Outward.Y, 0.0));
		const FVector2D Point(WorldPoint.X, WorldPoint.Y);
		const FVector2D Outward = FVector2D(WorldOut.X, WorldOut.Y).GetSafeNormal();
		const int32 Added = CSHouseFrame::AppendCornerPier(Point, Outward, BaseZ, BaseZ + TopZ,
			Seed, Corner, Params, InOutElements, Cursor);
		if (Added <= 0) continue;
		++CurrentCornerPierCount;
		InOutBrickCount += Added;
		// 哈希只记标量：角序号、砖数、柱心、墩顶。逐砖位置是它们的纯函数。
		H.Append({ Corner, Added, CSHouse_Q(Point.X, 1), CSHouse_Q(Point.Y, 1), CSHouse_Q(TopZ, 1) });
	}
	return H.IsEmpty() ? 0u : CSHouse_Hash(H);
}

// -----------------------------------------------------------------------------
// 角石（D7 的墙自身转角）—— 纯函数：footprint 四角 → 竖直砖柱，与接缝柱共用发射器
// -----------------------------------------------------------------------------

uint32 ACSHouseActor::BuildQuoinBricks(TArray<CSHouseFrame::FElement>& InOutElements, int32& InOutBrickCount)
{
	CurrentQuoinColumnCount = 0;
	CurrentQuoinBrickCount = 0;
	// 与接缝砖同一条前置：没有组件宿主 / 没有砖网格就整条不出。`bFrameEnabled` 是三家共同的总闸。
	if (!bQuoinEnabled || !bFrameEnabled || !FrameBrickMesh) return 0;

	CSHouseFrame::FBrickParams Params;
	Params.Length = FMath::Max(FrameBrickLength, 1.0f);
	Params.Gap = FMath::Max(FrameBrickGap, 0.0f);
	// **与门框砖 / 接缝砖共用同一份常驻容量**：角石只是同一个组件里排在最后的那些行。
	Params.MaxBricks = EffectiveFrameCapacity();
	// 角石的 TG 基准厘米在 BuildQuoinElements 里按层高折算。
	// 原函数随机改变长边，并按路内层号换向；不能把随机数加到角平分线位置上。
	Params.Jitter = FMath::Max(QuoinJitter, 0.0f);
	Params.SplitJitter = FMath::Max(QuoinSplitJitter, 0.0f);

	// 与房体面板、门框砖、藤蔓同一个变换口径（只取 yaw）。换口径就会在有 pitch/roll 的房子上
	// 与它们错开 —— 那正是「已知潜伏问题」里 `GetBuildTransform()` vs `ToInverseMatrixWithScale()`
	// 那条不对称的同族。
	TArray<CSHouseQuoin::FQuoin> Quoins;
	CurrentQuoinColumnCount = CSHouseQuoin::BuildQuoins(GetBuildTransform(), GetFootprint(), WallThickness,
		float(GetActorLocation().Z), WallHeight, QuoinInset, Quoins);
	if (Quoins.IsEmpty()) return 0;

	// 转角配成墩的角上，角石在墩顶以下让路：那一截由 `BuildCornerPierBricks` 的柱础/柱身/柱头
	// 顶替（2026-09-06 用户裁决：转角就是一个墩）。判据与高度都只有 `ResolvePierSpans` 写的那一份
	// `CornerPierTopZ`，这里不再自己找洞 —— 两处各判一次就会出现"墩砌到 A 高度、角石剔到 B 高度"。
	// `FQuoin::BottomZ` 已经是世界 Z，剔除线跟它同一个口径。
	{
		const float BaseZ = float(GetActorLocation().Z);
		// 按角号对，不按数组下标：不出角石的角（凹角 / 锐角）被跳过，下标与角号会错开。
		for (CSHouseQuoin::FQuoin& Q : Quoins)
		{
			if (CornerPierTopZ.IsValidIndex(Q.CornerIndex) && CornerPierTopZ[Q.CornerIndex] > UE_KINDA_SMALL_NUMBER)
			{
				Q.CullBelowZ = BaseZ + CornerPierTopZ[Q.CornerIndex];
			}
		}
	}

	// 随机数基取**房子身份**，不取槽位：槽位会被"这栋房多开一扇门"整体推走（门框砖排在前面），
	// 于是将来谁给砖材质接上 `PerInstanceRandom` 色差，开一扇门就会让四个角整体换色，
	// 而所有几何断言全绿。与接缝砖那条是同一个理由的另一半。
	const uint32 Seed = HouseId.IsValid() ? GetTypeHash(HouseId) : uint32(GetUniqueID());
	const FVector MeshSize = FrameBrickMesh->GetBoundingBox().GetSize();
	const FVector2f MeshInvSize(1.0 / FMath::Max(MeshSize.X, 1.0), 1.0 / FMath::Max(MeshSize.Z, 1.0));
	const int32 Added = CSHouseQuoin::BuildQuoinElements(Quoins, Seed, Params, InOutElements, MeshInvSize);
	CurrentQuoinBrickCount = Added;
	InOutBrickCount += Added;

	// 参数也进入哈希：只改随机幅度、层高或种子同样必须重新散布。
	// QuoinInset 已经包含在 Point 中。
	TArray<int32> H;
	H.Append({ Added, int32(Seed), CSHouse_Q(Params.Length, 0.001), CSHouse_Q(Params.Gap, 0.001),
		CSHouse_Q(Params.Jitter, 0.001), CSHouse_Q(Params.SplitJitter, 0.001) });
	for (const CSHouseQuoin::FQuoin& Q : Quoins)
	{
		H.Append({ CSHouse_Q(Q.Point.X, 1), CSHouse_Q(Q.Point.Y, 1),
			CSHouse_Q(Q.Outward.X, 0.01), CSHouse_Q(Q.Outward.Y, 0.01), CSHouse_Q(Q.HalfTurnCos, 0.001),
			CSHouse_Q(Q.BottomZ, 1), CSHouse_Q(Q.TopZ, 1),
			// 剔除高度也得进：它决定哪些砖被写成负值随机数 ⇒ 决定画面。漏掉它 =
			// 转角门开了/关了而角柱照旧，且**没有任何报错**（同 `StyleFlags` 那条）。
			CSHouse_Q(Q.CullBelowZ, 1) });
	}
	return CSHouse_Hash(H);
}

// -----------------------------------------------------------------------------
// 包边石（D7 第三样）—— 墙顶压顶 + 墙脚勒脚，两条带共用一套算法
// -----------------------------------------------------------------------------

/** 砖容量的硬上限（用户 2026-09-06 定）。5 个 float4 = 80 B/块 ⇒ 65536 块 ≈ 5.2 MB/房。 */
static constexpr int32 CSHouse_MaxFrameCapacity = 65536;

int32 ACSHouseActor::EffectiveFrameCapacity() const
{
	const int32 Authored = FMath::Max(FrameReserveCapacity, 64);
	if (!bBrickWallEnabled) return FMath::Min(Authored, CSHouse_MaxFrameCapacity);

	// 砖层的量级是 footprint 的函数（6 × 4 m、檐高 3 m 就要 1110 块），让用户手算等于把
	// "墙砌到一半"的责任推给他。所以砖层自己按上界加够，**authored 那份原样留给其余四家**
	// （门框 / 接缝 / 角石 / 包边）——两边互不挤占，谁超了都能从数字上看出来。
	return FMath::Clamp(Authored + GetBrickWallBrickBudget(), 64, CSHouse_MaxFrameCapacity);
}

int32 ACSHouseActor::GetBrickWallBrickBudget() const
{
	const CSHouseBrickWall::FCourses Courses =
		CSHouseBrickWall::PlanCourses(WallHeight, BrickWallCourseHeight);
	return CSHouseBrickWall::EstimateBricks(GetFootprint(), WallThickness, Courses,
		FMath::Max(FrameBrickLength, 1.0f));
}

uint32 ACSHouseActor::BuildBrickWallBricks(TArray<CSHouseFrame::FElement>& InOutElements, int32& InOutBrickCount)
{
	CurrentBrickWallBrickCount = 0;
	CurrentBrickWallCourseCount = 0;
	if (!bBrickWallEnabled || !bFrameEnabled || !FrameBrickMesh) return 0;

	const CSHouseBrickWall::FCourses Courses =
		CSHouseBrickWall::PlanCourses(WallHeight, BrickWallCourseHeight);
	if (Courses.Count <= 0) return 0;
	CurrentBrickWallCourseCount = Courses.Count;

	CSHouseFrame::FBrickParams Params;
	Params.Length = FMath::Max(FrameBrickLength, 1.0f);
	Params.Gap = FMath::Max(FrameBrickGap, 0.0f);
	// **与门框 / 接缝 / 角石 / 包边共用同一份常驻容量**：砖层只是同一个组件里排在最后的那些行。
	// 撞上限就停发，绝不扩容 —— 扩容是一次阻塞刷新，落在用户恰好画到的那一笔上。
	Params.MaxBricks = EffectiveFrameCapacity();

	const FTransform World = GetBuildTransform();
	const uint32 Seed = HouseId.IsValid() ? GetTypeHash(HouseId) : uint32(GetUniqueID());
	const float Clearance = FMath::Max(BrickWallOpeningClearance, 0.0f);

	TArray<int32> H;
	TArray<CSHouseTrim::FRun> Runs;

	// 哈希只记标量：层高 + 每层的砖数与各段的 (边号, S 区间)。逐砖位置是它们的纯函数。
	// ⚠️ 必须逐层记，不能只记总砖数：把一扇窗左右挪半米，总数可以一块不差而位置全变了。
	H.Append({ Courses.Count, CSHouse_Q(Courses.Height, 0.5) });
	const int32 Added = CSHouseBrickWall::BuildWall(World, GetFootprint(), WallThickness, Courses,
		Clearance, MakeArrayView(CurrentOpenings), Seed, Params, Runs, InOutElements,
		[&H](int32 CourseIndex, const CSHouseTrim::FBand& Band, int32 CourseBricks,
			const TArray<CSHouseTrim::FRun>& CourseRuns)
		{
			H.Append({ CourseIndex, CourseBricks, CSHouse_Q(Band.CenterZ, 1) });
			for (const CSHouseTrim::FRun& R : CourseRuns)
			{
				H.Append({ R.EdgeIndex, CSHouse_Q(R.S0, 1), CSHouse_Q(R.S1, 1) });
			}
		});

	CurrentBrickWallBrickCount = Added;
	InOutBrickCount += Added;
	if (Added <= 0) return 0;

	UE_LOG(LogTinyGladeHouse, Verbose,
		TEXT("[TinyGladeHouse] %s brick wall: courses=%d bricks=%d budget=%d capacity=%d"),
		*GetName(), Courses.Count, Added, GetBrickWallBrickBudget(), Params.MaxBricks);
	return CSHouse_Hash(H);
}

uint32 ACSHouseActor::BuildTrimBricks(TArray<CSHouseFrame::FElement>& InOutElements, int32& InOutBrickCount)
{
	CurrentTrimTopRunCount = 0;
	CurrentTrimBaseRunCount = 0;
	CurrentTrimBrickCount = 0;
	if (!bTrimEnabled || !bFrameEnabled || !FrameBrickMesh) return 0;
	if (WallHeight <= 0.0f) return 0;

	CSHouseFrame::FBrickParams Params;
	Params.Length = FMath::Max(FrameBrickLength, 1.0f);
	Params.Gap = FMath::Max(FrameBrickGap, 0.0f);
	// **与门框 / 接缝 / 角石共用同一份常驻容量**：包边只是同一个组件里排在最后的那些行。
	Params.MaxBricks = EffectiveFrameCapacity();

	// 课程半高 = 砖的进深轴（`AppendFlatRun` 里 AxisX 朝上）。它只参与"洞挡不挡得住"的判据。
	const float HalfHeight = FMath::Max(FrameBrickDepth, 1.0f) * 0.5f;
	const FTransform World = GetBuildTransform();
	const uint32 Seed = HouseId.IsValid() ? GetTypeHash(HouseId) : uint32(GetUniqueID());
	const float Clearance = FMath::Max(TrimOpeningClearance, 0.0f);

	TArray<int32> H;
	TArray<CSHouseTrim::FRun> Runs;

	auto RunBand = [&](bool bWanted, float CenterZ, uint32 Salt, int32& OutRunCount)
	{
		OutRunCount = 0;
		if (!bWanted) return;
		CSHouseTrim::FBand Band;
		Band.CenterZ = CenterZ;
		Band.HalfHeight = HalfHeight;
		const int32 Added = CSHouseTrim::BuildBand(World, GetFootprint(), WallThickness, Band, Clearance,
			MakeArrayView(CurrentOpenings), Seed, Salt, Params, Runs, InOutElements);
		OutRunCount = Runs.Num();
		CurrentTrimBrickCount += Added;
		InOutBrickCount += Added;
		// 哈希只记标量：带高 + 每段的边号与 S 区间 + 砖数。逐砖位置是它们的纯函数。
		H.Append({ Added, CSHouse_Q(CenterZ, 1) });
		for (const CSHouseTrim::FRun& R : Runs)
		{
			H.Append({ R.EdgeIndex, CSHouse_Q(R.S0, 1), CSHouse_Q(R.S1, 1) });
		}
	};

	// 顺序固定：先顶后底。换序会推走对方的槽位（两者的随机数都已从身份派生，所以只是纪律）。
	RunBand(bTrimTop, WallHeight + TrimTopOffset, CSHouseFrame::EPathFamily::TrimTop, CurrentTrimTopRunCount);
	RunBand(bTrimBase, TrimBaseOffset, CSHouseFrame::EPathFamily::TrimBase, CurrentTrimBaseRunCount);

	return CSHouse_Hash(H);
}

bool ACSHouseActor::IsSeamDrawable(FString& OutReason) const
{
	FlushPendingReevaluate();
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
	FlushPendingReevaluate();
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
	FlushPendingReevaluate();
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
	FlushPendingReevaluate();
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

	// ---- 洞缘那一半：不归房子，所以一道砖判据都不设 ----
	//
	// ⚠️ **窗周围没有任何砖**：2026-09-06 起窗不出框砖（洞缘归标记自带的 `OpeningMesh`，见
	// `ACSWindowMarker`），2026-09-10 起也不再把砖层算作窗的洞缘补全。别把门框砖 / 砖层的健康
	// 挂回这里 —— 演示关卡里门恰好在排砖，窗会**因为错误的理由通过**；关掉门 / 角石 / 包边之后，
	// 一栋只有窗的房子又会被误判成"不可画"（回归里"只剩窗"那一枪钉的就是它）。砖组件本身的
	// 健康也不归窗管：接缝判据查它的全链（有接缝时），`DebugGetGpuAssetMismatchSync` 查画的是不是那块砖。
	//
	// ⚠️ 属性面板 `Windows` 那一份没有标记、没有网格 ⇒ 洞缘是**裸的** —— 这是已知并接受的代价
	// （那条路在计划里一直写着是"授权 / 测试用的便利入口"），所以它**不构成**"不可画"。
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
	const FCSHouseFootprint Footprint = GetFootprint();
	for (int32 EdgeIndex = 0; EdgeIndex < Footprint.NumEdges(); ++EdgeIndex)
	{
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(EdgeIndex, Footprint, WallThickness);
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

		// 地面空隙采样：与承重柱**同一个量**（`Gap = 房底 Z − SampleHeight`，见 ComputePillars），
		// 只是采样点跟着墙走而不是跟着柱距走。没有地面时留空数组 ⇒ `SampleGroundGap` 返回 0
		// ⇒ 按贴地处理：没有地面的场景（纯单测、还没落座）不该因此秃掉。
		if (Ground && F.Len > UE_KINDA_SMALL_NUMBER)
		{
			const int32 SampleCount = FMath::Clamp(
				FMath::CeilToInt(F.Len / FMath::Max(VineGroundSampleSpacing, 10.0f)) + 1, 2, 64);
			Strip.GroundGaps.SetNumUninitialized(SampleCount);
			const double BaseZ = GetActorLocation().Z;
			for (int32 K = 0; K < SampleCount; ++K)
			{
				const FVector P = Strip.Origin + Strip.U * (F.Len * double(K) / double(SampleCount - 1));
				Strip.GroundGaps[K] = float(BaseZ - Ground->SampleHeight(FVector2D(P.X, P.Y)));
			}
		}

		OutStrips.Add(Strip);
	}
}

CSShaperSteps::EHandoverResult ACSHouseActor::EnsureVineComponents()
{
	// 新组件身上没有基础网格快照。三条按下标对齐，任一条是新的就整批重建。
	auto EnsureOne = [this](TObjectPtr<UCSGpuInstancedMeshComponent>& Component)
	{
		if (CSShaperSteps::EnsureInstancedComponent(this, Component)) bVineBaseMeshReady = false;
	};
	EnsureOne(VineBranchComponent);
	EnsureOne(VineLeafComponent);
	EnsureOne(VineFlowerComponent);
	VineBranchComponent->InstanceMaterial = VineBranchMaterial;

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

	// 枝 / 花的 MID：只为把 `VineGrowSpeed` 下推过去。父换了必须重建 —— 在细节面板里
	// 换掉材质资产时旧 MID 仍然有效，于是"换了材质但画面没变"（同 VineLeafSeasonMID 那条）。
	// 秒 → 弧长 cm。材质侧的前沿是按弧长推的，而面板上给的是秒（"延迟"问的就是时间）。
	// ⚠️ 这三行是**唯一**的换算点：别在材质里再折算一次，两处各算一份的症状是
	// "改速度时延迟莫名其妙地平方级变化"，而两边各自都自洽。
	const float Speed = FMath::Max(VineGrowSpeed, 1.0f);
	const float FadeCm = FMath::Max(VineGrowFadeSeconds, 0.01f) * Speed;
	const float LeafLagCm = FMath::Max(VineLeafGrowDelay, 0.0f) * Speed;
	const float FlowerLagCm = FMath::Max(VineFlowerGrowDelay, 0.0f) * Speed;

	auto EnsureGrowMID = [this, Speed, FadeCm](TObjectPtr<UMaterialInstanceDynamic>& MID, UMaterialInterface* Parent)
	{
		if (!Parent) { MID = nullptr; return; }
		if (!MID || MID->Parent != Parent) MID = UMaterialInstanceDynamic::Create(Parent, this);
		if (!MID) return;
		MID->SetScalarParameterValue(TEXT("VineGrowSpeed"), Speed);
		MID->SetScalarParameterValue(TEXT("VineGrowFade"), FadeCm);
	};
	EnsureGrowMID(VineBranchGrowMID, VineBranchMaterial);
	EnsureGrowMID(VineFlowerGrowMID, VineFlowerMaterial);
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

	VineFlowerComponent->InstanceMaterial = VineFlowerGrowMID
		? static_cast<UMaterialInterface*>(VineFlowerGrowMID) : ToRawPtr(VineFlowerMaterial);

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
		// 管子模式下枝不进实例路，它的基础网格快照建不出来也无所谓。
		bVineBaseMeshReady = (bVineUseTube || bBranchOk) && bLeafOk;
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

	if (!bVineBaseMeshReady) return CSShaperSteps::EHandoverResult::UpToDate;

	// 容量按**配置上限**一次付清，之后永不扩容（零阻塞纪律）。上限是纯配置量：
	// 四面墙的总周长 / 间距 × 每根最多几段。规划真的排超了就在 kernel 里截断 ——
	// 少画几段藤，远好过在拖动的某一帧上付一次设备同步。
	const FCSHouseFootprint Footprint = GetFootprint();
	const double Perimeter = Footprint.GetPerimeter();
	const int32 MaxStrands = FMath::CeilToInt(Perimeter / FMath::Max(VineStrandSpacing, 20.0f)) + Footprint.NumEdges();
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
	const double Reach = CSShaperSteps::QuantizeUp(Footprint.GetCenteredSpan() * 0.6
		+ FMath::Max(VineLeafSize, VineFlowerSize) + VineStandOff);
	// ⚠️ 上界取**屋脊高**而不是墙高：藤自己只爬到檐口，但瓦 / 尖顶那一层将来也吃这只盒子，
	// 停在墙高的话斜看时会成片被剔掉（症状是"转个视角就闪没"，最难复现的那一类）。
	// `CSShaperSteps::QuantizeUp` 只涨不缩，所以这一项不是每帧变的量。
	const double Top = CSShaperSteps::QuantizeUp(CSHouseRoof_RidgeZ(GetRoofDesc())
		+ FMath::Max(VineLeafSize, VineFlowerSize));
	FBox LocalBounds(FVector(-Reach, -Reach, -VineLeafSize), FVector(Reach, Reach, Top));
	// 三个调色板的组件是同一棵挂接树上的兄弟、变换相同，取叶那一个即可（藤没有叶就不会走到这里）。
	LocalBounds = CSHouse_BuildBoundsToComponent(GetBuildTransform(), VineLeafComponent.Get(), LocalBounds);
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

void ACSHouseActor::RebuildVine()
{
	// ⚠️ 管子模式下**不再要求** `VineBranchMesh`：枝已经不是实例，那个资产只服务旧路。
	// 忘了改这一条的症状是"把枝网格清空之后连叶子也没了"，而两条路各自都没错。
	if (!bVineEnabled || (!bVineUseTube && !VineBranchMesh) || !VineLeafMesh)
	{
		if (CurrentVineSegmentCount != 0 || CurrentVineLeafCount != 0 || CurrentVineFlowerCount != 0)
		{
			// ⚠️ **撤实例源之前必须先把 counter 清零**（与 `RebuildFrame` 那条同源，
			// 门框砖已经因此在画面上留过 12 层砖）：撤掉之后 `VineHandover` 被清空，
			// 下一次 `EnsureVineComponents` 会把同一批 buffer 交回组件。这条路上的窗口是
			// **重新打开藤之后 `bVineBaseMeshReady` 为假**那一次 —— 那时交接已经发生，
			// 而 `CSHouseVine::Pack`（它自己的空表分支会清零）根本走不到。
			CSShaperSteps::ZeroCounters(VineGpuBuffers);
			ClearMeshSlot(VineTubeComponent, VineTubeMesh, PendingVineTubePath);   // 下次有藤时重建，别让空网格留在组件上
			if (VineBranchComponent) VineBranchComponent->ClearInstanceSourceGPU();
			if (VineLeafComponent) VineLeafComponent->ClearInstanceSourceGPU();
			if (VineFlowerComponent) VineFlowerComponent->ClearInstanceSourceGPU();
			VineHandover.Reset();
			CurrentVineSegmentCount = 0;
			CurrentVineLeafCount = 0;
			CurrentVineFlowerCount = 0;
			VineDescHash = 0;
		}
		return;
	}

	const CSShaperSteps::EHandoverResult VineHandoverResult = EnsureVineComponents();
	if (!bVineBaseMeshReady) return;

	TArray<CSHouseVine::FWallStrip> Strips;
	BuildVineStrips(Strips);

	CSHouseVine::FParams Params;
	Params.StrandSpacing = VineStrandSpacing;
	Params.SegmentLength = VineSegmentLength;
	Params.MaxSegments = VineMaxSegments;
	Params.Wander = VineWander;
	Params.MaxLean = VineMaxLean;
	Params.MaxTurn = VineMaxTurn;
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
	Params.TipTaperLength = VineTipTaperLength;
	Params.TipTaperMin = VineTipTaperMin;
	Params.MaxGroundGap = VineMaxGroundGap;
	Params.Seed = VineSeed;

	CSHouseVine::FPlan Plan;
	CSHouseVine::BuildPlan(Strips, CurrentOpenings, Params, Plan);

	// 哈希用的空隙摘要：取四面墙采样里的**最大值**。最大值正好是"最悬空的那一点"，
	// 也就是判据真正会翻转的那个量；取平均会让一角翘起被别处摊平。
	double GroundGapSummary = 0.0;
	for (const CSHouseVine::FWallStrip& S : Strips)
	{
		for (float G : S.GroundGaps) GroundGapSummary = FMath::Max(GroundGapSummary, double(G));
	}

	// 幂等短路。哈希覆盖"会改变藤的形态的一切"：摆位 + 尺寸 + 洞集合 + 参数。
	// ⚠️ 短路点在 BuildPlan **之后**是有意的：规划是纯 CPU、微秒量级，而它同时是哈希的
	// 唯一诚实来源 —— 拿参数拼一个哈希而不跑规划，会在"参数没变但洞变了"时静默漏更新。
	const TArray<int32> HashInput = {
		int32(Plan.Branch.Num()), int32(Plan.Leaf.Num()), int32(Plan.Flower.Num()),
		CSHouse_Q(FootprintSize.X, 1), CSHouse_Q(FootprintSize.Y, 1), CSHouse_Q(WallHeight, 1),
		CSHouse_Q(VineStrandSpacing, 0.5), CSHouse_Q(VineSegmentLength, 0.5), VineMaxSegments,
		CSHouse_Q(VineWander, 0.01), CSHouse_Q(VineMaxLean, 0.01), CSHouse_Q(VineMaxTurn, 0.01),
		CSHouse_Q(VineBloat, 0.01),
		CSHouse_Q(VineThickness, 0.1), CSHouse_Q(VineStandOff, 0.1), CSHouse_Q(VineLeafSize, 0.1),
		CSHouse_Q(VineLeafChance, 0.01), CSHouse_Q(VineLeafSizeJitter, 0.01), VineSeed,
		CSHouse_Q(VineFlowerChance, 0.01), CSHouse_Q(VineFlowerFromFrac, 0.01), CSHouse_Q(VineFlowerSize, 0.1),
		CSHouse_Q(VineJumpChance, 0.01), CSHouse_Q(VineMaxGroundGap, 1),
		CSHouse_Q(VineTipTaperLength, 1), CSHouse_Q(VineTipTaperMin, 0.01),
		// 速度进哈希：它参与 `ResolveVineSpawnTimes` 的"前沿到哪儿了"判断，改它等于换一套相位。
		CSHouse_Q(VineGrowSpeed, 1),
		// ⚠️ 空隙本身必须进哈希：它由**地面**决定，而地面变化只发一个"重求值"的广播，
		// 房子的摆位与参数一个都没变。不进哈希的症状是"把地面拉低、柱子冒出来了、藤还在"。
		CSHouse_Q(GroundGapSummary, 1),
		// 四坡以后四面墙顶一律平在 WallHeight（已在上面进哈希），屋面参数不再决定藤怎么排。
		int32(ComputePlacementHash() & 0x7FFFFFFF) };
	// 顶点表另拼：藤条沿每条边排，形状变了而包围盒尺寸没变时（异形房子改 `FootprintShape`）也必须重排。
	TArray<int32> FootprintHash;
	CSHouse_AppendFootprintHash(FootprintHash, GetFootprint());
	const uint32 NewHash = HashCombine(CSHouse_Hash(HashInput), CSHouse_Hash(FootprintHash));

	bool bBuffersReady = VineGpuBuffers.Num() == CSHouseVine::Palette_Num;
	for (int32 Index = 0; bBuffersReady && Index < CSHouseVine::Palette_Num; ++Index)
	{
		bBuffersReady = VineGpuBuffers[Index].IsValid();
	}
	// 刚交接过（扩容换了清零的新 buffer / 包围盒变了）就必须重打包，哈希没变也不能早退（审查 B1）。
	const bool bMustRepack = VineHandoverResult == CSShaperSteps::EHandoverResult::HandedOver;
	if (!bMustRepack && NewHash == VineDescHash && VineHandover.Capacities.Num() == CSHouseVine::Palette_Num && bBuffersReady) return;

	VineDescHash = NewHash;
	CurrentVineSegmentCount = Plan.Branch.Num();
	CurrentVineLeafCount = Plan.Leaf.Num();
	CurrentVineFlowerCount = Plan.Flower.Num();

	// 世界 → 组件。⚠️ **用组件自己的变换求逆**，不用 actor 的：已删的门框旧路混用
	// `GetBuildTransform()`（只取 yaw）与 actor 的完整逆变换，正是状态文件「已知潜伏问题」
	// 里那条 —— 房子一旦被 pitch/roll 或缩放就错位。这里不重复它。
	const FMatrix44f WorldToComponent = FMatrix44f(
		VineBranchComponent->GetComponentTransform().ToInverseMatrixWithScale());
	// ⚠️ **管子模式下枝不能再走实例**，否则管子与分段实例同时画（用户实测："连续的管子和
	// 分段似乎同时存在"）。摘掉记录而不是"不画"：`Pack` 的空表分支会 `AddClearUAVPass`
	// 把 counter 清零，而单纯跳过打包会让 counter **停在上一次的值** —— 那正是重影的成因。
	// 计数（CurrentVineSegmentCount）在上面已经取过，日志与哈希都不受影响；
	// `PackTubePath` 只读 `Plan.Strands`，也不受影响。
	// SpawnTime：老藤沿用、新藤记当前时刻、消失的藤从表里删掉。
	// ⚠️ 必须**先删后加**地重建，不能只往里塞：房子反复改尺寸会让键不断变化，
	// 只加不删的话这张表会随编辑次数无界增长（而且泄漏得毫无症状）。
	// ⚠️ 位置在 `Pack` **之前**、且在管子分支**之外** —— 枝（管子）与叶花（实例）
	// 两条路共用同一份相位，同一根藤的枝与叶必须同时长出来。放进管子分支里的后果是
	// 叶子的 SpawnTime 恒为 0（一出现就长成），而管子长得好好的。
	TArray<float> SpawnTimes;
	ResolveVineSpawnTimes(Plan, SpawnTimes);

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

	if (bVineUseTube) Plan.Branch.Reset();
	CSHouseVine::Pack(Plan, VineGpuBuffers, WorldToComponent);

	// 枝：折线 → 管子。叶与花仍走上面那两个调色板（2026-09-06 裁决 5）。
	if (bVineUseTube)
	{
		TSharedPtr<CSHouseVine::FTubePath, ESPMode::ThreadSafe> Path =
			MakeShared<CSHouseVine::FTubePath, ESPMode::ThreadSafe>();
		CSHouseVine::PackTubePath(Strips, Plan, Params, VineTubeSubdivide,
			CSHouseVine_TubeCircleScale, SpawnTimes, *Path);
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

	UE_LOG(LogTinyGladeHouse, Log, TEXT("[TinyGladeHouse] %s vine packed: strips=%d strands=%d branches=%d leaves=%d flowers=%d"),
		*GetName(), Strips.Num(), Plan.Strands.Num(), CurrentVineSegmentCount, CurrentVineLeafCount, CurrentVineFlowerCount);
}

bool ACSHouseActor::IsVineDrawable(FString& OutReason) const
{
	FlushPendingReevaluate();
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

bool ACSHouseActor::IsVineSuppressedByGroundGap() const
{
	// 与 `CSHouseVine::BuildPlan` 里那条判据**同一个量、同一个阈值**：藤脚处的地面空隙。
	// 这里取四面墙采样的最大值 —— 只要还有一处贴地，藤就该长得出来，那时零藤才是真缺陷。
	TArray<CSHouseVine::FWallStrip> Strips;
	BuildVineStrips(Strips);
	if (Strips.IsEmpty()) return false;

	float MinGap = TNumericLimits<float>::Max();
	for (const CSHouseVine::FWallStrip& S : Strips)
	{
		// 空采样 = 不知道 = 按贴地处理（同 `FWallStrip::SampleGroundGap` 的口径）。
		if (S.GroundGaps.IsEmpty()) return false;
		for (float G : S.GroundGaps) MinGap = FMath::Min(MinGap, G);
	}
	return MinGap > VineMaxGroundGap;
}

FString ACSHouseActor::GetVineUndrawableReason() const
{
	FlushPendingReevaluate();
	FString OutReason;
	// 逐环检查渲染那一侧 —— readback 断言对这些一个字都说不了（见头文件里那段教训）。
	if (!bVineEnabled) { OutReason = TEXT("bVineEnabled 关着"); return OutReason; }

	// **悬空是合法的"没有藤"，不是"藤画不出来"**（用户裁决 2026-09-06）。
	// 这一环必须排在所有资产/渲染判据**之前**：悬空的房子既没有实例也没有管子，
	// 后面每一条都会挨个报红，而它们说的都是同一件不成立的事。
	//
	// ⚠️ 用**规划的结果**（一根藤都没排出来）配合空隙判据，而不是只看空隙 ——
	// 半边悬空的房子仍然该长藤，那时空隙超阈但藤是有的，不能一并放过。
	if (CurrentVineSegmentCount <= 0 && IsVineSuppressedByGroundGap())
	{
		return OutReason;   // 空串 = 没问题
	}

	if (!bVineUseTube && !VineBranchMesh) { OutReason = TEXT("没有 VineBranchMesh"); return OutReason; }
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

// -----------------------------------------------------------------------------
// 屋面瓦（四坡的屋面本体）
// -----------------------------------------------------------------------------

CSHouseTile::FParams ACSHouseActor::MakeRoofTileParams() const
{
	CSHouseTile::FParams Params;
	Params.RowPitch = RoofTileRowPitch;
	Params.ColumnPitch = RoofTileColumnPitch;
	Params.RowOverlap = RoofTileRowOverlap;
	Params.ColumnOverlap = RoofTileColumnOverlap;
	Params.Thickness = RoofTileThickness;
	Params.SizeScale = RoofTileSizeScale;
	Params.RidgeCapScale = RoofRidgeCapScale;
	Params.StandOff = RoofTileStandOff;
	Params.ScaleJitter = RoofTileScaleJitter;
	Params.YawJitter = RoofTileYawJitter;
	Params.LiftJitter = RoofTileLiftJitter;
	Params.Seed = RoofTileSeed;
	Params.Axes = RoofTileAxes;
	return Params;
}

CSShaperSteps::EHandoverResult ACSHouseActor::EnsureRoofTileComponent()
{
	// 新组件身上没有基础网格快照，得重建。
	if (CSShaperSteps::EnsureInstancedComponent(this, RoofTileComponent)) bRoofTileBaseMeshReady = false;
	RoofTileComponent->InstanceMaterial = RoofMaterial;

	if (RoofTileGpuBuffers.Num() != 1)
	{
		CSShaperSteps::ReleaseOnRenderThread(RoofTileGpuBuffers);
		RoofTileGpuBuffers.SetNum(1);
		RoofTileHandover.Capacities.Reset();
		bRoofTileBaseMeshReady = false;
	}

	// 换过网格资产就必须重建快照（同 `VineBranchMeshBuiltFrom` 的字段注释：只靠一个 bool
	// 会出现"换了资产但什么都没发生"）。
	if (RoofTileMeshBuiltFrom != RoofTileMesh) bRoofTileBaseMeshReady = false;

	if (!bRoofTileBaseMeshReady)
	{
		FCSGpuMeshCPUData Data;
		// **复用藤蔓那份读取器**：它判"法线/UV 读出来合不合法"（有流不等于有数据）、缺了就现补、
		// 并把顶点色搬进快照。长度轴传 2 = **不换轴** —— 瓦的朝向由记录自带的整组基决定，
		// 网格一个顶点都不用动（见 `CSHouseTile::FMeshAxes` 的注释）。
		const bool bOk = CSHouseVine::BuildBaseMesh(RoofTileMesh, 2, Data);
		if (bOk)
		{
			RoofTileComponent->SetBaseMeshFromGpuData(Data);

			FBox3f Local(ForceInit);
			for (const FVector3f& P : Data.Positions) Local += P;
			const FVector3f Size = Local.IsValid ? Local.GetSize() : FVector3f(1.0f, 1.0f, 1.0f);

			CSShaperSteps::FPaletteBuffers& Buffers = RoofTileGpuBuffers[0];
			Buffers.BaseSphereCentre = Local.IsValid ? Local.GetCenter() : FVector3f::ZeroVector;
			Buffers.BaseSphereRadius = Local.IsValid ? Local.GetExtent().Size() : 0.0f;
			// 记录里的尺寸是**世界 cm**，所以块尺寸取 1/网格自身尺寸（同藤蔓那条手法）——
			// 逐排改瓦的大小就不必回头动 BlockSize。
			Buffers.BlockSize = FVector3f(
				1.0f / FMath::Max(Size.X, 0.01f),
				1.0f / FMath::Max(Size.Y, 0.01f),
				1.0f / FMath::Max(Size.Z, 0.01f));

			// 法线轴从包围盒判：瓦是一块**扁片** ⇒ 最薄的一轴就是屋面法线。
			const float Ext[3] = { Size.X, Size.Y, Size.Z };
			int32 NormalAxis = 0;
			for (int32 Axis = 1; Axis < 3; ++Axis)
			{
				if (Ext[Axis] < Ext[NormalAxis]) NormalAxis = Axis;
			}
			RoofTileAxes.Normal = NormalAxis;
			RoofTileAxes.NormalSign = 1.0f;
			RoofTileAxes.NativeSize = Size;
			// 枢轴不在中心的瓦资产靠这一项对齐（见 `FMeshAxes::NativeCentre`）。
			RoofTileAxes.NativeCentre = Buffers.BaseSphereCentre;
			UE_LOG(LogTinyGladeHouse, Log,
				TEXT("[TinyGladeHouse] %s roof tile mesh: size=(%.1f, %.1f, %.1f) normal axis=%d"),
				*GetName(), Size.X, Size.Y, Size.Z, NormalAxis);
		}
		RoofTileMeshBuiltFrom = bOk ? RoofTileMesh : nullptr;
		bRoofTileBaseMeshReady = bOk;
	}
	if (!bRoofTileBaseMeshReady) return CSShaperSteps::EHandoverResult::UpToDate;

	// 面内那两轴**每次都重算**，不能留在上面那个只跑一次的快照块里：`bRoofTileSwapAxes` 是
	// 交互旋钮，判定留在快照里的话勾了它什么都不会发生（"换了资产但什么都没发生"的同族失效）。
	// 哪根朝坡上从形状分不出来（两者都在瓦面内），默认把**长的**给上坡向。
	{
		const float Ext[3] = { RoofTileAxes.NativeSize.X, RoofTileAxes.NativeSize.Y, RoofTileAxes.NativeSize.Z };
		const int32 A = (RoofTileAxes.Normal + 1) % 3;
		const int32 B = (RoofTileAxes.Normal + 2) % 3;
		const bool bTakeA = (Ext[A] >= Ext[B]) != bRoofTileSwapAxes;
		RoofTileAxes.UpSlope = bTakeA ? A : B;
		RoofTileAxes.AlongRow = bTakeA ? B : A;
	}

	// 容量按**配置上限**一次付清，之后永不扩容（零阻塞纪律）。
	// ⚠️ **必须再走一次 `CSShaperSteps::ReserveCount` 的台阶**，不能把上限直接喂给 ReserveCapacity：
	// 上限是 FootprintSize 的**连续函数**（边长 / 间距），而 ReserveCapacity 只对齐到 64 ——
	// 拖尺寸时每涨过一列就重新分配一次。藤蔓那轮正是漏了这一步，实测一段拖动 21 次阻塞刷新。
	const CSHouseTile::FParams Params = MakeRoofTileParams();
	const FCSRoofDesc Roof = GetRoofDesc();
	const int32 Bound = CSHouseTile::MaxTilesBound(Roof, Params);
	const uint32 MaxRecords = uint32(FMath::Clamp(CSShaperSteps::ReserveCount(Bound), 64, 1 << 16));
	CSShaperSteps::ReserveCapacity(RoofTileGpuBuffers, MaxRecords);

	// 交接包围盒：量化 + 只涨不缩，理由与门框砖 / 藤 / 摆件那三段逐字相同（1 cm 阈值会让
	// 拖尺寸时每帧都重走一次阻塞的 SetInstanceSourceGPU）。
	// MeshReach 是"一块瓦最多伸多远"的**保守**估计：拿目标间距 × 重叠 × 抖动上界再乘 2，
	// 反正它只用来给剔除盒留边，宁可大一点也不要在视锥边缘闪掉整片屋面。
	const double TileReach = 2.0 * double(FMath::Max(
		FMath::Max(RoofTileRowPitch, RoofTileColumnPitch),
		FMath::Max(RoofTileAxes.NativeAlongSlope(), RoofTileAxes.NativeAcrossRow())))
		* double(FMath::Max(RoofTileRowOverlap, RoofTileColumnOverlap)) * (1.0 + double(RoofTileScaleJitter));
	const double Reach = CSShaperSteps::QuantizeUp(
		GetFootprint().GetCenteredSpan() * 0.5 + double(RoofOverhang) + TileReach);
	const double Top = CSShaperSteps::QuantizeUp(
		double(CSHouseRoof_RidgeZ(Roof)) + double(RoofTileStandOff) + double(RoofTileLiftJitter) + TileReach);
	FBox LocalBounds(FVector(-Reach, -Reach, -TileReach), FVector(Reach, Reach, Top));
	LocalBounds = CSHouse_BuildBoundsToComponent(GetBuildTransform(), RoofTileComponent, LocalBounds);
	LocalBounds = CSShaperSteps::MergeHandoverBounds(LocalBounds, RoofTileHandover, bForceFullRebuild);
	return CSShaperSteps::HandOverInstanceSource(
		CSShaperSteps::MakeHandoverSource(RoofTileComponent, RoofTileGpuBuffers[0], /*bWithCustomData*/ false),
		LocalBounds, RoofTileHandover);
}

void ACSHouseActor::RebuildRoofTiles()
{
	if (!bRoofTilesEnabled || !RoofTileMesh)
	{
		if (CurrentRoofTileCount != 0)
		{
			// ⚠️ 同 `RebuildVine` / `RebuildDecor`：撤实例源之前先清 counter，
			// 否则下一次 `EnsureRoofTileComponent` 把同一批带陈旧计数器的 buffer 交回组件。
			CSShaperSteps::ZeroCounters(RoofTileGpuBuffers);
			if (IsValid(RoofTileComponent)) RoofTileComponent->ClearInstanceSourceGPU();
			RoofTileHandover.Reset();
			CurrentRoofTileCount = 0;
			RoofTileDescHash = 0;
		}
		return;
	}

	const CSShaperSteps::EHandoverResult RoofTileHandoverResult = EnsureRoofTileComponent();
	if (!bRoofTileBaseMeshReady || RoofTileGpuBuffers.Num() != 1) return;

	const FCSRoofDesc Roof = GetRoofDesc();
	const CSHouseTile::FParams Params = MakeRoofTileParams();

	// 幂等短路：瓦是 (屋面 desc, 参数, 摆位) 的**纯函数** —— 不读门、不读地面、不读道路，
	// 所以拿输入拼哈希是诚实的（摆件那条不行，它的锚点吃地面采样，只能拿产物拼）。
	TArray<int32> HashInput = {
		int32(ComputePlacementHash() & 0x7FFFFFFF),
		CSHouse_Q(WallHeight, 1),
		CSHouse_Q(RoofPitch, 0.1), CSHouse_Q(RoofOverhang, 1), CSHouse_Q(RoofHeightOffset, 0.1),
		CSHouse_Q(RoofTileRowPitch, 0.1), CSHouse_Q(RoofTileColumnPitch, 0.1),
		CSHouse_Q(RoofTileRowOverlap, 0.01), CSHouse_Q(RoofTileColumnOverlap, 0.01),
		CSHouse_Q(RoofTileThickness, 0.1), CSHouse_Q(RoofTileSizeScale, 0.01),
		CSHouse_Q(RoofRidgeCapScale, 0.01),
		CSHouse_Q(RoofTileStandOff, 0.1), CSHouse_Q(RoofTileScaleJitter, 0.01),
		CSHouse_Q(RoofTileYawJitter, 0.01), CSHouse_Q(RoofTileLiftJitter, 0.1), RoofTileSeed,
		// 网格换了（尺寸/轴向跟着变）也要重排 —— 只认指针的话换成同尺寸的另一张瓦不会重排。
		RoofTileAxes.UpSlope, RoofTileAxes.AlongRow, RoofTileAxes.Normal,
		CSHouse_Q(RoofTileAxes.NativeSize.X, 0.1), CSHouse_Q(RoofTileAxes.NativeSize.Y, 0.1),
		CSHouse_Q(RoofTileAxes.NativeSize.Z, 0.1),
		CSHouse_Q(RoofTileAxes.NativeCentre.X, 0.1), CSHouse_Q(RoofTileAxes.NativeCentre.Y, 0.1),
		CSHouse_Q(RoofTileAxes.NativeCentre.Z, 0.1) };
	CSHouse_AppendFootprintHash(HashInput, GetFootprint());
	const uint32 NewHash = CSHouse_Hash(HashInput);
	// 交接结果也进早退门（审查 B1）：容量比对挡的是"扩容后没重排"，交接结果还多挡一种 ——
	// 包围盒变了、同一批 buffer 重新交出去的那一趟，重排一次是便宜的保险。
	if (RoofTileHandoverResult != CSShaperSteps::EHandoverResult::HandedOver
		&& NewHash == RoofTileDescHash && RoofTileHandover.Capacities.Num() == 1
		&& RoofTileHandover.Capacities[0] == RoofTileGpuBuffers[0].Capacity
		&& RoofTileGpuBuffers[0].IsValid())
	{
		return;
	}
	RoofTileDescHash = NewHash;

	TArray<CSHouseTile::FRecord> Tiles;
	CSHouseTile::BuildPlan(Roof, GetBuildTransform(), Params, Tiles);
	CurrentRoofTileCount = Tiles.Num();

	// 世界 → 组件。⚠️ **用组件自己的变换求逆**，不用 actor 的（同摆件那段：已删的门框旧路
	// 混用 `GetBuildTransform()` 与 actor 的完整逆变换，房子一旦被 pitch/roll 或缩放就错位）。
	const FMatrix44f WorldToComponent = FMatrix44f(
		RoofTileComponent->GetComponentTransform().ToInverseMatrixWithScale());
	CSHouseTile::Pack(Tiles, RoofTileGpuBuffers[0], WorldToComponent);

	UE_LOG(LogTinyGladeHouse, Log, TEXT("[TinyGladeHouse] %s roof tiles packed: tiles=%d capacity=%u"),
		*GetName(), CurrentRoofTileCount, RoofTileGpuBuffers[0].Capacity);
}

FString ACSHouseActor::GetRoofTileUndrawableReason() const
{
	FlushPendingReevaluate();
	// 逐环检查渲染那一侧 —— readback 断言对这些一个字都说不了（见头文件里那段教训）。
	if (!bRoofTilesEnabled) return TEXT("bRoofTilesEnabled 关着");
	if (!RoofTileMesh) return TEXT("没有 RoofTileMesh");
	if (!bRoofTileBaseMeshReady) return TEXT("基础网格快照没建起来（读不到 LOD0 顶点？）");
	if (CurrentRoofTileCount <= 0) return TEXT("一块瓦都没排出来");
	if (!IsValid(RoofTileComponent)) return TEXT("没有渲染组件");
	if (!RoofTileComponent->IsRegistered()) return TEXT("渲染组件没注册");
	if (!RoofTileComponent->IsVisible()) return TEXT("渲染组件不可见");
	if (!RoofTileComponent->HasInstanceSourceGPU()) return TEXT("实例源没交接");
	if (RoofTileComponent->GetBaseMeshSnapshot().Positions.Num() < 3) return TEXT("基础网格快照是空的");
	if (!RoofTileComponent->GetGpuMesh()) return TEXT("GPU 网格没分配");

	// **这一条就是石阶那个坑**：材质为空时组件仍然会画，只是退回引擎默认表面材质 ——
	// 画面上是一片灰，而所有 readback 断言照绿。
	const UMaterialInterface* Material = RoofTileComponent->InstanceMaterial;
	if (!Material) return TEXT("没有绑材质（会用引擎默认表面材质画成一片灰）");
	// ⚠️ 多一环：没勾 `bUsedWithInstancedStaticMeshes` 的材质在实例路径上会被引擎**静默替换**成
	// 默认材质，症状与"没绑材质"逐像素相同。现成的 `MI_roof_*` 全挂在 `M_TG_Texture` 下，
	// 而它恰恰没勾 —— 这是本模块最容易中的一枪。
	const UMaterial* Base = Material->GetMaterial();
	if (!Base || !Base->bUsedWithInstancedStaticMeshes)
	{
		return FString::Printf(
			TEXT("材质 '%s' 的母材质没有勾 bUsedWithInstancedStaticMeshes（引擎会静默换成默认材质）"),
			*Material->GetName());
	}
	return FString();
}

void ACSHouseActor::RebuildRoofFinials()
{
	// 收工：网格撤了就把组件全销毁。**不是隐藏** —— 留着的话换资产时会先画一帧旧网格。
	auto Discard = [this](int32 KeepCount)
	{
		for (int32 Index = RoofFinialComponents.Num() - 1; Index >= KeepCount; --Index)
		{
			if (IsValid(RoofFinialComponents[Index])) RoofFinialComponents[Index]->DestroyComponent();
			RoofFinialComponents.RemoveAt(Index);
		}
	};

	if (!RoofFinialMesh)
	{
		if (CurrentRoofFinialCount != 0 || RoofFinialComponents.Num() != 0)
		{
			Discard(0);
			CurrentRoofFinialCount = 0;
			RoofFinialDescHash = 0;
		}
		return;
	}

	const FCSRoofDesc Roof = GetRoofDesc();
	// 尖顶立在最高那段脊的两端（骨架的 TopA / TopB）。脊长收到 0（正方形、正多边形）时两端重合
	// ⇒ 只立一根，金字塔尖不需要特例。阈值取 1 cm：比这更短的"脊"两根尖顶会互相穿模，而肉眼分不出一根两根。
	FCSRoofSkeleton Skeleton;
	CSHouseRoof_BuildSkeleton(Roof.Footprint, double(Roof.Overhang), Skeleton);
	const int32 WantCount = FVector2D::Distance(Skeleton.TopA, Skeleton.TopB) > 1.0 ? 2 : 1;

	// 竖直轴从包围盒判：尖顶是**一根细长的东西** ⇒ 最长的一轴就是它朝上那根。
	// （TG 的 `roof_spire` 是 y-up 导出的，不判轴直接进 UE 会躺倒。）
	const FBox Local = RoofFinialMesh->GetBoundingBox();
	const FVector Size = Local.GetSize();
	const double Ext[3] = { Size.X, Size.Y, Size.Z };
	int32 UpAxis = 0;
	for (int32 Axis = 1; Axis < 3; ++Axis)
	{
		if (Ext[Axis] > Ext[UpAxis]) UpAxis = Axis;
	}

	const FTransform Build = GetBuildTransform();
	TArray<int32> HashInput = {
		int32(ComputePlacementHash() & 0x7FFFFFFF),
		CSHouse_Q(WallHeight, 1),
		CSHouse_Q(RoofPitch, 0.1), CSHouse_Q(RoofHeightOffset, 0.1),
		CSHouse_Q(RoofFinialScale, 0.001), CSHouse_Q(RoofFinialSink, 0.1),
		// 资产换了必须重建（同 `RoofTileMeshBuiltFrom` 的教训：只认一个 bool 会"换了资产什么都没发生"）。
		int32(GetTypeHash(RoofFinialMesh) & 0x7FFFFFFF),
		int32(GetTypeHash(RoofFinialMaterial) & 0x7FFFFFFF),
		UpAxis, WantCount };
	CSHouse_AppendFootprintHash(HashInput, GetFootprint());
	const uint32 NewHash = CSHouse_Hash(HashInput);
	// ⚠️ 组件数也要进短路条件：蓝图重跑构造脚本会把 transient 组件销毁，而形态哈希一个字没变 ——
	// 只比哈希的话尖顶会**在重跑构造脚本后永久消失**（同 `EnsureRoofTileComponent` 开头那条）。
	if (NewHash == RoofFinialDescHash && RoofFinialComponents.Num() == WantCount)
	{
		bool bAllValid = true;
		for (const TObjectPtr<UStaticMeshComponent>& Component : RoofFinialComponents)
		{
			if (!IsValid(Component)) { bAllValid = false; break; }
		}
		if (bAllValid) return;
	}
	RoofFinialDescHash = NewHash;

	Discard(WantCount);

	// 资产的"上"送到局部 +Z。尖顶绕自身轴对称 ⇒ 剩下那两根随便配一组右手基即可，
	// 不必去求"它原本朝哪"。FMatrix(X, Y, Z, O) 把 e0/e1/e2 分别送到三个轴参数上，
	// 所以把 +Z 摆在 `UpAxis` 那一格，就得到 M·(资产的上) = 局部 +Z。
	const int32 SideAxis = (UpAxis + 1) % 3;
	const int32 ThirdAxis = (UpAxis + 2) % 3;
	FVector Basis[3];
	Basis[UpAxis] = FVector::UpVector;
	Basis[SideAxis] = FVector::ForwardVector;
	Basis[ThirdAxis] = FVector::CrossProduct(Basis[UpAxis], Basis[SideAxis]);
	const FQuat Stand = FMatrix(Basis[0], Basis[1], Basis[2], FVector::ZeroVector).ToQuat();

	const float RidgeZ = CSHouseRoof_RidgeZ(Roof);
	for (int32 Index = 0; Index < WantCount; ++Index)
	{
		if (!RoofFinialComponents.IsValidIndex(Index))
		{
			UStaticMeshComponent* Created = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
			Created->SetupAttachment(RootComponent);
			Created->SetCollisionEnabled(ECollisionEnabled::NoCollision);   // 纯装饰，别挡住玩家
			Created->RegisterComponent();
			RoofFinialComponents.Add(Created);
		}
		UStaticMeshComponent* Component = RoofFinialComponents[Index];
		if (!IsValid(Component)) continue;

		Component->SetStaticMesh(RoofFinialMesh);
		// 留空 = 用网格自带的材质槽（`SetMaterial(nullptr)` 会清成空槽画成灰，不能这么写）。
		if (RoofFinialMaterial) Component->SetMaterial(0, RoofFinialMaterial);

		const FVector2D XY = (WantCount == 2) ? ((Index == 0) ? Skeleton.TopA : Skeleton.TopB)
			: (Skeleton.TopA + Skeleton.TopB) * 0.5;
		const FVector LocalPos(XY.X, XY.Y, double(RidgeZ) - double(RoofFinialSink));
		Component->SetWorldTransform(FTransform(
			Build.GetRotation() * Stand,
			Build.TransformPosition(LocalPos),
			FVector(double(FMath::Max(RoofFinialScale, 0.01f)))));
	}

	CurrentRoofFinialCount = WantCount;
	UE_LOG(LogTinyGladeHouse, Log,
		TEXT("[TinyGladeHouse] %s roof finials: count=%d up axis=%d native=(%.1f, %.1f, %.1f) scale=%.2f"),
		*GetName(), CurrentRoofFinialCount, UpAxis, Size.X, Size.Y, Size.Z, RoofFinialScale);
}

void ACSHouseActor::RebuildDoorLeaves()
{
	// 与尖顶同一档设施：一洞一个普通 `UStaticMeshComponent` + 形态哈希短路。
	// **不走实例化路径** —— 一栋房子的门是个位数，实例化那套（母材质要勾
	// `bUsedWithInstancedStaticMeshes`，勾漏了引擎静默换默认材质画成一片灰）不值得。
	auto Discard = [this](int32 KeepCount)
	{
		for (int32 Index = DoorLeafComponents.Num() - 1; Index >= KeepCount; --Index)
		{
			if (IsValid(DoorLeafComponents[Index])) DoorLeafComponents[Index]->DestroyComponent();
			DoorLeafComponents.RemoveAt(Index);
		}
	};

	// 档位表：按每张网格**自己的**包围盒宽度升序。宽度轴与竖直轴逐张自判（见下面 PickRank）。
	struct FLeafRank
	{
		UStaticMesh* Mesh = nullptr;
		FBox Local = FBox(ForceInit);
		int32 UpAxis = 2;
		int32 WidthAxis = 0;
		int32 DepthAxis = 1;
		double NativeWidth = 0.0;
	};
	TArray<FLeafRank> Ranks;
	if (bDoorLeafEnabled)
	{
		for (const TObjectPtr<UStaticMesh>& Mesh : DoorLeafMeshes)
		{
			if (!Mesh) continue;
			FLeafRank Rank;
			Rank.Mesh = Mesh;
			Rank.Local = Mesh->GetBoundingBox();
			const FVector Size = Rank.Local.GetSize();
			const double Ext[3] = { Size.X, Size.Y, Size.Z };
			// 竖直轴 = 最长的一轴；宽度轴 = 剩下两轴里较长的那根（同 `RebuildRoofFinials` 的口径，
			// 换一张 y-up 直进的资产也立得起来）。
			Rank.UpAxis = 0;
			for (int32 Axis = 1; Axis < 3; ++Axis)
			{
				if (Ext[Axis] > Ext[Rank.UpAxis]) Rank.UpAxis = Axis;
			}
			Rank.WidthAxis = (Rank.UpAxis + 1) % 3;
			const int32 Other = (Rank.UpAxis + 2) % 3;
			if (Ext[Other] > Ext[Rank.WidthAxis]) Rank.WidthAxis = Other;
			Rank.DepthAxis = 3 - Rank.UpAxis - Rank.WidthAxis;
			Rank.NativeWidth = Ext[Rank.WidthAxis];
			if (Rank.NativeWidth > UE_KINDA_SMALL_NUMBER) Ranks.Add(Rank);
		}
		Ranks.Sort([](const FLeafRank& A, const FLeafRank& B) { return A.NativeWidth < B.NativeWidth; });
	}

	// 门扇只长在**门**上，窗和第三方注入的洞不算。
	TArray<const FCSWallOpening*> Doors;
	if (!Ranks.IsEmpty())
	{
		for (const FCSWallOpening& O : CurrentOpenings)
		{
			// 墩侧的拱（含转角一对）与拱廊子拱都不装门扇（见 `CSHouse_StyleNoLeafMask` 的注释与实拍）。
			if (O.Type != ECSOpeningType::Door || !O.IsValid()) continue;
			if ((O.StyleFlags & CSHouse_StyleNoLeafMask) != 0) continue;
			Doors.Add(&O);
		}
	}
	const int32 WantCount = Doors.Num();

	// 挑档：native 宽度不超过门扇宽的**最大**一档；一档都不够窄就用最小那档（再靠缩放收进去）。
	auto PickRank = [&Ranks](float LeafWidth) -> const FLeafRank&
	{
		int32 Best = 0;
		for (int32 Index = 0; Index < Ranks.Num(); ++Index)
		{
			if (Ranks[Index].NativeWidth <= double(LeafWidth)) Best = Index;
		}
		return Ranks[Best];
	};

	if (WantCount == 0 || Ranks.IsEmpty())
	{
		if (CurrentDoorLeafCount != 0 || DoorLeafComponents.Num() != 0)
		{
			Discard(0);
			CurrentDoorLeafCount = 0;
			DoorLeafDescHash = 0;
		}
		return;
	}

	const FCSHouseFootprint Footprint = GetFootprint();
	TArray<int32> HashInput = {
		int32(ComputePlacementHash() & 0x7FFFFFFF),
		CSHouse_Q(WallThickness, 0.5),
		CSHouse_Q(DoorLeafWidthRatio, 0.001), CSHouse_Q(DoorLeafRise, 0.001),
		CSHouse_Q(DoorLeafMaxStretch, 0.001), CSHouse_Q(DoorLeafInset, 0.1),
		int32(GetTypeHash(DoorLeafMaterial) & 0x7FFFFFFF),
		WantCount };
	CSHouse_AppendFootprintHash(HashInput, Footprint);
	// 档位表进哈希：换一张网格、加一档、或换了顺序都必须重建（同 `RoofTileMeshBuiltFrom` 的教训）。
	for (const FLeafRank& Rank : Ranks) HashInput.Add(int32(GetTypeHash(Rank.Mesh) & 0x7FFFFFFF));
	// 洞变了门扇就得跟着变。**逐洞进哈希**，只记洞数会漏掉"同样两个洞、其中一个变宽了"。
	for (const FCSWallOpening* Door : Doors)
	{
		HashInput.Append({ Door->EdgeIndex, CSHouse_Q(Door->CenterS, 1),
			CSHouse_Q(Door->Width, DoorWidthQuantum), CSHouse_Q(Door->Z1, 1) });
	}
	const uint32 NewHash = CSHouse_Hash(HashInput);
	// ⚠️ 组件数进短路条件：蓝图重跑构造脚本会销毁 transient 组件而哈希一个字没变
	// （同 `RebuildRoofFinials` 那条），只比哈希门扇会在重跑构造脚本后永久消失。
	if (NewHash == DoorLeafDescHash && DoorLeafComponents.Num() == WantCount)
	{
		bool bAllValid = true;
		for (const TObjectPtr<UStaticMeshComponent>& Component : DoorLeafComponents)
		{
			if (!IsValid(Component)) { bAllValid = false; break; }
		}
		if (bAllValid) return;
	}
	DoorLeafDescHash = NewHash;

	Discard(WantCount);

	const FTransform Build = GetBuildTransform();
	for (int32 Index = 0; Index < WantCount; ++Index)
	{
		if (!DoorLeafComponents.IsValidIndex(Index))
		{
			UStaticMeshComponent* Created = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
			Created->SetupAttachment(RootComponent);
			Created->SetCollisionEnabled(ECollisionEnabled::NoCollision);   // 洞是逐像素 clip 出来的，门扇也不该挡人
			// ⚠️ **必须关 Nanite，否则一次注册多个门扇会把编辑器打崩**（2026-09-04 实测，
			// `DoorMaxWidth` 调到会切拱廊、同一帧要建 2 个以上门扇那一版）：
			//   批量注册走 `FStaticMeshComponentBulkReregisterContext` → `FScene::BatchAddPrimitives`
			//   → `ShouldCreateNaniteProxy` → `AuditMaterials` → `CheckMaterialUsage(MATUSAGE_Nanite)`
			//   → 材质缺这个用途标记 ⇒ `SetMaterialUsage` 重编译 ⇒ **`FlushRenderingCommands()`**
			//   ⇒ 在 post-tick 组件更新阶段抽干任务队列、把在途的 `EditMeshAsync` 完成回调拉进来
			//   ⇒ `MarkActorComponentForNeededEndOfFrameUpdate` 的 `!bPostTickComponentUpdate` 断言炸。
			// 门扇是二十几个顶点的板子，Nanite 对它零收益；关掉直接砍掉整条审计分支。
			// 这与"母材质没勾 `bUsedWithInstancedStaticMeshes` 会被静默换成默认材质"是同一族坑：
			// **材质用途标记要在资产上预先备好，不能等运行时注册的时候现补。**
			Created->bDisallowNanite = true;
			Created->RegisterComponent();
			DoorLeafComponents.Add(Created);
		}
		UStaticMeshComponent* Component = DoorLeafComponents[Index];
		if (!IsValid(Component)) continue;

		const FCSWallOpening& Door = *Doors[Index];
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Door.EdgeIndex, Footprint, WallThickness);

		// 门扇高：默认齐**起拱线**（拱顶那半圆留空，读作气窗）。矩形门扇顶到拱顶必然切角，
		// 所以 `DoorLeafRise` 默认 0，要顶满得先有拱形门扇网格。
		const float SpringZ = FMath::Max(Door.Z1 - Door.Rise(), 0.0f);
		const float LeafHeight = FMath::Max(FMath::Lerp(SpringZ, Door.Z1, DoorLeafRise), 1.0f);
		const float LeafWidth = FMath::Max(Door.Width * DoorLeafWidthRatio, 1.0f);

		// 挑档（TG 的 `balcony_door_rank1/2/3` 就是这么用的），再把余下的差值交给缩放。
		const FLeafRank& Rank = PickRank(LeafWidth);
		const FVector RankSize = Rank.Local.GetSize();
		const double Ext[3] = { RankSize.X, RankSize.Y, RankSize.Z };
		const int32 UpAxis = Rank.UpAxis, WidthAxis = Rank.WidthAxis, DepthAxis = Rank.DepthAxis;

		Component->SetStaticMesh(Rank.Mesh);
		// 留空 = 用网格自带材质槽（`SetMaterial(nullptr)` 会清成空槽画成灰，不能这么写）。
		if (DoorLeafMaterial) Component->SetMaterial(0, DoorLeafMaterial);

		// 资产的"上"送到局部 +Z、"宽"送到局部 +X。`FMatrix(X, Y, Z, O)` 把 e0/e1/e2 分别送到三个
		// 轴参数上。深度那根取 Z×X 保右手，否则门扇会镜像。**逐档算** —— 不同档可能轴向不同。
		FVector Basis[3];
		Basis[UpAxis] = FVector::UpVector;
		Basis[WidthAxis] = FVector::ForwardVector;
		Basis[DepthAxis] = FVector::CrossProduct(Basis[UpAxis], Basis[WidthAxis]);
		const FQuat Stand = FMatrix(Basis[0], Basis[1], Basis[2], FVector::ZeroVector).ToQuat();

		// 缩放：宽按洞宽、高按门扇高。纵横比被拉得太狠时**只缩不拉** —— 板条拉成面条比门扇
		// 小一圈难看。缩放作用在**资产自己的**局部空间（Stand 之前），所以按资产轴下标写。
		float ScaleWidth = (Ext[WidthAxis] > UE_KINDA_SMALL_NUMBER) ? LeafWidth / float(Ext[WidthAxis]) : 1.0f;
		float ScaleUp = (Ext[UpAxis] > UE_KINDA_SMALL_NUMBER) ? LeafHeight / float(Ext[UpAxis]) : 1.0f;
		const float Stretch = FMath::Max(DoorLeafMaxStretch, 1.0f);
		if (ScaleUp > ScaleWidth * Stretch) ScaleUp = ScaleWidth * Stretch;
		if (ScaleWidth > ScaleUp * Stretch) ScaleWidth = ScaleUp * Stretch;
		// **厚度必须留在墙里**：门扇要靠墙的外表面遮住溢出的部分，探出去就穿帮了
		// （宽门那档 ScaleWidth 会 > 1，`balcony_door_rank*` 原生 31.6 cm 厚，
		//  乘上去比 24 cm 的墙还厚 —— 2026-09-04 顺手抓到的）。两侧各留 2 cm。
		const double NativeDepth = FMath::Max(Ext[DepthAxis], double(UE_KINDA_SMALL_NUMBER));
		const float DepthRoom = FMath::Max(WallThickness - 4.0f, 2.0f);
		const float ScaleDepth = FMath::Min(ScaleWidth, float(DepthRoom / NativeDepth));

		FVector Scale3D;
		Scale3D[WidthAxis] = ScaleWidth;
		Scale3D[DepthAxis] = ScaleDepth;
		Scale3D[UpAxis] = ScaleUp;

		// 摆位：洞心沿边。厚度方向压在**墙中面**上（`F.Start` 在外表面、`F.In` 朝内 ⇒ 中面是
		// `+In * T/2`），`DoorLeafInset` 正值再沿**外**法线（= −In）往外挪。
		const FVector2D LocalXY = F.Start + F.U * Door.CenterS
			+ F.In * (WallThickness * 0.5 - double(DoorLeafInset));
		// 竖直：把资产在"上"轴上的最小值压到 Z0（枢轴在包围盒中心时该值为负，乘缩放正好抬起来）。
		const double BaseZ = double(Door.Z0) - Rank.Local.Min[UpAxis] * ScaleUp;
		const FVector LocalPos(LocalXY.X, LocalXY.Y, BaseZ);

		// 朝向：立正之后再绕 Z 转到这面墙上 —— 门扇的宽度方向贴着 `F.U`，正面朝外。
		const FQuat Face(FRotator(0.0, FMath::RadiansToDegrees(FMath::Atan2(F.U.Y, F.U.X)), 0.0));
		Component->SetWorldTransform(FTransform(
			Build.GetRotation() * Face * Stand,
			Build.TransformPosition(LocalPos),
			Scale3D));
	}

	CurrentDoorLeafCount = WantCount;
	FString RankText;
	for (const FLeafRank& Rank : Ranks)
	{
		RankText += FString::Printf(TEXT("%s(%.0f) "), *Rank.Mesh->GetName(), Rank.NativeWidth);
	}
	UE_LOG(LogTinyGladeHouse, Log, TEXT("[TinyGladeHouse] %s door leaves: count=%d ranks=[ %s]"),
		*GetName(), CurrentDoorLeafCount, *RankText);
}

FString ACSHouseActor::GetDoorLeafUndrawableReason() const
{
	FlushPendingReevaluate();
	// 逐环检查渲染那一侧（同 `GetRoofFinialUndrawableReason`）——"洞开了"证明不了"门扇画出来了"。
	if (!bDoorLeafEnabled) return TEXT("bDoorLeafEnabled 关着");
	if (DoorLeafMeshes.IsEmpty()) return TEXT("DoorLeafMeshes 是空的（留空 = 有意不长门扇）");
	int32 DoorCount = 0;
	for (const FCSWallOpening& O : CurrentOpenings)
	{
		if (O.Type == ECSOpeningType::Door && O.IsValid()) ++DoorCount;
	}
	if (DoorCount <= 0) return TEXT("这栋房一个门洞都没有（路没经过任何一面墙）");
	if (CurrentDoorLeafCount <= 0) return TEXT("有门洞却一扇门都没立起来");
	if (DoorLeafComponents.Num() != CurrentDoorLeafCount)
	{
		return FString::Printf(TEXT("组件数 %d 与记账数 %d 对不上"), DoorLeafComponents.Num(), CurrentDoorLeafCount);
	}
	for (int32 Index = 0; Index < DoorLeafComponents.Num(); ++Index)
	{
		const UStaticMeshComponent* Component = DoorLeafComponents[Index];
		if (!IsValid(Component)) return FString::Printf(TEXT("门扇 %d：组件无效"), Index);
		if (!Component->IsRegistered()) return FString::Printf(TEXT("门扇 %d：组件没注册"), Index);
		if (!Component->IsVisible()) return FString::Printf(TEXT("门扇 %d：组件不可见"), Index);
		if (!Component->GetStaticMesh()) return FString::Printf(TEXT("门扇 %d：网格是空的"), Index);
		if (Component->GetComponentScale().IsNearlyZero()) return FString::Printf(TEXT("门扇 %d：缩放退化成 0"), Index);
	}
	return FString();
}

FString ACSHouseActor::GetRoofFinialUndrawableReason() const
{
	FlushPendingReevaluate();
	// 逐环检查渲染那一侧 —— readback 断言对这些一个字都说不了（同 `GetRoofTileUndrawableReason`）。
	if (!RoofFinialMesh) return TEXT("没有 RoofFinialMesh");
	if (CurrentRoofFinialCount <= 0) return TEXT("一根尖顶都没立起来");
	if (RoofFinialComponents.Num() != CurrentRoofFinialCount)
	{
		return FString::Printf(TEXT("组件数 %d 与记账数 %d 对不上"), RoofFinialComponents.Num(), CurrentRoofFinialCount);
	}
	for (int32 Index = 0; Index < RoofFinialComponents.Num(); ++Index)
	{
		const UStaticMeshComponent* Component = RoofFinialComponents[Index];
		if (!IsValid(Component)) return FString::Printf(TEXT("第 %d 根没有组件"), Index);
		if (!Component->IsRegistered()) return FString::Printf(TEXT("第 %d 根的组件没注册"), Index);
		if (!Component->IsVisible()) return FString::Printf(TEXT("第 %d 根不可见"), Index);
		if (Component->GetStaticMesh() != RoofFinialMesh) return FString::Printf(TEXT("第 %d 根挂的不是 RoofFinialMesh"), Index);
		// **石阶那个坑的同一条**：材质槽为空时组件仍然会画，只是退回引擎默认表面材质 ——
		// 画面上是一片灰，而所有 readback 断言照绿。
		if (!Component->GetMaterial(0)) return FString::Printf(TEXT("第 %d 根 0 号材质槽是空的（会画成一片灰）"), Index);
	}
	return FString();
}

CSShaperSteps::EHandoverResult ACSHouseActor::EnsureDecorComponents()
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

	// ⚠️ 组件数一变就必须重建基础网格快照：palette 与组件是**按下标**对齐的，少一个就全体错位。
	if (CSShaperSteps::EnsureInstancedComponents(this, DecorComponents, Wanted.Num())) bDecorBaseMeshReady = false;
	for (const TObjectPtr<UCSGpuInstancedMeshComponent>& Component : DecorComponents) Component->InstanceMaterial = DecorMaterial;

	if (DecorGpuBuffers.Num() != Wanted.Num())
	{
		CSShaperSteps::ReleaseOnRenderThread(DecorGpuBuffers);
		DecorGpuBuffers.SetNum(Wanted.Num());
		DecorHandover.Capacities.Reset();
		bDecorBaseMeshReady = false;
	}
	if (Wanted.Num() == 0) return CSShaperSteps::EHandoverResult::UpToDate;

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
			// 顶点色这一条对杂物是决定性的：`Content/HouseTest/TinyGladeAsset/Textures/` 里**没有一张 clutter 贴图**
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
	if (!bDecorBaseMeshReady) return CSShaperSteps::EHandoverResult::UpToDate;

	// 容量按**配置上限**一次付清，之后永不扩容（零阻塞纪律）。
	// ⚠️ **必须再走一次 `CSShaperSteps::ReserveCount` 的台阶**，不能把上限直接喂给 ReserveCapacity：
	// 上限是 FootprintSize 的**连续函数**（周长 / 间距），而 ReserveCapacity 只对齐到 64 ——
	// 拖尺寸时每涨过一个间距就重新分配一次。藤蔓那轮正是漏了这一步，实测一段拖动 21 次阻塞刷新。
	const CSHouseDecor::FParams Params = MakeDecorParams();
	const int32 Bound = CSHouseDecor::MaxRecordsBound(GetFootprint(), RoofOverhang, PierWidth, Params);
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
	const double Reach = CSShaperSteps::QuantizeUp(GetFootprint().GetCenteredSpan() * 0.6
		+ Params.GateApproachDistance + Params.GateApproachSpread + MeshReach);
	const double Top = CSShaperSteps::QuantizeUp(CSHouseRoof_RidgeZ(GetRoofDesc()) + Params.RoofStandOff + MeshReach);
	// 下界也取 -Reach：摆件落在**地面**上，而房子坐在 footprint 内的最高点，
	// 所以墙脚那一圈可以比房底低不少（低多少由地形说了算，不是常数）。
	FBox LocalBounds(FVector(-Reach, -Reach, -Reach), FVector(Reach, Reach, Top));
	// 各 palette 的组件是兄弟、变换相同，取第 0 个即可。
	LocalBounds = CSHouse_BuildBoundsToComponent(GetBuildTransform(),
		DecorComponents.IsValidIndex(0) ? DecorComponents[0].Get() : nullptr, LocalBounds);
	LocalBounds = CSShaperSteps::MergeHandoverBounds(LocalBounds, DecorHandover, bForceFullRebuild);

	TArray<CSShaperSteps::FHandoverSource, TInlineAllocator<8>> Sources;
	for (int32 Index = 0; Index < DecorGpuBuffers.Num(); ++Index)
	{
		Sources.Add(CSShaperSteps::MakeHandoverSource(
			DecorComponents.IsValidIndex(Index) ? ToRawPtr(DecorComponents[Index]) : nullptr,
			DecorGpuBuffers[Index], /*bWithCustomData*/ false));
	}
	return CSShaperSteps::HandOverInstanceSources(Sources, LocalBounds, DecorHandover);
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
			DecorHandover.Reset();
			CurrentDecorInstanceCount = 0;
			CurrentDecorAnchorCount = 0;
			CurrentDecorGateAnchorCount = 0;
			DecorDescHash = 0;
		}
		return;
	}

	const CSShaperSteps::EHandoverResult DecorHandoverResult = EnsureDecorComponents();
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
		CSHouse_Q(WallHeight, 1),
		CSHouse_Q(RoofPitch, 0.1), CSHouse_Q(RoofOverhang, 1),
		CSHouse_Q(DecorWallFootSpacing, 0.5), CSHouse_Q(DecorEaveSpacing, 0.5),
		CSHouse_Q(DecorMinSpacing, 0.5), CSHouse_Q(DecorRoadReject, 0.01),
		CSHouse_Q(DecorScale, 0.01), CSHouse_Q(DecorScaleJitter, 0.01), DecorSeed });
	CSHouse_AppendFootprintHash(HashInput, GetFootprint());
	for (const CSHouseDecor::FAnchor& Anchor : Anchors)
	{
		HashInput.Append({ int32(Anchor.Family), Anchor.AnchorId,
			CSHouse_Q(Anchor.Location.X, 1), CSHouse_Q(Anchor.Location.Y, 1), CSHouse_Q(Anchor.Location.Z, 1) });
	}
	const uint32 NewHash = CSHouse_Hash(HashInput);

	bool bBuffersReady = DecorGpuBuffers.Num() == DecorComponents.Num();
	for (const CSShaperSteps::FPaletteBuffers& Buffers : DecorGpuBuffers) bBuffersReady &= Buffers.IsValid();
	// 刚交接过（扩容换了清零的新 buffer / 包围盒变了）就必须重打包，哈希没变也不能早退（审查 B1）。
	const bool bMustRepack = DecorHandoverResult == CSShaperSteps::EHandoverResult::HandedOver;
	if (!bMustRepack && NewHash == DecorDescHash && DecorHandover.Capacities.Num() == DecorGpuBuffers.Num() && bBuffersReady) return;

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
	FlushPendingReevaluate();
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
	FlushPendingReevaluate();
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
	FlushPendingReevaluate();
	return CSHouse_ReadGpuInstanceCount(FrameComponent);
}

int32 ACSHouseActor::DebugReadVineBranchCountGpuSync() const
{
	FlushPendingReevaluate();
	return CSHouse_ReadGpuInstanceCount(VineBranchComponent);
}

int32 ACSHouseActor::DebugReadVineLeafCountGpuSync() const
{
	FlushPendingReevaluate();
	return CSHouse_ReadGpuInstanceCount(VineLeafComponent);
}

int32 ACSHouseActor::DebugReadVineFlowerCountGpuSync() const
{
	FlushPendingReevaluate();
	return CSHouse_ReadGpuInstanceCount(VineFlowerComponent);
}

int32 ACSHouseActor::DebugReadDecorInstanceCountGpuSync() const
{
	FlushPendingReevaluate();
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
	FlushPendingReevaluate();
	return Super::DebugGetGpuAssetMismatchSync();
}

void ACSHouseActor::GetInstancedFamilies(TArray<FCSInstancedFamily>& OutFamilies) const
{
	// ⚠️ 不许在这里补票（FlushPendingReevaluate）：族表必须无副作用，补票放在诊断入口里（见上）。
	// 门框砖 + 接缝砖 + 角石 + 包边 + 砖层共用这一个组件。
	OutFamilies.Add({ FrameComponent, TEXT("门框砖"), TEXT("FrameBricks"), CurrentFrameBrickCount > 0 });
	OutFamilies.Add({ VineBranchComponent, TEXT("藤枝"), TEXT("VineBranch"), CurrentVineSegmentCount > 0 });
	OutFamilies.Add({ VineLeafComponent, TEXT("藤叶"), TEXT("VineLeaf"), CurrentVineSegmentCount > 0 });
	// 花单独判：一栋不长花的房子（`VineFlowerMesh` 留空）是**合法**的，拿枝数当门会把它误判。
	OutFamilies.Add({ VineFlowerComponent, TEXT("藤花"), TEXT("VineFlower"), CurrentVineFlowerCount > 0 });
	for (int32 Index = 0; Index < DecorComponents.Num(); ++Index) OutFamilies.Add({ DecorComponents[Index], FString::Printf(TEXT("摆件[%d]"), Index), FString::Printf(TEXT("Decor%d"), Index), CurrentDecorInstanceCount > 0 });
	OutFamilies.Add({ RoofTileComponent, TEXT("屋瓦"), TEXT("RoofTiles"), CurrentRoofTileCount > 0 });
	OutFamilies.Add({ PillarBrickComponent, TEXT("柱砖"), TEXT("PillarBricks"), bPillarUseBricks && PillarBrickMesh && CurrentPillarCount > 0 });
}

void ACSHouseActor::ReleaseInstancedBuffers()
{
	// 只放本 actor 分配的生产者那一份（排在已入队的命令之后，见 CSShaperSteps::ReleaseOnRenderThread）。
	// 组件手上的实例源、组件自己的常驻网格、槽网格的显存，各由组件在 OnComponentDestroyed 里放。
	CSShaperSteps::ReleaseOnRenderThread(FrameGpuBuffers);
	CSShaperSteps::ReleaseOnRenderThread(PillarGpuBuffers);
	CSShaperSteps::ReleaseOnRenderThread(VineGpuBuffers);
	CSShaperSteps::ReleaseOnRenderThread(RoofTileGpuBuffers);
	CSShaperSteps::ReleaseOnRenderThread(DecorGpuBuffers);
	// 缓冲都没了 ⇒ 交接缓存跟着作废，下次 Ensure* 按新分配的那批重交。
	FrameHandover.Reset();
	PillarHandover.Reset();
	VineHandover.Reset();
	RoofTileHandover.Reset();
	DecorHandover.Reset();
	// ⚠️ 编辑器里删除可以撤销 —— 复活的是**同一批对象**：缓冲刚交回去，槽网格与实例组件的显存也已被
	// 各自的组件还掉，而各家的早退门只看输入哈希。复活后的第一次重求值因此必须全量重建（它会把各族的
	// desc 哈希清零）。
	bForceFullRebuild = true;
}

#if WITH_EDITOR

bool ACSHouseActor::DebugBakeFrameBricksSync(const FString& AssetPath, int32& OutTriangles,
	int32& OutVertexInstances, int32& OutUVChannels, int32& OutDistinctBakedRandoms,
	int32& OutGpuInstanceCount, bool& bOutRandomsMatchGpu)
{
	FlushPendingReevaluate();
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
	FlushPendingReevaluate();
	return Super::SaveInstancedToStaticMeshes(BakeFolder, bSaveAssets);
}

UStaticMesh* ACSHouseActor::DebugBakeVineBranchesSync(const FString& AssetPath)
{
	FlushPendingReevaluate();
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
	// 瓦与房体的屋顶槽共用 `RoofMaterial`：它们是同一样东西的两半（槽 1 现在没有三角，
	// 屋面全在瓦上）。⚠️ 实例路径要求母材质勾了 `bUsedWithInstancedStaticMeshes`，
	// 没勾会被引擎**静默换成默认材质** —— `GetRoofTileUndrawableReason` 专门查这一条。
	if (IsValid(RoofTileComponent)) RoofTileComponent->InstanceMaterial = RoofMaterial;
	if (PillarMesh) BindMeshSlotMaterials(PillarMeshComponent, PillarMesh, { PillarMaterial });

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

void ACSHouseActor::PostLoad()
{
	Super::PostLoad();
	// ⚠️ **模板（蓝图 CDO / 原型）不迁、口径字段也不写**。`WallSConvention` 是普通属性，关卡里的实例
	// 按**原型**做增量存盘，而蓝图 CDO 同样会走到这里（`UClass::PostLoadDefaultObject`）：
	//   · CDO 若先被置 1，磁盘上根本没有这个字段的旧实例就从原型继承到 1 ⇒ 跳过迁移；
	//   · 实例若先于 CDO 构造，迁完写回的 1 又与之后被置 1 的 CDO 相等 ⇒ 不落盘 ⇒ 下次加载再迁一遍
	//     （奇数边的窗每存读一轮就挪一个墙厚）。
	// 两种加载次序各错一边。模板恒为 0，实例的 1 就永远与原型不同、必定落盘，迁不迁只由实例自己的
	// 存盘数据决定。代价：斜接之前存的蓝图若在 CDO 里带着奇数边的默认 `Windows`，从它新拖出来的
	// 实例会继承未换算的值 —— 现有资产没有这种默认值（2026-09-14 核对 BP_TinyGladeHouse）。
	if (IsTemplate() || WallSConvention >= 1) return;

	// 直角对接 → 斜接（2026-09-13）。只迁**存下来的绝对弧长**：锚点自带口径字段、在
	// `CSHouse_AnchorS` 里就地换算，门的位置每轮从道路重算，都不需要在这里动。
	//
	// ⚠️ 不 `Modify()`：这里在加载序列里，不是用户编辑。迁完的值随下一次存盘落地；不存盘就
	// 下次加载再迁一遍，读的仍是磁盘上那份旧值 —— 幂等。
	const float T = WallThickness;
	for (FCSHouseWindow& Window : Windows)
	{
		// 旧口径奇数边从外角点往里 T 处量起 ⇒ 同一个物理点在斜接口径下远一个 T。
		// 旧存档只可能是矩形（边号 0..3），奇偶判据成立。
		if ((Window.EdgeIndex & 1) != 0) Window.CenterS += T;
	}
	// 环参数随奇数边变长而整体错位，旧记忆没有意义；清掉由下一轮重判（至多一次迟回决策）。
	DoorRunMemory.Empty();
	PierSpanIsPier.Empty();
	WallSConvention = 1;
}

void ACSHouseActor::PostActorCreated()
{
	Super::PostActorCreated();
	WallSConvention = 1;
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

void ACSHouseActor::Destroyed()
{
	// **编辑器 world 里只有这一条会来**（那个 world 没有 begun play，`DestroyActor` 不发
	// `EndPlay`）。漏掉它的症状是"删掉房子，四个锥子还浮在原地" —— 而抓手的宿主弱引用
	// 此刻已经失效，它们既不认识任何房子又能被选中拖动，纯粹的噪声。
	ExitResizeMode();
	Super::Destroyed();
}

void ACSHouseActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 覆盖 PIE 结束与关卡卸载 —— 这两条 `Destroyed` 收不到。`ExitResizeMode` 幂等，两处都调。
	ExitResizeMode();
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

	// 整栋被拖动时抓手靠 attach 跟着走，位置本来就是对的；松手补一次归位是为了兜住
	// **旋转与缩放** —— 那两样会改变墙外皮中心的世界位置，而 attach 只保相对量不变。
	if (bFinished) SnapResizeHandles();
}

void ACSHouseActor::PostEditUndo()
{
	Super::PostEditUndo();

	// 编辑器 world 里撤销本来就会经 UObject::PostEditUndo → PostEditChange →
	// AActor::PostEditChangeProperty 重跑构造脚本（→ 基类 OnConstruction → ReevaluateSite）；
	// 这里兜的是不重跑的那些情形（PIE world：ReregisterComponentsWhenModified 为假）。
	// 重求值幂等，编辑器里多来一次无害 —— 而不来的话，撤销一次移动之后房子会画在旧位置。
	ReevaluateSite();
}
#endif
