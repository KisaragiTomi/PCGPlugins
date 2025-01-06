using UnrealBuildTool;

public class FoliageCluster : ModuleRules
{
    public FoliageCluster(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core", 
                "Foliage",
            }
        );

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "CoreUObject",
                "Engine",
                "Slate",
                "UnrealEd",
                "GeometryFramework",
                "SlateCore"
            }
        );
    }
}