// Copyright Epic Games, Inc. All Rights Reserved.

#include "CSEditorProcess.h"

#define LOCTEXT_NAMESPACE "FCSEditorProcessModule"

void FCSEditorProcessModule::StartupModule()
{
	// 着色器目录已在 ComputeShaderGenerator 模块中注册，此处无需重复注册
}

void FCSEditorProcessModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FCSEditorProcessModule, CSEditorProcess)