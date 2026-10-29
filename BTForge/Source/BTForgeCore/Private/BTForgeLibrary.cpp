#include "BTForgeLibrary.h"

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
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Tasks/BTTask_MoveTo.h"
#include "BehaviorTree/Tasks/BTTask_RunEQSQuery.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
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
		static const TMap<FString, UClass*> Types =
		{
			{ TEXT("Sequence"), UBTComposite_Sequence::StaticClass() },
			{ TEXT("Selector"), UBTComposite_Selector::StaticClass() },
			{ TEXT("Wait"),     UBTTask_Wait::StaticClass() },
			{ TEXT("MoveTo"),   UBTTask_MoveTo::StaticClass() },
			{ TEXT("RunEQS"),   UBTTask_RunEQSQuery::StaticClass() },
		};
		const UClass* const* Found = Types.Find(TypeName);
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
		static const TMap<FString, UClass*> Decorators =
		{
			{ TEXT("Blackboard"), UBTDecorator_Blackboard::StaticClass() },
		};
		static const TMap<FString, UClass*> Services =
		{
			{ TEXT("RunEQS"), UBTService_RunEQS::StaticClass() },
		};
		const TMap<FString, UClass*>& Table = Kind == TEXT("decorator") ? Decorators : Services;
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
			? TEXT("decorators: Blackboard, or a '/path/Asset.Asset_C' class path (any non-abstract UBTDecorator-derived class). Multiple decorators on one child combine as AND")
			: TEXT("services: RunEQS, or a '/path/Asset.Asset_C' class path (any non-abstract UBTService-derived class)");
	}

	FString WhitelistText()
	{
		return TEXT("Sequence, Selector, Wait, MoveTo, RunEQS, or a '/path/Asset.Asset_C' class path (any non-abstract UBTCompositeNode/UBTTaskNode-derived class, engine or blueprint)");
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

	bool ApplyParamsToContainer(UStruct* Struct, void* DataPtr, const TSharedPtr<FJsonObject>& Params, const TSet<FName>& KnownBlackboardKeys, const FString& Path, FString& OutError);

	bool WriteValueToProperty(FProperty* Property, void* DataPtr, const TSharedPtr<FJsonValue>& Value, const TSet<FName>& KnownBlackboardKeys, const FString& ParamPath, FString& OutError)
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
			// FValueOrBBKey_* family: accept a plain value, write the nested DefaultValue member.
			if (FProperty* DefaultProperty = StructProperty->Struct->FindPropertyByName(TEXT("DefaultValue")))
			{
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
				if (!KnownBlackboardKeys.Contains(FName(*KeyName)))
				{
					TArray<FString> Known;
					for (const FName& Known2 : KnownBlackboardKeys)
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

	bool ApplyParamsToContainer(UStruct* Struct, void* DataPtr, const TSharedPtr<FJsonObject>& Params, const TSet<FName>& KnownBlackboardKeys, const FString& Path, FString& OutError)
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

		const TSharedPtr<FJsonObject> Params = (*SpecObject)->HasField(TEXT("params"))
			? (*SpecObject)->GetObjectField(TEXT("params"))
			: MakeShared<FJsonObject>();
		if (!ApplyParamsToContainer(AuxNode->GetClass(), AuxNode, Params, KnownBlackboardKeys, NodePath, OutError))
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

		const TSharedPtr<FJsonObject> Params = NodeJson->HasField(TEXT("params"))
			? NodeJson->GetObjectField(TEXT("params"))
			: MakeShared<FJsonObject>();
		if (!ApplyParamsToContainer(Node->GetClass(), Node, Params, KnownBlackboardKeys, NodePath, OutError))
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
			if (NodeJson->HasField(TEXT("services")))
			{
				const TArray<TSharedPtr<FJsonValue>>& ServiceSpecs = NodeJson->GetArrayField(TEXT("services"));
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
			}

			if (!NodeJson->HasField(TEXT("children")))
			{
				OutError = NodePath + TEXT(": composite requires a non-empty 'children' array");
				return nullptr;
			}
			const TArray<TSharedPtr<FJsonValue>>& Children = NodeJson->GetArrayField(TEXT("children"));
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
				if ((*ChildObject)->HasField(TEXT("decorators")))
				{
					const TArray<TSharedPtr<FJsonValue>>& DecoratorSpecs = (*ChildObject)->GetArrayField(TEXT("decorators"));
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

	void InitializeNodesRecursive(UBTNode* Node, UBehaviorTree* Tree)
	{
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
			// FBlackboardKeySelector key-name readback is temporarily disabled
			// while hunting a deterministic abort (Core.dll fast-fail) triggered
			// by inspecting Lyra's bot tree. Struct readback is limited to the
			// FValueOrBBKey numeric half until the culprit is identified.
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

	TSharedRef<FJsonObject> DumpAuxNode(UBTNode* AuxNode)
	{
		TSharedRef<FJsonObject> OutNode = MakeShared<FJsonObject>();
		OutNode->SetStringField(TEXT("type"), AuxNode->GetClass()->GetName());
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		DumpScalarProperties(AuxNode, Params);
		OutNode->SetObjectField(TEXT("params"), Params);
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

	TSharedRef<FJsonObject> DumpNode(UBTNode* Node)
	{
		TSharedRef<FJsonObject> OutNode = MakeShared<FJsonObject>();
		OutNode->SetStringField(TEXT("type"), Node->GetClass()->GetName());
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		DumpScalarProperties(Node, Params);
		OutNode->SetObjectField(TEXT("params"), Params);

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
						Services.Add(MakeShared<FJsonValueObject>(DumpAuxNode(const_cast<UBTService*>(Service))));
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
					TSharedRef<FJsonObject> ChildJson = DumpNode(const_cast<UBTNode*>(ChildNode));

					// Decorators gate entry to this child branch; DecoratorOps is
					// the stack-encoded logic combination over the decorator list.
					if (Child.Decorators.Num() > 0)
					{
						TArray<TSharedPtr<FJsonValue>> Decorators;
						for (const UBTDecorator* Decorator : Child.Decorators)
						{
							if (Decorator)
							{
								Decorators.Add(MakeShared<FJsonValueObject>(DumpAuxNode(const_cast<UBTDecorator*>(Decorator))));
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
	const TArray<TSharedPtr<FJsonValue>>& KeySpecs = BlackboardSpec->GetArrayField(TEXT("keys"));

	struct FKeySpec
	{
		FString Name;
		FString Type;
	};
	TArray<FKeySpec> ValidatedKeys;
	TSet<FString> SeenNames;
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
		if (SeenNames.Contains(Spec.Name))
		{
			return MakeErrorJson(FString::Printf(TEXT("keys[%d]: duplicate key name '%s'"), KeyIndex, *Spec.Name));
		}
		SeenNames.Add(Spec.Name);
		ValidatedKeys.Add(Spec);
	}

	FAssetToolsModule& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));

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
	}
	Blackboard->UpdateKeyIDs();

	UBehaviorTree* Tree = Cast<UBehaviorTree>(
		AssetTools.Get().CreateAsset(AssetName, PackagePath, UBehaviorTree::StaticClass(), nullptr));
	if (Tree == nullptr)
	{
		return MakeErrorJson(TEXT("Failed to create BehaviorTree asset at: ") + AssetPath);
	}
	Tree->BlackboardAsset = Blackboard;

	if (UEditorAssetSubsystem* AssetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr)
	{
		AssetSubsystem->SaveLoadedAsset(Blackboard, false);
		AssetSubsystem->SaveLoadedAsset(Tree, false);
	}
	else
	{
		return MakeErrorJson(TEXT("EditorAssetSubsystem unavailable; assets created but not saved: ") + AssetPath);
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

	Tree->RootNode = NewRoot;
	InitializeNodesRecursive(NewRoot, Tree);
	if (UEditorAssetSubsystem* AssetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr)
	{
		if (!AssetSubsystem->SaveLoadedAsset(Tree, false))
		{
			return MakeErrorJson(TEXT("Tree built in memory but saving failed: ") + AssetPath);
		}
	}
	// else: tree built in memory; next editor save will persist it.

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetNumberField(TEXT("nodeCount"), NodeCount);
	Result->SetStringField(TEXT("root"), NewRoot->GetClass()->GetName());
	return JsonToString(Result.ToSharedRef());
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
			Keys.Add(MakeShared<FJsonValueObject>(KeyObject));
		}
		Result->SetArrayField(TEXT("blackboardKeys"), Keys);
	}

	if (Tree->RootNode)
	{
		Result->SetObjectField(TEXT("root"), DumpNode(Tree->RootNode));
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
	TSet<FName> KnownBlackboardKeys;
	const TArray<TSharedPtr<FJsonValue>>& KeySpecs = BlackboardSpec->GetArrayField(TEXT("keys"));
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
		if (KnownBlackboardKeys.Contains(FName(*KeyName)))
		{
			return MakeErrorJson(FString::Printf(TEXT("keys[%d]: duplicate key name '%s'"), KeyIndex, *KeyName));
		}
		KnownBlackboardKeys.Add(FName(*KeyName));
		FBlackboardEntry& Entry = Blackboard->Keys.AddDefaulted_GetRef();
		Entry.EntryName = FName(*KeyName);
		Entry.KeyType = NewObject<UBlackboardKeyType>(Blackboard, ResolveBlackboardKeyType(KeyType));
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
	return JsonToString(Result.ToSharedRef());
}
