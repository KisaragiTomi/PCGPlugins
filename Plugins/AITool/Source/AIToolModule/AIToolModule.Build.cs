using UnrealBuildTool;

public class AIToolModule : ModuleRules
{
	public AIToolModule(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Projects",           // IPluginManager (weights path resolution)
			"GeometryCore",       // FDynamicMesh3
			"GeometryFramework",  // UDynamicMesh
		});

		// Dense float32 math for the NKSR C++ port (GEMM etc.)
		AddEngineThirdPartyPrivateStaticDependencies(Target, "Eigen");

		// Ship the converted NKSR network weights with packaged builds.
		RuntimeDependencies.Add("$(PluginDir)/Resources/nksr_ks.nkw");
	}
}
