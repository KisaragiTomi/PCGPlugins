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

/** 第零趟（路足迹模糊）的 2D 线程组边长，经 `ROCKSHELL_BLUR_GROUP` 喂给 kernel，两边只有这一处定义。 */
constexpr int32 CSRockShell_BlurGroupSize = 8;
/**
 * 路足迹模糊的单侧抽头上限（格）。50 cm 格距下 = 16 m 伸展，远超任何有意义的缓坡；
 * 它只挡"半径填错一个数量级"：1025² 的地面 × 两趟 × (2·32+1) 抽头 ≈ 1.4 亿次读，仍是一次性的几毫秒。
 */
constexpr int32 CSRockShell_BlurMaxTaps = 32;

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

	// --- 假倒角载荷（Docs/TinyGlade/CSRockShellEdgeBevel.md）：逐胞腔盖轮廓 → 逐源顶点平面距离 ---
	// 轮廓 = 只被一个盖三角使用的边；距离**只在图案平面（XY）里量** —— 裙面在静止姿态竖直
	// 展开约 3.1 m，运行时被高度替换压扁，量三维距离会让整张裙读成"远"。
	// 算法与离线脚本 Scripts/BakeRockShellBevelChannels.py 同式（那份烘 StaticMesh 资产，这份喂运行时壳）。
	TArray<float> SourceRimDistCm;
	{
		auto QuantKey = [](const FVector3f& P) -> uint64   // 0.1 cm 栅格上的位置身份
		{
			return (uint64(uint32(FMath::RoundToInt(P.X * 10.0f))) << 32)
				 |  uint64(uint32(FMath::RoundToInt(P.Y * 10.0f)));
		};
		TArray<TMap<TPair<uint64, uint64>, TPair<int32, FVector4f>>> CellEdges;
		CellEdges.SetNum(int32(Out.CellCount));
		for (uint32 Tri = 0; Tri < NumIndices / 3u; ++Tri)
		{
			const uint32 I0 = IndexView[int32(Tri * 3u)];
			if (I0 >= NumSourceVerts) continue;
			if (AttrVB.GetVertexUV(I0, CSRockShell_UVRim).Y <= 0.5f) continue;   // 裙三角不参与轮廓
			const int32 Cell = SourceCellId[int32(I0)];
			for (uint32 K = 0; K < 3u; ++K)
			{
				const uint32 A = IndexView[int32(Tri * 3u + K)];
				const uint32 B = IndexView[int32(Tri * 3u + (K + 1u) % 3u)];
				if (A >= NumSourceVerts || B >= NumSourceVerts) continue;
				const FVector3f PA = PosVB.VertexPosition(A);
				const FVector3f PB = PosVB.VertexPosition(B);
				uint64 KA = QuantKey(PA), KB = QuantKey(PB);
				if (KB < KA) Swap(KA, KB);
				TPair<int32, FVector4f>& Entry = CellEdges[Cell].FindOrAdd(
					TPair<uint64, uint64>(KA, KB), TPair<int32, FVector4f>(0, FVector4f(PA.X, PA.Y, PB.X, PB.Y)));
				++Entry.Key;
			}
		}
		TArray<TArray<FVector4f>> RimSegs;
		RimSegs.SetNum(int32(Out.CellCount));
		for (int32 Cell = 0; Cell < int32(Out.CellCount); ++Cell)
			for (const TPair<TPair<uint64, uint64>, TPair<int32, FVector4f>>& It : CellEdges[Cell])
				if (It.Value.Key == 1) RimSegs[Cell].Add(It.Value.Value);

		SourceRimDistCm.SetNumUninitialized(int32(NumSourceVerts));
		for (uint32 V = 0; V < NumSourceVerts; ++V)
		{
			const FVector2f P(PosVB.VertexPosition(V).X, PosVB.VertexPosition(V).Y);
			// ⚠️ 图案空间不封顶（BuildMesh 乘完 Scale 才按 RimDistMaxCm 归一）：
			// 在这里按 50 cm 截断，×0.35 的默认缩放会让"远"永远到不了 1，倒角糊满整个盖面。
			float Best = 1.0e9f;
			for (const FVector4f& S : RimSegs[SourceCellId[int32(V)]])
			{
				const FVector2f SegA(S.X, S.Y);
				const FVector2f SegD(S.Z - S.X, S.W - S.Y);
				const float T = FMath::Clamp(
					FVector2f::DotProduct(P - SegA, SegD) / FMath::Max(SegD.SizeSquared(), 1e-6f), 0.0f, 1.0f);
				Best = FMath::Min(Best, (SegA + SegD * T - P).Size());
			}
			SourceRimDistCm[int32(V)] = Best;
		}
	}

	// --- 逐三角展开 ---
	Out.TriangleCount = NumIndices / 3u;
	Out.VertexCount = Out.TriangleCount * 3u;
	Out.RestDir.SetNumUninitialized(int32(Out.VertexCount));
	Out.CellFlags.SetNumUninitialized(int32(Out.VertexCount));
	Out.BevelData.SetNumUninitialized(int32(Out.VertexCount));
	// 逐角的静止位置量化键（0.1 cm 栅格，与上面轮廓边表同一口径）。法线全平均的重合组按它分。
	TArray<FIntVector> CornerKey;
	CornerKey.SetNumUninitialized(int32(Out.VertexCount));

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
			// ⚠️ 键必须是**三维**的：盖的边界与裙的顶圈平面位置相同、厚度轴不同，二维键会把
			// 它们混进一组。而它们在运行时确实重合（同 XY、同 bIsTopRim ⇒ 同位移），
			// 该合的那一对靠三维键照样合得上，不该合的（底圈）自然分开。
			CornerKey[int32(Dst)] = FIntVector(
				FMath::RoundToInt(P.X * 10.0f), FMath::RoundToInt(P.Y * 10.0f), FMath::RoundToInt(P.Z * 10.0f));
			Out.RestDir[int32(Dst)] = FVector4f(RestXY.X, RestXY.Y, Dir.X, Dir.Y);
			Out.CellFlags[int32(Dst)] =
				  (uint32(CellId) & 0x00FFFFFFu)
				| (bIsTopRim ? uint32(CSRockShell::ECellFlag::TopRim) : 0u)
				| (bIsCorner ? uint32(CSRockShell::ECellFlag::Corner) : 0u)
				| (bCapTri   ? uint32(CSRockShell::ECellFlag::CapTri) : 0u);
			// 假倒角载荷：外向 = −DirToCentroid（Dir 实测指向质心），θ 用现算的 Dir 而不是烘焙件，
			// 与 RestDir 同一条"免疫导入器轴翻转"的理由。
			Out.BevelData[int32(Dst)] = FVector4f(
				bCapTri ? 1.0f : 0.0f,
				SourceRimDistCm[int32(Src)],
				FMath::Frac(float(CellId) * 0.6180339887f),
				FMath::Frac(FMath::Atan2(-Dir.Y, -Dir.X) / (2.0f * PI) + 1.0f));
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

	// --- 法线全平均的重合组：逐角 (偏移, 数量) + 展平的入射三角表 ---
	// 计数 → 前缀和 → 回填，全程三个平表。写成 TArray<TArray<uint32>> 的话是十几万次小分配，
	// 而这条路径在关卡加载 / 改格数 / 松手对齐时每次都会走到。
	int32 GroupCount = 0;
	int32 MaxIncidence = 0;
	int32 BoundaryEdges = 0;
	int32 NonManifoldEdges = 0;
	int32 CrossCellEdges = 0;
	{
		TMap<FIntVector, int32> KeyToGroup;
		KeyToGroup.Reserve(int32(Out.VertexCount));
		TArray<int32> CornerGroup;
		CornerGroup.SetNumUninitialized(int32(Out.VertexCount));
		for (uint32 V = 0; V < Out.VertexCount; ++V)
		{
			if (const int32* Found = KeyToGroup.Find(CornerKey[int32(V)]))
			{
				CornerGroup[int32(V)] = *Found;
			}
			else
			{
				CornerGroup[int32(V)] = GroupCount;
				KeyToGroup.Add(CornerKey[int32(V)], GroupCount);
				++GroupCount;
			}
		}
		// 名字带 Group 前缀：外层求胞腔质心那段已经有 Counts，重名会被 -WarningsAsErrors 拦下（C4456）。
		TArray<int32> GroupSizes;
		GroupSizes.Init(0, GroupCount);
		for (uint32 V = 0; V < Out.VertexCount; ++V) ++GroupSizes[CornerGroup[int32(V)]];
		TArray<int32> GroupOffsets;
		GroupOffsets.SetNumUninitialized(GroupCount);
		int32 Running = 0;
		for (int32 G = 0; G < GroupCount; ++G)
		{
			GroupOffsets[G] = Running;
			Running += GroupSizes[G];
			MaxIncidence = FMath::Max(MaxIncidence, GroupSizes[G]);
		}
		Out.IncidentTris.SetNumUninitialized(Running);
		TArray<int32> GroupCursor = GroupOffsets;
		// 一个角只属于一个组，所以"组里的角"与"组里的入射三角"是同一批 —— 直接写 V/3。
		// 同一个三角在同一组里出现两次只可能是它自己两个角重合（退化三角），面积为零、
		// 对面积加权和没有贡献，不需要去重。
		for (uint32 V = 0; V < Out.VertexCount; ++V) Out.IncidentTris[GroupCursor[CornerGroup[int32(V)]]++] = V / 3u;
		Out.IncidentRange.SetNumUninitialized(int32(Out.VertexCount) * 2);
		for (uint32 V = 0; V < Out.VertexCount; ++V)
		{
			const int32 G = CornerGroup[int32(V)];
			Out.IncidentRange[int32(V) * 2 + 0] = uint32(GroupOffsets[G]);
			Out.IncidentRange[int32(V) * 2 + 1] = uint32(GroupSizes[G]);
		}

		// --- 逐边邻接表（假倒角 v3）---
		// 复用同一份重合组：边 = 两端组 id 的无序对。壳是三角汤，按顶点序号配对永远配不上，
		// 而组 id 正是「哪些角在静止姿态重合」的答案 —— 与法线全平均那张表一个来源、一次量化。
		// 下标约定见 EAuxSlot::Neighbours：槽 k 对着角 k，与 kernel 里的垂距 d[k] 严格对齐。
		Out.Neighbours.Init(-1, int32(Out.TriangleCount) * 3);
		{
			// 值 = 该边**首次**出现的槽号（T*3+k）；配上对之后改写成 -1 当作「已用掉」，
			// 第三次及以后的入射就是非流形边，直接丢（两侧都留 -1 = 当外轮廓处理，不会画错，
			// 只是那条边不倒角）。收尾时仍持有正槽号的项 = 一次都没配上 = 石头外轮廓边。
			TMap<FIntPoint, int32> EdgeToCorner;
			EdgeToCorner.Reserve(int32(Out.TriangleCount) * 3);
			for (uint32 T = 0; T < Out.TriangleCount; ++T)
			{
				for (uint32 K = 0; K < 3u; ++K)
				{
					const int32 GA = CornerGroup[int32(T * 3u + (K + 1u) % 3u)];
					const int32 GB = CornerGroup[int32(T * 3u + (K + 2u) % 3u)];
					if (GA == GB) continue;                       // 退化边：两端重合，没有方向可言
					const FIntPoint Key(FMath::Min(GA, GB), FMath::Max(GA, GB));
					const int32 Slot = int32(T * 3u + K);
					if (int32* First = EdgeToCorner.Find(Key))
					{
						if (*First >= 0)
						{
							const int32 Other = *First / 3;
							Out.Neighbours[Slot] = Other;
							Out.Neighbours[*First] = int32(T);
							*First = -1;
							// 跨胞腔边 = 两侧属于不同石头。假倒角 v3 会在这种边上把法线混向邻面 ⇒
							// 石头与石头之间不再是硬边。计数打进日志，好让「组不跨胞腔」这条
							// 早先的实测结论保持可证伪（CellId 在打包字的低 24 位）。
							if ((Out.CellFlags[int32(T * 3u)] & 0x00FFFFFFu)
								!= (Out.CellFlags[Other * 3] & 0x00FFFFFFu)) ++CrossCellEdges;
						}
						else ++NonManifoldEdges;
					}
					else EdgeToCorner.Add(Key, Slot);
				}
			}
			for (const TPair<FIntPoint, int32>& It : EdgeToCorner) if (It.Value >= 0) ++BoundaryEdges;
		}
	}

	UE_LOG(LogCSRockShell, Log,
		TEXT("[CSRockShell] 图案 %s：%u 三角 / %u 顶点（源 %u，焊掉 %u）/ %u 胞腔；")
		TEXT(" 跨度 %.0f × %.0f cm、厚 %.1f cm；UV %d 通道、CellId 上界 %.1f；")
		TEXT(" 盖三角朝上 %d / 朝下 %d ⇒ %s；dir 与烘焙件平均点积 %.4f；环标记不符 %d 个；")
		TEXT(" 法线重合组 %d 个（最大入射 %d、平均 %.2f）；")
		TEXT(" 逐边邻接：轮廓边 %d 条、非流形丢弃 %d 条、跨胞腔 %d 条。"),
		*PatternMesh->GetPathName(), Out.TriangleCount, Out.VertexCount, NumSourceVerts,
		Out.VertexCount > NumSourceVerts ? Out.VertexCount - NumSourceVerts : 0u, Out.CellCount,
		Size.X, Size.Y, Size.Z, Out.NumUVChannels, Out.MaxCellId,
		CapUp, CapDown, Out.bFlipWinding ? TEXT("kernel 取负") : TEXT("直接用"),
		Out.DirAgreement, RimMismatch,
		GroupCount, MaxIncidence, GroupCount > 0 ? double(Out.VertexCount) / double(GroupCount) : 0.0,
		BoundaryEdges, NonManifoldEdges, CrossCellEdges);

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
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, RockShellRoadField)
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
		SHADER_PARAMETER(float, RockShellRoadSink)
		SHADER_PARAMETER(float, RockShellCellJitter)
		SHADER_PARAMETER(float, RockShellCellRelief)
		SHADER_PARAMETER(float, RockShellReliefFloor)
		SHADER_PARAMETER(float, RockShellCellExpand)
		SHADER_PARAMETER(float, RockShellSkirtTilt)
		SHADER_PARAMETER(float, RockShellRiseMultiplier)
		SHADER_PARAMETER(float, RockShellRiseNoiseAmp)
		SHADER_PARAMETER(float, RockShellRiseNoiseFreq)
		SHADER_PARAMETER(float, RockShellRiseExtend)
		SHADER_PARAMETER(float, RockShellEdgeCeiling)
		SHADER_PARAMETER(float, RockShellPeakHeight)
		SHADER_PARAMETER(float, RockShellBaseLift)
		SHADER_PARAMETER(float, RockShellBaseSink)
		SHADER_PARAMETER(float, RockShellNoiseAmp)
		SHADER_PARAMETER(float, RockShellNoiseFreq)
		SHADER_PARAMETER(float, RockShellChipAmount)
		SHADER_PARAMETER(float, RockShellChipFreq)
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

