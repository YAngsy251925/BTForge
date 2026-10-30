// Internal verify-session manager for M3 runtime verification.
//
// One in-process PIE session at a time. Flow:
//   StartVerify(options)            -> spawns a PIE session (GameModeOverride=AGameModeBase)
//   FEditorDelegates::PostPIEStarted-> spawns the fixture (AIController + Pawn), starts the
//                                      tree or runs the EQS instant query, samples on a ticker
//   ticker / finished               -> assembles checks + samples into a result JSON,
//                                      requests EndPlayMap, marks the session done
//   GetVerifyResult(sessionId)      -> poll: pending | running | done (with result) | failed
//
// Result contract: every terminal result carries an explicit top-level "pass" boolean
// that is the AND of all checks[] entries, and "status" is derived from that same
// boolean ("done" when every check passed, "failed" otherwise). Never sniff pass/status
// out of the serialized JSON text.
//
// Everything runs on the game thread: the MCP handler thread IS the game thread
// (HTTPServer module ticks there), PIE transitions span multiple frames which is
// why this is a two-call protocol (start_verify / get_verify_result) instead of
// one blocking request - blocking the game thread here would deadlock PIE startup.

#pragma once

#include "CoreMinimal.h"

class FOutputDevice;
class AAIController;
class APawn;
class UBehaviorTree;
class UBehaviorTreeComponent;
class UBlackboardComponent;
class UBlackboardKeyType;
class UWorld;
class FJsonValue;

struct FBTForgeVerifyOptions
{
	FString Kind;                 // "bt" | "eqs"
	FString AssetPath;
	FString MapPath;              // optional; empty = see BareGameMode below
	FString Mode;                 // bt: "SingleRun" (default) | "Looped"
	double DurationSec = 5.0;     // bt SingleRun: sampling cap / Looped: required running time
	double StartDeadlineSec = 90.0; // Lyra maps can take a minute+ to bring up PIE
	double ToleranceSec = 0.5;
	double ExpectedDurationSec = -1.0; // -1 = not set
	double MinPawnDisplacement = -1.0; // -1 = do not assert fixture pawn movement (M4 golden path)
	FString ExpectedActiveTask;   // optional substring match against DescribeActiveTasks
	TSharedPtr<FJsonObject> BlackboardInit; // optional {"Key": value,...}
	bool BareGameMode = true;     // override PIE GameMode with bare AGameModeBase
	FString GameModeClassPath;    // optional explicit GameMode class path
	bool FixturePawn = true;      // bt: spawn + possess a Pawn so MoveTo/RunBehavior can work
	FString QueryRunMode;         // eqs: optional override (SingleResult/AllMatching/...)
	TSharedPtr<FJsonObject> QueryParams;    // eqs: optional {"Name": number}

	/** Map actually handed to PIE; filled in by StartVerify (auto-defaults for bareGameMode). */
	FString ResolvedMapPath;
	bool bMapAutoDefaulted = false;

	bool Parse(const FString& OptionsJson, FString& OutError);
};

struct FBTForgeVerifySession
{
	FString SessionId;
	FBTForgeVerifyOptions Options;

	enum class EState { Idle, RequestingPIE, Running, Done, Failed };
	EState State = EState::Idle;
	FString Error;

	bool bPIERequested = false;
	double RequestTime = 0.0;
	double PIEStartTime = 0.0;
	double TreeStartTime = 0.0;
	double VerifyDeadline = 0.0;

	TWeakObjectPtr<UWorld> PIEWorld;
	TWeakObjectPtr<AAIController> FixtureController;
	TWeakObjectPtr<APawn> FixturePawn;
	TWeakObjectPtr<UBehaviorTreeComponent> BTComp;
	TWeakObjectPtr<UBlackboardComponent> BBComp;

	FVector PawnStartLocation = FVector::ZeroVector;  // captured right before the tree starts
	bool bPawnStartCaptured = false;

	TArray<FString> Samples;            // JSON fragments: {"t":..,"running":..,"tasks":".."}
	TSet<FString> ActiveTasksSeen;
	TArray<FString> LogErrors;
	TArray<FString> LogWarnings;
	FOutputDevice* LogCapture = nullptr;

	bool bSawRunning = false;           // tree was observed running (t=0 sample included)
	bool bTreeStarted = false;          // FBehaviorTreeDelegates::OnTreeStarted fired for our component
	double MaxTickDelta = 0.0;          // worst frame time seen, used to size the duration window

	FString ResultJson;                 // full output once Done/Failed

	FDelegateHandle PIEStartedHandle;
	FDelegateHandle PIEEndHandle;
	FDelegateHandle TreeStartedHandle;
	FTSTicker::FDelegateHandle TickHandle;
};

/** Manager singleton: owns at most one verify session per editor process. */
class FBTForgeVerifyManager
{
public:
	static FBTForgeVerifyManager& Get();

	/** Parse options, load the asset, request PIE. Returns {"ok":true,"sessionId":..} JSON or an error JSON. */
	FString StartVerify(const FString& OptionsJson);
	/** Poll a session: pending/running -> {"ok":true,"status":..}; done/failed -> full result JSON. */
	FString GetVerifyResult(const FString& SessionId);
	/** Module shutdown: abort any session, unsubscribe. */
	void Shutdown();

private:
	FBTForgeVerifyManager() = default;

	// The bool FEditorDelegates::PostPIEStarted passes is bIsSimulatingInEditor
	// (PlayLevel.cpp: PostPIEStarted.Broadcast(WorldType == SimulateInEditor)), NOT a
	// start-failure flag: PIE start failures are detected by polling the session state.
	static void HandlePostPIEStarted(bool bIsSimulatingInEditor);
	static void HandlePIEEnded(bool bWasSimulating);
	/** FBehaviorTreeDelegates::OnTreeStarted: the only reliable "the tree really started"
	 *  signal (IsRunning() is already false again when a tree completes inside StartTree,
	 *  and is still true when PushInstance failed). */
	static void HandleTreeStarted(const UBehaviorTreeComponent& Component, const UBehaviorTree& TreeAsset);
	bool Tick(float DeltaTime);

	void OnPIEReady();
	void StartBTVerification();
	void RunEQSVerification();
	void FinishSession(const FString& Error);
	void AbortToIdle(const FString& Error);
	void FailEQS(const FString& Error, int32 Steps, const FString& RunModeLabel, double Elapsed);
	UWorld* FindPIEWorld() const;
	void CaptureLog(bool bEnable);

	void SubscribePIEDelegates();
	void RemovePIEDelegates();
	void SubscribeTreeStarted();
	void RemoveTreeStarted();
	void StopTicker();
	void RecordSample(double Elapsed);

	FBTForgeVerifySession Session;
};
