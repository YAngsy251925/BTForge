#include "BTForgeCatalogLibrary.h"

#include "BTForgeEQSLibrary.h"
#include "BTForgeLibrary.h"
#include "BTForgeParamSchema.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "Blueprint/BlueprintSupport.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryContext.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "Misc/EngineVersion.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectHash.h"

namespace
{
	constexpr int32 MaxEntriesPerSection = 400;
	constexpr int32 MaxParamsPerClass = 40;

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

	TArray<TSharedPtr<FJsonValue>> DumpEditableParams(UClass* Class)
	{
		TArray<TSharedPtr<FJsonValue>> Params;
		if (Class == nullptr)
		{
			return Params;
		}
		for (TFieldIterator<FProperty> It(Class); It && Params.Num() < MaxParamsPerClass; ++It)
		{
			const FProperty* Property = *It;
			if (!Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
			{
				continue;
			}
			TSharedRef<FJsonObject> Param = MakeShared<FJsonObject>();
			Param->SetStringField(TEXT("name"), Property->GetName());
			Param->SetStringField(TEXT("type"), Property->GetCPPType());
			// JSON shape hint: what value the spec writer actually accepts
			// (number / boolean / enum values / class path / ...).
			Param->SetStringField(TEXT("json"), BTForgeParamSchema::GetJsonPropertyShape(Property));
			Params.Add(MakeShared<FJsonValueObject>(Param));
		}
		return Params;
	}

	FString ClassSource(const UClass* Class)
	{
		return Class->HasAnyClassFlags(CLASS_CompiledFromBlueprint) ? TEXT("blueprint") : TEXT("native");
	}

	TSharedRef<FJsonObject> MakeClassEntry(UClass* Class, const FString& Kind, const FString& Source)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("kind"), Kind);
		Entry->SetStringField(TEXT("classPath"), Class->GetPathName());
		Entry->SetStringField(TEXT("name"), Class->GetName());
		Entry->SetStringField(TEXT("source"), Source);
		Entry->SetArrayField(TEXT("editableParams"), DumpEditableParams(Class));
		return Entry;
	}

	FString ClassifyBehaviorTreeNode(const UClass* Class)
	{
		if (Class == nullptr)
		{
			return FString();
		}
		if (Class->IsChildOf(UBTService::StaticClass()))
		{
			return TEXT("service");
		}
		if (Class->IsChildOf(UBTDecorator::StaticClass()))
		{
			return TEXT("decorator");
		}
		if (Class->IsChildOf(UBTTaskNode::StaticClass()))
		{
			return TEXT("task");
		}
		if (Class->IsChildOf(UBTCompositeNode::StaticClass()))
		{
			return TEXT("composite");
		}
		return FString();
	}

	FString ClassifyEQSClass(const UClass* Class)
	{
		if (Class == nullptr)
		{
			return FString();
		}
		if (Class->IsChildOf(UEnvQueryContext::StaticClass()))
		{
			return TEXT("context");
		}
		if (Class->IsChildOf(UEnvQueryGenerator::StaticClass()))
		{
			return TEXT("generator");
		}
		if (Class->IsChildOf(UEnvQueryTest::StaticClass()))
		{
			return TEXT("test");
		}
		return FString();
	}

	void AddNativeClassEntries(UClass* BaseClass, const FString& Kind, TArray<TSharedPtr<FJsonValue>>& OutEntries)
	{
		TArray<UClass*> DerivedClasses;
		GetDerivedClasses(BaseClass, DerivedClasses, true);
		DerivedClasses.Sort([](const UClass& A, const UClass& B) { return A.GetName() < B.GetName(); });
		for (UClass* Class : DerivedClasses)
		{
			if (OutEntries.Num() >= MaxEntriesPerSection)
			{
				break;
			}
			if (Class == nullptr || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
			{
				continue;
			}
			// Loaded blueprint node classes are reported by the AssetRegistry path
			// below; skipping them here keeps the native list free of duplicates.
			if (Class->HasAnyClassFlags(CLASS_CompiledFromBlueprint))
			{
				continue;
			}
			OutEntries.Add(MakeShared<FJsonValueObject>(MakeClassEntry(Class, Kind, ClassSource(Class))));
		}
	}

	/** AssetRegistry tags store object paths in export form,
	 *  e.g. "/Script/Engine.BlueprintGeneratedClass'/Game/AI/BTS_X.BTS_X_C'".
	 *  Extract the object path inside the quotes so LoadObject can use it. */
	FString NormalizeExportPath(const FString& ExportPath)
	{
		int32 FirstQuote = INDEX_NONE;
		int32 LastQuote = INDEX_NONE;
		if (ExportPath.FindChar(TEXT('\''), FirstQuote) && ExportPath.FindLastChar(TEXT('\''), LastQuote) && LastQuote > FirstQuote)
		{
			return ExportPath.Mid(FirstQuote + 1, LastQuote - FirstQuote - 1);
		}
		return ExportPath;
	}

	void AddBlueprintClassEntries(bool bBehaviorTree, TArray<TSharedPtr<FJsonValue>>& OutEntries)
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& Registry = AssetRegistryModule.Get();

		FARFilter Filter;
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = false;
		Filter.bRecursivePaths = true;
		TArray<FAssetData> BlueprintAssets;
		Registry.GetAssets(Filter, BlueprintAssets);

		TArray<TSharedPtr<FJsonValue>> Found;
		for (const FAssetData& AssetData : BlueprintAssets)
		{
			if (Found.Num() >= MaxEntriesPerSection)
			{
				break;
			}
			FString NativeParentPath;
			FString GeneratedClassPath;
			if (!AssetData.GetTagValue(FBlueprintTags::NativeParentClassPath, NativeParentPath)
				|| !AssetData.GetTagValue(FBlueprintTags::GeneratedClassPath, GeneratedClassPath))
			{
				continue;
			}
			NativeParentPath = NormalizeExportPath(NativeParentPath);
			GeneratedClassPath = NormalizeExportPath(GeneratedClassPath);
			if (NativeParentPath.IsEmpty() || GeneratedClassPath.IsEmpty() || GeneratedClassPath == TEXT("None"))
			{
				continue;
			}
			UClass* NativeParent = LoadObject<UClass>(nullptr, *NativeParentPath);
			if (NativeParent == nullptr)
			{
				continue;
			}

			const FString Kind = bBehaviorTree ? ClassifyBehaviorTreeNode(NativeParent) : ClassifyEQSClass(NativeParent);
			if (Kind.IsEmpty())
			{
				continue;
			}

			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("kind"), Kind);
			Entry->SetStringField(TEXT("classPath"), GeneratedClassPath);
			Entry->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
			Entry->SetStringField(TEXT("source"), TEXT("blueprint"));
			Entry->SetStringField(TEXT("assetPath"), AssetData.GetObjectPathString());
			Found.Add(MakeShared<FJsonValueObject>(Entry));
		}

		// AssetRegistry order is not guaranteed; sort for deterministic output.
		Found.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			const TSharedPtr<FJsonObject> ObjectA = A.IsValid() ? A->AsObject() : nullptr;
			const TSharedPtr<FJsonObject> ObjectB = B.IsValid() ? B->AsObject() : nullptr;
			const FString NameA = ObjectA.IsValid() ? ObjectA->GetStringField(TEXT("name")) : FString();
			const FString NameB = ObjectB.IsValid() ? ObjectB->GetStringField(TEXT("name")) : FString();
			return NameA < NameB;
		});
		OutEntries.Append(Found);
	}

	/** Short aliases derived from UBTForgeEQSLibrary's authoritative tables, so
	 *  the catalog can never drift from what the spec parser accepts. */
	void AddAliasEntries(const TMap<FString, UClass*>& Aliases, TArray<TSharedPtr<FJsonValue>>& OutEntries)
	{
		TArray<FString> AliasNames;
		Aliases.GetKeys(AliasNames);
		AliasNames.Sort();
		for (const FString& Alias : AliasNames)
		{
			const UClass* Class = Aliases.FindRef(Alias);
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("alias"), Alias);
			Entry->SetStringField(TEXT("classPath"), Class != nullptr ? Class->GetPathName() : FString());
			Entry->SetStringField(TEXT("name"), Class != nullptr ? Class->GetName() : FString());
			OutEntries.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	void AddEQSAliasMap(TArray<TSharedPtr<FJsonValue>>& OutEntries)
	{
		AddAliasEntries(UBTForgeEQSLibrary::GetGeneratorTypeAliases(), OutEntries);
	}

	void AddEQSTestAliasMap(TArray<TSharedPtr<FJsonValue>>& OutEntries)
	{
		AddAliasEntries(UBTForgeEQSLibrary::GetTestTypeAliases(), OutEntries);
	}
}

