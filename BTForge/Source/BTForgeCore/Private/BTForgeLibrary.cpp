#include "BTForgeLibrary.h"

#include "BTForgeBehaviorNodes.h"
#include "AssetToolsModule.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/Decorators/BTDecorator_Blackboard.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/Services/BTService_RunEQS.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/Items/EnvQueryItemType_ActorBase.h"
#include "EnvironmentQuery/Items/EnvQueryItemType_VectorBase.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Tasks/BTTask_MoveTo.h"
#include "BehaviorTree/Tasks/BTTask_RunEQSQuery.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystems/AssetEditorSubsystem.h"
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

	/** "/Game/AI/BT_X" -> package "/Game/AI", name "BT_X". */
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

	UClass* ResolveBlackboardKeyType(const FString& TypeName)
	{
		static const TMap<FString, UClass*> Types =
		{
			{ TEXT("Bool"),    UBlackboardKeyType_Bool::StaticClass() },
			{ TEXT("Int"),     UBlackboardKeyType_Int::StaticClass() },
			{ TEXT("Float"),   UBlackboardKeyType_Float::StaticClass() },
			{ TEXT("String"),  UBlackboardKeyType_String::StaticClass() },
			{ TEXT("Name"),    UBlackboardKeyType_Name::StaticClass() },
			{ TEXT("Vector"),  UBlackboardKeyType_Vector::StaticClass() },
			{ TEXT("Rotator"), UBlackboardKeyType_Rotator::StaticClass() },
			{ TEXT("Object"),  UBlackboardKeyType_Object::StaticClass() },
		};
		UClass* const* Found = Types.Find(TypeName);
		return Found ? *Found : nullptr;
	}

	/** Resolve a class from a short name ("LyraCharacter") or a full object path
	 *  ("/Script/LyraGame.LyraCharacter", "/Game/.../BP_X.BP_X_C"). Deterministic:
	 *  fails (nullptr) when nothing by that name is loaded/loadable. */
	UClass* ResolveClassLoose(const FString& Text)
	{
		if (Text.IsEmpty())
		{
			return nullptr;
		}
		if (Text.Contains(TEXT("/")))
		{
			return StaticLoadClass(UObject::StaticClass(), nullptr, *Text);
		}
		return FindFirstObject<UClass>(*Text);
	}

	/** Resolve a "/path/Asset.Asset_C" class reference (engine or blueprint) for a
	 *  BT node slot. Blueprint classes are how projects carry their own node
	 *  types (e.g. Lyra's BTS_Shoot_C); params still flow through the generic
	 *  reflection writers, so admitting a class only requires base-class and
	 *  abstractness checks here. */
	FString WhitelistText();
	FString AuxWhitelistText(const FString& Kind);

	UClass* LoadBTClassFromPath(const FString& Path, UClass* ExpectedBase, const FString& KindName, FString& OutError)
	{
		UObject* Loaded = StaticLoadObject(UClass::StaticClass(), nullptr, *Path);
		UClass* Class = Cast<UClass>(Loaded);
		if (Class == nullptr)
		{
			OutError = FString::Printf(TEXT("could not load a %s class from '%s'. Use an object path like /Game/Folder/Asset.Asset_C (blueprint) or /Script/Module.ClassName (native)"), *KindName, *Path);
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

	UClass* ResolveNodeType(const FString& TypeName, FString& OutError)
	{
		if (TypeName.StartsWith(TEXT("/")))
		{
			UClass* Class = LoadBTClassFromPath(TypeName, UBTNode::StaticClass(), TEXT("node"), OutError);
			if (Class != nullptr && !Class->IsChildOf(UBTCompositeNode::StaticClass()) && !Class->IsChildOf(UBTTaskNode::StaticClass()))
			{
				OutError = FString::Printf(TEXT("'%s' is a %s; tree nodes must derive from UBTCompositeNode or UBTTaskNode"), *TypeName, *Class->GetName());
				return nullptr;
			}
			return Class;
		}
		const UClass* const* Found = UBTForgeLibrary::GetNodeTypeAliases().Find(TypeName);
		if (Found == nullptr)
		{
			OutError = FString::Printf(TEXT("unknown type '%s'. Allowed types: %s"), *TypeName, *WhitelistText());
			return nullptr;
		}
		return const_cast<UClass*>(*Found);
	}

	/** Whitelist for auxiliary node types: services (attach to composites) and
	 *  decorators (attach to child slots). Empty DecoratorOps = AND semantics,
	 *  which is the engine's native behavior for ops-less decorator lists. */
	UClass* ResolveAuxType(const FString& Kind, const FString& TypeName, FString& OutError)
	{
		if (TypeName.StartsWith(TEXT("/")))
		{
			const UClass* ExpectedBase = Kind == TEXT("decorator") ? UBTDecorator::StaticClass() : UBTService::StaticClass();
			return LoadBTClassFromPath(TypeName, const_cast<UClass*>(ExpectedBase), Kind, OutError);
		}
		const TMap<FString, UClass*>& Table = UBTForgeLibrary::GetAuxTypeAliases(Kind);
		const UClass* const* Found = Table.Find(TypeName);
		if (Found == nullptr)
		{
			OutError = FString::Printf(TEXT("unknown %s type '%s'. Allowed %s"), *Kind, *TypeName, *AuxWhitelistText(Kind));
			return nullptr;
		}
		return const_cast<UClass*>(*Found);
	}

	FString AuxWhitelistText(const FString& Kind)
	{
		return Kind == TEXT("decorator")
			? TEXT("decorators: Blackboard, FloatCompare, DistanceCheck, Chance, or a '/path/Asset.Asset_C' class path (any non-abstract UBTDecorator-derived class). Multiple decorators on one child combine as AND")
			: TEXT("services: RunEQS, SetValue, UpdateDistance, or a '/path/Asset.Asset_C' class path (any non-abstract UBTService-derived class)");
	}

	FString WhitelistText()
	{
		return TEXT("Sequence, Selector, Wait, MoveTo, RunEQS, ComputeValue, Log, or a '/path/Asset.Asset_C' class path (any non-abstract UBTCompositeNode/UBTTaskNode-derived class, engine or blueprint)");
	}

	/** Keys a fresh UBlackboardData already carries. UBlackboardData::PostInitProperties
	 *  calls UpdatePersistentKeys, which adds SelfActor unless the "add blackboard self
	 *  key" AI setting is off, so appending a spec key with the same name produced a
	 *  duplicated entry: GetKeyID resolved to the built-in one and the duplicate showed
	 *  up in every inspect while its type was ignored. */
	void CollectExistingBlackboardKeys(const UBlackboardData* Blackboard, TSet<FName>& OutNames)
	{
		OutNames.Reset();
		if (Blackboard == nullptr)
		{
			return;
		}
		for (const FBlackboardEntry& Entry : Blackboard->Keys)
		{
			OutNames.Add(Entry.EntryName);
		}
	}

	FString ExistingBlackboardKeyError(int32 KeyIndex, const FString& KeyName, const TSet<FName>& Existing)
	{
		TArray<FString> Names;
		Names.Reserve(Existing.Num());
		for (const FName& Name : Existing)
		{
			Names.Add(Name.ToString());
		}
		Names.Sort();
		return FString::Printf(
			TEXT("keys[%d] ('%s'): this key already exists on every blackboard (UBlackboardData adds %s on creation); "
				"remove it from the spec - nodes can still reference it by name"),
			KeyIndex, *KeyName, *FString::Join(Names, TEXT(", ")));
	}

	/** Type-checked optional field access. UE's GetObjectField/GetArrayField only log a
	 *  warning and hand back nullptr / an empty array on a type mismatch, so "params":"x"
	 *  used to become "no params" and the build still reported ok:true. Absent stays absent. */
	bool TryGetSpecObject(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const FString& Path,
		TSharedPtr<FJsonObject>& Out, FString& OutError)
	{
		Out.Reset();
		if (!Object.IsValid() || !Object->HasField(Field))
		{
			return true;
		}
		const TSharedPtr<FJsonObject>* Value = nullptr;
		if (!Object->TryGetObjectField(Field, Value) || Value == nullptr || !Value->IsValid())
		{
			OutError = FString::Printf(TEXT("%s.%s: expected an object"), *Path, Field);
			return false;
		}
		Out = *Value;
		return true;
	}

	bool TryGetSpecArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const FString& Path,
		TArray<TSharedPtr<FJsonValue>>& Out, FString& OutError)
	{
		Out.Reset();
		if (!Object.IsValid() || !Object->HasField(Field))
		{
			return true;
		}
		const TArray<TSharedPtr<FJsonValue>>* Value = nullptr;
		if (!Object->TryGetArrayField(Field, Value) || Value == nullptr)
		{
			OutError = FString::Printf(TEXT("%s.%s: expected an array"), *Path, Field);
			return false;
		}
		Out = *Value;
		return true;
	}

	/** Describe one property's expected value shape, for error messages. */
	FString ParamKindHint(const FProperty* Property)
	{
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct->FindPropertyByName(TEXT("DefaultValue")))
			{
				return TEXT("(number)");
			}
			if (StructProperty->Struct->FindPropertyByName(TEXT("SelectedKeyName")))
			{
				return TEXT("(blackboard key name)");
			}
			return TEXT("(object)");
		}
		if (Property->IsA<FArrayProperty>())
		{
			return TEXT("(array)");
		}
		if (const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
		{
			return FString::Printf(TEXT("(asset path of %s)"), *ObjectProperty->PropertyClass->GetName());
		}
		const UEnum* EnumDef = nullptr;
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			EnumDef = ByteProperty->Enum;
		}
		else if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			EnumDef = EnumProperty->GetEnum();
		}
		if (EnumDef != nullptr)
		{
			TArray<FString> ValueNames;
			for (int32 Index = 0; Index < EnumDef->NumEnums(); Index++)
			{
				FString ShortName = EnumDef->GetNameStringByValue(Index);
				int32 ScopeIndex;
				if (ShortName.FindLastChar(TEXT(':'), ScopeIndex))
				{
					ShortName = ShortName.RightChop(ScopeIndex + 1);
				}
				ValueNames.Add(ShortName);
			}
			return TEXT("(enum: ") + FString::Join(ValueNames, TEXT("|")) + TEXT(")");
		}
		return TEXT("");
	}

	/** List settable property names with value-shape hints, for error messages. */
	FString AllowedParamsList(UStruct* Struct)
	{
		TArray<FString> Names;
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			const FProperty* Property = *It;
			const bool bScalar = Property->IsA<FNumericProperty>() || Property->IsA<FBoolProperty>() ||
				Property->IsA<FNameProperty>() || Property->IsA<FStrProperty>();
			const FString Hint = ParamKindHint(Property);
			if (bScalar || !Hint.IsEmpty())
			{
				Names.Add(Property->GetName() + Hint);
			}
		}
		return Names.Num() ? FString::Join(Names, TEXT(", ")) : TEXT("(none)");
	}

	bool ApplyParamsToContainer(UStruct* Struct, void* DataPtr, const TSharedPtr<FJsonObject>& Params, const TSet<FName>* KnownBlackboardKeys, const FString& Path, FString& OutError);

	bool WriteValueToProperty(FProperty* Property, void* DataPtr, const TSharedPtr<FJsonValue>& Value, const TSet<FName>* KnownBlackboardKeys, const FString& ParamPath, FString& OutError)
	{
		// Enum-typed properties first: FByteProperty is a numeric property, but
		// it must also accept the enum value NAME (string), not only numbers.
		const UEnum* EnumDef = nullptr;
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			EnumDef = ByteProperty->Enum;
		}
		else if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			EnumDef = EnumProperty->GetEnum();
		}
		if (EnumDef != nullptr)
		{
			int64 EnumValue = INDEX_NONE;
			double AsNumber = 0.0;
			FString AsString;
			if (Value->TryGetNumber(AsNumber))
			{
				EnumValue = static_cast<int64>(AsNumber);
			}
			else if (Value->TryGetString(AsString))
			{
				int32 ScopeIndex;
				if (AsString.FindLastChar(TEXT(':'), ScopeIndex))
				{
					AsString = AsString.RightChop(ScopeIndex + 1);
				}
				EnumValue = EnumDef->GetValueByName(FName(*AsString));
			}
			if (EnumValue == INDEX_NONE || !EnumDef->IsValidEnumValue(EnumValue))
			{
				OutError = TEXT("unknown enum value. ") + ParamKindHint(Property);
				return false;
			}
			if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
			{
				EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(DataPtr, EnumValue);
			}
			else
			{
				static_cast<const FNumericProperty*>(Property)->SetIntPropertyValue(DataPtr, EnumValue);
			}
			return true;
		}
		if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
		{
			double Number = 0.0;
			if (!Value->TryGetNumber(Number))
			{
				OutError = TEXT("expected a number");
				return false;
			}
			if (Numeric->IsFloatingPoint())
			{
				Numeric->SetFloatingPointPropertyValue(DataPtr, Number);
			}
			else
			{
				Numeric->SetIntPropertyValue(DataPtr, static_cast<int64>(Number));
			}
			return true;
		}
		if (const FBoolProperty* Bool = CastField<FBoolProperty>(Property))
		{
			bool bValue = false;
			if (!Value->TryGetBool(bValue))
			{
				OutError = TEXT("expected a bool");
				return false;
			}
			Bool->SetPropertyValue(DataPtr, bValue);
			return true;
		}
		if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			FString String;
			if (!Value->TryGetString(String))
			{
				OutError = TEXT("expected a string");
				return false;
			}
			NameProperty->SetPropertyValue(DataPtr, FName(*String));
			return true;
		}
		if (const FStrProperty* StringProperty = CastField<FStrProperty>(Property))
		{
			FString String;
			if (!Value->TryGetString(String))
			{
				OutError = TEXT("expected a string");
				return false;
			}
			StringProperty->SetPropertyValue(DataPtr, String);
			return true;
		}
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			// DataPtr is the property VALUE address - for a struct property that
			// is already the struct data start; do NOT apply the property offset
			// a second time through ContainerPtrToValuePtr here.
			// FValueOrBBKey_* family: a JSON object selects per member (e.g.
			// {"Key": "SomeKey"} for a key-bound value); a scalar writes the
			// nested DefaultValue.
			if (FProperty* DefaultProperty = StructProperty->Struct->FindPropertyByName(TEXT("DefaultValue")))
			{
				const TSharedPtr<FJsonObject>* SourceObject;
				if (Value->TryGetObject(SourceObject) && SourceObject->IsValid())
				{
					return ApplyParamsToContainer(StructProperty->Struct, DataPtr, SourceObject->ToSharedRef(), KnownBlackboardKeys, ParamPath, OutError);
				}
				void* DefaultDataPtr = DefaultProperty->ContainerPtrToValuePtr<void>(DataPtr);
				return WriteValueToProperty(DefaultProperty, DefaultDataPtr, Value, KnownBlackboardKeys, ParamPath, OutError);
			}
			// FBlackboardKeySelector: accept the key NAME; KeyID resolution happens
			// at runtime against the blackboard, so storing the name is sufficient.
			if (const FNameProperty* KeyNameProperty = CastField<FNameProperty>(StructProperty->Struct->FindPropertyByName(TEXT("SelectedKeyName"))))
			{
				FString KeyName;
				if (!Value->TryGetString(KeyName))
				{
					OutError = TEXT("expected a blackboard key name (string)");
					return false;
				}
				// KnownBlackboardKeys == nullptr means CDO-level params (blueprint node
			// creation): there is no tree to validate against, accept the bare name.
			if (KnownBlackboardKeys != nullptr && !KnownBlackboardKeys->Contains(FName(*KeyName)))
				{
					TArray<FString> Known;
					for (const FName& Known2 : *KnownBlackboardKeys)
					{
						Known.Add(Known2.ToString());
					}
					OutError = TEXT("unknown blackboard key '") + KeyName + TEXT("'. Keys on this tree's blackboard: ") + (Known.Num() ? FString::Join(Known, TEXT(", ")) : FString(TEXT("(none)")));
					return false;
				}
				KeyNameProperty->SetPropertyValue(KeyNameProperty->ContainerPtrToValuePtr<void>(DataPtr), FName(*KeyName));
				return true;
			}
			// Plain nested struct (e.g. FEQSParametrizedQueryExecutionRequest):
			// accept an object and apply it member by member.
			const TSharedPtr<FJsonObject>* NestedObject;
			if (Value->TryGetObject(NestedObject) && NestedObject->IsValid())
			{
				return ApplyParamsToContainer(StructProperty->Struct, DataPtr, NestedObject->ToSharedRef(), KnownBlackboardKeys, ParamPath, OutError);
			}
			OutError = TEXT("expected an object for struct property");
			return false;
		}
		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (!Value->TryGetArray(Items))
			{
				OutError = TEXT("expected an array");
				return false;
			}
			FScriptArrayHelper Helper(ArrayProperty, DataPtr);
			Helper.EmptyValues();
			for (int32 Index = 0; Index < Items->Num(); Index++)
			{
				// AddValues constructs each element (struct C++ defaults apply).
				const int32 ItemIndex = Helper.AddValues(1);
				void* ItemPtr = Helper.GetRawPtr(ItemIndex);
				const FString ItemPath = FString::Printf(TEXT("%s[%d]"), *ParamPath, Index);
				const TSharedPtr<FJsonObject>* ItemObject;
				if ((*Items)[Index]->TryGetObject(ItemObject) && ItemObject->IsValid())
				{
					if (const FStructProperty* InnerStruct = CastField<FStructProperty>(ArrayProperty->Inner))
					{
						if (!ApplyParamsToContainer(InnerStruct->Struct, ItemPtr, ItemObject->ToSharedRef(), KnownBlackboardKeys, ItemPath, OutError))
						{
							return false;
						}
						continue;
					}
				}
				if (!WriteValueToProperty(ArrayProperty->Inner, ItemPtr, (*Items)[Index], KnownBlackboardKeys, ItemPath, OutError))
				{
					return false;
				}
			}
			return true;
		}
		// Strong object reference (e.g. EQSRequest.QueryTemplate): accept an asset
		// path, load it and verify the type matches the property's slot.
		if (Property->IsA<FClassProperty>())
		{
			OutError = TEXT("class references are not supported yet");
			return false;
		}
		if (const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
		{
			FString AssetPath;
			if (!Value->TryGetString(AssetPath))
			{
				OutError = TEXT("expected an asset path (string), e.g. /Game/Folder/Asset.Asset");
				return false;
			}
			UObject* Loaded = LoadObject<UObject>(nullptr, *AssetPath);
			if (Loaded == nullptr)
			{
				OutError = TEXT("asset not found: ") + AssetPath;
				return false;
			}
			if (!Loaded->IsA(ObjectProperty->PropertyClass))
			{
				OutError = FString::Printf(TEXT("asset %s is a %s, expected a %s"), *AssetPath, *Loaded->GetClass()->GetName(), *ObjectProperty->PropertyClass->GetName());
				return false;
			}
			ObjectProperty->SetObjectPropertyValue(DataPtr, Loaded);
			return true;
		}
		OutError = TEXT("unsupported property type: ") + Property->GetClass()->GetName();
		return false;
	}

	bool ApplyParamsToContainer(UStruct* Struct, void* DataPtr, const TSharedPtr<FJsonObject>& Params, const TSet<FName>* KnownBlackboardKeys, const FString& Path, FString& OutError)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Params->Values)
		{
			FProperty* Property = Struct->FindPropertyByName(FName(*Pair.Key));
			if (Property == nullptr)
			{
				OutError = FString::Printf(TEXT("%s: unknown param '%s'. Allowed params: %s"),
					*Path, *Pair.Key, *AllowedParamsList(Struct));
				return false;
			}
			void* PropertyDataPtr = Property->ContainerPtrToValuePtr<void>(DataPtr);
			const FString ParamPath = Path + TEXT(".") + Pair.Key;
			FString SubError;
			if (!WriteValueToProperty(Property, PropertyDataPtr, Pair.Value, KnownBlackboardKeys, ParamPath, SubError))
			{
				// Errors from nested containers already carry their full path.
				OutError = SubError.StartsWith(ParamPath)
					? FString::Printf(TEXT("%s: %s"), *Path, *SubError)
					: FString::Printf(TEXT("%s: %s: %s"), *Path, *ParamPath, *SubError);
				return false;
			}
		}
		return true;
	}

	/** A query option's item type can only be stored into a blackboard key of the
	 *  matching kind: actor items -> Object keys, point/vector items -> Vector keys.
	 *  UEnvQueryItemType_ActorBase / VectorBase::StoreInBlackboard compare the exact
	 *  key class and return false otherwise - and that runtime failure is only
	 *  visible in the visual log (UE_VLOG), never in logErrors. Reject the pair
	 *  here instead. */
	bool IsItemStoreableIntoKey(const UClass* ItemTypeClass, const UClass* KeyTypeClass)
	{
		if (ItemTypeClass == nullptr || KeyTypeClass == nullptr)
		{
			return false;
		}
		if (ItemTypeClass->IsChildOf<UEnvQueryItemType_ActorBase>())
		{
			return KeyTypeClass == UBlackboardKeyType_Object::StaticClass();
		}
		if (ItemTypeClass->IsChildOf<UEnvQueryItemType_VectorBase>())
		{
			return KeyTypeClass == UBlackboardKeyType_Vector::StaticClass();
		}
		return false;
	}

	/** Binding a blackboard key on the RunEQS task means wanting the result, so let
	 *  bUseBBKey follow the binding unless the spec set it explicitly. The SERVICE
	 *  variant (UBTService_RunEQS) has no such flag - it stores unconditionally. */
	void AutoSetRunEQSUseBBKey(UClass* NodeClass, const TSharedPtr<FJsonObject>& Params)
	{
		if (NodeClass->IsChildOf<UBTTask_RunEQSQuery>()
			&& Params->HasField(TEXT("BlackboardKey")) && !Params->HasField(TEXT("bUseBBKey")))
		{
			Params->SetBoolField(TEXT("bUseBBKey"), true);
		}
	}

	/** After params are applied on a RunEQS node with a bound key, check that the
	 *  query's item type can actually be stored into that key (see
	 *  IsItemStoreableIntoKey). Catches "EQS_FindTarget returns Actor items;
	 *  MoveGoal is a Vector key" at build time instead of the silent runtime drop. */
	bool ValidateRunEQSKeyTypes(UObject* Node, const UBlackboardData* BlackboardAsset, const FString& NodePath, FString& OutError)
	{
		if (BlackboardAsset == nullptr
			|| (!Node->IsA<UBTTask_RunEQSQuery>() && !Node->IsA<UBTService_RunEQS>()))
		{
			return true;
		}
		FStructProperty* SelectorProperty = CastField<FStructProperty>(Node->GetClass()->FindPropertyByName(TEXT("BlackboardKey")));
		FBoolProperty* UseBBKeyProperty = CastField<FBoolProperty>(Node->GetClass()->FindPropertyByName(TEXT("bUseBBKey")));
		FStructProperty* RequestProperty = CastField<FStructProperty>(Node->GetClass()->FindPropertyByName(TEXT("EQSRequest")));
		if (SelectorProperty == nullptr || UseBBKeyProperty == nullptr || RequestProperty == nullptr)
		{
			return true; // not the expected shape; nothing to check
		}
		if (!UseBBKeyProperty->GetPropertyValue(UseBBKeyProperty->ContainerPtrToValuePtr<void>(Node)))
		{
			return true; // result is not stored; the pair is irrelevant
		}
		void* SelectorData = SelectorProperty->ContainerPtrToValuePtr<void>(Node);
		FNameProperty* NameProperty = CastField<FNameProperty>(SelectorProperty->Struct->FindPropertyByName(TEXT("SelectedKeyName")));
		if (NameProperty == nullptr)
		{
			return true;
		}
		const FName KeyName = NameProperty->GetPropertyValue(NameProperty->ContainerPtrToValuePtr<void>(SelectorData));
		if (KeyName.IsNone())
		{
			return true;
		}

		FObjectProperty* QueryProperty = CastField<FObjectProperty>(RequestProperty->Struct->FindPropertyByName(TEXT("QueryTemplate")));
		if (QueryProperty == nullptr)
		{
			return true;
		}
		const UEnvQuery* Query = Cast<UEnvQuery>(QueryProperty->GetObjectPropertyValue(QueryProperty->ContainerPtrToValuePtr<void>(RequestProperty->ContainerPtrToValuePtr<void>(Node))));
		if (Query == nullptr)
		{
			return true; // the asset-reference writer already rejected missing templates
		}

		const FBlackboard::FKey KeyId = BlackboardAsset->GetKeyID(KeyName);
		const UClass* KeyClass = BlackboardAsset->GetKeyType(KeyId).Get();

		int32 OptionIndex = 0;
		for (const UEnvQueryOption* Option : Query->GetOptions())
		{
			if (Option == nullptr || Option->Generator == nullptr)
			{
				OptionIndex++;
				continue;
			}
			UClass* ItemClass = Option->Generator->ItemType.Get();
			if (!IsItemStoreableIntoKey(ItemClass, KeyClass))
			{
				const TCHAR* ItemKind = ItemClass && ItemClass->IsChildOf<UEnvQueryItemType_ActorBase>()
					? TEXT("Actor")
					: (ItemClass && ItemClass->IsChildOf<UEnvQueryItemType_VectorBase>() ? TEXT("point") : TEXT("unsupported"));
				OutError = FString::Printf(TEXT("%s: EQS option %d produces %s items (%s) but BlackboardKey '%s' is a %s key - the result cannot be stored at runtime; use an Object key or a point-based query"),
					*NodePath, OptionIndex, ItemKind,
					ItemClass ? *ItemClass->GetName() : TEXT("None"),
					*KeyName.ToString(),
					KeyClass ? *KeyClass->GetName() : TEXT("None"));
				return false;
			}
			OptionIndex++;
		}
		return true;
	}

	// ------------------------------------------------------------------ key dataflow (P1)

	/** One key-selector usage of a node kind. Registration is by class NAME so
	 *  5.6+-only engine classes can be listed and simply resolve to nothing on
	 *  5.5 (zero version conditionals). */
	struct FKeyEffectEntry
	{
		const TCHAR* ClassName;
		const TCHAR* Property;
		bool bWrite;
	};

	const TArray<FKeyEffectEntry>& GetKeyEffectRegistry()
	{
		static const TArray<FKeyEffectEntry> Registry =
		{
			// The RunEQS task stores only when bUseBBKey is set (modeled at
			// analysis time); the service stores unconditionally (it has no flag).
			{ TEXT("BTTask_RunEQSQuery"), TEXT("BlackboardKey"), true },
			{ TEXT("BTService_RunEQS"), TEXT("BlackboardKey"), true },
			// BT Forge node library
			{ TEXT("BTForgeTask_ComputeValue"), TEXT("LeftKey"), false },
			{ TEXT("BTForgeTask_ComputeValue"), TEXT("RightKey"), false },
			{ TEXT("BTForgeTask_ComputeValue"), TEXT("ResultKey"), true },
			{ TEXT("BTForgeService_SetBlackboardValue"), TEXT("KeyToSet"), true },
			{ TEXT("BTForgeService_SetBlackboardValue"), TEXT("SourceKey"), false },
			{ TEXT("BTForgeService_UpdateDistance"), TEXT("SourceKey"), false },
			{ TEXT("BTForgeService_UpdateDistance"), TEXT("TargetKey"), false },
			{ TEXT("BTForgeService_UpdateDistance"), TEXT("ResultKey"), true },
			{ TEXT("BTForgeTask_LogMessage"), TEXT("KeyToLog"), false },
			// engine readers
			{ TEXT("BTTask_MoveTo"), TEXT("BlackboardKey"), false },
			{ TEXT("BTTask_RotateToFaceBBEntry"), TEXT("BlackboardKey"), false },
			{ TEXT("BTTask_MoveDirectlyToward"), TEXT("BlackboardKey"), false },
			// engine writers, 5.6+ only (resolve to nothing on 5.5)
			{ TEXT("BTTask_SetKeyValueBool"), TEXT("BlackboardKey"), true },
			{ TEXT("BTTask_SetKeyValueClass"), TEXT("BlackboardKey"), true },
			{ TEXT("BTTask_SetKeyValueEnum"), TEXT("BlackboardKey"), true },
			{ TEXT("BTTask_SetKeyValueInt32"), TEXT("BlackboardKey"), true },
			{ TEXT("BTTask_SetKeyValueFloat"), TEXT("BlackboardKey"), true },
			{ TEXT("BTTask_SetKeyValueName"), TEXT("BlackboardKey"), true },
			{ TEXT("BTTask_SetKeyValueString"), TEXT("BlackboardKey"), true },
			{ TEXT("BTTask_SetKeyValueObject"), TEXT("BlackboardKey"), true },
			{ TEXT("BTTask_SetKeyValueRotator"), TEXT("BlackboardKey"), true },
			{ TEXT("BTTask_SetKeyValueStruct"), TEXT("BlackboardKey"), true },
			{ TEXT("BTTask_SetKeyValueVector"), TEXT("BlackboardKey"), true },
		};
		return Registry;
	}

	/** Guarded single-field read of a FBlackboardKeySelector's SelectedKeyName.
	 *  This is the ONLY selector access the readback does: the L1 forensics
	 *  crash (Core.dll fast-fail on Lyra's bot tree) came from walking the
	 *  struct's object-pointer members; a named FName member read never
	 *  dereferences them. */
	bool ReadSelectorKeyName(FProperty* SelectorProperty, void* SelectorData, FName& OutKeyName)
	{
		const FStructProperty* SelectorStruct = CastField<FStructProperty>(SelectorProperty);
		if (SelectorStruct == nullptr || SelectorStruct->Struct == nullptr || SelectorData == nullptr)
		{
			return false;
		}
		const FNameProperty* NameProperty = CastField<FNameProperty>(SelectorStruct->Struct->FindPropertyByName(TEXT("SelectedKeyName")));
		if (NameProperty == nullptr)
		{
			return false;
		}
		OutKeyName = NameProperty->GetPropertyValue(NameProperty->ContainerPtrToValuePtr<void>(SelectorData));
		return true;
	}

	/** Effects for one node class: registry entries matched along the parent
	 *  chain (blueprint subclasses of registered natives inherit their
	 *  effects), plus the spec-level override. Decorators are handled
	 *  generically as readers by the analyzer. Returns false when the class
	 *  (or none of its parents) is registered - the caller records it as an
	 *  unknown-effects node instead of guessing. */
	bool CollectKeyEffects(UClass* NodeClass, const TMap<FName, TMap<FName, bool>>& NodeEffects, TMap<FName, bool>& OutEffects, bool& bOverridden)
	{
		bOverridden = false;
		// spec-level override wins, matched by class name (closest first)
		for (UClass* C = NodeClass; C && C != UObject::StaticClass(); C = C->GetSuperClass())
		{
			if (const TMap<FName, bool>* Override = NodeEffects.Find(C->GetFName()))
			{
				OutEffects = *Override;
				bOverridden = true;
				return true;
			}
		}
		for (UClass* C = NodeClass; C && C != UObject::StaticClass(); C = C->GetSuperClass())
		{
			bool bMatched = false;
			for (const FKeyEffectEntry& Entry : GetKeyEffectRegistry())
			{
				if (C->GetName().Equals(Entry.ClassName))
				{
					OutEffects.Add(FName(Entry.Property), Entry.bWrite);
					bMatched = true;
				}
			}
			if (bMatched)
			{
				return true;
			}
		}
		return false;
	}

	/** Path-segment-aware prefix test ("root.children[1]" is not a prefix of
	 *  "root.children[10]"). */
	bool IsScopePrefix(const FString& WriterScope, const FString& ReaderScope)
	{
		if (WriterScope.Equals(ReaderScope))
		{
			return true;
		}
		if (!ReaderScope.StartsWith(WriterScope))
		{
			return false;
		}
		return ReaderScope.Len() > WriterScope.Len() && ReaderScope[WriterScope.Len()] == TEXT('.');
	}

	/** One aggregated key flow across the built tree. */
	struct FKeyFlow
	{
		TArray<FString> Writers;
		TArray<FString> Readers;
		TArray<FString> WriterScopes;
		TArray<FString> ReaderScopes;
		TArray<int32> ReaderAbortModes; // decorators only; -1 otherwise
		bool bExternal = false;
		FString ExternalReason;
	};

	/** Walk the built tree, aggregate key flows and classify them. Fatal
	 *  findings (dead gates, EQS item-type mismatches) come back through
	 *  OutFatal; non-fatal ones (orphan writers, scope, abort, unknown
	 *  effects) land in the dataflow report. */
	void AnalyzeKeyDataflow(UBTCompositeNode* Root, const UBlackboardData* BlackboardAsset,
		const TMap<FString, FString>& ExternalKeys, const TMap<FName, TMap<FName, bool>>& NodeEffects,
		const TSharedRef<FJsonObject>& OutDataflow, FString& OutFatal)
	{
		TMap<FName, FKeyFlow> Flows;
		TSet<FString> UnknownEffectClasses;

		// One walk, two bookkeeping arrays: each node carries its own path and
		// its "scope" (the nearest enclosing composite - the branch whose
		// activation is required for the node to run).
		TFunction<void(UBTNode* Node, const FString& Path, const FString& Scope, bool bIsDecorator)> Visit =
			[&](UBTNode* Node, const FString& Path, const FString& Scope, bool bIsDecorator)
		{
			UClass* NodeClass = Node->GetClass();
			TMap<FName, bool> Effects;
			bool bOverridden = false;
			const bool bRegistered = CollectKeyEffects(NodeClass, NodeEffects, Effects, bOverridden);

			// Decorators are readers by definition: every key selector on a
			// decorator is a read regardless of registration.
			if (!bRegistered && bIsDecorator)
			{
				for (TFieldIterator<FProperty> It(NodeClass); It; ++It)
				{
					FName KeyName;
					if (It->IsA<FStructProperty>() && ReadSelectorKeyName(*It, It->ContainerPtrToValuePtr<void>(Node), KeyName) && !KeyName.IsNone())
					{
						Effects.Add(It->GetFName(), false);
					}
				}
			}

			if (Effects.Num() == 0)
			{
				// unregistered and not a decorator: does the class even carry
				// key selectors? If yes, report it instead of guessing.
				for (TFieldIterator<FProperty> It(NodeClass); It; ++It)
				{
					FName Unused;
					if (It->IsA<FStructProperty>() && ReadSelectorKeyName(*It, It->ContainerPtrToValuePtr<void>(Node), Unused))
					{
						UnknownEffectClasses.Add(NodeClass->GetName());
						break;
					}
				}
			}
			else
			{
			for (const TPair<FName, bool>& Effect : Effects)
			{
				FProperty* SelectorProperty = NodeClass->FindPropertyByName(Effect.Key);
				if (SelectorProperty == nullptr)
				{
					continue;
				}
				FName KeyName;
				if (!ReadSelectorKeyName(SelectorProperty, SelectorProperty->ContainerPtrToValuePtr<void>(Node), KeyName) || KeyName.IsNone())
				{
					continue; // unbound selector: neither reads nor writes
				}
				FKeyFlow& Flow = Flows.FindOrAdd(KeyName);
				if (Effect.Value)
				{
					// the RunEQS task stores only when bUseBBKey is set
					bool bStores = true;
					if (NodeClass->IsChildOf<UBTTask_RunEQSQuery>())
					{
						if (const FBoolProperty* UseBBKey = CastField<FBoolProperty>(NodeClass->FindPropertyByName(TEXT("bUseBBKey"))))
						{
							bStores = UseBBKey->GetPropertyValue(UseBBKey->ContainerPtrToValuePtr<void>(Node));
						}
					}
					if (bStores)
					{
						Flow.Writers.Add(Path);
						Flow.WriterScopes.Add(Scope);
					}
				}
				else
				{
					Flow.Readers.Add(Path);
					Flow.ReaderScopes.Add(Scope);
					int32 AbortMode = -1;
					if (bIsDecorator)
					{
						if (const FByteProperty* AbortProperty = CastField<FByteProperty>(NodeClass->FindPropertyByName(TEXT("FlowAbortMode"))))
						{
							AbortMode = static_cast<int32>(AbortProperty->GetPropertyValue(AbortProperty->ContainerPtrToValuePtr<void>(Node)));
						}
					}
					Flow.ReaderAbortModes.Add(AbortMode);
				}
			}
			}

			// recurse into composites (children already carry their services)
			if (const UBTCompositeNode* Composite = Cast<UBTCompositeNode>(Node))
			{
				for (int32 Index = 0; Index < Composite->Children.Num(); Index++)
				{
					const FBTCompositeChild& Child = Composite->Children[Index];
					const FString ChildPath = FString::Printf(TEXT("%s.children[%d]"), *Path, Index);
					for (int32 DecoratorIndex = 0; DecoratorIndex < Child.Decorators.Num(); DecoratorIndex++)
					{
						if (Child.Decorators[DecoratorIndex])
						{
							Visit(Child.Decorators[DecoratorIndex],
								FString::Printf(TEXT("%s.decorators[%d]"), *ChildPath, DecoratorIndex),
								Path, true); // a decorator is evaluated by its HOST composite
						}
					}
					UBTNode* ChildNode = Child.ChildComposite ? static_cast<UBTNode*>(Child.ChildComposite) : Child.ChildTask.Get();
					if (ChildNode)
					{
						Visit(ChildNode, ChildPath, ChildPath, false);
					}
				}
				for (int32 ServiceIndex = 0; ServiceIndex < Composite->Services.Num(); ServiceIndex++)
				{
					if (Composite->Services[ServiceIndex])
					{
						Visit(Composite->Services[ServiceIndex],
							FString::Printf(TEXT("%s.services[%d]"), *Path, ServiceIndex),
							Path, false); // a service's scope is its host branch
					}
				}
			}
		};
		Visit(Root, TEXT("root"), TEXT("root"), false);

		// aggregate the report
		TArray<TSharedPtr<FJsonValue>> KeyReports;
		TArray<TSharedPtr<FJsonValue>> DeadGates;
		TArray<TSharedPtr<FJsonValue>> OrphanWriters;
		TArray<TSharedPtr<FJsonValue>> ScopeWarnings;
		TArray<TSharedPtr<FJsonValue>> AbortWarnings;
		// FName lacks operator< on 5.5 - sort lexically for deterministic output
		Flows.KeySort([](const FName& A, const FName& B) { return A.ToString() < B.ToString(); });
		for (const TPair<FName, FKeyFlow>& Pair : Flows)
		{
			const FName KeyName = Pair.Key;
			const FKeyFlow& Flow = Pair.Value;

			bool bExternal = Flow.bExternal;
			FString Reason = Flow.ExternalReason;
			if (const FString* Declared = ExternalKeys.Find(KeyName.ToString()))
			{
				bExternal = true;
				Reason = *Declared;
			}
			// the engine writes the SelfActor key when the blackboard initializes
			// (AAIController::InitializeBlackboard) - implicitly external.
			if (KeyName == FBlackboard::KeySelf)
			{
				bExternal = true;
				if (Reason.IsEmpty())
				{
					Reason = TEXT("engine-written: AIController sets SelfActor on blackboard init");
				}
			}

			const bool bHasWriter = Flow.Writers.Num() > 0;
			const bool bHasReader = Flow.Readers.Num() > 0;

			FString TypeString = TEXT("(unresolved)");
			if (BlackboardAsset)
			{
				if (TSubclassOf<UBlackboardKeyType> KeyTypeClass = BlackboardAsset->GetKeyType(BlackboardAsset->GetKeyID(KeyName)))
				{
					TypeString = KeyTypeClass->GetName();
				}
			}

			TSharedRef<FJsonObject> KeyReport = MakeShared<FJsonObject>();
			KeyReport->SetStringField(TEXT("name"), KeyName.ToString());
			KeyReport->SetStringField(TEXT("type"), TypeString);
			KeyReport->SetBoolField(TEXT("external"), bExternal);
			if (!Reason.IsEmpty())
			{
				KeyReport->SetStringField(TEXT("externalReason"), Reason);
			}
			auto StringArray = [](const TArray<FString>& In)
			{
				TArray<TSharedPtr<FJsonValue>> Out;
				for (const FString& S : In)
				{
					Out.Add(MakeShared<FJsonValueString>(S));
				}
				return Out;
			};
			KeyReport->SetArrayField(TEXT("writtenBy"), StringArray(Flow.Writers));
			KeyReport->SetArrayField(TEXT("readBy"), StringArray(Flow.Readers));
			KeyReports.Add(MakeShared<FJsonValueObject>(KeyReport));

			if (bHasReader && !bHasWriter && !bExternal)
			{
				TSharedRef<FJsonObject> Dead = MakeShared<FJsonObject>();
				Dead->SetStringField(TEXT("key"), KeyName.ToString());
				Dead->SetStringField(TEXT("readBy"), FString::Join(Flow.Readers, TEXT(", ")));
				Dead->SetStringField(TEXT("reason"), TEXT("read by decorators/tasks but never written in this tree; declare it in externalKeys or add a writer"));
				DeadGates.Add(MakeShared<FJsonValueObject>(Dead));
				// An unregistered node with key selectors could be the writer we
				// cannot see, so a "dead" gate is only conclusive when every node in
				// the tree has known effects.
				if (OutFatal.IsEmpty() && UnknownEffectClasses.Num() == 0)
				{
					OutFatal = FString::Printf(TEXT("dead gate: key '%s' is read at %s but never written; add a writer or declare it in externalKeys (on the tree root - works for build too - or in BlackboardJson)"),
						*KeyName.ToString(), *Flow.Readers[0]);
				}
			}
			if (bHasWriter && !bHasReader && !bExternal)
			{
				TSharedRef<FJsonObject> Orphan = MakeShared<FJsonObject>();
				Orphan->SetStringField(TEXT("key"), KeyName.ToString());
				Orphan->SetStringField(TEXT("writtenBy"), FString::Join(Flow.Writers, TEXT(", ")));
				Orphan->SetStringField(TEXT("reason"), TEXT("written but never read inside this tree (test instrumentation or a missing consumer)"));
				OrphanWriters.Add(MakeShared<FJsonValueObject>(Orphan));
			}

			// scope: a writer only feeds readers inside branches at or below its
			// own mount point (services tick with their branch)
			for (int32 ri = 0; ri < Flow.Readers.Num(); ri++)
			{
				for (int32 wi = 0; wi < Flow.Writers.Num(); wi++)
				{
					if (!IsScopePrefix(Flow.WriterScopes[wi], Flow.ReaderScopes[ri]))
					{
						TSharedRef<FJsonObject> Scope = MakeShared<FJsonObject>();
						Scope->SetStringField(TEXT("key"), KeyName.ToString());
						Scope->SetStringField(TEXT("producer"), Flow.Writers[wi]);
						Scope->SetStringField(TEXT("consumer"), Flow.Readers[ri]);
						Scope->SetStringField(TEXT("reason"), TEXT("writer's branch does not contain the reader's branch; the service only ticks while its own branch is active"));
						ScopeWarnings.Add(MakeShared<FJsonValueObject>(Scope));
						break; // one warning per reader is enough
					}
				}
			}

			// abort: a gated branch whose key flips mid-run will not re-evaluate
			// unless the decorator declares a FlowAbortMode
			for (int32 ri = 0; ri < Flow.Readers.Num(); ri++)
			{
				if (Flow.ReaderAbortModes[ri] != 0 || !bHasWriter)
				{
					continue;
				}
				TSharedRef<FJsonObject> Abort = MakeShared<FJsonObject>();
				Abort->SetStringField(TEXT("decorator"), Flow.Readers[ri]);
				Abort->SetStringField(TEXT("key"), KeyName.ToString());
				Abort->SetStringField(TEXT("reason"), TEXT("FlowAbortMode=None: the branch will not re-evaluate when this key changes"));
				AbortWarnings.Add(MakeShared<FJsonValueObject>(Abort));
			}
		}

		OutDataflow->SetArrayField(TEXT("keys"), KeyReports);
		OutDataflow->SetArrayField(TEXT("deadGates"), DeadGates);
		OutDataflow->SetArrayField(TEXT("orphanWriters"), OrphanWriters);
		OutDataflow->SetArrayField(TEXT("scopeWarnings"), ScopeWarnings);
		OutDataflow->SetArrayField(TEXT("abortWarnings"), AbortWarnings);
		TArray<TSharedPtr<FJsonValue>> Unknown;
		for (const FString& ClassName : UnknownEffectClasses)
		{
			Unknown.Add(MakeShared<FJsonValueString>(ClassName));
		}
		OutDataflow->SetArrayField(TEXT("unknownEffects"), Unknown);
		OutDataflow->SetStringField(TEXT("note"), TEXT("deadGates fail the build when every node's effects are known; with unregistered nodes present they are reported but not fatal - declare nodeEffects for those classes. externalKeys declares keys fed from outside the tree (tree root works for build, tree root or BlackboardJson for validate)"));
	}

	/** Parse the optional spec-level declarations: "externalKeys": {name: reason}
	 *  and "nodeEffects": {ClassName: {writes:[...], reads:[...]}}. */
	bool ParseKeyDeclarations(const TSharedPtr<FJsonObject>& RootSpec, const TSharedPtr<FJsonObject>& BlackboardSpec,
		TMap<FString, FString>& OutExternalKeys, TMap<FName, TMap<FName, bool>>& OutNodeEffects, FString& OutError)
	{
		// Declarations live in the blackboard spec when there is one (that is the
		// contract about keys) and are still accepted on the tree root for callers
		// that only carry a tree. The blackboard wins on conflicts.
		auto ParseFrom = [&](const TSharedPtr<FJsonObject>& Source) -> bool
		{
			if (!Source.IsValid())
			{
				return true;
			}
			const TSharedPtr<FJsonObject>* ExternalObject = nullptr;
			if (Source->TryGetObjectField(TEXT("externalKeys"), ExternalObject) && ExternalObject->IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*ExternalObject)->Values)
				{
					FString Reason;
					Pair.Value->TryGetString(Reason);
					OutExternalKeys.Add(Pair.Key, Reason);
				}
			}
			const TSharedPtr<FJsonObject>* EffectsObject = nullptr;
			if (Source->TryGetObjectField(TEXT("nodeEffects"), EffectsObject) && EffectsObject->IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*EffectsObject)->Values)
				{
					const TSharedPtr<FJsonObject>* Effect = nullptr;
					if (!Pair.Value->TryGetObject(Effect) || !Effect->IsValid())
					{
						OutError = FString::Printf(TEXT("nodeEffects.%s: expected {\"writes\":[...],\"reads\":[...]}"), *Pair.Key);
						return false;
					}
					TMap<FName, bool> Props;
					for (const TCHAR* FieldName : { TEXT("writes"), TEXT("reads") })
					{
						const bool bWrite = FCString::Strcmp(FieldName, TEXT("writes")) == 0;
						const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
						(*Effect)->TryGetArrayField(FieldName, List);
						if (List)
						{
							for (const TSharedPtr<FJsonValue>& Item : *List)
							{
								FString PropName;
								if (!Item->TryGetString(PropName) || PropName.IsEmpty())
								{
									OutError = FString::Printf(TEXT("nodeEffects.%s.%s: expected property name strings"), *Pair.Key, FieldName);
									return false;
								}
								Props.Add(FName(*PropName), bWrite);
							}
						}
					}
					OutNodeEffects.Add(FName(*Pair.Key), Props);
				}
			}
			return true;
		};

		if (!ParseFrom(RootSpec))
		{
			return false;
		}
		return ParseFrom(BlackboardSpec);
	}

	/** Run the dataflow analysis and finish a validate/build result. Fatal
	 *  findings (dead gates) flip ok:false with the dataflow attached. */
	FString FinishWithKeyDataflow(UBehaviorTree* Tree, UBTCompositeNode* Root,
		const TSharedPtr<FJsonObject>& RootSpec, const TMap<FString, FString>& ExternalKeys,
		const TMap<FName, TMap<FName, bool>>& NodeEffects, TSharedRef<FJsonObject> Result)
	{
		TSharedRef<FJsonObject> Dataflow = MakeShared<FJsonObject>();
		FString Fatal;
		AnalyzeKeyDataflow(Root, Tree ? Tree->BlackboardAsset : nullptr, ExternalKeys, NodeEffects, Dataflow, Fatal);
		Result->SetObjectField(TEXT("dataflow"), Dataflow);
		if (!Fatal.IsEmpty())
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Fatal);
		}
		return JsonToString(Result);
	}

	/** Build one service/decorator node from its spec object: {"type":...,"name":...,"params":{...}}. */
	UBTNode* BuildAuxNode(UBehaviorTree* Tree, const TSet<FName>& KnownBlackboardKeys, const FString& Kind, const TSharedPtr<FJsonValue>& SpecValue, const FString& NodePath, int32& OutNodeCount, FString& OutError)
	{
		const TSharedPtr<FJsonObject>* SpecObject;
		if (!SpecValue->TryGetObject(SpecObject) || !SpecObject->IsValid())
		{
			OutError = NodePath + TEXT(": expected an object with 'type' and optional 'params'. Allowed ") + AuxWhitelistText(Kind);
			return nullptr;
		}
		FString TypeName;
		if (!(*SpecObject)->TryGetStringField(TEXT("type"), TypeName))
		{
			OutError = NodePath + TEXT(": missing 'type'. Allowed ") + AuxWhitelistText(Kind);
			return nullptr;
		}
		UClass* AuxClass = ResolveAuxType(Kind, TypeName, OutError);
		if (AuxClass == nullptr)
		{
			OutError = NodePath + TEXT(": ") + OutError;
			return nullptr;
		}

		UBTNode* AuxNode = NewObject<UBTNode>(Tree, AuxClass);
		OutNodeCount++;

		FString DisplayName;
		if ((*SpecObject)->TryGetStringField(TEXT("name"), DisplayName) && !DisplayName.IsEmpty())
		{
			if (FStrProperty* NameProperty = CastField<FStrProperty>(AuxNode->GetClass()->FindPropertyByName(TEXT("NodeName"))))
			{
				NameProperty->SetPropertyValue(NameProperty->ContainerPtrToValuePtr<void>(AuxNode), DisplayName);
			}
		}

		TSharedPtr<FJsonObject> Params;
		if (!TryGetSpecObject(*SpecObject, TEXT("params"), NodePath, Params, OutError))
		{
			return nullptr;
		}
		if (!Params.IsValid())
		{
			Params = MakeShared<FJsonObject>();
		}
		AutoSetRunEQSUseBBKey(AuxClass, Params);
		if (!ApplyParamsToContainer(AuxNode->GetClass(), AuxNode, Params, &KnownBlackboardKeys, NodePath, OutError))
		{
			return nullptr;
		}
		{
			FName DbgKey;
			FProperty* DbgProp = AuxNode->GetClass()->FindPropertyByName(TEXT("BlackboardKey"));
			if (DbgProp && ReadSelectorKeyName(DbgProp, DbgProp->ContainerPtrToValuePtr<void>(AuxNode), DbgKey))
			{
				UE_LOG(LogTemp, Warning, TEXT("[BTForge diag] %s: BlackboardKey after apply = '%s'"), *NodePath, *DbgKey.ToString());
			}
		}
		if (!ValidateRunEQSKeyTypes(AuxNode, Tree ? Tree->BlackboardAsset : nullptr, NodePath, OutError))
		{
			return nullptr;
		}
		return AuxNode;
	}

	UBTNode* BuildNodeRecursive(UBehaviorTree* Tree, const TSet<FName>& KnownBlackboardKeys, const TSharedPtr<FJsonObject>& NodeJson, const FString& NodePath, int32& OutNodeCount, FString& OutError)
	{
		FString TypeName;
		if (!NodeJson->TryGetStringField(TEXT("type"), TypeName))
		{
			OutError = NodePath + TEXT(": missing 'type'. Allowed types: ") + WhitelistText();
			return nullptr;
		}
		UClass* NodeClass = ResolveNodeType(TypeName, OutError);
		if (NodeClass == nullptr)
		{
			OutError = NodePath + TEXT(": ") + OutError;
			return nullptr;
		}

		UBTNode* Node = NewObject<UBTNode>(Tree, NodeClass);
		OutNodeCount++;

		// Optional display name (UBTNode::NodeName is an FString UPROPERTY).
		FString DisplayName;
		if (NodeJson->TryGetStringField(TEXT("name"), DisplayName) && !DisplayName.IsEmpty())
		{
			if (FStrProperty* NameProperty = CastField<FStrProperty>(Node->GetClass()->FindPropertyByName(TEXT("NodeName"))))
			{
				NameProperty->SetPropertyValue(NameProperty->ContainerPtrToValuePtr<void>(Node), DisplayName);
			}
		}

		TSharedPtr<FJsonObject> Params;
		if (!TryGetSpecObject(NodeJson, TEXT("params"), NodePath, Params, OutError))
		{
			return nullptr;
		}
		if (!Params.IsValid())
		{
			Params = MakeShared<FJsonObject>();
		}
		AutoSetRunEQSUseBBKey(NodeClass, Params);
		if (!ApplyParamsToContainer(Node->GetClass(), Node, Params, &KnownBlackboardKeys, NodePath, OutError))
		{
			return nullptr;
		}
		if (!ValidateRunEQSKeyTypes(Node, Tree ? Tree->BlackboardAsset : nullptr, NodePath, OutError))
		{
			return nullptr;
		}

		UBTCompositeNode* Composite = Cast<UBTCompositeNode>(Node);
		if (Composite == nullptr && NodeJson->HasField(TEXT("services")))
		{
			OutError = NodePath + TEXT(": 'services' can only be attached to composite nodes (Sequence/Selector), not to tasks");
			return nullptr;
		}
		if (Composite != nullptr)
		{
			// Optional "services": tick alongside this composite's branch.
			TArray<TSharedPtr<FJsonValue>> ServiceSpecs;
			if (!TryGetSpecArray(NodeJson, TEXT("services"), NodePath, ServiceSpecs, OutError))
			{
				return nullptr;
			}
			for (int32 ServiceIndex = 0; ServiceIndex < ServiceSpecs.Num(); ServiceIndex++)
			{
				const FString ServicePath = FString::Printf(TEXT("%s.services[%d]"), *NodePath, ServiceIndex);
				UBTNode* Service = BuildAuxNode(Tree, KnownBlackboardKeys, TEXT("service"), ServiceSpecs[ServiceIndex], ServicePath, OutNodeCount, OutError);
				if (Service == nullptr)
				{
					return nullptr;
				}
				Composite->Services.Add(Cast<UBTService>(Service));
			}

			if (!NodeJson->HasField(TEXT("children")))
			{
				OutError = NodePath + TEXT(": composite requires a non-empty 'children' array");
				return nullptr;
			}
			TArray<TSharedPtr<FJsonValue>> Children;
			if (!TryGetSpecArray(NodeJson, TEXT("children"), NodePath, Children, OutError))
			{
				return nullptr;
			}
			if (Children.Num() == 0)
			{
				OutError = NodePath + TEXT(": 'children' must not be empty");
				return nullptr;
			}
			for (int32 ChildIndex = 0; ChildIndex < Children.Num(); ChildIndex++)
			{
				const TSharedPtr<FJsonObject>* ChildObject;
				if (!Children[ChildIndex]->TryGetObject(ChildObject) || !ChildObject->IsValid())
				{
					OutError = FString::Printf(TEXT("%s.children[%d]: expected an object"), *NodePath, ChildIndex);
					return nullptr;
				}
				const FString ChildPath = FString::Printf(TEXT("%s.children[%d]"), *NodePath, ChildIndex);
				UBTNode* ChildNode = BuildNodeRecursive(Tree, KnownBlackboardKeys, *ChildObject, ChildPath, OutNodeCount, OutError);
				if (ChildNode == nullptr)
				{
					return nullptr;
				}
				FBTCompositeChild& Slot = Composite->Children.AddDefaulted_GetRef();
				if (UBTCompositeNode* ChildComposite = Cast<UBTCompositeNode>(ChildNode))
				{
					Slot.ChildComposite = ChildComposite;
				}
				else
				{
					Slot.ChildTask = Cast<UBTTaskNode>(ChildNode);
				}

				// Optional "decorators" on the child spec: gate entry to this
				// child branch. Empty DecoratorOps = AND over the list.
				TArray<TSharedPtr<FJsonValue>> DecoratorSpecs;
				if (!TryGetSpecArray(*ChildObject, TEXT("decorators"), ChildPath, DecoratorSpecs, OutError))
				{
					return nullptr;
				}
				{
					for (int32 DecoratorIndex = 0; DecoratorIndex < DecoratorSpecs.Num(); DecoratorIndex++)
					{
						const FString DecoratorPath = FString::Printf(TEXT("%s.decorators[%d]"), *ChildPath, DecoratorIndex);
						UBTNode* Decorator = BuildAuxNode(Tree, KnownBlackboardKeys, TEXT("decorator"), DecoratorSpecs[DecoratorIndex], DecoratorPath, OutNodeCount, OutError);
						if (Decorator == nullptr)
						{
							return nullptr;
						}
						Slot.Decorators.Add(Cast<UBTDecorator>(Decorator));
					}
				}
			}
		}
		else if (NodeJson->HasField(TEXT("children")))
		{
			OutError = NodePath + TEXT(": tasks cannot have 'children'");
			return nullptr;
		}

		return Node;
	}

	/** Resolve FBlackboardKeySelector fields (SelectedKeyType / SelectedKeyID) from the
	 *  tree's blackboard. Writing the key name alone is not enough: runtime code that
	 *  compares the selector's *type* (e.g. UBTTask_RunEQSQuery::OnQueryFinished ->
	 *  UEnvQueryItemType::StoreInBlackboard) fails silently and the task reports a
	 *  failed result even though the query produced items. Walks nested structs so
	 *  FEQSParametrizedQueryExecutionRequest's inner selectors are covered too. */
	void FixupKeySelectorsInStruct(UStruct* Struct, void* Data, const UBlackboardData* BlackboardAsset, int32 Depth, UObject* Owner)
	{
		if (Struct == nullptr || Data == nullptr || BlackboardAsset == nullptr || Depth > 4)
		{
			return;
		}
		const FNameProperty* NameProperty = CastField<FNameProperty>(Struct->FindPropertyByName(TEXT("SelectedKeyName")));
		if (NameProperty != nullptr)
		{
			FClassProperty* TypeProperty = CastField<FClassProperty>(Struct->FindPropertyByName(TEXT("SelectedKeyType")));
			FNumericProperty* IdProperty = CastField<FNumericProperty>(Struct->FindPropertyByName(TEXT("SelectedKeyID")));
			const FName KeyName = NameProperty->GetPropertyValue(NameProperty->ContainerPtrToValuePtr<void>(Data));
			if (!KeyName.IsNone())
			{
				const FBlackboard::FKey KeyId = BlackboardAsset->GetKeyID(KeyName);
				// FKey is an id in the *whole* blackboard chain, not an index into this
				// asset's Keys array: with a parent blackboard FirstKeyID > 0, so
				// indexing Keys directly writes the wrong KeyType (or none at all) while
				// the id still looks right - exactly the silent runtime failure this
				// fixup exists to prevent.
				const FBlackboardEntry* Entry = BlackboardAsset->GetKey(KeyId);
				if (Entry != nullptr)
				{
					if (TypeProperty != nullptr)
					{
						TypeProperty->SetObjectPropertyValue(TypeProperty->ContainerPtrToValuePtr<void>(Data),
							Entry->KeyType != nullptr ? Entry->KeyType->GetClass() : nullptr);
					}
					if (IdProperty != nullptr)
					{
						IdProperty->SetIntPropertyValue(IdProperty->ContainerPtrToValuePtr<void>(Data), static_cast<int64>(static_cast<int32>(KeyId)));
					}
					// UBTNode::PreSave validates every bound selector against its
					// AllowedTypes filters and RESETS bindings the filters reject
					// ("first matching key" or none) - the RunEQS service's
					// constructor filter Object(AActor) rejects a generic Object key
					// (BaseClass=UObject), which silently rewrote TargetEnemy to
					// SelfActor at save time. Mirror the editor's selector details:
					// rebuild the filters from the bound entry's key type.
					if (Struct->IsChildOf(FBlackboardKeySelector::StaticStruct()) && Entry->KeyType != nullptr && Owner != nullptr)
					{
						FBlackboardKeySelector* Selector = static_cast<FBlackboardKeySelector*>(Data);
						Selector->AllowedTypes.Reset();
						if (UBlackboardKeyType* FilterCopy = DuplicateObject(Entry->KeyType, Owner))
						{
							Selector->AllowedTypes.Add(FilterCopy);
						}
					}
				}
			}
			return;
		}
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			if (const FStructProperty* InnerStruct = CastField<FStructProperty>(*It))
			{
				FixupKeySelectorsInStruct(InnerStruct->Struct, InnerStruct->ContainerPtrToValuePtr<void>(Data), BlackboardAsset, Depth + 1, Owner);
			}
		}
	}

	void FixupKeySelectors(UObject* Node, const UBlackboardData* BlackboardAsset)
	{
		if (Node == nullptr || BlackboardAsset == nullptr)
		{
			return;
		}
		for (TFieldIterator<FProperty> It(Node->GetClass()); It; ++It)
		{
			if (const FStructProperty* StructProperty = CastField<FStructProperty>(*It))
			{
				FixupKeySelectorsInStruct(StructProperty->Struct, StructProperty->ContainerPtrToValuePtr<void>(Node), BlackboardAsset, 0, Node);
			}
		}
	}

	void InitializeNodesRecursive(UBTNode* Node, UBehaviorTree* Tree)
	{
		// Key selectors must be fully resolved *before* InitializeFromAsset: nodes such
		// as UBTTask_RunEQSQuery read the selector's type/id while initializing.
		FixupKeySelectors(Node, Tree->BlackboardAsset);
		Node->InitializeFromAsset(*Tree);
		if (UBTCompositeNode* Composite = Cast<UBTCompositeNode>(Node))
		{
			for (UBTService* Service : Composite->Services)
			{
				if (Service)
				{
					InitializeNodesRecursive(Service, Tree);
				}
			}
			for (const FBTCompositeChild& Child : Composite->Children)
			{
				for (UBTDecorator* Decorator : Child.Decorators)
				{
					if (Decorator)
					{
						InitializeNodesRecursive(Decorator, Tree);
					}
				}
				if (Child.ChildComposite)
				{
					InitializeNodesRecursive(Child.ChildComposite, Tree);
				}
				else if (Child.ChildTask)
				{
					InitializeNodesRecursive(Child.ChildTask, Tree);
				}
			}
		}
	}

	TSharedPtr<FJsonValue> ReadPropertyAsJson(FProperty* Property, void* DataPtr)
	{
		if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
		{
			const double Value = Numeric->IsFloatingPoint()
				? Numeric->GetFloatingPointPropertyValue(DataPtr)
				: static_cast<double>(Numeric->GetSignedIntPropertyValue(DataPtr));
			return MakeShared<FJsonValueNumber>(Value);
		}
		if (const FBoolProperty* Bool = CastField<FBoolProperty>(Property))
		{
			return MakeShared<FJsonValueBoolean>(Bool->GetPropertyValue(DataPtr));
		}
		if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			return MakeShared<FJsonValueString>(NameProperty->GetPropertyValue(DataPtr).ToString());
		}
		if (const FStrProperty* StringProperty = CastField<FStrProperty>(Property))
		{
			return MakeShared<FJsonValueString>(StringProperty->GetPropertyValue(DataPtr));
		}
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			// A BP variable whose struct type failed to load leaves Struct null;
			// any method call through it reads near-null memory.
			if (StructProperty->Struct == nullptr)
			{
				UE_LOG(LogTemp, Warning, TEXT("[BTForge] skipping struct property %s: null Struct (type not loaded)"), *Property->GetName());
				return nullptr;
			}
			// FValueOrBBKey_*: report the nested DefaultValue when it is numeric.
			if (FProperty* DefaultProperty = StructProperty->Struct->FindPropertyByName(TEXT("DefaultValue")))
			{
				void* DefaultDataPtr = DefaultProperty->ContainerPtrToValuePtr<void>(DataPtr);
				if (DefaultProperty->IsA<FNumericProperty>())
				{
					return ReadPropertyAsJson(DefaultProperty, DefaultDataPtr);
				}
			}
			// FBlackboardKeySelector: report ONLY the bound key name. The L1
			// forensics crash (Core.dll fast-fail on Lyra's bot tree) came from
			// walking the struct's object-pointer members (AllowedTypes /
			// SelectedKeyType); a named FName member read never touches them.
			// Unbound selectors are omitted to keep dumps meaningful.
			FName KeyName;
			if (ReadSelectorKeyName(Property, DataPtr, KeyName) && !KeyName.IsNone())
			{
				return MakeShared<FJsonValueString>(KeyName.ToString());
			}
			return nullptr;
		}
		// Object/class references are NOT dumped for now: BP-authored assets can
		// carry uninitialized EditInstanceOnly reference fields whose raw bits
		// are small integers, and IsValid() itself dereferences them (real crash
		// observed reading Lyra's bot tree). Reading them safely needs a typed
		// whitelist; until then the dump stays a faithful subset, not the whole.
		return nullptr;
	}

	void DumpScalarProperties(UObject* Object, const TSharedRef<FJsonObject>& OutObject)
	{
		// Base-class back pointers carry no spec value (they reference the tree
		// itself or the owning node instance) - skip them.
		static const TSet<FName> SkippedProperties =
		{
			TEXT("TreeAsset"), TEXT("ParentNode"), TEXT("SourceNode"),
		};
	for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
	{
		FProperty* Property = *It;
		if (SkippedProperties.Contains(Property->GetFName()))
		{
			continue;
		}
			void* DataPtr = Property->ContainerPtrToValuePtr<void>(Object);
			TSharedPtr<FJsonValue> Value = ReadPropertyAsJson(Property, DataPtr);
			if (Value.IsValid())
			{
				OutObject->SetField(Property->GetName(), Value);
			}
		}
	}

	/** Key bindings of a node: {property: {name, type}}. Only SelectedKeyName is read
	 *  from the selector (the L1 fast-fail came from walking the struct's object
	 *  pointers); the type is resolved from the blackboard asset by name. */
	void DumpKeySelectors(UObject* Node, const UBlackboardData* Blackboard, const TSharedRef<FJsonObject>& OutNode)
	{
		TSharedRef<FJsonObject> Keys = MakeShared<FJsonObject>();
		for (TFieldIterator<FProperty> It(Node->GetClass()); It; ++It)
		{
			FProperty* Property = *It;
			FName KeyName;
			if (!Property->IsA<FStructProperty>() || !ReadSelectorKeyName(Property, Property->ContainerPtrToValuePtr<void>(Node), KeyName))
			{
				continue;
			}
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), KeyName.IsNone() ? FString() : KeyName.ToString());
			FString TypeName;
			if (Blackboard != nullptr && !KeyName.IsNone())
			{
				if (TSubclassOf<UBlackboardKeyType> KeyType = Blackboard->GetKeyType(Blackboard->GetKeyID(KeyName)))
				{
					TypeName = KeyType->GetName();
				}
			}
			Entry->SetStringField(TEXT("type"), TypeName);
			Keys->SetObjectField(Property->GetName(), Entry);
		}
		if (Keys->Values.Num() > 0)
		{
			OutNode->SetObjectField(TEXT("keys"), Keys);
		}
	}

	TSharedRef<FJsonObject> DumpAuxNode(UBTNode* AuxNode, const UBlackboardData* Blackboard)
	{
		TSharedRef<FJsonObject> OutNode = MakeShared<FJsonObject>();
		OutNode->SetStringField(TEXT("type"), AuxNode->GetClass()->GetName());
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		DumpScalarProperties(AuxNode, Params);
		OutNode->SetObjectField(TEXT("params"), Params);
		DumpKeySelectors(AuxNode, Blackboard, OutNode);
		return OutNode;
	}

	/** Names for EBTDecoratorLogic ops (keep in sync with BTCompositeNode.h). */
	FString DecoratorOpName(int32 Operation)
	{
		switch (Operation)
		{
			case 1: return TEXT("Test");
			case 2: return TEXT("And");
			case 3: return TEXT("Or");
			case 4: return TEXT("Not");
			default: return TEXT("Invalid");
		}
	}

	TSharedRef<FJsonObject> DumpNode(UBTNode* Node, const UBlackboardData* Blackboard)
	{
		TSharedRef<FJsonObject> OutNode = MakeShared<FJsonObject>();
		OutNode->SetStringField(TEXT("type"), Node->GetClass()->GetName());
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		DumpScalarProperties(Node, Params);
		OutNode->SetObjectField(TEXT("params"), Params);
		DumpKeySelectors(Node, Blackboard, OutNode);

		if (const UBTCompositeNode* Composite = Cast<UBTCompositeNode>(Node))
		{
			// Services run while this composite's branch is active.
			if (Composite->Services.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> Services;
				for (const UBTService* Service : Composite->Services)
				{
					if (Service)
					{
						Services.Add(MakeShared<FJsonValueObject>(DumpAuxNode(const_cast<UBTService*>(Service), Blackboard)));
					}
				}
				OutNode->SetArrayField(TEXT("services"), Services);
			}

			TArray<TSharedPtr<FJsonValue>> Children;
			for (const FBTCompositeChild& Child : Composite->Children)
			{
				const UBTNode* ChildNode = Child.ChildComposite ? static_cast<const UBTNode*>(Child.ChildComposite) : Child.ChildTask.Get();
				if (ChildNode)
				{
					TSharedRef<FJsonObject> ChildJson = DumpNode(const_cast<UBTNode*>(ChildNode), Blackboard);

					// Decorators gate entry to this child branch; DecoratorOps is
					// the stack-encoded logic combination over the decorator list.
					if (Child.Decorators.Num() > 0)
					{
						TArray<TSharedPtr<FJsonValue>> Decorators;
						for (const UBTDecorator* Decorator : Child.Decorators)
						{
							if (Decorator)
							{
								Decorators.Add(MakeShared<FJsonValueObject>(DumpAuxNode(const_cast<UBTDecorator*>(Decorator), Blackboard)));
							}
						}
						ChildJson->SetArrayField(TEXT("decorators"), Decorators);

						TArray<TSharedPtr<FJsonValue>> Ops;
						for (const FBTDecoratorLogic& Op : Child.DecoratorOps)
						{
							TSharedRef<FJsonObject> OpObject = MakeShared<FJsonObject>();
							OpObject->SetStringField(TEXT("op"), DecoratorOpName(Op.Operation));
							OpObject->SetNumberField(TEXT("number"), Op.Number);
							Ops.Add(MakeShared<FJsonValueObject>(OpObject));
						}
						ChildJson->SetArrayField(TEXT("decoratorOps"), Ops);
					}

					Children.Add(MakeShared<FJsonValueObject>(ChildJson));
				}
			}
			OutNode->SetArrayField(TEXT("children"), Children);
		}
		return OutNode;
	}
}

