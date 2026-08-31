// 藤蔓输入的现场生成：把一个表面 actor 变成 GrowTarget（吸引子）+ TubeVineSource（起点）两组实例。
//
// 单独成文件而不是塞进 GeometryEditorActor.cpp：那份已经 4500 行且几乎全是 RDG pass 与 shader 声明，
// 这里是纯 game-thread 的采样/筛选逻辑，没有一行共享代码。
//
// 采样源目前是 StaticMesh 的渲染数据（测试拿一个 cube）。TinyGlade 房屋落地后墙面是参数化的，
// 换掉 CollectSurfacePoints 一个函数即可 —— 下面"贴地带筛选 + 最远点抽稀"那套筛法与几何来源无关。

#include "GeometryEditorActor.h"

#include "CSGroundActor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Algo/BinarySearch.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "Engine/World.h"
#include "WorldCollision.h"
#include "EngineUtils.h"

namespace VineScatter
{
	/** 一个世界空间三角形，外加散点要用到的法线与面积。 */
	struct FSurfaceTriangle
	{
		FVector V0, V1, V2;
		FVector Normal = FVector::UpVector;
		double Area = 0.0;
	};

	/**
	 * 从 actor 的静态网格组件抽出世界空间三角形（CPU 侧）。
	 *
	 * 走 LOD0 的渲染数据 + IndexBuffer.GetCopy —— 与 UCSGpuInstancedMeshComponent 读基础网格
	 * 是同一条路。编辑器里恒可用；打包后要求网格勾了 Allow CPU Access，读不到时静默产出空集，
	 * 由调用方报警。
	 */
	static void CollectSurfaceTriangles(const AActor* SurfaceActor, float MaxNormalZ, TArray<FSurfaceTriangle>& OutTriangles, int32& OutRejectedByNormal)
	{
		OutTriangles.Reset();
		OutRejectedByNormal = 0;
		if (!SurfaceActor)
		{
			return;
		}

		TArray<UStaticMeshComponent*> MeshComponents;
		SurfaceActor->GetComponents<UStaticMeshComponent>(MeshComponents);

		for (const UStaticMeshComponent* MeshComponent : MeshComponents)
		{
			UStaticMesh* Mesh = MeshComponent ? MeshComponent->GetStaticMesh() : nullptr;
			const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
			if (!RenderData || RenderData->LODResources.Num() == 0)
			{
				continue;
			}

			const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
			const uint32 NumVerts = LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();
			if (NumVerts == 0)
			{
				continue;
			}

			TArray<uint32> Indices;
			LOD.IndexBuffer.GetCopy(Indices);
			if (Indices.Num() < 3)
			{
				continue;
			}

			const FTransform LocalToWorld = MeshComponent->GetComponentTransform();
			OutTriangles.Reserve(OutTriangles.Num() + Indices.Num() / 3);

			for (int32 i = 0; i + 2 < Indices.Num(); i += 3)
			{
				const uint32 I0 = Indices[i], I1 = Indices[i + 1], I2 = Indices[i + 2];
				if (I0 >= NumVerts || I1 >= NumVerts || I2 >= NumVerts)
				{
					continue;
				}

				FSurfaceTriangle Tri;
				Tri.V0 = LocalToWorld.TransformPosition(FVector(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(I0)));
				Tri.V1 = LocalToWorld.TransformPosition(FVector(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(I1)));
				Tri.V2 = LocalToWorld.TransformPosition(FVector(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(I2)));

				// 几何法线而不是顶点法线：这里只用来判"是不是墙面"，插值法线反而会让
				// 倒角处的三角形被误判。
				const FVector Cross = FVector::CrossProduct(Tri.V1 - Tri.V0, Tri.V2 - Tri.V0);
				const double CrossLen = Cross.Length();
				if (CrossLen <= UE_DOUBLE_SMALL_NUMBER)
				{
					continue; // 退化三角形
				}

				Tri.Normal = Cross / CrossLen;
				Tri.Area = 0.5 * CrossLen;

				if (FMath::Abs(Tri.Normal.Z) > double(MaxNormalZ))
				{
					++OutRejectedByNormal;
					continue;
				}

				OutTriangles.Add(Tri);
			}
		}
	}

