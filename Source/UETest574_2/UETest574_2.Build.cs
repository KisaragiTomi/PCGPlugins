// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class UETest574_2 : ModuleRules
{
	public UETest574_2(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"AIModule",
			"NavigationSystem",
			"StateTreeModule",
			"GameplayStateTreeModule",
			"Niagara",
			"UMG",
			"Slate"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });

		PublicIncludePaths.AddRange(new string[] {
			"UETest574_2",
			"UETest574_2/Variant_Strategy",
			"UETest574_2/Variant_Strategy/UI",
			"UETest574_2/Variant_TwinStick",
			"UETest574_2/Variant_TwinStick/AI",
			"UETest574_2/Variant_TwinStick/Gameplay",
			"UETest574_2/Variant_TwinStick/UI"
		});

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
