#include "HairMeshImporter.h"
#include "HairMeshAsset.h"
#include "Misc/FileHelper.h"

// ============================================================================
// OBJ Parser
// ============================================================================

bool UHairMeshImporter::ParseOBJ(const FString& FilePath, FOBJParseResult& OutResult)
{
	FString FileContent;
	if (!FFileHelper::LoadFileToString(FileContent, *FilePath))
	{
		UE_LOG(LogTemp, Error, TEXT("HairMeshImporter: Failed to read file: %s"), *FilePath);
		return false;
	}

	TArray<FString> Lines;
	FileContent.ParseIntoArrayLines(Lines);

	FString CurrentGroup = TEXT("default");

	for (const FString& Line : Lines)
	{
		FString Trimmed = Line.TrimStartAndEnd();
		if (Trimmed.IsEmpty() || Trimmed.StartsWith(TEXT("#")))
			continue;

		TArray<FString> Tokens;
		Trimmed.ParseIntoArrayWS(Tokens);
		if (Tokens.Num() == 0) continue;

		if (Tokens[0] == TEXT("v") && Tokens.Num() >= 4)
		{
			FVector Pos;
			Pos.X = FCString::Atof(*Tokens[1]);
			Pos.Y = FCString::Atof(*Tokens[2]);
			Pos.Z = FCString::Atof(*Tokens[3]);
			OutResult.Positions.Add(Pos);
		}
		else if (Tokens[0] == TEXT("vt") && Tokens.Num() >= 3)
		{
			FVector2D UV;
			UV.X = FCString::Atof(*Tokens[1]);
			UV.Y = FCString::Atof(*Tokens[2]);
			OutResult.UVs.Add(UV);
		}
		else if (Tokens[0] == TEXT("g") || Tokens[0] == TEXT("o"))
		{
			CurrentGroup = (Tokens.Num() >= 2) ? Tokens[1] : TEXT("default");
		}
		else if (Tokens[0] == TEXT("f") && Tokens.Num() >= 4)
		{
			TArray<int32> VertIndices;
			TArray<int32> UVIndices;

			for (int32 i = 1; i < Tokens.Num(); ++i)
			{
				TArray<FString> Parts;
				Tokens[i].ParseIntoArray(Parts, TEXT("/"), false);

				int32 VI = FCString::Atoi(*Parts[0]) - 1; // OBJ is 1-based
				VertIndices.Add(VI);

				if (Parts.Num() >= 2 && !Parts[1].IsEmpty())
				{
					int32 TI = FCString::Atoi(*Parts[1]) - 1;
					UVIndices.Add(TI);
				}
			}

			int32 FaceIdx = OutResult.FaceVertexIndices.Num();
			OutResult.FaceVertexIndices.Add(VertIndices);
			OutResult.FaceUVIndices.Add(UVIndices);
			OutResult.GroupFaces.FindOrAdd(CurrentGroup).Add(FaceIdx);
		}
	}

	UE_LOG(LogTemp, Log,
		TEXT("HairMeshImporter: Parsed %d vertices, %d UVs, %d faces, %d groups from %s"),
		OutResult.Positions.Num(), OutResult.UVs.Num(),
		OutResult.FaceVertexIndices.Num(),
		OutResult.GroupFaces.Num(), *FilePath);

	return OutResult.Positions.Num() > 0 && OutResult.FaceVertexIndices.Num() > 0;
}

// ============================================================================
// Build Asset
// ============================================================================