/**
 * 第二趟：法线**全平均**（用户裁决 2026-09-03，不设夹角阈值）。一线程一角。
 *
 * 为什么不是在第一趟里顺手算：面法线要三个角的**最终**位置，而一个角的邻居分散在别的线程组里，
 * 第一趟跑到写切线那一步时它们不一定写完。分成两趟由 RDG 靠 Positions 的 UAV→SRV 转换定序，
 * 比手工插 barrier 少一份会漂掉的规则。
 *
 * ⚠️ 参数结构里**每一条都被 kernel 真读**。`FComputeShaderUtils::AddPass` 会调
 * `ClearUnusedGraphResources` 把没绑上的参数置空、连带抹掉那条依赖边 —— 藤蔓 SC pass 就是
 * 这么挂过 GPU 的。多声明一条自己不读的 SRV 在这里是有代价的，别加。
 */
class FCSRockShellAverageNormalsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSRockShellAverageNormalsCS);
	SHADER_USE_PARAMETER_STRUCT(FCSRockShellAverageNormalsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, RockShellPositionsRO)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RockShellIncidentRange)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RockShellIncidentTris)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_RockShellTangents)
		SHADER_PARAMETER(uint32, RockShellVertexCount)
		SHADER_PARAMETER(float, RockShellWindingSign)
		SHADER_PARAMETER(float, RockShellSmoothCosThreshold)
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

