#include "CSStairs.h"

namespace CSStairs
{
ESegmentType ClassifySegment(float DeltaHeight, float HorizontalChord, const FParams& Params)
{
	// 竖直段（水平弦为零）没有坡度可言，按最陡那一档走。
	if (HorizontalChord <= UE_KINDA_SMALL_NUMBER)
	{
		return FMath::Abs(DeltaHeight) <= UE_KINDA_SMALL_NUMBER ? ESegmentType::Walkway : ESegmentType::Ladder;
	}
	const float Slope = FMath::Abs(DeltaHeight) / HorizontalChord;
	// 边界归**缓**的一侧：TG 的比较是 `s ≤ 0.25` / `s ≤ 2.5`（附录 D §1.3）。
	if (Slope <= Params.WalkwayMaxSlope) return ESegmentType::Walkway;
	if (Slope <= Params.LadderMinSlope) return ESegmentType::Steps;
	return ESegmentType::Ladder;
}

float ClampToGround(const FVector& Position, const FVector2D& Dir, float Width, const FGroundSampler& Ground, const FParams& Params)
{
	if (!Ground) return float(Position.Z);
	// 左右各 1/4 宽处取**较高**的地面：斜坡上横穿的楼梯，高的那一侧不该被埋。
	const FVector2D Perp(-Dir.Y, Dir.X);
	const FVector2D XY(Position.X, Position.Y);
	const float Quarter = Width * 0.25f;
	const float GroundZ = FMath::Max(Ground(XY + Perp * Quarter), Ground(XY - Perp * Quarter));
	return FMath::Max(float(Position.Z), GroundZ - Params.BuryTolerance);
}

void ResamplePolyline(TArrayView<const FVector> Points, float Step, TArray<FVector>& OutPoints, TArray<TPair<int32, float>>& OutSource)
{
	OutPoints.Reset();
	OutSource.Reset();
	if (Points.Num() < 2 || Step <= UE_KINDA_SMALL_NUMBER) return;

	// 累计弧长**按 3D 算**（TG `try_new_from_points` 用 √(dx²+dy²+dz²)）：斜边长才是踏步的尺度。
	TArray<double> Cumulative;
	Cumulative.SetNum(Points.Num());
	Cumulative[0] = 0.0;
	for (int32 Index = 1; Index < Points.Num(); ++Index)
	{
		Cumulative[Index] = Cumulative[Index - 1] + FVector::Distance(Points[Index - 1], Points[Index]);
	}
	const double Total = Cumulative.Last();
	if (Total <= UE_KINDA_SMALL_NUMBER) return;

	// `round` 而不是 `ceil`：余数不单独吸收，整段均分 —— 每级略大或略小于步长（附录 D §2.1）。
	// 上限只防病态输入（极长的样条 × 极小的步长把编辑器拖死），同 `SolveBlockLayout` 的 MaxBlocks。
	constexpr int32 MaxSteps = 65536;
	const int32 N = FMath::Clamp(FMath::RoundToInt(Total / double(Step)), 1, MaxSteps);
	OutPoints.Reserve(N + 1);
	OutSource.Reserve(N + 1);

	int32 Segment = 0;
	for (int32 K = 0; K <= N; ++K)
	{
		const double Target = Total * double(K) / double(N);
		while (Segment + 1 < Points.Num() - 1 && Cumulative[Segment + 1] < Target) ++Segment;
		const double SegLen = Cumulative[Segment + 1] - Cumulative[Segment];
		const float T = SegLen > UE_KINDA_SMALL_NUMBER
			? float(FMath::Clamp((Target - Cumulative[Segment]) / SegLen, 0.0, 1.0))
			: 0.0f;
		// 端点逐位保留：插值出来的末点可能差一个 1e-6，而接平台 / 接墙靠的正是端点对齐。
		const FVector P = (K == 0) ? Points[0] : (K == N ? Points.Last() : FMath::Lerp(Points[Segment], Points[Segment + 1], double(T)));
		OutPoints.Add(P);
		OutSource.Add(TPair<int32, float>(K == N ? Points.Num() - 2 : Segment, K == N ? 1.0f : T));
	}
}

void RandomSplits(int32 Count, float Jitter, FRandomStream& Rand, TArray<float>& OutBounds)
{
	OutBounds.Reset();
	const int32 Pieces = FMath::Max(Count, 1);
	// 抖动量夹到 0.495 / 片数：每条分界最多挪不到四分之一片，相邻两条永远不交叉（附录 D §6.4）。
	const float J = FMath::Clamp(Jitter, 0.0f, 0.495f / float(Pieces));
	OutBounds.SetNum(Pieces + 1);
	OutBounds[0] = 0.0f;
	for (int32 Index = 1; Index < Pieces; ++Index)
	{
		OutBounds[Index] = float(Index) / float(Pieces) + (Rand.FRand() - 0.5f) * J;
	}
	OutBounds[Pieces] = 1.0f;
}

bool BuildDoorSteps(const FDoorStepsInput& Door, const FDoorStepsParams& Params, const FGroundSampler& Ground, uint32 Seed, TArray<FBrick>& OutBricks)
{
	if (!Ground || Door.Width <= 1.0f) return false;
	const FVector2D Out = Door.Outward.GetSafeNormal();
	if (Out.IsNearlyZero()) return false;
	const FVector2D Perp(-Out.Y, Out.X);

	// ① 判据（`check_for_door_stairs`）。TG 另有"水边门"一支（水栅格 + 门底 ≤ 10 cm），本项目没有水，不做。
	const float Ground0 = Ground(Door.DoorXY);
	if (Ground0 <= Door.DoorBottomZ - Params.MaxSillGap) return false;          // 门悬空：不是落地门，交给栏杆
	const FVector2D Front = Door.DoorXY + Out * Params.ProbeDistance;
	const float FrontZ = Ground(Front);
	if (Ground0 - FrontZ <= Params.MinDrop) return false;                     // 门外没有下坡
	const float HalfWidth = Door.Width * 0.5f;
	const float LowestZ = FMath::Min3(FrontZ, Ground(Front + Perp * HalfWidth), Ground(Front - Perp * HalfWidth));
	const float Drop = Ground0 - LowestZ;
	if (Drop >= Params.MaxDrop) return false;                                 // 太高

	// ② 砌法（`construct_door_stairs`）。
	const float LayerHeight = FMath::Max(Params.LayerHeight, 1.0f);
	const int32 Rows = FMath::Clamp(FMath::CeilToInt(Drop / LayerHeight), Params.MinRows, Params.MaxRows);
	const float RowDepth = Params.Outreach / float(Rows);
	const int32 Layers = FMath::Max(FMath::CeilToInt(Drop / LayerHeight), 2) - 1;
	// 横向分界点数含两端 ⇒ 块数 = 点数 − 1（至少两块）。
	const int32 Pieces = FMath::Max(FMath::CeilToInt(Door.Width / Params.LateralPieceWidth), 3) - 1;
	const FQuat Rotation(FVector::UpVector, FMath::Atan2(Out.Y, Out.X));

	FRandomStream Rand(static_cast<int32>(Seed));
	TArray<float> LayerBounds;
	// 抖动量是**全区间的比例**：±2 cm 换成 (rand − 0.5) · j 的 j = 4 / H。
	RandomSplits(Layers, 2.0f * Params.LayerJitter / Drop, Rand, LayerBounds);

	TArray<float> Lateral;
	for (int32 Layer = 0; Layer < Layers; ++Layer)
	{
		// 自上而下：第 0 层贴着门下地面。
		const float TopZ = Ground0 - LayerBounds[Layer] * Drop;
		const float BottomZ = Ground0 - LayerBounds[Layer + 1] * Drop;
		// 金字塔：第 k 层铺 min(k + 1, N) 排 —— 顶层一排贴门，往下每层多外伸一排。
		const int32 RowsHere = FMath::Min(Layer + 1, Rows);
		for (int32 Row = 0; Row < RowsHere; ++Row)
		{
			const FVector2D RowCenter = Door.DoorXY + Out * double(Params.FirstRowOffset + Row * RowDepth);
			RandomSplits(Pieces, 2.0f * Params.LateralJitter / Door.Width, Rand, Lateral);
			for (int32 Piece = 0; Piece < Pieces; ++Piece)
			{
				const float U0 = Lateral[Piece];
				const float U1 = Lateral[Piece + 1];
				const FVector2D XY = RowCenter + Perp * double(((U0 + U1) * 0.5f - 0.5f) * Door.Width);

				FBrick Brick;
				Brick.Center = FVector(XY.X, XY.Y, double(TopZ + BottomZ) * 0.5);
				Brick.Rotation = Rotation;
				// 进深 = 一排 + 前后搭 10 cm；竖向 = 这一层的厚度。
				Brick.Size = FVector(RowDepth + Params.RowOverlap, (U1 - U0) * Door.Width, TopZ - BottomZ);
				Brick.StepIndex = Row;
				OutBricks.Add(Brick);
			}
		}
	}
	return true;
}

int32 BuildRunBricks(const FRun& Run, const FParams& Params, const FGroundSampler& Ground, uint32 Seed, TArray<FBrick>& OutBricks)
{
	if (Run.Samples.Num() < 2) return 0;

	// ① 同类型的一串点 → 3D 折线 → 按一级的斜边长等弧长重采样（附录 D §2.1 第 3–4 步）。
	TArray<FVector> Polyline;
	Polyline.Reserve(Run.Samples.Num());
	for (const FSample& Sample : Run.Samples) Polyline.Add(Sample.Position);

	TArray<FVector> Points;
	TArray<TPair<int32, float>> Source;
	ResamplePolyline(Polyline, Params.StepLength, Points, Source);
	if (Points.Num() < 2) return 0;

	// 宽度与方向按重采样点所在的源段线性插值（TG 同样只插这两样 + 支撑系数）。
	TArray<float> Widths;
	TArray<FVector2D> Dirs;
	Widths.SetNum(Points.Num());
	Dirs.SetNum(Points.Num());
	for (int32 Index = 0; Index < Points.Num(); ++Index)
	{
		const int32 Seg = Source[Index].Key;
		const float T = Source[Index].Value;
		const FSample& A = Run.Samples[Seg];
		const FSample& B = Run.Samples[FMath::Min(Seg + 1, Run.Samples.Num() - 1)];
		Widths[Index] = FMath::Lerp(A.Width, B.Width, T);
		Dirs[Index] = FMath::Lerp(A.Dir, B.Dir, double(T)).GetSafeNormal();
		if (Dirs[Index].IsNearlyZero()) Dirs[Index] = A.Dir;
	}

	// 平走道：踏面抬到地面 + 15 cm 以上（TG 只对 type < 2 做这一步）。
	if (Run.Type == ESegmentType::Walkway && Ground)
	{
		for (FVector& P : Points)
		{
			P.Z = FMath::Max(P.Z, double(Ground(FVector2D(P.X, P.Y)) + Params.TreadClearance));
		}
	}

	const int32 StepCount = Points.Num() - 1;

	// ② 较高那一端的加厚（附录 D §2.2「高端加厚」）：那一端水平 56 cm 以内的几级，块高再加一个首级踏高，
	//    免得楼梯顶上接平台 / 接墙的那几级底下露出空当。首级 = 高端那一级。
	const bool bTopIsLast = Points.Last().Z > Points[0].Z;
	const FVector& TopA = bTopIsLast ? Points[StepCount] : Points[0];
	const FVector& TopB = bTopIsLast ? Points[StepCount - 1] : Points[1];
	const float TopRun = float(FVector2D::Distance(FVector2D(TopA.X, TopA.Y), FVector2D(TopB.X, TopB.Y)));
	const float TopRise = float(FMath::Abs(TopA.Z - TopB.Z));
	const int32 ThickenCount = TopRun > UE_KINDA_SMALL_NUMBER ? FMath::FloorToInt(Params.TopThickenReach / TopRun) : 0;

	FRandomStream Rand(static_cast<int32>(Seed));
	int32 Emitted = 0;
	for (int32 K = 0; K < StepCount; ++K)
	{
		const FVector& A = Points[K];
		const FVector& B = Points[K + 1];
		const FVector2D A2(A.X, A.Y), B2(B.X, B.Y);
		const FVector2D Mid2 = (A2 + B2) * 0.5;

		// 踏面标高：两点里高的那个，且不低于地面 + 15（附录 D §2.2）。
		float TreadZ = float(FMath::Max(A.Z, B.Z));
		if (Ground) TreadZ = FMath::Max(TreadZ, Ground(Mid2) + Params.TreadClearance);

		// 块高：至少 60 cm，从踏面往下长 —— 相邻两级互相叠压，底面永远看不到。
		float Height = FMath::Max(float(FMath::Abs(B.Z - A.Z)), Params.MinBlockHeight);
		const int32 FromTop = bTopIsLast ? (StepCount - 1 - K) : K;
		if (FromTop < ThickenCount) Height += TopRise;

		// 宽：两端平均，有栏杆再收窄，夹到下限。
		float Width = (Widths[K] + Widths[K + 1]) * 0.5f;
		if (Params.bNarrowByRailing) Width -= Params.RailingNarrow;
		Width = FMath::Max(Width, Params.MinWidth);

		// 进深：两点水平距离 × 1.13（前后级互搭 13%）。TG 还乘 `1 − rand·0.42·ḡ`，ḡ 是支撑系数 ——
		// 支撑结构不在这一层，ḡ 恒 0，这一项恒为 1。
		const float Run2D = float(FVector2D::Distance(A2, B2));
		const float Depth = FMath::Max(Run2D, 1.0f) * Params.DepthOverlap;

		// 朝向：块的局部 X 沿前进方向。两点水平重合（梯子）时退回插值方向。
		FVector2D Forward = (B2 - A2).GetSafeNormal();
		if (Forward.IsNearlyZero()) Forward = (Dirs[K] + Dirs[K + 1]).GetSafeNormal();
		if (Forward.IsNearlyZero()) Forward = FVector2D(1.0, 0.0);
		const FVector2D Perp(-Forward.Y, Forward.X);
		const FQuat Rotation(FVector::UpVector, FMath::Atan2(Forward.Y, Forward.X));

		// 横向切砖（附录 D §2.2「横向切砖」）：块数定档，分界近似等分再抖（`RandomSplits` 的 TG 语义）。
		int32 Pieces = 1;
		float JitterWidth = Params.SplitJitterWidth;
		if (Width >= Params.TwoBrickMaxWidth)
		{
			const float Wide = FMath::Clamp((Width - 200.0f) / 200.0f, 0.0f, 1.0f);
			const float R = Rand.FRand();
			const float BrickLength = FMath::SmoothStep(0.0f, 1.0f, R) * FMath::Lerp(Params.BrickLengthNarrow, Params.BrickLengthWide, Wide)
				+ Params.BrickLengthBase;
			Pieces = FMath::Max(1, FMath::CeilToInt(Width / BrickLength));
			JitterWidth = FMath::Max(0.3f * BrickLength, Params.SplitJitterWidth);
		}
		else if (Width >= Params.SingleBrickMaxWidth)
		{
			Pieces = Rand.FRand() < 0.5f ? 1 : 2;
		}

		// 一层砖：`[BottomZ, TopZ]` 横向按块数切开。踏步块与支撑砌体共用。
		TArray<float> Bounds;
		auto EmitCourse = [&](float TopZ, float BottomZ, int32 CoursePieces, float CourseJitterWidth)
		{
			RandomSplits(CoursePieces, CourseJitterWidth / Width, Rand, Bounds);
			for (int32 Piece = 0; Piece + 1 < Bounds.Num(); ++Piece)
			{
				const float U0 = Bounds[Piece];
				const float U1 = Bounds[Piece + 1];
				// 片心沿横向从 −w/2 量起。
				const FVector2D PieceXY = Mid2 + Perp * double(((U0 + U1) * 0.5f - 0.5f) * Width);

				FBrick Brick;
				Brick.Center = FVector(PieceXY.X, PieceXY.Y, double(TopZ + BottomZ) * 0.5);
				Brick.Rotation = Rotation;
				Brick.Size = FVector(Depth, (U1 - U0) * Width, TopZ - BottomZ);
				Brick.StepIndex = K;
				OutBricks.Add(Brick);
			}
		};
		EmitCourse(TreadZ, TreadZ - Height, Pieces, JitterWidth);

		// MVP 支撑（`FParams::bSolidToGround`）：踏步块底到地面之间砌成一层层的砖，层缝落在世界 Z 的
		// `SupportCourseHeight` 整数倍上 —— 相邻两级各砌各的，水平缝照样对齐。
		if (Params.bSolidToGround && Ground)
		{
			const float SupportBottom = Ground(Mid2) - Params.BuryTolerance;
			const float Course = FMath::Max(Params.SupportCourseHeight, 5.0f);
			float Top = TreadZ - Height;
			while (Top - SupportBottom > 0.5f)
			{
				// 下一条层缝：Top 以下最近的整数倍；贴着上一层太薄（不到半层）就再往下一条，免得出一片薄砖。
				float Bottom = FMath::FloorToFloat(Top / Course) * Course;
				if (Top - Bottom < Course * 0.5f) Bottom -= Course;
				// 最底一层同理：剩下不到半层就并进这一层，一直砌到地面。
				if (Bottom - SupportBottom < Course * 0.5f) Bottom = SupportBottom;
				// 支撑砖比踏步砖长一档：同样的块数定档，宽楼梯按砖长切。
				int32 CoursePieces = 1;
				float CourseJitter = Params.SplitJitterWidth;
				if (Width >= Params.SingleBrickMaxWidth)
				{
					const float BrickLength = FMath::SmoothStep(0.0f, 1.0f, Rand.FRand()) * (Params.BrickLengthWide - Params.BrickLengthNarrow)
						+ Params.BrickLengthNarrow;
					CoursePieces = FMath::Max(1, FMath::RoundToInt(Width / BrickLength));
					CourseJitter = FMath::Max(0.3f * BrickLength, Params.SplitJitterWidth);
				}
				EmitCourse(Top, Bottom, CoursePieces, CourseJitter);
				Top = Bottom;
			}
		}
		++Emitted;
	}
	return Emitted;
}
}
