using System;
using System.IO;
using UnrealBuildTool;

public class GeometryMath : ModuleRules
{
    public GeometryMath(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
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
                "Core", 
            }
        );

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "CoreUObject",
                "Engine",
                "Slate",
                "SlateCore"
            }
        );
    }
}