IMPLEMENT_GLOBAL_SHADER(FCSRockShellAverageNormalsCS, "/Plugin/PCGPlugins/Shaders/Private/CSGroundRockShell.usf", "AverageRockShellNormalsCS", SF_Compute);

/**
 * 第三趟：假倒角 v3 的逐像素载荷（垂距 one-hot + 三个邻面法线）写进 UV1..UV5。
 * kernel 与通道字典见 `CSGroundRockShell.usf` 的第三趟小节与头文件的 `namespace TexCoord`。
 *
 * ⚠️ 与第二趟同一条规矩：参数结构里每一条都被 kernel 真读。`ClearUnusedGraphResources`
 * 会把没绑上的参数连同那条 RDG 依赖边一起抹掉，多声明一条不读的 SRV 是要付代价的。
 */
class FCSRockShellBevelPayloadCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSRockShellBevelPayloadCS);
	SHADER_USE_PARAMETER_STRUCT(FCSRockShellBevelPayloadCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, RockShellPositionsRO)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<int>, RockShellNeighbours)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RW_RockShellTexCoords)
		SHADER_PARAMETER(uint32, RockShellPayloadTriCount)
		SHADER_PARAMETER(uint32, RockShellTexCoordStride)
		SHADER_PARAMETER(float, RockShellWindingSign)
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

