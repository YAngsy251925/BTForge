#include "BTForgeVerifyLibrary.h"
#include "BTForgeVerifySession.h"

FString UBTForgeVerifyLibrary::StartVerify(const FString& OptionsJson)
{
	return FBTForgeVerifyManager::Get().StartVerify(OptionsJson);
}

FString UBTForgeVerifyLibrary::GetVerifyResult(const FString& SessionId)
{
	return FBTForgeVerifyManager::Get().GetVerifyResult(SessionId);
}
