#include "PointFunction.h"

int32 UPointFunction::FindNearPointIteration(TArray<FVector> TarLocations, FVector SourceLocation)
{
	int32 Index = -1;
	int32 Iteration = TarLocations.Num();
	float Dist = 999999999.0;
	for (int32 i = 0; i < Iteration; i++)
	{
		float TarDist = FVector::Dist(TarLocations[i], SourceLocation);
		if (TarDist < Dist)
		{
			Dist = TarDist;
			Index = i;
		}
	}
	
	return Index;
}


int32 UPointFunction::FindNearPointIteration(TArray<FVector> TarLocations, FVector SourceLocation,  TFunction<bool(int32)> Func)
{
	int32 Index = -1;
	int32 Iteration = TarLocations.Num();
	float Dist = 999999999.0;
	for (int32 i = 0; i < Iteration; i++)
	{
		if (!Func(i))
			continue;
		
		float TarDist = FVector::Dist(TarLocations[i], SourceLocation);
		if (TarDist < Dist)
		{
			Dist = TarDist;
			Index = i;
		}
	}
	
	return Index;
}
// int32 UGeometryScriptLibrary_PolyPathFunctions::GetNearestVertexIndex(FGeometryScriptPolyPath PolyPath, FVector Point)
// {
// 	if (PolyPath.Path.IsValid())
// 	{
// 		return UE::Geometry::CurveUtil::FindNearestIndex<double, FVector>(*PolyPath.Path, Point);
// 	}
// 	return -1;
// }