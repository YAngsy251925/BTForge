#include "BTForgeEQSLibrary.h"

#include "BTForgeParamSchema.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryContext.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_ActorsOfClass.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_Cone.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_CurrentLocation.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_Donut.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_OnCircle.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_PathingGrid.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_PerceivedActors.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_SimpleGrid.h"
#include "EnvironmentQuery/Items/EnvQueryItemType.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Distance.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Dot.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Overlap.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Pathfinding.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Project.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Random.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Trace.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Volume.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/UnrealType.h"

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

	/** "/Game/AI/EQS_X" -> package "/Game/AI", name "EQS_X". */
	bool SplitAssetPath(const FString& AssetPath, FString& OutPackagePath, FString& OutAssetName)
	{
		if (!AssetPath.StartsWith(TEXT("/Game/")) || AssetPath.EndsWith(TEXT("/")))
		{
			return false;
		}
		int32 LastSlash = INDEX_NONE;
		if (!AssetPath.FindLastChar(TEXT('/'), LastSlash) || LastSlash <= 0)
		{
			return false;
		}
		OutAssetName = AssetPath.Mid(LastSlash + 1);
		OutPackagePath = AssetPath.Left(LastSlash);
		return OutAssetName.Len() > 0 && OutPackagePath.Len() > 5;
	}

	UClass* LoadEQSClassFromPath(const FString& Path, UClass* ExpectedBase, const FString& KindName, FString& OutError)
	{
		UObject* Loaded = StaticLoadObject(UClass::StaticClass(), nullptr, *Path);
		UClass* Class = Cast<UClass>(Loaded);
		if (Class == nullptr)
		{
			OutError = FString::Printf(TEXT("could not load a %s class from '%s'. Use an object path like /Script/AIModule.EnvQueryContext_Querier or /Game/Folder/Asset.Asset_C"), *KindName, *Path);
			return nullptr;
		}
		if (!Class->IsChildOf(ExpectedBase))
		{
			OutError = FString::Printf(TEXT("'%s' is a %s, expected a class derived from %s"), *Path, *Class->GetName(), *ExpectedBase->GetName());
			return nullptr;
		}
		if (Class->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = FString::Printf(TEXT("'%s' is abstract and cannot be instantiated"), *Path);
			return nullptr;
		}
		return Class;
	}

	UClass* ResolveGeneratorClass(const FString& TypeName, FString& OutError)
	{
		if (TypeName.StartsWith(TEXT("/")))
		{
			return LoadEQSClassFromPath(TypeName, UEnvQueryGenerator::StaticClass(), TEXT("generator"), OutError);
		}
		const TMap<FString, UClass*>& Types = UBTForgeEQSLibrary::GetGeneratorTypeAliases();
		const UClass* const* Found = Types.Find(TypeName);
		if (Found == nullptr)
		{
			OutError = FString::Printf(TEXT("unknown generator type '%s'. Allowed: SimpleGrid, CurrentLocation, OnCircle, Donut, Cone, PathingGrid, ActorsOfClass, PerceivedActors, or a '/Script/...'/'/Game/...' class path to a UEnvQueryGenerator"), *TypeName);
			return nullptr;
		}
		return const_cast<UClass*>(*Found);
	}

	UClass* ResolveTestClass(const FString& TypeName, FString& OutError)
	{
		if (TypeName.StartsWith(TEXT("/")))
		{
			return LoadEQSClassFromPath(TypeName, UEnvQueryTest::StaticClass(), TEXT("test"), OutError);
		}
		const TMap<FString, UClass*>& Types = UBTForgeEQSLibrary::GetTestTypeAliases();
		const UClass* const* Found = Types.Find(TypeName);
		if (Found == nullptr)
		{
			OutError = FString::Printf(TEXT("unknown test type '%s'. Allowed: Distance, Dot, Trace, Pathfinding, Random, Overlap, Project, Volume, or a '/Script/...'/'/Game/...' class path to a UEnvQueryTest"), *TypeName);
			return nullptr;
		}
		return const_cast<UClass*>(*Found);
	}

	using BTForgeParamSchema::DescribeEnumValues;
	using BTForgeParamSchema::EnumEntryShortName;
	using BTForgeParamSchema::GetJsonPropertyShape;

	bool FindEnumIndex(const UEnum* Enum, const FString& Requested, int32& OutIndex)
	{
		for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
		{
			const FString ShortName = EnumEntryShortName(Enum, Index);
			if (ShortName.Equals(Requested, ESearchCase::IgnoreCase)
				|| Enum->GetNameStringByIndex(Index).Equals(Requested, ESearchCase::IgnoreCase)
				|| Enum->GetDisplayNameTextByIndex(Index).ToString().Equals(Requested, ESearchCase::IgnoreCase))
			{
				OutIndex = Index;
				return true;
			}
		}
		return false;
	}

	/** Error-message adapter: the shared shape helper returns bare text, error
	 *  paths in this file use parenthesised hints. */
	FString PropertyKindHint(const FProperty* Property)
	{
		return FString::Printf(TEXT("(%s)"), *GetJsonPropertyShape(Property));
	}

	/** Names of properties the spec may write, for error messages. */
	FString DescribeEditableParams(UObject* Object)
	{
		FString Out;
		for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
		{
			const FProperty* Property = *It;
			if (Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
			{
				if (!Out.IsEmpty())
				{
					Out += TEXT(", ");
				}
				Out += Property->GetName();
			}
		}
		return Out.IsEmpty() ? TEXT("(none)") : Out;
	}

	void CollectEditableProperties(UObject* Object, TMap<FName, FProperty*>& OutProperties)
	{
		OutProperties.Reset();
		for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
		{
			FProperty* Property = *It;
			if (Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
			{
				OutProperties.Add(Property->GetFName(), Property);
			}
		}
	}

	bool WriteEnumProperty(FProperty* Property, void* DataPtr, const TSharedPtr<FJsonValue>& Value, const FString& Path, FString& OutError)
	{
		const UEnum* Enum = nullptr;
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			Enum = ByteProperty->Enum;
		}
		else if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			Enum = EnumProperty->GetEnum();
		}
		if (Enum == nullptr)
		{
			OutError = FString::Printf(TEXT("%s: expected an enum property, got %s"), *Path, *Property->GetName());
			return false;
		}

		FString EnumName;
		if (Value->TryGetString(EnumName))
		{
			int32 Index = INDEX_NONE;
			if (!FindEnumIndex(Enum, EnumName, Index))
			{
				OutError = FString::Printf(TEXT("%s: unknown value '%s'. Allowed: %s"), *Path, *EnumName, *DescribeEnumValues(Enum));
				return false;
			}
			const int64 NumericValue = Enum->GetValueByIndex(Index);
			if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
			{
				EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(DataPtr, NumericValue);
			}
			else
			{
				CastFieldChecked<FByteProperty>(Property)->SetIntPropertyValue(DataPtr, NumericValue);
			}
			return true;
		}
		double NumericInput = 0.0;
		if (Value->TryGetNumber(NumericInput))
		{
			const int64 NumericValue = static_cast<int64>(NumericInput);
			if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
			{
				EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(DataPtr, NumericValue);
			}
			else
			{
				CastFieldChecked<FByteProperty>(Property)->SetIntPropertyValue(DataPtr, NumericValue);
			}
			return true;
		}
		OutError = FString::Printf(TEXT("%s: expected an enum name (string). Allowed: %s"), *Path, *DescribeEnumValues(Enum));
		return false;
	}

	bool ApplyStructProps(UScriptStruct* Struct, void* DataPtr, const TSharedPtr<FJsonObject>& Object, const FString& Path, FString& OutError);

	bool WriteEQSValue(FProperty* Property, void* DataPtr, const TSharedPtr<FJsonValue>& Value, const FString& Path, FString& OutError)
	{
		const TSharedPtr<FJsonValue> ActualValue = Value;
		if (!ActualValue.IsValid() || ActualValue->IsNull())
		{
			OutError = FString::Printf(TEXT("%s: null is not allowed; %s"), *Path, *PropertyKindHint(Property));
			return false;
		}

		if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			bool bValue = false;
			if (!ActualValue->TryGetBool(bValue))
			{
				OutError = FString::Printf(TEXT("%s: expected true/false"), *Path);
				return false;
			}
			BoolProperty->SetPropertyValue(DataPtr, bValue);
			return true;
		}

		if (CastField<FByteProperty>(Property) != nullptr || CastField<FEnumProperty>(Property) != nullptr)
		{
			return WriteEnumProperty(Property, DataPtr, ActualValue, Path, OutError);
		}

		if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			double ValueNumber = 0.0;
			if (!ActualValue->TryGetNumber(ValueNumber))
			{
				OutError = FString::Printf(TEXT("%s: expected a number"), *Path);
				return false;
			}
			if (NumericProperty->IsFloatingPoint())
			{
				NumericProperty->SetFloatingPointPropertyValue(DataPtr, ValueNumber);
			}
			else
			{
				NumericProperty->SetIntPropertyValue(DataPtr, static_cast<int64>(ValueNumber));
			}
			return true;
		}

		if (const FStrProperty* StrProperty = CastField<FStrProperty>(Property))
		{
			FString StringValue;
			if (!ActualValue->TryGetString(StringValue))
			{
				OutError = FString::Printf(TEXT("%s: expected a string"), *Path);
				return false;
			}
			StrProperty->SetPropertyValue(DataPtr, StringValue);
			return true;
		}

		if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			FString StringValue;
			if (!ActualValue->TryGetString(StringValue))
			{
				OutError = FString::Printf(TEXT("%s: expected a string"), *Path);
				return false;
			}
			NameProperty->SetPropertyValue(DataPtr, FName(*StringValue));
			return true;
		}

		if (const FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
		{
			FString ClassPath;
			if (!ActualValue->TryGetString(ClassPath) || ClassPath.IsEmpty())
			{
				OutError = FString::Printf(TEXT("%s: expected a class path string; %s"), *Path, *PropertyKindHint(Property));
				return false;
			}
			UClass* LoadedClass = LoadEQSClassFromPath(ClassPath, ClassProperty->MetaClass, TEXT("property"), OutError);
			if (LoadedClass == nullptr)
			{
				OutError = FString::Printf(TEXT("%s: %s"), *Path, *OutError);
				return false;
			}
			ClassProperty->SetObjectPropertyValue(DataPtr, LoadedClass);
			return true;
		}

		if (const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
		{
			FString ObjectPath;
			if (!ActualValue->TryGetString(ObjectPath) || ObjectPath.IsEmpty())
			{
				OutError = FString::Printf(TEXT("%s: expected an object path string; %s"), *Path, *PropertyKindHint(Property));
				return false;
			}
			UObject* LoadedObject = StaticLoadObject(ObjectProperty->PropertyClass, nullptr, *ObjectPath);
			if (LoadedObject == nullptr)
			{
				OutError = FString::Printf(TEXT("%s: could not load an object of class %s from '%s'"), *Path,
					*GetNameSafe(ObjectProperty->PropertyClass), *ObjectPath);
				return false;
			}
			ObjectProperty->SetObjectPropertyValue(DataPtr, LoadedObject);
			return true;
		}

		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct == nullptr)
			{
				OutError = FString::Printf(TEXT("%s: struct type is not loaded"), *Path);
				return false;
			}
			// Data-provider wrappers (FAIDataProviderFloatValue etc.) are most
			// ergonomic as bare numbers: write their DefaultValue directly.
			if (FProperty* DefaultProperty = StructProperty->Struct->FindPropertyByName(TEXT("DefaultValue")))
			{
				if (DefaultProperty->IsA<FNumericProperty>() && ActualValue->Type == EJson::Number)
				{
					return WriteEQSValue(DefaultProperty, DefaultProperty->ContainerPtrToValuePtr<void>(DataPtr), ActualValue, Path, OutError);
				}
			}
			const TSharedPtr<FJsonObject>* StructObject = nullptr;
			if (!ActualValue->TryGetObject(StructObject) || !StructObject->IsValid())
			{
				OutError = FString::Printf(TEXT("%s: expected an object (struct %s); %s"), *Path,
					*StructProperty->Struct->GetName(), *PropertyKindHint(Property));
				return false;
			}
			return ApplyStructProps(StructProperty->Struct, DataPtr, *StructObject, Path, OutError);
		}

		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			if (const TArray<TSharedPtr<FJsonValue>>* ArrayValue = nullptr; ActualValue->TryGetArray(ArrayValue) && ArrayValue->Num() == 0)
			{
				return true; // empty array: nothing to write, not an error
			}
			OutError = FString::Printf(TEXT("%s: array parameters are not supported yet (pass no value or an empty array)"), *Path);
			return false;
		}

		OutError = FString::Printf(TEXT("%s: unsupported property type '%s'"), *Path, *Property->GetCPPType());
		return false;
	}

	bool ApplyStructProps(UScriptStruct* Struct, void* DataPtr, const TSharedPtr<FJsonObject>& Object, const FString& Path, FString& OutError)
	{
		TMap<FName, FProperty*> Properties;
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			FProperty* Property = *It;
			if (Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
			{
				Properties.Add(Property->GetFName(), Property);
			}
		}
		for (const TTuple<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
		{
			FProperty** Found = Properties.Find(FName(*Pair.Key));
			if (Found == nullptr)
			{
				FString Allowed;
				for (const TTuple<FName, FProperty*>& Entry : Properties)
				{
					if (!Allowed.IsEmpty())
					{
						Allowed += TEXT(", ");
					}
					Allowed += Entry.Key.ToString();
				}
				OutError = FString::Printf(TEXT("%s.%s: unknown field. Allowed fields of %s: %s"), *Path, *Pair.Key, *Struct->GetName(), Allowed.IsEmpty() ? TEXT("(none)") : *Allowed);
				return false;
			}
			void* PropertyData = (*Found)->ContainerPtrToValuePtr<void>(DataPtr);
			if (!WriteEQSValue(*Found, PropertyData, Pair.Value, FString::Printf(TEXT("%s.%s"), *Path, *Pair.Key), OutError))
			{
				return false;
			}
		}
		return true;
	}

	bool ApplyEQSProps(UObject* Object, const TSharedPtr<FJsonObject>& Params, const FString& Path, FString& OutError)
	{
		if (!Params.IsValid() || Params->Values.Num() == 0)
		{
			return true;
		}
		TMap<FName, FProperty*> Properties;
		CollectEditableProperties(Object, Properties);
		for (const TTuple<FString, TSharedPtr<FJsonValue>>& Pair : Params->Values)
		{
			FProperty** Found = Properties.Find(FName(*Pair.Key));
			if (Found == nullptr)
			{
				OutError = FString::Printf(TEXT("%s.%s: unknown param. Allowed params of %s: %s"), *Path, *Pair.Key,
					*Object->GetClass()->GetName(), *DescribeEditableParams(Object));
				return false;
			}
			void* DataPtr = (*Found)->ContainerPtrToValuePtr<void>(Object);
			if (!WriteEQSValue(*Found, DataPtr, Pair.Value, FString::Printf(TEXT("%s.%s"), *Path, *Pair.Key), OutError))
			{
				return false;
			}
		}
		return true;
	}

	struct FGeneratorSpec
	{
		FString TypeName;
		UClass* Class = nullptr;
		TSharedPtr<FJsonObject> Params;
	};

	struct FTestSpec
	{
		FString TypeName;
		UClass* Class = nullptr;
		TSharedPtr<FJsonObject> Params;
	};

	struct FOptionSpec
	{
		FString OptionName;
		FGeneratorSpec Generator;
		TArray<FTestSpec> Tests;
	};

	bool ParseOptionSpec(const TSharedPtr<FJsonObject>& OptionJson, int32 OptionIndex, FOptionSpec& OutOption, FString& OutError)
	{
		const FString OptionPath = FString::Printf(TEXT("options[%d]"), OptionIndex);
		OptionJson->TryGetStringField(TEXT("optionName"), OutOption.OptionName);

		const TSharedPtr<FJsonObject>* GeneratorJson = nullptr;
		if (!OptionJson->TryGetObjectField(TEXT("generator"), GeneratorJson) || !GeneratorJson->IsValid())
		{
			OutError = FString::Printf(TEXT("%s.generator: expected an object {\"type\":...}"), *OptionPath);
			return false;
		}
		if (!(*GeneratorJson)->TryGetStringField(TEXT("type"), OutOption.Generator.TypeName) || OutOption.Generator.TypeName.IsEmpty())
		{
			OutError = FString::Printf(TEXT("%s.generator.type: missing or empty"), *OptionPath);
			return false;
		}
		OutOption.Generator.Class = ResolveGeneratorClass(OutOption.Generator.TypeName, OutError);
		if (OutOption.Generator.Class == nullptr)
		{
			OutError = FString::Printf(TEXT("%s.generator.type: %s"), *OptionPath, *OutError);
			return false;
		}
		const TSharedPtr<FJsonObject>* ParamsJson = nullptr;
		if ((*GeneratorJson)->TryGetObjectField(TEXT("params"), ParamsJson) && ParamsJson->IsValid())
		{
			OutOption.Generator.Params = *ParamsJson;
		}

		const TArray<TSharedPtr<FJsonValue>>* TestsJson = nullptr;
		if (OptionJson->TryGetArrayField(TEXT("tests"), TestsJson))
		{
			for (int32 TestIndex = 0; TestIndex < TestsJson->Num(); ++TestIndex)
			{
				const TSharedPtr<FJsonObject>* TestJson = nullptr;
				if (!(*TestsJson)[TestIndex]->TryGetObject(TestJson) || !TestJson->IsValid())
				{
					OutError = FString::Printf(TEXT("%s.tests[%d]: expected an object {\"type\":...}"), *OptionPath, TestIndex);
					return false;
				}
				FTestSpec Test;
				if (!(*TestJson)->TryGetStringField(TEXT("type"), Test.TypeName) || Test.TypeName.IsEmpty())
				{
					OutError = FString::Printf(TEXT("%s.tests[%d].type: missing or empty"), *OptionPath, TestIndex);
					return false;
				}
				Test.Class = ResolveTestClass(Test.TypeName, OutError);
				if (Test.Class == nullptr)
				{
					OutError = FString::Printf(TEXT("%s.tests[%d].type: %s"), *OptionPath, TestIndex, *OutError);
					return false;
				}
				if ((*TestJson)->TryGetObjectField(TEXT("params"), ParamsJson) && ParamsJson->IsValid())
				{
					Test.Params = *ParamsJson;
				}
				OutOption.Tests.Add(Test);
			}
		}
		return true;
	}

	bool ParseQuerySpec(const FString& QueryJson, TArray<FOptionSpec>& OutOptions, bool& bOutStripFromClientBuilds, FString& OutError)
	{
		TSharedPtr<FJsonObject> Root;
		{
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(QueryJson);
			if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
			{
				OutError = TEXT("QueryJson is not valid JSON. Expected {\"options\":[...]}");
				return false;
			}
		}
		for (const TTuple<FString, TSharedPtr<FJsonValue>>& Pair : Root->Values)
		{
			if (!Pair.Key.Equals(TEXT("options")) && !Pair.Key.Equals(TEXT("bStripFromClientBuilds")))
			{
				OutError = FString::Printf(TEXT("unknown top-level field '%s'. Allowed: options, bStripFromClientBuilds"), *Pair.Key);
				return false;
			}
		}
		Root->TryGetBoolField(TEXT("bStripFromClientBuilds"), bOutStripFromClientBuilds);

		const TArray<TSharedPtr<FJsonValue>>* OptionsJson = nullptr;
		if (!Root->TryGetArrayField(TEXT("options"), OptionsJson) || OptionsJson->Num() == 0)
		{
			OutError = TEXT("options: required non-empty array of options");
			return false;
		}
		for (int32 OptionIndex = 0; OptionIndex < OptionsJson->Num(); ++OptionIndex)
		{
			const TSharedPtr<FJsonObject>* OptionJson = nullptr;
			if (!(*OptionsJson)[OptionIndex]->TryGetObject(OptionJson) || !OptionJson->IsValid())
			{
				OutError = FString::Printf(TEXT("options[%d]: expected an object"), OptionIndex);
				return false;
			}
			FOptionSpec Option;
			if (!ParseOptionSpec(*OptionJson, OptionIndex, Option, OutError))
			{
				return false;
			}
			OutOptions.Add(Option);
		}
		return true;
	}

	bool BuildQuery(UEnvQuery* Query, const TArray<FOptionSpec>& OptionSpecs, bool bStripFromClientBuilds, FString& OutError)
	{
		if (bStripFromClientBuilds)
		{
			FBoolProperty* StripProperty = FindFProperty<FBoolProperty>(UEnvQuery::StaticClass(), TEXT("bStripFromClientBuilds"));
			if (StripProperty == nullptr)
			{
				OutError = TEXT("bStripFromClientBuilds is not supported by this engine version (5.8+ only)");
				return false;
			}
			StripProperty->SetPropertyValue_InContainer(Query, true);
		}

		Query->GetOptionsMutable().Reset();
		for (int32 OptionIndex = 0; OptionIndex < OptionSpecs.Num(); ++OptionIndex)
		{
			const FOptionSpec& OptionSpec = OptionSpecs[OptionIndex];
			const FString OptionPath = FString::Printf(TEXT("options[%d]"), OptionIndex);

			UEnvQueryOption* Option = NewObject<UEnvQueryOption>(Query);
			UEnvQueryGenerator* Generator = NewObject<UEnvQueryGenerator>(Query, OptionSpec.Generator.Class);
			if (Generator == nullptr)
			{
				OutError = FString::Printf(TEXT("%s.generator: failed to instantiate %s"), *OptionPath, *GetNameSafe(OptionSpec.Generator.Class));
				return false;
			}
			if (!ApplyEQSProps(Generator, OptionSpec.Generator.Params, OptionPath + TEXT(".generator.params"), OutError))
			{
				return false;
			}
			if (!OptionSpec.OptionName.IsEmpty())
			{
				if (FStrProperty* OptionNameProperty = FindFProperty<FStrProperty>(UEnvQueryGenerator::StaticClass(), TEXT("OptionName")))
				{
					OptionNameProperty->SetPropertyValue_InContainer(Generator, OptionSpec.OptionName);
				}
			}
			Generator->UpdateNodeVersion();
			if (!Generator->IsValidGenerator())
			{
				OutError = FString::Printf(TEXT("%s.generator: %s is not a valid generator (missing ItemType)"), *OptionPath, *GetNameSafe(OptionSpec.Generator.Class));
				return false;
			}
			Option->Generator = Generator;

			for (int32 TestIndex = 0; TestIndex < OptionSpec.Tests.Num(); ++TestIndex)
			{
				const FTestSpec& TestSpec = OptionSpec.Tests[TestIndex];
				UEnvQueryTest* Test = NewObject<UEnvQueryTest>(Query, TestSpec.Class);
				if (Test == nullptr)
				{
					OutError = FString::Printf(TEXT("%s.tests[%d]: failed to instantiate %s"), *OptionPath, TestIndex, *GetNameSafe(TestSpec.Class));
					return false;
				}

				// "WorkOnFloatValues" is not a UPROPERTY: it switches the test
				// between bool and float scoring modes (e.g. Pathfinding
				// PathExist vs PathLength) and must be applied via the setter.
				TSharedPtr<FJsonObject> Params = TestSpec.Params;
				bool bHasWorkOnFloatValues = false;
				bool bWorkOnFloatValues = false;
				if (Params.IsValid() && Params->HasTypedField<EJson::Boolean>(TEXT("WorkOnFloatValues")))
				{
					bHasWorkOnFloatValues = true;
					bWorkOnFloatValues = Params->GetBoolField(TEXT("WorkOnFloatValues"));
					TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
					Copy->Values = Params->Values;
					Copy->RemoveField(TEXT("WorkOnFloatValues"));
					Params = Copy;
				}
				if (!ApplyEQSProps(Test, Params, FString::Printf(TEXT("%s.tests[%d].params"), *OptionPath, TestIndex), OutError))
				{
					return false;
				}
				if (bHasWorkOnFloatValues)
				{
					Test->SetWorkOnFloatValues(bWorkOnFloatValues);
				}
				Test->TestOrder = TestIndex;
				Test->UpdateNodeVersion();
				// ItemType is a generator UPROPERTY; read it through reflection so
				// this code does not depend on the member's access level.
				UClass* ItemTypeClass = nullptr;
				if (const FClassProperty* ItemTypeProperty = FindFProperty<FClassProperty>(UEnvQueryGenerator::StaticClass(), TEXT("ItemType")))
				{
					ItemTypeClass = Cast<UClass>(ItemTypeProperty->GetObjectPropertyValue(ItemTypeProperty->ContainerPtrToValuePtr<void>(Generator)));
				}
				TSubclassOf<UEnvQueryItemType> ItemType;
				ItemType = ItemTypeClass;
				if (!Test->IsSupportedItem(ItemType))
				{
					OutError = FString::Printf(TEXT("%s.tests[%d]: %s does not support generator item type %s"), *OptionPath, TestIndex,
						*GetNameSafe(TestSpec.Class), *GetNameSafe(ItemTypeClass));
					return false;
				}
				Option->Tests.Add(Test);
			}
			Query->GetOptionsMutable().Add(Option);
		}
		return true;
	}

	/** Read one editable property as JSON (best effort, for inspection). */
	TSharedPtr<FJsonValue> ReadEQSPropertyAsJson(const FProperty* Property, const void* DataPtr)
	{
		if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			return MakeShared<FJsonValueBoolean>(BoolProperty->GetPropertyValue(DataPtr));
		}
		if (CastField<FByteProperty>(Property) != nullptr || CastField<FEnumProperty>(Property) != nullptr)
		{
			const UEnum* Enum = nullptr;
			int64 NumericValue = 0;
			if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
			{
				Enum = ByteProperty->Enum;
				NumericValue = static_cast<int64>(ByteProperty->GetUnsignedIntPropertyValue(DataPtr));
			}
			else
			{
				const FEnumProperty* EnumProperty = CastFieldChecked<FEnumProperty>(Property);
				Enum = EnumProperty->GetEnum();
				const FNumericProperty* Underlying = EnumProperty->GetUnderlyingProperty();
				NumericValue = Underlying->IsA<FByteProperty>()
					? static_cast<int64>(Underlying->GetUnsignedIntPropertyValue(DataPtr))
					: Underlying->GetSignedIntPropertyValue(DataPtr);
			}
			if (Enum != nullptr)
			{
				const int32 Index = Enum->GetIndexByValue(NumericValue);
				if (Index != INDEX_NONE)
				{
					return MakeShared<FJsonValueString>(EnumEntryShortName(Enum, Index));
				}
			}
			return MakeShared<FJsonValueNumber>(static_cast<double>(NumericValue));
		}
		if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			const double Value = NumericProperty->IsFloatingPoint()
				? NumericProperty->GetFloatingPointPropertyValue(DataPtr)
				: static_cast<double>(NumericProperty->GetSignedIntPropertyValue(DataPtr));
			return MakeShared<FJsonValueNumber>(Value);
		}
		if (const FStrProperty* StrProperty = CastField<FStrProperty>(Property))
		{
			return MakeShared<FJsonValueString>(StrProperty->GetPropertyValue(DataPtr));
		}
		if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			return MakeShared<FJsonValueString>(NameProperty->GetPropertyValue(DataPtr).ToString());
		}
		if (const FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
		{
			if (const UObject* Value = ClassProperty->GetObjectPropertyValue(DataPtr))
			{
				return MakeShared<FJsonValueString>(Value->GetPathName());
			}
			return nullptr;
		}
		if (const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
		{
			if (const UObject* Value = ObjectProperty->GetObjectPropertyValue(DataPtr))
			{
				return MakeShared<FJsonValueString>(Value->GetPathName());
			}
			return nullptr;
		}
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct == nullptr)
			{
				return nullptr;
			}
			if (const FProperty* DefaultProperty = StructProperty->Struct->FindPropertyByName(TEXT("DefaultValue")))
			{
				if (DefaultProperty->IsA<FNumericProperty>())
				{
					return ReadEQSPropertyAsJson(DefaultProperty, DefaultProperty->ContainerPtrToValuePtr<void>(DataPtr));
				}
			}
			return nullptr;
		}
		return nullptr;
	}

	TSharedRef<FJsonObject> DumpParams(UObject* Object, bool bIncludeAllEditable)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
		{
			const FProperty* Property = *It;
			if (bIncludeAllEditable && !Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
			{
				continue;
			}
			const void* DataPtr = Property->ContainerPtrToValuePtr<void>(Object);
			TSharedPtr<FJsonValue> Value = ReadEQSPropertyAsJson(Property, DataPtr);
			if (Value.IsValid())
			{
				Params->SetField(Property->GetName(), Value);
			}
		}
		return Params;
	}
}

