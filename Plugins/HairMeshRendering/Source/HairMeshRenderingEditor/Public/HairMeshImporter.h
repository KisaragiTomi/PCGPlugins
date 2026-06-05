#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "HairMeshImporter.generated.h"

class UHairMeshAsset;

/**
 * Import hair mesh data from OBJ files.
 *
 * Expected OBJ convention:
 *   - Multiple object groups named with layer suffix: "scalp", "layer1", "layer2", ...
 *   - OR a single mesh where vertex groups/colors encode layer assignment
 *   - The scalp (root layer) mesh defines the bundle topology
 *   - Each subsequent layer has matching vertex count per face
 *
 * Simple mode (single OBJ with naming convention):
 *   - Faces with material "scalp" or object "scalp" → root layer
 *   - Faces with material "layerN" or object "layerN" → extrusion layer N
 *   - Vertex UVs are used directly as styling UV coordinates
 */
UCLASS()
class HAIRMESHRENDERINGEDITOR_API UHairMeshImporter : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Import a hair mesh from an OBJ file.
	 * @param FilePath     Absolute path to the .obj file
	 * @param Outer        Outer object for the created asset
	 * @param AssetName    Desired asset name
	 * @return Created UHairMeshAsset, or nullptr on failure
	 */
	UFUNCTION(BlueprintCallable, Category="HairMesh|Import")
	static UHairMeshAsset* ImportFromOBJ(const FString& FilePath, UObject* Outer, const FString& AssetName);

	/**
	 * Import from a minimal format: flat list of layers with matching vertex topology.
	 * @param FilePath  Path to text file with format: NumLayers NumVertsPerLayer, then XYZ per vertex
	 */
	UFUNCTION(BlueprintCallable, Category="HairMesh|Import")
	static UHairMeshAsset* ImportFromSimpleFormat(const FString& FilePath, UObject* Outer, const FString& AssetName);

private:
	struct FOBJParseResult
	{
		TArray<FVector> Positions;
		TArray<FVector2D> UVs;
		TArray<TArray<int32>> FaceVertexIndices;    // per-face vertex index list (0-based)
		TArray<TArray<int32>> FaceUVIndices;         // per-face UV index list
		TMap<FString, TArray<int32>> GroupFaces;      // group name → face indices
	};

	static bool ParseOBJ(const FString& FilePath, FOBJParseResult& OutResult);
	static UHairMeshAsset* BuildAssetFromParsedOBJ(const FOBJParseResult& Result, UObject* Outer, const FString& AssetName);
};