IMPLEMENT_GLOBAL_SHADER(FCSRockShellBevelPayloadCS, "/Plugin/PCGPlugins/Shaders/Private/CSGroundRockShell.usf", "RockShellBevelPayloadCS", SF_Compute);

/**
 * 第零趟 X：色流 R × RoadFade 截成足迹，再沿 X 做高斯。一线程一地面格点。
 * 为什么先截再糊、以及核的口径，见 `CSGroundRockShell.usf` 的「第零趟」小节。
 *
 * ⚠️ 与第二、三趟同一条规矩：参数结构里每一条都被 kernel 真读。
 */
class FCSRockShellRoadBlurXCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSRockShellRoadBlurXCS);
	SHADER_USE_PARAMETER_STRUCT(FCSRockShellRoadBlurXCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RockShellGroundColors)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RW_RockShellRoadBlur)
		SHADER_PARAMETER(FUintVector2, RockShellGroundVerts)
		SHADER_PARAMETER(float, RockShellRoadFade)
		SHADER_PARAMETER(uint32, RockShellBlurTaps)
		SHADER_PARAMETER(float, RockShellBlurInvTwoSigmaSq)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("ROCKSHELL_BLUR_GROUP"), CSRockShell_BlurGroupSize);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCSRockShellRoadBlurXCS, "/Plugin/PCGPlugins/Shaders/Private/CSGroundRockShell.usf", "BlurRockShellRoadXCS", SF_Compute);