FString UBTForgeEQSLibrary::CreateEQSAsset(const FString& AssetPath, const FString& QueryJson)
{
	FString PackagePath;
	FString AssetName;
	if (!SplitAssetPath(AssetPath, PackagePath, AssetName))
	{
		return MakeErrorJson(TEXT("AssetPath must look like /Game/<Folder>/<Name>, got: ") + AssetPath);
	}
	if (FindObject<UEnvQuery>(nullptr, *AssetPath) != nullptr)
	{
		return MakeErrorJson(TEXT("EQS asset already exists: ") + AssetPath + TEXT(". Use InspectEQSAsset to verify it, or pick a new path."));
	}
	if (FPackageName::DoesPackageExist(AssetPath))
	{
		return MakeErrorJson(TEXT("EQS asset already exists on disk: ") + AssetPath);
	}

	TArray<FOptionSpec> OptionSpecs;
	bool bStripFromClientBuilds = false;
	FString Error;
	if (!ParseQuerySpec(QueryJson, OptionSpecs, bStripFromClientBuilds, Error))
	{
		return MakeErrorJson(Error);
	}

	// Validate the whole query against transient objects first: class loading,
	// parameter writing, item-type compatibility. Nothing is created on failure.
	{
		UEnvQuery* TransientQuery = NewObject<UEnvQuery>(GetTransientPackage());
		if (!BuildQuery(TransientQuery, OptionSpecs, bStripFromClientBuilds, Error))
		{
			return MakeErrorJson(Error);
		}
	}

	FAssetToolsModule& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	UEnvQuery* Query = Cast<UEnvQuery>(AssetTools.Get().CreateAsset(AssetName, PackagePath, UEnvQuery::StaticClass(), nullptr));
	if (Query == nullptr)
	{
		return MakeErrorJson(TEXT("Failed to create EQS asset at: ") + AssetPath);
	}

	UEditorAssetSubsystem* AssetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
	if (!BuildQuery(Query, OptionSpecs, bStripFromClientBuilds, Error))
	{
		if (AssetSubsystem != nullptr)
		{
			AssetSubsystem->DeleteAsset(AssetPath);
		}
		return MakeErrorJson(Error);
	}
	if (AssetSubsystem == nullptr)
	{
		return MakeErrorJson(TEXT("EditorAssetSubsystem unavailable; asset created but not saved: ") + AssetPath);
	}

	Query->MarkPackageDirty();
	if (!AssetSubsystem->SaveLoadedAsset(Query, false))
	{
		return MakeErrorJson(TEXT("EQS query built in memory but saving failed: ") + AssetPath);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("asset"), AssetPath);
	Result->SetStringField(TEXT("queryName"), Query->GetQueryName().ToString());
	Result->SetNumberField(TEXT("optionCount"), Query->GetOptions().Num());
	TArray<TSharedPtr<FJsonValue>> Options;
	for (const UEnvQueryOption* Option : Query->GetOptions())
	{
		TSharedRef<FJsonObject> OptionResult = MakeShared<FJsonObject>();
		if (Option == nullptr || Option->Generator == nullptr)
		{
			continue;
		}
		OptionResult->SetStringField(TEXT("generator"), Option->Generator->GetClass()->GetName());
		TArray<TSharedPtr<FJsonValue>> Tests;
		for (const UEnvQueryTest* Test : Option->Tests)
		{
			if (Test != nullptr)
			{
				Tests.Add(MakeShared<FJsonValueString>(Test->GetClass()->GetName()));
			}
		}
		OptionResult->SetArrayField(TEXT("tests"), Tests);
		Options.Add(MakeShared<FJsonValueObject>(OptionResult));
	}
	Result->SetArrayField(TEXT("options"), Options);
	return JsonToString(Result.ToSharedRef());
}

