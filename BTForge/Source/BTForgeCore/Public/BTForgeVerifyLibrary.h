// M3 runtime verification facade.
//
// Two-call protocol (works on both MCP endpoints - the official ToolsetRegistry
// path only supports synchronous UFUNCTIONs, so one blocking call that waits
// for PIE would deadlock the game thread):
//   StartVerify(OptionsJson)  -> {"ok":true,"sessionId":"V..","status":"pending"}
//   GetVerifyResult(SessionId) -> pending/running JSON, or the full result with
//                                 checks[] once the session is done/failed.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "BTForgeVerifyLibrary.generated.h"

UCLASS()
class BTFORGECORE_API UBTForgeVerifyLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Start a runtime verification session (spawns an in-process PIE session).
	 * OptionsJson: {"kind":"bt"|"eqs", "assetPath":"...", "mapPath":"?"(opt),
	 *   bt: "mode":"SingleRun"|"Looped", "durationSec":n, "expectedDurationSec":n,
	 *       "toleranceSec":n, "expectedActiveTask":"BTTask_Wait", "expectedMinPawnDisplacement":n,
	 *       "blackboardInit":{"Key":value}, "fixturePawn":true|false,
	 *   eqs: "queryRunMode":"SingleResult", "queryParams":{"Name":n}}
	 * bareGameMode (default true) overrides PIE's GameMode with a bare AGameModeBase;
	 * an empty mapPath then defaults to /Engine/Maps/Templates/Template_Default.
	 * @return {"ok":true,"sessionId":..,"status":"pending","mapPath":..} or an error JSON.
	 */
	UFUNCTION(BlueprintCallable, Category = "BTForge")
	static FString StartVerify(const FString& OptionsJson);

	/**
	 * Poll a verification session. Returns pending/running JSON, or the full
	 * result (checks[], samples, blackboardFinal / EQS itemCount) once done.
	 * The terminal result always carries a top-level "pass" boolean - the AND of
	 * every checks[] entry - and "status" is "done" only when that boolean is true.
	 */
	UFUNCTION(BlueprintCallable, Category = "BTForge")
	static FString GetVerifyResult(const FString& SessionId);
};