FString UBTForgeCatalogLibrary::GetCapabilityCatalog(const FString& SectionJson)
{
	bool bIncludeBT = true;
	bool bIncludeEQS = true;
	if (!SectionJson.IsEmpty())
	{
		TSharedPtr<FJsonObject> SectionObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SectionJson);
		if (FJsonSerializer::Deserialize(Reader, SectionObject) && SectionObject.IsValid())
		{
			FString Section;
			if (SectionObject->TryGetStringField(TEXT("section"), Section))
			{
				bIncludeBT = Section.Equals(TEXT("all"), ESearchCase::IgnoreCase) || Section.Equals(TEXT("bt"), ESearchCase::IgnoreCase);
				bIncludeEQS = Section.Equals(TEXT("all"), ESearchCase::IgnoreCase) || Section.Equals(TEXT("eqs"), ESearchCase::IgnoreCase);
				if (!bIncludeBT && !bIncludeEQS)
				{
					return MakeErrorJson(TEXT("unknown section '") + Section + TEXT("'. Allowed: all, bt, eqs"));
				}
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
	Result->SetStringField(TEXT("note"), TEXT("Blueprint entries come from AssetRegistry tags; unmounted plugin/GameFeature content may be missing. classPath values can be used directly as spec 'type' values."));

	if (bIncludeBT)
	{
		TSharedPtr<FJsonObject> BT = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> NativeNodes;
		AddNativeClassEntries(UBTCompositeNode::StaticClass(), TEXT("composite"), NativeNodes);
		AddNativeClassEntries(UBTTaskNode::StaticClass(), TEXT("task"), NativeNodes);
		AddNativeClassEntries(UBTService::StaticClass(), TEXT("service"), NativeNodes);
		AddNativeClassEntries(UBTDecorator::StaticClass(), TEXT("decorator"), NativeNodes);
		BT->SetArrayField(TEXT("nativeNodes"), NativeNodes);

		TArray<TSharedPtr<FJsonValue>> BlueprintNodes;
		AddBlueprintClassEntries(/*bBehaviorTree=*/true, BlueprintNodes);
		BT->SetArrayField(TEXT("blueprintNodes"), BlueprintNodes);

		TSharedRef<FJsonObject> SpecTypes = MakeShared<FJsonObject>();
		// Alias lists come from UBTForgeLibrary's authoritative tables, so the
		// catalog can never drift from what the spec parser accepts.
		TArray<TSharedPtr<FJsonValue>> NodeTypes;
		AddAliasEntries(UBTForgeLibrary::GetNodeTypeAliases(), NodeTypes);
		SpecTypes->SetArrayField(TEXT("nodeTypes"), NodeTypes);
		TArray<TSharedPtr<FJsonValue>> ServiceTypes;
		AddAliasEntries(UBTForgeLibrary::GetAuxTypeAliases(TEXT("service")), ServiceTypes);
		SpecTypes->SetArrayField(TEXT("serviceTypes"), ServiceTypes);
		TArray<TSharedPtr<FJsonValue>> DecoratorTypes;
		AddAliasEntries(UBTForgeLibrary::GetAuxTypeAliases(TEXT("decorator")), DecoratorTypes);
		SpecTypes->SetArrayField(TEXT("decoratorTypes"), DecoratorTypes);
		TArray<TSharedPtr<FJsonValue>> KeyTypes;
		for (const TCHAR* Name : { TEXT("Bool"), TEXT("Int"), TEXT("Float"), TEXT("String"), TEXT("Name"), TEXT("Vector"), TEXT("Rotator"), TEXT("Object") })
		{
			KeyTypes.Add(MakeShared<FJsonValueString>(Name));
		}
		SpecTypes->SetArrayField(TEXT("blackboardKeyTypes"), KeyTypes);
		BT->SetObjectField(TEXT("specTypes"), SpecTypes);
		Result->SetObjectField(TEXT("bt"), BT);
	}

	if (bIncludeEQS)
	{
		TSharedPtr<FJsonObject> EQS = MakeShared<FJsonObject>();
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& Registry = AssetRegistryModule.Get();

		TArray<FAssetData> QueryAssets;
		Registry.GetAssetsByClass(UEnvQuery::StaticClass()->GetClassPathName(), QueryAssets, true);
		TArray<TSharedPtr<FJsonValue>> Queries;
		for (const FAssetData& AssetData : QueryAssets)
		{
			if (Queries.Num() >= MaxEntriesPerSection)
			{
				break;
			}
			Queries.Add(MakeShared<FJsonValueString>(AssetData.GetObjectPathString()));
		}
		EQS->SetArrayField(TEXT("queries"), Queries);

		TArray<TSharedPtr<FJsonValue>> NativeContexts;
		AddNativeClassEntries(UEnvQueryContext::StaticClass(), TEXT("context"), NativeContexts);
		EQS->SetArrayField(TEXT("nativeContexts"), NativeContexts);

		TArray<TSharedPtr<FJsonValue>> BlueprintContexts;
		AddBlueprintClassEntries(/*bBehaviorTree=*/false, BlueprintContexts);
		EQS->SetArrayField(TEXT("blueprintClasses"), BlueprintContexts);

		TArray<TSharedPtr<FJsonValue>> Generators;
		AddNativeClassEntries(UEnvQueryGenerator::StaticClass(), TEXT("generator"), Generators);
		EQS->SetArrayField(TEXT("generators"), Generators);

		TArray<TSharedPtr<FJsonValue>> Tests;
		AddNativeClassEntries(UEnvQueryTest::StaticClass(), TEXT("test"), Tests);
		EQS->SetArrayField(TEXT("tests"), Tests);

		TSharedRef<FJsonObject> SpecTypes = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> GeneratorAliases;
		AddEQSAliasMap(GeneratorAliases);
		SpecTypes->SetArrayField(TEXT("generatorAliases"), GeneratorAliases);
		TArray<TSharedPtr<FJsonValue>> TestAliases;
		AddEQSTestAliasMap(TestAliases);
		SpecTypes->SetArrayField(TEXT("testAliases"), TestAliases);
		EQS->SetObjectField(TEXT("specTypes"), SpecTypes);
		Result->SetObjectField(TEXT("eqs"), EQS);
	}

	return JsonToString(Result.ToSharedRef());
}
