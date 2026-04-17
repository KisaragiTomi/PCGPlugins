// Copyright Epic Games, Inc. All Rights Reserved.

#include "PCGEditorProcess.h"
#include "ComputeShaderShallowWater.h"
#include "CSShallowWaterProcess.h"

#define LOCTEXT_NAMESPACE "FPCGEditorProcessModule"

void FPCGEditorProcessModule::StartupModule()
{
	ACSShallowWaterCapture::OnBakeResultMeshDelegate.BindStatic(&UCSShallowWaterProcess::SaveSWData);
}

void FPCGEditorProcessModule::ShutdownModule()
{
	ACSShallowWaterCapture::OnBakeResultMeshDelegate.Unbind();
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FPCGEditorProcessModule, PCGEditorProcess)