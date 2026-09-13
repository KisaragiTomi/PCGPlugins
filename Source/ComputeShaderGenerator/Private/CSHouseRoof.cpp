#include "CSHouseRoof.h"

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSHouseRoof_ 前缀。

/** 波前上的一个活动顶点：它夹在两条活动边之间，随内距 d 沿角平分线匀速走。 */
struct FCSHouseRoof_Vertex
{
	/** 当前内距处的位置。 */
	FVector2D P = FVector2D::ZeroVector;
	/** 内距每增加 1，位置走多少（= 两条边内法线之和 / (1 + 点积)，长度 1/cos(转角/2)）。 */
	FVector2D Velocity = FVector2D::ZeroVector;
	/** 这个顶点划出的骨架弧从哪儿起算。 */
	FVector2D OriginP = FVector2D::ZeroVector;
	double OriginInset = 0.0;
	/** 原始角号（footprint 的角）；骨架节点为 INDEX_NONE。 */
	int32 Corner = INDEX_NONE;
	int32 EdgeIn = INDEX_NONE;
	int32 EdgeOut = INDEX_NONE;
};

FVector2D CSHouseRoof_Velocity(const FVector2D& InA, const FVector2D& InB)
{
	const double Denom = 1.0 + FVector2D::DotProduct(InA, InB);
	// 两条边平行反向：它们之间的波前宽度恒定，顶点不再移动（矩形脊两端就是这种顶点）。
	if (Denom <= 1.0e-9) return FVector2D::ZeroVector;
	return (InA + InB) / Denom;
}
}

void CSHouseRoof_BuildSkeleton(const FCSHouseFootprint& Footprint, double Overhang, FCSRoofSkeleton& Out)
{
	Out = FCSRoofSkeleton();
	const int32 N = Footprint.NumEdges();
	if (N < 3) return;

	TArray<FVector2D, TInlineAllocator<16>> In;
	In.SetNum(N);
	TArray<FVector2D, TInlineAllocator<16>> U;
	U.SetNum(N);
	for (int32 Edge = 0; Edge < N; ++Edge)
	{
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Footprint, 0.0f);
		if (F.Len <= 0.0f) return;   // 零长边：不是合法 footprint，输出空骨架
		In[Edge] = F.In;
		U[Edge] = F.U;
	}

	// 顶点 i 在 Verts[i]，夹在 i−1 号边与 i 号边之间 ⇒ 它是角 i−1（角 k 在 Verts[k+1]）。
	TArray<FCSHouseRoof_Vertex, TInlineAllocator<16>> Ring;
	for (int32 Index = 0; Index < N; ++Index)
	{
		FCSHouseRoof_Vertex V;
		V.EdgeIn = (Index + N - 1) % N;
		V.EdgeOut = Index;
		V.Corner = V.EdgeIn;
		V.Velocity = CSHouseRoof_Velocity(In[V.EdgeIn], In[V.EdgeOut]);
		V.P = Footprint.Verts[Index];
		// 角斜脊从檐口外角起算：内距 −Overhang 处的波前顶点。
		V.OriginP = V.P - V.Velocity * Overhang;
		V.OriginInset = -Overhang;
		Ring.Add(V);
	}

	TArray<FCSRoofSkeletonArc> Hips;
	Hips.SetNum(N);
	TArray<FCSRoofSkeletonArc> Ridges;

	auto Emit = [&Hips, &Ridges](const FCSHouseRoof_Vertex& V, const FVector2D& EndP, double EndInset)
	{
		FCSRoofSkeletonArc Arc;
		Arc.A = V.OriginP;
		Arc.InsetA = V.OriginInset;
		Arc.B = EndP;
		Arc.InsetB = EndInset;
		Arc.FaceA = V.EdgeIn;
		Arc.FaceB = V.EdgeOut;
		if (V.Corner != INDEX_NONE)
		{
			Hips[V.Corner] = Arc;               // 角斜脊一条不落，零长也记（按角号取）
		}
		else if (FVector2D::DistSquared(Arc.A, Arc.B) > 1.0e-12)
		{
			Ridges.Add(Arc);                    // 节点刚生成就被吃掉的零长弧不记
		}
	};

	double Now = 0.0;
	while (Ring.Num() >= 3)
	{
		// 最早塌掉的活动边：边 i 从 Ring[i] 走到 Ring[i+1]，长度按 −(两端 tan(转角/2) 之和) 的速率收短。
		int32 Best = INDEX_NONE;
		double BestT = TNumericLimits<double>::Max();
		for (int32 Index = 0; Index < Ring.Num(); ++Index)
		{
			const FCSHouseRoof_Vertex& V0 = Ring[Index];
			const FCSHouseRoof_Vertex& V1 = Ring[(Index + 1) % Ring.Num()];
			const FVector2D& Dir = U[V0.EdgeOut];
			const double Length = FVector2D::DotProduct(V1.P - V0.P, Dir);
			const double Rate = FVector2D::DotProduct(V1.Velocity - V0.Velocity, Dir);
			if (Rate >= -1.0e-12) continue;     // 不收短（两端都共线）的边永远不塌
			const double T = FMath::Max(Length, 0.0) / -Rate;
			// 严格小于：同时塌的边取环上靠前的那条，结果与输入顺序一一对应、可复现。
			if (T < BestT)
			{
				BestT = T;
				Best = Index;
			}
		}
		if (Best == INDEX_NONE) break;          // 非凸输入会走到这里：到此为止，不猜

		Now += BestT;
		for (FCSHouseRoof_Vertex& V : Ring) V.P += V.Velocity * BestT;

		const int32 NextIndex = (Best + 1) % Ring.Num();
		const FCSHouseRoof_Vertex V0 = Ring[Best];
		const FCSHouseRoof_Vertex V1 = Ring[NextIndex];
		const FVector2D Node = (V0.P + V1.P) * 0.5;
		Emit(V0, Node, Now);
		Emit(V1, Node, Now);

		FCSHouseRoof_Vertex Merged;
		Merged.EdgeIn = V0.EdgeIn;
		Merged.EdgeOut = V1.EdgeOut;
		Merged.P = Node;
		Merged.OriginP = Node;
		Merged.OriginInset = Now;
		Merged.Velocity = CSHouseRoof_Velocity(In[Merged.EdgeIn], In[Merged.EdgeOut]);

		// 先覆盖再删：NextIndex 为 0 时删掉的是环首，覆盖过的那一格顺移一位，环序不变。
		Ring[Best] = Merged;
		Ring.RemoveAt(NextIndex);
	}

	Out.MaxInset = Now;
	for (const FCSHouseRoof_Vertex& V : Ring) Emit(V, V.P, Now);

	// 剩下两个顶点（两条平行反向的边之间）就是最高那段脊的两端；三角形收成一点时两端重合。
	if (Ring.Num() == 2)
	{
		FCSRoofSkeletonArc Top;
		Top.A = Ring[0].P;
		Top.B = Ring[1].P;
		Top.InsetA = Top.InsetB = Now;
		Top.FaceA = Ring[0].EdgeOut;
		Top.FaceB = Ring[1].EdgeOut;
		if (Top.B.X < Top.A.X || (Top.B.X == Top.A.X && Top.B.Y < Top.A.Y))
		{
			Swap(Top.A, Top.B);
		}
		Out.TopA = Top.A;
		Out.TopB = Top.B;
		Ridges.Add(Top);
	}
	else if (Ring.Num() == 1)
	{
		Out.TopA = Out.TopB = Ring[0].P;
	}

	Out.NumHips = N;
	Out.Arcs = MoveTemp(Hips);
	Out.Arcs.Append(Ridges);
}
