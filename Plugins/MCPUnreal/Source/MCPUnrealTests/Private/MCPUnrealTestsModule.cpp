// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.

#include "Modules/ModuleManager.h"

class FMCPUnrealTestsModule : public IModuleInterface {
 public:
  virtual void StartupModule() override {}
  virtual void ShutdownModule() override {}
};

IMPLEMENT_MODULE(FMCPUnrealTestsModule, MCPUnrealTests)
