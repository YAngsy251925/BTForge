using UnrealBuildTool;

public class BTForgeOfficial : ModuleRules
{
	public BTForgeOfficial(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"ToolsetRegistry"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"BTForgeCore"
		});
	}
}