bool UBTForgeLibrary::ApplyParamsToObject(UObject* Target, const FString& ParamsJson, int32& OutAppliedCount, FString& OutError)
{
	OutAppliedCount = 0;
	if (Target == nullptr)
	{
		OutError = TEXT("target object is null");
		return false;
	}
	TSharedPtr<FJsonObject> Params;
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ParamsJson);
		if (!FJsonSerializer::Deserialize(Reader, Params) || !Params.IsValid())
		{
			OutError = TEXT("ParamsJson is not a valid JSON object, e.g. {\"Message\":\"hi\",\"Severity\":\"Warning\"}");
			return false;
		}
	}
	// nullptr = no tree blackboard to validate key selectors against (CDO-level
	// defaults are tree-independent; a tree build re-validates on placement).
	if (!ApplyParamsToContainer(Target->GetClass(), Target, Params, nullptr, TEXT("params"), OutError))
	{
		return false;
	}
	OutAppliedCount = Params->Values.Num();
	return true;
}

const TMap<FString, UClass*>& UBTForgeLibrary::GetNodeTypeAliases()
{
	static const TMap<FString, UClass*> Types =
	{
		{ TEXT("Sequence"), UBTComposite_Sequence::StaticClass() },
		{ TEXT("Selector"), UBTComposite_Selector::StaticClass() },
		{ TEXT("Wait"),     UBTTask_Wait::StaticClass() },
		{ TEXT("MoveTo"),   UBTTask_MoveTo::StaticClass() },
		{ TEXT("RunEQS"),   UBTTask_RunEQSQuery::StaticClass() },
		// M5 native node library
		{ TEXT("ComputeValue"), UBTForgeTask_ComputeValue::StaticClass() },
		{ TEXT("Log"),          UBTForgeTask_LogMessage::StaticClass() },
	};
	return Types;
}

