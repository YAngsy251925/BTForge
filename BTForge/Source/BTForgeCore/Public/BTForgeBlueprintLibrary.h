// BT Forge blueprint node generation (M6, part 1: data-only subclasses).
//
// Projects express their own node types as Blueprint subclasses of BT node
// base classes (Lyra's BTS_*/BTT_* convention). Authoring those by hand breaks
// the "agent writes one spec" loop, so this library creates data-only Blueprint
// assets programmatically: a generated class derived from a BT node base, with
// per-project defaults written onto the class CDO and no event graph logic.
// The resulting class path (/Game/..._C) plugs straight into build_behavior_tree
// "type", the capability catalog lists it automatically (AssetRegistry), and
// PIE verification proves the defaults actually drive runtime behavior.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "BTForgeBlueprintLibrary.generated.h"

UCLASS()
class BTFORGECORE_API UBTForgeBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Create a data-only Blueprint node asset: a generated class derived from a
	 * BT node base class (UBTTaskNode / UBTService / UBTDecorator — Blueprint
	 * composites do not exist in the engine), with ParamsJson written onto the
	 * class CDO through the same reflection writers build_behavior_tree uses
	 * (enums by name, class paths, nested structs). No event-graph logic is
	 * generated. Fails if the asset already exists; nothing is kept on failure
	 * (the half-created asset is deleted).
	 *
	 * Blackboard key names inside params are accepted without tree validation:
	 * a CDO is tree-independent; the key is resolved per-tree when the node is
	 * placed into a behavior tree.
	 *
	 * @param AssetPath       Target blueprint asset, e.g. "/Game/AI/BTA_LogShot".
	 * @param ParentClassPath Class path of a non-abstract UBTTaskNode/UBTService/
	 *                        UBTDecorator-derived class (/Script/... or /Game/..._C).
	 * @param ParamsJson      {"PropertyName": value, ...} applied to the class
	 *                        defaults, e.g. {"Message":"shot fired","Severity":"Warning"}.
	 * @return JSON {"ok":true,"assetPath":...,"classPath":"..._C","parent":...,
	 *         "appliedParams":N} or {"ok":false,"error":msg}
	 */
	UFUNCTION(BlueprintCallable, Category = "BTForge")
	static FString CreateNodeBlueprint(const FString& AssetPath, const FString& ParentClassPath, const FString& ParamsJson);
};
