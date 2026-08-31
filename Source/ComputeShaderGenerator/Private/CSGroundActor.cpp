#include "CSGroundActor.h"

#include "CSGpuInstancedMeshComponent.h"
#include "CSGpuMeshTypes.h"
#include "CSGroundRockShell.h"
#include "CSGroundShaperActor.h"
#include "CSGroundShaperField.h"
#include "CSHouseVine.h"      // BuildBaseMesh：裙边摆件与房子那四家共用同一份基础网格读取器
#include "CSMesh.h"
#include "CSMeshRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "Materials/Material.h"          // bUsedWithInstancedStaticMeshes —— 三条"静默换材质"里的一条
#include "Materials/MaterialInterface.h"
#include "Misc/Crc.h"                    // 裙边摆件的幂等哈希（unity 会掩盖，-SingleFile 才照得出来）
#if WITH_EDITOR
#include "StaticMeshAttributes.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogTinyGladeGround, Log, All);

FCSGroundPaintEditorRequest ACSGroundActor::OnGroundPaintEditorRequest;

namespace
{
// Unity/jumbo builds share a TU, so file-local names carry a CSGround_ prefix.

/** CPU twin of PaintVertexColorsSphereCS's falloff weight. Same constants, same smoothstep —
 *  the mirror stays bit-stable against the GPU because both read 8-bit storage, run this exact
 *  math, and round back to 8 bits. Change it together with the kernel and ECSMeshPaintBlendOp. */
float CSGround_BrushWeight(float Distance, float Radius, float Falloff)
{
	const float HardRadius = Radius * (1.0f - Falloff);
	const float T = FMath::Clamp((Radius - Distance) / FMath::Max(Radius - HardRadius, 1e-3f), 0.0f, 1.0f);
	return T * T * (3.0f - 2.0f * T);
}

/** CPU twin of the kernel's per-channel blend. C/Paint/E are RGBA in 0..1. */
FLinearColor CSGround_BlendColor(const FLinearColor& C, const FLinearColor& Paint, const FLinearColor& E, ECSMeshPaintBlendOp Op)
{
	auto Blend = [Op](float Channel, float PaintChannel, float Amount)
	{
		switch (Op)
		{
		case ECSMeshPaintBlendOp::Replace: return FMath::Lerp(Channel, PaintChannel, Amount);
		case ECSMeshPaintBlendOp::Add:     return Channel + PaintChannel * Amount;
		case ECSMeshPaintBlendOp::Max:     return FMath::Max(Channel, PaintChannel * Amount);
		case ECSMeshPaintBlendOp::Erase:   return Channel * (1.0f - Amount);
		default:                           return Channel;
		}
	};
	return FLinearColor(
		Blend(C.R, Paint.R, E.R),
		Blend(C.G, Paint.G, E.G),
		Blend(C.B, Paint.B, E.B),
		Blend(C.A, Paint.A, E.A));
}

/** Mirror storage quantisation, the CPU twin of PackColorBGRA's round(saturate(x) * 255). */
FColor CSGround_QuantizeColor(const FLinearColor& Value)
{
	auto PackByte = [](float Channel)
	{
		return uint8(FMath::RoundToInt(FMath::Clamp(Channel, 0.0f, 1.0f) * 255.0f));
	};
	return FColor(PackByte(Value.R), PackByte(Value.G), PackByte(Value.B), PackByte(Value.A));
}

FLinearColor CSGround_UnquantizeColor(const FColor& Stored)
{
	return FLinearColor(
		float(Stored.R) / 255.0f,
		float(Stored.G) / 255.0f,
		float(Stored.B) / 255.0f,
		float(Stored.A) / 255.0f);
}
}

void ACSGroundActor::StartVertexColorPaint()
{
	OnGroundPaintEditorRequest.Broadcast(this);
}

bool ACSGroundActor::EnsureMirrorInitialized()
{
	const int32 WantVertsX = NumCellsX + 1;
	const int32 WantVertsY = NumCellsY + 1;
	const bool bMatches = Mirror.IsInitialized()
		&& Mirror.NumVertsX == WantVertsX
		&& Mirror.NumVertsY == WantVertsY
		&& FMath::IsNearlyEqual(Mirror.CellSize, CellSize);
	if (bMatches)
	{
		// MaxAbsHeight 是 transient 的：反序列化出的镜像在这里恢复它，RaycastGround 的
		// 平面/march 分路才不会对着旧值走错。
		MaxAbsHeight = 0.0f;
		for (const float Height : Mirror.Heights) MaxAbsHeight = FMath::Max(MaxAbsHeight, FMath::Abs(Height));
		return false;
	}

	// 尺寸/格距变了没有可靠的重采样语义，v1 直接重置为平地 + 底色（计划文档 D1 已注明）。
	Mirror.NumVertsX = WantVertsX;
	Mirror.NumVertsY = WantVertsY;
	Mirror.CellSize = CellSize;
	Mirror.Heights.Init(0.0f, WantVertsX * WantVertsY);
	Mirror.Colors.Init(CSGround_QuantizeColor(BaseColor), WantVertsX * WantVertsY);
	MaxAbsHeight = 0.0f;
	++PaintRevision;   // 顶点色被整片重置 = 道路没了，岩壳要重新浮上来
	return true;
}

FVector ACSGroundActor::VertexWorldPosition(int32 X, int32 Y) const
{
	const FVector Origin = GetActorLocation();
	return FVector(
		Origin.X + X * Mirror.CellSize,
		Origin.Y + Y * Mirror.CellSize,
		Origin.Z + Mirror.Heights[Mirror.VertexIndex(X, Y)]);
}

void ACSGroundActor::BuildSnapshotFromMirror(FCSGpuMeshCPUData& OutSnapshot) const
{
	OutSnapshot.Reset();
	if (!Mirror.IsInitialized()) return;

	const int32 VertsX = Mirror.NumVertsX;
	const int32 VertsY = Mirror.NumVertsY;
	const int32 VertexCount = VertsX * VertsY;
	const float Cell = Mirror.CellSize;
	const float SafeUVPeriod = FMath::Max(UVWorldPeriod, 1.0f);

	OutSnapshot.Positions.SetNumUninitialized(VertexCount);
	OutSnapshot.Normals.SetNumUninitialized(VertexCount);
	OutSnapshot.Tangents.SetNumUninitialized(VertexCount);
	OutSnapshot.TexCoords().SetNumUninitialized(VertexCount);
	OutSnapshot.Colors.SetNumUninitialized(VertexCount);

	for (int32 Y = 0; Y < VertsY; ++Y)
	{
		for (int32 X = 0; X < VertsX; ++X)
		{
			const int32 Vertex = Mirror.VertexIndex(X, Y);
			OutSnapshot.Positions[Vertex] = FVector3f(VertexWorldPosition(X, Y));

			// 中心差分法线；边界用单侧差分（clamp 邻居）。平地退化为 +Z。
			const int32 XPrev = FMath::Max(X - 1, 0);
			const int32 XNext = FMath::Min(X + 1, VertsX - 1);
			const int32 YPrev = FMath::Max(Y - 1, 0);
			const int32 YNext = FMath::Min(Y + 1, VertsY - 1);
			const float DzDx = (Mirror.Heights[Mirror.VertexIndex(XNext, Y)] - Mirror.Heights[Mirror.VertexIndex(XPrev, Y)]) / (Cell * float(XNext - XPrev));
			const float DzDy = (Mirror.Heights[Mirror.VertexIndex(X, YNext)] - Mirror.Heights[Mirror.VertexIndex(X, YPrev)]) / (Cell * float(YNext - YPrev));
			const FVector3f Normal = FVector3f(-DzDx, -DzDy, 1.0f).GetSafeNormal(UE_SMALL_NUMBER, FVector3f::UnitZ());
			OutSnapshot.Normals[Vertex] = Normal;
			// 沿 +X 的切线；CopyFromMeshSnapshot 会再正交化一遍。
			OutSnapshot.Tangents[Vertex] = FVector3f::CrossProduct(FVector3f::UnitY(), Normal).GetSafeNormal(UE_SMALL_NUMBER, FVector3f::UnitX());

			OutSnapshot.TexCoords()[Vertex] = FVector2f(X * Cell / SafeUVPeriod, Y * Cell / SafeUVPeriod);

			const FLinearColor Color = CSGround_UnquantizeColor(Mirror.Colors[Vertex]);
			OutSnapshot.Colors[Vertex] = FVector4f(Color.R, Color.G, Color.B, Color.A);
		}
	}

	// 每格两个三角，按**引擎绕序**排角点（正面 = cross(C-A, B-A) 朝 +Z）。
	// 注意这与 CSMeshBuild::ResidentFaceNormalScaled 的"从缓冲推法线"口径差一个负号：
	// 渲染路径吃的是引擎绕序（CopyFromStaticMesh 默认 bFlipWinding=0 直传 StaticMesh 索引就能正常
	// 渲染即为实证），按 cross(B-A,C-A)=外法线 去排角点会让整片地面朝里 —— 实测确认过。
	OutSnapshot.Indices.Reserve((VertsX - 1) * (VertsY - 1) * 6);
	for (int32 Y = 0; Y < VertsY - 1; ++Y)
	{
		for (int32 X = 0; X < VertsX - 1; ++X)
		{
			const uint32 V00 = uint32(Mirror.VertexIndex(X, Y));
			const uint32 V10 = uint32(Mirror.VertexIndex(X + 1, Y));
			const uint32 V01 = uint32(Mirror.VertexIndex(X, Y + 1));
			const uint32 V11 = uint32(Mirror.VertexIndex(X + 1, Y + 1));
			OutSnapshot.Indices.Append({ V00, V01, V10, V10, V01, V11 });
		}
	}

	OutSnapshot.SourceSpace = FCSGpuMeshCPUData::ESpace::World;
	OutSnapshot.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerVertex;
	if (GroundMaterial) OutSnapshot.Materials.Add(GroundMaterial);
}

void ACSGroundActor::RebuildGroundMesh()
{
	EnsureMirrorInitialized();
	if (!Mirror.IsInitialized()) return;

	FCSGpuMeshCPUData Snapshot;
	BuildSnapshotFromMirror(Snapshot);

	const TArray<TObjectPtr<UMaterialInterface>> Materials = { GroundMaterial };
	if (!UploadTinyGladeSnapshot(Snapshot, Materials)) return;
	MeshBuiltAtLocation = GetActorLocation();
	OnGroundChanged.Broadcast(this, ComputeGroundWorldBox());
	// 全量重建换掉了整份常驻流（石阶扫描与岩壳披挂都要读它的色流），所以这里必须重扫一遍。
	RebuildStairs();
	// 岩壳的哈希在这里清零：本函数也是"怀疑状态不同步时手动点一次即对齐"的入口，
	// 而披挂的短路是纯哈希的 —— 不清零的话，因缺资产失败过一次就再也不会重试。
	RockShellBuiltHash = 0;
	RebuildRockShell();
	// 裙边摆件同样在这里清哈希：它的短路也是纯哈希的，因缺资产失败过一次就再也不会重试。
	SkirtDecorHash = 0;
	RebuildSkirtDecor();
}

FBox ACSGroundActor::ComputeGroundWorldBox() const
{
	const FBox2D Rect = GetWorldRect2D();
	const double GroundZ = GetActorLocation().Z;
	return FBox(
		FVector(Rect.Min.X, Rect.Min.Y, GroundZ - MaxAbsHeight - 1.0),
		FVector(Rect.Max.X, Rect.Max.Y, GroundZ + MaxAbsHeight + 1.0));
}

void ACSGroundActor::ResetPaint()
{
	EnsureMirrorInitialized();
	if (!Mirror.IsInitialized()) return;

	for (FColor& Stored : Mirror.Colors) Stored = CSGround_QuantizeColor(BaseColor);
	++PaintRevision;
	RebuildGroundMesh();
	MarkPackageDirty();
}

void ACSGroundActor::BeginPaintStroke()
{
	bPaintStrokeOpen = true;
	StrokeDirtyBounds = FBox(ForceInit);
}

void ACSGroundActor::EndPaintStroke()
{
	if (!bPaintStrokeOpen) return;
	bPaintStrokeOpen = false;

	// 收笔：把队列彻底清空。EdMode 退出后没人再每帧推，剩下的笔不推就永远停在上一帧的画面。
	FlushPaintToGpu(/*bBlockIfNeeded*/ true);

	// 变更通知已在每次落笔时直推（OnGroundChanged），这里只负责序列化脏标记。
	if (StrokeDirtyBounds.IsValid) MarkPackageDirty();
}

