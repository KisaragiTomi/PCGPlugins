// Copyright Epic Games, Inc. All Rights Reserved.

#include "ComputeShaderGenerator.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "FComputeShaderGeneratorModule"

void FComputeShaderGeneratorModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PCGPlugins"));
	if (Plugin.IsValid())
	{
		const FString ShaderDirectory = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders/Private"));
		AddShaderSourceDirectoryMapping(TEXT("/Plugin/PCGPlugins/Shaders/Private"), ShaderDirectory);
	}
}

void FComputeShaderGeneratorModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FComputeShaderGeneratorModule, ComputeShaderGenerator)
