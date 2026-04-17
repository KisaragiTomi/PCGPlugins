// Some copyright should be here...

using UnrealBuildTool;

public class GeometryEditor : ModuleRules
{
	public GeometryEditor(ReadOnlyTargetRules Target) : base(Target)
	{
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

				// ... add other public dependencies that you statically link with here ...
			}
			);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Engine",
				"Slate",
				"UnrealEd",
				"SlateCore",
				"LevelEditor",
				"ModelingOperators",
				"CoreUObject",
				"DeveloperSettings",
				"GeometryAlgorithms",
				"GeometryCore",
				"GeometryFramework",
				"Projects",
				"ModelingComponentsEditorOnly",
				"TraceLog",
				"Core",
				"InputCore",
				"RawMesh",
				"StaticMeshDescription",
				"MeshUtilities",
				"MaterialUtilities",
				"PropertyEditor",
				"RenderCore",
				"RHI",
				"QuadricMeshReduction",
				"DynamicMesh",
				"MeshConversion",
				"MeshDescription",
				"GeometryScriptingCore",
				// ... add private dependencies that you statically link with here ...	
			}
			);
		
	}
}
