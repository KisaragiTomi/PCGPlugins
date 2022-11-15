// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"
#if WITH_OPENCV
#include "opencv2/core.hpp"
#include "opencv2/imgcodecs.hpp"
#endif
//#include "opencv2/core.hpp"

class FTAToolsPluginModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
private:
	void* OpenCV_World_Handler;
	void* OpenCV_FFmpeg_Handler;
};
