#include "CSStairsConnection.h"

FCSStairsTerraceConnection CSStairs_ConnectTerrace(const FCSStairsTerraceSite& Site,
	const FVector& Endpoint, const FVector& Neighbor, float Width, float HorizontalRange, float VerticalRange)
{
	FCSStairsTerraceConnection Best;
	if (!Site.Roof.bFlat || !Site.Roof.Footprint.IsValidFootprint() || Width <= 0.0f || HorizontalRange <= 0.0f || VerticalRange < 0.0f) return Best;
	const FVector P = Site.World.InverseTransformPosition(Endpoint);
	const FVector N = Site.World.InverseTransformPosition(Neighbor);
	const double DeckZ = Site.Roof.EaveZ + 0.5; // 与房体露台顶面同高。
	if (FMath::Abs(P.Z - DeckZ) > VerticalRange) return Best;
	const FVector2D Toward = FVector2D(P - N).GetSafeNormal();
	if (Toward.IsNearlyZero()) return Best;
	const double Thickness = FMath::Clamp(double(Site.WallThickness), 12.0, FMath::Max(12.0, Site.Roof.MaxInset() * 0.5));
	for (int32 I = 0; I < Site.Roof.Footprint.NumEdges(); ++I)
	{
		const FCSHouseEdgeFrame E = CSHouse_GetEdge(I, Site.Roof.Footprint, 0.0f);
		const double Facing = FVector2D::DotProduct(Toward, E.In);
		if (Facing < 0.5 || FVector2D::DotProduct(FVector2D(N) - E.Start, E.In) >= 0.0) continue;
		const double Half = (double(Width) + 20.0) / (2.0 * Facing);
		const double Margin = Thickness + Half;
		if (E.Len <= Margin * 2.0) continue;
		const double Along = FMath::Clamp(FVector2D::DotProduct(FVector2D(P) - E.Start, E.U), Margin, double(E.Len) - Margin);
		const FVector2D Gate = E.Start + E.U * Along;
		const double XYDistance = FVector2D::DistSquared(FVector2D(P), Gate);
		if (XYDistance > FMath::Square(double(HorizontalRange))) continue;
		const double Score = XYDistance + FMath::Square(P.Z - DeckZ);
		if (Score >= Best.DistanceSquared) continue;
		const FVector2D Landing = Gate + Toward * ((Thickness + 10.0) / Facing);
		if (Site.Roof.InsetDistance(Landing) < Thickness) continue;
		Best.bConnected = true;
		Best.Position = Site.World.TransformPosition(FVector(Landing.X, Landing.Y, DeckZ));
		Best.Opening = { I, Along - Half, Along + Half };
		Best.DistanceSquared = Score;
	}
	return Best;
}
