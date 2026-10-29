#include "BTForgeVerifySession.h"

#include "Modules/ModuleManager.h"

namespace
{
	/** BTForgeCore is an editor module whose only long-lived state is the verify
	 *  session manager (PIE delegate bindings, the sampling ticker and the log
	 *  capture device). Without this Shutdown hook that state outlives the module
	 *  and the editor unloads with dangling registrations. */
	class FBTForgeCoreModule : public FDefaultModuleImpl
	{
	public:
		virtual void ShutdownModule() override
		{
			FBTForgeVerifyManager::Get().Shutdown();
		}
	};
}

IMPLEMENT_MODULE(FBTForgeCoreModule, BTForgeCore);