	/**
	 * 面积加权表面散点：按面积 CDF 抽三角形，再取重心坐标随机点，最后用体素占位表压最小间距。
	 *
	 * 刻意做成"点数与三角形数无关"：引擎自带的 FStaticMeshRenderDataPointSampler 把采样数夹到
	 * 三角形数、且每个三角形只吐质心，一个 12 面的 cube 封顶就那么多点，而 TinyGlade 的墙面是
	 * 参数化的两三角形四边形 —— 那条路在真正的用例上只会给出两个点。
	 *
	 * @param ZFilterMin/ZFilterMax  世界 Z 预过滤范围，只散落在这一层里的三角形（贴地带用）。
	 *                               传一个无效区间表示不过滤。
	 */
	static void ScatterOnTriangles(const TArray<FSurfaceTriangle>& Triangles, int32 DesiredCount, float MinSpacing,
		int32 Seed, double ZFilterMin, double ZFilterMax, TArray<FVector>& OutPoints)
	{
		OutPoints.Reset();
		if (Triangles.IsEmpty() || DesiredCount <= 0)
		{
			return;
		}

		const bool bFilterZ = ZFilterMin <= ZFilterMax;

		TArray<double> CumulativeArea;
		TArray<int32> TriangleIndices;
		CumulativeArea.Reserve(Triangles.Num());
		TriangleIndices.Reserve(Triangles.Num());

		double TotalArea = 0.0;
		for (int32 i = 0; i < Triangles.Num(); ++i)
		{
			const FSurfaceTriangle& Tri = Triangles[i];
			if (bFilterZ)
			{
				const double TriMinZ = FMath::Min3(Tri.V0.Z, Tri.V1.Z, Tri.V2.Z);
				const double TriMaxZ = FMath::Max3(Tri.V0.Z, Tri.V1.Z, Tri.V2.Z);
				if (TriMaxZ < ZFilterMin || TriMinZ > ZFilterMax)
				{
					continue;
				}
			}
			TotalArea += Tri.Area;
			CumulativeArea.Add(TotalArea);
			TriangleIndices.Add(i);
		}

		if (TriangleIndices.IsEmpty() || TotalArea <= 0.0)
		{
			return;
		}

		const double CellSize = FMath::Max(double(MinSpacing), 0.1);
		TSet<FIntVector> OccupiedCells;
		OccupiedCells.Reserve(DesiredCount * 2);

		FRandomStream Rand(Seed);
		OutPoints.Reserve(DesiredCount);

		// 拒绝采样要有上限：间距给得比表面能容纳的还大时，循环永远填不满目标数。
		const int32 MaxAttempts = FMath::Max(DesiredCount * 32, 4096);
		for (int32 Attempt = 0; Attempt < MaxAttempts && OutPoints.Num() < DesiredCount; ++Attempt)
		{
			const double Pick = Rand.FRand() * TotalArea;
			const int32 Slot = Algo::LowerBound(CumulativeArea, Pick);
			const FSurfaceTriangle& Tri = Triangles[TriangleIndices[FMath::Min(Slot, TriangleIndices.Num() - 1)]];

			// 均匀重心坐标：sqrt 那一下是必须的，直接用两个均匀数会把点堆到一个角上。
			double U = Rand.FRand();
			double V = Rand.FRand();
			const double SqrtU = FMath::Sqrt(U);
			const double A = 1.0 - SqrtU;
			const double B = SqrtU * (1.0 - V);
			const double C = SqrtU * V;
			const FVector P = Tri.V0 * A + Tri.V1 * B + Tri.V2 * C;

			if (bFilterZ && (P.Z < ZFilterMin || P.Z > ZFilterMax))
			{
				continue;
			}

			const FIntVector Cell(
				FMath::FloorToInt32(P.X / CellSize),
				FMath::FloorToInt32(P.Y / CellSize),
				FMath::FloorToInt32(P.Z / CellSize));
			bool bAlreadyThere = false;
			OccupiedCells.Add(Cell, &bAlreadyThere);
			if (bAlreadyThere)
			{
				continue;
			}

			OutPoints.Add(P);
		}
	}

