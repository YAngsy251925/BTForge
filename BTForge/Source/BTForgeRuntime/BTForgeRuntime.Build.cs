using UnrealBuildTool;

/**
 * Runtime half of BT Forge.
 *
 * Why this module exists: BT assets and EQS assets are *runtime* data and may
 * reference node/context classes at cook time. Anything referenced from a tree
 * or query must therefore live in a Runtime module - an Editor module would
 * work in PIE and then break in a packaged build. This module is deliberately
 * tiny (no editor dependencies) so it can ship with the plugin.
 */
public class BTForgeRuntime : ModuleRules
{
	public BTForgeRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AIModule"
		});
	}
}
