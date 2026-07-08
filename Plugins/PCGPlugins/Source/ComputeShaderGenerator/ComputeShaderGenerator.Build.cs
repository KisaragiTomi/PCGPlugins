// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.IO;
using UnrealBuildTool;

public class ComputeShaderGenerator : ModuleRules
{
	public ComputeShaderGenerator(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseRTTI = true;
		bool bPCGPluginsDebug = Target.Configuration != UnrealTargetConfiguration.Shipping;
		string PCGPluginsDebugEnv = Environment.GetEnvironmentVariable("PCGPLUGINS_DEBUG");
		if (!string.IsNullOrWhiteSpace(PCGPluginsDebugEnv))
		{
			bPCGPluginsDebug = PCGPluginsDebugEnv != "0" && !PCGPluginsDebugEnv.Equals("false", StringComparison.OrdinalIgnoreCase);
		}
		PublicDefinitions.Add("PCGPLUGINS_DEBUG=" + (bPCGPluginsDebug ? "1" : "0"));

		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
			);
				
		
		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add private include paths required here ...
			}
			);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"GeometryFramework",
				"Renderer",
				"RenderCore",
				"RHI",
				// ... add other public dependencies that you statically link with here ...
			}
			);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Projects", 
				"GeometryScriptingCore",
				"DynamicMesh",
				"GeometryCore",
				"MeshConversion", 
				"Landscape",
				"ImageCore",
				"Foliage",
				"MeshDescription",
				"StaticMeshDescription",
				"GeometryAlgorithms",
				"ModelingComponents",
				"AssetRegistry",
			}
			);

		if (Target.Type == TargetType.Editor)
		{
			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"UnrealEd",
					"ModelingComponentsEditorOnly",
					"EditorScriptingUtilities",
				}
			);
		}

		AddEngineThirdPartyPrivateStaticDependencies(Target,
			"IntelTBB",
			"OpenVDB",
			"Blosc",
			"zlib"
		);

		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
	}
}
