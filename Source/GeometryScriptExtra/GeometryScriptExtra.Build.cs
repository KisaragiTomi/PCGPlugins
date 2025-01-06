using UnrealBuildTool;

public class GeometryScriptExtra : ModuleRules
{
    public GeometryScriptExtra(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
        
        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "GeometryScriptingCore",
                "Projects",
                "MeshDescription",
                "StaticMeshDescription",
                "MeshConversion",
                "RenderCore",
                "RHI",
                "Landscape",
                "Foliage",
                "GeometryMath",
                "FoliageCluster",
                "DynamicMesh",
                "GeometryCore",
                "GeometryFramework",
                "ModelingComponents"
                
            }
        );


        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "ModelingOperators",
                "CoreUObject",
                "DeveloperSettings",
                "Engine",
                "GeometryAlgorithms",
                "GeometryCore",
                "GeometryFramework",
                "GeometryScriptingCore",
                "Projects",
                "Slate",
                "UnrealEd",
                "ProxyLODMeshReduction",
                "SlateCore", 
                "ModelingComponentsEditorOnly"
            }
        );
    }
}