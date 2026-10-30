// BT Forge native parameterized node library (M5).
//
// The engine ships a solid set of BT nodes (Wait, MoveTo, Set-key tasks,
// Cooldown / TimeLimit / Loop / Blackboard / CompareBBEntries decorators,
// DefaultFocus / RunEQS services), but leaves real gaps that projects
// otherwise close with Blueprint nodes:
//   - no numeric arithmetic task and no log-checkpoint task;
//   - no service that writes the blackboard periodically (the engine's
//     set-value tasks are one-shot tasks, and the only native services are
//     DefaultFocus and RunEQS);
//   - no numeric-threshold, distance-range or probability decorator
//     (CompareBBEntries only does Equal/NotEqual between two keys).
//
// These classes close exactly those gaps. They are deliberately stateless
// (no node instances needed), read/write the blackboard through its own
// typed accessors, and resolve their key selectors in InitializeFromAsset
// the same way the engine's nodes do. Everything is configurable through
// the generic BTForge spec writers, shows up in get_capability_catalog via
// reflection, and is PIE-verifiable with the M3 harness.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BTForgeBehaviorNodes.generated.h"

class UBehaviorTree;
class UBlackboardComponent;

/** Dedicated category so verify runs can tell node-authored log lines apart
 *  from engine and project log output. */
DECLARE_LOG_CATEGORY_EXTERN(LogBTForge, Log, All);

/**
 * Tasks
 */

UENUM(BlueprintType)
enum class EBTForgeArithmeticOperation : uint8
{
	Add			UMETA(DisplayName = "Add (Left + Right)"),
	Subtract	UMETA(DisplayName = "Subtract (Left - Right)"),
	Multiply	UMETA(DisplayName = "Multiply (Left * Right)"),
	Divide		UMETA(DisplayName = "Divide (Left / Right)"),
	Min			UMETA(DisplayName = "Min (Left, Right)"),
	Max			UMETA(DisplayName = "Max (Left, Right)")
};

/**
 * Computes Left (Key) <Operation> Right (Key or constant) and stores the
 * result in ResultKey. Left/Right keys must be Float or Int; ResultKey must
 * be Float (exact) or Int (rounded). Division by zero and missing/unreadable
 * keys fail the task with a warning in LogBTForge.
 */
UCLASS(DisplayName = "BT Forge: Compute Value")
class BTFORGERUNTIME_API UBTForgeTask_ComputeValue : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UBTForgeTask_ComputeValue();

	virtual void InitializeFromAsset(UBehaviorTree& Asset) override;
	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual FString GetStaticDescription() const override;

protected:
	/** operation applied to Left and Right */
	UPROPERTY(EditAnywhere, Category = "BT Forge")
	EBTForgeArithmeticOperation Operation = EBTForgeArithmeticOperation::Add;

	/** left operand key (Float or Int) */
	UPROPERTY(EditAnywhere, Category = "BT Forge")
	FBlackboardKeySelector LeftKey;

	/** right operand key (Float or Int). When unset, RightConstant is used. */
	UPROPERTY(EditAnywhere, Category = "BT Forge")
	FBlackboardKeySelector RightKey;

	/** right operand used when RightKey is not set */
	UPROPERTY(EditAnywhere, Category = "BT Forge")
	double RightConstant = 0.0;

	/** destination key (Float keeps full precision, Int rounds) */
	UPROPERTY(EditAnywhere, Category = "BT Forge")
	FBlackboardKeySelector ResultKey;
};

UENUM(BlueprintType)
enum class EBTForgeLogSeverity : uint8
{
	Log,
	Warning,
	Error
};

/**
 * Writes a message to LogBTForge and succeeds. Optionally appends
 * "KeyName=value" for any blackboard key type. Intended as an execution
 * checkpoint: verification can assert the message was logged (or, with
 * Severity=Error, deliberately prove that a broken tree fails no_log_errors).
 */
UCLASS(DisplayName = "BT Forge: Log Message")
class BTFORGERUNTIME_API UBTForgeTask_LogMessage : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UBTForgeTask_LogMessage();

	virtual void InitializeFromAsset(UBehaviorTree& Asset) override;
	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual FString GetStaticDescription() const override;

