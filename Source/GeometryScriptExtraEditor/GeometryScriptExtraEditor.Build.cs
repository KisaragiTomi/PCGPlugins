using UnrealBuildTool;

public class GeometryScriptExtraEditor : ModuleRules
{
    public GeometryScriptExtraEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseRTTI = true;
        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                
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
                "Landscape",
                "Foliage",
                "GeometryMath",
                "FoliageCluster",
                "DynamicMesh",
                "GeometryCore",
                "GeometryFramework",
                "ModelingComponents",
                "ModelingComponentsEditorOnly",
                "GeometryScriptingCore",
                "Projects",
                "MeshDescription",
                "StaticMeshDescription",
                "MeshConversion",
                "RenderCore",
                "RHI",
            }
        );
        
        if (Target.Type == TargetType.Editor)
        {
            PublicDependencyModuleNames.Add("FoliageCluster");
            
        }
        else
        {
            PublicDependencyModuleNames.Remove("FoliageCluster");
        }
        AddEngineThirdPartyPrivateStaticDependencies(Target,
            "IntelTBB",
            "UVAtlas",
            "DirectXMesh",
            "OpenVDB"
        );
    }
}