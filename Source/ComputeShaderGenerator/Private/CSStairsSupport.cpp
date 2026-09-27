#include "CSStairsSupport.h"

#include "CSHouseFrame.h"
#include "CSHouseTrim.h"

namespace
{
// A wall coordinate is distance ALONG the horizontal curve; heights remain world Z.
struct FCSStairsSupportCurve
{
	const CSStairs::FRun& Run;
	TArray<float> Distances;
	explicit FCSStairsSupportCurve(const CSStairs::FRun& InRun) : Run(InRun)
	{
		if (Run.Samples.IsEmpty()) return;
		Distances.Add(0.0f);
		for (int32 I = 1; I < Run.Samples.Num(); ++I) Distances.Add(Distances.Last() + float(FVector2D::Distance(FVector2D(Run.Samples[I - 1].Position), FVector2D(Run.Samples[I].Position))));
	}
	float Length() const { return Distances.IsEmpty() ? 0.0f : Distances.Last(); }
	CSStairs::FSample At(float S) const
	{
		int32 I = 0;
		while (I + 2 < Distances.Num() && Distances[I + 1] < S) ++I;
		const float T = FMath::Clamp((S - Distances[I]) / FMath::Max(Distances[I + 1] - Distances[I], 0.01f), 0.0f, 1.0f);
		CSStairs::FSample P;
		P.Position = FMath::Lerp(Run.Samples[I].Position, Run.Samples[I + 1].Position, double(T));
		P.Width = FMath::Lerp(Run.Samples[I].Width, Run.Samples[I + 1].Width, T);
		P.Dir = FMath::Lerp(Run.Samples[I].Dir, Run.Samples[I + 1].Dir, double(T)).GetSafeNormal();
		if (P.Dir.IsNearlyZero()) P.Dir = FVector2D(Run.Samples[I + 1].Position - Run.Samples[I].Position).GetSafeNormal();
		return P;
	}
};

constexpr float CSStairsSupport_RingDepth = 30.0f;
constexpr float CSStairsSupport_BrickLength = 42.0f;

float CSStairsSupport_Ground(const FCSStairsSupportCurve& Curve, float S, const CSStairs::FGroundSampler& Ground)
{
	const CSStairs::FSample P = Curve.At(S);
	const FVector2D Perp(-P.Dir.Y, P.Dir.X);
	// Full support width, unlike the narrower tread probe. Neither exposed side may float.
	return FMath::Min3(Ground(FVector2D(P.Position)), Ground(FVector2D(P.Position) + Perp * (P.Width * 0.5)), Ground(FVector2D(P.Position) - Perp * (P.Width * 0.5)));
}

void CSStairsSupport_Emit(const FCSStairsSupportCurve& Curve, float S, float Z, float Length, float Height,
	const FVector2f& Tangent, float WidthInset, TArray<CSStairs::FBrick>& Out)
{
	if (Length <= 0.5f || Height <= 0.5f || Out.Num() >= 65536) return;
	const CSStairs::FSample P = Curve.At(S);
	const FVector Along(P.Dir.X, P.Dir.Y, 0.0);
	const FVector X = (Along * Tangent.X + FVector::UpVector * Tangent.Y).GetSafeNormal();
	const FVector Y(-P.Dir.Y, P.Dir.X, 0.0);
	CSStairs::FBrick& B = Out.AddDefaulted_GetRef();
	B.Center = FVector(P.Position.X, P.Position.Y, Z);
	B.Rotation = FRotationMatrix::MakeFromXY(X, Y).ToQuat();
	B.Size = FVector(Length, FMath::Max(P.Width - WidthInset, 30.0f), Height);
	B.StepIndex = INDEX_NONE;
}
}

