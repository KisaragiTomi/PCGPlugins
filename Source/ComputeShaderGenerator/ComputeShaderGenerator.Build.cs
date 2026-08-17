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
				"Projects", 
				"GeometryScriptingCore",
				"DynamicMesh",
				"GeometryCore",
				"MeshConversion", 
				"Landscape",
				"ImageCore",
				"MeshDescription",
				"StaticMeshDescription",
				"GeometryAlgorithms",
				"ModelingComponents",
				"AssetRegistry",
			}
			);

		// CSGpuMemoryBudget queries the adapter's live local-VRAM budget through DXGI, which is the
		// only way to see memory other processes already took. Everything else falls back to the
		// cross-RHI estimate, so these dependencies stay Windows-only.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("D3D12RHI");
			AddEngineThirdPartyPrivateStaticDependencies(Target, "DX12");
		}

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
