#pragma once

#include "Modules/ModuleManager.h"

class FHairMeshRenderingEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