void ACSGroundActor::FlushPaintToGpu(bool bBlockIfNeeded)
{
	if (PendingPaintDabs.IsEmpty() || !TinyGladeMesh) return;

	// 在途：这一帧不推，队列原样留到下一帧（顺序不变，混合结果不变）。收笔时不能这样拖着。
	if (TinyGladeMesh->IsEditInFlight())
	{
		if (!bBlockIfNeeded) return;
		// 同步路径 FIFO 排在异步之后，会一直阻塞到两者都跑完 —— 一次 stroke 至多付一次。
		for (const UCSMeshOps::FCSMeshPaintSphereDab& Dab : PendingPaintDabs)
		{
			UCSMeshOps::PaintVertexColorsSphereInRegion(TinyGladeMesh, Dab.Center, Dab.Radius, Dab.Falloff, Dab.Strength,
				Dab.Color, Dab.ChannelMask, Dab.BlendOp, Dab.VertsX, Dab.VertsY, Dab.RegionMin, Dab.RegionMax);
		}
		PendingPaintDabs.Reset();
		RebuildStairs();
		RebuildRockShell();
		RebuildSkirtDecor();
		return;
	}

	// EditFunc 是 owned：整批落笔按值搬进 lambda，不能留下指向本帧栈/成员的引用。
	TArray<UCSMeshOps::FCSMeshPaintSphereDab> Batch = MoveTemp(PendingPaintDabs);
	PendingPaintDabs.Reset();

	// 拷一份留作兜底：EditMeshAsync 拒了就必须自己把这批笔补上，否则镜像有色而 GPU 没有 ——
	// 那正是本 actor 第一纪律（镜像/GPU 双写不许分叉）要防的分叉，且再也不会自愈。
	TArray<UCSMeshOps::FCSMeshPaintSphereDab> Fallback = Batch;
	const bool bAccepted = TinyGladeMesh->EditMeshAsync(
		[Dabs = MoveTemp(Batch)](FCSMeshEditContext& Context)
		{
			for (const UCSMeshOps::FCSMeshPaintSphereDab& Dab : Dabs) UCSMeshOps::AddPaintSpherePasses(Context, Dab);
		});
	if (!bAccepted)
	{
		UE_LOG(LogTinyGladeGround, Warning, TEXT("[TinyGladeGround] %s async paint flush refused; falling back to sync."), *GetName());
		for (const UCSMeshOps::FCSMeshPaintSphereDab& Dab : Fallback)
		{
			UCSMeshOps::PaintVertexColorsSphereInRegion(TinyGladeMesh, Dab.Center, Dab.Radius, Dab.Falloff, Dab.Strength,
				Dab.Color, Dab.ChannelMask, Dab.BlendOp, Dab.VertsX, Dab.VertsY, Dab.RegionMin, Dab.RegionMax);
		}
	}

	// 石阶的道路门控读的是**GPU 色流**，而落笔只写镜像 + 排队 —— 所以重扫必须排在这里，
	// 不能排在 ApplyPaintStroke 里。渲染命令 FIFO 保证扫描看到的是刚推上去的那一笔；
	// 排错位置的症状是"石阶比路慢一笔"，只在快速涂抹时才看得见。
	RebuildStairs();
	// 岩壳同理，而且它的症状更隐蔽：壳在路上是**连续下沉**（裁决五），慢一笔看起来只是
	// "沉得不够深"，不像石阶那样明显缺一截。
	RebuildRockShell();
	// 裙边摆件读的是**镜像**（落笔已同步写进去了），不是 GPU 色流，所以它其实不依赖上面
	// 那条 FIFO；跟着排在这里只是为了让"改了地面 ⇒ 三条派生链一起对齐"只有一个位置要维护。
	RebuildSkirtDecor();
}

void ACSGroundActor::ApplyPaintStroke(FVector WorldCenter)
{
	if (!TinyGladeMesh || !Mirror.IsInitialized()) return;
	if (BrushRadius <= 0.0f || BrushStrength <= 0.0f) return;
	if (!bPaintStrokeOpen) BeginPaintStroke();   // 脚本/测试直接落笔时自开括号

	// 区域派发：默认半径 300 / 格距 50 只覆盖 13×13 = 169 个格点，而全网格版会按顶点容量
	// 开线程（256² 是 66,049，1024² 是 1,050,625）—— 派发量随地面尺寸线性膨胀而实际工作量恒定。
	FIntPoint RectMin, RectMax;
	if (!ComputeBrushGridRect(WorldCenter, RectMin, RectMax)) return;

	// **落笔不碰 GPU**：只写镜像（权威）+ 把这一笔排进队列，由 FlushPaintToGpu 每帧一次异步推送。
	// 早先这里每次鼠标移动就是一次 EditMeshSync = 一次 FlushRenderingCommands，交互热路径
	// 因此彻底取消了 GT/RT 流水并行（危害不是停顿本身，是帧率被钉在 GT + RT 的串行和上）。
	UCSMeshOps::FCSMeshPaintSphereDab Dab;
	Dab.Center = WorldCenter;
	Dab.Radius = BrushRadius;
	Dab.Falloff = BrushFalloff;
	Dab.Strength = BrushStrength;
	Dab.Color = PaintColor;
	Dab.ChannelMask = PaintChannelMask;
	Dab.BlendOp = PaintBlendOp;
	Dab.VertsX = Mirror.NumVertsX;
	Dab.VertsY = Mirror.NumVertsY;
	Dab.RegionMin = RectMin;
	Dab.RegionMax = RectMax;
	PendingPaintDabs.Add(Dab);

	ApplyPaintToMirror(WorldCenter);

	const FVector Extent(BrushRadius);
	const FBox TickBounds(WorldCenter - Extent, WorldCenter + Extent);
	StrokeDirtyBounds += TickBounds;

	// 逐笔直推：画的过程中消费者（房屋）就实时重判，不等 stroke 结束。
	OnGroundChanged.Broadcast(this, TickBounds);
}

bool ACSGroundActor::ComputeBrushGridRect(const FVector& WorldCenter, FIntPoint& OutMin, FIntPoint& OutMax) const
{
	if (!Mirror.IsInitialized() || Mirror.CellSize <= 0.0f) return false;

	const FVector Origin = GetActorLocation();
	const float Cell = Mirror.CellSize;
	// 未 clamp 的矩形先判出界：clamp 后的空矩形与"贴边一格"长得一样，先判再夹。
	const int32 RawMinX = FMath::FloorToInt32((WorldCenter.X - Origin.X - BrushRadius) / Cell);
	const int32 RawMaxX = FMath::CeilToInt32((WorldCenter.X - Origin.X + BrushRadius) / Cell);
	const int32 RawMinY = FMath::FloorToInt32((WorldCenter.Y - Origin.Y - BrushRadius) / Cell);
	const int32 RawMaxY = FMath::CeilToInt32((WorldCenter.Y - Origin.Y + BrushRadius) / Cell);
	if (RawMaxX < 0 || RawMinX > Mirror.NumVertsX - 1 || RawMaxY < 0 || RawMinY > Mirror.NumVertsY - 1) return false;

	OutMin = FIntPoint(FMath::Clamp(RawMinX, 0, Mirror.NumVertsX - 1), FMath::Clamp(RawMinY, 0, Mirror.NumVertsY - 1));
	OutMax = FIntPoint(FMath::Clamp(RawMaxX, 0, Mirror.NumVertsX - 1), FMath::Clamp(RawMaxY, 0, Mirror.NumVertsY - 1));
	return true;
}

void ACSGroundActor::ApplyPaintToMirror(const FVector& WorldCenter)
{
	FIntPoint RectMin, RectMax;
	if (!ComputeBrushGridRect(WorldCenter, RectMin, RectMax)) return;
	const int32 MinX = RectMin.X, MaxX = RectMax.X, MinY = RectMin.Y, MaxY = RectMax.Y;

	const float Strength = FMath::Clamp(BrushStrength, 0.0f, 1.0f);
	const FLinearColor Mask(
		FMath::Clamp(PaintChannelMask.R, 0.0f, 1.0f),
		FMath::Clamp(PaintChannelMask.G, 0.0f, 1.0f),
		FMath::Clamp(PaintChannelMask.B, 0.0f, 1.0f),
		FMath::Clamp(PaintChannelMask.A, 0.0f, 1.0f));

	for (int32 Y = MinY; Y <= MaxY; ++Y)
	{
		for (int32 X = MinX; X <= MaxX; ++X)
		{
			// 与 kernel 相同的三维距离（含高度），别退化成 2D —— 那会在坡地上分叉。
			const float Distance = float(FVector::Dist(VertexWorldPosition(X, Y), WorldCenter));
			if (Distance >= BrushRadius) continue;

			const float Weight = CSGround_BrushWeight(Distance, BrushRadius, FMath::Clamp(BrushFalloff, 0.0f, 1.0f));
			const FLinearColor E = Mask * (Weight * Strength);

			FColor& Stored = Mirror.Colors[Mirror.VertexIndex(X, Y)];
			const FLinearColor Blended = CSGround_BlendColor(CSGround_UnquantizeColor(Stored), PaintColor, E, PaintBlendOp);
			Stored = CSGround_QuantizeColor(Blended);
		}
	}

	// 道路权重变了 ⇒ 岩壳的下沉量变了。逐笔哈希 257² 个字节太贵，一个单调计数器
	// 给出同样的"变了没有"判定（见 PaintRevision 的注释）。
	++PaintRevision;
}

bool ACSGroundActor::WorldToGrid(const FVector2D& WorldXY, FVector2D& OutGrid) const
{
	const FVector Origin = GetActorLocation();
	const FVector2D Grid((WorldXY.X - Origin.X) / Mirror.CellSize, (WorldXY.Y - Origin.Y) / Mirror.CellSize);
	OutGrid.X = FMath::Clamp(Grid.X, 0.0, double(Mirror.NumVertsX - 1));
	OutGrid.Y = FMath::Clamp(Grid.Y, 0.0, double(Mirror.NumVertsY - 1));
	return Grid.X >= 0.0 && Grid.Y >= 0.0 && Grid.X <= double(Mirror.NumVertsX - 1) && Grid.Y <= double(Mirror.NumVertsY - 1);
}

bool ACSGroundActor::SampleBilinear(const FVector2D& WorldXY, float& OutHeight, FLinearColor& OutColor) const
{
	if (!Mirror.IsInitialized()) return false;

	FVector2D Grid;
	if (!WorldToGrid(WorldXY, Grid)) return false;

	const int32 X0 = FMath::Clamp(FMath::FloorToInt32(Grid.X), 0, Mirror.NumVertsX - 2);
	const int32 Y0 = FMath::Clamp(FMath::FloorToInt32(Grid.Y), 0, Mirror.NumVertsY - 2);
	const float FracX = FMath::Clamp(float(Grid.X - X0), 0.0f, 1.0f);
	const float FracY = FMath::Clamp(float(Grid.Y - Y0), 0.0f, 1.0f);

	auto Lerp2 = [FracX, FracY](float V00, float V10, float V01, float V11)
	{
		return FMath::Lerp(FMath::Lerp(V00, V10, FracX), FMath::Lerp(V01, V11, FracX), FracY);
	};

	const int32 I00 = Mirror.VertexIndex(X0, Y0);
	const int32 I10 = Mirror.VertexIndex(X0 + 1, Y0);
	const int32 I01 = Mirror.VertexIndex(X0, Y0 + 1);
	const int32 I11 = Mirror.VertexIndex(X0 + 1, Y0 + 1);

	OutHeight = Lerp2(Mirror.Heights[I00], Mirror.Heights[I10], Mirror.Heights[I01], Mirror.Heights[I11]);

	const FLinearColor C00 = CSGround_UnquantizeColor(Mirror.Colors[I00]);
	const FLinearColor C10 = CSGround_UnquantizeColor(Mirror.Colors[I10]);
	const FLinearColor C01 = CSGround_UnquantizeColor(Mirror.Colors[I01]);
	const FLinearColor C11 = CSGround_UnquantizeColor(Mirror.Colors[I11]);
	OutColor = FLinearColor(
		Lerp2(C00.R, C10.R, C01.R, C11.R),
		Lerp2(C00.G, C10.G, C01.G, C11.G),
		Lerp2(C00.B, C10.B, C01.B, C11.B),
		Lerp2(C00.A, C10.A, C01.A, C11.A));
	return true;
}

float ACSGroundActor::SampleHeight(FVector2D WorldXY) const
{
	float Height = 0.0f;
	FLinearColor Color;
	if (!SampleBilinear(WorldXY, Height, Color)) return GetActorLocation().Z;
	return GetActorLocation().Z + Height;
}

float ACSGroundActor::SampleRoadWeight(FVector2D WorldXY) const
{
	float Height = 0.0f;
	FLinearColor Color;
	if (!SampleBilinear(WorldXY, Height, Color)) return 0.0f;
	return Color.R;
}

FLinearColor ACSGroundActor::SampleColor(FVector2D WorldXY) const
{
	float Height = 0.0f;
	FLinearColor Color = FLinearColor::Black;
	SampleBilinear(WorldXY, Height, Color);
	return Color;
}

FBox2D ACSGroundActor::GetWorldRect2D() const
{
	const FVector Origin = GetActorLocation();
	const FVector2D Min(Origin.X, Origin.Y);
	if (!Mirror.IsInitialized()) return FBox2D(Min, Min);
	return FBox2D(Min, Min + FVector2D((Mirror.NumVertsX - 1) * Mirror.CellSize, (Mirror.NumVertsY - 1) * Mirror.CellSize));
}

bool ACSGroundActor::RaycastGround(const FVector& RayOrigin, const FVector& RayDirection, FVector& OutHit) const
{
	if (!Mirror.IsInitialized()) return false;

	const FVector Direction = RayDirection.GetSafeNormal();
	if (Direction.IsNearlyZero()) return false;

	const FBox2D Rect = GetWorldRect2D();
	const double GroundZ = GetActorLocation().Z;

	// 平地快路径：当前没有任何高度写入路径（MaxAbsHeight 恒 0），这条就是全部。
	if (MaxAbsHeight <= UE_KINDA_SMALL_NUMBER)
	{
		if (FMath::Abs(Direction.Z) < UE_DOUBLE_SMALL_NUMBER) return false;
		const double T = (GroundZ - RayOrigin.Z) / Direction.Z;
		if (T < 0.0) return false;
		const FVector Hit = RayOrigin + Direction * T;
		if (!Rect.IsInside(FVector2D(Hit.X, Hit.Y))) return false;
		OutHit = Hit;
		return true;
	}

	// 起伏路径：先裁到地面 AABB，再按半格步进找"射线穿到地表下"的符号翻转，翻转区间内
	// 线性插值一次收敛（半格步长下误差远小于格距）。
	const FBox GroundBox(
		FVector(Rect.Min.X, Rect.Min.Y, GroundZ - MaxAbsHeight - 1.0),
		FVector(Rect.Max.X, Rect.Max.Y, GroundZ + MaxAbsHeight + 1.0));
	FVector Entry = RayOrigin;
	if (!GroundBox.IsInsideOrOn(RayOrigin))
	{
		const FVector RayEnd = RayOrigin + Direction * double(HALF_WORLD_MAX);
		FVector HitNormal = FVector::UpVector;
		float HitTime = 0.0f;
		if (!FMath::LineExtentBoxIntersection(GroundBox, RayOrigin, RayEnd, FVector::ZeroVector, Entry, HitNormal, HitTime)) return false;
	}

	const double Step = Mirror.CellSize * 0.5;
	const double MaxDistance = GroundBox.GetSize().Size() + Step;
	double PrevT = 0.0;
	double PrevDelta = Entry.Z - double(SampleHeight(FVector2D(Entry.X, Entry.Y)));
	for (double T = Step; T <= MaxDistance; T += Step)
	{
		const FVector P = Entry + Direction * T;
		const double Delta = P.Z - double(SampleHeight(FVector2D(P.X, P.Y)));
		if (Delta <= 0.0 && PrevDelta > 0.0)
		{
			const double Alpha = PrevDelta / FMath::Max(PrevDelta - Delta, UE_DOUBLE_SMALL_NUMBER);
			const FVector Hit = Entry + Direction * FMath::Lerp(PrevT, T, Alpha);
			if (!Rect.IsInside(FVector2D(Hit.X, Hit.Y))) return false;
			OutHit = FVector(Hit.X, Hit.Y, SampleHeight(FVector2D(Hit.X, Hit.Y)));
			return true;
		}
		PrevT = T;
		PrevDelta = Delta;
	}
	return false;
}

void ACSGroundActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	if (IsTemplate() || !GetWorld()) return;
	ResolveShapers();
	RebuildGroundMesh();
}

void ACSGroundActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 显存交回渲染线程释放：在游戏线程上直接丢引用会把在途帧正在读的 buffer 抽走。
	CSGroundStairs::ReleaseOnRenderThread(StairBuffers);
	CSShaperSteps::ReleaseOnRenderThread(SkirtDecorGpuBuffers);
	Super::EndPlay(EndPlayReason);
}

void ACSGroundActor::Destroyed()
{
	if (IsValid(StairComponent)) StairComponent->ClearInstanceSourceGPU();
	// 石子那个组件也拿着同一批 pooled buffer 的引用：漏掉它，`ReleaseOnRenderThread` 交回去的
	// 就不是最后一份引用，显存要拖到组件自己被 GC 才放。
	if (IsValid(StairPebbleComponent)) StairPebbleComponent->ClearInstanceSourceGPU();
	CSGroundStairs::ReleaseOnRenderThread(StairBuffers);
	// 裙边摆件同理：每个 palette 的组件各拿着一份引用，全撤掉才轮得到 ReleaseOnRenderThread
	// 交回最后一份。
	for (const TObjectPtr<UCSGpuInstancedMeshComponent>& Component : SkirtDecorComponents)
	{
		if (IsValid(Component)) Component->ClearInstanceSourceGPU();
	}
	CSShaperSteps::ReleaseOnRenderThread(SkirtDecorGpuBuffers);
	Super::Destroyed();
}

// -----------------------------------------------------------------------------
// Shapers（计划 D9：高度的唯一权威在镜像，塑形物只是它的输入）
// -----------------------------------------------------------------------------

void ACSGroundActor::ResolveShapers()
{
	Shapers.Reset();
	if (!GetWorld()) return;
	for (TActorIterator<ACSGroundShaperActor> It(GetWorld()); It; ++It) Shapers.Add(*It);
}

void ACSGroundActor::RegisterShaper(ACSGroundShaperActor* Shaper)
{
	if (!Shaper) return;
	Shapers.RemoveAll([](const TWeakObjectPtr<ACSGroundShaperActor>& Weak) { return !Weak.IsValid(); });
	if (Shapers.Contains(Shaper)) return;   // 已登记：区域更新由塑形物自己发，别在这里再跑一次全域
	Shapers.Add(Shaper);
	RebuildHeightsFromShapers();
	// ⚠️ **不能只靠上面那句**：`RefreshHeightsInRegion` 有一道 `if (!bChanged) return`，而
	// 加载期高度早就随关卡序列化好了 ⇒ 登记一座塑形物**不会**改变任何高度 ⇒ 那条早退成立，
	// 裙边摆件于是永远停在"零座塑形物"的那一版。症状是"加载后土台在、摆件不在，手点一次
	// RebuildGroundMesh 就出来了"。裙边摆件的输入是**塑形物集合本身**，不是高度差。
	RebuildSkirtDecor();
}

void ACSGroundActor::UnregisterShaper(ACSGroundShaperActor* Shaper)
{
	Shapers.RemoveAll([Shaper](const TWeakObjectPtr<ACSGroundShaperActor>& Weak) { return !Weak.IsValid() || Weak.Get() == Shaper; });
	// 同上：注销这一侧连高度重导出都不发（那是塑形物自己 `UnregisterFromGround` 的活），
	// 少了这一句，删掉塑形物之后那一圈摆件会留在原地飘着。
	RebuildSkirtDecor();
}

void ACSGroundActor::BuildShaperGpuParams(TArray<FVector4f>& OutParams) const
{
	OutParams.Reset();
	for (const TWeakObjectPtr<ACSGroundShaperActor>& Weak : Shapers)
	{
		const ACSGroundShaperActor* Shaper = Weak.Get();
		if (!Shaper) continue;
		FVector4f Profile, Top, Noise;
		Shaper->GetHeightFieldParams(Profile, Top, Noise);
		OutParams.Add(Profile);
		OutParams.Add(Top);
		OutParams.Add(Noise);
	}
}

void ACSGroundActor::RebuildHeightsFromShapers()
{
	RefreshHeightsInRegion(GetWorldRect2D());
}

void ACSGroundActor::RefreshHeightsInRegion(const FBox2D& WorldRectXY)
{
	EnsureMirrorInitialized();
	if (!Mirror.IsInitialized()) return;

	const FVector Origin = GetActorLocation();

	// 世界矩形 → 格点闭区间，向外取整（同 ApplyPaintToMirror 的 Floor/Ceil，不能用 WorldToGrid：
	// 它把出界点夹回边界，会把区域悄悄缩掉）。
	const int32 X0 = FMath::Clamp(FMath::FloorToInt32((WorldRectXY.Min.X - Origin.X) / Mirror.CellSize), 0, Mirror.NumVertsX - 1);
	const int32 Y0 = FMath::Clamp(FMath::FloorToInt32((WorldRectXY.Min.Y - Origin.Y) / Mirror.CellSize), 0, Mirror.NumVertsY - 1);
	const int32 X1 = FMath::Clamp(FMath::CeilToInt32((WorldRectXY.Max.X - Origin.X) / Mirror.CellSize), X0, Mirror.NumVertsX - 1);
	const int32 Y1 = FMath::Clamp(FMath::CeilToInt32((WorldRectXY.Max.Y - Origin.Y) / Mirror.CellSize), Y0, Mirror.NumVertsY - 1);

	// 镜像（CPU 权威，供 SampleHeight/拾取/房子落座查询）。公式是绝对式的"基底 0 与全部
	// 塑形物取 max"，所以在区域内重算与全量重算逐位相同 —— 撤掉旧隆起靠的是重算，不是减法。
	bool bChanged = false;
	float NewMaxAbsHeight = 0.0f;
	for (int32 Y = Y0; Y <= Y1; ++Y)
	{
		for (int32 X = X0; X <= X1; ++X)
		{
			const FVector2D World(Origin.X + X * Mirror.CellSize, Origin.Y + Y * Mirror.CellSize);
			float Height = 0.0f;
			for (const TWeakObjectPtr<ACSGroundShaperActor>& Weak : Shapers)
			{
				if (const ACSGroundShaperActor* Shaper = Weak.Get()) Height = FMath::Max(Height, Shaper->SampleShapeHeight(World));
			}
			float& Slot = Mirror.Heights[Mirror.VertexIndex(X, Y)];
			if (FMath::IsNearlyEqual(Slot, Height, 0.01f)) continue;
			Slot = Height;
			bChanged = true;
		}
	}
	if (!bChanged) return;   // 幂等短路：加载后重导出结果与序列化值一致时不重建、不标脏

	// 拾取的平面/march 分路看的是全局上界；高度恒 = max(0, 各塑形物台高)，直接从参数取，
	// 不用扫全表（区域更新的意义就在于不碰区域外的格点）。
	for (const TWeakObjectPtr<ACSGroundShaperActor>& Weak : Shapers)
	{
		const ACSGroundShaperActor* Shaper = Weak.Get();
		if (!Shaper) continue;
		FVector4f Profile, Top, Noise;
		Shaper->GetHeightFieldParams(Profile, Top, Noise);
		// 上界要取**峰值**：二次抬升在台顶又加了一档，拿 Top.X 当上界会让拾取的平面/march
		// 分路在台顶附近判错，而裙边噪声只会往下啃、不抬高，所以不用为它留余量。
		NewMaxAbsHeight = FMath::Max(NewMaxAbsHeight, CSGroundShaperField::PeakHeight(Top));
	}
	MaxAbsHeight = NewMaxAbsHeight;
	MarkPackageDirty();

	// GPU 投影：一个 compute pass 原地改 Z 与法线。网格还没建、或 actor 被拖过导致常驻流
	// （世界空间）与当前位置脱节时，退回全量重建对齐。
	if (!TinyGladeMesh || !MeshBuiltAtLocation.Equals(Origin))
	{
		RebuildGroundMesh();
		return;
	}

	TArray<FVector4f> ShaperParams;
	BuildShaperGpuParams(ShaperParams);
	++GpuDisplaceCount;
	UCSMeshOps::DisplaceGroundShapers(
		TinyGladeMesh, ShaperParams,
		FVector2f(float(Origin.X), float(Origin.Y)), Mirror.CellSize, float(Origin.Z),
		Mirror.NumVertsX, Mirror.NumVertsY, FIntPoint(X0, Y0), FIntPoint(X1, Y1));

	const FBox ChangedBox(
		FVector(Origin.X + X0 * Mirror.CellSize, Origin.Y + Y0 * Mirror.CellSize, Origin.Z - MaxAbsHeight - 1.0),
		FVector(Origin.X + X1 * Mirror.CellSize, Origin.Y + Y1 * Mirror.CellSize, Origin.Z + MaxAbsHeight + 1.0));
	OnGroundChanged.Broadcast(this, ChangedBox);

	// 高度场变了 ⇒ 等值线变了。石阶不订阅 OnGroundChanged（它就归本 actor，订阅自己的广播
	// 是个自环），直接排在广播之后重扫一遍。
	RebuildStairs();
	// 岩壳与地面位移**同一趟**（计划 D9 链 B 的接线）：两者读同一份 GroundShaperParams，
	// 排在 GPU 位移之后就天然同帧一致，不需要任何跨 actor 的同步。
	RebuildRockShell();
	// 裙边摆件是第三条同源派生链：高度场一动，那一圈锚点的落高与"被邻座埋了没有"都要重判。
	RebuildSkirtDecor();
}

// -----------------------------------------------------------------------------
// Stairs（计划「石阶改造：100% GPU 决策 + 零回读」的 S1/S2/S3）
//
// **全项目唯一的一条石阶路**：塑形物自持的旧路（`ACSGroundShaperActor::RebuildSteps` +
// `CSShaperSteps` + `CSGroundSteps.usf`）已随 2026-08-30「裁决一」整条删除。
// 这条路上 CPU 不接触单个台阶：层 / 弧段 / 摆位全部由 GPU 从合成后的高度场推导。
// -----------------------------------------------------------------------------

bool ACSGroundActor::EnsureStairComponent()
{
	// 关掉石阶：连组件带显存一起收掉。留着一个空组件只会在 details 面板里留个误导性的槽位。
	// 石子是石阶的从属支线（它只在摆出一级台阶的那一段等值线上抽签），所以跟着一起收 ——
	// 单独留着它会得到一地没有台阶的石头。
	if (!StairMesh)
	{
		if (IsValid(StairComponent))
		{
			StairComponent->ClearInstanceSourceGPU();
			StairComponent->DestroyComponent();
		}
		StairComponent = nullptr;
		if (IsValid(StairPebbleComponent))
		{
			StairPebbleComponent->ClearInstanceSourceGPU();
			StairPebbleComponent->DestroyComponent();
		}
		StairPebbleComponent = nullptr;
		CSGroundStairs::ReleaseOnRenderThread(StairBuffers);
		HandedStairCapacity = 0;
		HandedStairBounds = FBox(ForceInit);
		HandedPebbleCapacity = 0;
		HandedPebbleBounds = FBox(ForceInit);
		return false;
	}

	// 蓝图 actor 重跑构造脚本会把实例组件销毁，指针会失效 —— 先判有效再复用。
	if (!IsValid(StairComponent))
	{
		StairComponent = NewObject<UCSGpuInstancedMeshComponent>(this, NAME_None, RF_Transient);
		StairComponent->SetupAttachment(RootComponent);
		StairComponent->RegisterComponent();   // 未注册时任何变更都会释放 GPU 网格，必须先注册再喂
	}
	StairComponent->InstanceMaterial = StairMaterial;
	StairComponent->SetBaseMesh(StairMesh);    // 同一张网格时内部直接早退

	// 从网格推导三个量：包围球（剔除用）、块缩放、抬升。
	const FBox Local = StairMesh->GetBoundingBox();
	StairBaseSphereCentre = Local.IsValid ? FVector3f(Local.GetCenter()) : FVector3f::ZeroVector;
	StairBaseSphereRadius = Local.IsValid ? float(Local.GetExtent().Size()) : 0.0f;

	// 某一轴填 0（或网格该轴退化）就保持网格自带的尺寸 —— 喂"真实尺寸的石阶网格"与喂
	// "居中单位立方体字典 mesh"走的是同一条式子，不需要两套口径。
	const FVector Size = Local.IsValid ? Local.GetSize() : FVector::OneVector;
	auto AxisScale = [](double Want, double Have) -> float
	{
		return (Want > UE_KINDA_SMALL_NUMBER && Have > UE_KINDA_SMALL_NUMBER) ? float(Want / Have) : 1.0f;
	};
	StairBlockScale = FVector3f(
		AxisScale(StairBlockSize.X, Size.X),
		AxisScale(StairBlockSize.Y, Size.Y),
		AxisScale(StairBlockSize.Z, Size.Z));
	// S2 的长度轴跟着弦长走（世界 cm），kernel 需要网格自身的 Y 尺寸才能换算回缩放。
	StairBaseSizeY = (Local.IsValid && Size.Y > UE_KINDA_SMALL_NUMBER) ? float(Size.Y) : 1.0f;

	// 半个身位抬升：盒心直接落在等值线上的话石块一半埋在地里（原型如此，旧路的实测修正）。
	StairRise = Local.IsValid ? float(-Local.Min.Z) * StairBlockScale.Z : 0.0f;

	// ---- 小石子（TG 的 15% 支线）：同样从网格推导，网格为空则缩放归零 = 这一支关掉 ----
	// 归零而不是"跳过绑定"：石子的 UAV 在 RDG 里是无条件绑定的，真正的开关只有 `PebbleChance`
	// 与这里的缩放；留一个非零缩放配空网格，画出来的是一地引擎默认球。
	if (StairPebbleMesh)
	{
		if (!IsValid(StairPebbleComponent))
		{
			StairPebbleComponent = NewObject<UCSGpuInstancedMeshComponent>(this, NAME_None, RF_Transient);
			StairPebbleComponent->SetupAttachment(RootComponent);
			StairPebbleComponent->RegisterComponent();
		}
		StairPebbleComponent->InstanceMaterial = StairPebbleMaterial;
		StairPebbleComponent->SetBaseMesh(StairPebbleMesh);

		const FBox PebbleLocal = StairPebbleMesh->GetBoundingBox();
		StairPebbleSphereCentre = PebbleLocal.IsValid ? FVector3f(PebbleLocal.GetCenter()) : FVector3f::ZeroVector;
		StairPebbleSphereRadius = PebbleLocal.IsValid ? float(PebbleLocal.GetExtent().Size()) : 0.0f;
		// 均匀缩放，所以换算基准取网格**最长**的那一轴：石子是团块，用哪一轴都行，
		// 取最长轴才让 `StairPebbleSize` 读作"这颗石头最大有多大"而不是某个看不见的轴。
		const FVector PebbleSize = PebbleLocal.IsValid ? PebbleLocal.GetSize() : FVector::OneVector;
		const double PebbleLongest = FMath::Max3(PebbleSize.X, PebbleSize.Y, PebbleSize.Z);
		const double Lo = FMath::Max(FMath::Min(StairPebbleSize.X, StairPebbleSize.Y), 0.0);
		const double Hi = FMath::Max(StairPebbleSize.X, StairPebbleSize.Y);
		StairPebbleScaleMin = (PebbleLongest > UE_KINDA_SMALL_NUMBER) ? float(Lo / PebbleLongest) : 0.0f;
		StairPebbleScaleMax = (PebbleLongest > UE_KINDA_SMALL_NUMBER) ? float(Hi / PebbleLongest) : 0.0f;
	}
	else
	{
		if (IsValid(StairPebbleComponent))
		{
			StairPebbleComponent->ClearInstanceSourceGPU();
			StairPebbleComponent->DestroyComponent();
		}
		StairPebbleComponent = nullptr;
		HandedPebbleCapacity = 0;
		HandedPebbleBounds = FBox(ForceInit);
		StairPebbleSphereCentre = FVector3f::ZeroVector;
		StairPebbleSphereRadius = 0.0f;
		StairPebbleScaleMin = 0.0f;
		StairPebbleScaleMax = 0.0f;
	}
	return true;
}

