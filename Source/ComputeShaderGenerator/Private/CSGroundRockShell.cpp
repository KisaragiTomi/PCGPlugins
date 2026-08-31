#include "CSGroundRockShell.h"

#include "CSGpuMeshTypes.h"
#include "CSGroundShaperField.h"
#include "CSMeshOps.h"
#include "ComputeShaderGenerateHelper.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "Engine/StaticMesh.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ShaderParameterStruct.h"
#include "StaticMeshResources.h"

DEFINE_LOG_CATEGORY_STATIC(LogCSRockShell, Log, All);

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSRockShell_ 前缀（与 CSGround_ / CSStairs_ /
// CSShaperSteps_ 必须都不同，否则 unity blob 里同名符号打架，而报错位置会指向一个跟改动
// 无关的文件）。

constexpr int32 CSRockShell_GroupSizeX = 64;

/** 契约字段所在的 UV 通道，见 Docs/TinyGlade/CSRockShellPattern.md「契约字段 → 通道映射」。 */
constexpr int32 CSRockShell_UVDir = 0;      // (dir_to_centroid.x, .z)，只作核对
constexpr int32 CSRockShell_UVCell = 1;     // (cell_id, is_corner)
constexpr int32 CSRockShell_UVRim = 2;      // (cell_bby = bIsTopRim, is_top = bIsCapTri)

/**
 * 从图案 StaticMesh 抽一份逐三角展开的数据。
 *
 * 承重的三件事，每一件失手都不报错、只是画面不对：
 *   ① **按索引缓冲展回 `Tri*3 + k`** —— 原件 44.2% 的顶点槽逐字节相同，UE 会焊掉它们，
 *      而「一个 NaN 出局整个三角」这条裁决靠的正是逐三角展开的布局。
 *   ② **`DirToCentroid` 从坐标现算，不用烘焙的 TEXCOORD_0** —— UV 不参与轴变换而 POSITION 会，
 *      导入器一旦翻掉一个平面轴，烘焙的 dir 就与实际坐标不再对应，且完全静默。烘焙件留作核对。
 *   ③ **绕序实测，不写死** —— glTF 右手 Y-up → UE 左手 Z-up 时导入器可能翻绕序，
 *      猜错的症状是整张壳翻在里面（从上方完全看不见），没有任何断言会红。
 */