protected:
	/** message text */
	UPROPERTY(EditAnywhere, Category = "BT Forge")
	FString Message = TEXT("BT Forge checkpoint");

	/** optional key whose value is appended to the message (any key type) */
	UPROPERTY(EditAnywhere, Category = "BT Forge")
	FBlackboardKeySelector KeyToLog;

	/** Log/Warning/Error. Error fails the PIE run's no_log_errors check. */
	UPROPERTY(EditAnywhere, Category = "BT Forge")
	EBTForgeLogSeverity Severity = EBTForgeLogSeverity::Log;
};

/**
 * Services
 */

/**
 * Writes a value into KeyToSet every service tick: either a copy of SourceKey
 * (when set, any compatible key type) or a per-type constant. This is the
 * periodic counterpart of the engine's one-shot Set-key tasks, usable as a
 * background knowledge updater (e.g. re-stamping a "branch entered" marker).
 */
UCLASS(DisplayName = "BT Forge: Set Blackboard Value (periodic)")
class BTFORGERUNTIME_API UBTForgeService_SetBlackboardValue : public UBTService
{
	GENERATED_BODY()

public:
	UBTForgeService_SetBlackboardValue();

	virtual void InitializeFromAsset(UBehaviorTree& Asset) override;
	virtual void TickNode(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;
	virtual FString GetStaticDescription() const override;

protected:
	/** destination key */
	UPROPERTY(EditAnywhere, Category = "BT Forge")
	FBlackboardKeySelector KeyToSet;

	/** source key to copy from; when unset the constants below are used */
	UPROPERTY(EditAnywhere, Category = "BT Forge")
	FBlackboardKeySelector SourceKey;

	/** constant for Float keys, and for Int keys (rounded) */
	UPROPERTY(EditAnywhere, Category = "BT Forge")
	double Constant = 0.0;

	/** constant for Bool keys */
	UPROPERTY(EditAnywhere, Category = "BT Forge")
	bool bConstantBool = false;

	/** constant for String and Name keys */
	UPROPERTY(EditAnywhere, Category = "BT Forge")
	FString ConstantString;

	/** constant for Vector keys */
	UPROPERTY(EditAnywhere, Category = "BT Forge")
	FVector ConstantVector = FVector::ZeroVector;

	/** constant for Rotator keys */
	UPROPERTY(EditAnywhere, Category = "BT Forge")
	FRotator ConstantRotator = FRotator::ZeroRotator;
};

/**
 * Computes the distance between two location sources every service tick and
 * stores it in ResultKey (Float). A location source is an Object key holding
 * an actor or a Vector key. This is the utility-AI input pattern: keep a
 * "distance to target" key fresh and let FloatCompare/DistanceCheck
 * decorators gate branches on it.
 */
UCLASS(DisplayName = "BT Forge: Update Distance (periodic)")
class BTFORGERUNTIME_API UBTForgeService_UpdateDistance : public UBTService
{
	GENERATED_BODY()

public:
	UBTForgeService_UpdateDistance();

	virtual void InitializeFromAsset(UBehaviorTree& Asset) override;
	virtual void TickNode(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;
	virtual FString GetStaticDescription() const override;

protected:
	/** measured from: Object key (actor) or Vector key */
	UPROPERTY(EditAnywhere, Category = "BT Forge")
	FBlackboardKeySelector SourceKey;

	/** measured to: Object key (actor) or Vector key */
	UPROPERTY(EditAnywhere, Category = "BT Forge")
	FBlackboardKeySelector TargetKey;

	/** result key (Float) */
	UPROPERTY(EditAnywhere, Category = "BT Forge")
	FBlackboardKeySelector ResultKey;
};

/**
 * Decorators
 */

UENUM(BlueprintType)
enum class EBTForgeFloatComparison : uint8
{
	Less			UMETA(DisplayName = "Is Less Than"),
	LessOrEqual		UMETA(DisplayName = "Is Less Than Or Equal To"),
	Greater			UMETA(DisplayName = "Is Greater Than"),
	GreaterOrEqual	UMETA(DisplayName = "Is Greater Than Or Equal To"),
	Equal			UMETA(DisplayName = "Is Equal To"),
	NotEqual		UMETA(DisplayName = "Is Not Equal To")
};

/**
 * Numeric-threshold condition: BlackboardKey <Operator> (CompareToKey or
 * CompareToValue). The engine's CompareBBEntries only compares two keys with
 * Equal/NotEqual; thresholds like "Health < 30" need this. Both keys must be
 * Float or Int. Observes the involved keys and aborts the underlying branch
 * when the result flips.
 */
UCLASS(DisplayName = "BT Forge: Float Compare")
class BTFORGERUNTIME_API UBTForgeDecorator_FloatCompare : public UBTDecorator
{
	GENERATED_BODY()

public:
	UBTForgeDecorator_FloatCompare();

