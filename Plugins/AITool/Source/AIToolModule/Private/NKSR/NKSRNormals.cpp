// NKSR normal estimation: kd-tree kNN + PCA (smallest eigenvector) + consistent
// orientation via minimum spanning tree propagation.
// Mirrors open3d estimate_normals(KDTreeSearchParamKNN(k)) followed by
// orient_normals_consistent_tangent_plane(k). See G_cpp_design.md §8.
// Deterministic: kNN sets are the K smallest under the (dist², index) total order,
// covariance accumulation follows that fixed order, adjacency rows are sorted,
// and the Prim heap uses the (weight, idxFrom, idxTo) total order.

#include "NKSRNormals.h"

#include "Async/ParallelFor.h"

#include <algorithm>
#include <atomic>

namespace NKSRNormalsPrivate
{

// ---------------------------------------------------------------------------
// kd-tree (median split, iterative queries)
// ---------------------------------------------------------------------------

struct FKdNode
{
	float Split = 0.f;
	int32 PointIndex = -1;
	int32 Left = -1;
	int32 Right = -1;
	uint8 Axis = 0;
};

struct FKnnCandidate
{
	float Dist2;
	int32 Index;
};

using FKnnResult = TArray<FKnnCandidate, TInlineAllocator<64>>;

class FKdTree
{
public:
	void Build(TConstArrayView<FVector3f> InPoints)
	{
		Points = InPoints;
		const int32 N = Points.Num();
		Nodes.Reset();
		Nodes.Reserve(N); // exact node count; no reallocation during build

		TArray<int32> Order;
		Order.SetNumUninitialized(N);
		for (int32 I = 0; I < N; ++I) Order[I] = I;

		struct FBuildRange { int32 Lo; int32 Hi; int32 Parent; bool bLeft; };
		TArray<FBuildRange> Stack; // explicit stack: depth is O(log N) but avoid recursion anyway
		Stack.Push({0, N, -1, false});
		while (Stack.Num() > 0)
		{
			const FBuildRange R = Stack.Pop();
			if (R.Lo >= R.Hi) continue;

			// Split along the axis of largest extent (ties -> smaller axis index).
			FVector3f Mn(MAX_flt, MAX_flt, MAX_flt);
			FVector3f Mx(-MAX_flt, -MAX_flt, -MAX_flt);
			for (int32 I = R.Lo; I < R.Hi; ++I)
			{
				const FVector3f& P = Points[Order[I]];
				Mn = Mn.ComponentMin(P);
				Mx = Mx.ComponentMax(P);
			}
			const FVector3f Ext = Mx - Mn;
			uint8 Axis = 0;
			if (Ext.Y > Ext[Axis]) Axis = 1;
			if (Ext.Z > Ext[Axis]) Axis = 2;

			const int32 Mid = (R.Lo + R.Hi) / 2;
			TConstArrayView<FVector3f> Pts = Points;
			// Tie-break by original index: strict total order => deterministic partition
			// regardless of the std::nth_element implementation.
			std::nth_element(Order.GetData() + R.Lo, Order.GetData() + Mid, Order.GetData() + R.Hi,
				[Pts, Axis](int32 A, int32 B)
				{
					const float Ca = Pts[A][Axis];
					const float Cb = Pts[B][Axis];
					if (Ca != Cb) return Ca < Cb;
					return A < B;
				});

			const int32 NodeIdx = Nodes.AddDefaulted();
			FKdNode& Node = Nodes[NodeIdx];
			Node.PointIndex = Order[Mid];
			Node.Axis = Axis;
			Node.Split = Points[Order[Mid]][Axis];
			if (R.Parent >= 0)
			{
				if (R.bLeft) Nodes[R.Parent].Left = NodeIdx;
				else Nodes[R.Parent].Right = NodeIdx;
			}
			Stack.Push({R.Lo, Mid, NodeIdx, true});
			Stack.Push({Mid + 1, R.Hi, NodeIdx, false});
		}
	}