UHairMeshAsset* UHairMeshImporter::BuildAssetFromParsedOBJ(
	const FOBJParseResult& Result, UObject* Outer, const FString& AssetName)
{
	UHairMeshAsset* Asset = NewObject<UHairMeshAsset>(Outer, *AssetName, RF_Public | RF_Standalone);
	if (!Asset) return nullptr;

	// Identify layer groups: "scalp" or "root" or "layer0" → root, "layer1", "layer2", etc.
	TArray<FString> SortedGroupNames;
	Result.GroupFaces.GetKeys(SortedGroupNames);

	FString RootGroupName;
	TArray<FString> LayerGroupNames;

	for (const FString& Name : SortedGroupNames)
	{
		FString Lower = Name.ToLower();
		if (Lower.Contains(TEXT("scalp")) || Lower.Contains(TEXT("root")) || Lower == TEXT("layer0"))
		{
			RootGroupName = Name;
		}
		else if (Lower.StartsWith(TEXT("layer")))
		{
			LayerGroupNames.Add(Name);
		}
	}

	// If no explicit root found, use first group or "default"
	if (RootGroupName.IsEmpty())
	{
		RootGroupName = SortedGroupNames.Num() > 0 ? SortedGroupNames[0] : TEXT("default");
	}

	// Sort layer groups by number
	LayerGroupNames.Sort([](const FString& A, const FString& B)
	{
		auto ExtractNum = [](const FString& S) -> int32
		{
			FString NumStr;
			for (int32 i = S.Len() - 1; i >= 0; --i)
			{
				if (FChar::IsDigit(S[i])) NumStr = FString(1, &S[i]) + NumStr;
				else break;
			}
			return NumStr.IsEmpty() ? 0 : FCString::Atoi(*NumStr);
		};
		return ExtractNum(A) < ExtractNum(B);
	});

	const int32 NumExtrusionLayers = LayerGroupNames.Num();
	Asset->NumExtrusionLayers = FMath::Max(NumExtrusionLayers, 1);

	// Copy all vertices
	Asset->Vertices.SetNum(Result.Positions.Num());
	for (int32 i = 0; i < Result.Positions.Num(); ++i)
	{
		Asset->Vertices[i].Position = Result.Positions[i];

		// Use OBJ UV as styling UV; W defaults to 0 for root, computed per-layer later
		if (i < Result.UVs.Num())
		{
			Asset->Vertices[i].UVW = FVector(Result.UVs[i].X, Result.UVs[i].Y, 0.0);
		}
	}

	// Build bundles from root faces
	const TArray<int32>* RootFaceIndices = Result.GroupFaces.Find(RootGroupName);
	if (!RootFaceIndices || RootFaceIndices->Num() == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("HairMeshImporter: No root faces found in group '%s'"), *RootGroupName);
		return Asset;
	}

	Asset->Bundles.SetNum(RootFaceIndices->Num());

	for (int32 BundleIdx = 0; BundleIdx < RootFaceIndices->Num(); ++BundleIdx)
	{
		int32 FaceIdx = (*RootFaceIndices)[BundleIdx];
		const TArray<int32>& FaceVerts = Result.FaceVertexIndices[FaceIdx];

		FHairMeshBundle& Bundle = Asset->Bundles[BundleIdx];
		Bundle.RootFace.VertexIndices = FaceVerts;
		Bundle.RootFace.bIsTriangle = (FaceVerts.Num() == 3);
		Bundle.NumLayers = NumExtrusionLayers;
		Bundle.MaxStrandsPerBundle = Asset->DefaultMaxStrands;

		// Root layer = the face itself
		Bundle.LayerVertexIndices.Append(FaceVerts);

		// For each extrusion layer, find matching faces
		for (int32 LayerIdx = 0; LayerIdx < NumExtrusionLayers; ++LayerIdx)
		{
			const TArray<int32>* LayerFaces = Result.GroupFaces.Find(LayerGroupNames[LayerIdx]);
			if (LayerFaces && BundleIdx < LayerFaces->Num())
			{
				int32 LayerFaceIdx = (*LayerFaces)[BundleIdx];
				const TArray<int32>& LayerFaceVerts = Result.FaceVertexIndices[LayerFaceIdx];
				Bundle.LayerVertexIndices.Append(LayerFaceVerts);

				// Set W coordinate for layer vertices (linear from 0 to 1)
				float WVal = (float)(LayerIdx + 1) / (float)NumExtrusionLayers;
				for (int32 VI : LayerFaceVerts)
				{
					if (VI >= 0 && VI < Asset->Vertices.Num())
					{
						Asset->Vertices[VI].UVW.Z = WVal;
					}
				}
			}
			else
			{
				// Pad with root face if no matching layer face
				Bundle.LayerVertexIndices.Append(FaceVerts);
			}
		}
	}

	UE_LOG(LogTemp, Log,
		TEXT("HairMeshImporter: Built HairMeshAsset '%s' with %d vertices, %d bundles, %d extrusion layers"),
		*AssetName, Asset->Vertices.Num(), Asset->Bundles.Num(), NumExtrusionLayers);

	return Asset;
}

