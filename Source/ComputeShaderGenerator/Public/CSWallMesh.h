#pragma once

#include "CoreMinimal.h"
#include "CSGpuMeshTypes.h"    // FCSGpuMeshCPUData
#include "CSHouseProfile.h"    // FCSOpeningClipField —— 洞的解析裁剪场（UV1）

/**
 * 墙（房子的墙与样条墙共用）的三角形写手 —— 2026-09-22「统一房子和墙的逻辑」从 `CSHouseActor.cpp` 的匿名命名空间
 * 原样搬出来（原名 `FCSHouseMeshWriter`）。房体、承重柱、平屋顶面、样条墙都经它写三角形，口径只有这一份：
 *
 *   · 位置按 `World` 烘成世界空间（常驻流口径），法线 / 切线同旋转；
 *   · 面法线 = cross(B−A, C−A)（朝外），索引按**引擎绕序**交换 1/2 写（反过来整面墙朝里）；
 *   · 顶点色 = 通道字典（R 构件色号 / G 洞 Tag / B 洞形状 id，255 = 这块面板没有洞 / A 保留），见 `ACSHouseActor` 类注释；
 *   · UV0 = 墙面贴图坐标 / `UVScale`，UV1 = 洞的解析裁剪场（材质按它逐像素 discard，几何上不挖）。
 *
 * ⚠️ 房子那几条函数（`AddTri` / `AddQuad` / `AddBox` / `AddQuad4` / `AddWallPrism`）是逐字搬的：房体与承重柱的三角汤
 * 必须与搬之前**逐位相同**（`Wall.HouseBodyMatchesLegacy` 拿冻结的旧实现逐个 float 对照）。改它们 = 改房子的画面。
 */
namespace CSWallMesh
{
/** 世界 cm → UV 的平铺周期（房体与样条墙同一个贴图密度）。 */
inline constexpr float UVScale = 200.0f;

/** 构件色号里"墙"的值（= `ECSHousePart::Wall`；那个 UENUM 住在 `CSHouseActor.h`，墙的核心不去包含它，两边用 static_assert 钉住）。 */
inline constexpr uint8 PartWall = 0;

/**
 * 按通道字典组一份逐顶点语义色（R = 构件色号, G = 洞 Tag, B = 洞形状 id, A = 保留）。`Part` 是 `ECSHousePart` 的值。
 *
 * ⚠️ **B 的初值必须是 255（= 这块面板没有洞），不能是 0** —— 字典里 0 是一个**合法**的形状 id
 * （`ECSOpeningShape::Arch`）。屋面板曾经直接写 Semantic 绕过 `SetPanel`，于是整片屋面的 B 恒为 0，
 * 按字典读出来正好是"这块屋面上有个拱洞"。默认值放在**安全**那一侧。
 */
inline FVector4f Semantic(uint8 Part, uint8 Tag = 0)
{
	return FVector4f(float(Part) / 255.0f, float(Tag) / 255.0f, 1.0f, 0.0f);
}
}

/** 把三角形逐个写进快照的小写手。位置按 World 变换烘成世界空间（常驻流口径），
 *  法线/切线同旋转；面法线遵守常驻流绕序 cross(B-A, C-A)（CSMeshBuild.h）。 */
struct FCSWallMeshWriter
{
	FCSGpuMeshCPUData& S;
	FTransform World;

	/** 当前正在写的构件的逐顶点语义色（通道字典见 ACSHouseActor 类注释）。写一组几何前设一次。
	 *  初值走 `CSWallMesh::Semantic` 而不是零向量：B 的安全值是 255 不是 0，理由逐字见那个函数。 */
	FVector4f Semantic = CSWallMesh::Semantic(CSWallMesh::PartWall);

	/**
	 * 当前面板所属墙面的框架 + 洞的裁剪场。AddTri 据此为每个顶点算 UV1 = q —— 洞由材质
	 * 逐像素 discard 切出来，几何上不挖（Tiny Glade 原版做法，见 CSHouseProfile.h）。
	 * 无洞面板保持 Field.bValid = false，写哨兵 (8, 8)，判据下恒保留。
	 */
	FVector ClipOrigin = FVector::ZeroVector;
	FVector ClipAlong = FVector::ForwardVector;
	FCSOpeningClipField ClipField;

	/**
	 * 顶点局部位置 → UV1。S 是沿墙弧长、Z 是**离墙脚**的高度，与剖面/判据同一套坐标。
	 * 墙脚取 `ClipOrigin.Z`：房子的面板原点恒在 Z = 0（`x − 0` 逐位不变），墙脚不在 0 的墙照样对。
	 */
	FVector2f ClipUV(const FVector& LocalP) const
	{
		return ClipField.Eval(float(FVector::DotProduct(LocalP - ClipOrigin, ClipAlong)), float(LocalP.Z - ClipOrigin.Z));
	}

	/** 换一块面板：设墙框架与裁剪场，并把形状 id 写进语义色 B 通道。`Part` 是 `ECSHousePart`（或它的 uint8 值）。 */
	template <typename TPart>
	void SetPanel(const FVector& Origin, const FVector& Along, const FCSOpeningClipField& Field, TPart Part, uint8 Tag)
	{
		ClipOrigin = Origin;
		ClipAlong = Along;
		ClipField = Field;
		Semantic = CSWallMesh::Semantic(uint8(Part), Tag);
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
		const float LU = float(U.Size()) / CSWallMesh::UVScale;
		const float LV = float(V.Size()) / CSWallMesh::UVScale;
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
		const float LU = float((P1 - P0).Size()) / CSWallMesh::UVScale;
		const float LV = float((P3 - P0).Size()) / CSWallMesh::UVScale;
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

	/**
	 * 全显式的三角形（`ECSWallSurface::Continuous` 那种墙用）：逐顶点法线、UV0、UV1 都由调用方给，
	 * 法线与切线**原样**写（只过 `World` 的旋转、不再单位化 —— 调用方给的就是单位向量，再单位化一次会改掉末位）。
	 * 绕序口径与 `AddTri` 相同：A、B、C 按外法线的右手序给，索引交换 1/2 写。
	 */
	void AddTriEx(const FVector& A, const FVector& B, const FVector& C,
		const FVector& NA, const FVector& NB, const FVector& NC, int32 Slot,
		const FVector2f& UVA, const FVector2f& UVB, const FVector2f& UVC,
		const FVector2f& ClipA, const FVector2f& ClipB, const FVector2f& ClipC, const FVector& Tangent)
	{
		const int32 Base = S.Positions.Num();
		const FVector P[3] = { A, B, C };
		const FVector Nrm[3] = { NA, NB, NC };
		const FVector2f UV[3] = { UVA, UVB, UVC };
		const FVector2f Clip[3] = { ClipA, ClipB, ClipC };
		const FVector3f T(World.TransformVectorNoScale(Tangent));
		for (int32 i = 0; i < 3; ++i)
		{
			S.Positions.Add(FVector3f(World.TransformPosition(P[i])));
			S.Normals.Add(FVector3f(World.TransformVectorNoScale(Nrm[i])));
			S.Tangents.Add(T);
			S.TexCoords().Add(UV[i]);
			S.TexCoordChannels[1].Add(Clip[i]);
			S.Colors.Add(Semantic);
		}
		S.Indices.Append({ uint32(Base), uint32(Base + 2), uint32(Base + 1) });
		S.TriangleMaterialSlots.Add(Slot);
	}
};