	virtual void InitializeFromAsset(UBehaviorTree& Asset) override;
	virtual bool CalculateRawConditionValue(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) const override;
	virtual void OnBecomeRelevant(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual void OnCeaseRelevant(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	// Not an engine virtual: the observer delegate binds this exact signature
	// (same pattern as UBTDecorator_CompareBBEntries).
	virtual EBlackboardNotificationResult OnBlackboardKeyValueChange(const UBlackboardComponent& Blackboard, FBlackboard::FKey ChangedKeyID);
	virtual FString GetStaticDescription() const override;

protected:
	/** key to test (Float or Int) */
	UPROPERTY(EditAnywhere, Category = "BT Forge")
	FBlackboardKeySelector BlackboardKey;

	/** comparison applied to the key's value */
	UPROPERTY(EditAnywhere, Category = "BT Forge")
	EBTForgeFloatComparison Operator = EBTForgeFloatComparison::Less;

	/** comparison against this key when set (Float or Int) */
	UPROPERTY(EditAnywhere, Category = "BT Forge")
	FBlackboardKeySelector CompareToKey;

	/** comparison against this constant when CompareToKey is not set */
	UPROPERTY(EditAnywhere, Category = "BT Forge")
	double CompareToValue = 0.0;
};

/**
 * Distance-range condition: passes while the distance between two location
 * sources (Object actor keys or Vector keys) lies inside
 * [MinDistance, MaxDistance]. A negative bound disables that side. With both
 * bounds disabled the condition fails closed (a range check that checks
 * nothing should not gate a branch). Observes both keys.
 */
UCLASS(DisplayName = "BT Forge: Distance Check")
class BTFORGERUNTIME_API UBTForgeDecorator_DistanceCheck : public UBTDecorator
{
	GENERATED_BODY()

public:
	UBTForgeDecorator_DistanceCheck();

	virtual void InitializeFromAsset(UBehaviorTree& Asset) override;
	virtual bool CalculateRawConditionValue(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) const override;
	virtual void OnBecomeRelevant(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual void OnCeaseRelevant(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual EBlackboardNotificationResult OnBlackboardKeyValueChange(const UBlackboardComponent& Blackboard, FBlackboard::FKey ChangedKeyID);
	virtual FString GetStaticDescription() const override;

protected:
	/** measured from: Object key (actor) or Vector key */
	UPROPERTY(EditAnywhere, Category = "BT Forge")
	FBlackboardKeySelector SourceKey;

	/** measured to: Object key (actor) or Vector key */
	UPROPERTY(EditAnywhere, Category = "BT Forge")
	FBlackboardKeySelector TargetKey;

	/** minimum distance; negative disables the lower bound */
	UPROPERTY(EditAnywhere, Category = "BT Forge")
	double MinDistance = -1.0;

	/** maximum distance; negative disables the upper bound */
	UPROPERTY(EditAnywhere, Category = "BT Forge")
	double MaxDistance = -1.0;
};

/**
 * Passes the condition with Probability percent (stateless random per check).
 * Useful to keep a selector from always choosing the same branch. Probability
 * >= 100 always passes, <= 0 never does (exact at the boundaries).
 */
UCLASS(DisplayName = "BT Forge: Chance")
class BTFORGERUNTIME_API UBTForgeDecorator_Chance : public UBTDecorator
{
	GENERATED_BODY()

public:
	UBTForgeDecorator_Chance();

	virtual bool CalculateRawConditionValue(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) const override;
	virtual FString GetStaticDescription() const override;

protected:
	/** pass probability in percent (0..100) */
	UPROPERTY(EditAnywhere, Category = "BT Forge", meta = (ClampMin = "0.0", ClampMax = "100.0"))
	double Probability = 50.0;
};
