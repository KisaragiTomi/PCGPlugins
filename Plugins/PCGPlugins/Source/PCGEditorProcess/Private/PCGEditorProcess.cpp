// Copyright Epic Games, Inc. All Rights Reserved.

#include "PCGEditorProcess.h"
#include "CSInstanceBrushEdMode.h"
#include "ComputeShaderMeshGenerator.h"
#include "ComputeShaderShallowWater.h"
#include "CSShallowWaterProcess.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "EditorModeRegistry.h"
#include "Textures/SlateIcon.h"

#define LOCTEXT_NAMESPACE "FPCGEditorProcessModule"

void FPCGEditorProcessModule::StartupModule()
{
	ACSShallowWaterCapture::OnBakeResultMeshDelegate.BindStatic(&UCSShallowWaterProcess::SaveSWData);
	FEditorModeRegistry::Get().RegisterMode<FCSInstanceBrushEdMode>(
		FCSInstanceBrushEdMode::EM_CSInstanceBrush,
		LOCTEXT("CSInstanceBrushMode", "CS Instance Brush"),
		FSlateIcon(),
		false);
	AComputeShaderMeshGenerator::OnInstanceBrushEditorRequest.AddRaw(this, &FPCGEditorProcessModule::StartInstanceBrush);
}

void FPCGEditorProcessModule::ShutdownModule()
{
	AComputeShaderMeshGenerator::OnInstanceBrushEditorRequest.RemoveAll(this);
	FEditorModeRegistry::Get().UnregisterMode(FCSInstanceBrushEdMode::EM_CSInstanceBrush);
	ACSShallowWaterCapture::OnBakeResultMeshDelegate.Unbind();
}

void FPCGEditorProcessModule::StartInstanceBrush(AComputeShaderMeshGenerator* TargetActor)
{
	if (!TargetActor || !GEditor)
	{
		return;
	}

	FEditorModeTools& ModeTools = GLevelEditorModeTools();
	ModeTools.ActivateMode(FCSInstanceBrushEdMode::EM_CSInstanceBrush);
	if (FCSInstanceBrushEdMode* BrushMode = ModeTools.GetActiveModeTyped<FCSInstanceBrushEdMode>(FCSInstanceBrushEdMode::EM_CSInstanceBrush))
	{
		BrushMode->SetTargetActor(TargetActor);
	}
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FPCGEditorProcessModule, PCGEditorProcess)
