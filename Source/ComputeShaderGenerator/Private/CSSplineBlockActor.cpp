#include "CSSplineBlockActor.h"

#include "CSGpuMeshTypes.h"
#include "CSMesh.h"
#include "CSMeshOps.h"
#include "Components/SplineComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Rendering/ColorVertexBuffer.h"
#include "Rendering/PositionVertexBuffer.h"
#include "Rendering/StaticMeshVertexBuffer.h"
#include "StaticMeshResources.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogCSSplineBlock, Log, All);

namespace
{
// Unity/jumbo builds share a TU, so file-local names carry a CSSplineBlock_ prefix.

/** 一个 palette 条目的 CPU 三角缓存：局部空间、bounds 中心已移到原点、绕序已按常驻流口径翻转。 */
struct FCSSplineBlock_PaletteEntry
{
	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	TArray<FVector3f> Tangents;
	TArray<float> BinormalSigns;
	TArray<FVector2f> UVs;
	TArray<FVector4f> Colors;                     // 源无顶点色时铺白
	TArray<uint32> Indices;                       // 每三角已交换角点 1/2（StaticMesh → 常驻流的绕序转换）
	float LengthX = 0.0f;                         // 沿局部 X 的块长（bounds SizeX），排布唯一读的尺寸
	UMaterialInterface* SlotMaterial = nullptr;   // 该条目的槽材质（源 mesh 槽 0）
};

/**
 * 读 LOD0 渲染缓冲提取一个 palette 条目。编辑器里渲染缓冲常驻 CPU 可读；打包运行需要
 * 源 mesh 勾选 Allow CPU Access（同 UCSGpuInstancedMeshComponent::RebuildBaseMeshSnapshot
 * 的约定）。提取失败返回 false，调用方跳过该条目并告警。
 */
bool CSSplineBlock_ExtractPaletteEntry(UStaticMesh* Mesh, FCSSplineBlock_PaletteEntry& Out)
{
	if (!Mesh) return false;

	const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	if (!RenderData || RenderData->LODResources.Num() == 0) return false;
	const FStaticMeshLODResources& LOD = RenderData->LODResources[0];

	const uint32 NumVerts = LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();
	if (NumVerts < 3 || LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumVertices() != NumVerts) return false;

	TArray<uint32> SourceIndices;
	LOD.IndexBuffer.GetCopy(SourceIndices);
	if (SourceIndices.Num() < 3) return false;

	// bounds 直接用提取到的位置算（与上传几何严格一致），把中心移到原点；SizeX 即块长。
	FBox LocalBox(ForceInit);
	for (uint32 V = 0; V < NumVerts; ++V) LocalBox += FVector(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(V));
	const FVector3f Center(LocalBox.GetCenter());
	Out.LengthX = float(LocalBox.GetSize().X);
	if (Out.LengthX <= UE_KINDA_SMALL_NUMBER) return false;   // 沿 X 零厚度的块没有 pitch 语义

	const bool bHasUVs = LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords() > 0;
	const bool bHasColors = LOD.VertexBuffers.ColorVertexBuffer.GetNumVertices() == NumVerts;

	Out.Positions.Reserve(NumVerts);
	Out.Normals.Reserve(NumVerts);
	Out.Tangents.Reserve(NumVerts);
	Out.BinormalSigns.Reserve(NumVerts);
	Out.UVs.Reserve(NumVerts);
	Out.Colors.Reserve(NumVerts);
	for (uint32 V = 0; V < NumVerts; ++V)
	{
		Out.Positions.Add(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(V) - Center);

		const FVector4f TangentZ = LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(V);
		Out.Normals.Add(FVector3f(TangentZ.X, TangentZ.Y, TangentZ.Z));
		Out.Tangents.Add(LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentX(V));
		Out.BinormalSigns.Add(TangentZ.W < 0.0f ? -1.0f : 1.0f);
		Out.UVs.Add(bHasUVs ? LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(V, 0) : FVector2f::ZeroVector);

		// FColor 按字节直读（ReinterpretAsLinear，不做 sRGB），与 GPU 上传路径的 pass-through 一致。
		FVector4f Color(1.0f, 1.0f, 1.0f, 1.0f);
		if (bHasColors)
		{
			const FLinearColor Linear = LOD.VertexBuffers.ColorVertexBuffer.VertexColor(V).ReinterpretAsLinear();
			Color = FVector4f(Linear.R, Linear.G, Linear.B, Linear.A);
		}
		Out.Colors.Add(Color);
	}

	// StaticMesh → 常驻流：交换角点 1/2 翻转绕序，顶点法线不取反 —— CopyFromStaticMesh 的
	// bFlipWinding 口径（翻的是索引不是法线；两边面法线口径差一个负号，见 CSMeshBuild.h）。
	const int32 TriangleCount = SourceIndices.Num() / 3;
	Out.Indices.Reserve(TriangleCount * 3);
	for (int32 Tri = 0; Tri < TriangleCount; ++Tri)
	{
		Out.Indices.Add(SourceIndices[Tri * 3 + 0]);
		Out.Indices.Add(SourceIndices[Tri * 3 + 2]);
		Out.Indices.Add(SourceIndices[Tri * 3 + 1]);
	}

	Out.SlotMaterial = Mesh->GetMaterial(0);
	return true;
}

/** 把缓存条目按块变换烘到世界空间 append 进快照，并给该块的每个三角填 MaterialSlot。 */
void CSSplineBlock_AppendEntryTransformed(const FCSSplineBlock_PaletteEntry& Entry,
	const FTransform& BlockTransform, int32 MaterialSlot, FCSGpuMeshCPUData& Out)
{
	const FMatrix44f LocalToWorld(BlockTransform.ToMatrixWithScale());
	// 沿 X 的 pitch 缩放是非均匀缩放：法线要用逆转置，否则被剪切出表面（同 CopyFromStaticMesh）。
	const FMatrix44f NormalToWorld(BlockTransform.ToMatrixWithScale().Inverse().GetTransposed());

	const uint32 VertexBase = uint32(Out.Positions.Num());
	const int32 VertexCount = Entry.Positions.Num();
	for (int32 V = 0; V < VertexCount; ++V)
	{
		Out.Positions.Add(FVector3f(LocalToWorld.TransformPosition(Entry.Positions[V])));

		const FVector3f Normal = FVector3f(NormalToWorld.TransformVector(Entry.Normals[V])).GetSafeNormal(UE_SMALL_NUMBER, FVector3f::UnitZ());
		// 切线走完整矩阵，再对法线重正交（非均匀缩放会把两者剪出正交）；残余退化由
		// CopyFromMeshSnapshot 上传时的兜底轴替换收尾。
		FVector3f Tangent = FVector3f(LocalToWorld.TransformVector(Entry.Tangents[V]));
		Tangent = (Tangent - Normal * FVector3f::DotProduct(Tangent, Normal)).GetSafeNormal(UE_SMALL_NUMBER, FVector3f::UnitX());
		Out.Normals.Add(Normal);
		Out.Tangents.Add(Tangent);
		Out.BinormalSigns.Add(Entry.BinormalSigns[V]);
		Out.TexCoords().Add(Entry.UVs[V]);
		Out.Colors.Add(Entry.Colors[V]);
	}

	for (const uint32 Index : Entry.Indices) Out.Indices.Add(VertexBase + Index);

	const int32 TriangleCount = Entry.Indices.Num() / 3;
	for (int32 Tri = 0; Tri < TriangleCount; ++Tri) Out.TriangleMaterialSlots.Add(MaterialSlot);
}
}