FString UBTForgeEQSLibrary::InspectEQSAsset(const FString& AssetPath)
{
	UEnvQuery* Query = LoadObject<UEnvQuery>(nullptr, *AssetPath);
	if (Query == nullptr)
	{
		return MakeErrorJson(TEXT("EQS asset not found: ") + AssetPath);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("asset"), AssetPath);
	Result->SetStringField(TEXT("queryName"), Query->GetQueryName().ToString());
	if (const FBoolProperty* StripProperty = FindFProperty<FBoolProperty>(UEnvQuery::StaticClass(), TEXT("bStripFromClientBuilds")))
	{
		Result->SetBoolField(TEXT("bStripFromClientBuilds"), StripProperty->GetPropertyValue_InContainer(Query));
	}

	TArray<TSharedPtr<FJsonValue>> Options;
	for (const UEnvQueryOption* Option : Query->GetOptions())
	{
		if (Option == nullptr || Option->Generator == nullptr)
		{
			continue;
		}
		TSharedRef<FJsonObject> OptionResult = MakeShared<FJsonObject>();
		FString OptionName;
		if (const FStrProperty* OptionNameProperty = FindFProperty<FStrProperty>(UEnvQueryGenerator::StaticClass(), TEXT("OptionName")))
		{
			OptionName = OptionNameProperty->GetPropertyValue_InContainer(Option->Generator);
		}
		OptionResult->SetStringField(TEXT("optionName"), OptionName);
		TSharedRef<FJsonObject> Generator = MakeShared<FJsonObject>();
		Generator->SetStringField(TEXT("type"), Option->Generator->GetClass()->GetName());
		Generator->SetObjectField(TEXT("params"), DumpParams(Option->Generator, true));
		OptionResult->SetObjectField(TEXT("generator"), Generator);

		TArray<TSharedPtr<FJsonValue>> Tests;
		for (const UEnvQueryTest* Test : Option->Tests)
		{
			if (Test == nullptr)
			{
				continue;
			}
			TSharedRef<FJsonObject> TestResult = MakeShared<FJsonObject>();
			TestResult->SetStringField(TEXT("type"), Test->GetClass()->GetName());
			TestResult->SetNumberField(TEXT("testOrder"), Test->TestOrder);
			TestResult->SetObjectField(TEXT("params"), DumpParams(const_cast<UEnvQueryTest*>(Test), true));
			Tests.Add(MakeShared<FJsonValueObject>(TestResult));
		}
		OptionResult->SetArrayField(TEXT("tests"), Tests);
		Options.Add(MakeShared<FJsonValueObject>(OptionResult));
	}
	Result->SetArrayField(TEXT("options"), Options);
	return JsonToString(Result.ToSharedRef());
}