const TMap<FString, UClass*>& UBTForgeLibrary::GetAuxTypeAliases(const FString& Kind)
{
	if (Kind == TEXT("decorator"))
	{
		static const TMap<FString, UClass*> Decorators =
		{
			{ TEXT("Blackboard"),    UBTDecorator_Blackboard::StaticClass() },
			// M5 native node library
			{ TEXT("FloatCompare"),  UBTForgeDecorator_FloatCompare::StaticClass() },
			{ TEXT("DistanceCheck"), UBTForgeDecorator_DistanceCheck::StaticClass() },
			{ TEXT("Chance"),        UBTForgeDecorator_Chance::StaticClass() },
		};
		return Decorators;
	}
	static const TMap<FString, UClass*> Services =
	{
		{ TEXT("RunEQS"),         UBTService_RunEQS::StaticClass() },
		// M5 native node library
		{ TEXT("SetValue"),       UBTForgeService_SetBlackboardValue::StaticClass() },
		{ TEXT("UpdateDistance"), UBTForgeService_UpdateDistance::StaticClass() },
	};
	return Services;
}

FString UBTForgeLibrary::CreateBehaviorTreeAsset(const FString& AssetPath, const FString& BlackboardJson)
{
	FString PackagePath;
	FString AssetName;
	if (!SplitAssetPath(AssetPath, PackagePath, AssetName))
	{
		return MakeErrorJson(TEXT("AssetPath must look like /Game/<Folder>/<Name>, got: ") + AssetPath);
	}
	if (FindObject<UBehaviorTree>(nullptr, *AssetPath) != nullptr)
	{
		return MakeErrorJson(TEXT("BehaviorTree asset already exists: ") + AssetPath + TEXT(". Use BuildBehaviorTree to (re)build its tree, or pick a new path."));
	}

	const FString BlackboardPath = AssetPath + TEXT("_BB");
	FString BlackboardPackagePath;
	FString BlackboardAssetName;
	if (!SplitAssetPath(BlackboardPath, BlackboardPackagePath, BlackboardAssetName))
	{
		return MakeErrorJson(TEXT("Internal: derived blackboard path is invalid: ") + BlackboardPath);
	}
	if (FindObject<UBlackboardData>(nullptr, *BlackboardPath) != nullptr)
	{
		return MakeErrorJson(TEXT("Blackboard asset already exists: ") + BlackboardPath);
	}

	// Parse and fully validate the blackboard spec before creating anything.
	TSharedPtr<FJsonObject> BlackboardSpec;
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BlackboardJson);
		if (!FJsonSerializer::Deserialize(Reader, BlackboardSpec) || !BlackboardSpec.IsValid())
		{
			return MakeErrorJson(TEXT("BlackboardJson is not valid JSON. Expected {\"keys\":[{\"name\":\"...\",\"type\":\"...\"}]}"));
		}
	}
	if (!BlackboardSpec->HasField(TEXT("keys")))
	{
		return MakeErrorJson(TEXT("BlackboardJson must contain a 'keys' array (may be empty)."));
	}
	const TArray<TSharedPtr<FJsonValue>>* RawKeySpecs = nullptr;
	if (!BlackboardSpec->TryGetArrayField(TEXT("keys"), RawKeySpecs) || RawKeySpecs == nullptr)
	{
		return MakeErrorJson(TEXT("BlackboardJson.keys: expected an array of {name,type} objects"));
	}
	const TArray<TSharedPtr<FJsonValue>>& KeySpecs = *RawKeySpecs;

	struct FKeySpec
	{
		FString Name;
		FString Type;
		FString Base;
		UClass* ResolvedBase = nullptr;
	};
	TArray<FKeySpec> ValidatedKeys;
	TSet<FName> SeenNames;
	// Probe what a fresh blackboard already contains (SelfActor today) without creating
	// the asset: the same PostInitProperties path runs for a transient object.
	TSet<FName> ExistingKeys;
	CollectExistingBlackboardKeys(NewObject<UBlackboardData>(GetTransientPackage()), ExistingKeys);
	for (int32 KeyIndex = 0; KeyIndex < KeySpecs.Num(); KeyIndex++)
	{
		const TSharedPtr<FJsonObject>* KeyObject;
		if (!KeySpecs[KeyIndex]->TryGetObject(KeyObject) || !KeyObject->IsValid())
		{
			return MakeErrorJson(FString::Printf(TEXT("keys[%d]: expected an object"), KeyIndex));
		}
		FKeySpec Spec;
		if (!(*KeyObject)->TryGetStringField(TEXT("name"), Spec.Name) || Spec.Name.IsEmpty())
		{
			return MakeErrorJson(FString::Printf(TEXT("keys[%d]: missing 'name'"), KeyIndex));
		}
		if (!(*KeyObject)->TryGetStringField(TEXT("type"), Spec.Type) || ResolveBlackboardKeyType(Spec.Type) == nullptr)
		{
			return MakeErrorJson(FString::Printf(TEXT("keys[%d] ('%s'): unknown type '%s'. Allowed: Bool, Int, Float, String, Name, Vector, Rotator, Object"),
				KeyIndex, *Spec.Name, *Spec.Type));
		}
		// Optional base-class declaration for Object keys ("base": "LyraCharacter"
		// or a full object path). UBlackboardKeyType_Object::BaseClass defaults to
		// UObject, which the editor's PreSave filter check rejects for node
		// constructors that only accept Actor-derived keys (the RunEQS service's
		// Object(AActor) filter rewrote TargetEnemy to SelfActor at save time).
		// A declared base makes the binding survive and narrows the semantics.
		(*KeyObject)->TryGetStringField(TEXT("base"), Spec.Base);
		Spec.Base.TrimStartAndEndInline();
		if (!Spec.Base.IsEmpty())
		{
			if (Spec.Type != TEXT("Object"))
			{
				return MakeErrorJson(FString::Printf(TEXT("keys[%d] ('%s'): 'base' is only supported for Object keys (this key is %s)"),
					KeyIndex, *Spec.Name, *Spec.Type));
			}
			Spec.ResolvedBase = ResolveClassLoose(Spec.Base);
			if (Spec.ResolvedBase == nullptr)
			{
				return MakeErrorJson(FString::Printf(TEXT("keys[%d] ('%s'): base: could not load class '%s'. Use a short class name or a full object path like /Script/Module.ClassName"),
					KeyIndex, *Spec.Name, *Spec.Base));
			}
		}
		const FName KeyFName(*Spec.Name);
		if (ExistingKeys.Contains(KeyFName))
		{
			return MakeErrorJson(ExistingBlackboardKeyError(KeyIndex, Spec.Name, ExistingKeys));
		}
		if (SeenNames.Contains(KeyFName))
		{
			return MakeErrorJson(FString::Printf(TEXT("keys[%d]: duplicate key name '%s'"), KeyIndex, *Spec.Name));
		}
		SeenNames.Add(KeyFName);
		ValidatedKeys.Add(Spec);
	}

	FAssetToolsModule& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	UEditorAssetSubsystem* AssetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
	if (AssetSubsystem == nullptr)
	{
		// No subsystem means no save *and* no cleanup: creating the assets anyway would
		// leave two unsaved objects that vanish on the next editor restart while the
		// caller believes they exist.
		return MakeErrorJson(TEXT("EditorAssetSubsystem unavailable; nothing was created: ") + AssetPath);
	}

	UBlackboardData* Blackboard = Cast<UBlackboardData>(
		AssetTools.Get().CreateAsset(BlackboardAssetName, BlackboardPackagePath, UBlackboardData::StaticClass(), nullptr));
	if (Blackboard == nullptr)
	{
		return MakeErrorJson(TEXT("Failed to create blackboard asset at: ") + BlackboardPath);
	}
	for (const FKeySpec& Spec : ValidatedKeys)
	{
		FBlackboardEntry& Entry = Blackboard->Keys.AddDefaulted_GetRef();
		Entry.EntryName = FName(*Spec.Name);
		Entry.KeyType = NewObject<UBlackboardKeyType>(Blackboard, ResolveBlackboardKeyType(Spec.Type));
		if (Spec.ResolvedBase != nullptr)
		{
			if (UBlackboardKeyType_Object* ObjectKey = Cast<UBlackboardKeyType_Object>(Entry.KeyType))
			{
				ObjectKey->BaseClass = Spec.ResolvedBase;
			}
		}
	}
	Blackboard->UpdateKeyIDs();

	UBehaviorTree* Tree = Cast<UBehaviorTree>(
		AssetTools.Get().CreateAsset(AssetName, PackagePath, UBehaviorTree::StaticClass(), nullptr));
	if (Tree == nullptr)
	{
		// The blackboard already exists on disk at this point. Leaving it behind makes
		// every retry fail with "Blackboard asset already exists" for a path the caller
		// never got anything from, so the whole pair is rolled back.
		AssetSubsystem->DeleteAsset(BlackboardPath);
		return MakeErrorJson(TEXT("Failed to create BehaviorTree asset at: ") + AssetPath);
	}
	Tree->BlackboardAsset = Blackboard;

	if (!AssetSubsystem->SaveLoadedAsset(Blackboard, false) || !AssetSubsystem->SaveLoadedAsset(Tree, false))
	{
		// A silent save failure reports ok:true for assets that are gone after a
		// restart: drop the pair and report the failure instead.
		AssetSubsystem->DeleteAsset(AssetPath);
		AssetSubsystem->DeleteAsset(BlackboardPath);
		return MakeErrorJson(TEXT("created but failed to save; nothing was kept: ") + AssetPath);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("behaviorTree"), AssetPath);
	Result->SetStringField(TEXT("blackboard"), BlackboardPath);
	Result->SetNumberField(TEXT("keyCount"), ValidatedKeys.Num());
	return JsonToString(Result.ToSharedRef());
}