namespace CSStairs
{
void PlanSupportOpenings(const FRun& Run, const FParams& Params, const FGroundSampler& Ground, TArray<FCSWallOpening>& OutOpenings)
{
	OutOpenings.Reset();
	if (!Ground || !Params.bSolidToGround || !Params.bArchedSupport || Run.Type == ESegmentType::Ladder || Run.Samples.Num() < 2) return;
	const FCSStairsSupportCurve Curve(Run);
	if (Curve.Length() < 100.0f) return;
	// TG preprocess_curve: nominal 400 cm per arch. Clearance and pier width below are UE fit rules,
	// not guessed constants from the untraced ArchWalker height branch (appendix D §3.3).
	const int32 Count = FMath::Clamp(FMath::RoundToInt(Curve.Length() / 400.0f), 1, 128);
	const float Cell = Curve.Length() / Count;
	for (int32 I = 0; I < Count; ++I)
	{
		const float Center = Cell * (I + 0.5f);
		const float GroundZ = CSStairsSupport_Ground(Curve, Center, Ground);
		const float Space = float(Curve.At(Center).Position.Z) - GroundZ - Params.MinBlockHeight - CSStairsSupport_RingDepth;
		const float Width = FMath::Min(Cell - 70.0f, Space * 2.0f);
		if (Width < 80.0f) continue;
		float Bottom = GroundZ - Params.BuryTolerance;
		float Crown = float(Curve.At(Center).Position.Z) - Params.MinBlockHeight - CSStairsSupport_RingDepth - 10.0f;
		const float Rise = FMath::Min(Width * 0.5f, FMath::Max(Space * 0.8f, 20.0f));
		for (int32 K = 0; K <= 16; ++K)
		{
			const float X = float(K) / 8.0f - 1.0f;
			const float S = Center + X * Width * 0.5f;
			Bottom = FMath::Min(Bottom, CSStairsSupport_Ground(Curve, S, Ground) - Params.BuryTolerance);
			const float Drop = Rise * (1.0f - FMath::Sqrt(FMath::Max(1.0f - X * X, 0.0f)));
			Crown = FMath::Min(Crown, float(Curve.At(S).Position.Z) - Params.MinBlockHeight - CSStairsSupport_RingDepth - 10.0f + Drop);
		}
		if (Crown - GroundZ < 40.0f) continue;
		FCSWallOpening O;
		O.CenterS = Center;
		O.Width = Width;
		O.Z0 = Bottom;
		O.Z1 = Crown;
		O.ArchRise = FMath::Min(Rise, Crown - Bottom);
		O.Shape = ECSOpeningShape::Arch;
		OutOpenings.Add(O);
	}
}

void BuildRunSupports(const FRun& Run, const FParams& Params, const FGroundSampler& Ground, uint32 Seed, TArray<FBrick>& OutBricks)
{
	if (!Ground || !Params.bSolidToGround || Run.Type == ESegmentType::Ladder || Run.Samples.Num() < 2) return;
	const FCSStairsSupportCurve Curve(Run);
	if (Curve.Length() < 1.0f) return;
	TArray<FCSWallOpening> Openings;
	PlanSupportOpenings(Run, Params, Ground, Openings);
	float Lowest = TNumericLimits<float>::Max(), Highest = -TNumericLimits<float>::Max();
	for (float S : Curve.Distances)
	{
		Lowest = FMath::Min(Lowest, CSStairsSupport_Ground(Curve, S, Ground) - Params.BuryTolerance);
		Highest = FMath::Max(Highest, float(Curve.At(S).Position.Z) - Params.MinBlockHeight * 0.5f);
	}
	const float Course = FMath::Max(Params.SupportCourseHeight, 10.0f);
	const float WidthInset = Params.bNarrowByRailing ? Params.RailingNarrow : 0.0f;
	CSHouseFrame::FBrickParams BrickParams;
	BrickParams.Length = CSStairsSupport_BrickLength;
	BrickParams.MaxBricks = 65536;
	CSHouseFrame::FWallFrame Frame;
	TArray<CSHouseTrim::FRun> Spans;
	TArray<CSHouseFrame::FElement> Elements;
	const int32 First = FMath::FloorToInt(Lowest / Course);
	const int32 Last = FMath::Min(FMath::CeilToInt(Highest / Course), First + 512);
	for (int32 Row = First; Row < Last; ++Row)
	{
		const float Z0 = Row * Course, Z1 = Z0 + Course;
		CSHouseTrim::FBand Band;
		Band.CenterZ = (Z0 + Z1) * 0.5f;
		Band.HalfHeight = Course * 0.5f;
		Spans.Reset(); Elements.Reset();
		CSHouseTrim::SplitEdge(0, 0.0f, Curve.Length(), Band, 0.0f, Openings, 1.0f, Spans);
		int32 Cursor = 0;
		for (const CSHouseTrim::FRun& Span : Spans)
		{
			// Alternate half bricks at the ends to stagger the mortar joints.
			const float FirstEnd = (Row & 1) ? FMath::Min(Span.S0 + BrickParams.Length * 0.5f, Span.S1) : Span.S0;
			if (FirstEnd > Span.S0) CSHouseFrame::AppendFlatRun(Frame, Span.S0, FirstEnd, Band.CenterZ, Seed, BrickParams, Elements, Cursor);
			CSHouseFrame::AppendFlatRun(Frame, FirstEnd, Span.S1, Band.CenterZ, Seed, BrickParams, Elements, Cursor);
		}
		for (const CSHouseFrame::FElement& E : Elements)
		{
			for (int32 I = 0; I < E.BrickCount; ++I)
			{
				FVector2f SZ, Tangent;
				CSHouseFrame::EvalPath(E.Path, E.HalfLen + I * E.Pitch, SZ, Tangent);
				const float Half = E.HalfLen;
				const float Low = FMath::Max(Z0, CSStairsSupport_Ground(Curve, SZ.X, Ground) - Params.BuryTolerance);
				const float Top = FMath::Min(Z1, float(FMath::Min3(Curve.At(SZ.X - Half).Position.Z, Curve.At(SZ.X).Position.Z, Curve.At(SZ.X + Half).Position.Z)) - Params.MinBlockHeight * 0.5f);
				CSStairsSupport_Emit(Curve, SZ.X, (Low + Top) * 0.5f, Half * 2.0f + 0.8f, Top - Low + (Top > Low ? 0.6f : 0.0f), Tangent, WidthInset, OutBricks);
			}
		}
	}
	for (const FCSWallOpening& O : Openings)
	{
		CSHouseFrame::FPath Path;
		if (!CSHouseFrame::MakeOpeningPath(O, Path)) continue;
		// Jambs are already solid coursed masonry; only the curved ring needs radial stones.
		Path.bLeftJamb = Path.bRightJamb = false;
		float Scale = 0.0f;
		const int32 Count = CSHouseFrame::SolveRun(Path.TotalLen(), 32.0f, 0.0f, Scale);
		for (int32 I = 0; I < Count; ++I)
		{
			FVector2f SZ, T;
			CSHouseFrame::EvalPath(Path, (I + 0.5f) * 32.0f * Scale, SZ, T);
			const FVector2f Outward(-T.Y, T.X);
			SZ += Outward * (CSStairsSupport_RingDepth * 0.5f - 1.0f);
			CSStairsSupport_Emit(Curve, SZ.X, SZ.Y, 32.0f * Scale + 2.0f, CSStairsSupport_RingDepth, T, WidthInset, OutBricks);
		}
	}
}
}