void ACSGroundActor::RebuildStairs()
{
	if (IsTemplate() || !GetWorld()) return;
	if (!EnsureStairComponent()) return;
	if (!Mirror.IsInitialized() || !TinyGladeMesh) return;

	const FCSMeshResidentRef Resident = TinyGladeMesh->GetResident();
	if (!Resident.IsValid()) return;   // 网格还没分配：地面重建完会再走一次这条路

	// 固定容量：只在配置真的变了时分配（一次阻塞）。交互期（画笔刷、拖塑形物）永远走
	// "已经够大"的零成本分支 —— 这正是把已删旧路那种"每次重排记录都重算需要多少格、
	// 于是逐笔扩容"的做法换掉之后拿到的东西。
	// 石子的容量**从石阶容量推导，不另开一个属性**：它是 15% 的伯努利抽样，N = 4096 时均值 614、
	// σ ≈ 23，取一半（2048）是 60 σ 以上的余量，而且谁调大 MaxStairInstances 石子就跟着涨 ——
	// 两个各自可调的容量迟早会配错，症状是"石阶好好的，石子在某个尺寸以上开始缺"，没人查得到。
	const uint32 StairCapacity = uint32(FMath::Max(MaxStairInstances, 64));
	if (!CSGroundStairs::EnsureBuffers(StairBuffers, StairCapacity, FMath::Max(StairCapacity / 2u, 64u))) return;

	const FVector Origin = GetActorLocation();
	const FBox2D Rect = GetWorldRect2D();
	const FVector2D Span = Rect.GetSize();
	const float Cell = FMath::Max(StairCellSize, 10.0f);

	CSGroundStairs::FScanParams Params;
	// 组件挂在根上、相对变换为单位阵 ⇒ 组件空间就是 actor 空间。
	Params.WorldToComponent = FMatrix44f(GetActorTransform().ToInverseMatrixWithScale());
	Params.GridOriginXY = FVector2f(float(Rect.Min.X), float(Rect.Min.Y));
	Params.CellSize = Cell;
	Params.GridDims = FIntPoint(
		FMath::Max(FMath::CeilToInt32(Span.X / Cell), 1),
		FMath::Max(FMath::CeilToInt32(Span.Y / Cell), 1));
	Params.GroundOriginXY = FVector2f(float(Origin.X), float(Origin.Y));
	Params.GroundCellSize = Mirror.CellSize;
	Params.GroundVerts = FIntPoint(Mirror.NumVertsX, Mirror.NumVertsY);
	Params.GroundBaseZ = float(Origin.Z);
	Params.StepHeight = StairStepHeight;
	Params.RoadThreshold = StairRoadThreshold;
	Params.Embed = StairEmbed;
	Params.Rise = StairRise;
	Params.ZOffset = StairZOffset;
	Params.MaxLayersPerCell = uint32(FMath::Clamp(StairMaxLayersPerCell, 1, 256));
	Params.BaseSphereCentre = StairBaseSphereCentre;
	Params.BaseSphereRadius = StairBaseSphereRadius;
	Params.BlockSize = StairBlockScale;
	Params.BaseSizeY = StairBaseSizeY;
	Params.LengthBloat = FMath::Max(StairLengthBloat, 1.0f);
	Params.LengthJitter = FMath::Max(StairLengthJitter, 0.0f);
	Params.SizeJitter = FMath::Clamp(StairSizeJitter, 0.0f, 0.5f);
	Params.YawJitterRad = FMath::DegreesToRadians(FMath::Max(StairYawJitter, 0.0f));
	Params.JitterSeed = uint32(StairJitterSeed);
	// 网格缺席时概率直接归零：kernel 于是一个字节都不写，counter 被 clear 归零，
	// 上一趟的石子当场消失 —— 不需要任何"注销"路径（同岩壳裁决二的形状）。
	Params.PebbleChance = StairPebbleMesh ? FMath::Clamp(StairPebbleChance, 0.0f, 1.0f) : 0.0f;
	Params.PebbleScaleMin = StairPebbleScaleMin;
	Params.PebbleScaleMax = StairPebbleScaleMax;
	Params.PebbleSphereCentre = StairPebbleSphereCentre;
	Params.PebbleSphereRadius = StairPebbleSphereRadius;

	TArray<FVector4f> ShaperParams;
	BuildShaperGpuParams(ShaperParams);
	if (!CSGroundStairs::Scan(Resident, StairBuffers, Params, ShaperParams)) return;

	// **包围盒不能靠实例算**（只有 GPU 知道摆了哪些），所以按"地面矩形 × MaxAbsHeight"写死 ——
	// 与岩壳用 NaN 顶点污染包围盒是同一条处置。组件空间的保守盒，且要自己把基础网格的尺寸
	// 算进去（packed 路径不代劳）。
	// 保守包围盒的最坏缩放。**只能由配置算出来，一个实例都不能读** —— 一旦掺进实例数据，
	// 包围盒就会随落笔漂移，`bNeedHandover` 每 dab 都成立，阻塞的 SetInstanceSourceGPU
	// 会把"交互期零阻塞"整条纪律退化掉（这正是拉尺寸那一轮踩过的坑）。
	// 长度轴的最坏值 = 最长可能弦长（格对角线 √2 × 格距）× 胀大 × (1 + 单侧抖动)。
	const float WorstLenWorld = FMath::Max(Cell * UE_SQRT_2, StairBlockScale.Y * StairBaseSizeY)
		* FMath::Max(StairLengthBloat, 1.0f) * (1.0f + FMath::Max(StairLengthJitter, 0.0f));
	const float WorstScaleY = (StairBaseSizeY > UE_KINDA_SMALL_NUMBER) ? (WorstLenWorld / StairBaseSizeY) : StairBlockScale.Y;
	const float WorstScaleXZ = FMath::Max(StairBlockScale.X, StairBlockScale.Z)
		* (1.0f + FMath::Clamp(StairSizeJitter, 0.0f, 0.5f));
	const double Reach = double(StairBaseSphereRadius) * double(FMath::Max(WorstScaleY, WorstScaleXZ));
	const FVector LocalMin = GetActorTransform().InverseTransformPosition(
		FVector(Rect.Min.X, Rect.Min.Y, Origin.Z - MaxAbsHeight)) - FVector(Reach + FMath::Abs(StairEmbed));
	const FVector LocalMax = GetActorTransform().InverseTransformPosition(
		FVector(Rect.Max.X, Rect.Max.Y, Origin.Z + MaxAbsHeight)) + FVector(Reach + FMath::Abs(StairEmbed));
	const FBox LocalBounds(LocalMin.ComponentMin(LocalMax), LocalMin.ComponentMax(LocalMax));

	// 交接是阻塞的（内部 SetStreamLayoutSync/ResizeStreamsSync + 立刻重建 render state）。
	// 容量固定、包围盒只跟地面尺寸与 MaxAbsHeight 走，所以画路那种高频路径上它永远不触发。
	// 组件自己的状态要一起看：蓝图重跑构造脚本会重建组件，新组件身上没有实例源，只看缓存
	// 就会永远画不出东西。
	const bool bNeedHandover =
		HandedStairCapacity != StairBuffers.Capacity
		|| !HandedStairBounds.IsValid
		|| !HandedStairBounds.Min.Equals(LocalBounds.Min, 1.0)
		|| !HandedStairBounds.Max.Equals(LocalBounds.Max, 1.0)
		|| !StairComponent->HasInstanceSourceGPU();
	if (bNeedHandover)
	{
		FCSGpuInstanceSourceGPU Source;
		Source.PackedInstances = StairBuffers.PackedInstances;   // 保留自己的引用，重扫还要用
		Source.Counter = StairBuffers.Counter;
		Source.Capacity = StairBuffers.Capacity;
		Source.LocalBounds = LocalBounds;
		StairComponent->SetInstanceSourceGPU(Source);
		HandedStairCapacity = StairBuffers.Capacity;
		HandedStairBounds = LocalBounds;
		// 这一行是阻塞的那一趟的唯一痕迹。稳态下它一次都不该打 —— 画路时反复出现就说明
		// 上面某个"没变"的判据其实每次都在变，那正是交互期掉帧的来源。
		UE_LOG(LogTinyGladeGround, Log, TEXT("[TinyGladeGround] %s stair instance source handed over (capacity=%u)"),
			*GetName(), StairBuffers.Capacity);
	}

	// 石子的交接是石阶那条的逐字副本，判据也必须逐字相同（容量固定 + 包围盒只由配置算）——
	// 石子的包围盒能直接沿用石阶那个保守盒：石子落在同一条等值线上、缩放比石阶小一个量级，
	// 它伸不出石阶已经算进去的那圈余量。共用一个盒子还顺带保证两条交接同时稳态、同时触发。
	if (IsValid(StairPebbleComponent))
	{
		const bool bNeedPebbleHandover =
			HandedPebbleCapacity != StairBuffers.PebbleCapacity
			|| !HandedPebbleBounds.IsValid
			|| !HandedPebbleBounds.Min.Equals(LocalBounds.Min, 1.0)
			|| !HandedPebbleBounds.Max.Equals(LocalBounds.Max, 1.0)
			|| !StairPebbleComponent->HasInstanceSourceGPU();
		if (bNeedPebbleHandover)
		{
			FCSGpuInstanceSourceGPU PebbleSource;
			PebbleSource.PackedInstances = StairBuffers.PebbleInstances;
			PebbleSource.Counter = StairBuffers.PebbleCounter;
			PebbleSource.Capacity = StairBuffers.PebbleCapacity;
			PebbleSource.LocalBounds = LocalBounds;
			StairPebbleComponent->SetInstanceSourceGPU(PebbleSource);
			HandedPebbleCapacity = StairBuffers.PebbleCapacity;
			HandedPebbleBounds = LocalBounds;
			UE_LOG(LogTinyGladeGround, Log, TEXT("[TinyGladeGround] %s stair pebble instance source handed over (capacity=%u)"),
				*GetName(), StairBuffers.PebbleCapacity);
		}
	}
}

int32 ACSGroundActor::DebugReadStairRowsSync(TArray<FVector4f>& OutRows)
{
	OutRows.Reset();
	if (!StairBuffers.IsValid()) return 0;
	return CSGroundStairs::DebugReadInstancesSync(StairBuffers, nullptr, &OutRows);
}

int32 ACSGroundActor::DebugReadStairsSync(TArray<FVector>& OutWorldOrigins)
{
	OutWorldOrigins.Reset();
	if (!StairBuffers.IsValid()) return 0;

	TArray<FVector> LocalOrigins;
	const int32 Count = CSGroundStairs::DebugReadInstancesSync(StairBuffers, &LocalOrigins);
	const FTransform ToWorld = GetActorTransform();
	OutWorldOrigins.Reserve(LocalOrigins.Num());
	for (const FVector& Local : LocalOrigins) OutWorldOrigins.Add(ToWorld.TransformPosition(Local));
	return Count;
}

int32 ACSGroundActor::DebugReadStairPebbleRowsSync(TArray<FVector4f>& OutRows)
{
	OutRows.Reset();
	if (!StairBuffers.HasPebbles()) return 0;
	return CSGroundStairs::DebugReadInstancesSync(StairBuffers, nullptr, &OutRows, /*bPebbles=*/true);
}

