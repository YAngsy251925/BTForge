#include "BTForgeToolset.h"

#include "BTForgeBlueprintLibrary.h"
#include "BTForgeCatalogLibrary.h"
#include "BTForgeEQSLibrary.h"
#include "BTForgeLibrary.h"
#include "BTForgeReferenceLibrary.h"
#include "BTForgeVerifyLibrary.h"

FString UBTForgeToolset::CreateBehaviorTreeAsset(const FString& AssetPath, const FString& BlackboardJson)
{
	return UBTForgeLibrary::CreateBehaviorTreeAsset(AssetPath, BlackboardJson);
}

FString UBTForgeToolset::BuildBehaviorTree(const FString& AssetPath, const FString& TreeJson)
{
	return UBTForgeLibrary::BuildBehaviorTree(AssetPath, TreeJson);
}

FString UBTForgeToolset::InspectBehaviorTree(const FString& AssetPath)
{
	return UBTForgeLibrary::InspectBehaviorTree(AssetPath);
}

FString UBTForgeToolset::ValidateBehaviorTree(const FString& TreeJson, const FString& BlackboardJson)
{
	return UBTForgeLibrary::ValidateBehaviorTree(TreeJson, BlackboardJson);
}

FString UBTForgeToolset::CreateEQSAsset(const FString& AssetPath, const FString& QueryJson)
{
	return UBTForgeEQSLibrary::CreateEQSAsset(AssetPath, QueryJson);
}

FString UBTForgeToolset::InspectEQSAsset(const FString& AssetPath)
{
	return UBTForgeEQSLibrary::InspectEQSAsset(AssetPath);
}

FString UBTForgeToolset::ValidateEQSSpec(const FString& QueryJson)
{
	return UBTForgeEQSLibrary::ValidateEQSSpec(QueryJson);
}

FString UBTForgeToolset::GetCapabilityCatalog(const FString& SectionJson)
{
	return UBTForgeCatalogLibrary::GetCapabilityCatalog(SectionJson);
}

FString UBTForgeToolset::StartVerify(const FString& OptionsJson)
{
	return UBTForgeVerifyLibrary::StartVerify(OptionsJson);
}

FString UBTForgeToolset::GetVerifyResult(const FString& SessionId)
{
	return UBTForgeVerifyLibrary::GetVerifyResult(SessionId);
}

FString UBTForgeToolset::InspectReferences(const FString& RequestJson)
{
	return UBTForgeReferenceLibrary::InspectReferences(RequestJson);
}

FString UBTForgeToolset::CreateNodeBlueprint(const FString& AssetPath, const FString& ParentClassPath, const FString& ParamsJson)
{
	return UBTForgeBlueprintLibrary::CreateNodeBlueprint(AssetPath, ParentClassPath, ParamsJson);
}
