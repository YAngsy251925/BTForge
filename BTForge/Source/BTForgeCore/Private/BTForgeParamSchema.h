// Shared parameter-schema helpers for BT Forge.
//
// The same reflection knowledge is needed in two places:
//  - UBTForgeEQSLibrary uses it to explain expected values in error messages;
//  - UBTForgeCatalogLibrary uses it to tell an agent, before it writes a spec,
//    what JSON shape a node parameter accepts.
// Keeping one implementation prevents the catalog from drifting away from what
// the writers actually accept.

#pragma once

#include "CoreMinimal.h"
#include "UObject/UnrealType.h"

namespace BTForgeParamSchema
{
	/** Short, human-facing name of an enum entry ("EEnvTestPurpose::Filter" -> "Filter"). */
	inline FString EnumEntryShortName(const UEnum* Enum, int32 Index)
	{
		FString Name = Enum->GetNameStringByIndex(Index);
		int32 ColonIdx = INDEX_NONE;
		if (Name.FindLastChar(TEXT(':'), ColonIdx))
		{
			Name = Name.Mid(ColonIdx + 1);
		}
		return Name;
	}

	inline FString DescribeEnumValues(const UEnum* Enum)
	{
		FString Out;
		for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
		{
			const FString ShortName = EnumEntryShortName(Enum, Index);
			if (ShortName.EndsWith(TEXT("_MAX")) || ShortName.Equals(TEXT("MAX")))
			{
				continue;
			}
			if (!Out.IsEmpty())
			{
				Out += TEXT(", ");
			}
			Out += ShortName;
		}
		return Out;
	}

	/**
	 * Describe the JSON shape a spec parameter accepts, derived from the actual
	 * UPROPERTY type. Examples: "boolean", "number", "enum: Filter|Score|...",
	 * "class path (UEnvQueryContext)", "number (or object{DefaultValue})".
	 */
	inline FString GetJsonPropertyShape(const FProperty* Property)
	{
		if (Property == nullptr)
		{
			return TEXT("(unknown)");
		}
		if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			return TEXT("boolean");
		}
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			if (ByteProperty->Enum != nullptr)
			{
				return FString::Printf(TEXT("enum: %s"), *DescribeEnumValues(ByteProperty->Enum));
			}
			return TEXT("number");
		}
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			const UEnum* Enum = EnumProperty->GetEnum();
			return Enum != nullptr ? FString::Printf(TEXT("enum: %s"), *DescribeEnumValues(Enum)) : TEXT("enum");
		}
		if (Property->IsA<FNumericProperty>())
		{
			return TEXT("number");
		}
		if (Property->IsA<FStrProperty>())
		{
			return TEXT("string");
		}
		if (Property->IsA<FNameProperty>())
		{
			return TEXT("string (name)");
		}
		if (const FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
		{
			return FString::Printf(TEXT("class path (%s)"), ClassProperty->MetaClass ? *ClassProperty->MetaClass->GetName() : TEXT("UObject"));
		}
		if (const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
		{
			return FString::Printf(TEXT("object path (%s)"), ObjectProperty->PropertyClass ? *ObjectProperty->PropertyClass->GetName() : TEXT("UObject"));
		}
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct == nullptr)
			{
				return TEXT("(unloaded struct)");
			}
			if (StructProperty->Struct->FindPropertyByName(TEXT("SelectedKeyName")) != nullptr)
			{
				return TEXT("blackboard key name (string)");
			}
			if (const FProperty* DefaultProperty = StructProperty->Struct->FindPropertyByName(TEXT("DefaultValue")))
			{
				if (DefaultProperty->IsA<FNumericProperty>())
				{
					return TEXT("number (or object{DefaultValue})");
				}
			}
			return FString::Printf(TEXT("object {%s}"), *StructProperty->Struct->GetName());
		}
		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			return TEXT("array (unsupported in params)");
		}
		return FString::Printf(TEXT("unsupported (%s)"), *Property->GetCPPType());
	}
}