CSRockShell::FPattern CSRockShell_ExtractPattern(UStaticMesh* PatternMesh)
{
	CSRockShell::FPattern Out;
	if (!PatternMesh) return Out;

	const FStaticMeshRenderData* RenderData = PatternMesh->GetRenderData();
	if (!RenderData || RenderData->LODResources.Num() == 0)
	{
		UE_LOG(LogCSRockShell, Warning, TEXT("[CSRockShell] %s 没有渲染数据，岩壳关闭。"), *PatternMesh->GetPathName());
		return Out;
	}

	const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
	const FPositionVertexBuffer& PosVB = LOD.VertexBuffers.PositionVertexBuffer;
	const FStaticMeshVertexBuffer& AttrVB = LOD.VertexBuffers.StaticMeshVertexBuffer;
	const FIndexArrayView IndexView = LOD.IndexBuffer.GetArrayView();

	const uint32 NumSourceVerts = PosVB.GetNumVertices();
	const uint32 NumIndices = uint32(IndexView.Num());
	Out.NumUVChannels = int32(AttrVB.GetNumTexCoords());
	if (NumSourceVerts == 0 || NumIndices < 3 || NumIndices % 3 != 0)
	{
		// 打包后最常见的原因就是 bAllowCPUAccess 没开：顶点/索引缓冲只在显存里，CPU 读到空。
		UE_LOG(LogCSRockShell, Warning,
			TEXT("[CSRockShell] %s 的 CPU 侧缓冲读不到（顶点 %u / 索引 %u）—— 多半是 bAllowCPUAccess 没开。岩壳关闭。"),
			*PatternMesh->GetPathName(), NumSourceVerts, NumIndices);
		return Out;
	}
	if (Out.NumUVChannels <= CSRockShell_UVRim)
	{
		UE_LOG(LogCSRockShell, Warning,
			TEXT("[CSRockShell] %s 只有 %d 条 UV 通道，逐顶点胞腔数据丢了（需要 ≥ 3）——")
			TEXT(" 检查导入时 generate_lightmap_u_vs 是否关掉。岩壳关闭。"),
			*PatternMesh->GetPathName(), Out.NumUVChannels);
		return Out;
	}

	// --- 轴映射：图案是平的，最薄的那一轴就是厚度轴。期望换轴后落在 UE 的 Z 上。 ---
	FVector3f Min(FLT_MAX), Max(-FLT_MAX);
	for (uint32 V = 0; V < NumSourceVerts; ++V)
	{
		const FVector3f P = PosVB.VertexPosition(V);
		Min = FVector3f(FMath::Min(Min.X, P.X), FMath::Min(Min.Y, P.Y), FMath::Min(Min.Z, P.Z));
		Max = FVector3f(FMath::Max(Max.X, P.X), FMath::Max(Max.Y, P.Y), FMath::Max(Max.Z, P.Z));
	}
	const FVector3f Size = Max - Min;
	if (Size.Z > Size.X || Size.Z > Size.Y)
	{
		// 不猜一个替代坐标系：猜错只会把错误挪到更远的地方（壳立起来、坡度判据全错）。
		UE_LOG(LogCSRockShell, Error,
			TEXT("[CSRockShell] %s 的最薄轴不是 Z（尺寸 %.0f × %.0f × %.0f cm）—— 导入器的轴映射与预期不符，")
			TEXT(" 见 Docs/TinyGlade/CSRockShellPattern.md「首次导入后必须核对的四项」第 3/4 条。岩壳关闭。"),
			*PatternMesh->GetPathName(), Size.X, Size.Y, Size.Z);
		return Out;
	}
	Out.BoundsMin = FVector2f(Min.X, Min.Y);
	Out.BoundsMax = FVector2f(Max.X, Max.Y);
	Out.ThicknessCm = Size.Z;

	// --- 逐胞腔质心（从**源**顶点算，展开后的重复顶点会把均值往高价点偏）---
	int32 MaxCellId = 0;
	TArray<int32> SourceCellId;
	SourceCellId.SetNumUninitialized(int32(NumSourceVerts));
	for (uint32 V = 0; V < NumSourceVerts; ++V)
	{
		const FVector2f UVCell = AttrVB.GetVertexUV(V, CSRockShell_UVCell);
		const int32 CellId = FMath::Max(FMath::RoundToInt32(UVCell.X), 0);
		SourceCellId[int32(V)] = CellId;
		MaxCellId = FMath::Max(MaxCellId, CellId);
		Out.MaxCellId = FMath::Max(Out.MaxCellId, UVCell.X);
	}
	Out.CellCount = uint32(MaxCellId + 1);
	TArray<FVector2f> Sums;
	TArray<int32> Counts;
	Sums.Init(FVector2f::ZeroVector, int32(Out.CellCount));
	Counts.Init(0, int32(Out.CellCount));
	for (uint32 V = 0; V < NumSourceVerts; ++V)
	{
		const FVector3f P = PosVB.VertexPosition(V);
		const int32 Cell = SourceCellId[int32(V)];
		Sums[Cell] += FVector2f(P.X, P.Y);
		++Counts[Cell];
	}
	Out.Centroids.SetNumUninitialized(int32(Out.CellCount));
	for (int32 Cell = 0; Cell < int32(Out.CellCount); ++Cell)
	{
		Out.Centroids[Cell] = Counts[Cell] > 0 ? Sums[Cell] / float(Counts[Cell]) : FVector2f::ZeroVector;
	}

	// --- 逐三角展开 ---
	Out.TriangleCount = NumIndices / 3u;
	Out.VertexCount = Out.TriangleCount * 3u;
	Out.RestDir.SetNumUninitialized(int32(Out.VertexCount));
	Out.CellFlags.SetNumUninitialized(int32(Out.VertexCount));

	double DirDotSum = 0.0;
	int32 DirDotCount = 0;
	int32 RimMismatch = 0;
	int32 CapUp = 0;
	int32 CapDown = 0;
	const float TopRimZ = Max.Z;
	for (uint32 Tri = 0; Tri < Out.TriangleCount; ++Tri)
	{
		FVector3f Corner[3];
		bool bCapTri = false;
		for (uint32 K = 0; K < 3u; ++K)
		{
			const uint32 Src = IndexView[int32(Tri * 3u + K)];
			if (Src >= NumSourceVerts) return CSRockShell::FPattern();   // 索引越界：整件作废，别画垃圾

			const FVector3f P = PosVB.VertexPosition(Src);
			const FVector2f RestXY(P.X, P.Y);
			const FVector2f UVCell = AttrVB.GetVertexUV(Src, CSRockShell_UVCell);
			const FVector2f UVRim = AttrVB.GetVertexUV(Src, CSRockShell_UVRim);
			const int32 CellId = SourceCellId[int32(Src)];

			// DirToCentroid：**指向质心**（实测，与计划契约注释的符号相反）。从坐标现算。
			const FVector2f ToCentroid = Out.Centroids[CellId] - RestXY;
			const float Len = ToCentroid.Size();
			const FVector2f Dir = Len > 1e-3f ? ToCentroid / Len : FVector2f::ZeroVector;

			// 核对：烘焙件 TEXCOORD_0 与现算的应当一致（实测中位点积 +0.9996）。
			// 偏离说明导入器翻了一个平面轴 —— 那时现算的这份仍然对，核对值让人看得见。
			if (Len > 1e-3f)
			{
				const FVector2f Baked = AttrVB.GetVertexUV(Src, CSRockShell_UVDir);
				const float BakedLen = Baked.Size();
				if (BakedLen > 1e-3f)
				{
					DirDotSum += double(FVector2f::DotProduct(Baked / BakedLen, Dir));
					++DirDotCount;
				}
			}

			const bool bIsTopRim = UVRim.X > 0.5f;
			const bool bIsCorner = UVCell.Y > 0.5f;
			bCapTri = UVRim.Y > 0.5f;
			// 环标记的等价判据是"厚度轴在顶端"，两者不符说明 UV2.x 不是 cell_bby（通道错位）。
			if (bIsTopRim != (P.Z > TopRimZ - 1.0f)) ++RimMismatch;

			const uint32 Dst = Tri * 3u + K;
			Out.RestDir[int32(Dst)] = FVector4f(RestXY.X, RestXY.Y, Dir.X, Dir.Y);
			Out.CellFlags[int32(Dst)] =
				  (uint32(CellId) & 0x00FFFFFFu)
				| (bIsTopRim ? uint32(CSRockShell::ECellFlag::TopRim) : 0u)
				| (bIsCorner ? uint32(CSRockShell::ECellFlag::Corner) : 0u)
				| (bCapTri   ? uint32(CSRockShell::ECellFlag::CapTri) : 0u);
			Corner[K] = P;
		}

		// 绕序实测：盖三角在静止姿态是水平的，叉积必然是 ±Z。
		if (bCapTri)
		{
			const float NZ = FVector3f::CrossProduct(Corner[2] - Corner[0], Corner[1] - Corner[0]).Z;
			if (NZ > 0.0f) ++CapUp; else if (NZ < 0.0f) ++CapDown;
		}
	}

	Out.bFlipWinding = CapDown > CapUp;
	Out.DirAgreement = DirDotCount > 0 ? float(DirDotSum / double(DirDotCount)) : 0.0f;

	UE_LOG(LogCSRockShell, Log,
		TEXT("[CSRockShell] 图案 %s：%u 三角 / %u 顶点（源 %u，焊掉 %u）/ %u 胞腔；")
		TEXT(" 跨度 %.0f × %.0f cm、厚 %.1f cm；UV %d 通道、CellId 上界 %.1f；")
		TEXT(" 盖三角朝上 %d / 朝下 %d ⇒ %s；dir 与烘焙件平均点积 %.4f；环标记不符 %d 个。"),
		*PatternMesh->GetPathName(), Out.TriangleCount, Out.VertexCount, NumSourceVerts,
		Out.VertexCount > NumSourceVerts ? Out.VertexCount - NumSourceVerts : 0u, Out.CellCount,
		Size.X, Size.Y, Size.Z, Out.NumUVChannels, Out.MaxCellId,
		CapUp, CapDown, Out.bFlipWinding ? TEXT("kernel 取负") : TEXT("直接用"),
		Out.DirAgreement, RimMismatch);

	if (RimMismatch > 0)
	{
		UE_LOG(LogCSRockShell, Warning,
			TEXT("[CSRockShell] %d 个顶点的 UV2.x 与它所在的环不符 —— UV2.y（逐三角的盖/裙标记）")
			TEXT(" 与 UV2.x（cell_bby = bIsTopRim）多半被通道错位换掉了，壳的厚度层会失效。"), RimMismatch);
	}
	if (Out.DirAgreement < 0.9f)
	{
		UE_LOG(LogCSRockShell, Warning,
			TEXT("[CSRockShell] 现算的 DirToCentroid 与烘焙件平均点积只有 %.4f（期望 ≈ +1）——")
			TEXT(" 导入器多半翻了一个平面轴。kernel 用的是现算的那份，仍然正确；但绕序也要一起看。"),
			Out.DirAgreement);
	}
	return Out;
}

class FCSGroundRockShellCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSGroundRockShellCS);
	SHADER_USE_PARAMETER_STRUCT(FCSGroundRockShellCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// 名字必须与 CSGroundShaperField.ush 里的声明逐字相同：这两个是与地面位移 pass 共享的
		// 那一份高度场的输入，改名等于把绑定悄悄拆掉（不报错，只是地面永远读成平的）。
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GroundShaperParams)
		SHADER_PARAMETER(uint32, GroundShaperCount)

		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, RockShellRestDir)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RockShellCellFlags)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float2>, RockShellCentroids)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RockShellGroundColors)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RW_RockShellPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_RockShellTangents)

		SHADER_PARAMETER(uint32, RockShellTriangleCount)
		SHADER_PARAMETER(FVector2f, RockShellPatternCentre)
		SHADER_PARAMETER(FVector2f, RockShellWorldCentre)
		SHADER_PARAMETER(float, RockShellScale)
		SHADER_PARAMETER(float, RockShellCellRadiusCm)
		SHADER_PARAMETER(FVector2f, RockShellDomainMin)
		SHADER_PARAMETER(FVector2f, RockShellDomainMax)
		SHADER_PARAMETER(float, RockShellWindingSign)
		SHADER_PARAMETER(FVector2f, RockShellGroundOriginXY)
		SHADER_PARAMETER(float, RockShellGroundCellSize)
		SHADER_PARAMETER(FUintVector2, RockShellGroundVerts)
		SHADER_PARAMETER(float, RockShellGroundBaseZ)
		SHADER_PARAMETER(float, RockShellSlopeLo)
		SHADER_PARAMETER(float, RockShellSlopeHi)
		SHADER_PARAMETER(float, RockShellRoadFade)
		SHADER_PARAMETER(float, RockShellRoadSink)
		SHADER_PARAMETER(float, RockShellCellJitter)
		SHADER_PARAMETER(float, RockShellCellRelief)
		SHADER_PARAMETER(float, RockShellNoiseAmp)
		SHADER_PARAMETER(float, RockShellNoiseFreq)
		SHADER_PARAMETER(uint32, RockShellSeed)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), CSRockShell_GroupSizeX);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCSGroundRockShellCS, "/Plugin/PCGPlugins/Shaders/Private/CSGroundRockShell.usf", "DisplaceRockShellCS", SF_Compute);
}

