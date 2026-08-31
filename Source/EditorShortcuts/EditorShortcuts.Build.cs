// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class EditorShortcuts : ModuleRules
{
	public EditorShortcuts(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"EditorSubsystem",
				"InputCore",
				"Slate",
				"SlateCore",
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"DeveloperSettings",
				"AssetRegistry",
				"BlueprintGraph",
				"Blutility",
				"UnrealEd",
			}
			);
	}
}
