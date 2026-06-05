// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.

using UnrealBuildTool;

public class MCPUnrealTests : ModuleRules
{
	public MCPUnrealTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
			"JsonUtilities",
			"HTTPServer",
			"MCPUnreal",
		});
	}
}
