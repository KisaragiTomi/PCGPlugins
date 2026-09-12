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
				// Shaders/Private 里的 CSGpuSharedLayout.ush 是 C++ 与 .usf 共用的布局常量（只含 #define）。
				// C++ 侧按普通头文件 include 它，所以着色器目录也得在 include 路径里。
				Path.Combine(PluginDirectory, "Shaders", "Private"),
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
				"Foliage",
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

		// MeshCardBuild.h (the Lumen card set the GPU-mesh proxy hands the surface cache) includes
		// MeshUtilities.h under WITH_EDITORONLY_DATA. Include path only — nothing is linked, the
		// Engine module adds it the same way for the skeletal-mesh proxy.
		if (Target.bBuildWithEditorOnlyData)
		{
			PrivateIncludePathModuleNames.Add("MeshUtilities");
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