ACSSplineBlockActor::ACSSplineBlockActor()
{
	// 排布参照样条：默认 3 点、总长约 800cm、带一点弯，放进关卡不调参就能看到效果。
	Spline = CreateDefaultSubobject<USplineComponent>(TEXT("Spline"));
	Spline->SetupAttachment(RootComponent);
	Spline->ClearSplinePoints(false);
	Spline->AddSplinePoint(FVector::ZeroVector, ESplineCoordinateSpace::Local, false);
	Spline->AddSplinePoint(FVector(400.0, 120.0, 0.0), ESplineCoordinateSpace::Local, false);
	Spline->AddSplinePoint(FVector(800.0, 0.0, 0.0), ESplineCoordinateSpace::Local, false);
	Spline->UpdateSpline();

	// 默认块型盘：引擎自带 100cm 立方体，开箱即用。
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeFinder.Succeeded()) BlockPalette.Add(CubeFinder.Object);
}

void ACSSplineBlockActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	// 常驻流是世界空间：拖 spline 点、移动 actor 都会重跑构造脚本，统一从这里全量重建。
	RebuildBlocks();
}

float ACSSplineBlockActor::SolveBlockLayout(float TotalLength, float InGap,
	const TArray<float>& PaletteLengths, FRandomStream& Rand, TArray<int32>& OutSequence)
{
	OutSequence.Reset();
	if (TotalLength <= UE_KINDA_SMALL_NUMBER) return 0.0f;

	// 只在正长度条目里选：0/负长度块让贪心永不推进（也没有排布语义）。全无效等价于空 palette。
	TArray<int32> ValidIndices;
	for (int32 Index = 0; Index < PaletteLengths.Num(); ++Index) if (PaletteLengths[Index] > UE_KINDA_SMALL_NUMBER) ValidIndices.Add(Index);
	if (ValidIndices.IsEmpty()) return 0.0f;

	// 负 gap 在这里没有语义：贪心靠 "gap + 块长" 推进，够负就永不推进（只剩 MaxBlocks 兜底）。
	// ⚠️ 想要 TG 那种**负砖缝**别来动这一行 —— TG 的负缝不出在排布上，而是把逐实例缩放乘在
	// 单位立方体上、让渲染尺寸大于排布槽位（本项目对应 ACSHouseActor::FrameBrickBloat）。
	// 排布保持"近定距"、渲染尺寸单独胀，块数跳变时穿插量才只是微调而不会翻成正缝。
	const float SafeGap = FMath::Max(InGap, 0.0f);

	// 1) 贪心随机填充到首次越界：S >= TotalLength 才停，所以最后一块必然是越界块。
	//    块数上限只防病态参数（微小块长 × 超长样条把编辑器拖死），正常配置远碰不到。
	constexpr int32 MaxBlocks = 65536;
	float Sum = 0.0f;       // 含最后一块（及其前置 gap）的累计
	float PrevSum = 0.0f;   // 不含最后一块（及其前置 gap）的累计
	while (Sum < TotalLength)
	{
		if (OutSequence.Num() >= MaxBlocks)
		{
			OutSequence.Reset();
			return 0.0f;
		}
		const int32 Pick = ValidIndices[Rand.RandRange(0, ValidIndices.Num() - 1)];
		PrevSum = Sum;
		if (!OutSequence.IsEmpty()) Sum += SafeGap;
		Sum += PaletteLengths[Pick];
		OutSequence.Add(Pick);
	}

	// 2) 候选 A：保留最后一块整体压缩（scaleA ≤ 1）；候选 B：去掉最后一块整体拉伸（scaleB ≥ 1）。
	//    |log(scale)| 较小者胜 —— 压缩 0.8x 与拉伸 1.25x 视为同等代价。只有一块时没有候选 B。
	const float ScaleA = TotalLength / Sum;
	if (OutSequence.Num() <= 1 || PrevSum <= UE_KINDA_SMALL_NUMBER) return ScaleA;

	const float ScaleB = TotalLength / PrevSum;
	if (FMath::Abs(FMath::Loge(ScaleA)) <= FMath::Abs(FMath::Loge(ScaleB))) return ScaleA;

	OutSequence.Pop();
	return ScaleB;
}

