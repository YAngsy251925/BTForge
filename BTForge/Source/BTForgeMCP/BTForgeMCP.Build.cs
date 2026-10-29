using UnrealBuildTool;

public class BTForgeMCP : ModuleRules
{
	public BTForgeMCP(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"BTForgeCore"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"HTTPServer",
			"Json"
		});
	}
}
