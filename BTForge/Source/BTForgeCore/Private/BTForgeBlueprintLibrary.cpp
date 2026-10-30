#include "BTForgeBlueprintLibrary.h"

#include "AssetToolsModule.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BTForgeLibrary.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Factories/BlueprintFactory.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Package.h"

namespace
{
	FString JsonToString(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		auto Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	FString MakeErrorJson(const FString& Message)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetBoolField(TEXT("ok"), false);
		Object->SetStringField(TEXT("error"), Message);
		return JsonToString(Object.ToSharedRef());
	}

	/** "/Game/AI/BTA_Log" -> package "/Game/AI", name "BTA_Log". */
	bool SplitAssetPath(const FString& AssetPath, FString& OutPackagePath, FString& OutAssetName)
	{
		if (!AssetPath.StartsWith(TEXT("/Game/")) || AssetPath.EndsWith(TEXT("/")))
		{
			return false;
		}
		FString Path = AssetPath;
		int32 LastSlash = INDEX_NONE;
		if (!Path.FindLastChar(TEXT('/'), LastSlash) || LastSlash <= 0)
		{
			return false;
		}
		OutAssetName = Path.Mid(LastSlash + 1);
		OutPackagePath = Path.Left(LastSlash);
		return OutAssetName.Len() > 0 && OutPackagePath.Len() > 5; // at least "/Game/X"
	}

	/** Remove the half-created asset so a failed call leaves nothing behind. */
	void DeleteHalfCreatedAsset(const FString& AssetPath)
	{
		if (GEditor)
		{
			if (UEditorAssetSubsystem* AssetSubsystem = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>())
			{
				AssetSubsystem->DeleteAsset(AssetPath);
			}
		}
	}
}

FString UBTForgeBlueprintLibrary::CreateNodeBlueprint(const FString& AssetPath, const FString& ParentClassPath, const FString& ParamsJson)
{
	FString PackagePath;
	FString AssetName;
	if (!SplitAssetPath(AssetPath, PackagePath, AssetName))
	{
		return MakeErrorJson(FString::Printf(TEXT("'%s' is not a valid /Game asset path, e.g. /Game/AI/BTA_LogShot"), *AssetPath));
	}
	if (LoadObject<UBlueprint>(nullptr, *AssetPath) != nullptr || StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath) != nullptr)
	{
		return MakeErrorJson(TEXT("asset already exists at '") + AssetPath + TEXT("'; nothing is overwritten"));
	}
	if (ParentClassPath.IsEmpty())
	{
		return MakeErrorJson(TEXT("ParentClassPath is required, e.g. /Script/BTForgeRuntime.BTForgeTask_LogMessage"));
	}

	UClass* ParentClass = StaticLoadClass(UObject::StaticClass(), nullptr, *ParentClassPath);
	if (ParentClass == nullptr)
	{
		return MakeErrorJson(FString::Printf(TEXT("could not load a class from '%s'. Use an object path like /Script/Module.ClassName (native) or /Game/Folder/Asset.Asset_C (blueprint)"), *ParentClassPath));
	}
	if (ParentClass->HasAnyClassFlags(CLASS_Abstract))
	{
		return MakeErrorJson(FString::Printf(TEXT("'%s' is abstract; blueprint node classes need a concrete parent"), *ParentClassPath));
	}
	// Blueprint composites do not exist in the engine (UBTCompositeNode is not
	// blueprint-creatable), so task/service/decorator are the supported bases.
	if (!ParentClass->IsChildOf(UBTTaskNode::StaticClass())
		&& !ParentClass->IsChildOf(UBTService::StaticClass())
		&& !ParentClass->IsChildOf(UBTDecorator::StaticClass()))
	{
		return MakeErrorJson(FString::Printf(TEXT("'%s' is a %s; blueprint node parents must derive from UBTTaskNode, UBTService or UBTDecorator"), *ParentClassPath, *ParentClass->GetName()));
	}
	// The engine's CreateBlueprint refuses non-Blueprintable parents; engine BT
	// node bases are MinimalAPI-only, so in practice this means a BTForge node
	// class or an engine *BlueprintBase. Say so instead of returning a bare null.
	if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
	{
		return MakeErrorJson(FString::Printf(TEXT("'%s' is not Blueprintable; derive the blueprint from a BTForge node class (/Script/BTForgeRuntime.*) or an engine BlueprintBase class instead"), *ParentClassPath));
	}

	FAssetToolsModule& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
	Factory->ParentClass = ParentClass;
	Factory->BlueprintType = BPTYPE_Normal;
	Factory->bSkipClassPicker = true; // never open the class-picker dialog

	UBlueprint* NewBP = Cast<UBlueprint>(AssetTools.Get().CreateAsset(AssetName, PackagePath, UBlueprint::StaticClass(), Factory));
	if (NewBP == nullptr)
	{
		return MakeErrorJson(TEXT("failed to create a blueprint asset at '") + AssetPath + TEXT("'"));
	}

	// A data-only blueprint still needs a compiled generated class: the BT
	// asset (and a cooked build) reference the BPGC, not the editor asset.
	FCompilerResultsLog CompileResults;
	FKismetEditorUtilities::CompileBlueprint(NewBP, EBlueprintCompileOptions::None, &CompileResults);
	if (NewBP->Status == BS_Error || CompileResults.NumErrors > 0)
	{
		FString FirstError;
		for (const TSharedRef<FTokenizedMessage>& Message : CompileResults.Messages)
		{
			if (Message->GetSeverity() == EMessageSeverity::Error)
			{
				FirstError = Message->ToText().ToString();
				break;
			}
		}
		DeleteHalfCreatedAsset(AssetPath);
		return MakeErrorJson(TEXT("blueprint compile failed") + (FirstError.IsEmpty() ? FString() : (TEXT(": ") + FirstError)));
	}

	UClass* GeneratedClass = NewBP->GeneratedClass.Get();
	if (GeneratedClass == nullptr)
	{
		DeleteHalfCreatedAsset(AssetPath);
		return MakeErrorJson(TEXT("blueprint has no generated class after compile"));
	}

	int32 AppliedCount = 0;
	if (!ParamsJson.IsEmpty())
	{
		FString ApplyError;
		if (!UBTForgeLibrary::ApplyParamsToObject(GeneratedClass->GetDefaultObject(), ParamsJson, AppliedCount, ApplyError))
		{
			DeleteHalfCreatedAsset(AssetPath);
			return MakeErrorJson(ApplyError);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(NewBP);
	if (GEditor)
	{
		if (UEditorAssetSubsystem* AssetSubsystem = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>())
		{
			AssetSubsystem->SaveLoadedAsset(NewBP, false);
		}
		else
		{
			return MakeErrorJson(TEXT("blueprint created but not saved: EditorAssetSubsystem unavailable at ") + AssetPath);
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("assetPath"), AssetPath);
	Result->SetStringField(TEXT("classPath"), GeneratedClass->GetPathName());
	Result->SetStringField(TEXT("parent"), ParentClass->GetPathName());
	Result->SetNumberField(TEXT("appliedParams"), AppliedCount);
	Result->SetStringField(TEXT("note"), TEXT("use the classPath as a build_behavior_tree node 'type'; blackboard key names in params resolve per-tree at build time"));
	return JsonToString(Result);
}
