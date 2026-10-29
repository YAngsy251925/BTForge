// BT Forge capability catalog.
//
// An LLM cannot reliably guess what node classes / EQS assets / contexts exist
// in a project. This tool returns a machine-readable inventory so the agent can
// pick real class paths instead of hallucinating them, and so "generate a BTS /
// EQS on demand" can compose existing project assets where appropriate.
//
// Read-only: it never creates or modifies assets.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "BTForgeCatalogLibrary.generated.h"

UCLASS()
class BTFORGECORE_API UBTForgeCatalogLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Return a JSON catalog of what the project can build with BT Forge.
	 *
	 * @param SectionJson Optional JSON, e.g. {"section":"bt"} or {"section":"eqs"}.
	 *                    Defaults to all sections. Sections: bt, eqs.
	 * @return JSON {"ok":true,"engineVersion":...,"bt":{...},"eqs":{...}}
	 */
	UFUNCTION(BlueprintCallable, Category = "BTForge")
	static FString GetCapabilityCatalog(const FString& SectionJson);
};