int32 ACSGroundActor::DebugReadStairPebblesSync(TArray<FVector>& OutWorldOrigins)
{
	OutWorldOrigins.Reset();
	if (!StairBuffers.HasPebbles()) return 0;

	TArray<FVector> LocalOrigins;
	const int32 Count = CSGroundStairs::DebugReadInstancesSync(StairBuffers, &LocalOrigins, nullptr, /*bPebbles=*/true);
	const FTransform ToWorld = GetActorTransform();
	OutWorldOrigins.Reserve(LocalOrigins.Num());
	for (const FVector& Local : LocalOrigins) OutWorldOrigins.Add(ToWorld.TransformPosition(Local));
	return Count;
}

// -----------------------------------------------------------------------------
// GPU 侧真值（诊断 / 验收专用，阻塞）—— 声明处那段注释是这几条为什么存在
// -----------------------------------------------------------------------------

int32 ACSGroundActor::DebugReadStairCountGpuSync() const
{
	// 组件不在（石阶关着 / 蓝图重跑了构造脚本）时读到的就是 0：那一路确实一个实例都不画。
	return IsValid(StairComponent) ? StairComponent->DebugReadDrawnInstanceCountSync() : 0;
}

int32 ACSGroundActor::DebugReadStairPebbleCountGpuSync() const
{
	return IsValid(StairPebbleComponent) ? StairPebbleComponent->DebugReadDrawnInstanceCountSync() : 0;
}

int32 ACSGroundActor::DebugReadRockShellDrawIndexCountGpuSync() const
{
	if (!RockShellMesh) return 0;
	const FCSMeshResidentRef Resident = RockShellMesh->GetResident();
	if (!Resident.IsValid() || !Resident->IsAllocated()) return 0;

	const FCSMeshResident::FStream* Args = Resident->FindStream(ECSGpuStreamRole::IndirectArgs);
	if (!Args || !Args->Pooled.IsValid()) return -1;

	// DrawIndexedIndirect 的第 0 组参数，[0] = IndexCountPerInstance —— 绘制真正消费的就是它。
	// 读完必须放回 ERHIAccess::IndirectArgs（`FinalAccessForRole` 那张表），否则下一次绘制会
	// 在 CopySrc 状态上撞见这个 buffer。
	TArray<uint32> Values;
	if (!CSMeshReadback::ReadUintBufferSync(Args->Pooled, 1u, ECSGpuStreamRole::IndirectArgs, Values) || Values.IsEmpty())
	{
		return -1;
	}
	return int32(FMath::Min<uint32>(Values[0], uint32(MAX_int32)));
}

FString ACSGroundActor::DebugGetGpuAssetMismatchSync() const
{
	auto Check = [](const UCSGpuInstancedMeshComponent* Component, const TCHAR* Label) -> FString
	{
		// 没有组件不算"画错了"——那是 `IsRockShellDrawable` 那一族的职责范围，
		// 这里只答"画的是不是那个"。
		if (!IsValid(Component)) return FString();
		const FString Reason = Component->DebugGetDrawnAssetMismatchSync();
		return Reason.IsEmpty() ? FString() : FString::Printf(TEXT("%s：%s"), Label, *Reason);
	};

	FString Reason = Check(StairComponent, TEXT("石阶"));
	if (!Reason.IsEmpty()) return Reason;
	Reason = Check(StairPebbleComponent, TEXT("石子"));
	return Reason;
}

// -----------------------------------------------------------------------------
// Rock Shell（计划 D9「侧面碎石：Tiny Glade 式披挂岩壳」的链 B）
//
// 裁决二：碎石归**地面**，不归塑形物 —— 图案覆盖整张地面、与任何单座塑形物无关，
// mask 由全部塑形物合成后的坡度决定。删掉塑形物 → 高度场塌回 → 坡度降到阈下 →
// 那批胞腔自己写 NaN，**归属簿记整个消失**，没有一行注销代码。
// -----------------------------------------------------------------------------

uint32 ACSGroundActor::RockShellInputHash() const
{
	// float 走位模式哈希：GetTypeHash(float) 对 −0.0 / NaN 的处理不是我们要的语义，而这里
	// 只需要"变了没有"。
	auto HashFloat = [](uint32 Seed, float Value) { return HashCombine(Seed, *reinterpret_cast<const uint32*>(&Value)); };

	uint32 Hash = ::GetTypeHash(bRockShell);
	// 落笔计数：壳在路上是连续下沉（裁决五），下沉量由道路权重驱动 ⇒ 画一笔就得重披挂。
	Hash = HashCombine(Hash, ::GetTypeHash(PaintRevision));
	Hash = HashCombine(Hash, GetTypeHash(RockShellPatternMesh.ToSoftObjectPath()));
	Hash = HashCombine(Hash, ::GetTypeHash(RockShellMaterial.Get()));
	Hash = HashCombine(Hash, ::GetTypeHash(Mirror.NumVertsX));
	Hash = HashCombine(Hash, ::GetTypeHash(Mirror.NumVertsY));
	Hash = HashFloat(Hash, Mirror.CellSize);
	const FVector Origin = GetActorLocation();
	Hash = HashFloat(Hash, float(Origin.X));
	Hash = HashFloat(Hash, float(Origin.Y));
	Hash = HashFloat(Hash, float(Origin.Z));
	Hash = HashFloat(Hash, RockShellPatternScale);
	Hash = HashFloat(Hash, RockShellSlopeLo);
	Hash = HashFloat(Hash, RockShellSlopeHi);
	Hash = HashFloat(Hash, RockShellRoadFade);
	Hash = HashFloat(Hash, RockShellRoadSink);
	Hash = HashFloat(Hash, RockShellCellJitter);
	Hash = HashFloat(Hash, RockShellCellRelief);
	Hash = HashFloat(Hash, RockShellNoiseAmount);
	Hash = HashFloat(Hash, RockShellNoiseWavelength);
	Hash = HashCombine(Hash, ::GetTypeHash(RockShellSeed));

	// 塑形物：哈希的是**打包后的高度场参数**而不是 actor 指针 —— 拖动一座塑形物时指针不变、
	// 参数变，而壳恰恰要跟着参数走。顺序敏感是对的：登记顺序变了合成结果不变，但重跑一趟
	// 披挂的代价只有一个 dispatch，远小于为它维护一份顺序无关哈希的复杂度。
	for (const TWeakObjectPtr<ACSGroundShaperActor>& Weak : Shapers)
	{
		const ACSGroundShaperActor* Shaper = Weak.Get();
		if (!Shaper) continue;
		FVector4f Profile, Top, Noise;
		Shaper->GetHeightFieldParams(Profile, Top, Noise);
		for (const FVector4f& Packed : { Profile, Top, Noise })
		{
			Hash = HashFloat(Hash, Packed.X);
			Hash = HashFloat(Hash, Packed.Y);
			Hash = HashFloat(Hash, Packed.Z);
			Hash = HashFloat(Hash, Packed.W);
		}
	}
	return Hash;
}

bool ACSGroundActor::EnsureRockShellMesh()
{
	// 关掉岩壳：连组件带显存一起收掉。留着一个空组件只会在 details 面板里留个误导性的槽位。
	if (!bRockShell)
	{
		if (IsValid(RockShellComponent))
		{
			RockShellComponent->SetGpuMesh(nullptr);
			RockShellComponent->DestroyComponent();
		}
		RockShellComponent = nullptr;
		if (RockShellMesh) RockShellMesh->ReleaseSync();
		RockShellMesh = nullptr;
		RockShellBuiltPattern = nullptr;
		RockShellBuiltRect = FBox2D(ForceInit);
		RockShellBuiltScale = 0.0f;
		return false;
	}
	if (!Mirror.IsInitialized()) return false;

	UStaticMesh* PatternAsset = RockShellPatternMesh.LoadSynchronous();
	if (!PatternAsset)
	{
		UE_LOG(LogTinyGladeGround, Warning,
			TEXT("[TinyGladeGround] %s 的岩壳图案资产 %s 加载不到 —— 先跑 Scripts/TinyGladeImportRockShell.py。"),
			*GetName(), *RockShellPatternMesh.ToString());
		return false;
	}
	const CSRockShell::FPattern& Pattern = CSRockShell::GetSharedPattern(PatternAsset);
	if (!Pattern.IsValid()) return false;   // 抽取失败的具体原因已由 CSRockShell 侧写进日志

	// 蓝图 actor 重跑构造脚本会把组件销毁，指针会失效 —— 先判有效再复用。
	if (!IsValid(RockShellComponent))
	{
		RockShellComponent = NewObject<UCSMeshRenderComponent>(this, TEXT("RockShellMesh"), RF_Transient);
		RockShellComponent->SetupAttachment(RootComponent);
		RockShellComponent->RegisterComponent();   // 未注册时任何变更都会释放 GPU 网格
	}
	RockShellComponent->MeshMaterial = RockShellMaterial;

	const FBox2D Rect = GetWorldRect2D();
	const float Scale = FMath::Clamp(RockShellPatternScale, 0.05f, 4.0f);
	// **建壳是阻塞的**（声明流集 / 分配 / 上传各 flush 一次），所以只在"图案或地面矩形真的
	// 变了"时走。交互期（画笔刷、拖塑形物）永远命中下面这条零成本早退。
	const bool bBuilt = RockShellMesh != nullptr
		&& RockShellBuiltPattern.Get() == PatternAsset
		&& RockShellBuiltRect.bIsValid
		&& RockShellBuiltRect.Min.Equals(Rect.Min, 1.0)
		&& RockShellBuiltRect.Max.Equals(Rect.Max, 1.0)
		&& FMath::IsNearlyEqual(RockShellBuiltScale, Scale)
		&& RockShellComponent->GetGpuMesh() == RockShellMesh;
	if (bBuilt) return true;

	// ⚠️ 缩小图案 = 地面边上一圈**无壳且无任何提示**。原件 tile 136.5 m 只在 Scale=1 时盖得住
	// 128 m 的地面；Scale=0.5 只盖 68 m，剩下的地面裸着，而 clamp 的下界是 0.05 —— 从属性面板
	// 上完全看不出这里有个悬崖。平铺救不回来（实测 tile 两侧边界点不一致、不是周期的，接缝会露），
	// 所以这里只报警不改行为：clamp 与几何一个字都不动，让用户自己决定要不要那圈裸地。
	// 只在真的重建时报（上面那条早退吃掉了交互期的每一帧），不会刷屏。
	//
	// ⚠️ 判据是地面的**边长**不是对角线：图案与地面都是轴对齐、同心的矩形，
	// 覆盖条件就是逐轴的 `图案跨度 ≥ 地面跨度`。拿对角线当尺子会在**原生口径**
	// （136.5 m 图案 × 128 m 地面，文档明写"每边富余 4.25 m"）上报红 ——
	// 对角线 181 m 永远大于 136.5 m，那条警告会在正确配置下天天响，
	// 而天天响的警告等于没有警告。
	{
		const FVector2f PatternSpan = Pattern.BoundsMax - Pattern.BoundsMin;
		const double Covered = double(FMath::Max(PatternSpan.X, PatternSpan.Y)) * double(Scale);
		const FVector2D GroundSpan = Rect.GetSize();
		const double Needed = FMath::Max(GroundSpan.X, GroundSpan.Y);
		if (Covered < Needed)
		{
			UE_LOG(LogTinyGladeGround, Warning,
				TEXT("[TinyGladeGround] %s rock shell pattern covers only %.1f m at scale %.3g, but the ground is %.1f m across")
				TEXT(" — everything outside the middle %.1f m gets no shell at all, silently.")
				TEXT(" The tile is not periodic, so tiling cannot fix it: raise RockShellPatternScale back to 1.0")
				TEXT(" (the native 136.5 m tile) or shrink the ground."),
				*GetName(), Covered * 0.01, Scale, Needed * 0.01, Covered * 0.01);
		}
	}

	if (!RockShellMesh) RockShellMesh = NewObject<UCSMesh>(this);

	// **包围盒按地面矩形写死**（kernel 用 NaN 关掉看不见的三角，NaN 会污染任何从顶点算出来
	// 的包围盒）。Z 的半高刻意**只由地面尺寸推导、不看 MaxAbsHeight** —— 掺进塑形物状态的话
	// 拖一座塑形物就会改变包围盒 ⇒ 每帧重走这条阻塞的建壳路径，那正是拉尺寸那一轮踩过的坑。
	const FVector Origin = GetActorLocation();
	const FVector2D Span = Rect.GetSize();
	const double ZHalf = FMath::Max(2000.0, FMath::Max(Span.X, Span.Y) * 0.25);
	const FBox Hard(
		FVector(Rect.Min.X, Rect.Min.Y, Origin.Z - ZHalf),
		FVector(Rect.Max.X, Rect.Max.Y, Origin.Z + ZHalf));
	const FVector2D Centre2D = Rect.GetCenter();

	if (!CSRockShell::BuildMesh(RockShellMesh, Pattern, Hard,
		FVector2f(float(Centre2D.X), float(Centre2D.Y)), Scale, UVWorldPeriod))
	{
		return false;
	}

	// 材质表与组件的 MeshMaterial 一起更新，再广播 —— 无 section 表时组件的 MeshMaterial
	// 就是那唯一一个绘制批次的材质（同 ACSTinyGlade::BindTinyGladeMaterials 的分工）。
	if (RockShellMesh->Materials.Num() < 1) RockShellMesh->Materials.SetNum(1);
	RockShellMesh->Materials[0] = RockShellMaterial;
	RockShellMesh->NotifyMaterialsChanged();
	RockShellComponent->SetGpuMesh(RockShellMesh);

	RockShellBuiltPattern = PatternAsset;
	RockShellBuiltRect = Rect;
	RockShellBuiltScale = Scale;
	RockShellBuiltHash = 0;   // 新分配的流里是池子上一位租客的字节，必须无条件披挂一趟

	UE_LOG(LogTinyGladeGround, Log,
		TEXT("[TinyGladeGround] %s 岩壳已建：%u 三角、图案 %s、缩放 %.3f。"),
		*GetName(), Pattern.TriangleCount, *PatternAsset->GetName(), Scale);
	return true;
}