	/**
	 * K nearest neighbors of Q (the query point itself included when it is in the tree),
	 * returned ascending by (dist², index). The result is the unique K-smallest set under
	 * that total order, independent of traversal order.
	 */
	void QueryKnn(const FVector3f& Q, int32 K, FKnnResult& OutBest) const
	{
		OutBest.Reset();
		if (Nodes.Num() == 0 || K <= 0) return;

		TArray<int32, TInlineAllocator<96>> Stack;
		Stack.Push(0);
		while (Stack.Num() > 0)
		{
			const int32 NodeIdx = Stack.Pop();
			const FKdNode& Node = Nodes[NodeIdx];
			const FVector3f D = Points[Node.PointIndex] - Q;
			const float Dist2 = D.X * D.X + D.Y * D.Y + D.Z * D.Z;
			TryInsert(OutBest, K, Dist2, Node.PointIndex);

			const float Diff = Q[Node.Axis] - Node.Split;
			const int32 Near = Diff <= 0.f ? Node.Left : Node.Right;
			const int32 Far = Diff <= 0.f ? Node.Right : Node.Left;
			// '<=' keeps equidistant candidates reachable so index tie-breaks stay exact.
			if (Far >= 0 && (OutBest.Num() < K || Diff * Diff <= OutBest.Last().Dist2)) Stack.Push(Far);
			if (Near >= 0) Stack.Push(Near); // pushed last => visited first (better pruning)
		}
	}

private:
	static void TryInsert(FKnnResult& Best, int32 K, float Dist2, int32 Index)
	{
		if (Best.Num() == K)
		{
			const FKnnCandidate& Worst = Best.Last();
			if (Dist2 > Worst.Dist2 || (Dist2 == Worst.Dist2 && Index > Worst.Index)) return;
			Best.Pop(EAllowShrinking::No);
		}
		int32 Pos = Best.Num();
		while (Pos > 0 && (Dist2 < Best[Pos - 1].Dist2 || (Dist2 == Best[Pos - 1].Dist2 && Index < Best[Pos - 1].Index))) --Pos;
		Best.Insert(FKnnCandidate{Dist2, Index}, Pos);
	}

	TConstArrayView<FVector3f> Points;
	TArray<FKdNode> Nodes;
};

// ---------------------------------------------------------------------------
// Symmetric 3x3 eigen decomposition (cyclic Jacobi, fixed sweep budget)
// ---------------------------------------------------------------------------

/** In-place cyclic Jacobi. On exit diag(A) holds eigenvalues, columns of V the eigenvectors. */
static void JacobiEigenSymmetric3(float A[3][3], float V[3][3])
{
	for (int32 R = 0; R < 3; ++R) for (int32 C = 0; C < 3; ++C) V[R][C] = (R == C) ? 1.f : 0.f;

	constexpr int32 MaxSweeps = 24; // 3x3 converges in a handful of sweeps; fixed budget for determinism
	static const int32 Pairs[3][2] = {{0, 1}, {0, 2}, {1, 2}};
	for (int32 Sweep = 0; Sweep < MaxSweeps; ++Sweep)
	{
		const float Off = FMath::Abs(A[0][1]) + FMath::Abs(A[0][2]) + FMath::Abs(A[1][2]);
		if (Off == 0.f) break;
		for (int32 PairIdx = 0; PairIdx < 3; ++PairIdx)
		{
			const int32 P = Pairs[PairIdx][0];
			const int32 Q = Pairs[PairIdx][1];
			const float Apq = A[P][Q];
			if (Apq == 0.f) continue;
			const float Theta = (A[Q][Q] - A[P][P]) / (2.f * Apq);
			// If Theta² overflows, T collapses to 0 and the (negligible) off-diagonal is dropped.
			const float T = (Theta >= 0.f ? 1.f : -1.f) / (FMath::Abs(Theta) + FMath::Sqrt(Theta * Theta + 1.f));
			const float C = 1.f / FMath::Sqrt(T * T + 1.f);
			const float S = T * C;

			const int32 R3 = 3 - P - Q; // the remaining row/column index
			A[P][P] -= T * Apq;
			A[Q][Q] += T * Apq;
			A[P][Q] = 0.f;
			A[Q][P] = 0.f;
			const float Arp = A[R3][P];
			const float Arq = A[R3][Q];
			A[R3][P] = C * Arp - S * Arq;
			A[P][R3] = A[R3][P];
			A[R3][Q] = S * Arp + C * Arq;
			A[Q][R3] = A[R3][Q];
			for (int32 R = 0; R < 3; ++R)
			{
				const float Vrp = V[R][P];
				const float Vrq = V[R][Q];
				V[R][P] = C * Vrp - S * Vrq;
				V[R][Q] = S * Vrp + C * Vrq;
			}
		}
	}
}

} // namespace NKSRNormalsPrivate

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

