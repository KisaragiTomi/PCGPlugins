#pragma once

#include "Modules/ModuleManager.h"

class FHairMeshRenderingModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
