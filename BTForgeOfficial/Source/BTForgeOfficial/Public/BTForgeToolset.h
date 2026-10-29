// UE 5.8+ adapter: registers BTForge's builder as an official MCP toolset.
//
// The tool bodies live in BTForgeCore (UBTForgeLibrary); this class only
// declares the AICallable surface for Epic's ToolsetRegistry, whose schema
// generation and JSON-to-UFUNCTION binding handle the rest. Tool names on the
// official endpoint follow <Plugin>.<Class>.<Function>:
//   BTForgeOfficial.BTForgeToolset.CreateBehaviorTreeAsset / BuildBehaviorTree / InspectBehaviorTree
// The doc comments below are the tool descriptions shown to the agent.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "BTForgeToolset.generated.h"

UCLASS(BlueprintType)
class BTFORGEOFFICIAL_API UBTForgeToolset : public UToolsetDefinition
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
	UFUNCTION(meta = (AICallable), Category = "BTForge")
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
	 * Rules: root must be Sequence/Selector; every composite needs a non-empty
	 * "children" array; tasks take no children; unknown "type" or unknown param
	 * names are rejected (wait params: WaitTime, RandomDeviation as numbers).
	 *
	 * @param AssetPath Existing BT asset created by CreateBehaviorTreeAsset.
	 * @param TreeJson   Root node object as above.
	 * @return JSON {"ok":true,"nodeCount":N,"root":class} or {"ok":false,"error":msg with JSON path}
	 */
	UFUNCTION(meta = (AICallable), Category = "BTForge")
	static FString BuildBehaviorTree(const FString& AssetPath, const FString& TreeJson);

	/**
	 * Read a BT asset back as JSON: blackboard keys plus the full node tree
	 * with each node's class, name and scalar property values. Use this to
	 * verify a build or to inspect an existing tree before extending it.
	 *
	 * @param AssetPath Existing BT asset path.
	 * @return JSON {"ok":true,"blackboard":{...},"root":{...}} ; root is null for an empty tree.
	 */
	UFUNCTION(meta = (AICallable), Category = "BTForge")
	static FString InspectBehaviorTree(const FString& AssetPath);

	/**
	 * Validate a Behavior Tree spec + blackboard spec WITHOUT creating or
	 * modifying any asset. Everything is built into transient objects and
	 * discarded; errors carry the exact JSON path.
	 *
	 * @param TreeJson      Root node object (same syntax as BuildBehaviorTree).
	 * @param BlackboardJson Optional {"keys":[...]} spec; pass "" for no keys.
	 * @return JSON {"ok":true,"nodeCount":N,"root":class,"blackboardKeys":[...]} or {"ok":false,"error":msg}
	 */
	UFUNCTION(meta = (AICallable), Category = "BTForge")
	static FString ValidateBehaviorTree(const FString& TreeJson, const FString& BlackboardJson);

	/**
	 * Create an EQS (UEnvQuery) asset from a JSON spec. Options are built from
	 * native generators/tests or any non-abstract UEnvQueryGenerator/UEnvQueryTest
	 * class path. The editor graph is rebuilt from the data when opened.
	 * Fails if the asset exists; nothing is created on validation error.
	 * Query JSON: {"options":[{"optionName":"...","generator":{"type":"SimpleGrid",
	 * "params":{...}},"tests":[{"type":"Distance","params":{...}}]}]}.
	 * Param names are exact UPROPERTY names; class references are object paths.
	 * Runtime blackboard context: /Script/BTForgeRuntime.BTForgeEnvQueryContext_Blackboard
	 * (param BlackboardKey).
	 *
	 * @return JSON {"ok":true,"asset":path,"queryName":...,"optionCount":N,...} or {"ok":false,"error":msg}
	 */
	UFUNCTION(meta = (AICallable), Category = "BTForge")
	static FString CreateEQSAsset(const FString& AssetPath, const FString& QueryJson);

	/**
	 * Read an EQS query asset back as JSON: options, generator/test classes and
	 * their editable parameter values.
	 *
	 * @param AssetPath Existing EQS asset path.
	 * @return JSON {"ok":true,"asset":path,"queryName":...,"options":[...]} or {"ok":false,"error":msg}
	 */
	UFUNCTION(meta = (AICallable), Category = "BTForge")
	static FString InspectEQSAsset(const FString& AssetPath);

	/**
	 * Validate an EQS query spec without creating or modifying any asset.
	 * Loads referenced classes, writes parameters into transient objects and
	 * checks generator item types against tests.
	 *
	 * @param QueryJson EQS query spec (same syntax as CreateEQSAsset).
	 * @return JSON {"ok":true,"optionCount":N,...} or {"ok":false,"error":msg}
	 */
	UFUNCTION(meta = (AICallable), Category = "BTForge")
	static FString ValidateEQSSpec(const FString& QueryJson);

	/**
	 * Return a machine-readable inventory of what can be used in this project:
	 * native + project Blueprint BT node classes (class paths and editable
	 * params), EQS query assets, native/Blueprint EQS contexts, generators and
	 * tests, plus spec type aliases. Call this before authoring a spec so class
	 * paths are real instead of guessed. Read-only.
	 *
	 * @param SectionJson Optional {"section":"all"|"bt"|"eqs"}; pass "" for all.
	 * @return JSON {"ok":true,"engineVersion":...,"bt":{...},"eqs":{...}}
	 */
	UFUNCTION(meta = (AICallable), Category = "BTForge")
	static FString GetCapabilityCatalog(const FString& SectionJson);
};
