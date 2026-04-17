// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class PCGEditorProcess : ModuleRules
{
	public PCGEditorProcess(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
			
		
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
				"Landscape",
				"RenderCore",
				"ComputeShaderGenerator", 
				"EditorScriptingUtilities",
				"Renderer",
				"Projects", 
				"GeometryScriptExtraEditor",
				"GeometryScriptingCore",
				"DynamicMesh",
				"GeometryCore", 
				"RHI",
				"Core", 
				"InputCore",
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
				"MeshDescription",
				"StaticMeshDescription",
				// ... add private dependencies that you statically link with here ...	
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
