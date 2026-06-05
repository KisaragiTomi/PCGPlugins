#pragma once

#include "Modules/ModuleManager.h"

class FEnhancedHairCardsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	FString ShaderDirectory;
};
