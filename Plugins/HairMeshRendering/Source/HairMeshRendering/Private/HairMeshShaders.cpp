#include "HairMeshShaders.h"

IMPLEMENT_GLOBAL_SHADER(
	FHairMeshGenerationCS,
	"/Plugin/HairMeshRendering/Private/HairMeshGeneration.usf",
	"MainCS",
	SF_Compute);

IMPLEMENT_GLOBAL_SHADER(
	FHairMeshRasterVS,
	"/Plugin/HairMeshRendering/Private/HairMeshRaster.usf",
	"MainVS",
	SF_Vertex);

IMPLEMENT_GLOBAL_SHADER(
	FHairMeshRasterPS,
	"/Plugin/HairMeshRendering/Private/HairMeshRaster.usf",
	"MainPS",
	SF_Pixel);