/** 第零趟 Y：读 X 趟的结果沿 Y 做高斯，写出披挂要采的 `RockShellRoadField`。 */
class FCSRockShellRoadBlurYCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSRockShellRoadBlurYCS);
	SHADER_USE_PARAMETER_STRUCT(FCSRockShellRoadBlurYCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, RockShellRoadBlurRO)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RW_RockShellRoadBlur)
		SHADER_PARAMETER(FUintVector2, RockShellGroundVerts)
		SHADER_PARAMETER(uint32, RockShellBlurTaps)
		SHADER_PARAMETER(float, RockShellBlurInvTwoSigmaSq)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("ROCKSHELL_BLUR_GROUP"), CSRockShell_BlurGroupSize);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCSRockShellRoadBlurYCS, "/Plugin/PCGPlugins/Shaders/Private/CSGroundRockShell.usf", "BlurRockShellRoadYCS", SF_Compute);

/**
 * 录第零趟：色流 → 模糊足迹。返回披挂要采的那块 buffer（本图内的临时 buffer，图执行完就还池）。
 *
 * 格点数退化（< 2×2）或色流装不下整张格时不糊，直接给一块清零的场 —— 披挂读到 0 就是
 * "没有路"，与旧写法在同一情形下 `CSRockShell_SampleRoad` 早退返回 0 是同一个结果。
 * ⚠️ **不能不写**：没被写过的 RDG buffer 被读，内容是池子上一位租客的字节。
 */
FRDGBufferRef CSRockShell_AddRoadBlurPasses(
	FRDGBuilder& GraphBuilder, FRDGBufferRef GroundColors, const CSRockShell::FDisplaceParams& Params)
{
	const FIntPoint Verts(FMath::Max(Params.GroundVerts.X, 0), FMath::Max(Params.GroundVerts.Y, 0));
	const uint64 NumVerts = uint64(Verts.X) * uint64(Verts.Y);

	FRDGBufferRef Field = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(float), uint32(FMath::Max<uint64>(NumVerts, 1))),
		TEXT("CSRockShell.RoadField"));

	const bool bGridUsable = Verts.X >= 2 && Verts.Y >= 2 && NumVerts <= uint64(MAX_uint32)
		&& GroundColors && uint64(GroundColors->Desc.NumElements) >= NumVerts;
	if (!bGridUsable)
	{
		AddClearUAVFloatPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Field, PF_R32_FLOAT)), 0.0f);
		return Field;
	}

	// 半径 → 抽头与 σ（都以**格**为单位）：伸展 = ceil(半径 / 格距)，σ = 半径 / 3（核截在 3σ）。
	// 半径为 0 时一个抽头都没有，这时 1/(2σ²) 是 1/0 —— 给 0，kernel 里 K = 0 那一项的
	// exp(−0 × inv) 才不会变成 exp(NaN)。
	const float RadiusCells = FMath::Clamp(
		Params.RoadBlurRadius / FMath::Max(Params.GroundCellSize, 1e-3f), 0.0f, float(CSRockShell_BlurMaxTaps));
	const uint32 Taps = uint32(FMath::CeilToInt(RadiusCells));
	const float Sigma = RadiusCells / 3.0f;
	const float InvTwoSigmaSq = Taps > 0u ? 1.0f / FMath::Max(2.0f * Sigma * Sigma, 1e-6f) : 0.0f;

	const FUintVector2 GridVerts(uint32(Verts.X), uint32(Verts.Y));
	const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(Verts, CSRockShell_BlurGroupSize);

	FRDGBufferRef Temp = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(float), uint32(NumVerts)), TEXT("CSRockShell.RoadBlurX"));

	FCSRockShellRoadBlurXCS::FParameters* XParams = GraphBuilder.AllocParameters<FCSRockShellRoadBlurXCS::FParameters>();
	XParams->RockShellGroundColors = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(GroundColors, PF_R32_UINT));
	XParams->RW_RockShellRoadBlur = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Temp, PF_R32_FLOAT));
	XParams->RockShellGroundVerts = GridVerts;
	// RoadFade 在**模糊之前**乘（顺序为什么承重见 usf 的「第零趟」）。
	XParams->RockShellRoadFade = FMath::Max(Params.RoadFade, 0.0f);
	XParams->RockShellBlurTaps = Taps;
	XParams->RockShellBlurInvTwoSigmaSq = InvTwoSigmaSq;
	TShaderMapRef<FCSRockShellRoadBlurXCS> XShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSRockShell.RoadBlurX"), XShader, XParams, GroupCount);

	FCSRockShellRoadBlurYCS::FParameters* YParams = GraphBuilder.AllocParameters<FCSRockShellRoadBlurYCS::FParameters>();
	YParams->RockShellRoadBlurRO = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Temp, PF_R32_FLOAT));
	YParams->RW_RockShellRoadBlur = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Field, PF_R32_FLOAT));
	YParams->RockShellGroundVerts = GridVerts;
	YParams->RockShellBlurTaps = Taps;
	YParams->RockShellBlurInvTwoSigmaSq = InvTwoSigmaSq;
	TShaderMapRef<FCSRockShellRoadBlurYCS> YShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSRockShell.RoadBlurY"), YShader, YParams, GroupCount);

	return Field;
}
}

