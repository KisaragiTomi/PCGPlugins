// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class AssetHandlerPlugin : ModuleRules
{
	public AssetHandlerPlugin(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"AssetRegistry",
				"AssetTools",
				"EditorSubsystem"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"InputCore",
				"Slate",
				"SlateCore",
				"EditorStyle",
				"EditorScriptingUtilities",
				"UnrealEd",
				"Json",
				"JsonUtilities",
				"GeometryCollectionEngine"
			}
		);
	}
}
