#include "EnhancedHairCardsModule.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#define LOCTEXT_NAMESPACE "FEnhancedHairCardsModule"

void FEnhancedHairCardsModule::StartupModule()
{
	FString PluginShaderDir = FPaths::Combine(
		IPluginManager::Get().FindPlugin(TEXT("EnhancedHairCards"))->GetBaseDir(),
		TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/Plugin/EnhancedHairCards"), PluginShaderDir);
}

void FEnhancedHairCardsModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FEnhancedHairCardsModule, EnhancedHairCards)
