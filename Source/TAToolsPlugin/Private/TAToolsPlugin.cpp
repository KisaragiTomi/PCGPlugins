// Copyright Epic Games, Inc. All Rights Reserved.

#include "TAToolsPlugin.h"
#include "Interfaces/IPluginManager.h"
#include "Modules/ModuleManager.h"
//#include "opencv2/core.hpp"

#define LOCTEXT_NAMESPACE "FTAToolsPluginModule"
#define OpenCV_Version "451"

void FTAToolsPluginModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
	const FString PluginDir = IPluginManager::Get().FindPlugin(TEXT("TAToolsPlugin"))->GetBaseDir();
	FString LibraryPath;

	//#if PLATFORM_WINDOWS
#if WITH_OPENCV
	LibraryPath = FPaths::Combine(*PluginDir, TEXT("ThirdParty/OpenCV/Libs/"));
	UE_LOG(LogTemp, Warning, TEXT("opencv world LibraryPath == %s"), *(LibraryPath + TEXT("opencv_world" + OpenCV_Version+ ".dll")));
	OpenCV_World_Handler = FPlatformProcess::GetDllHandle(*(LibraryPath + TEXT("opencv_world" + OpenCV_Version ".dll")));
	OpenCV_FFmpeg_Handler = FPlatformProcess::GetDllHandle(*(LibraryPath + TEXT("opencv_videoio_ffmpeg" + OpenCV_Version + "_64.dll")));
	if (!OpenCV_World_Handler || !OpenCV_FFmpeg_Handler)
	{
		UE_LOG(LogTemp, Error, TEXT("Load OpenCV dll failed!"));
	}
#endif
}

void FTAToolsPluginModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
#if WITH_OPENCV
	if (OpenCV_World_Handler)
	{
		FPlatformProcess::FreeDllHandle(OpenCV_World_Handler);
		OpenCV_World_Handler = nullptr;
	}
	if (OpenCV_FFmpeg_Handler)
	{
		FPlatformProcess::FreeDllHandle(OpenCV_FFmpeg_Handler);
		OpenCV_FFmpeg_Handler = nullptr;
	}
#elif PLATFORM_ANDROID

#elif PLATFORM_IOS

#endif
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FTAToolsPluginModule, TAToolsPlugin)