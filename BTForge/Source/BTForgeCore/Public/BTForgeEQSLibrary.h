// BT Forge EQS library: deterministic UEnvQuery (EQS) builder from JSON specs.
//
// Design mirrors UBTForgeLibrary for Behavior Trees:
// - The agent produces a JSON spec; this library is the only writer.
// - Only the *data* of the query is written (Options / Generator / Tests).
//   The editor graph is left empty: opening the asset rebuilds it from the
//   Options array (UEnvironmentQueryGraph::SpawnMissingNodes), so no
//   EnvQueryEditor / AIGraph dependency is required here.
// - Everything is validated before an asset is created; on failure nothing is
//   saved and the error names the exact JSON path, so an agent can self-correct.
//
// Creating EQS assets needs editor APIs (AssetTools / UnrealEd); therefore this
// class lives in the editor-only BTForgeCore module. EQS *contexts* that must be
// usable at runtime (e.g. the blackboard context) live in BTForgeRuntime.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "BTForgeEQSLibrary.generated.h"

/**
 * Build UEnvQuery (EQS) assets from JSON specs.
 *
 * Supported generators: SimpleGrid, CurrentLocation, OnCircle, Donut, Cone,
 * PathingGrid, ActorsOfClass, PerceivedActors - or any class path to a
 * non-abstract UEnvQueryGenerator (engine, project or plugin class).
 * Supported tests: Distance, Dot, Trace, Pathfinding, Random, Overlap, Project,
 * Volume - or any class path to a non-abstract UEnvQueryTest.
 *
 * Query JSON:
 * {
 *   "options": [
 *     {
 *       "optionName": "FindCover",
 *       "generator": {"type": "SimpleGrid", "params": {"GridSize": 1500, "SpaceBetween": 200}},
 *       "tests": [
 *         {"type": "Distance",
 *          "params": {"TestPurpose": "Score", "ScoringEquation": "InverseLinear",
 *                     "ScoringFactor": 1.0,
 *                     "DistanceTo": "/Script/AIModule.EnvQueryContext_Querier"}}
 *       ]
 *     }
 *   ],
 *   "bStripFromClientBuilds": false
 * }
 *
 * Param names are the exact UPROPERTY names; unknown names are rejected with
 * the allowed list. Class references take object paths (e.g.
 * /Script/AIModule.EnvQueryContext_Querier or /Game/.../Asset.Asset_C).
 * The runtime blackboard context is
 * /Script/BTForgeRuntime.BTForgeEnvQueryContext_Blackboard with a
 * "BlackboardKey" param.
 */
UCLASS()
class BTFORGECORE_API UBTForgeEQSLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Create an EQS query asset from a JSON spec. Fails if the asset already
	 * exists; on any validation error no asset is created.
	 * @return JSON {"ok":true,"asset":path,"queryName":...,"optionCount":N,...} or {"ok":false,"error":msg}
	 */
	UFUNCTION(BlueprintCallable, Category = "BTForge|EQS")
	static FString CreateEQSAsset(const FString& AssetPath, const FString& QueryJson);

	/**
	 * Read an EQS query asset back as JSON (options, generator/test classes and
	 * their editable parameter values) so a build can be verified.
	 * @return JSON {"ok":true,"asset":path,"queryName":...,"options":[...]} or {"ok":false,"error":msg}
	 */
	UFUNCTION(BlueprintCallable, Category = "BTForge|EQS")
	static FString InspectEQSAsset(const FString& AssetPath);

	/**
	 * Validate a query spec without creating or modifying any asset. Builds the
	 * whole query into transient objects (including class loading and parameter
	 * writing) and reports the first error with the exact JSON path.
	 * @return JSON {"ok":true,"optionCount":N,...} or {"ok":false,"error":msg}
	 */
	UFUNCTION(BlueprintCallable, Category = "BTForge|EQS")
	static FString ValidateEQSSpec(const FString& QueryJson);

	/**
	 * Short alias -> class maps accepted by the spec language ("SimpleGrid",
	 * "Distance", ...). Exposed so the capability catalog derives its alias list
	 * from this table instead of keeping a hand-maintained copy.
	 */
	static const TMap<FString, UClass*>& GetGeneratorTypeAliases();
	static const TMap<FString, UClass*>& GetTestTypeAliases();
};