FString UBTForgeLibrary::BuildBehaviorTree(const FString& AssetPath, const FString& TreeJson)
{
	UBehaviorTree* Tree = LoadObject<UBehaviorTree>(nullptr, *AssetPath);
	if (Tree == nullptr)
	{
		return MakeErrorJson(TEXT("BehaviorTree asset not found: ") + AssetPath + TEXT(". Call CreateBehaviorTreeAsset first."));
	}

	TSharedPtr<FJsonObject> RootSpec;
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(TreeJson);
		FString ParseError;
		if (!FJsonSerializer::Deserialize(Reader, RootSpec) || !RootSpec.IsValid())
		{
			return MakeErrorJson(TEXT("TreeJson is not valid JSON. It must be the root node object, e.g. {\"type\":\"Selector\",\"children\":[...]}"));
		}
	}

	int32 NodeCount = 0;
	FString Error;
	// Collect the tree's blackboard key names once: every key-reference write
	// is validated against this set before it happens.
	TSet<FName> KnownBlackboardKeys;
	if (Tree->BlackboardAsset != nullptr)
	{
		for (const FBlackboardEntry& Entry : Tree->BlackboardAsset->Keys)
		{
			KnownBlackboardKeys.Add(Entry.EntryName);
		}
	}

	// The root has no parent slot, so root-level "decorators" would be silently
	// dropped - reject up front instead.
	if (RootSpec->HasField(TEXT("decorators")))
	{
		return MakeErrorJson(TEXT("root: 'decorators' gate a child's entry in its parent's children array; the root node has no parent slot"));
	}

	TMap<FString, FString> ExternalKeys;
	TMap<FName, TMap<FName, bool>> NodeEffects;
	// Build has no blackboard JSON (the asset already owns its keys): declarations
	// come from the tree root here; ValidateBehaviorTree accepts either place.
	if (!ParseKeyDeclarations(RootSpec, nullptr, ExternalKeys, NodeEffects, Error))
	{
		return MakeErrorJson(Error);
	}

	UBTNode* NewRootNode = BuildNodeRecursive(Tree, KnownBlackboardKeys, RootSpec, TEXT("root"), NodeCount, Error);
	if (NewRootNode == nullptr)
	{
		return MakeErrorJson(Error);
	}
	UBTCompositeNode* NewRoot = Cast<UBTCompositeNode>(NewRootNode);
	if (NewRoot == nullptr)
	{
		return MakeErrorJson(TEXT("root node must be a composite (Sequence/Selector)"));
	}

	// Dataflow analysis runs BEFORE the tree is touched: a fatal finding leaves
	// the saved asset completely untouched (failure does not persist).
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetNumberField(TEXT("nodeCount"), NodeCount);
	Result->SetStringField(TEXT("root"), NewRoot->GetClass()->GetName());
	const FString ResultJson = FinishWithKeyDataflow(Tree, NewRoot, RootSpec, ExternalKeys, NodeEffects, Result);
	{
		TSharedPtr<FJsonObject> Parsed;
		bool bOk = true;
		if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ResultJson), Parsed) && Parsed.IsValid())
		{
			Parsed->TryGetBoolField(TEXT("ok"), bOk);
		}
		if (!bOk)
		{
			return ResultJson; // dead gate: the saved tree is untouched
		}
	}

	Tree->RootNode = NewRoot;
