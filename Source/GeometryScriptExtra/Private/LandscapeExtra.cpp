#include "LandscapeExtra.h"
#include "Landscape.h"
#include "Kismet/GameplayStatics.h"
#include "Generators/RectangleMeshGenerator.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "UDynamicMesh.h"
#include "DynamicMeshEditor.h"
#include "DynamicMesh/MeshTransforms.h"
#include "EngineUtils.h"
#include "LandscapeEdit.h"
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

static void ApplyPrimitiveOptionsToMesh(
	FDynamicMesh3& Mesh, const FTransform& Transform, 
	FGeometryScriptPrimitiveOptions PrimitiveOptions, 
	FVector3d PreTranslate = FVector3d::Zero(),
	TOptional<FQuaterniond> PreRotate = TOptional<FQuaterniond>())
{
	bool bHasTranslate = PreTranslate.SquaredLength() > 0;
	if (PreRotate.IsSet())
	{
		FFrame3d Frame(PreTranslate, *PreRotate);
		MeshTransforms::FrameCoordsToWorld(Mesh, Frame);
	}
	else if (bHasTranslate)
	{
		MeshTransforms::Translate(Mesh, PreTranslate);
	}

	MeshTransforms::ApplyTransform(Mesh, (FTransformSRT3d)Transform, true);
	if (PrimitiveOptions.PolygroupMode == EGeometryScriptPrimitivePolygroupMode::SingleGroup)
	{
		for (int32 tid : Mesh.TriangleIndicesItr())
		{
			Mesh.SetTriangleGroup(tid, 0);
		}
	}
	if (PrimitiveOptions.bFlipOrientation)
	{
		Mesh.ReverseOrientation(true);
		if (Mesh.HasAttributes())
		{
			FDynamicMeshNormalOverlay* Normals = Mesh.Attributes()->PrimaryNormals();
			for (int elemid : Normals->ElementIndicesItr())
			{
				Normals->SetElement(elemid, -Normals->GetElement(elemid));
			}
		}
	}
}

static void AppendPrimitive(
	UDynamicMesh* TargetMesh,
	FMeshShapeGenerator* Generator, 
	FTransform Transform, 
	FGeometryScriptPrimitiveOptions PrimitiveOptions,
	FVector3d PreTranslate = FVector3d::Zero(),
	TOptional<FQuaterniond> PreRotate = TOptional<FQuaterniond>())
{
	if (TargetMesh->IsEmpty())
	{
		TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh)
		{
			EditMesh.Copy(Generator);
			ApplyPrimitiveOptionsToMesh(EditMesh, Transform, PrimitiveOptions, PreTranslate, PreRotate);
		}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	}
	else
	{
		FDynamicMesh3 TempMesh(Generator);
		ApplyPrimitiveOptionsToMesh(TempMesh, Transform, PrimitiveOptions, PreTranslate, PreRotate);
		TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh)
		{
			FMeshIndexMappings TmpMappings;
			FDynamicMeshEditor Editor(&EditMesh);
			Editor.AppendMesh(&TempMesh, TmpMappings);
		}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	}
}

