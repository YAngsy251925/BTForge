// BT Forge core: deterministic Behavior Tree / Blackboard builder from JSON specs.
//
// Design contract:
// - The agent produces a BTSpec (JSON). This library is the only writer.
// - Building happens on the BT data chain only (RootNode/Children arrays).
//   The editor graph (BTGraph) is intentionally left empty: opening the asset
//   in the Behavior Tree editor auto-spawns graph nodes that reuse the data
//   chain node instances (UBehaviorTreeGraph::SpawnMissingNodes), so no graph
//   editing API is needed here.
// - Everything is validated before any UObject is created; on failure nothing
//   is saved and a path-precise error is returned for agent self-correction.
//
// This module is engine-version agnostic (targets 5.4+): it uses only BT,
// reflection and asset APIs that have been stable since 4.x. MCP exposure
// lives in the BTForgeMCP module (self-contained HTTP server, any version)
// and in the separate BTForgeOfficial plugin (Epic's ToolsetRegistry, 5.8+).

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "BTForgeLibrary.generated.h"

/**
 * Build Behavior Tree + Blackboard assets from JSON specs, deterministically.
 *
 * Node whitelist (engine classes + the node library): composites
 * Sequence/Selector; tasks Wait, MoveTo, RunEQS, ComputeValue, Log; services
 * RunEQS, SetValue, UpdateDistance; decorators Blackboard, FloatCompare,
 * DistanceCheck, Chance. Any other type may be given as a class path
 * (/Script/Module.Class or /Game/Folder/Asset.Asset_C) and is base-class,
 * abstract-flag and Blueprintability checked. Composites accept "services",
 * any child accepts "decorators". Blackboard key references are validated
 * against the tree's blackboard; asset references are loaded and type-checked
 * before writing.
 *
 * Every function returns a JSON string: {"ok":true,...} on success,
 * {"ok":false,"error":<message with a JSON path>} on failure.
 */
UCLASS()
class BTFORGECORE_API UBTForgeLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Create a Blackboard + BehaviorTree asset pair. The tree is empty (no
	 * root); fill it with BuildBehaviorTree. Fails if either asset already
	 * exists; nothing is overwritten.
	 *
	 * @param AssetPath      Target BT asset, e.g. "/Game/AI/BT_Sentinel".
	 *                       The blackboard is created next to it as "<AssetPath>_BB".
	 * @param BlackboardJson {"keys":[{"name":"HomeLocation","type":"Vector"}]}.
	 *                       Types: Bool, Int, Float, String, Name, Vector, Rotator, Object.
	 * @return JSON {"ok":true,"blackboard":path,"behaviorTree":path} or {"ok":false,"error":msg}
	 */
	UFUNCTION(BlueprintCallable, Category = "BTForge")
	static FString CreateBehaviorTreeAsset(const FString& AssetPath, const FString& BlackboardJson);

	/**
	 * Build (or rebuild) the tree of an existing BT asset from a JSON spec.
	 * Replaces the whole root; the previous tree is discarded on success.
	 * Do not call while PIE is running.
	 *
	 * Tree JSON (the root object), e.g.:
	 *  {"type":"Selector","name":"Root","children":[
	 *    {"type":"Sequence","children":[
	 *      {"type":"Wait","name":"Pause","params":{"WaitTime":2.0,"RandomDeviation":1.0}}]},
	 *    {"type":"Wait","params":{"WaitTime":5.0}}]}
	 *
	 * Rules: root must be a composite (any UBTCompositeNode subclass, or such a
	 * class path); every composite needs a non-empty "children" array; tasks take
	 * no children; unknown "type" or unknown param names are rejected (wait params:
	 * WaitTime, RandomDeviation as numbers). Composites accept "services":[{type,params}];
	 * any child accepts "decorators":[{type,params}] (several combine as AND).
	 *
	 * @param AssetPath Existing BT asset created by CreateBehaviorTreeAsset.
	 * @param TreeJson   Root node object as above.
	 * @return JSON {"ok":true,"nodeCount":N,"root":class} or {"ok":false,"error":msg with JSON path}
	 */
	UFUNCTION(BlueprintCallable, Category = "BTForge")
	static FString BuildBehaviorTree(const FString& AssetPath, const FString& TreeJson);

	/**
	 * Validate a tree spec + blackboard spec WITHOUT creating or modifying any
	 * asset. Everything (blackboard keys, node classes, parameters, services,
	 * decorators) is built into transient objects and discarded, so an agent can
	 * iterate on a spec without leaving partial assets behind.
	 *
	 * @param TreeJson      Root node object (same syntax as BuildBehaviorTree).
	 * @param BlackboardJson Optional {"keys":[...]} spec; defaults to no keys.
	 * @return JSON {"ok":true,"nodeCount":N,"root":class,"blackboardKeys":[...]}
	 *         or {"ok":false,"error":msg with JSON path}
	 */
	UFUNCTION(BlueprintCallable, Category = "BTForge")
	static FString ValidateBehaviorTree(const FString& TreeJson, const FString& BlackboardJson);

	/**
	 * Read a BT asset back as JSON: blackboard keys plus the full node tree
	 * with each node's class, name and scalar property values. Use this to
	 * verify a build or to inspect an existing tree before extending it.
	 *
	 * @param AssetPath Existing BT asset path.
	 * @return JSON {"ok":true,"blackboardKeys":[...],"root":{...},"nodeCount":N};
	 *         root is null for an empty tree. Scalar params only - object references
	 *         inside structs are reported by inspect_references instead.
	 */
	UFUNCTION(BlueprintCallable, Category = "BTForge")
	static FString InspectBehaviorTree(const FString& AssetPath);

	/**
	 * Apply spec-style params ({name: value, ...}) to any object through the
	 * same reflection writers build_behavior_tree uses (enums by name, class
	 * paths, nested structs, arrays). Blackboard key names are accepted without
	 * tree validation: this entry point is for objects that exist outside a
	 * tree (e.g. a blueprint node class CDO); tree-time resolution still runs
	 * when the node is placed.
	 *
	 * @param Target        Object to write onto (its CDO for blueprint defaults).
	 * @param ParamsJson    {"ParamName": value, ...}.
	 * @param OutAppliedCount Number of top-level params written.
	 * @param OutError      First failure with the param path.
	 * @return true when every param applied.
	 */
	static bool ApplyParamsToObject(UObject* Target, const FString& ParamsJson, int32& OutAppliedCount, FString& OutError);

	/**
	 * Spec aliases for tree node "type" values, authoritative for both the
	 * spec parser and the capability catalog: "Sequence"/"Selector"/"Wait"/
	 * "MoveTo"/"RunEQS" plus the M5 native library aliases "ComputeValue"/"Log".
	 * Anything else must be a '/Script/...' or '/Game/..._C' class path.
	 */
	static const TMap<FString, UClass*>& GetNodeTypeAliases();

	/**
	 * Spec aliases for auxiliary nodes; Kind is "service" ("RunEQS", "SetValue",
	 * "UpdateDistance") or "decorator" ("Blackboard", "FloatCompare",
	 * "DistanceCheck", "Chance"). Same table the parser accepts, so the catalog
	 * cannot drift from the whitelist.
	 */
	static const TMap<FString, UClass*>& GetAuxTypeAliases(const FString& Kind);
};