void ACSGroundActor::RebuildRockShell()
{
	// ⚠️ **第一句就是哈希比较**：短路必须发生在昂贵计算**之前**（已删的塑形物石阶旧路
	// 恰好相反，那是它当年记在案的缺陷）。下面每一步都比这次比较贵。
	const uint32 Hash = RockShellInputHash();
	if (RockShellBuiltHash != 0 && RockShellBuiltHash == Hash) return;

	if (IsTemplate() || !GetWorld()) return;
	if (!EnsureRockShellMesh())
	{
		// 失败也记下哈希：抽不出图案的原因（缺资产 / UV 通道不够 / CPU 访问没开）不会在
		// 同一份输入下自愈，每一 dab 重试一次只是把日志刷爆。改任何一个岩壳属性、或点一次
		// RebuildGroundMesh，哈希就变了，自然会重试。
		RockShellBuiltHash = Hash;
		return;
	}
	if (!TinyGladeMesh) return;   // 地面网格还没建：重建完会再走一次这条路
	const FCSMeshResidentRef GroundResident = TinyGladeMesh->GetResident();
	if (!GroundResident.IsValid()) return;

	UStaticMesh* PatternAsset = RockShellPatternMesh.Get();
	const CSRockShell::FPattern& Pattern = CSRockShell::GetSharedPattern(PatternAsset);
	if (!Pattern.IsValid()) return;

	const FVector Origin = GetActorLocation();
	const FBox2D Rect = GetWorldRect2D();
	const FVector2D Centre2D = Rect.GetCenter();

	CSRockShell::FDisplaceParams Params;
	Params.PatternCentre = Pattern.Centre();
	Params.WorldCentre = FVector2f(float(Centre2D.X), float(Centre2D.Y));
	Params.Scale = FMath::Clamp(RockShellPatternScale, 0.05f, 4.0f);
	// 标称胞腔半径：原件 609 个胞腔铺满 tile ⇒ 平均间距 = 跨度 / sqrt(胞腔数)，半径取一半。
	const FVector2f PatternSpan = Pattern.BoundsMax - Pattern.BoundsMin;
	Params.CellRadiusCm = 0.5f * FMath::Max(PatternSpan.X, PatternSpan.Y) / FMath::Sqrt(float(FMath::Max(Pattern.CellCount, 1u)));
	Params.DomainMin = FVector2f(float(Rect.Min.X), float(Rect.Min.Y));
	Params.DomainMax = FVector2f(float(Rect.Max.X), float(Rect.Max.Y));
	Params.bFlipWinding = Pattern.bFlipWinding;
	Params.GroundOriginXY = FVector2f(float(Origin.X), float(Origin.Y));
	Params.GroundCellSize = Mirror.CellSize;
	Params.GroundVerts = FIntPoint(Mirror.NumVertsX, Mirror.NumVertsY);
	Params.GroundBaseZ = float(Origin.Z);
	Params.SlopeLo = FMath::Max(RockShellSlopeLo, 0.0f);
	Params.SlopeHi = RockShellSlopeHi;
	Params.RoadFade = FMath::Max(RockShellRoadFade, 1.0f);
	Params.RoadSink = FMath::Max(RockShellRoadSink, 0.0f);
	Params.CellJitter = FMath::Max(RockShellCellJitter, 0.0f);
	Params.CellRelief = FMath::Max(RockShellCellRelief, 0.0f);
	Params.NoiseAmp = FMath::Max(RockShellNoiseAmount, 0.0f);
	Params.NoiseFrequency = 1.0f / FMath::Max(RockShellNoiseWavelength, 1.0f);
	Params.Seed = uint32(RockShellSeed);

	TArray<FVector4f> ShaperParams;
	BuildShaperGpuParams(ShaperParams);
	if (!CSRockShell::Displace(RockShellMesh, GroundResident, Params, ShaperParams)) return;

	++RockShellDisplaceCount;
	RockShellBuiltHash = Hash;
}

bool ACSGroundActor::IsRockShellDrawable(FString& OutReason) const
{
	OutReason = GetRockShellUndrawableReason();
	if (!OutReason.IsEmpty())
	{
		// ⚠️ **原因串到不了脚本里，所以这里必须自己打一行日志**（实测）：UE Python 把
		// "bool 返回值 + 一个 out 参数"收成单一返回值 —— 可画时拿到空串，**不可画时拿到 `None`**，
		// 原因串直接丢了。脚本要拿原因请调 GetRockShellUndrawableReason()。
		UE_LOG(LogTinyGladeGround, Warning, TEXT("[TinyGladeGround] %s rock shell not drawable: %s"), *GetName(), *OutReason);
	}
	return OutReason.IsEmpty();
}

FString ACSGroundActor::GetRockShellUndrawableReason() const
{
	FString OutReason;
	// 逐环检查渲染那一侧 —— readback 断言对这些一个字都说不了（见头文件里那段教训）。
	if (!bRockShell) { OutReason = TEXT("bRockShell 关着"); return OutReason; }
	if (!IsValid(RockShellComponent)) { OutReason = TEXT("没有渲染组件"); return OutReason; }
	if (!RockShellComponent->IsRegistered()) { OutReason = TEXT("渲染组件没注册"); return OutReason; }
	if (!RockShellComponent->IsVisible()) { OutReason = TEXT("渲染组件不可见"); return OutReason; }
	if (!RockShellMesh) { OutReason = TEXT("没有岩壳网格对象"); return OutReason; }
	if (RockShellComponent->GetGpuMesh() != RockShellMesh)
	{
		OutReason = TEXT("渲染组件绑的不是本 actor 的岩壳网格");
		return OutReason;
	}

	const FCSMeshResidentRef Resident = RockShellMesh->GetResident();
	if (!Resident.IsValid() || !Resident->IsAllocated()) { OutReason = TEXT("常驻流没分配"); return OutReason; }
	if (Resident->VertexCapacity < 3u) { OutReason = TEXT("顶点容量不足一个三角"); return OutReason; }
	if (!Resident->WorldBounds.IsValid) { OutReason = TEXT("包围盒无效（会被剔除掉）"); return OutReason; }

	// **这一条就是石阶那个坑**：材质为空时组件仍然会画，只是退回引擎默认表面材质 ——
	// 画面上是一片灰，而所有 readback 断言照绿。所以把"有材质"做成显式判据。
	const bool bHasMaterial = RockShellComponent->MeshMaterial != nullptr
		|| (RockShellMesh->Materials.IsValidIndex(0) && RockShellMesh->Materials[0] != nullptr);
	if (!bHasMaterial) { OutReason = TEXT("没有绑材质（会用引擎默认表面材质画成一片灰）"); return OutReason; }

	if (RockShellDisplaceCount <= 0) { OutReason = TEXT("披挂 pass 一次都没跑过"); return OutReason; }
	return FString();
}

bool ACSGroundActor::GetRockShellPatternStats(int32& OutTriangles, int32& OutUVChannels, float& OutMaxCellId,
	bool& bOutFlipWinding, float& OutDirAgreement) const
{
	const CSRockShell::FPattern& Pattern = CSRockShell::GetSharedPattern(RockShellPatternMesh.Get());
	OutTriangles = int32(Pattern.TriangleCount);
	OutUVChannels = Pattern.NumUVChannels;
	OutMaxCellId = Pattern.MaxCellId;
	bOutFlipWinding = Pattern.bFlipWinding;
	OutDirAgreement = Pattern.DirAgreement;
	return Pattern.IsValid();
}

int32 ACSGroundActor::DebugReadRockShellSync(TArray<FVector>& OutWorldPositions)
{
	OutWorldPositions.Reset();
	if (!RockShellMesh) return 0;

	FCSGpuMeshCPUData Readback;
	if (!RockShellMesh->ReadbackMeshSync(Readback)) return 0;
	// 常驻流就是世界空间（同地面），不需要变换。NaN 原样带出去 ——
	// 调用方靠它判三角死活（只看第 0 个顶点，见 CSGroundRockShellTests 里那段注释）。
	OutWorldPositions.Reserve(Readback.Positions.Num());
	for (const FVector3f& P : Readback.Positions) OutWorldPositions.Add(FVector(P));
	return OutWorldPositions.Num();
}

int32 ACSGroundActor::DebugReadRockShellCapSplitSync(int32& OutCapVerts, int32& OutSkirtVerts)
{
	OutCapVerts = 0;
	OutSkirtVerts = 0;
	if (!RockShellMesh) return 0;

	FCSGpuMeshCPUData Readback;
	if (!RockShellMesh->ReadbackMeshSync(Readback)) return 0;

	// 判据取中点而不是"== CapValue"：顶点色在常驻流里是 8 位量化过的（PackColorBGRA），
	// 回读回来是 255/255 与 0/255，浮点等号在别的通道宽度下会静默全判成裙。
	for (const FVector4f& C : Readback.Colors)
	{
		((C.X > 0.5f) ? OutCapVerts : OutSkirtVerts)++;
	}
	return Readback.Colors.Num();
}


// -----------------------------------------------------------------------------
// 塑形物裙边摆件（D12 锚点层的第五家）
//
// **归地面，不归塑形物** —— 三条依据逐条写在 `CSGroundDecor.h` 的文件头，一句话是
// 「塑形物只提供高度场，地面负责派生几何」。锚点取法也在那里：解析剖面的等值带上按弧长布点。
// 这一侧只做接线：读世界 → 生产锚点 → 共用的规划段 → 共用的打包 kernel。
// -----------------------------------------------------------------------------

void ACSGroundActor::BuildSkirtDecorSite(CSGroundDecor::FSite& OutSite) const
{
	OutSite.Rings.Reset();
	OutSite.BaseZ = GetActorLocation().Z;

	// ⚠️ 高度场参数走 `GetHeightFieldParams`，**不从 actor 属性再算一遍** ——
	// GPU 位移 pass、石阶扫描、CPU 镜像重导出读的都是这一份打包结果。各自再算一遍的症状是
	// "摆件那圈的半径与土台差了一点点"，只在改过 `SecondaryLiftScale` 之类之后才显形。
	for (const TWeakObjectPtr<ACSGroundShaperActor>& Weak : Shapers)
	{
		const ACSGroundShaperActor* Shaper = Weak.Get();
		if (!Shaper) continue;

		CSGroundDecor::FShaperRing Ring;
		Shaper->GetHeightFieldParams(Ring.Profile, Ring.Top, Ring.Noise);
		// 身份取 **actor 名**：随关卡序列化、拖动不变、加载顺序无关。数组下标与世界坐标
		// 为什么都不行，逐条写在 `CSGroundDecor.h` 的文件头（S1 在槽位上栽过一次）。
		Ring.Key = CSGroundDecor::RingKey(Shaper->GetName());
		OutSite.Rings.Add(Ring);
	}

	// 落高与道路排除走**镜像**（CPU 权威，与 `SampleHeight` / `SampleRoadWeight` 同一份）。
	// 这条链一次回读都不会有 —— 计划 D12 里那次异步回读是给复杂度场准备的，锚点这一半不需要。
	OutSite.SampleGroundZ = [this](const FVector2D& XY) { return SampleHeight(XY); };
	OutSite.SampleRoadWeight = [this](const FVector2D& XY) { return SampleRoadWeight(XY); };
}

CSHouseDecor::FParams ACSGroundActor::MakeSkirtDecorParams() const
{
	CSHouseDecor::FParams Params;
	Params.SkirtSpacing = SkirtDecorSpacing;
	Params.SkirtBandT = SkirtDecorBandT;
	Params.MinSpacing = SkirtDecorMinSpacing;
	Params.RoadReject = SkirtDecorRoadReject;
	Params.BaseScale = SkirtDecorScale;
	Params.ScaleJitter = SkirtDecorScaleJitter;
	Params.Seed = SkirtDecorSeed;
	return Params;
}

