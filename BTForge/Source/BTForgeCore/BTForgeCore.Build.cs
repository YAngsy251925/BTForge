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
			// M4 reference policy: module host type (Runtime/Editor) comes from the
			// plugin descriptors, so the report can tell cook-safe classes from
			// editor-only ones.
			"Projects",
			// M5: the spec-alias tables reference the runtime node library's
			// classes directly (StaticClass), so renames break at compile time.
			"BTForgeRuntime",
			"UnrealEd"
		});
	}
}
