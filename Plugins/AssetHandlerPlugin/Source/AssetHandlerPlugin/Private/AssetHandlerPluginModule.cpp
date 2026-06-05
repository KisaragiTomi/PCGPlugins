// Copyright Epic Games, Inc. All Rights Reserved.

#include "AssetHandlerPluginModule.h"

DEFINE_LOG_CATEGORY(LogAssetHandler);

void FAssetHandlerPluginModule::StartupModule()
{
	UE_LOG(LogAssetHandler, Log, TEXT("AssetHandlerPlugin: StartupModule"));
}

void FAssetHandlerPluginModule::ShutdownModule()
{
	UE_LOG(LogAssetHandler, Log, TEXT("AssetHandlerPlugin: ShutdownModule"));
}

IMPLEMENT_MODULE(FAssetHandlerPluginModule, AssetHandlerPlugin)