#if WITH_EDITORONLY_DATA
	// The editor graph is only a view over the data chain. Dropping it makes the
	// editor rebuild it from RootNode when the asset is opened; keeping a stale
	// graph across rebuilds leaves graph nodes pointing at replaced node instances,
	// which the editor shows as "class not found" while the tree itself still runs.
	Tree->BTGraph = nullptr;
#endif
	InitializeNodesRecursive(NewRoot, Tree);
	if (NewRoot->Services.Num() > 0 && NewRoot->Services[0])
	{
		FName DbgKey;
		FProperty* DbgProp = NewRoot->Services[0]->GetClass()->FindPropertyByName(TEXT("BlackboardKey"));
		if (DbgProp && ReadSelectorKeyName(DbgProp, DbgProp->ContainerPtrToValuePtr<void>(NewRoot->Services[0]), DbgKey))
		{
			UE_LOG(LogTemp, Warning, TEXT("[BTForge diag] post-init root.services[0] BlackboardKey = '%s'"), *DbgKey.ToString());
		}
	}
	if (UEditorAssetSubsystem* AssetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr)
	{
		if (!AssetSubsystem->SaveLoadedAsset(Tree, false))
		{
			return MakeErrorJson(TEXT("Tree built in memory but saving failed: ") + AssetPath);
		}
	}
	// else: tree built in memory; next editor save will persist it.

	return ResultJson;
}

