// Copyright Epic Games, Inc. All Rights Reserved.

#include "PCGEditorProcess.h"
#include "CSInstanceBrushEdMode.h"
#include "ComputeShaderMeshGenerator.h"
#include "ComputeShaderShallowWater.h"
#include "CSShallowWaterProcess.h"
#include "VineContainerViewportOverlay.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "EditorModeRegistry.h"
#include "Textures/SlateIcon.h"
#include "Misc/CoreDelegates.h"
#include "Misc/App.h"
#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "FPCGEditorProcessModule"

void FPCGEditorProcessModule::StartupModule()
{
	// Bind delegates that don't require Slate/LevelEditor
	ACSShallowWaterCapture::OnBakeResultMeshDelegate.BindStatic(&UCSShallowWaterProcess::SaveSWData);
	AComputeShaderMeshGenerator::OnInstanceBrushEditorRequest.AddRaw(this, &FPCGEditorProcessModule::StartInstanceBrush);

	// Defer editor UI initialization until the engine is fully loaded.
	// At PostConfigInit, FCoreStyle and LevelEditor are not yet available.
	const ELoadingPhase::Type CurrentPhase = IPluginManager::Get().GetLastCompletedLoadingPhase();
	if (CurrentPhase == ELoadingPhase::None || CurrentPhase < ELoadingPhase::PostEngineInit)
	{
		PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddRaw(this, &FPCGEditorProcessModule::InitializeEditorUI);
	}
	else
	{
		InitializeEditorUI();
	}
}

void FPCGEditorProcessModule::ShutdownModule()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}

	VineContainerViewportOverlay.Reset();
	AComputeShaderMeshGenerator::OnInstanceBrushEditorRequest.RemoveAll(this);
	if (bEditorModeRegistered && !IsEngineExitRequested() && GEditor)
	{
		FEditorModeRegistry::Get().UnregisterMode(FCSInstanceBrushEdMode::EM_CSInstanceBrush);
	}
	bEditorModeRegistered = false;
	ACSShallowWaterCapture::OnBakeResultMeshDelegate.Unbind();
}

void FPCGEditorProcessModule::InitializeEditorUI()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}

	if (IsEngineExitRequested() || IsRunningCommandlet() || !FApp::CanEverRender() || !GEditor)
	{
		return;
	}

	const bool bModeAlreadyRegistered = FEditorModeRegistry::Get().GetFactoryMap().Contains(FCSInstanceBrushEdMode::EM_CSInstanceBrush);
	if (!bModeAlreadyRegistered)
	{
		FEditorModeRegistry::Get().RegisterMode<FCSInstanceBrushEdMode>(
			FCSInstanceBrushEdMode::EM_CSInstanceBrush,
			LOCTEXT("CSInstanceBrushMode", "CS Instance Brush"),
			FSlateIcon(),
			false);
		bEditorModeRegistered = true;
	}

	VineContainerViewportOverlay = MakeUnique<FVineContainerViewportOverlay>();
	VineContainerViewportOverlay->Start();
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