namespace CSRockShell
{
// ⚠️ 路径里的 `rocky_terrain_shell` 出现**两次**，这不是笔误：`TinyGladeImportRockShell.py`
// 同时给了 destination_path 与 destination_name，而 Interchange 的 glTF 管线还会自己再套一层
// `<name>/StaticMeshes/`，三者叠出这个形状。实测落点就是这里（2026-08-30 首次导入核对过）。
// 不去搬资产：搬完再跑一次那个脚本又会在这里重建一份，两份都在的话谁都不知道哪份是活的。
const TCHAR* const DefaultPatternAssetPath =
	TEXT("/Game/TinyGlade/Meshes/rocky_terrain_shell/rocky_terrain_shell/StaticMeshes/rocky_terrain_shell.rocky_terrain_shell");

const FPattern& GetSharedPattern(UStaticMesh* PatternMesh)
{
	// 按资产缓存：抽一次要遍历 148,794 个顶点槽 + 逐胞腔求质心，而重建路径每次都会走到。
	// 游戏线程独占（所有调用方都在 actor 的重建路径上），不需要锁。
	static TWeakObjectPtr<UStaticMesh> CachedFor;
	static TSharedPtr<FPattern> Cached;
	static const FPattern Empty;

	if (!PatternMesh) return Empty;
	if (Cached.IsValid() && CachedFor.Get() == PatternMesh) return *Cached;

	Cached = MakeShared<FPattern>(CSRockShell_ExtractPattern(PatternMesh));
	CachedFor = PatternMesh;
	return *Cached;
}

bool BuildMesh(
	UCSMesh* ShellMesh, const FPattern& Pattern, const FBox& HardWorldBounds,
	const FVector2f& WorldCentre, float Scale, float UVWorldPeriod)
{
	if (!ShellMesh || !Pattern.IsValid()) return false;

	// --- 1) 声明流集：标准集 + 三条 aux。**在第一次分配之前声明**，下一次分配就按它建。 ---
	FCSMeshStreamLayout Layout;
	{
		FCSGpuStreamDesc Desc;
		Desc.DebugName = TEXT("CSRockShell.RestDir");
		Desc.Role = ECSGpuStreamRole::AuxVertex;
		Desc.BytesPerElement = sizeof(FVector4f);
		Desc.ElementsPerUnit = 1;
		Desc.CountSource = ECSGpuCountSource::PerVertex;
		Desc.SrvFormat = PF_A32B32G32R32F;            // -> Buffer<float4>
		Desc.VfType = VET_None;                       // aux 不进顶点工厂
		Desc.TexCoordIndex = uint8(EAuxSlot::RestDir);
		Layout.ExtraStreams.Add(Desc);

		Desc.DebugName = TEXT("CSRockShell.CellFlags");
		Desc.BytesPerElement = sizeof(uint32);
		Desc.SrvFormat = PF_R32_UINT;                 // -> Buffer<uint>
		Desc.TexCoordIndex = uint8(EAuxSlot::CellFlags);
		Layout.ExtraStreams.Add(Desc);

		// 逐**胞腔**而不是逐顶点 ⇒ Fixed：ElementsPerUnit 就是整个元素数（609 个 float2）。
		Desc.DebugName = TEXT("CSRockShell.Centroids");
		Desc.BytesPerElement = sizeof(FVector2f);
		Desc.ElementsPerUnit = FMath::Max(Pattern.CellCount, 1u);
		Desc.CountSource = ECSGpuCountSource::Fixed;
		Desc.SrvFormat = PF_G32R32F;                  // -> Buffer<float2>
		Desc.TexCoordIndex = uint8(EAuxSlot::Centroids);
		Layout.ExtraStreams.Add(Desc);
	}
	if (!ShellMesh->SetStreamLayoutSync(Layout))
	{
		// AddStream 的槽位冲撞只返回 false 且不写日志 —— 这里必须自己喊出来，否则症状是
		// "壳一个三角都不出现"，而没有任何东西指向槽位。
		UE_LOG(LogCSRockShell, Warning, TEXT("[CSRockShell] aux 流声明被拒（槽位 32/33/34 冲撞或显存预检不过），岩壳关闭。"));
		return false;
	}

	// --- 2) 基底几何：三角汤，索引 0..V-1，位置先摆在静止 XY（Z 由第一趟 Displace 写）。---
	// 走 CopyFromMeshSnapshot 是因为它把计数器 + indirect args + 索引一次性写对；自己拼
	// 这几样等于把 AddSetCountersPass 那套约定再抄一遍，而抄漏的那份不会报错，只是不画。
	FCSGpuMeshCPUData Snapshot;
	Snapshot.SourceSpace = FCSGpuMeshCPUData::ESpace::World;
	Snapshot.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerVertex;
	Snapshot.NumTexCoordChannels = 1;
	const int32 NumVerts = int32(Pattern.VertexCount);
	Snapshot.Positions.SetNumUninitialized(NumVerts);
	Snapshot.Normals.SetNumUninitialized(NumVerts);
	Snapshot.Tangents.SetNumUninitialized(NumVerts);
	Snapshot.TexCoords().SetNumUninitialized(NumVerts);
	Snapshot.Colors.SetNumUninitialized(NumVerts);
	Snapshot.Indices.SetNumUninitialized(NumVerts);
	const FVector2f PatternCentre = Pattern.Centre();
	const float UVPeriod = FMath::Max(UVWorldPeriod, 1.0f);
	for (int32 V = 0; V < NumVerts; ++V)
	{
		const FVector2f World = WorldCentre + (FVector2f(Pattern.RestDir[V].X, Pattern.RestDir[V].Y) - PatternCentre) * Scale;
		Snapshot.Positions[V] = FVector3f(World.X, World.Y, float(HardWorldBounds.Min.Z));
		Snapshot.Normals[V] = FVector3f(0.0f, 0.0f, 1.0f);
		Snapshot.Tangents[V] = FVector3f(1.0f, 0.0f, 0.0f);
		Snapshot.TexCoords()[V] = FVector2f(World.X / UVPeriod, World.Y / UVPeriod);
		// 顶点色 R = bIsCapTri（1 = 盖 / 0 = 裙），通道字典在 CSGroundRockShell.h。
		// 写在 CPU 这一步而不是 kernel 里：盖/裙是**烘死的图案属性**，每趟披挂重写它是白付，
		// 而且 Displace 只声明了 Positions/Tangents 两个 UAV，多写一条流就要动那份声明。
		// ❗ 它只给**材质**用，位移一行不读 —— 同 TG：`Triangle.is_top` 在
		// `displace_rocky_terrain.cs` 里从没被读过，只原样搬运给光栅化 / 材质。
		const bool bCapTriVert = (Pattern.CellFlags[V] & uint32(ECellFlag::CapTri)) != 0u;
		Snapshot.Colors[V] = FVector4f(
			bCapTriVert ? VertexColor::CapValue : VertexColor::SkirtValue, 1.0f, 1.0f, 1.0f);
		Snapshot.Indices[V] = uint32(V);
	}
	if (!UCSMeshOps::CopyFromMeshSnapshot(ShellMesh, Snapshot))
	{
		UE_LOG(LogCSRockShell, Warning, TEXT("[CSRockShell] 基底几何上传失败（多半是显存预检拒了 %d 顶点），岩壳关闭。"), NumVerts);
		return false;
	}

	// --- 3) 图案进 aux 流 + 把包围盒按地面矩形写死。 ---
	const bool bUploaded = ShellMesh->EditMeshSync([&Pattern, &HardWorldBounds](FCSMeshEditContext& Context)
	{
		auto Upload = [&Context](EAuxSlot Slot, const void* Data, uint64 Bytes, uint32 Alignment)
		{
			FRDGBufferRef Stream = Context.Find(ECSGpuStreamRole::AuxVertex, uint8(Slot));
			if (!Stream || Bytes == 0) return;
			// GraphBuilder.Alloc + Memcpy：上传源必须活到图执行，直接指 TArray 会悬空
			// （这个 lambda 早就返回了，而 QueueBufferUpload 只记下指针）。
			void* Copy = Context.GraphBuilder.Alloc(Bytes, Alignment);
			FMemory::Memcpy(Copy, Data, Bytes);
			Context.GraphBuilder.QueueBufferUpload(Stream, Copy, Bytes, ERDGInitialDataFlags::None);
		};
		Upload(EAuxSlot::RestDir, Pattern.RestDir.GetData(), uint64(Pattern.RestDir.Num()) * sizeof(FVector4f), 16);
		Upload(EAuxSlot::CellFlags, Pattern.CellFlags.GetData(), uint64(Pattern.CellFlags.Num()) * sizeof(uint32), 4);
		Upload(EAuxSlot::Centroids, Pattern.Centroids.GetData(), uint64(Pattern.Centroids.Num()) * sizeof(FVector2f), 8);

		// **写死包围盒**：kernel 用 NaN 关掉看不见的三角，NaN 会污染任何"从顶点算出来"的
		// 包围盒（计划已定这是对的做法）。CopyFromMeshSnapshot 刚按静止姿态算过一份，
		// 那份是平的、也不含下沉量，必须在这里覆盖掉。
		Context.Resident.WorldBounds = HardWorldBounds;
	});
	if (!bUploaded)
	{
		UE_LOG(LogCSRockShell, Warning, TEXT("[CSRockShell] 图案上传 aux 流失败，岩壳关闭。"));
		return false;
	}
	return true;
}

bool Displace(
	UCSMesh* ShellMesh,
	const FCSMeshResidentRef& GroundResident,
	const FDisplaceParams& Params,
	const TArray<FVector4f>& ShaperParams)
{
	if (!ShellMesh || !GroundResident.IsValid()) return false;
	const FCSMeshResidentRef ShellResident = ShellMesh->GetResident();
	if (!ShellResident.IsValid() || !ShellResident->IsAllocated()) return false;

	const uint32 TriangleCount = ShellResident->VertexCapacity / 3u;
	if (TriangleCount == 0) return false;

	// 空 palette 也要跑：塑形物被删光时正是"全 0 高度场"把壳收掉的那一趟 —— 坡度降到阈下，
	// 每个三角自己写 NaN，不需要任何注销代码（裁决二）。
	TArray<FVector4f> UploadParams = ShaperParams;
	const int32 ShaperCount = UploadParams.Num() / CSGroundShaperField::Float4sPerShaper;
	if (UploadParams.IsEmpty()) UploadParams.Add(FVector4f::Zero());   // 结构化 buffer 不能是 0 长度

	ENQUEUE_RENDER_COMMAND(CSRockShellDisplace)(
		[Shell = ShellResident, Ground = GroundResident, Params, UploadParams, ShaperCount, TriangleCount](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSRockShell.Displace"));

			CSHelper::FRDGStructuredBufferRefs ShaperRefs = CSHelper::CreateUploadedStructuredBuffer<FVector4f>(
				GraphBuilder, UploadParams, TEXT("CSRockShell.ShaperParams"), false, true);
			if (!ShaperRefs.SRV)
			{
				GraphBuilder.Execute();
				return;
			}

			{
				// 两份常驻流各开一个编辑作用域：壳写位置/切线，地面只读色流。进出都走 mesh 层
				// 自己的入口，访问状态由 ~FCSMeshRenderThreadEdit 恢复 —— 手工写流再手工恢复
				// 是同一条规则的第二份拷贝，而漂掉的那份不报错，只会安静地不画。
				FCSMeshRenderThreadEdit ShellEdit(GraphBuilder, *Shell);
				FCSMeshRenderThreadEdit GroundEdit(GraphBuilder, *Ground);

				FRDGBufferRef Positions = ShellEdit->Positions();
				FRDGBufferRef Tangents = ShellEdit->Tangents();
				FRDGBufferRef RestDir = ShellEdit->Find(ECSGpuStreamRole::AuxVertex, uint8(EAuxSlot::RestDir));
				FRDGBufferRef CellFlags = ShellEdit->Find(ECSGpuStreamRole::AuxVertex, uint8(EAuxSlot::CellFlags));
				FRDGBufferRef Centroids = ShellEdit->Find(ECSGpuStreamRole::AuxVertex, uint8(EAuxSlot::Centroids));
				FRDGBufferRef GroundColors = GroundEdit->Colors();

				if (Positions && Tangents && RestDir && CellFlags && Centroids && GroundColors)
				{
					FCSGroundRockShellCS::FParameters* PassParams = GraphBuilder.AllocParameters<FCSGroundRockShellCS::FParameters>();
					PassParams->GroundShaperParams = ShaperRefs.SRV;
					PassParams->GroundShaperCount = uint32(FMath::Max(ShaperCount, 0));
					PassParams->RockShellRestDir = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(RestDir, PF_A32B32G32R32F));
					PassParams->RockShellCellFlags = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CellFlags, PF_R32_UINT));
					PassParams->RockShellCentroids = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Centroids, PF_G32R32F));
					PassParams->RockShellGroundColors = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(GroundColors, PF_R32_UINT));
					PassParams->RW_RockShellPositions = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Positions, PF_R32_FLOAT));
					PassParams->RW_RockShellTangents = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Tangents, PF_R32_UINT));
					PassParams->RockShellTriangleCount = TriangleCount;
					PassParams->RockShellPatternCentre = Params.PatternCentre;
					PassParams->RockShellWorldCentre = Params.WorldCentre;
					PassParams->RockShellScale = Params.Scale;
					PassParams->RockShellCellRadiusCm = FMath::Max(Params.CellRadiusCm, 1.0f);
					PassParams->RockShellDomainMin = Params.DomainMin;
					PassParams->RockShellDomainMax = Params.DomainMax;
					PassParams->RockShellWindingSign = Params.bFlipWinding ? -1.0f : 1.0f;
					PassParams->RockShellGroundOriginXY = Params.GroundOriginXY;
					PassParams->RockShellGroundCellSize = Params.GroundCellSize;
					PassParams->RockShellGroundVerts = FUintVector2(
						uint32(FMath::Max(Params.GroundVerts.X, 0)), uint32(FMath::Max(Params.GroundVerts.Y, 0)));
					PassParams->RockShellGroundBaseZ = Params.GroundBaseZ;
					PassParams->RockShellSlopeLo = Params.SlopeLo;
					// Hi 必须严格大于 Lo：smoothstep 在两者相等时是 0/0，整片 mask 变 NaN，
					// 而 NaN 会顺着 Relief 写进位置 —— 症状是"壳整个消失"，与坡度判据无关。
					PassParams->RockShellSlopeHi = FMath::Max(Params.SlopeHi, Params.SlopeLo + 1e-3f);
					PassParams->RockShellRoadFade = FMath::Max(Params.RoadFade, 0.0f);
					PassParams->RockShellRoadSink = Params.RoadSink;
					PassParams->RockShellCellJitter = FMath::Max(Params.CellJitter, 0.0f);
					PassParams->RockShellCellRelief = FMath::Max(Params.CellRelief, 0.0f);
					PassParams->RockShellNoiseAmp = FMath::Max(Params.NoiseAmp, 0.0f);
					PassParams->RockShellNoiseFreq = Params.NoiseFrequency;
					PassParams->RockShellSeed = Params.Seed;

					TShaderMapRef<FCSGroundRockShellCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSRockShell.Drape"), Shader, PassParams,
						FComputeShaderUtils::GetGroupCountWrapped(int32(TriangleCount), CSRockShell_GroupSizeX));
				}
			}

			GraphBuilder.Execute();
		});

	return true;
}
}