UDynamicMesh* ULandscapeExtra::CreateProjectPlane(UDynamicMesh* Mesh, FVector Center, FVector Extent, int32 ExtentPlus)
{
	UDynamicMesh* PlaneMesh = NewObject<UDynamicMesh>();
	FVector Min = Center - Extent;
	FVector Max = Center + Extent;
	if (Extent.Length() < .001)
	{
		return nullptr;
	}
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
		return nullptr;
	}
	const ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
	const FVector MaxLocalPoint = Landscape->GetTransform().InverseTransformPosition(Max);
	const FVector MinLocalPoint = Landscape->GetTransform().InverseTransformPosition(Min);
	const FIntPoint KeyMax(FMath::CeilToInt(MaxLocalPoint.X) + ExtentPlus, FMath::CeilToInt(MaxLocalPoint.Y) + ExtentPlus);
	const FIntPoint KeyMin(FMath::Floor(MinLocalPoint.X) - ExtentPlus, FMath::Floor(MinLocalPoint.Y) - ExtentPlus);
	int32 ComponentSizeQuads = LandscapeInfo->ComponentSizeQuads;
	int32 XNum = KeyMax.X - KeyMin.X;
	int32 YNum = KeyMax.Y - KeyMin.Y;
	int32 NumVertices = XNum * YNum;
	
	
	FRectangleMeshGenerator RectGenerator;
	RectGenerator.Origin = FVector3d(0, 0, 0);
	RectGenerator.Normal = FVector3f::UnitZ();
	RectGenerator.Width = 100;
	RectGenerator.Height = 100;
	RectGenerator.WidthVertexCount = FMath::Max(0, XNum);
	RectGenerator.HeightVertexCount = FMath::Max(0, YNum);
	RectGenerator.bSinglePolyGroup = true;
	RectGenerator.Generate();
	
	FGeometryScriptPrimitiveOptions PrimitiveOptions;
	AppendPrimitive(PlaneMesh, &RectGenerator, FTransform::Identity, PrimitiveOptions);
	
	TArray<uint16> Values;
	Values.AddZeroed(NumVertices );
	
	FScopedSetLandscapeEditingLayer Scope(Landscape, Landscape->GetLayer(0)->Guid, [&] { /*Landscape->RequestLayersContentUpdate(ELandscapeLayerUpdateMode::Update_All); */});

	int32 X1 = KeyMin.X;
	int32 Y1 = KeyMin.Y;
	int32 X2 = KeyMax.X - 1;
	int32 Y2 = KeyMax.Y - 1;
	FIntPoint MapKey = FIntPoint (X1 / ComponentSizeQuads, Y1 / ComponentSizeQuads );
	ULandscapeComponent* Comp = LandscapeInfo->XYtoComponentMap.FindRef(MapKey);
	FLandscapeEditDataInterface LandscapeEdit(Landscape->GetLandscapeInfo());
	LandscapeEdit.SetShouldDirtyPackage(false);
	LandscapeEdit.GetHeightDataFast(X1, Y1, X2, Y2, (uint16*)Values.GetData(), 0);

	//
	int32 FindIndex = 0;
	TArray<FVector> Vertices;
	Vertices.Reserve(NumVertices);
	for (int32 j = KeyMin.Y; j < KeyMax.Y; j++)
	{
		for (int32 i = KeyMin.X; i < KeyMax.X; i++)
		{
			FVector LandscapePosition = FVector(i , j , LandscapeDataAccess::GetLocalHeight(Values[(j- KeyMin.Y) * XNum + (i - KeyMin.X)]));
			Vertices.Add(LandscapePosition);
		}
	}

	PlaneMesh->EditMesh([&](FDynamicMesh3& EditMesh)
	{
		for (int32 i = 0; i < NumVertices; i++)
		{
			FVector NewPosition = Vertices[i];
			if (EditMesh.IsVertex(i))
			{
				EditMesh.SetVertex(i, (FVector3d)NewPosition);
			}
		}
		MeshTransforms::ApplyTransform(EditMesh, (FTransformSRT3d)(Landscape->GetTransform()), true);
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);

	Vertices.Empty();
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

TArray<FLinearColor> ULandscapeExtra::GetLandscapeData( FVector Center, FVector Extent, int32 ExtentPlus)
{
	TArray<FLinearColor> OutHeightNormals;
	FVector Min = Center - Extent;
	FVector Max = Center + Extent;
	if (Extent.Length() < .001)
	{
		return OutHeightNormals;
	}
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
		return OutHeightNormals;
	}
	const ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
	const FVector MaxLocalPoint = Landscape->GetTransform().InverseTransformPosition(Max);
	const FVector MinLocalPoint = Landscape->GetTransform().InverseTransformPosition(Min);
	const FIntPoint KeyMax(FMath::CeilToInt(MaxLocalPoint.X) + ExtentPlus, FMath::CeilToInt(MaxLocalPoint.Y) + ExtentPlus);
	const FIntPoint KeyMin(FMath::Floor(MinLocalPoint.X) - ExtentPlus, FMath::Floor(MinLocalPoint.Y) - ExtentPlus);
	int32 ComponentSizeQuads = LandscapeInfo->ComponentSizeQuads;
	int32 XNum = KeyMax.X - KeyMin.X;
	int32 YNum = KeyMax.Y - KeyMin.Y;
	int32 NumVertices = XNum * YNum;
	
	TArray<uint16> Values;
	Values.AddZeroed(NumVertices );
	
	FScopedSetLandscapeEditingLayer Scope(Landscape, Landscape->GetLayer(0)->Guid, [&] { /*Landscape->RequestLayersContentUpdate(ELandscapeLayerUpdateMode::Update_All); */});
	
	int32 X1 = KeyMin.X;
	int32 Y1 = KeyMin.Y;
	int32 X2 = KeyMax.X - 1;
	int32 Y2 = KeyMax.Y - 1;
	FLandscapeEditDataInterface LandscapeEdit(Landscape->GetLandscapeInfo());
	LandscapeEdit.SetShouldDirtyPackage(false);
	LandscapeEdit.GetHeightDataFast(X1, Y1, X2, Y2, (uint16*)Values.GetData(), 0);
	
	TArray<FLinearColor> HeightNormals;
	HeightNormals.Reserve(NumVertices);
	for (int32 j = KeyMin.Y; j < KeyMax.Y; j++)
	{
		for (int32 i = KeyMin.X; i < KeyMax.X; i++)
		{
			FVector LandscapePosition = FVector(i , j , LandscapeDataAccess::GetLocalHeight(Values[(j- KeyMin.Y) * XNum + (i - KeyMin.X)]));
			
			int xPrev = (i == X1) ? KeyMin.X : i - 1;
			int xNext = (i == X2) ? i : i + 1;
			int yPrev = (j == Y1) ? KeyMin.Y : j - 1;
			int yNext = (j == Y2) ? j : j + 1;
			
			FVector LandscapePositionXN = FVector(xNext , j , LandscapeDataAccess::GetLocalHeight(Values[(j- KeyMin.Y) * XNum + (xNext - KeyMin.X)]));
			FVector LandscapePositionXP = FVector(xPrev , j , LandscapeDataAccess::GetLocalHeight(Values[(j- KeyMin.Y) * XNum + (xPrev - KeyMin.X)]));
			FVector LandscapePositionYN = FVector(i , yNext , LandscapeDataAccess::GetLocalHeight(Values[(yNext- KeyMin.Y) * XNum + (i - KeyMin.X)]));
			FVector LandscapePositionYP = FVector(i , yPrev , LandscapeDataAccess::GetLocalHeight(Values[(yPrev- KeyMin.Y) * XNum + (i - KeyMin.X)]));
			FVector DX = (LandscapePositionXN - LandscapePositionXP).GetSafeNormal();
			FVector DY = (LandscapePositionYN - LandscapePositionYP).GetSafeNormal();
			FVector Normal = FVector::CrossProduct(DX, DY).GetSafeNormal();
			HeightNormals.Add(FLinearColor(Normal.X, Normal.Y, Normal.Z, LandscapePosition.Z));
			
		}
	}
	return  HeightNormals;
	
}

