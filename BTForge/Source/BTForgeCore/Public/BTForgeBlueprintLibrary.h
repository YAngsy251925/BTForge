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
	 * Create a Blueprint node asset: a generated class derived from a BT node
	 * base class, with ParamsJson written onto the class CDO through the same
	 * reflection writers build_behavior_tree uses (enums by name, class paths,
	 * nested structs), and - when RecipeJson is given - a constrained
	 * event-graph recipe synthesized into the event graph (M6 part 2).
	 * Fails if the asset already exists; nothing is kept on failure (the
	 * half-created asset is deleted).
	 *
	 * Recipes (the only kind in this release is "call_function"): on the node's
	 * main event (tasks: ReceiveExecuteAI, services: ReceiveActivationAI) call
	 * a zero-parameter BlueprintCallable function on the AI controller or its
	 * pawn (taken from the event's OwnerController/ControlledPawn pins), then -
	 * for tasks - finish with success. Graph shape is fixed; the only choices
	 * are the ones in RecipeJson:
	 *   {"recipe":"call_function",
	 *    "target":"controller"|"pawn",
	 *    "function":"ForceNetUpdate"}
	 * The function must exist on AAIController (controller target) or APawn
	 * (pawn target), be BlueprintCallable, non-pure, non-static, and take no
	 * parameters - everything else is rejected with an exact reason.
	 *
	 * Blackboard key names inside params are accepted without tree validation:
	 * a CDO is tree-independent; the key is resolved per-tree when the node is
	 * placed into a behavior tree.
	 *
	 * @param AssetPath       Target blueprint asset, e.g. "/Game/AI/BTA_LogShot".
	 * @param ParentClassPath Class path of a UBTTaskNode/UBTService/UBTDecorator-derived
	 *                        class (/Script/... or /Game/..._C). Abstract parents are
	 *                        allowed - "derive and implement" is the standard pattern and
	 *                        every engine *BlueprintBase class is abstract; the real gate
	 *                        is FKismetEditorUtilities::CanCreateBlueprintOfClass, so
	 *                        non-Blueprintable engine BT classes are rejected. Recipes
	 *                        additionally require the *BlueprintBase class.
	 * @param ParamsJson      {"PropertyName": value, ...} applied to the class
	 *                        defaults, e.g. {"Message":"shot fired","Severity":"Warning"}.
	 * @param RecipeJson      Optional constrained event-graph recipe (see above).
	 * @return JSON {"ok":true,"assetPath":...,"classPath":"..._C","parent":...,
	 *         "appliedParams":N,"recipe":...} or {"ok":false,"error":msg}
	 */
	UFUNCTION(BlueprintCallable, Category = "BTForge")
	static FString CreateNodeBlueprint(const FString& AssetPath, const FString& ParentClassPath, const FString& ParamsJson, const FString& RecipeJson);
};