	/**
	 * 地面高度查询。两条路，顺序有讲究：
	 *  1. ACSGroundActor 的 CPU 权威镜像 —— 它的网格是 gpumesh，全线 NoCollision，line trace
	 *     根本打不到，只能问镜像。
	 *  2. 向下 line trace —— Landscape 和普通带碰撞的静态网格走这条。
	 */
	static bool ResolveGroundZ(UWorld* World, const AActor* IgnoredActor, const AActor* AlsoIgnored,
		const FVector& Probe, float ProbeUp, float ProbeDown, double& OutZ)
	{
		if (!World)
		{
			return false;
		}

		const FVector2D ProbeXY(Probe.X, Probe.Y);
		for (TActorIterator<ACSGroundActor> It(World); It; ++It)
		{
			const FBox2D Rect = It->GetWorldRect2D();
			if (Rect.bIsValid && Rect.IsInside(ProbeXY))
			{
				OutZ = It->SampleHeight(ProbeXY);
				return true;
			}
		}

		FCollisionQueryParams Params(SCENE_QUERY_STAT(VineScatterGroundProbe), /*bTraceComplex=*/true);
		// 表面 actor 自己必须排除，否则从它内部往下打第一个命中的就是它自身的底面；
		// 藤蔓容器也排除，它那两组实例组件是带 Overlap 碰撞的。
		Params.AddIgnoredActor(IgnoredActor);
		Params.AddIgnoredActor(AlsoIgnored);

		FHitResult Hit;
		const FVector Start = Probe + FVector(0.0, 0.0, ProbeUp);
		const FVector End = Probe - FVector(0.0, 0.0, ProbeDown);
		if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_WorldStatic, Params))
		{
			OutZ = Hit.ImpactPoint.Z;
			return true;
		}

		return false;
	}

	/** 贪心最远点抽稀：起点数很少（个位数），要的是"铺开"而不是"随机"，随机抽很容易挤在一面墙上。 */
	static void PickSpreadSubset(const TArray<FVector>& Candidates, int32 DesiredCount, TArray<FVector>& OutPicked)
	{
		OutPicked.Reset();
		if (Candidates.IsEmpty() || DesiredCount <= 0)
		{
			return;
		}
		if (Candidates.Num() <= DesiredCount)
		{
			OutPicked = Candidates;
			return;
		}

		// 种子取离质心最远的那个：从质心开始能让第一批点落在外缘，比固定取 [0] 稳定
		// （采样器的输出顺序跟着三角形索引走，[0] 基本总在同一个角上）。
		FVector Centroid = FVector::ZeroVector;
		for (const FVector& P : Candidates) Centroid += P;
		Centroid /= double(Candidates.Num());

		int32 SeedIndex = 0;
		double SeedDistSq = -1.0;
		for (int32 i = 0; i < Candidates.Num(); ++i)
		{
			const double DistSq = FVector::DistSquared(Candidates[i], Centroid);
			if (DistSq > SeedDistSq) { SeedDistSq = DistSq; SeedIndex = i; }
		}

		TArray<double> NearestPickedDistSq;
		NearestPickedDistSq.Init(TNumericLimits<double>::Max(), Candidates.Num());

		OutPicked.Reserve(DesiredCount);
		int32 CurrentIndex = SeedIndex;
		for (int32 Picked = 0; Picked < DesiredCount; ++Picked)
		{
			OutPicked.Add(Candidates[CurrentIndex]);
			NearestPickedDistSq[CurrentIndex] = -1.0; // 标记已取

			int32 NextIndex = INDEX_NONE;
			double BestDistSq = -1.0;
			for (int32 i = 0; i < Candidates.Num(); ++i)
			{
				if (NearestPickedDistSq[i] < 0.0) continue;
				NearestPickedDistSq[i] = FMath::Min(NearestPickedDistSq[i],
					FVector::DistSquared(Candidates[i], Candidates[CurrentIndex]));
				if (NearestPickedDistSq[i] > BestDistSq) { BestDistSq = NearestPickedDistSq[i]; NextIndex = i; }
			}

			if (NextIndex == INDEX_NONE) break;
			CurrentIndex = NextIndex;
		}
	}

	/** 点列 → 单位缩放的 transform 列，写进 ISM 组件。实例缩放会被读作藤蔓粗细系数，故固定 1。 */
	static int32 FillInstances(UInstancedStaticMeshComponent* Component, const TArray<FVector>& Points)
	{
		if (!Component)
		{
			return 0;
		}

		Component->ClearInstances();
		if (Points.IsEmpty())
		{
			return 0;
		}

		TArray<FTransform> Transforms;
		Transforms.Reserve(Points.Num());
		for (const FVector& P : Points)
		{
			Transforms.Emplace(FQuat::Identity, P, FVector::OneVector);
		}

		// bWorldSpace = true：采样点是世界坐标，组件挂在 actor 底下会带上 actor 变换。
		Component->AddInstances(Transforms, /*bReturnIndices=*/false, /*bWorldSpace=*/true, /*bUpdateNavigation=*/false);
		return Transforms.Num();
	}
}

