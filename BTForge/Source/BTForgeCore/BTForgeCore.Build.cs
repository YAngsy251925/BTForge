using UnrealBuildTool;

public class BTForgeCore : ModuleRules
{
	public BTForgeCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AIModule",
			"AssetRegistry",
			"AssetTools",
			"HTTPServer",
			"Json",
			"UnrealEd"
		});
	}
}