bool ACSGroundActor::EnsureSkirtDecorComponents()
{
	TArray<UStaticMesh*> Wanted;
	for (const TObjectPtr<UStaticMesh>& Mesh : SkirtDecorMeshes)
	{
		if (Mesh) Wanted.Add(Mesh);
	}

	// palette 段：只有裙边那一格非空。其余四家的载体是房子，地面对它们一无所知 ——
	// 留 `{0, 0}` 就是 `BuildPlan` 里那句"这一家没配网格 ⇒ 一件都不长"。
	SkirtDecorPaletteRanges.SetNum(int32(CSHouseDecor::EFamily::Count));
	for (CSHouseDecor::FPaletteRange& Range : SkirtDecorPaletteRanges) Range = CSHouseDecor::FPaletteRange();
	SkirtDecorPaletteRanges[int32(CSHouseDecor::EFamily::Skirt)] = { 0, Wanted.Num() };

	// 蓝图 actor 重跑构造脚本会把实例组件销毁，指针会失效 —— 先判再补（同 EnsureStairComponent）。
	// ⚠️ 组件数一变就必须重建基础网格快照：palette 与组件是**按下标**对齐的，少一个就全体错位。
	bool bComponentsChanged = false;
	while (SkirtDecorComponents.Num() > Wanted.Num())
	{
		TObjectPtr<UCSGpuInstancedMeshComponent> Extra = SkirtDecorComponents.Pop();
		if (IsValid(Extra))
		{
			Extra->ClearInstanceSourceGPU();
			Extra->DestroyComponent();
		}
		bComponentsChanged = true;
	}
	while (SkirtDecorComponents.Num() < Wanted.Num())
	{
		SkirtDecorComponents.Add(nullptr);
		bComponentsChanged = true;
	}
	for (int32 Index = 0; Index < Wanted.Num(); ++Index)
	{
		if (!IsValid(SkirtDecorComponents[Index]))
		{
			UCSGpuInstancedMeshComponent* Component = NewObject<UCSGpuInstancedMeshComponent>(this, NAME_None, RF_Transient);
			Component->SetupAttachment(RootComponent);
			Component->RegisterComponent();   // 未注册时任何变更都会释放 GPU 网格，必须先注册再喂
			SkirtDecorComponents[Index] = Component;
			bComponentsChanged = true;
		}
		SkirtDecorComponents[Index]->InstanceMaterial = SkirtDecorMaterial;
	}
	if (bComponentsChanged) bSkirtDecorBaseMeshReady = false;

	if (SkirtDecorGpuBuffers.Num() != Wanted.Num())
	{
		CSShaperSteps::ReleaseOnRenderThread(SkirtDecorGpuBuffers);
		SkirtDecorGpuBuffers.SetNum(Wanted.Num());
		SkirtDecorHandedCapacities.Reset();
		bSkirtDecorBaseMeshReady = false;
	}
	if (Wanted.Num() == 0) return false;

	bool bMeshesChanged = SkirtDecorMeshesBuiltFrom.Num() != Wanted.Num();
	for (int32 Index = 0; !bMeshesChanged && Index < Wanted.Num(); ++Index)
	{
		bMeshesChanged = SkirtDecorMeshesBuiltFrom[Index] != Wanted[Index];
	}
	if (bMeshesChanged) bSkirtDecorBaseMeshReady = false;

	if (!bSkirtDecorBaseMeshReady)
	{
		SkirtDecorMeshesBuiltFrom.SetNum(Wanted.Num());
		bool bAllOk = true;
		for (int32 Index = 0; Index < Wanted.Num(); ++Index)
		{
			FCSGpuMeshCPUData Data;
			// ⚠️ **复用藤蔓那份读取器，与房子那四家同一条**：它判"法线/UV 读出来合不合法"
			// （有流不等于有数据）、缺了就现补、并把**顶点色**搬进快照。顶点色对 clutter 是决定性的
			// —— `Content/TinyGlade/Textures/` 里一张 clutter 贴图都没有，颜色全烘在顶点流里。
			// 长度轴传 2（不换轴）：摆件本来就以 +Z 为上。
			const bool bOk = CSHouseVine::BuildBaseMesh(Wanted[Index], 2, Data);
			if (bOk)
			{
				SkirtDecorComponents[Index]->SetBaseMeshFromGpuData(Data);

				FBox3f Local(ForceInit);
				for (const FVector3f& P : Data.Positions) Local += P;
				CSShaperSteps::FPaletteBuffers& Buffers = SkirtDecorGpuBuffers[Index];
				Buffers.BaseSphereCentre = Local.IsValid ? Local.GetCenter() : FVector3f::ZeroVector;
				Buffers.BaseSphereRadius = Local.IsValid ? Local.GetExtent().Size() : 0.0f;
				// clutter 网格自带真实尺寸（不是石阶那种单位立方体字典 mesh），块尺寸留 1 ——
				// 缩放全部活在记录的 Scale/ScaleZ 里。
				Buffers.BlockSize = FVector3f(1.0f, 1.0f, 1.0f);
			}
			SkirtDecorMeshesBuiltFrom[Index] = bOk ? Wanted[Index] : nullptr;
			bAllOk &= bOk;
		}
		bSkirtDecorBaseMeshReady = bAllOk;
	}
	if (!bSkirtDecorBaseMeshReady) return false;

	// 容量按**配置上限**一次付清，之后永不扩容（零阻塞纪律）。
	// ⚠️ **必须再走一次 `CSShaperSteps::ReserveCount` 的台阶**，不能把上限直接喂给 ReserveCapacity：
	// 上限是半径的**连续函数**（周长 / 间距），而 ReserveCapacity 只对齐到 64 —— 拖半径时
	// 每涨过一个间距就重新分配一次。藤蔓那轮正是漏了这一步，实测一段拖动 21 次阻塞刷新。
	CSGroundDecor::FSite Site;
	BuildSkirtDecorSite(Site);
	const CSHouseDecor::FParams Params = MakeSkirtDecorParams();
	const uint32 MaxRecords = uint32(FMath::Clamp(
		CSShaperSteps::ReserveCount(CSGroundDecor::MaxRecordsBound(Site, Params)), 64, 1 << 16));
	CSShaperSteps::ReserveCapacity(SkirtDecorGpuBuffers, MaxRecords);

	// 交接包围盒：量化 + 只涨不缩，理由与门框砖 / 藤蔓 / 房子摆件那三段逐字相同
	// （1 cm 阈值会让拖动时每帧都重走一次阻塞的 SetInstanceSourceGPU）。
	// 组件挂在根上、相对变换为单位阵 ⇒ 组件空间就是 actor 空间（同石阶那条）。
	double MeshReach = 0.0;
	for (const CSShaperSteps::FPaletteBuffers& Buffers : SkirtDecorGpuBuffers)
	{
		MeshReach = FMath::Max(MeshReach, double(Buffers.BaseSphereRadius));
	}
	MeshReach *= FMath::Max(double(SkirtDecorScale) * (1.0 + double(SkirtDecorScaleJitter)), 0.05);

	// 伸展取"最远那一圈离本 actor 原点多远"，而不是地面矩形 —— 塑形物完全可以站在地面外面
	// （高度场按 max 合成，出界那部分只是采不到镜像），拿地面矩形当上界会把它剔掉。
	const FVector Origin = GetActorLocation();
	double Reach = 0.0;
	double Top = 0.0;
	for (const CSGroundDecor::FShaperRing& Ring : Site.Rings)
	{
		const double Radius = double(CSGroundDecor::RingRadius(Ring, Params));
		const double DistX = FMath::Abs(double(Ring.Profile.X) - Origin.X);
		const double DistY = FMath::Abs(double(Ring.Profile.Y) - Origin.Y);
		Reach = FMath::Max(Reach, FMath::Max(DistX, DistY) + Radius + MeshReach);
		Top = FMath::Max(Top, double(CSGroundShaperField::PeakHeight(Ring.Top)) + MeshReach);
	}
	Reach = CSShaperSteps::QuantizeUp(FMath::Max(Reach, double(MaxAbsHeight) + MeshReach + 1.0));
	Top = CSShaperSteps::QuantizeUp(Top + MeshReach + 1.0);
	// 下界取 −Reach：摆件落在**地面**上，而地面可以低于 actor 原点（被别人挖过 / 本来就有起伏）。
	FBox LocalBounds(FVector(-Reach, -Reach, -Reach), FVector(Reach, Reach, Top));
	if (SkirtDecorHandedLocalBounds.IsValid) LocalBounds += SkirtDecorHandedLocalBounds;

	bool bNeedHandover = SkirtDecorHandedCapacities.Num() != SkirtDecorGpuBuffers.Num()
		|| !SkirtDecorHandedLocalBounds.IsValid
		|| !SkirtDecorHandedLocalBounds.Min.Equals(LocalBounds.Min, 1.0)
		|| !SkirtDecorHandedLocalBounds.Max.Equals(LocalBounds.Max, 1.0);
	for (int32 Index = 0; !bNeedHandover && Index < SkirtDecorGpuBuffers.Num(); ++Index)
	{
		// 蓝图重跑构造脚本会销毁并重建实例组件：新组件身上没有实例源，缓存说"已交接"就会
		// 永远画不出东西 —— 拿组件自己的状态兜底（同 EnsureDecorComponents）。
		bNeedHandover = SkirtDecorHandedCapacities[Index] != SkirtDecorGpuBuffers[Index].Capacity
			|| !IsValid(SkirtDecorComponents[Index])
			|| !SkirtDecorComponents[Index]->HasInstanceSourceGPU();
	}
	if (!bNeedHandover) return true;

	for (int32 Index = 0; Index < SkirtDecorGpuBuffers.Num(); ++Index)
	{
		if (!SkirtDecorGpuBuffers[Index].IsValid() || !IsValid(SkirtDecorComponents[Index])) return false;
	}

	for (int32 Index = 0; Index < SkirtDecorGpuBuffers.Num(); ++Index)
	{
		FCSGpuInstanceSourceGPU Source;
		Source.PackedInstances = SkirtDecorGpuBuffers[Index].PackedInstances;   // 保留自己的引用，重打包还要用
		Source.Counter = SkirtDecorGpuBuffers[Index].Counter;
		Source.Capacity = SkirtDecorGpuBuffers[Index].Capacity;
		Source.LocalBounds = LocalBounds;
		SkirtDecorComponents[Index]->SetInstanceSourceGPU(Source);
	}

	SkirtDecorHandedCapacities.SetNumUninitialized(SkirtDecorGpuBuffers.Num());
	for (int32 Index = 0; Index < SkirtDecorGpuBuffers.Num(); ++Index)
	{
		SkirtDecorHandedCapacities[Index] = SkirtDecorGpuBuffers[Index].Capacity;
	}
	SkirtDecorHandedLocalBounds = LocalBounds;
	return true;
}

void ACSGroundActor::RebuildSkirtDecor()
{
	if (IsTemplate() || !GetWorld()) return;

	bool bAnyMesh = false;
	for (const TObjectPtr<UStaticMesh>& Mesh : SkirtDecorMeshes) bAnyMesh |= (Mesh != nullptr);

	if (!bSkirtDecorEnabled || !bAnyMesh)
	{
		if (CurrentSkirtDecorInstanceCount != 0 || CurrentSkirtDecorAnchorCount != 0 || !SkirtDecorComponents.IsEmpty())
		{
			// ⚠️ 撤实例源之前先清 counter：不清的话下一次 `EnsureSkirtDecorComponents` 把同一批
			// 带陈旧计数器的 buffer 交回组件，剔除 pass 照着它再画一遍上一代的摆件 ——
			// 而 CPU 计数、三角形数、零阻塞断言**全部照绿**（同 RebuildVine / RebuildFrame / RebuildDecor）。
			CSShaperSteps::ZeroCounters(SkirtDecorGpuBuffers);
			for (const TObjectPtr<UCSGpuInstancedMeshComponent>& Component : SkirtDecorComponents)
			{
				if (IsValid(Component)) Component->ClearInstanceSourceGPU();
			}
			SkirtDecorHandedCapacities.Reset();
			SkirtDecorHandedLocalBounds = FBox(ForceInit);
			CurrentSkirtDecorAnchorCount = 0;
			CurrentSkirtDecorInstanceCount = 0;
			SkirtDecorHash = 0;
		}
		return;
	}
	if (!Mirror.IsInitialized()) return;
	if (!EnsureSkirtDecorComponents()) return;

	CSGroundDecor::FSite Site;
	BuildSkirtDecorSite(Site);
	const CSHouseDecor::FParams Params = MakeSkirtDecorParams();

	TArray<CSHouseDecor::FAnchor> Anchors;
	CSGroundDecor::BuildSkirtAnchors(Site, Params, Anchors);

	CSHouseDecor::FPlan Plan;
	CSHouseDecor::BuildPlan(Anchors, Params, SkirtDecorPaletteRanges, SkirtDecorGpuBuffers.Num(), Plan);

	// 幂等短路。⚠️ 短路点在生产 + 规划**之后**是有意的（同房子那四家）：两步都是纯 CPU、
	// 微秒量级，而锚点表是哈希的**唯一诚实来源** —— 拿参数拼哈希而不跑生产，会在
	// "参数没动但塑形物被拖了 / 路被画到裙边上了"时静默漏更新，而这两件事恰恰都会改锚点。
	// 哈希直接盖住锚点的身份与量化后的位置，土台抬高一厘米也逃不掉。
	//
	// ⚠️ 岩壳那条"短路必须在昂贵计算之前"在这里**不适用**，别照抄：它短路掉的是一趟
	// 181 行规划 + 一次 dispatch，这里短路掉的是几十个三角函数。真正贵的那一步
	// （容量与包围盒交接）在 `EnsureSkirtDecorComponents` 里，它自己有零成本的稳态分支。
	TArray<int32> HashInput;
	HashInput.Reserve(Anchors.Num() * 5 + 16);
	HashInput.Append({ Anchors.Num(), Plan.TotalRecords(), SkirtDecorGpuBuffers.Num(),
		FMath::RoundToInt(SkirtDecorSpacing * 2.0f), FMath::RoundToInt(SkirtDecorBandT * 1000.0f),
		FMath::RoundToInt(SkirtDecorMinSpacing * 2.0f), FMath::RoundToInt(SkirtDecorRoadReject * 100.0f),
		FMath::RoundToInt(SkirtDecorScale * 100.0f), FMath::RoundToInt(SkirtDecorScaleJitter * 100.0f),
		SkirtDecorSeed });
	for (const CSHouseDecor::FAnchor& Anchor : Anchors)
	{
		// ⚠️ 逐个显式截回 int32：`FMath::RoundToInt(double)` 在 UE5 里返回的是 **int64**，
		// 位置量到 cm 早就在 int32 里放得下，但不写这层转换直接编译不过。
		HashInput.Append({ Anchor.AnchorId,
			int32(FMath::RoundToInt(Anchor.Location.X)), int32(FMath::RoundToInt(Anchor.Location.Y)),
			int32(FMath::RoundToInt(Anchor.Location.Z)), int32(FMath::RoundToInt(Anchor.Facing.X * 1000.0)) });
	}
	const uint32 NewHash = FCrc::MemCrc32(HashInput.GetData(), HashInput.Num() * sizeof(int32));

	bool bBuffersReady = SkirtDecorGpuBuffers.Num() == SkirtDecorComponents.Num();
	for (const CSShaperSteps::FPaletteBuffers& Buffers : SkirtDecorGpuBuffers) bBuffersReady &= Buffers.IsValid();
	if (NewHash == SkirtDecorHash && SkirtDecorHandedCapacities.Num() == SkirtDecorGpuBuffers.Num() && bBuffersReady) return;

	SkirtDecorHash = NewHash;
	CurrentSkirtDecorAnchorCount = Anchors.Num();
	CurrentSkirtDecorInstanceCount = Plan.TotalRecords();

	// 各 palette 的组件是同一棵挂接树上的兄弟、变换相同，取第 0 个即可。
	const FMatrix44f WorldToComponent = FMatrix44f(
		SkirtDecorComponents[0]->GetComponentTransform().ToInverseMatrixWithScale());
	CSHouseDecor::Pack(Plan, SkirtDecorGpuBuffers, WorldToComponent);

	UE_LOG(LogTinyGladeGround, Log, TEXT("[TinyGladeGround] %s 裙边摆件：锚点 %d 件 %d palette %d（塑形物 %d 座）"),
		*GetName(), CurrentSkirtDecorAnchorCount, CurrentSkirtDecorInstanceCount,
		SkirtDecorGpuBuffers.Num(), Site.Rings.Num());
}