int32 AVineContainer::ScatterTargetsFromSurfaceActor()
{
	AActor* SurfaceActor = ScatterSurfaceActor ? ScatterSurfaceActor.Get() : this;
	if (!GrowTarget)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineScatter] %s has no GrowTarget component."), *GetActorNameOrLabel());
		return 0;
	}

	TArray<VineScatter::FSurfaceTriangle> Triangles;
	int32 RejectedByNormal = 0;
	VineScatter::CollectSurfaceTriangles(SurfaceActor, ScatterMaxSurfaceNormalZ, Triangles, RejectedByNormal);
	if (Triangles.IsEmpty())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[VineScatter] No wall triangles on %s (%d rejected by ScatterMaxSurfaceNormalZ=%.2f). ")
			TEXT("没有静态网格，或整个物体都是水平面 —— 把该阈值调到 1.0 可关掉墙面过滤。"),
			*GetNameSafe(SurfaceActor), RejectedByNormal, ScatterMaxSurfaceNormalZ);
		return 0;
	}

	TArray<FVector> Points;
	VineScatter::ScatterOnTriangles(Triangles, ScatterTargetCount, ScatterTargetSpacing, ScatterSeed,
		/*ZFilterMin=*/1.0, /*ZFilterMax=*/-1.0, Points);
	if (Points.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineScatter] Scatter produced no points on %s."), *GetNameSafe(SurfaceActor));
		return 0;
	}

	Modify();
	GrowTarget->Modify();
	const int32 Count = VineScatter::FillInstances(GrowTarget, Points);
	MarkPackageDirty();

	// 实际点数低于请求数不是错误，是最小间距把表面填满了 —— 但它会让"调大 target 数"看起来
	// 没反应，所以照直说出来。
	UE_LOG(LogTemp, Display,
		TEXT("[VineScatter] GrowTarget <- %d/%d points on %s (%d wall tris, %d rejected, spacing %.1f cm)%s"),
		Count, ScatterTargetCount, *GetNameSafe(SurfaceActor), Triangles.Num(), RejectedByNormal, ScatterTargetSpacing,
		Count < ScatterTargetCount ? TEXT("  <-- spacing-limited, 想更密就调小 ScatterTargetSpacing") : TEXT(""));
	return Count;
}