bool NKSREstimateNormals(TConstArrayView<FVector3f> Points, int32 K, TArray<FVector3f>& OutNormals, FString& OutError)
{
	using namespace NKSRNormalsPrivate;

	OutNormals.Reset();
	const int32 N = Points.Num();
	if (N < 3)
	{
		OutError = FString::Printf(TEXT("NKSREstimateNormals: need at least 3 points, got %d"), N);
		return false;
	}
	if (K < 3)
	{
		OutError = FString::Printf(TEXT("NKSREstimateNormals: K must be >= 3, got %d"), K);
		return false;
	}

	// NaN/Inf coordinates would break the strict-weak-order comparators (kd-tree build is UB then).
	for (int32 I = 0; I < N; ++I)
	{
		if (Points[I].ContainsNaN())
		{
			OutError = FString::Printf(TEXT("NKSREstimateNormals: point %d has non-finite coordinates"), I);
			return false;
		}
	}

	const int32 KEff = FMath::Min(K, N); // kNN includes the query point itself
	if ((int64)N * (int64)KEff * 2 > (int64)MAX_int32)
	{
		OutError = FString::Printf(TEXT("NKSREstimateNormals: N*K too large (N=%d, K=%d)"), N, KEff);
		return false;
	}

	FKdTree Tree;
	Tree.Build(Points);

	// ---- Pass 1: kNN + PCA normal per point (independent writes -> ParallelFor) ----
	OutNormals.SetNumUninitialized(N);
	TArray<int32> NeighborIdx; // [N][KEff], ascending (dist², index) per row
	NeighborIdx.SetNumUninitialized(N * KEff);
	std::atomic<int32> DegenerateCount{0};

	ParallelFor(N, [&Tree, &Points, &OutNormals, &NeighborIdx, &DegenerateCount, KEff](int32 I)
	{
		FKnnResult Best;
		Tree.QueryKnn(Points[I], KEff, Best);
		const int32 Count = Best.Num(); // == KEff (KEff <= N and every point is in the tree)
		for (int32 S = 0; S < Count; ++S) NeighborIdx[I * KEff + S] = Best[S].Index;

		// Neighborhood covariance (accumulated in the fixed (dist², index) order).
		FVector3f Mean(0.f, 0.f, 0.f);
		for (int32 S = 0; S < Count; ++S) Mean += Points[Best[S].Index];
		Mean /= (float)Count;
		float Cov[3][3] = {{0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}};
		for (int32 S = 0; S < Count; ++S)
		{
			const FVector3f D = Points[Best[S].Index] - Mean;
			Cov[0][0] += D.X * D.X; Cov[0][1] += D.X * D.Y; Cov[0][2] += D.X * D.Z;
			Cov[1][1] += D.Y * D.Y; Cov[1][2] += D.Y * D.Z; Cov[2][2] += D.Z * D.Z;
		}
		Cov[1][0] = Cov[0][1]; Cov[2][0] = Cov[0][2]; Cov[2][1] = Cov[1][2];

		const float CovAbsSum = FMath::Abs(Cov[0][0]) + FMath::Abs(Cov[1][1]) + FMath::Abs(Cov[2][2])
			+ FMath::Abs(Cov[0][1]) + FMath::Abs(Cov[0][2]) + FMath::Abs(Cov[1][2]);
		if (Count < 3 || CovAbsSum == 0.f || !FMath::IsFinite(CovAbsSum))
		{
			OutNormals[I] = FVector3f(0.f, 0.f, 1.f); // stable fallback for degenerate/duplicate neighborhoods
			DegenerateCount.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		float V[3][3];
		JacobiEigenSymmetric3(Cov, V);
		int32 MinIdx = 0; // smallest eigenvalue; ties -> smallest index
		if (Cov[1][1] < Cov[MinIdx][MinIdx]) MinIdx = 1;
		if (Cov[2][2] < Cov[MinIdx][MinIdx]) MinIdx = 2;
		FVector3f Normal(V[0][MinIdx], V[1][MinIdx], V[2][MinIdx]);
		const float Len = FMath::Sqrt(Normal.X * Normal.X + Normal.Y * Normal.Y + Normal.Z * Normal.Z);
		if (Len < 1e-12f || !FMath::IsFinite(Len))
		{
			OutNormals[I] = FVector3f(0.f, 0.f, 1.f);
			DegenerateCount.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		OutNormals[I] = Normal / Len;
	});

	const int32 NumDegenerate = DegenerateCount.load(std::memory_order_relaxed);
	if (NumDegenerate > 0) UE_LOG(LogNKSR, Warning, TEXT("NKSREstimateNormals: %d/%d degenerate neighborhoods fell back to +Z"), NumDegenerate, N);

	// ---- Symmetrized kNN graph as CSR (rows sorted + deduplicated) ----
	TArray<int32> Deg;
	Deg.SetNumZeroed(N);
	for (int32 I = 0; I < N; ++I)
	{
		for (int32 S = 0; S < KEff; ++S)
		{
			const int32 J = NeighborIdx[I * KEff + S];
			if (J == I) continue;
			Deg[I]++;
			Deg[J]++;
		}
	}
	TArray<int32> RawOffset;
	RawOffset.SetNumUninitialized(N + 1);
	RawOffset[0] = 0;
	for (int32 I = 0; I < N; ++I) RawOffset[I + 1] = RawOffset[I] + Deg[I];

	TArray<int32> AdjRaw;
	AdjRaw.SetNumUninitialized(RawOffset[N]);
	{
		TArray<int32> Cursor(RawOffset.GetData(), N); // copy of row starts
		for (int32 I = 0; I < N; ++I)
		{
			for (int32 S = 0; S < KEff; ++S)
			{
				const int32 J = NeighborIdx[I * KEff + S];
				if (J == I) continue;
				AdjRaw[Cursor[I]++] = J;
				AdjRaw[Cursor[J]++] = I;
			}
		}
	}
	NeighborIdx.Empty();

	// Sort + dedup each row (disjoint ranges -> ParallelFor is safe).
	TArray<int32> RowNum;
	RowNum.SetNumUninitialized(N);
	ParallelFor(N, [&AdjRaw, &RawOffset, &RowNum](int32 I)
	{
		int32* Lo = AdjRaw.GetData() + RawOffset[I];
		int32* Hi = AdjRaw.GetData() + RawOffset[I + 1];
		std::sort(Lo, Hi);
		RowNum[I] = (int32)(std::unique(Lo, Hi) - Lo);
	});
	TArray<int32> AdjOffset;
	AdjOffset.SetNumUninitialized(N + 1);
	AdjOffset[0] = 0;
	for (int32 I = 0; I < N; ++I) AdjOffset[I + 1] = AdjOffset[I] + RowNum[I];
	TArray<int32> Adj;
	Adj.SetNumUninitialized(AdjOffset[N]);
	ParallelFor(N, [&Adj, &AdjRaw, &AdjOffset, &RawOffset, &RowNum](int32 I)
	{
		FMemory::Memcpy(Adj.GetData() + AdjOffset[I], AdjRaw.GetData() + RawOffset[I], RowNum[I] * sizeof(int32));
	});
	AdjRaw.Empty();
	RawOffset.Empty();
	RowNum.Empty();
	Deg.Empty();

	// ---- Prim MST over edge weight 1 - |n_i . n_j| ----
	struct FPrimEdge { float W; int32 From; int32 To; };
	struct FPrimEdgeLess
	{
		FORCEINLINE bool operator()(const FPrimEdge& A, const FPrimEdge& B) const
		{
			if (A.W != B.W) return A.W < B.W;
			if (A.From != B.From) return A.From < B.From;
			return A.To < B.To; // (weight, idxFrom, idxTo) strict total order -> deterministic pops
		}
	};

	TArray<int32> Parent;
	Parent.Init(-1, N);
	TArray<bool> Visited;
	Visited.Init(false, N);
	TArray<int32> VisitOrder;
	VisitOrder.Reserve(N);
	TArray<int32> CompOffsets;
	CompOffsets.Add(0);
	TArray<FPrimEdge> Heap;

	auto EdgeWeight = [&OutNormals](int32 A, int32 B)
	{
		return FMath::Max(0.f, 1.f - FMath::Abs(FVector3f::DotProduct(OutNormals[A], OutNormals[B])));
	};
	auto PushEdges = [&Heap, &Adj, &AdjOffset, &Visited, &EdgeWeight](int32 U)
	{
		for (int32 E = AdjOffset[U]; E < AdjOffset[U + 1]; ++E)
		{
			const int32 V = Adj[E];
			if (!Visited[V]) Heap.HeapPush(FPrimEdge{EdgeWeight(U, V), U, V}, FPrimEdgeLess());
		}
	};
	auto RunPrim = [&](int32 Start)
	{
		Visited[Start] = true;
		VisitOrder.Add(Start);
		PushEdges(Start);
		FPrimEdge E;
		while (Heap.Num() > 0)
		{
			Heap.HeapPop(E, FPrimEdgeLess(), EAllowShrinking::No);
			if (Visited[E.To]) continue;
			Visited[E.To] = true;
			Parent[E.To] = E.From;
			VisitOrder.Add(E.To);
			PushEdges(E.To);
		}
		CompOffsets.Add(VisitOrder.Num());
	};

	// First component grows from the global max-Z point; leftovers (disconnected kNN
	// graph) start new components in ascending index order.
	int32 GlobalSeed = 0;
	for (int32 I = 1; I < N; ++I) if (Points[I].Z > Points[GlobalSeed].Z) GlobalSeed = I; // strict '>' keeps the smallest index on ties
	RunPrim(GlobalSeed);
	for (int32 I = 0; I < N; ++I) if (!Visited[I]) RunPrim(I);
	Heap.Empty();

	// ---- Undirected MST adjacency (CSR, rows sorted) ----
	TArray<int32> TreeDeg;
	TreeDeg.SetNumZeroed(N);
	for (int32 V = 0; V < N; ++V)
	{
		if (Parent[V] < 0) continue;
		TreeDeg[V]++;
		TreeDeg[Parent[V]]++;
	}
	TArray<int32> TreeOffset;
	TreeOffset.SetNumUninitialized(N + 1);
	TreeOffset[0] = 0;
	for (int32 I = 0; I < N; ++I) TreeOffset[I + 1] = TreeOffset[I] + TreeDeg[I];
	TArray<int32> TreeAdj;
	TreeAdj.SetNumUninitialized(TreeOffset[N]);
	{
		TArray<int32> Cursor(TreeOffset.GetData(), N);
		for (int32 V = 0; V < N; ++V)
		{
			if (Parent[V] < 0) continue;
			TreeAdj[Cursor[V]++] = Parent[V];
			TreeAdj[Cursor[Parent[V]]++] = V;
		}
	}
	ParallelFor(N, [&TreeAdj, &TreeOffset](int32 I)
	{
		std::sort(TreeAdj.GetData() + TreeOffset[I], TreeAdj.GetData() + TreeOffset[I + 1]);
	});

	// ---- BFS flip propagation along the MST, per component ----
	TArray<bool> Oriented;
	Oriented.Init(false, N);
	TArray<int32> Queue;
	Queue.Reserve(N);
	for (int32 Comp = 0; Comp + 1 < CompOffsets.Num(); ++Comp)
	{
		// Component seed = its max-Z point (ties -> smallest index).
		int32 Seed = -1;
		for (int32 S = CompOffsets[Comp]; S < CompOffsets[Comp + 1]; ++S)
		{
			const int32 Idx = VisitOrder[S];
			if (Seed < 0 || Points[Idx].Z > Points[Seed].Z || (Points[Idx].Z == Points[Seed].Z && Idx < Seed)) Seed = Idx;
		}
		if (Seed < 0) continue;
		if (OutNormals[Seed].Z < 0.f) OutNormals[Seed] = -OutNormals[Seed]; // orient seed toward +Z

		Queue.Reset();
		Oriented[Seed] = true;
		Queue.Add(Seed);
		for (int32 Head = 0; Head < Queue.Num(); ++Head)
		{
			const int32 U = Queue[Head];
			for (int32 E = TreeOffset[U]; E < TreeOffset[U + 1]; ++E)
			{
				const int32 V = TreeAdj[E];
				if (Oriented[V]) continue;
				if (FVector3f::DotProduct(OutNormals[U], OutNormals[V]) < 0.f) OutNormals[V] = -OutNormals[V];
				Oriented[V] = true;
				Queue.Add(V);
			}
		}
	}

	return true;
}