// ============================================================================
// Public API
// ============================================================================

UHairMeshAsset* UHairMeshImporter::ImportFromOBJ(
	const FString& FilePath, UObject* Outer, const FString& AssetName)
{
	FOBJParseResult ParseResult;
	if (!ParseOBJ(FilePath, ParseResult))
	{
		return nullptr;
	}
	return BuildAssetFromParsedOBJ(ParseResult, Outer, AssetName);
}

UHairMeshAsset* UHairMeshImporter::ImportFromSimpleFormat(
	const FString& FilePath, UObject* Outer, const FString& AssetName)
{
	FString FileContent;
	if (!FFileHelper::LoadFileToString(FileContent, *FilePath))
	{
		UE_LOG(LogTemp, Error, TEXT("HairMeshImporter: Failed to read file: %s"), *FilePath);
		return nullptr;
	}

	TArray<FString> Lines;
	FileContent.ParseIntoArrayLines(Lines);

	if (Lines.Num() < 2)
	{
		UE_LOG(LogTemp, Error, TEXT("HairMeshImporter: Simple format requires at least 2 lines"));
		return nullptr;
	}

	// Header: NumLayers NumVertsPerFace NumFaces
	TArray<FString> Header;
	Lines[0].ParseIntoArrayWS(Header);
	if (Header.Num() < 3) return nullptr;

	int32 NumLayers = FCString::Atoi(*Header[0]);
	int32 VertsPerFace = FCString::Atoi(*Header[1]);
	int32 NumFaces = FCString::Atoi(*Header[2]);

	int32 TotalVertsExpected = (NumLayers + 1) * NumFaces * VertsPerFace;

	UHairMeshAsset* Asset = NewObject<UHairMeshAsset>(Outer, *AssetName, RF_Public | RF_Standalone);
	Asset->NumExtrusionLayers = NumLayers;
	Asset->Vertices.SetNum(TotalVertsExpected);

	int32 LineIdx = 1;
	int32 VertIdx = 0;

	for (int32 Layer = 0; Layer <= NumLayers; ++Layer)
	{
		float WVal = (float)Layer / (float)FMath::Max(NumLayers, 1);

		for (int32 Face = 0; Face < NumFaces; ++Face)
		{
			for (int32 V = 0; V < VertsPerFace; ++V)
			{
				if (LineIdx < Lines.Num() && VertIdx < TotalVertsExpected)
				{
					TArray<FString> Vals;
					Lines[LineIdx].ParseIntoArrayWS(Vals);

					if (Vals.Num() >= 3)
					{
						Asset->Vertices[VertIdx].Position = FVector(
							FCString::Atof(*Vals[0]),
							FCString::Atof(*Vals[1]),
							FCString::Atof(*Vals[2]));
					}

					if (Vals.Num() >= 5)
					{
						Asset->Vertices[VertIdx].UVW = FVector(
							FCString::Atof(*Vals[3]),
							FCString::Atof(*Vals[4]),
							WVal);
					}
					else
					{
						Asset->Vertices[VertIdx].UVW.Z = WVal;
					}

					++VertIdx;
					++LineIdx;
				}
			}
		}
	}

	// Build bundles
	Asset->Bundles.SetNum(NumFaces);
	for (int32 Face = 0; Face < NumFaces; ++Face)
	{
		FHairMeshBundle& Bundle = Asset->Bundles[Face];
		Bundle.NumLayers = NumLayers;
		Bundle.MaxStrandsPerBundle = Asset->DefaultMaxStrands;
		Bundle.RootFace.bIsTriangle = (VertsPerFace == 3);

		for (int32 V = 0; V < VertsPerFace; ++V)
		{
			int32 VI = Face * VertsPerFace + V;
			Bundle.RootFace.VertexIndices.Add(VI);
		}

		for (int32 Layer = 0; Layer <= NumLayers; ++Layer)
		{
			for (int32 V = 0; V < VertsPerFace; ++V)
			{
				int32 VI = Layer * NumFaces * VertsPerFace + Face * VertsPerFace + V;
				Bundle.LayerVertexIndices.Add(VI);
			}
		}
	}

	return Asset;
}