TArray<FLinearColor> ULandscapeExtra::CreateLandscapeTextureData(FVector& MapMin, FVector& MapMax, FVector Center, FVector Extent, int32 TextureSize, int32 ExtentPlus)
{
	
	TArray<FLinearColor> OutHeightNormals;
	const FVector Min = Center - Extent;
	const FVector Max = Center + Extent;
	if (Extent.Length() < .001)
	{
		return OutHeightNormals;
	}
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
		return OutHeightNormals;
	}
	const ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
	const FVector MaxLocalPoint = Landscape->GetTransform().InverseTransformPosition(Max);
	const FVector MinLocalPoint = Landscape->GetTransform().InverseTransformPosition(Min);
	const FIntPoint KeyMax(FMath::CeilToInt(MaxLocalPoint.X) + ExtentPlus, FMath::CeilToInt(MaxLocalPoint.Y) + ExtentPlus);
	const FIntPoint KeyMin(FMath::Floor(MinLocalPoint.X) - ExtentPlus, FMath::Floor(MinLocalPoint.Y) - ExtentPlus);
	int32 XMin = FMath::Floor(MinLocalPoint.X) - ExtentPlus;
	int32 YMin = FMath::Floor(MinLocalPoint.Y) - ExtentPlus;
	int32 XMax = FMath::CeilToInt(MaxLocalPoint.X) + ExtentPlus;
	int32 YMax = FMath::CeilToInt(MaxLocalPoint.Y) + ExtentPlus;
	int32 ComponentSizeQuads = LandscapeInfo->ComponentSizeQuads;
	int32 XNum = KeyMax.X - KeyMin.X;
	int32 YNum = KeyMax.Y - KeyMin.Y;
	int32 NumVertices = XNum * YNum;
	
	TArray<uint16> Values;
	Values.AddZeroed(NumVertices );
	
	FScopedSetLandscapeEditingLayer Scope(Landscape, Landscape->GetLayer(0)->Guid, [&] { /*Landscape->RequestLayersContentUpdate(ELandscapeLayerUpdateMode::Update_All); */});

	int32 X1 = KeyMin.X;
	int32 Y1 = KeyMin.Y;
	int32 X2 = KeyMax.X - 1;
	int32 Y2 = KeyMax.Y - 1;
	FLandscapeEditDataInterface LandscapeEdit(Landscape->GetLandscapeInfo());
	LandscapeEdit.SetShouldDirtyPackage(false);
	LandscapeEdit.GetHeightDataFast(X1, Y1, X2, Y2, (uint16*)Values.GetData(), 0);


	int32 NumPixel = 0;
	for (int i = 4; i < 12; i ++)
	{
		if (FMath::Pow(2.0, i) >= XNum)
		{
			NumPixel = FMath::Pow(2.0, i);
			break;
		}
	}
	if (NumPixel == 0) return OutHeightNormals;
	
	int32 NumPixelX = NumPixel;
	NumPixel = FMath::Pow(NumPixel, 2.0);
	
	TMap<FIntPoint, FLinearColor> MapHeightNormals;
	TArray<FLinearColor> HeightNormals;
	HeightNormals.AddZeroed(NumPixel);
	for (int32 j = YMin; j < YMax; j++)
	{
		for (int32 i = XMin; i < XMax; i++)
		{
			FVector LandscapePosition = FVector(i , j , LandscapeDataAccess::GetLocalHeight(Values[(j- YMin) * XNum + (i - XMin)]));
			LandscapePosition = Landscape->GetTransform().TransformPosition(LandscapePosition);
			int xPrev = (i == X1) ? XMin : i - 1;
			int xNext = (i == X2) ? i : i + 1;
			int yPrev = (j == Y1) ? YMin : j - 1;
			int yNext = (j == Y2) ? j : j + 1;

			FVector LandscapePositionXN = FVector(xNext , j , LandscapeDataAccess::GetLocalHeight(Values[(j- YMin) * XNum + (xNext - XMin)]));
			FVector LandscapePositionXP = FVector(xPrev , j , LandscapeDataAccess::GetLocalHeight(Values[(j- YMin) * XNum + (xPrev - XMin)]));
			FVector LandscapePositionYN = FVector(i , yNext , LandscapeDataAccess::GetLocalHeight(Values[(yNext- YMin) * XNum + (i - XMin)]));
			FVector LandscapePositionYP = FVector(i , yPrev , LandscapeDataAccess::GetLocalHeight(Values[(yPrev- YMin) * XNum + (i - XMin)]));
			FVector DX = (LandscapePositionXN - LandscapePositionXP).GetSafeNormal();
			FVector DY = (LandscapePositionYN - LandscapePositionYP).GetSafeNormal();
			FVector Normal = FVector::CrossProduct(DX, DY).GetSafeNormal();
			HeightNormals[i - XMin + (j - YMin) * NumPixelX] = (FLinearColor(Normal.X, Normal.Y, Normal.Z, LandscapePosition.Z));
			MapHeightNormals.Add(FIntPoint(i, j), FLinearColor(Normal.X, Normal.Y, Normal.Z, LandscapePosition.Z));
			
		}
	}
	
	//
	// OutHeightNormals.Reserve(TextureSize * TextureSize);
	// for (int32 Y = 0; Y < TextureSize; Y++)
	// {
	// 	for (int32 X = 0; X < TextureSize; X++)
	// 	{
	// 		FVector CurrentPostion = Min + FVector(X / (TextureSize - 1.0), Y / (TextureSize - 1.0), 0) * (Max - Min);
	// 		
	// 		FVector LocalPoint = Landscape->GetTransform().InverseTransformPosition(CurrentPostion);
	// 		int32 CellMaxX = FMath::CeilToInt(LocalPoint.X);
	// 		int32 CellMaxY = FMath::CeilToInt(LocalPoint.Y);
	// 		int32 CellMinX = FMath::Floor(LocalPoint.X);
	// 		int32 CellMinY = FMath::Floor(LocalPoint.Y);
	// 		
	// 		float XFraction = FMath::Fractional(CurrentPostion.X);
	// 		float YFraction = FMath::Fractional(CurrentPostion.Y);
	//
	// 		FLinearColor Color00 = *MapHeightNormals.Find(FIntPoint(CellMinX, CellMinY));
	// 		FLinearColor Color10 = *MapHeightNormals.Find(FIntPoint(CellMaxX, CellMinY));
	// 		FLinearColor Color01 = *MapHeightNormals.Find(FIntPoint(CellMinX, CellMaxY));
	// 		FLinearColor Color11 = *MapHeightNormals.Find(FIntPoint(CellMaxX, CellMaxY));
	// 		
	// 		FVector WorldPos00 = Landscape->GetTransform().TransformPosition(FVector(CellMinX, CellMinY, Color00.A));
	// 		FVector WorldPos10 = Landscape->GetTransform().TransformPosition(FVector(CellMaxX, CellMinY, Color10.A));
	// 		FVector WorldPos01 = Landscape->GetTransform().TransformPosition(FVector(CellMinX, CellMaxY, Color01.A));
	// 		FVector WorldPos11 = Landscape->GetTransform().TransformPosition(FVector(CellMaxX, CellMaxY, Color11.A));
	//
	// 		FVector Normal00 = FVector(Color00.R, Color00.G, Color00.B);
	// 		FVector Normal10 = FVector(Color10.R, Color10.G, Color10.B);
	// 		FVector Normal01 = FVector(Color01.R, Color01.G, Color01.B);
	// 		FVector Normal11 = FVector(Color11.R, Color11.G, Color11.B);
	// 		
	// 		const FVector LerpPositionY0 = FMath::Lerp(WorldPos00, WorldPos10, XFraction);
	// 		const FVector LerpPositionY1 = FMath::Lerp(WorldPos01, WorldPos11, XFraction);
	// 		const FVector Position = FMath::Lerp(LerpPositionY0, LerpPositionY1, YFraction);
	// 		const FVector LerpNormalY0 = FMath::Lerp(Normal00, Normal10, XFraction).GetSafeNormal();
	// 		const FVector LerpNormalY1 = FMath::Lerp(Normal01, Normal11, XFraction).GetSafeNormal();
	// 		const FVector Normal = Normal11;
	// 		
	// 		OutHeightNormals.Add(FLinearColor(Normal.X, Normal.Y, Normal.Z, Position.Z));
	// 	}
	// }
	
	MapMin = Landscape->GetTransform().TransformPosition(FVector(XMin - .5, YMin - .5, 0));
	MapMax = Landscape->GetTransform().TransformPosition(FVector(NumPixelX + XMin -.5, NumPixelX + YMin - .5, 0));
	return  HeightNormals;
}

