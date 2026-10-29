#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "BTForgeToolset.h"

// C++ toolsets are registered explicitly (see ConfigSettingsToolsetModule.cpp,
// SemanticSearchToolsetModule.cpp for the same pattern). RegisterToolsetClass
// silently no-ops if the registry subsystem is not ready yet; this module loads
// PostEngineInit, well after the registry.
class FBTForgeOfficialModule : public IModuleInterface
{
	virtual void StartupModule() override
	{
		UToolsetRegistry::RegisterToolsetClass(UBTForgeToolset::StaticClass());
	}
};

IMPLEMENT_MODULE(FBTForgeOfficialModule, BTForgeOfficial);
