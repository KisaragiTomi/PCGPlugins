// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.IO;
using UnrealBuildTool;

public class PCGEditorProcess : ModuleRules
{
	public PCGEditorProcess(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "PCGPluginsShared"));
		bool bPCGPluginsDebug = Target.Configuration != UnrealTargetConfiguration.Shipping;
		string PCGPluginsDebugEnv = Environment.GetEnvironmentVariable("PCGPLUGINS_DEBUG");
		if (!string.IsNullOrWhiteSpace(PCGPluginsDebugEnv))
		{
			bPCGPluginsDebug = PCGPluginsDebugEnv != "0" && !PCGPluginsDebugEnv.Equals("false", StringComparison.OrdinalIgnoreCase);
		}
		PublicDefinitions.Add("PCGPLUGINS_DEBUG=" + (bPCGPluginsDebug ? "1" : "0"));

			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core", 
				"GeometryFramework", 
				"Blutility",
				// ... add other public dependencies that you statically link with here ...
			}
			);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"EditorFramework",
				"Engine",
				"Foliage",
				"Landscape",
				"RenderCore",
				"ComputeShaderGenerator", 
				"EditorScriptingUtilities",
				"Renderer",
				"Projects", 
				"PropertyEditor",
				"GeometryScriptExtraEditor",
				"GeometryScriptingCore",
				"DynamicMesh",
				"GeometryCore", 
				"RHI",
				"Core", 
				"InputCore",
				"Slate",
				"SlateCore",
				"AssetRegistry",
				"Core",
				"CoreUObject",
				"EditorStyle",
				"LevelEditor",
				"UnrealEd",
				"MaterialEditor",
				"MaterialUtilities",
				"Blutility",
				"LandscapeEditorUtilities",
				"AnimationCore",
				"MeshDescription",
				"SkeletalMeshDescription",
				"StaticMeshDescription",
			}
			);
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
		
		// if (Target.Type == TargetType.Editor)
		// {
		// 	PublicDependencyModuleNames.Add("PCGEditorProcess");
		// }
		// else
		// {
		// 	PublicDependencyModuleNames.Remove("PCGEditorProcess");
		// }
	}
}