TArray<FLinearColor> ULandscapeExtra::CreateLandscapeMeshTextureData(FVector& MapMin, FVector& MapMax, FVector Center,
	FVector Extent, int32 TextureSize, int32 ExtentPlus)
{
	TArray<FLinearColor> OutHeightNormals;
	UDynamicMesh* PlaneMesh = NewObject<UDynamicMesh>();
	FVector Min = Center - Extent;
	FVector Max = Center + Extent;
	if (Extent.Length() < .001)
	{
		return OutHeightNormals;
	}
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
		return OutHeightNormals;
	}
	const ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
	const FVector MaxLocalPoint = Landscape->GetTransform().InverseTransformPosition(Max);
	const FVector MinLocalPoint = Landscape->GetTransform().InverseTransformPosition(Min);
	const FIntPoint KeyMax(FMath::CeilToInt(MaxLocalPoint.X) + ExtentPlus, FMath::CeilToInt(MaxLocalPoint.Y) + ExtentPlus);
	const FIntPoint KeyMin(FMath::Floor(MinLocalPoint.X) - ExtentPlus, FMath::Floor(MinLocalPoint.Y) - ExtentPlus);
	int32 ComponentSizeQuads = LandscapeInfo->ComponentSizeQuads;
	int32 XNum = KeyMax.X - KeyMin.X;
	int32 YNum = KeyMax.Y - KeyMin.Y;
	int32 NumVertices = XNum * YNum;
	
	
	FRectangleMeshGenerator RectGenerator;
	RectGenerator.Origin = FVector3d(0, 0, 0);
	RectGenerator.Normal = FVector3f::UnitZ();
	RectGenerator.Width = 100;
	RectGenerator.Height = 100;
	RectGenerator.WidthVertexCount = FMath::Max(0, XNum);
	RectGenerator.HeightVertexCount = FMath::Max(0, YNum);
	RectGenerator.bSinglePolyGroup = true;
	RectGenerator.Generate();
	
	FGeometryScriptPrimitiveOptions PrimitiveOptions;
	AppendPrimitive(PlaneMesh, &RectGenerator, FTransform::Identity, PrimitiveOptions);
	
	TArray<uint16> Values;
	Values.AddZeroed(NumVertices );
	
	FScopedSetLandscapeEditingLayer Scope(Landscape, Landscape->GetLayer(0)->Guid, [&] { /*Landscape->RequestLayersContentUpdate(ELandscapeLayerUpdateMode::Update_All); */});

	int32 X1 = KeyMin.X;
	int32 Y1 = KeyMin.Y;
	int32 X2 = KeyMax.X - 1;
	int32 Y2 = KeyMax.Y - 1;
	FIntPoint MapKey = FIntPoint (X1 / ComponentSizeQuads, Y1 / ComponentSizeQuads );
	ULandscapeComponent* Comp = LandscapeInfo->XYtoComponentMap.FindRef(MapKey);
	FLandscapeEditDataInterface LandscapeEdit(Landscape->GetLandscapeInfo());
	LandscapeEdit.SetShouldDirtyPackage(false);
	LandscapeEdit.GetHeightDataFast(X1, Y1, X2, Y2, (uint16*)Values.GetData(), 0);

	//
	int32 FindIndex = 0;
	TArray<FVector> Vertices;
	Vertices.Reserve(NumVertices);
	for (int32 j = KeyMin.Y; j < KeyMax.Y; j++)
	{
		for (int32 i = KeyMin.X; i < KeyMax.X; i++)
		{
			FVector LandscapePosition = FVector(i , j , LandscapeDataAccess::GetLocalHeight(Values[(j- KeyMin.Y) * XNum + (i - KeyMin.X)]));
			Vertices.Add(LandscapePosition);
		}
	}

	PlaneMesh->EditMesh([&](FDynamicMesh3& EditMesh)
	{
		for (int32 i = 0; i < NumVertices; i++)
		{
			FVector NewPosition = Vertices[i];
			if (EditMesh.IsVertex(i))
			{
				EditMesh.SetVertex(i, (FVector3d)NewPosition);
			}
		}
		MeshTransforms::ApplyTransform(EditMesh, (FTransformSRT3d)(Landscape->GetTransform()), true);
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	return OutHeightNormals;
	// GetNearestLocationNormal

}
