#pragma once

#include "Modules/ModuleManager.h"

class FAIToolModuleModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
