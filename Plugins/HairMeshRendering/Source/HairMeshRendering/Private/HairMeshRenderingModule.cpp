#include "HairMeshRenderingModule.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#define LOCTEXT_NAMESPACE "FHairMeshRenderingModule"

void FHairMeshRenderingModule::StartupModule()
{
	const FString ShaderDir = FPaths::Combine(
		IPluginManager::Get().FindPlugin(TEXT("HairMeshRendering"))->GetBaseDir(),
		TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/Plugin/HairMeshRendering"), ShaderDir);
}

void FHairMeshRenderingModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FHairMeshRenderingModule, HairMeshRendering)