FString UBTForgeLibrary::RepairEditorGraph(const FString& AssetPath)
{
	UBehaviorTree* Tree = LoadObject<UBehaviorTree>(nullptr, *AssetPath);
	if (Tree == nullptr)
	{
		return MakeErrorJson(TEXT("BehaviorTree asset not found: ") + AssetPath);
	}
	// An open graph editor holds the old graph in memory and would write it back
	// over this repair on its next save (the "graph wins" rule): refuse instead
	// of racing it. Checked before any mutation so a refusal leaves the asset as-is.
	if (GEditor != nullptr)
	{
		if (UAssetEditorSubsystem* AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			if (AssetEditors->FindEditorForAsset(Tree, /*bFocusIfOpen=*/false) != nullptr)
			{
				return MakeErrorJson(TEXT("asset is open in an editor; close its tab first - an open graph would overwrite this repair on save"));
			}
		}
	}
#if WITH_EDITORONLY_DATA
	const bool bHadGraph = Tree->BTGraph != nullptr;
	Tree->BTGraph = nullptr;
#else
	const bool bHadGraph = false;
#endif
	UEditorAssetSubsystem* AssetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
	if (AssetSubsystem == nullptr)
	{
		return MakeErrorJson(TEXT("EditorAssetSubsystem unavailable; graph cleared in memory but not saved: ") + AssetPath);
	}
	Tree->MarkPackageDirty();
	if (!AssetSubsystem->SaveLoadedAsset(Tree, false))
	{
		return MakeErrorJson(TEXT("could not save the repaired asset: ") + AssetPath);
	}
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("asset"), AssetPath);
	Result->SetBoolField(TEXT("graphCleared"), bHadGraph);
	Result->SetStringField(TEXT("note"), TEXT("stale editor graph dropped; the Behavior Tree editor rebuilds the graph from the data chain on next open"));
	return JsonToString(Result);
}

