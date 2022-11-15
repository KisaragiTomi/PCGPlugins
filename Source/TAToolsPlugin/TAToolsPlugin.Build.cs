// Some copyright should be here...

using UnrealBuildTool;
using System.IO;

public class TAToolsPlugin : ModuleRules
{

    public TAToolsPlugin(ReadOnlyTargetRules Target) : base(Target)
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
				"Eigen",
				"Core",
				"Projects",
				"CoreUObject",
				"Engine",
				"RenderCore",
				"InputCore",
				"RHI",
				"Foliage",
				"GeometryCore",
				"StaticMeshDescription",
				"Kismet",
				"MeshConversion",
				"MeshDescription",
				"MeshUtilitiesCommon",
				"GeometryAlgorithms",
				"DynamicMesh",
				"Landscape",

				// ... add other public dependencies that you statically link with here ...
			}
			);


		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Eigen",
				"CoreUObject",
                "Engine",
                "Slate",
                "SlateCore",
				
				// ... add private dependencies that you statically link with here ...	
			}
			);


		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);

        string PlatformDir = Target.Platform.ToString();
        string IncPath = Path.Combine(ModuleDirectory, "../../ThirdParty/OpenCV/includes");
        string LibPath = Path.Combine(ModuleDirectory, "../../ThirdParty/OpenCV/Libs");
        string LibName = "opencv_world451";

        PublicSystemIncludePaths.Add(IncPath);
        PublicAdditionalLibraries.Add(Path.Combine(LibPath, LibName + ".lib"));
        string DLLName = LibName + ".dll";
        PublicDelayLoadDLLs.Add(DLLName);
		RuntimeDependencies.Add(Path.Combine(DLLName));
		PublicDefinitions.Add("WITH_OPENCV=1");
    }
}