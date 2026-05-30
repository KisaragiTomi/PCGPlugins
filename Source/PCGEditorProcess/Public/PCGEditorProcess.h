// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "Modules/ModuleManager.h"

class AComputeShaderMeshGenerator;
class FVineContainerViewportOverlay;

class FPCGEditorProcessModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void InitializeEditorUI();
	void StartInstanceBrush(AComputeShaderMeshGenerator* TargetActor);

	FDelegateHandle PostEngineInitHandle;
	TUniquePtr<FVineContainerViewportOverlay> VineContainerViewportOverlay;
	bool bEditorModeRegistered = false;
};
