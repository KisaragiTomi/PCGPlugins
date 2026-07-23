#include "LandscapeExtra.h"
#include "Landscape.h"
#include "Kismet/GameplayStatics.h"
#include "Generators/RectangleMeshGenerator.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryGeneral.h"
#include "UDynamicMesh.h"
#include "DynamicMeshEditor.h"
#include "DynamicMesh/MeshTransforms.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "LandscapeEdit.h"
#include "LandscapeEditLayer.h"
#include "Util/ColorConstants.h"

using namespace UE::Geometry;

// #if WITH_EDITOR

struct FSafeIndices
{
	int32 X0Y0 = 0;
	int32 X1Y0 = 0;
	int32 X0Y1 = 0;
	int32 X1Y1 = 0;

	float XFraction = 0;
	float YFraction = 0;
};

static FSafeIndices CalcSafeIndices(FVector2D LocalPoint, int32 Stride)
{
	check(Stride != 0);

	const FVector2D ClampedLocalPoint = LocalPoint.ClampAxes(0.0, FVector2D::FReal(Stride-1));

	FSafeIndices Result;
	const int32 CellX0 = FMath::FloorToInt(ClampedLocalPoint.X);
	const int32 CellY0 = FMath::FloorToInt(ClampedLocalPoint.Y);
	const int32 CellX1 = FMath::Min(CellX0+1, Stride-1);
	const int32 CellY1 = FMath::Min(CellY0+1, Stride-1);

	Result.X0Y0 = CellX0 + CellY0 * Stride;
	Result.X1Y0 = CellX1 + CellY0 * Stride;
	Result.X0Y1 = CellX0 + CellY1 * Stride;
	Result.X1Y1 = CellX1 + CellY1 * Stride;

	Result.XFraction = FMath::Fractional(ClampedLocalPoint.X);
	Result.YFraction = FMath::Fractional(ClampedLocalPoint.Y);

	return Result;
}

// ---------- Shared landscape height reading context ----------

struct FLandscapeHeightReadContext
{
	ALandscape* Landscape = nullptr;
	FTransform LandscapeTransform;
	FIntPoint KeyMin = FIntPoint::ZeroValue;
	FIntPoint KeyMax = FIntPoint::ZeroValue;
	int32 XNum = 0;
	int32 YNum = 0;
	int32 NumVertices = 0;
	int32 ComponentSizeQuads = 0;
	TArray<uint16> HeightValues;
	bool bValid = false;
};

static FLandscapeHeightReadContext ReadLandscapeHeightInBox(FVector Center, FVector Extent, int32 ExtentPlus)
{
	FLandscapeHeightReadContext Ctx;
	if (Extent.Length() < .001) return Ctx;

	const FVector Min = Center - Extent;
	const FVector Max = Center + Extent;

	if (UWorld* World = GEngine->GetWorldFromContextObject(GWorld, EGetWorldErrorMode::LogAndReturnNull))
	{
		for (TActorIterator<ALandscape> It(World, ALandscape::StaticClass()); It; ++It)
		{
			Ctx.Landscape = *It;
			break;
		}
	}
	if (!Ctx.Landscape) return Ctx;

	Ctx.LandscapeTransform = Ctx.Landscape->GetTransform();
	const ULandscapeInfo* LandscapeInfo = Ctx.Landscape->GetLandscapeInfo();
	const FVector MaxLocalPoint = Ctx.LandscapeTransform.InverseTransformPosition(Max);
	const FVector MinLocalPoint = Ctx.LandscapeTransform.InverseTransformPosition(Min);

	Ctx.KeyMax = FIntPoint(FMath::CeilToInt(MaxLocalPoint.X) + ExtentPlus, FMath::CeilToInt(MaxLocalPoint.Y) + ExtentPlus);
	Ctx.KeyMin = FIntPoint(FMath::Floor(MinLocalPoint.X) - ExtentPlus, FMath::Floor(MinLocalPoint.Y) - ExtentPlus);
	Ctx.ComponentSizeQuads = LandscapeInfo->ComponentSizeQuads;
	Ctx.XNum = Ctx.KeyMax.X - Ctx.KeyMin.X;
	Ctx.YNum = Ctx.KeyMax.Y - Ctx.KeyMin.Y;
	Ctx.NumVertices = Ctx.XNum * Ctx.YNum;

	if (Ctx.NumVertices <= 0) return Ctx;

	Ctx.HeightValues.AddZeroed(Ctx.NumVertices);
	FLandscapeEditDataInterface LandscapeEdit(Ctx.Landscape->GetLandscapeInfo());
	LandscapeEdit.SetShouldDirtyPackage(false);
	LandscapeEdit.GetHeightDataFast(Ctx.KeyMin.X, Ctx.KeyMin.Y, Ctx.KeyMax.X - 1, Ctx.KeyMax.Y - 1,
		(uint16*)Ctx.HeightValues.GetData(), 0);

	Ctx.bValid = true;
	return Ctx;
}

 