FString UBTForgeEQSLibrary::ValidateEQSSpec(const FString& QueryJson)
{
	TArray<FOptionSpec> OptionSpecs;
	bool bStripFromClientBuilds = false;
	FString Error;
	if (!ParseQuerySpec(QueryJson, OptionSpecs, bStripFromClientBuilds, Error))
	{
		return MakeErrorJson(Error);
	}
	UEnvQuery* TransientQuery = NewObject<UEnvQuery>(GetTransientPackage());
	if (!BuildQuery(TransientQuery, OptionSpecs, bStripFromClientBuilds, Error))
	{
		return MakeErrorJson(Error);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetNumberField(TEXT("optionCount"), TransientQuery->GetOptions().Num());
	TArray<TSharedPtr<FJsonValue>> Options;
	for (const UEnvQueryOption* Option : TransientQuery->GetOptions())
	{
		if (Option == nullptr || Option->Generator == nullptr)
		{
			continue;
		}
		TSharedRef<FJsonObject> OptionResult = MakeShared<FJsonObject>();
		OptionResult->SetStringField(TEXT("generator"), Option->Generator->GetClass()->GetName());
		TArray<TSharedPtr<FJsonValue>> Tests;
		for (const UEnvQueryTest* Test : Option->Tests)
		{
			if (Test != nullptr)
			{
				Tests.Add(MakeShared<FJsonValueString>(Test->GetClass()->GetName()));
			}
		}
		OptionResult->SetArrayField(TEXT("tests"), Tests);
		Options.Add(MakeShared<FJsonValueObject>(OptionResult));
	}
	Result->SetArrayField(TEXT("options"), Options);
	return JsonToString(Result.ToSharedRef());
}

const TMap<FString, UClass*>& UBTForgeEQSLibrary::GetGeneratorTypeAliases()
{
	static const TMap<FString, UClass*> Types =
	{
		{ TEXT("SimpleGrid"),       UEnvQueryGenerator_SimpleGrid::StaticClass() },
		{ TEXT("CurrentLocation"),  UEnvQueryGenerator_CurrentLocation::StaticClass() },
		{ TEXT("OnCircle"),         UEnvQueryGenerator_OnCircle::StaticClass() },
		{ TEXT("Donut"),            UEnvQueryGenerator_Donut::StaticClass() },
		{ TEXT("Cone"),             UEnvQueryGenerator_Cone::StaticClass() },
		{ TEXT("PathingGrid"),      UEnvQueryGenerator_PathingGrid::StaticClass() },
		{ TEXT("ActorsOfClass"),    UEnvQueryGenerator_ActorsOfClass::StaticClass() },
		{ TEXT("PerceivedActors"),  UEnvQueryGenerator_PerceivedActors::StaticClass() },
	};
	return Types;
}

const TMap<FString, UClass*>& UBTForgeEQSLibrary::GetTestTypeAliases()
{
	static const TMap<FString, UClass*> Types =
	{
		{ TEXT("Distance"),    UEnvQueryTest_Distance::StaticClass() },
		{ TEXT("Dot"),         UEnvQueryTest_Dot::StaticClass() },
		{ TEXT("Trace"),       UEnvQueryTest_Trace::StaticClass() },
		{ TEXT("Pathfinding"), UEnvQueryTest_Pathfinding::StaticClass() },
		{ TEXT("Random"),      UEnvQueryTest_Random::StaticClass() },
		{ TEXT("Overlap"),     UEnvQueryTest_Overlap::StaticClass() },
		{ TEXT("Project"),     UEnvQueryTest_Project::StaticClass() },
		{ TEXT("Volume"),      UEnvQueryTest_Volume::StaticClass() },
	};
	return Types;
}
