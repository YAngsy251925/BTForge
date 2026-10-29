// Runtime EQS context types shipped by BT Forge.
//
// The engine has no blackboard-backed UEnvQueryContext (only Querier / Item /
// NavigationData / the abstract BlueprintBase). Generated queries therefore
// could not use gameplay data without a blueprint context. This native context
// closes that gap and - because it lives in the Runtime module - stays valid
// for cooked builds.

#pragma once

#include "CoreMinimal.h"
#include "EnvironmentQuery/EnvQueryContext.h"
#include "BTForgeEnvQueryContext_Blackboard.generated.h"

/**
 * Reads a blackboard key from the query owner (AAIController or any actor with
 * a BlackboardComponent) and provides it as EQS context:
 *  - Object keys pointing at an actor => actor context (item actor);
 *  - Vector keys => a single point;
 *  - Rotator keys => owner location offset along the rotation (a point).
 *
 * Unsupported/missing keys provide no context (the query then simply yields no
 * items for tests using this context), matching engine context behavior.
 */
UCLASS(DisplayName = "Blackboard Key (BT Forge)")
class BTFORGERUNTIME_API UBTForgeEnvQueryContext_Blackboard : public UEnvQueryContext
{
	GENERATED_BODY()

public:
	/** Blackboard key to read from the query owner. */
	UPROPERTY(EditAnywhere, Category = "BT Forge")
	FName BlackboardKey;

	virtual void ProvideContext(FEnvQueryInstance& QueryInstance, FEnvQueryContextData& ContextData) const override;
};