FString UBTForgeLibrary::InspectBehaviorTree(const FString& AssetPath)
{
	UBehaviorTree* Tree = LoadObject<UBehaviorTree>(nullptr, *AssetPath);
	if (Tree == nullptr)
	{
		return MakeErrorJson(TEXT("BehaviorTree asset not found: ") + AssetPath);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);

	if (Tree->BlackboardAsset)
	{
		TArray<TSharedPtr<FJsonValue>> Keys;
		for (const FBlackboardEntry& Entry : Tree->BlackboardAsset->Keys)
		{
			TSharedRef<FJsonObject> KeyObject = MakeShared<FJsonObject>();
			KeyObject->SetStringField(TEXT("name"), Entry.EntryName.ToString());
			KeyObject->SetStringField(TEXT("type"), Entry.KeyType ? Entry.KeyType->GetClass()->GetName() : TEXT("(none)"));
			if (const UBlackboardKeyType_Object* ObjectKey = Cast<UBlackboardKeyType_Object>(Entry.KeyType))
			{
				KeyObject->SetStringField(TEXT("base"), ObjectKey->BaseClass ? ObjectKey->BaseClass->GetPathName() : TEXT("(any)"));
			}
			Keys.Add(MakeShared<FJsonValueObject>(KeyObject));
		}
		Result->SetArrayField(TEXT("blackboardKeys"), Keys);
	}

	if (Tree->RootNode)
	{
		Result->SetObjectField(TEXT("root"), DumpNode(Tree->RootNode, Tree->BlackboardAsset));
	}
	else
	{
		Result->SetField(TEXT("root"), MakeShared<FJsonValueNull>());
	}
	return JsonToString(Result.ToSharedRef());
}

