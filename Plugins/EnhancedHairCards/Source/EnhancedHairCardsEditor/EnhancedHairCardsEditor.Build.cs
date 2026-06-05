using UnrealBuildTool;

public class EnhancedHairCardsEditor : ModuleRules
{
	public EnhancedHairCardsEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"EnhancedHairCards",
			"HairStrandsCore",
			"Niagara"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AdvancedPreviewScene",
			"AssetRegistry",
			"AssetTools",
			"EditorScriptingUtilities",
			"InputCore",
			"Kismet",
			"PropertyEditor",
			"Slate",
			"SlateCore",
			"ToolMenus",
			"UnrealEd"
		});

		PublicIncludePaths.Add(ModuleDirectory + "/Public");
		PrivateIncludePaths.Add(ModuleDirectory + "/Private");
	}
}