int32 AVineContainer::ScatterSourcesAtGroundContact()
{
	AActor* SurfaceActor = ScatterSurfaceActor ? ScatterSurfaceActor.Get() : this;
	if (!TubeVineSource)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineScatter] %s has no TubeVineSource component."), *GetActorNameOrLabel());
		return 0;
	}

	UWorld* World = GetWorld();
	if (!World || !SurfaceActor)
	{
		return 0;
	}

	TArray<VineScatter::FSurfaceTriangle> Triangles;
	int32 RejectedByNormal = 0;
	VineScatter::CollectSurfaceTriangles(SurfaceActor, ScatterMaxSurfaceNormalZ, Triangles, RejectedByNormal);
	if (Triangles.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineScatter] No wall triangles on %s for ground contact."), *GetNameSafe(SurfaceActor));
		return 0;
	}

	const FBox SurfaceBox = SurfaceActor->GetComponentsBoundingBox(true);
	if (!SurfaceBox.IsValid)
	{
		return 0;
	}

	// 三角形预过滤：只散在物体底部那一层。带高的 4 倍是给地形起伏留的余量 —— 真正的判定
	// 是下面逐点查地面高度，这一步只是不要为了几个起点把整面墙都散一遍。
	const double PrefilterMinZ = SurfaceBox.Min.Z - double(GroundContactBandHeight) * 4.0;
	const double PrefilterMaxZ = SurfaceBox.Min.Z + double(GroundContactBandHeight) * 4.0;

	// 候选要密：贴地带很薄，逐点查地面之后大部分会被淘汰。
	const float CandidateSpacing = FMath::Max(GroundContactBandHeight * 0.25f, 2.0f);
	TArray<FVector> Candidates;
	VineScatter::ScatterOnTriangles(Triangles, 4096, CandidateSpacing, ScatterSeed + 1,
		PrefilterMinZ, PrefilterMaxZ, Candidates);
	if (Candidates.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineScatter] No candidate points near the base of %s."), *GetNameSafe(SurfaceActor));
		return 0;
	}

	const double SurfaceHeight = SurfaceBox.Max.Z - SurfaceBox.Min.Z;
	const float ProbeUp = float(FMath::Max(SurfaceHeight, 100.0));
	const float ProbeDown = float(FMath::Max(SurfaceHeight * 4.0, 10000.0));

	TArray<FVector> ContactPoints;
	ContactPoints.Reserve(Candidates.Num());
	int32 GroundProbeMisses = 0;
	for (const FVector& P : Candidates)
	{
		double GroundZ = 0.0;
		if (!VineScatter::ResolveGroundZ(World, SurfaceActor, this, P, ProbeUp, ProbeDown, GroundZ))
		{
			++GroundProbeMisses;
			continue;
		}

		const double Above = P.Z - GroundZ;
		// 下界放宽半个带厚：网格插进地里一点点仍然算交界。
		if (Above >= -double(GroundContactBandHeight) * 0.5 && Above <= double(GroundContactBandHeight))
		{
			ContactPoints.Add(P);
		}
	}

	if (ContactPoints.IsEmpty())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[VineScatter] %s: no point lands in the ground contact band (band %.1f cm, %d/%d probes found no ground). ")
			TEXT("检查表面 actor 是否真的贴着地面，或把 GroundContactBandHeight 调大。"),
			*GetNameSafe(SurfaceActor), GroundContactBandHeight, GroundProbeMisses, Candidates.Num());
		return 0;
	}

	TArray<FVector> Picked;
	VineScatter::PickSpreadSubset(ContactPoints, ScatterSourceCount, Picked);

	Modify();
	TubeVineSource->Modify();
	const int32 Count = VineScatter::FillInstances(TubeVineSource, Picked);
	MarkPackageDirty();

	UE_LOG(LogTemp, Display,
		TEXT("[VineScatter] TubeVineSource <- %d/%d sources from %d contact candidates (band %.1f cm, %d probe misses)."),
		Count, ScatterSourceCount, ContactPoints.Num(), GroundContactBandHeight, GroundProbeMisses);
	return Count;
}

