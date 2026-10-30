// M4 reference / content-domain policy. Read-only.
//
// Answers one question for an agent before (or after) it writes assets:
// "every object reference in this asset/spec - does it resolve, is it
// cook-safe, and where does it live?"
//
// Two modes:
//   asset mode: {"assetPath":"/Game/AI/BT_X"} - reflection walk over the real
//               objects (BT node tree, blackboard keys, EQS options/generators/
//               tests, nested struct params). This is the authoritative view.
//   spec mode:  {"kind":"bt","treeJson":{...}} or {"kind":"eqs","queryJson":{...}}
//               - scans the spec JSON for object-path strings, so an agent can
//               check references BEFORE creating anything.
//
// A reference is classified by domain (native module / project content / plugin
// content / engine content / transient), and by cook safety:
//   error:   missing, transient, or a class from an editor-only module
//   warning: engine content (only ships if the cook pulls it in), or plugin
//            content whose plugin is not mounted
// The verdict is "fail" when any reference is an error, "warn" when there are
// warnings, "ok" otherwise - mirroring how the verify result reports pass.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "BTForgeReferenceLibrary.generated.h"

/**
 * Inspect every object reference of a BT/EQS asset (or spec) and report its
 * domain and cook safety. Read-only: never creates, loads-and-saves, or
 * modifies anything.
 *
 * RequestJson:
 *   {"assetPath":"/Game/AI/BT_X"}                          asset mode
 *   {"kind":"bt","treeJson":{...},"blackboardJson":{...}}  spec mode (bt)
 *   {"kind":"eqs","queryJson":{...}}                       spec mode (eqs)
 *
 * @return JSON {"ok":true,"mode":"asset|spec","verdict":"ok|warn|fail",
 *         "references":[{jsonPath,ref,kind,domain,module,plugin,editorOnly,
 *         blueprint,cookSafe,reason}],"errors":[...],"warnings":[...],
 *         "summary":{total,byDomain,...}} or {"ok":false,"error":msg}
 */
UCLASS()
class BTFORGECORE_API UBTForgeReferenceLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "BTForge")
	static FString InspectReferences(const FString& RequestJson);
};
