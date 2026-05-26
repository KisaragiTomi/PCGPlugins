// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class AComputeShaderMeshGenerator;

class FPCGEditorProcessModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void StartInstanceBrush(AComputeShaderMeshGenerator* TargetActor);
};
