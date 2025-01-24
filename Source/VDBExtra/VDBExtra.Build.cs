// Some copyright should be here...

using UnrealBuildTool;

public class VDBExtra : ModuleRules
{
	public VDBExtra(ReadOnlyTargetRules Target) : base(Target)
	{
		bUseRTTI = true;
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicIncludePaths.AddRange(
			new string[] {

				// ... add public include paths required here ...
			}
			);
				
		
		PrivateIncludePaths.AddRange(
			new string[] {

				// ... add other private include paths required here ...
			}
			);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"TraceLog",
				"Core",
				"CoreUObject",
				"Engine",
				"InputCore",
				"UnrealEd",
				"RawMesh",
				"StaticMeshDescription",
				"MeshUtilities",
				"MaterialUtilities",
				"PropertyEditor",
				"SlateCore",
				"Slate",
				"RenderCore",
				"RHI",
				"QuadricMeshReduction",
				"DynamicMesh",
				"GeometryAlgorithms",
				"GeometryCore",
				"GeometryFramework",
				"MeshConversion",
				"MeshDescription",
				"GeometryScriptingCore",
				"ProxyLODMeshReduction",
				// ... add other public dependencies that you statically link with here ...
			}
			);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Slate",
				"ProxyLODMeshReduction",
				"SlateCore",
				// ... add private dependencies that you statically link with here ...	
			}
			);
		
		
		AddEngineThirdPartyPrivateStaticDependencies(Target,
			"IntelTBB",
			"UVAtlas",
			"DirectXMesh",
			"OpenVDB"
		);
	}
}
