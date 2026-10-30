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

	/**
	 * Runtime verification (M3): start an in-process PIE session and actually
	 * run a generated asset. OptionsJson: {"kind":"bt"|"eqs", "assetPath":"...",
	 * "mode":"SingleRun"|"Looped", "durationSec":n, "expectedDurationSec":n,
	 * "toleranceSec":n, "expectedActiveTask":"...", "blackboardInit":{...},
	 * "fixturePawn":true|false, "mapPath":"...", "bareGameMode":true|false,
	 * "gameModeClassPath":"...", "queryRunMode":"...", "queryParams":{...}}.
	 * Starts PIE with GameModeOverride=AGameModeBase (bareGameMode, default true;
	 * an empty mapPath then defaults to /Engine/Maps/Templates/Template_Default
	 * because project maps such as Lyra assert on a bare GameMode), spawns an
	 * AIController + possessed Pawn fixture, starts the tree or runs the EQS
	 * query synchronously, and samples execution from t=0. Refuses to start while
	 * a PIE/SIE session is already running.
	 *
	 * @param OptionsJson Verification options as a JSON string.
	 * @return JSON {"ok":true,"sessionId":"V..","status":"pending","mapPath":...}
	 *         - poll GetVerifyResult until status is done/failed.
	 */
	UFUNCTION(meta = (AICallable), Category = "BTForge")
	static FString StartVerify(const FString& OptionsJson);

	/**
	 * Poll a runtime verification session started by StartVerify. While
	 * running returns {"status":"pending"|"running"}; when finished returns
	 * the full result: a top-level "pass" boolean (the AND of every checks[]
	 * entry; "status" is "done" only when it is true), checksPassed/checksTotal,
	 * checks[{name,pass,detail}], samples timeline (from t=0), activeTasksSeen,
	 * durationSec/frameMaxSec, fixture, blackboardFinal (bt) or
	 * itemCount(=NumValidItems)/rawItemCount/itemType/topScores (eqs), logErrors.
	 *
	 * @param SessionId Session id returned by StartVerify.
	 * @return JSON result or {"ok":false,"error":msg} for an unknown session.
	 */
	UFUNCTION(meta = (AICallable), Category = "BTForge")
	static FString GetVerifyResult(const FString& SessionId);

	/**
	 * Reference / content-domain policy (M4), read-only. Reports every object
	 * reference of a BT or EQS asset (or of a spec, before anything is written):
	 * domain (native module / project content / plugin content / engine content),
	 * module, plugin, blueprint flag, and whether it can exist in a cooked build.
	 * Use it after Build* and before shipping, or on a spec to catch missing /
	 * editor-only references early.
	 *
	 * @param RequestJson {"assetPath":"/Game/AI/BT_X"} for an existing asset, or
	 *                    {"kind":"bt","treeJson":{...},"blackboardJson":{...}} /
	 *                    {"kind":"eqs","queryJson":{...}} for a spec.
	 * @return JSON {"ok":true,"mode":"asset|spec","verdict":"ok|warn|fail",
	 *         "references":[...],"errors":[...],"warnings":[...],"summary":{...}}
	 */
	UFUNCTION(meta = (AICallable), Category = "BTForge")
	static FString InspectReferences(const FString& RequestJson);

	/**
	 * Blueprint node generation (M6): create a data-only Blueprint node asset -
	 * a generated class derived from a BT node base class (UBTTaskNode /
	 * UBTService / UBTDecorator, no Blueprint composites) with ParamsJson
	 * written onto the class CDO through the same reflection writers
	 * BuildBehaviorTree uses. No event-graph logic is generated. The returned
	 * classPath plugs straight into BuildBehaviorTree as a node "type", and the
	 * capability catalog lists the new class automatically. Fails if the asset
	 * exists; nothing is kept on failure. Blackboard key names in params
	 * resolve per-tree when the node is placed.
	 *
	 * @param AssetPath       Target blueprint asset, e.g. "/Game/AI/BTA_LogShot".
	 * @param ParentClassPath Non-abstract UBTTaskNode/UBTService/UBTDecorator-derived
	 *                        class path (/Script/... or /Game/..._C).
	 * @param ParamsJson      {"PropertyName": value, ...} for the class defaults;
	 *                        pass "" for engine defaults.
	 * @return JSON {"ok":true,"assetPath":...,"classPath":"..._C","parent":...,
	 *         "appliedParams":N} or {"ok":false,"error":msg}
	 */
	UFUNCTION(meta = (AICallable), Category = "BTForge")
	static FString CreateNodeBlueprint(const FString& AssetPath, const FString& ParentClassPath, const FString& ParamsJson);
};
