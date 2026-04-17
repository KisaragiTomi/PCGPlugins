#include "PolyLine.h"
#include "Curve/CurveUtil.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"

FGeometryScriptPolyPath UPolyLine::SmoothLine(FGeometryScriptPolyPath PolyPath, int NumIterations)
{
	int32 EndIdx = (PolyPath.Path.IsValid()) ? FMath::Max(PolyPath.Path->Num()-1,0) : 0;
	if (PolyPath.Path.IsValid())
	{
		UE::Geometry::CurveUtil::IterativeSmooth<double, FVector>(*PolyPath.Path, 0, EndIdx, 1, NumIterations, false);
	}
	return PolyPath;
}

FGeometryScriptPolyPath UPolyLine::ResamppleByCount(const FGeometryScriptPolyPath PolyPath, int32 NumIterations)
{
	float Sum = 0;
	float CurrentLength = 0;
	FGeometryScriptPolyPath ToReturn;
	ToReturn.Reset();

	TArray<FVector> Vertices = *PolyPath.Path;
	TArray<FVector> PathVertices;
	PathVertices.SetNum(NumIterations);
	int32 NV = Vertices.Num();
	int32 PointCount = 1;
	int32 iterat = NumIterations - 2;

	float ArcLength = UE::Geometry::CurveUtil::ArcLength<float, FVector>(*PolyPath.Path, false);
	float Interval = ArcLength / (NumIterations - 1);
	
	if (Vertices.Num() < 1)
	{
		return ToReturn;
	}

	PathVertices[0] = Vertices[0];
	Sum += FVector::Distance(Vertices[0], Vertices[1]);
	
	FVector Dir = (Vertices[1] - Vertices[0]).GetSafeNormal(.001);
	for (int i = 0; i < iterat; i++)
	{
		CurrentLength += Interval;
		while(CurrentLength > Sum)
		{
			PointCount += 1;
			Sum += FVector::Distance(Vertices[PointCount], Vertices[PointCount-1]);
			Dir = (Vertices[PointCount] - Vertices[PointCount-1]).GetSafeNormal(.001);
		}
		FVector SamplePos = Vertices[PointCount] - Dir * (Sum - CurrentLength);
		PathVertices[i + 1] = SamplePos;
	}
	PathVertices[NumIterations - 1] = Vertices[Vertices.Num()-1];

	ToReturn.Path->Append(PathVertices);
	return ToReturn;
}


FGeometryScriptPolyPath UPolyLine::ResamppleByLength(const FGeometryScriptPolyPath PolyPath, float IntervalExp)
{
	float Sum = 0;
	float CurrentLength = 0;
	FGeometryScriptPolyPath ToReturn;
	ToReturn.Reset();

	float ArcLength = UE::Geometry::CurveUtil::ArcLength<float, FVector>(*PolyPath.Path, false);
	int32 NumIterations = int32(ArcLength / IntervalExp);
	if (NumIterations < 2)
		return ToReturn;
	
	float Interval = ArcLength / NumIterations;
	
	TArray<FVector> Vertices = *PolyPath.Path;
	TArray<FVector> PathVertices;
	PathVertices.SetNum(NumIterations);
	int32 NV = Vertices.Num();
	int32 PointCount = 1;
	int32 iterat = NumIterations - 2;
	
	if (Vertices.Num() < 1)
	{
		return ToReturn;
	}

	PathVertices[0] = Vertices[0];
	Sum += FVector::Distance(Vertices[0], Vertices[1]);
	
	FVector Dir = (Vertices[1] - Vertices[0]).GetSafeNormal(.001);
	for (int i = 0; i < iterat; i++)
	{
		CurrentLength += Interval;
		while(CurrentLength > Sum)
		{
			PointCount += 1;
			Sum += FVector::Distance(Vertices[PointCount], Vertices[PointCount-1]);
			Dir = (Vertices[PointCount] - Vertices[PointCount-1]).GetSafeNormal(.001);
		}
		FVector SamplePos = Vertices[PointCount] - Dir * (Sum - CurrentLength);
		PathVertices[i + 1] = SamplePos;
	}
	PathVertices[NumIterations - 1] = Vertices[Vertices.Num()-1];

	ToReturn.Path->Append(PathVertices);
	return ToReturn;
}

TArray<FTransform> UPolyLine::ConvertPolyPathToTransforms(FGeometryScriptPolyPath PolyPath, bool GenerateRotator)
{
	TArray<FVector> Line = *PolyPath.Path;
	int32 LineVertexNum = Line.Num();
	TArray<FTransform> LineTransforms;
	LineTransforms.Reserve(LineVertexNum);

	for (int32 i = 0; i < LineVertexNum; i++)
	{
		FVector Tangent = UE::Geometry::CurveUtil::Tangent<double, FVector>(Line, i);
		LineTransforms.Add(FTransform(FRotationMatrix::MakeFromX(Tangent).Rotator(), Line[i], FVector::OneVector));
	}
	return LineTransforms;
}


TArray<float> UPolyLine::CurveU(FGeometryScriptPolyPath PolyPath, bool Normalize)
{
	float Sum = 0;
	TArray<FVector> Vertices = (*PolyPath.Path);
	int32 NPt = Vertices.Num();
	TArray<float> CurveU;
	CurveU.Reserve(NPt);
	for (int i = 0; i < NPt; ++i)
	{
		Sum += FVector::Dist(Vertices[i], Vertices[FMath::Max(i-1, 0)]);
		CurveU.Add(Sum);
	}
	if (!Normalize)
		return CurveU;

	for (int32 i = 0; i < NPt; ++i)
	{
		CurveU[i] /= CurveU[NPt - 1]; 
	}
	return CurveU;
}