namespace CSRockShell
{
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

	// --- 1) 声明流集：标准集 + 六条 aux。**在第一次分配之前声明**，下一次分配就按它建。 ---
	FCSMeshStreamLayout Layout;
	// 假倒角 v3 的载荷走 UV1..UV5（字典见 CSGroundRockShell.h 的 namespace TexCoord），
	// 由第三趟 kernel 每趟披挂重写。这里只声明组数 —— 单条流交错加宽，不是另起流。
	Layout.NumTexCoordSets = uint32(TexCoord::NumSets);
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

		// 法线全平均的拓扑。逐角 2 个 uint（偏移 + 数量）⇒ 回到 PerVertex。
		Desc.DebugName = TEXT("CSRockShell.IncidentRange");
		Desc.BytesPerElement = sizeof(uint32);
		Desc.ElementsPerUnit = 2;
		Desc.CountSource = ECSGpuCountSource::PerVertex;
		Desc.SrvFormat = PF_R32_UINT;                 // -> Buffer<uint>
		Desc.TexCoordIndex = uint8(EAuxSlot::IncidentRange);
		Layout.ExtraStreams.Add(Desc);

		// 展平的入射三角表：长度由拓扑决定、与顶点数不成整数比 ⇒ Fixed。
		Desc.DebugName = TEXT("CSRockShell.IncidentTris");
		Desc.ElementsPerUnit = FMath::Max(uint32(Pattern.IncidentTris.Num()), 1u);
		Desc.CountSource = ECSGpuCountSource::Fixed;
		Desc.TexCoordIndex = uint8(EAuxSlot::IncidentTris);
		Layout.ExtraStreams.Add(Desc);

		// 逐边邻接表：逐**三角** 3 个 int32 ⇒ 与顶点数不成整数比，同 IncidentTris 走 Fixed。
		Desc.DebugName = TEXT("CSRockShell.Neighbours");
		Desc.BytesPerElement = sizeof(int32);
		Desc.ElementsPerUnit = FMath::Max(uint32(Pattern.Neighbours.Num()), 1u);
		Desc.SrvFormat = PF_R32_SINT;                 // -> Buffer<int>；-1 要能读成 -1，不能用 uint 视图
		Desc.TexCoordIndex = uint8(EAuxSlot::Neighbours);
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
	// v3：声明满 6 组。通道 0 在下面的循环里写死世界 UV，1..5 先清零 —— 它们的真值由
	// 第三趟 kernel 在每趟披挂之后写，CPU 这里给的只是「分配出来」和一个安全的中性值
	// （全 0 ⇒ 垂距 0 ⇒ 材质端会读成「贴边」，所以 kernel 那趟**不能跳过**，见 BevelPayload）。
	Snapshot.NumTexCoordChannels = TexCoord::NumSets;
	const int32 NumVerts = int32(Pattern.VertexCount);
	Snapshot.Positions.SetNumUninitialized(NumVerts);
	Snapshot.Normals.SetNumUninitialized(NumVerts);
	Snapshot.Tangents.SetNumUninitialized(NumVerts);
	Snapshot.TexCoords().SetNumUninitialized(NumVerts);
	for (int32 Set = 1; Set < TexCoord::NumSets; ++Set) Snapshot.TexCoordChannels[Set].SetNumZeroed(NumVerts);
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
		// 通道字典 v2：R = 盖/裙，G = 折痕距离（此处乘 Scale 换成世界口径再归一，1 = 远 = 中性），
		// B = 石头相位，A = 外向角。见 CSGroundRockShell.h 的 VertexColor 注释与 CSRockShellEdgeBevel.md。
		const FVector4f Bevel = Pattern.BevelData.IsValidIndex(V)
			? Pattern.BevelData[V]
			: FVector4f(bCapTriVert ? VertexColor::CapValue : VertexColor::SkirtValue, 1.0e9f, 1.0f, 1.0f);
		Snapshot.Colors[V] = FVector4f(
			Bevel.X,
			FMath::Clamp(Bevel.Y * Scale / VertexColor::RimDistMaxCm, 0.0f, 1.0f),
			Bevel.Z, Bevel.W);
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
		Upload(EAuxSlot::IncidentRange, Pattern.IncidentRange.GetData(), uint64(Pattern.IncidentRange.Num()) * sizeof(uint32), 4);
		Upload(EAuxSlot::IncidentTris, Pattern.IncidentTris.GetData(), uint64(Pattern.IncidentTris.Num()) * sizeof(uint32), 4);
		Upload(EAuxSlot::Neighbours, Pattern.Neighbours.GetData(), uint64(Pattern.Neighbours.Num()) * sizeof(int32), 4);

