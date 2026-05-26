using System;
using System.IO;
using UnrealBuildTool;

public class GeometryScriptExtraEditor : ModuleRules
{
    public GeometryScriptExtraEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        // Avoid shared-PCH symbol/link mismatches in this editor-only module.
        PCHUsage = ModuleRules.PCHUsageMode.NoPCHs;
        bUseUnity = false;
        bUseRTTI = true;
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
                "ComputeShaderGenerator",
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
                "Renderer",
                "RenderCore",
                "RHI",
            }
        );

        AddEngineThirdPartyPrivateStaticDependencies(Target,
            "IntelTBB",
            "UVAtlas",
            "DirectXMesh",
            "OpenVDB",
            "Blosc",
            "zlib"
        );
    }
}