void AVineContainer::ScatterVineInputs()
{
	// 起点在前：它的地面探测是 line trace，而 GrowTarget 的实例带 Overlap 碰撞，先散 target
	// 会让每条探测都穿过几千个实例体（实测 4000 实例时起点散布 11ms -> 89ms）。两步互不依赖。
	const int32 NumSources = ScatterSourcesAtGroundContact();
	const int32 NumTargets = ScatterTargetsFromSurfaceActor();
	UE_LOG(LogTemp, Display, TEXT("[VineScatter] %s: targets=%d sources=%d."),
		*GetActorNameOrLabel(), NumTargets, NumSources);
}

void AVineContainer::ScatterAndGenerateTimed()
{
	const double T0 = FPlatformTime::Seconds();
	// 顺序同 ScatterVineInputs：起点在前，避免 target 实例污染地面探测的物理场景。
	const int32 NumSources = ScatterSourcesAtGroundContact();

	const double T1 = FPlatformTime::Seconds();
	const int32 NumTargets = ScatterTargetsFromSurfaceActor();

	const double T2 = FPlatformTime::Seconds();
	if (NumTargets == 0 || NumSources == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VinePerf] Aborted: targets=%d sources=%d."), NumTargets, NumSources);
		return;
	}

	const bool bGenerated = GenerateVineGPU();
	const double T3 = FPlatformTime::Seconds();

	// 容量诊断，公式与 PrepareVineFusedSCInputs 里那份保持一致。放在这里是因为那条 Warning 只说
	// "被截断了"，没说该把哪个旋钮调到多少；性能评估时最想先看到的就是这组数。
	constexpr int64 SpaceColonizationMaxBacktrack = 100;
	const int64 TheoreticalBound = FMath::Min<int64>(int64(NumTargets) * (SpaceColonizationMaxBacktrack + 1), 4000000);
	const int64 PerSourceShare = FMath::Max<int64>(1, int64(VV.MaxVinePointCount) / FMath::Max(1, NumSources));
	const int64 PerSourceCapacity = FMath::Max<int64>(1, FMath::Min(TheoreticalBound, PerSourceShare));

	UE_LOG(LogTemp, Display, TEXT("[VinePerf] ---- %s ----"), *GetActorNameOrLabel());
	UE_LOG(LogTemp, Display, TEXT("[VinePerf] scatter sources : %8.2f ms  -> %d points (band %.1f cm)"),
		(T1 - T0) * 1000.0, NumSources, GroundContactBandHeight);
	UE_LOG(LogTemp, Display, TEXT("[VinePerf] scatter targets : %8.2f ms  -> %d points (spacing %.1f cm)"),
		(T2 - T1) * 1000.0, NumTargets, ScatterTargetSpacing);
	UE_LOG(LogTemp, Display, TEXT("[VinePerf] GenerateVineGPU : %8.2f ms  (%s)"),
		(T3 - T2) * 1000.0, bGenerated ? TEXT("ok") : TEXT("FAILED"));
	UE_LOG(LogTemp, Display, TEXT("[VinePerf] total           : %8.2f ms"), (T3 - T0) * 1000.0);
	UE_LOG(LogTemp, Display,
		TEXT("[VinePerf] capacity: per-source %lld = min(theoretical %lld, share %lld)  [SC.Iteration=%d VoxelSize=%.2f MaxVinePointCount=%d]%s"),
		PerSourceCapacity, TheoreticalBound, PerSourceShare, SC.Iteration, SC.VoxelSize, VV.MaxVinePointCount,
		PerSourceCapacity < TheoreticalBound ? TEXT("  <-- TRUNCATED") : TEXT(""));

	// GenerateVineGPU 只把 pass 记进图，Execute 发生在渲染线程；上面那个数是 game thread 的
	// 准备耗时，不是 GPU 耗时。要看 GPU 就得等一次实际提交。
	UE_LOG(LogTemp, Display,
		TEXT("[VinePerf] 注意：GenerateVineGPU 的耗时是 game-thread 建图/准备，GPU 执行不在其中。")
		TEXT("GPU 侧请用 stat gpu 或 Unreal Insights 抓 VineMesh.Build。"));
}
