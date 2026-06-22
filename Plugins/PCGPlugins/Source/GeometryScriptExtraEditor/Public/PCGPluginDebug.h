#pragma once

#include "CoreMinimal.h"

#ifndef PCGPLUGINS_DEBUG
#define PCGPLUGINS_DEBUG (!UE_BUILD_SHIPPING)
#endif

#define PCGPLUGINS_DEBUG_ENABLED (PCGPLUGINS_DEBUG && !UE_BUILD_SHIPPING)

#if PCGPLUGINS_DEBUG_ENABLED

class FPCGPluginScopedDebugTimer
{
public:
	FPCGPluginScopedDebugTimer(const TCHAR* InLabel, const TCHAR* InPrefix = TEXT("[PCGDebugTiming]"))
		: Label(InLabel)
		, Prefix(InPrefix)
		, StartSeconds(FPlatformTime::Seconds())
	{
	}

	~FPCGPluginScopedDebugTimer()
	{
		UE_LOG(LogTemp, Display, TEXT("%s %s: %.3f ms"), Prefix, Label, (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
	}

private:
	const TCHAR* Label = nullptr;
	const TCHAR* Prefix = nullptr;
	double StartSeconds = 0.0;
};

#define PCG_DEBUG_ONLY(...) __VA_ARGS__
#define PCG_DEBUG_LOG(CategoryName, Verbosity, Format, ...) UE_LOG(CategoryName, Verbosity, Format, ##__VA_ARGS__)
#define PCG_DEBUG_TIME_SCOPE(Label) FPCGPluginScopedDebugTimer PREPROCESSOR_JOIN(PCGPluginDebugTimer_, __LINE__)(Label)
#define PCG_DEBUG_TIME_SCOPE_WITH_PREFIX(Prefix, Label) FPCGPluginScopedDebugTimer PREPROCESSOR_JOIN(PCGPluginDebugTimer_, __LINE__)(Label, Prefix)

#else

#define PCG_DEBUG_ONLY(...)
#define PCG_DEBUG_LOG(CategoryName, Verbosity, Format, ...)
#define PCG_DEBUG_TIME_SCOPE(Label)
#define PCG_DEBUG_TIME_SCOPE_WITH_PREFIX(Prefix, Label)

#endif
