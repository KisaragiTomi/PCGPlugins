// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.

using UnrealBuildTool;

public class MCPUnreal : ModuleRules
{
	public MCPUnreal(ReadOnlyTargetRules Target) : base(Target)
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
			"HTTPServer",
			"Json",
			"JsonUtilities",
			"Slate",
			"SlateCore",
			"UnrealEd",
			"AssetRegistry",
			"BlueprintGraph",
			"KismetCompiler",
			"Kismet",
			"AnimGraph",
			"ImageCore",
			"AssetTools",
			"InputCore",
			"EnhancedInput",
			"LevelEditor",
			"EditorSubsystem",
			"UMG",
			"HTTP",
			"ProceduralMeshComponent",
			"PCG",
			"GameplayAbilities",
			"GameplayTags",
			"GameplayTasks",
			"Niagara",
			"NiagaraCore",
		});

		// Optional Fab plugin dependency — only if installed.
		if (Target.bBuildWithEditorOnlyData)
		{
			// Fab is an editor-only plugin bundled with UE 5.3+.
			PrivateDependencyModuleNames.Add("Fab");
			PublicDefinitions.Add("WITH_FAB=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_FAB=0");
		}

		// LiveCoding is Windows-only.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("LiveCoding");
			PublicDefinitions.Add("WITH_LIVE_CODING=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_LIVE_CODING=0");
		}

		// Preprocessor guards — set to 0 to compile without the dependency.
		PublicDefinitions.Add("WITH_PCG=1");
		PublicDefinitions.Add("WITH_GAMEPLAY_ABILITIES=1");
		PublicDefinitions.Add("WITH_NIAGARA=1");
	}
}