bool ACSGroundActor::IsSkirtDecorDrawable(FString& OutReason) const
{
	OutReason = GetSkirtDecorUndrawableReason();
	if (!OutReason.IsEmpty())
	{
		// ⚠️ **原因串到不了脚本里，必须自己打一行日志**（实测，与 IsDecorDrawable / IsRockShellDrawable
		// 同一条）：UE Python 把"bool 返回值 + 一个 out 参数"收成单一返回值 —— 可画时拿到空串，
		// **不可画时拿到 `None`**，原因串直接丢了。脚本一律调 `GetSkirtDecorUndrawableReason()`。
		UE_LOG(LogTinyGladeGround, Warning, TEXT("[TinyGladeGround] %s 裙边摆件画不出来：%s"), *GetName(), *OutReason);
	}
	return OutReason.IsEmpty();
}

FString ACSGroundActor::GetSkirtDecorUndrawableReason() const
{
	// 逐环检查渲染那一侧 —— readback 断言对这些一个字都说不了（石阶那个坑：`StairMesh`
	// 恒 NULL、画面一撮黑块、而单测与回归全绿）。执行面照抄 `ACSHouseActor::GetDecorUndrawableReason`。
	if (!bSkirtDecorEnabled) return TEXT("bSkirtDecorEnabled 关着");
	if (SkirtDecorComponents.IsEmpty()) return TEXT("一个 palette 都没有（SkirtDecorMeshes 是空的）");
	if (!bSkirtDecorBaseMeshReady) return TEXT("基础网格快照没建起来（读不到 LOD0 顶点？）");
	if (Shapers.IsEmpty()) return TEXT("这张地面上一座塑形物都没有登记（没有裙边，自然没有裙边摆件）");
	if (CurrentSkirtDecorAnchorCount <= 0) return TEXT("一个锚点都没生产出来（台高为 0？整圈都被邻座埋了或被路排掉了？）");
	if (CurrentSkirtDecorInstanceCount <= 0) return TEXT("锚点全被填充概率/最小间距球筛掉了");

	for (int32 Index = 0; Index < SkirtDecorComponents.Num(); ++Index)
	{
		const UCSGpuInstancedMeshComponent* Component = SkirtDecorComponents[Index];
		if (!IsValid(Component)) return FString::Printf(TEXT("palette %d：没有渲染组件"), Index);
		if (!Component->IsRegistered()) return FString::Printf(TEXT("palette %d：渲染组件没注册"), Index);
		if (!Component->IsVisible()) return FString::Printf(TEXT("palette %d：渲染组件不可见"), Index);
		if (!Component->HasInstanceSourceGPU()) return FString::Printf(TEXT("palette %d：实例源没交接"), Index);
		if (Component->GetBaseMeshSnapshot().Positions.Num() < 3)
		{
			return FString::Printf(TEXT("palette %d：基础网格快照是空的"), Index);
		}
		if (!Component->GetGpuMesh()) return FString::Printf(TEXT("palette %d：GPU 网格没分配"), Index);

		// **这一条就是石阶那个坑**：材质为空时组件仍然会画，只是退回引擎默认表面材质 ——
		// 画面上是一片灰，而所有 readback 断言照绿。
		const UMaterialInterface* Material = Component->InstanceMaterial;
		if (!Material)
		{
			return FString::Printf(TEXT("palette %d：没有绑材质（会用引擎默认表面材质画成一片灰）"), Index);
		}
		// ⚠️ 比石阶那条**多一环**：没勾 `bUsedWithInstancedStaticMeshes` 的材质在实例路径上
		// 会被引擎**静默替换**成默认材质，症状与"没绑材质"逐像素相同。
		const UMaterial* Base = Material->GetMaterial();
		if (!Base || !Base->bUsedWithInstancedStaticMeshes)
		{
			return FString::Printf(
				TEXT("palette %d：材质 '%s' 的母材质没有勾 bUsedWithInstancedStaticMeshes（引擎会静默换成默认材质）"),
				Index, *Material->GetName());
		}
	}
	return FString();
}

int32 ACSGroundActor::DebugReadSkirtDecorInstanceCountGpuSync() const
{
	// −1 = 一个组件都没有，与"真的是 0 个实例"分开：把读不到当 0 会让守着
	// "关掉之后必须归零"的断言在管线坏掉时假绿（同 DebugReadStairCountGpuSync 的口径）。
	if (SkirtDecorComponents.IsEmpty()) return -1;
	int32 Total = 0;
	for (const TObjectPtr<UCSGpuInstancedMeshComponent>& Component : SkirtDecorComponents)
	{
		if (!IsValid(Component)) continue;
		const int32 One = Component->DebugReadDrawnInstanceCountSync();
		if (One < 0) return -1;
		Total += One;
	}
	return Total;
}

#if WITH_EDITOR

int32 ACSGroundActor::SaveInstancedToStaticMeshes(const FString& BakeFolder, bool bSaveAssets)
{
	const FString Folder = BakeFolder.TrimStartAndEnd().IsEmpty()
		? FString::Printf(TEXT("/Game/TinyGladeBake/%s"), *GetName())
		: BakeFolder.TrimStartAndEnd();

	int32 Saved = 0;
	auto BakeOne = [this, &Folder, bSaveAssets, &Saved](UCSGpuInstancedMeshComponent* Component, const TCHAR* Family)
	{
		if (!IsValid(Component)) return;
		const FString Path = FString::Printf(TEXT("%s/SM_%s_%s"), *Folder, *GetName(), Family);
		// 烘回本 actor 的局部空间：资产摆在同一个变换上就复现画面（同岩壳那条出口的口径）。
		if (Component->SaveToStaticMesh(GetActorTransform(), Path, /*bReplaceExistingAsset*/ true, bSaveAssets))
		{
			++Saved;
		}
	};

	BakeOne(StairComponent, TEXT("Stairs"));
	BakeOne(StairPebbleComponent, TEXT("StairPebbles"));
	// 裙边摆件（D12 第五家）也走这条出口 —— 裁决六 ① 要求每一类 GPU 生成物都有一条
	// **走得通**的 `SaveToStaticMesh`。判据不是"这里写了一行"，而是
	// `GroundDecor.SkirtPropsSurviveBake` 真烘一遍再从资产里读回来（同门框砖那条）。
	for (int32 Index = 0; Index < SkirtDecorComponents.Num(); ++Index)
	{
		BakeOne(SkirtDecorComponents[Index], *FString::Printf(TEXT("SkirtDecor%d"), Index));
	}

	UE_LOG(LogTinyGladeGround, Log, TEXT("[TinyGladeGround] %s 实例路烘焙：%d 张资产 -> %s"),
		*GetName(), Saved, *Folder);
	return Saved;
}

bool ACSGroundActor::DebugBakeSkirtDecorSync(const FString& AssetPath, int32& OutTriangles,
	int32& OutVertexInstances, int32& OutUVChannels, int32& OutDistinctUVs, int32& OutDistinctBakedRandoms,
	int32& OutGpuInstanceCount, bool& bOutRandomsMatchGpu)
{
	OutTriangles = 0;
	OutVertexInstances = 0;
	OutUVChannels = 0;
	OutDistinctUVs = 0;
	OutDistinctBakedRandoms = 0;
	OutGpuInstanceCount = 0;
	bOutRandomsMatchGpu = false;
	if (SkirtDecorComponents.IsEmpty() || !IsValid(SkirtDecorComponents[0])) return false;
	UCSGpuInstancedMeshComponent* Component = SkirtDecorComponents[0];

	// 先把 GPU 那一侧的真值取到手，再烘 —— 顺序反过来的话，烘焙本身若不小心动了实例源，
	// 后读到的就是被自己改过的值，断言等于自证（同 DebugBakeFrameBricksSync）。
	TArray<float> GpuRandoms;
	if (!Component->DebugReadInstanceRandomsSync(GpuRandoms)) return false;
	OutGpuInstanceCount = GpuRandoms.Num();

	// 走组件自己的出口，不另拼一条：要证的正是"**那条**出口带不带得走通道"。
	// BakeSpace 取本 actor 的变换（同石阶 / 岩壳 / 道路那三条出口的口径）。
	UStaticMesh* Baked = Component->SaveToStaticMesh(
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

	// 8 位量化后去重：顶点色最终就存成 FColor，比"浮点相等"更贴近资产里真正留下的东西。
	auto Quantize = [](float Value) { return int32(FMath::RoundToInt(FMath::Clamp(Value, 0.0f, 1.0f) * 255.0f)); };
	TSet<int32> BakedRandoms;
	// UV 是不是退化的：全 (0,0) 一样不报错，症状只是烘焙件的贴图变成一整片同一个像素。
	// 量化到 1/1024 再去重 —— 判的是"有没有内容"，不是"精度对不对"。
	TSet<int32> BakedUVs;
	for (const FVertexInstanceID InstanceID : Description->VertexInstances().GetElementIDs())
	{
		BakedRandoms.Add(Quantize(Colors[InstanceID].W));
		const FVector2f UV = UVs.Get(InstanceID, 0);
		BakedUVs.Add(int32(FMath::RoundToInt(UV.X * 1024.0f)) * 8191 + int32(FMath::RoundToInt(UV.Y * 1024.0f)));
	}
	OutDistinctBakedRandoms = BakedRandoms.Num();
	OutDistinctUVs = BakedUVs.Num();

	// 逐个对：GPU 上每一行的 `Origin.w` 都必须在烘焙件的 alpha 集合里找得到。
	// 反向不查 —— 同一个 8 位桶可能被两个实例共用，那不是缺陷；
	// "GPU 有而烘焙件没有"才是通道被丢掉的证据。
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

bool ACSGroundActor::DebugBakeRockShellCapSplitSync(const FString& AssetPath, int32& OutCapCorners,
	int32& OutSkirtCorners, int32& OutTriangles)
{
	OutCapCorners = 0;
	OutSkirtCorners = 0;
	OutTriangles = 0;
	if (!IsValid(RockShellComponent) || !RockShellMesh) return false;

	// 走组件自己的出口，不另拼一条：这里要证的正是"**那条**出口带不带得走顶点色"，
	// 自己拼一条等价路径只会证明另一条路能行。BakeSpace 取本 actor 的变换 ——
	// 壳的常驻流是世界空间的，烘回局部空间才能摆回原处（同 RoadMeshSaveTests 的口径）。
	UStaticMesh* Baked = RockShellComponent->SaveToStaticMesh(
		GetActorTransform(), AssetPath, /*bReplaceExistingAsset*/ true, /*bSaveAsset*/ false);
	if (!Baked) return false;

	const FMeshDescription* Description = Baked->GetMeshDescription(0);
	if (!Description) return false;

	FStaticMeshConstAttributes Attributes(*Description);
	TVertexInstanceAttributesConstRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
	if (!Colors.IsValid()) return false;

	OutTriangles = Description->Triangles().Num();
	for (const FVertexInstanceID InstanceID : Description->VertexInstances().GetElementIDs())
	{
		((Colors[InstanceID].X > 0.5f) ? OutCapCorners : OutSkirtCorners)++;
	}
	return true;
}

#endif // WITH_EDITOR

#if WITH_EDITOR
void ACSGroundActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// 形状属性 = 决定顶点位置或索引的量（计划 D14 的哈希纪律，这里是它的属性面板版本）。
	// GroundMaterial 与 BaseColor 都不在其列：前者只重绑材质，后者压根不参与快照（权威是
	// Mirror.Colors，要铺新底色请点 ResetPaint）。早先把两者算进来的代价是一次纯空转的
	// 全量重建 + 全场唤醒。
	const FName PropertyName = PropertyChangedEvent.GetPropertyName();
	const bool bShapeProperty =
		PropertyName == GET_MEMBER_NAME_CHECKED(ACSGroundActor, NumCellsX) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ACSGroundActor, NumCellsY) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ACSGroundActor, CellSize) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ACSGroundActor, UVWorldPeriod);
	if (bShapeProperty) RebuildGroundMesh();   // 内部末尾会重扫石阶
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(ACSGroundActor, GroundMaterial)) BindTinyGladeMaterials({ GroundMaterial });
	// 其余属性一律重扫石阶 + 重披挂岩壳：两条链各自就是一次 dispatch，为了省它去维护一张
	// "哪些属性算石阶/岩壳属性"的名单，收益远小于名单漏一条时"改了参数没反应"的排查成本。
	// 岩壳自己的哈希会把真正没变的那些吃掉。
	else { RebuildStairs(); RebuildRockShell(); RebuildSkirtDecor(); }
}

void ACSGroundActor::PostEditUndo()
{
	Super::PostEditUndo();

	// Mirror 是 NonTransactional，所以撤销永远不会动它 —— 但被撤销的可能是格数/格距这类
	// 形状属性，镜像与配置就此错配。EnsureMirrorInitialized 检出错配后重置，再全量对齐。
	// 放在这里是因为 AActor::PostEditUndo 既不调 PostEditMove、也不保证跑构造脚本。
	EnsureMirrorInitialized();
	RebuildGroundMesh();
}

void ACSGroundActor::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	if (!TinyGladeMesh)
	{
		RebuildGroundMesh();
		return;
	}

	// 常驻流是世界空间：拖动期间增量平移（便宜、无重建卡顿），松手后全量重建对齐，
	// 顺带把拖动过程中累计的浮点误差清零。
	const FVector Delta = GetActorLocation() - MeshBuiltAtLocation;
	if (bFinished)
	{
		if (!Delta.IsNearlyZero()) RebuildGroundMesh();
		return;
	}
	if (Delta.IsNearlyZero()) return;
	UCSMeshOps::TranslateMesh(TinyGladeMesh, Delta);
	MeshBuiltAtLocation = GetActorLocation();
	// 地面挪了，压在上面的查询结果全变——拖动中也逐帧直推。
	OnGroundChanged.Broadcast(this, ComputeGroundWorldBox());
}
#endif
