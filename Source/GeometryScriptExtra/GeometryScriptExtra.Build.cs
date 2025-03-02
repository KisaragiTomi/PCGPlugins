using UnrealBuildTool;

public class GeometryScriptExtra : ModuleRules
{
    public GeometryScriptExtra(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseRTTI = true;
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
                "ProxyLODMeshReduction",
                "SlateCore", 
                "ModelingComponentsEditorOnly"
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