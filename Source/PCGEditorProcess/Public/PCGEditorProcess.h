// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "Modules/ModuleManager.h"
#include "ComputeShaderDebugParams.h"

class AComputeShaderMeshGenerator;
class ACSPointBrushActor;
class AGPUSkeletalTree;
class FViewEditCategoryViewportOverlay;

class FPCGEditorProcessModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void InitializeEditorUI();
	void StartInstanceBrush(AComputeShaderMeshGenerator* TargetActor);
	void StartPointBrush(ACSPointBrushActor* TargetActor);
	void GenerateGPUSkeletalTree(AGPUSkeletalTree* TargetActor);

	FDelegateHandle PostEngineInitHandle;
	TUniquePtr<FViewEditCategoryViewportOverlay> ViewEditCategoryViewportOverlay;
	bool bEditorModeRegistered = false;
	bool bPointBrushModeRegistered = false;
};