UDynamicMesh* ULandscapeExtra::CreateProjectPlane(UDynamicMesh* Mesh, FVector Center, FVector Extent, int32 ExtentPlus)
{
	FLandscapeHeightReadContext Ctx = ReadLandscapeHeightInBox(Center, Extent, ExtentPlus);
	if (!Ctx.bValid) return nullptr;

	UDynamicMesh* PlaneMesh = NewObject<UDynamicMesh>();
	FRectangleMeshGenerator RectGenerator;
	RectGenerator.Origin = FVector3d(0, 0, 0);
	RectGenerator.Normal = FVector3f::UnitZ();
	RectGenerator.Width = 100;
	RectGenerator.Height = 100;
	RectGenerator.WidthVertexCount = FMath::Max(0, Ctx.XNum);
	RectGenerator.HeightVertexCount = FMath::Max(0, Ctx.YNum);
	RectGenerator.bSinglePolyGroup = true;
	RectGenerator.Generate();

	FGeometryScriptPrimitiveOptions PrimitiveOptions;
	UGeometryGeneral::AppendPrimitive(PlaneMesh, &RectGenerator, FTransform::Identity, PrimitiveOptions);

	TArray<FVector> Vertices;
	Vertices.Reserve(Ctx.NumVertices);
	for (int32 j = Ctx.KeyMin.Y; j < Ctx.KeyMax.Y; j++)
	{
		for (int32 i = Ctx.KeyMin.X; i < Ctx.KeyMax.X; i++)
		{
			FVector Pos = FVector(i, j, LandscapeDataAccess::GetLocalHeight(Ctx.HeightValues[(j - Ctx.KeyMin.Y) * Ctx.XNum + (i - Ctx.KeyMin.X)]));
			Vertices.Add(Pos);
		}
	}

	PlaneMesh->EditMesh([&](FDynamicMesh3& EditMesh)
	{
		for (int32 i = 0; i < Ctx.NumVertices; i++)
		{
			if (EditMesh.IsVertex(i))
			{
				EditMesh.SetVertex(i, (FVector3d)Vertices[i]);
			}
		}
		MeshTransforms::ApplyTransform(EditMesh, (FTransformSRT3d)(Ctx.LandscapeTransform), true);
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);

	FDynamicMesh3 MeshCopy;
	PlaneMesh->ProcessMesh([&](const FDynamicMesh3& EditMesh)
	{
		MeshCopy = EditMesh;
	});
	Mesh->SetMesh(MoveTemp(MeshCopy));
	return Mesh;
}

bool ULandscapeExtra::ProjectPoint(FVector SourceLocation, FVector& OutLocation, FVector& OutNormal)
{
	
	OutLocation = FVector::ZeroVector;
	OutNormal = FVector::ZeroVector;
	ALandscape* Landscape = nullptr;
	if (UWorld* World = GEngine->GetWorldFromContextObject(GWorld, EGetWorldErrorMode::LogAndReturnNull))
	{
		for (TActorIterator<ALandscape> It(World, ALandscape::StaticClass()); It; ++It)
		{
			Landscape = *It;
			break;
		}
	}
	if (!Landscape)
	{
		return false;
	}
	const ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
	int32 ComponentSizeQuads = LandscapeInfo->ComponentSizeQuads;
	const FVector LocalPoint = Landscape->GetTransform().InverseTransformPosition(SourceLocation);
	const FIntPoint ComponentMapKey(FMath::FloorToInt(LocalPoint.X / ComponentSizeQuads), FMath::FloorToInt(LocalPoint.Y / ComponentSizeQuads));
	ULandscapeComponent* LandscapeComponent = LandscapeInfo->XYtoComponentMap.FindRef(ComponentMapKey);
	if (!LandscapeComponent)
	{
		return false;
	}
	
	FLandscapeComponentDataInterface CDI(LandscapeComponent, 0, /*bWorkInEditingLayer=*/false);

	// CDI.GetRawHeightData() may be nullptr if InComponent has no texture data.
	if (CDI.GetRawHeightData() == nullptr)
	{
		return false ;
	}
	FSafeIndices Indices = CalcSafeIndices(FVector2D(LocalPoint.X - ComponentMapKey.X * ComponentSizeQuads, LocalPoint.Y - ComponentMapKey.Y * ComponentSizeQuads), ComponentSizeQuads + 1);

	FVector WorldPos[4];
	FVector WorldTangentX[4];
	FVector WorldTangentY[4];
	FVector WorldTangentZ[4];
	
	CDI.GetWorldPositionTangents(Indices.X0Y0, WorldPos[0], WorldTangentX[0], WorldTangentY[0], WorldTangentZ[0]);
	CDI.GetWorldPositionTangents(Indices.X1Y0, WorldPos[1], WorldTangentX[1], WorldTangentY[1], WorldTangentZ[1]);
	CDI.GetWorldPositionTangents(Indices.X0Y1, WorldPos[2], WorldTangentX[2], WorldTangentY[2], WorldTangentZ[2]);
	CDI.GetWorldPositionTangents(Indices.X1Y1, WorldPos[3], WorldTangentX[3], WorldTangentY[3], WorldTangentZ[3]);

	const FVector& PositionX0Y0 = WorldPos[0];
	const FVector& PositionX1Y0 = WorldPos[1];
	const FVector& PositionX0Y1 = WorldPos[2];
	const FVector& PositionX1Y1 = WorldPos[3];
	
	const FVector LerpPositionY0 = FMath::Lerp(PositionX0Y0, PositionX1Y0, Indices.XFraction);
	const FVector LerpPositionY1 = FMath::Lerp(PositionX0Y1, PositionX1Y1, Indices.XFraction);
	const FVector Position = FMath::Lerp(LerpPositionY0, LerpPositionY1, Indices.YFraction);
	
	const int32 Seed = 11;
	const float Density = 1;
	
	const FVector& NormalX0Y0 = WorldTangentZ[0];
	const FVector& NormalX1Y0 = WorldTangentZ[1];
	const FVector& NormalX0Y1 = WorldTangentZ[2];
	const FVector& NormalX1Y1 = WorldTangentZ[3];

	const FVector LerpNormalY0 = FMath::Lerp(NormalX0Y0.GetSafeNormal(), NormalX1Y0.GetSafeNormal(), Indices.XFraction).GetSafeNormal();
	const FVector LerpNormalY1 = FMath::Lerp(NormalX0Y1.GetSafeNormal(), NormalX1Y1.GetSafeNormal(), Indices.XFraction).GetSafeNormal();
	const FVector Normal = FMath::Lerp(LerpNormalY0, LerpNormalY1, Indices.YFraction);
	
	FVector TangentX;
	FVector TangentY;
	TangentX = FVector(Normal.Z, 0.f, -Normal.X);
	TangentY = Normal ^ TangentX;
	
	OutLocation = Position;
	OutNormal = Normal.GetSafeNormal();
	return true;
}

TArray<FLinearColor> ULandscapeExtra::GetLandscapeData(FVector Center, FVector Extent, int32 ExtentPlus)
{
	TArray<FLinearColor> HeightNormals;
	FLandscapeHeightReadContext Ctx = ReadLandscapeHeightInBox(Center, Extent, ExtentPlus);
	if (!Ctx.bValid) return HeightNormals;

	const int32 X1 = Ctx.KeyMin.X;
	const int32 Y1 = Ctx.KeyMin.Y;
	const int32 X2 = Ctx.KeyMax.X - 1;
	const int32 Y2 = Ctx.KeyMax.Y - 1;

	HeightNormals.Reserve(Ctx.NumVertices);
	for (int32 j = Ctx.KeyMin.Y; j < Ctx.KeyMax.Y; j++)
	{
		for (int32 i = Ctx.KeyMin.X; i < Ctx.KeyMax.X; i++)
		{
			FVector Pos = FVector(i, j, LandscapeDataAccess::GetLocalHeight(Ctx.HeightValues[(j - Ctx.KeyMin.Y) * Ctx.XNum + (i - Ctx.KeyMin.X)]));

			int xPrev = (i == X1) ? Ctx.KeyMin.X : i - 1;
			int xNext = (i == X2) ? i : i + 1;
			int yPrev = (j == Y1) ? Ctx.KeyMin.Y : j - 1;
			int yNext = (j == Y2) ? j : j + 1;

			FVector PosXN = FVector(xNext, j, LandscapeDataAccess::GetLocalHeight(Ctx.HeightValues[(j - Ctx.KeyMin.Y) * Ctx.XNum + (xNext - Ctx.KeyMin.X)]));
			FVector PosXP = FVector(xPrev, j, LandscapeDataAccess::GetLocalHeight(Ctx.HeightValues[(j - Ctx.KeyMin.Y) * Ctx.XNum + (xPrev - Ctx.KeyMin.X)]));
			FVector PosYN = FVector(i, yNext, LandscapeDataAccess::GetLocalHeight(Ctx.HeightValues[(yNext - Ctx.KeyMin.Y) * Ctx.XNum + (i - Ctx.KeyMin.X)]));
			FVector PosYP = FVector(i, yPrev, LandscapeDataAccess::GetLocalHeight(Ctx.HeightValues[(yPrev - Ctx.KeyMin.Y) * Ctx.XNum + (i - Ctx.KeyMin.X)]));
			FVector DX = (PosXN - PosXP).GetSafeNormal();
			FVector DY = (PosYN - PosYP).GetSafeNormal();
			FVector Normal = FVector::CrossProduct(DX, DY).GetSafeNormal();
			HeightNormals.Add(FLinearColor(Normal.X, Normal.Y, Normal.Z, Pos.Z));
		}
	}
	return HeightNormals;
}

void ULandscapeExtra::CreateLandscapeTextureData(FCSReadLandscapeData& LandscapeData, FVector Center, FVector Extent, int32 ExtentPlus)
{
	FLandscapeHeightReadContext Ctx = ReadLandscapeHeightInBox(Center, Extent, ExtentPlus);
	if (!Ctx.bValid) return;

	const FVector Min = Center - Extent;
	const FVector Max = Center + Extent;
	const int32 XMin = Ctx.KeyMin.X;
	const int32 YMin = Ctx.KeyMin.Y;
	const int32 XMax = Ctx.KeyMax.X;
	const int32 YMax = Ctx.KeyMax.Y;
	const int32 X1 = XMin;
	const int32 Y1 = YMin;
	const int32 X2 = XMax - 1;
	const int32 Y2 = YMax - 1;

	int32 NumPixelX = 0;
	for (int i = 5; i < 12; i++)
	{
		if (1 << i >= Ctx.XNum) { NumPixelX = 1 << i; break; }
	}
	if (NumPixelX == 0) return;

	int32 NumPixelY = 0;
	for (int i = 5; i < 12; i++)
	{
		if (1 << i >= Ctx.YNum) { NumPixelY = 1 << i; break; }
	}
	if (NumPixelY == 0) return;

	TArray<FLinearColor> HeightNormals;
	TArray<FLinearColor> ValidHeightNormals;
	HeightNormals.AddZeroed(NumPixelX * NumPixelY);
	ValidHeightNormals.Reserve(Ctx.XNum * Ctx.YNum);

	for (int32 j = YMin; j < YMax; j++)
	{
		for (int32 i = XMin; i < XMax; i++)
		{
			FVector Pos = FVector(i, j, LandscapeDataAccess::GetLocalHeight(Ctx.HeightValues[(j - YMin) * Ctx.XNum + (i - XMin)]));
			Pos = Ctx.LandscapeTransform.TransformPosition(Pos);

			int xPrev = (i == X1) ? XMin : i - 1;
			int xNext = (i == X2) ? i : i + 1;
			int yPrev = (j == Y1) ? YMin : j - 1;
			int yNext = (j == Y2) ? j : j + 1;

			FVector PosXN = FVector(xNext, j, LandscapeDataAccess::GetLocalHeight(Ctx.HeightValues[(j - YMin) * Ctx.XNum + (xNext - XMin)]));
			FVector PosXP = FVector(xPrev, j, LandscapeDataAccess::GetLocalHeight(Ctx.HeightValues[(j - YMin) * Ctx.XNum + (xPrev - XMin)]));
			FVector PosYN = FVector(i, yNext, LandscapeDataAccess::GetLocalHeight(Ctx.HeightValues[(yNext - YMin) * Ctx.XNum + (i - XMin)]));
			FVector PosYP = FVector(i, yPrev, LandscapeDataAccess::GetLocalHeight(Ctx.HeightValues[(yPrev - YMin) * Ctx.XNum + (i - XMin)]));
			FVector DX = (PosXN - PosXP).GetSafeNormal();
			FVector DY = (PosYN - PosYP).GetSafeNormal();
			FVector Normal = FVector::CrossProduct(DX, DY).GetSafeNormal();

			FLinearColor Color(Normal.X, Normal.Y, Normal.Z, Pos.Z);
			HeightNormals[i - XMin + (j - YMin) * NumPixelX] = Color;
			ValidHeightNormals.Add(Color);
		}
	}

	TArray<FFloat16Color> HeightNormals16;
	HeightNormals16.Reserve(HeightNormals.Num());
	for (const FLinearColor& C : HeightNormals)
	{
		HeightNormals16.Add(FFloat16Color(C));
	}

	LandscapeData.Colors16 = MoveTemp(HeightNormals16);
	LandscapeData.ValidColors = MoveTemp(ValidHeightNormals);
	LandscapeData.Colors = MoveTemp(HeightNormals);
	LandscapeData.MapMin = Ctx.LandscapeTransform.TransformPosition(FVector(XMin - .5, YMin - .5, 0)) + Max * FVector(0, 0, 1);
	LandscapeData.MapMax = Ctx.LandscapeTransform.TransformPosition(FVector(NumPixelX + XMin - .5, NumPixelY + YMin - .5, 0)) + Min * FVector(0, 0, 1);
	LandscapeData.ValidMapMin = Ctx.LandscapeTransform.TransformPosition(FVector(XMin - .5, YMin - .5, 0));
	LandscapeData.ValidMapMax = Ctx.LandscapeTransform.TransformPosition(FVector(Ctx.XNum + XMin - .5, Ctx.YNum + YMin - .5, 0));
	LandscapeData.TextureSize = FIntVector2(NumPixelX, NumPixelY);
	LandscapeData.TextureValidSize = FIntVector2(Ctx.XNum, Ctx.YNum);
	LandscapeData.ValidUVRange = FVector2f(Ctx.XNum / float(NumPixelX), Ctx.YNum / float(NumPixelY));
	LandscapeData.ReadRange = FIntVector4(X1, Y1, X2, Y2);
	LandscapeData.Transform = Ctx.LandscapeTransform;
	LandscapeData.TextureBounds = FBoxSphereBounds(FBox(LandscapeData.MapMin + .5, LandscapeData.MapMax + .5));
	LandscapeData.ValidTextureBounds = FBoxSphereBounds(FBox(LandscapeData.ValidMapMin + .5, LandscapeData.ValidMapMax + .5));
}

TArray<FLinearColor> ULandscapeExtra::CreateLandscapeMeshTextureData(FVector& MapMin, FVector& MapMax, FVector Center,
	FVector Extent, int32 TextureSize, int32 ExtentPlus)
{
	TArray<FLinearColor> OutHeightNormals;
	FLandscapeHeightReadContext Ctx = ReadLandscapeHeightInBox(Center, Extent, ExtentPlus);
	if (!Ctx.bValid) return OutHeightNormals;

	MapMin = Ctx.LandscapeTransform.TransformPosition(FVector(Ctx.KeyMin.X - .5, Ctx.KeyMin.Y - .5, 0));
	MapMax = Ctx.LandscapeTransform.TransformPosition(FVector(Ctx.KeyMax.X - .5, Ctx.KeyMax.Y - .5, 0));

	UDynamicMesh* PlaneMesh = NewObject<UDynamicMesh>();
	FRectangleMeshGenerator RectGenerator;
	RectGenerator.Origin = FVector3d(0, 0, 0);
	RectGenerator.Normal = FVector3f::UnitZ();
	RectGenerator.Width = 100;
	RectGenerator.Height = 100;
	RectGenerator.WidthVertexCount = FMath::Max(0, Ctx.XNum);
	RectGenerator.HeightVertexCount = FMath::Max(0, Ctx.YNum);
	RectGenerator.bSinglePolyGroup = true;
	RectGenerator.Generate();

	FGeometryScriptPrimitiveOptions PrimitiveOptions;
	UGeometryGeneral::AppendPrimitive(PlaneMesh, &RectGenerator, FTransform::Identity, PrimitiveOptions);

	TArray<FVector> Vertices;
	Vertices.Reserve(Ctx.NumVertices);
	for (int32 j = Ctx.KeyMin.Y; j < Ctx.KeyMax.Y; j++)
	{
		for (int32 i = Ctx.KeyMin.X; i < Ctx.KeyMax.X; i++)
		{
			FVector Pos = FVector(i, j, LandscapeDataAccess::GetLocalHeight(Ctx.HeightValues[(j - Ctx.KeyMin.Y) * Ctx.XNum + (i - Ctx.KeyMin.X)]));
			Vertices.Add(Pos);
		}
	}

	PlaneMesh->EditMesh([&](FDynamicMesh3& EditMesh)
	{
		for (int32 i = 0; i < Ctx.NumVertices; i++)
		{
			if (EditMesh.IsVertex(i))
			{
				EditMesh.SetVertex(i, (FVector3d)Vertices[i]);
			}
		}
		MeshTransforms::ApplyTransform(EditMesh, (FTransformSRT3d)(Ctx.LandscapeTransform), true);
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);

	return OutHeightNormals;
}