void ACSSplineBlockActor::RebuildBlocks()
{
	// 无效输入的统一出口：清空网格而不是保留旧块 —— 本次调用的语义是"替换"，
	// 拿到"没有几何"的调用方不能继续画上一次的结果（同 BuildGpuMeshFromCSTriangleData）。
	auto ClearBlocks = [this]() { if (TinyGladeMesh) TinyGladeMesh->Reset(); };

	const float TotalLength = Spline ? Spline->GetSplineLength() : 0.0f;
	if (TotalLength <= UE_KINDA_SMALL_NUMBER)
	{
		ClearBlocks();
		return;
	}

	// 每个 palette 条目只提取一次，本次重建内所有同型块共享这份缓存；提取失败的条目跳过。
	TArray<FCSSplineBlock_PaletteEntry> Entries;
	TArray<float> PaletteLengths;
	for (int32 PaletteIndex = 0; PaletteIndex < BlockPalette.Num(); ++PaletteIndex)
	{
		FCSSplineBlock_PaletteEntry Entry;
		if (!CSSplineBlock_ExtractPaletteEntry(BlockPalette[PaletteIndex], Entry))
		{
			UE_LOG(LogCSSplineBlock, Warning,
				TEXT("[CSSplineBlock] %s: palette[%d] '%s' 提取不到 CPU 三角数据，跳过。"),
				*GetName(), PaletteIndex, *GetNameSafe(BlockPalette[PaletteIndex]));
			continue;
		}
		PaletteLengths.Add(Entry.LengthX);
		Entries.Add(MoveTemp(Entry));
	}

	FRandomStream Rand(Seed);
	TArray<int32> Sequence;
	const float Scale = SolveBlockLayout(TotalLength, Gap, PaletteLengths, Rand, Sequence);
	if (Sequence.IsEmpty() || Scale <= 0.0f)
	{
		ClearBlocks();
		return;
	}

	// 材质表：Override 非空则全体单槽；否则槽 i = 有效条目 i 的槽 0 材质（与 Sequence 的索引同域）。
	TArray<TObjectPtr<UMaterialInterface>> Materials;
	if (OverrideMaterial) Materials.Add(OverrideMaterial);
	else for (const FCSSplineBlock_PaletteEntry& Entry : Entries) Materials.Add(Entry.SlotMaterial);

	// 预估容量，避免逐块 append 反复扩容。
	int32 TotalVerts = 0;
	int32 TotalIndices = 0;
	for (const int32 EntryIndex : Sequence)
	{
		TotalVerts += Entries[EntryIndex].Positions.Num();
		TotalIndices += Entries[EntryIndex].Indices.Num();
	}

	FCSGpuMeshCPUData Snapshot;
	Snapshot.Positions.Reserve(TotalVerts);
	Snapshot.Normals.Reserve(TotalVerts);
	Snapshot.Tangents.Reserve(TotalVerts);
	Snapshot.BinormalSigns.Reserve(TotalVerts);
	Snapshot.TexCoords().Reserve(TotalVerts);
	Snapshot.Colors.Reserve(TotalVerts);
	Snapshot.Indices.Reserve(TotalIndices);
	Snapshot.TriangleMaterialSlots.Reserve(TotalIndices / 3);

	// 逐块：中心弧长 = 已排 pitch 前缀和 + 本块缩放后长度的一半；取该处样条世界位置/切向，
	// 沿切向（块局部 X）乘 pitch 缩放，截面不缩。末块中心因浮点误差微越界时由
	// GetXxxAtDistanceAlongSpline 自身的端点 clamp 兜底。
	const float ScaledGap = FMath::Max(Gap, 0.0f) * Scale;
	float Cursor = 0.0f;
	for (const int32 EntryIndex : Sequence)
	{
		const FCSSplineBlock_PaletteEntry& Entry = Entries[EntryIndex];
		const float ScaledLength = Entry.LengthX * Scale;
		const float Distance = Cursor + ScaledLength * 0.5f;
		Cursor += ScaledLength + ScaledGap;

		const FVector Location = Spline->GetLocationAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);
		const FRotator Rotation = Spline->GetRotationAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);
		const FTransform BlockTransform(Rotation, Location, FVector(Scale, 1.0f, 1.0f));
		CSSplineBlock_AppendEntryTransformed(Entry, BlockTransform, OverrideMaterial ? 0 : EntryIndex, Snapshot);
	}

	// 常驻流契约：世界空间、逐顶点属性。材质表随快照带一份（上传管道实际以参数为准），同地面。
	Snapshot.SourceSpace = FCSGpuMeshCPUData::ESpace::World;
	Snapshot.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerVertex;
	Snapshot.Materials = Materials;

	if (!UploadTinyGladeSnapshot(Snapshot, Materials))
	{
		ClearBlocks();
		return;
	}
	// 多槽才需要分批绘制；单槽时"整网格一个批次"本来就是正确语义。
	if (Materials.Num() > 1) UCSMeshOps::BuildMaterialSections(GetTinyGladeMesh());
}