		// **写死包围盒**：kernel 用 NaN 关掉看不见的三角，NaN 会污染任何"从顶点算出来"的
		// 包围盒（计划已定这是对的做法）。CopyFromMeshSnapshot 刚按静止姿态算过一份，
		// 那份是平的、也不含下沉量，必须在这里覆盖掉。
		Context.SetWorldBounds(HardWorldBounds);
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
				FRDGBufferRef IncidentRange = ShellEdit->Find(ECSGpuStreamRole::AuxVertex, uint8(EAuxSlot::IncidentRange));
				FRDGBufferRef IncidentTris = ShellEdit->Find(ECSGpuStreamRole::AuxVertex, uint8(EAuxSlot::IncidentTris));
				FRDGBufferRef Neighbours = ShellEdit->Find(ECSGpuStreamRole::AuxVertex, uint8(EAuxSlot::Neighbours));
				FRDGBufferRef ShellTexCoords = ShellEdit->TexCoords();
				FRDGBufferRef GroundColors = GroundEdit->Colors();

				if (Positions && Tangents && RestDir && CellFlags && Centroids && GroundColors)
				{
					// 第零趟：色流 → 模糊足迹。**必须在披挂之前** —— 披挂采的就是它；定序由 RDG 靠
					// 这块临时 buffer 的 UAV→SRV 转换自己排，与第二、三趟同一套，不插手工 barrier。
					const FRDGBufferRef RoadField = CSRockShell_AddRoadBlurPasses(GraphBuilder, GroundColors, Params);

					FCSGroundRockShellCS::FParameters* PassParams = GraphBuilder.AllocParameters<FCSGroundRockShellCS::FParameters>();
					PassParams->GroundShaperParams = ShaperRefs.SRV;
					PassParams->GroundShaperCount = uint32(FMath::Max(ShaperCount, 0));
					PassParams->RockShellRestDir = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(RestDir, PF_A32B32G32R32F));
					PassParams->RockShellCellFlags = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CellFlags, PF_R32_UINT));
					PassParams->RockShellCentroids = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Centroids, PF_G32R32F));
					PassParams->RockShellRoadField = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(RoadField, PF_R32_FLOAT));
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
					PassParams->RockShellRoadSink = Params.RoadSink;
					PassParams->RockShellCellJitter = FMath::Max(Params.CellJitter, 0.0f);
					PassParams->RockShellCellRelief = FMath::Max(Params.CellRelief, 0.0f);
					PassParams->RockShellReliefFloor = FMath::Clamp(Params.ReliefFloor, 0.0f, 1.0f);
					PassParams->RockShellCellExpand = FMath::Max(Params.CellExpand, 0.0f);
					PassParams->RockShellSkirtTilt = FMath::Max(Params.SkirtTilt, 0.0f);
					PassParams->RockShellRiseMultiplier = FMath::Max(Params.RiseMultiplier, 0.0f);
					PassParams->RockShellRiseNoiseAmp = FMath::Max(Params.RiseNoiseAmp, 0.0f);
					PassParams->RockShellRiseNoiseFreq = FMath::Max(Params.RiseNoiseFrequency, 0.0f);
					PassParams->RockShellRiseExtend = FMath::Max(Params.RiseExtend, 0.0f);
					// ⑧ 夹到 < 1：等于 1 的话"末端小于地面高度"这条不变量就退化成"小于等于"，
					// 石头的最外圈会和地面共面，出图上是一圈 z-fighting。
					PassParams->RockShellEdgeCeiling = FMath::Clamp(Params.EdgeCeiling, 0.0f, 0.999f);
					PassParams->RockShellPeakHeight = FMath::Max(Params.PeakHeight, 1.0f);
					PassParams->RockShellBaseLift = FMath::Max(Params.BaseLift, 0.0f);
					PassParams->RockShellBaseSink = FMath::Max(Params.BaseSink, 0.0f);
					PassParams->RockShellNoiseAmp = FMath::Max(Params.NoiseAmp, 0.0f);
					PassParams->RockShellChipAmount = FMath::Max(Params.ChipAmount, 0.0f);
					PassParams->RockShellChipFreq = Params.ChipFrequency;
					PassParams->RockShellNoiseFreq = Params.NoiseFrequency;
					PassParams->RockShellSeed = Params.Seed;

					TShaderMapRef<FCSGroundRockShellCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSRockShell.Drape"), Shader, PassParams,
						FComputeShaderUtils::GetGroupCountWrapped(int32(TriangleCount), CSRockShell_GroupSizeX));

					// 第二趟：按重合组把面法线全平均。**必须在披挂之后**，读的正是上一趟刚写出来的位置。
					// 拓扑流缺席时安静跳过 —— 那时留下的是第一趟写的面法线（硬边），是降级不是不画。
					if (Params.bSmoothNormals && IncidentRange && IncidentTris)
					{
						const uint32 VertexCount = TriangleCount * 3u;
						FCSRockShellAverageNormalsCS::FParameters* AvgParams =
							GraphBuilder.AllocParameters<FCSRockShellAverageNormalsCS::FParameters>();
						AvgParams->RockShellPositionsRO = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Positions, PF_R32_FLOAT));
						AvgParams->RockShellIncidentRange = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(IncidentRange, PF_R32_UINT));
						AvgParams->RockShellIncidentTris = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(IncidentTris, PF_R32_UINT));
						AvgParams->RW_RockShellTangents = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Tangents, PF_R32_UINT));
						AvgParams->RockShellVertexCount = VertexCount;
						AvgParams->RockShellWindingSign = Params.bFlipWinding ? -1.0f : 1.0f;
						// 夹角阈值 → cos。夹到 [0,180]：负角会让 cos > 1 ⇒ 一个邻面都不收，
						// 症状是整壳退回面法线，而参数面上看不出哪里错了。
						AvgParams->RockShellSmoothCosThreshold = FMath::Cos(
							FMath::DegreesToRadians(FMath::Clamp(Params.SmoothAngleDeg, 0.0f, 180.0f)));

						TShaderMapRef<FCSRockShellAverageNormalsCS> AvgShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
						FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSRockShell.AverageNormals"), AvgShader, AvgParams,
							FComputeShaderUtils::GetGroupCountWrapped(int32(VertexCount), CSRockShell_GroupSizeX));
					}

					// 第三趟：假倒角 v3 的载荷。**同样必须在披挂之后** —— 垂距与邻面法线都随披挂变。
					// 与第二趟互不依赖（一个写切线、一个写 UV），RDG 会让它们并行；两者共同的前置
					// 只有 Positions 的 UAV→SRV 转换。
					// 邻接流缺席时安静跳过：那时 UV1..UV5 留在 BuildMesh 给的全 0 上，而 0 会被材质
					// 读成「贴边」⇒ 整壳满脸倒角。所以这里**必须**同时把消费侧关掉才算降级，
					// 目前的做法是让材质的 RockShellBevel 静态开关只在运行时壳的 MI 上打开。
					if (Neighbours && ShellTexCoords)
					{
						FCSRockShellBevelPayloadCS::FParameters* BevelParams =
							GraphBuilder.AllocParameters<FCSRockShellBevelPayloadCS::FParameters>();
						BevelParams->RockShellPositionsRO = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Positions, PF_R32_FLOAT));
						BevelParams->RockShellNeighbours = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Neighbours, PF_R32_SINT));
						BevelParams->RW_RockShellTexCoords = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(ShellTexCoords, PF_R32_FLOAT));
						BevelParams->RockShellPayloadTriCount = TriangleCount;
						BevelParams->RockShellTexCoordStride = uint32(TexCoord::NumSets) * 2u;
						BevelParams->RockShellWindingSign = Params.bFlipWinding ? -1.0f : 1.0f;

						TShaderMapRef<FCSRockShellBevelPayloadCS> BevelShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
						FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSRockShell.BevelPayload"), BevelShader, BevelParams,
							FComputeShaderUtils::GetGroupCountWrapped(int32(TriangleCount), CSRockShell_GroupSizeX));
					}
				}
			}

			GraphBuilder.Execute();
		});

	return true;
}
}