FString UBTForgeLibrary::ValidateBehaviorTree(const FString& TreeJson, const FString& BlackboardJson)
{
	TSharedPtr<FJsonObject> RootSpec;
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(TreeJson);
		if (!FJsonSerializer::Deserialize(Reader, RootSpec) || !RootSpec.IsValid())
		{
			return MakeErrorJson(TEXT("TreeJson is not valid JSON. It must be the root node object, e.g. {\"type\":\"Selector\",\"children\":[...]}"));
		}
	}

	// Build a transient blackboard (same rules as CreateBehaviorTreeAsset, minus
	// asset creation) so specs can be validated before any asset exists.
	TSharedPtr<FJsonObject> BlackboardSpec;
	const FString BlackboardText = BlackboardJson.IsEmpty() ? TEXT("{\"keys\":[]}") : BlackboardJson;
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BlackboardText);
		if (!FJsonSerializer::Deserialize(Reader, BlackboardSpec) || !BlackboardSpec.IsValid())
		{
			return MakeErrorJson(TEXT("BlackboardJson is not valid JSON. Expected {\"keys\":[{\"name\":\"...\",\"type\":\"...\"}]}"));
		}
	}
	if (!BlackboardSpec->HasField(TEXT("keys")))
	{
		return MakeErrorJson(TEXT("BlackboardJson must contain a 'keys' array (may be empty)."));
	}

	UBlackboardData* Blackboard = NewObject<UBlackboardData>(GetTransientPackage());
	// The engine already added its persistent keys (SelfActor) in PostInitProperties:
	// they are valid node key references and must not be re-declared by the spec.
	TSet<FName> ExistingKeys;
	CollectExistingBlackboardKeys(Blackboard, ExistingKeys);
	TSet<FName> KnownBlackboardKeys = ExistingKeys;
	const TArray<TSharedPtr<FJsonValue>>* RawKeySpecs = nullptr;
	if (!BlackboardSpec->TryGetArrayField(TEXT("keys"), RawKeySpecs) || RawKeySpecs == nullptr)
	{
		return MakeErrorJson(TEXT("BlackboardJson.keys: expected an array of {name,type} objects"));
	}
	const TArray<TSharedPtr<FJsonValue>>& KeySpecs = *RawKeySpecs;
	for (int32 KeyIndex = 0; KeyIndex < KeySpecs.Num(); ++KeyIndex)
	{
		const TSharedPtr<FJsonObject>* KeyObject = nullptr;
		if (!KeySpecs[KeyIndex]->TryGetObject(KeyObject) || !KeyObject->IsValid())
		{
			return MakeErrorJson(FString::Printf(TEXT("keys[%d]: expected an object"), KeyIndex));
		}
		FString KeyName;
		FString KeyType;
		if (!(*KeyObject)->TryGetStringField(TEXT("name"), KeyName) || KeyName.IsEmpty())
		{
			return MakeErrorJson(FString::Printf(TEXT("keys[%d]: missing 'name'"), KeyIndex));
		}
		if (!(*KeyObject)->TryGetStringField(TEXT("type"), KeyType) || ResolveBlackboardKeyType(KeyType) == nullptr)
		{
			return MakeErrorJson(FString::Printf(TEXT("keys[%d] ('%s'): unknown type '%s'. Allowed: Bool, Int, Float, String, Name, Vector, Rotator, Object"),
				KeyIndex, *KeyName, *KeyType));
		}
		FString KeyBase;
		(*KeyObject)->TryGetStringField(TEXT("base"), KeyBase);
		KeyBase.TrimStartAndEndInline();
		UClass* ResolvedBase = nullptr;
		if (!KeyBase.IsEmpty())
		{
			if (KeyType != TEXT("Object"))
			{
				return MakeErrorJson(FString::Printf(TEXT("keys[%d] ('%s'): 'base' is only supported for Object keys (this key is %s)"),
					KeyIndex, *KeyName, *KeyType));
			}
			ResolvedBase = ResolveClassLoose(KeyBase);
			if (ResolvedBase == nullptr)
			{
				return MakeErrorJson(FString::Printf(TEXT("keys[%d] ('%s'): base: could not load class '%s'. Use a short class name or a full object path like /Script/Module.ClassName"),
					KeyIndex, *KeyName, *KeyBase));
			}
		}
		const FName KeyFName(*KeyName);
		if (ExistingKeys.Contains(KeyFName))
		{
			return MakeErrorJson(ExistingBlackboardKeyError(KeyIndex, KeyName, ExistingKeys));
		}
		if (KnownBlackboardKeys.Contains(KeyFName))
		{
			return MakeErrorJson(FString::Printf(TEXT("keys[%d]: duplicate key name '%s'"), KeyIndex, *KeyName));
		}
		KnownBlackboardKeys.Add(KeyFName);
		FBlackboardEntry& Entry = Blackboard->Keys.AddDefaulted_GetRef();
		Entry.EntryName = FName(*KeyName);
		Entry.KeyType = NewObject<UBlackboardKeyType>(Blackboard, ResolveBlackboardKeyType(KeyType));
		if (ResolvedBase != nullptr)
		{
			if (UBlackboardKeyType_Object* ObjectKey = Cast<UBlackboardKeyType_Object>(Entry.KeyType))
			{
				ObjectKey->BaseClass = ResolvedBase;
			}
		}
	}
	Blackboard->UpdateKeyIDs();

	UBehaviorTree* TransientTree = NewObject<UBehaviorTree>(GetTransientPackage());
	TransientTree->BlackboardAsset = Blackboard;

	if (RootSpec->HasField(TEXT("decorators")))
	{
		return MakeErrorJson(TEXT("root: 'decorators' gate a child's entry in its parent's children array; the root node has no parent slot"));
	}

	int32 NodeCount = 0;
	FString Error;
	TMap<FString, FString> ExternalKeys;
	TMap<FName, TMap<FName, bool>> NodeEffects;
	if (!ParseKeyDeclarations(RootSpec, BlackboardSpec, ExternalKeys, NodeEffects, Error))
	{
		return MakeErrorJson(Error);
	}
	UBTNode* NewRootNode = BuildNodeRecursive(TransientTree, KnownBlackboardKeys, RootSpec, TEXT("root"), NodeCount, Error);
	if (NewRootNode == nullptr)
	{
		return MakeErrorJson(Error);
	}
	UBTCompositeNode* NewRoot = Cast<UBTCompositeNode>(NewRootNode);
	if (NewRoot == nullptr)
	{
		return MakeErrorJson(TEXT("root node must be a composite (Sequence/Selector)"));
	}
	TransientTree->RootNode = NewRoot;
	InitializeNodesRecursive(NewRoot, TransientTree);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetNumberField(TEXT("nodeCount"), NodeCount);
	Result->SetStringField(TEXT("root"), NewRoot->GetClass()->GetName());
	TArray<TSharedPtr<FJsonValue>> KeyResults;
	for (const FName& KeyName : KnownBlackboardKeys)
	{
		KeyResults.Add(MakeShared<FJsonValueString>(KeyName.ToString()));
	}
	Result->SetArrayField(TEXT("blackboardKeys"), KeyResults);
	return FinishWithKeyDataflow(TransientTree, NewRoot, RootSpec, ExternalKeys, NodeEffects, Result.ToSharedRef());
}
