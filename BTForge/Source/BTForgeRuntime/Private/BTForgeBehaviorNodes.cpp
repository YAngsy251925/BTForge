#include "BTForgeBehaviorNodes.h"

#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "GameFramework/Actor.h"

DEFINE_LOG_CATEGORY(LogBTForge);

namespace
{
	/** Type-erased blackboard value, so one copy helper can bridge any two
	 *  compatible key types (Float -> Int, Name -> String, ...). */
	struct FBForgeValueSnapshot
	{
		enum class EKind : uint8 { None, Bool, Number, String, Name, Vector, Rotator, Object };
		EKind Kind = EKind::None;
		bool BoolValue = false;
		double NumberValue = 0.0;
		FString StringValue;
		FName NameValue;
		FVector VectorValue = FVector::ZeroVector;
		FRotator RotatorValue = FRotator::ZeroRotator;
		TWeakObjectPtr<UObject> ObjectValue;
	};

	bool ReadNumericValue(const UBlackboardComponent& Blackboard, const FBlackboardKeySelector& Key, double& OutValue)
	{
		if (Key.SelectedKeyType == UBlackboardKeyType_Int::StaticClass())
		{
			OutValue = Blackboard.GetValueAsInt(Key.SelectedKeyName);
			return true;
		}
		if (Key.SelectedKeyType == UBlackboardKeyType_Float::StaticClass())
		{
			OutValue = Blackboard.GetValueAsFloat(Key.SelectedKeyName);
			return true;
		}
		return false;
	}

	bool SnapshotKey(const UBlackboardComponent& Blackboard, const FBlackboardKeySelector& Key, FBForgeValueSnapshot& Out)
	{
		const FName KeyName = Key.SelectedKeyName;
		if (KeyName.IsNone())
		{
			return false;
		}
		if (Key.SelectedKeyType == UBlackboardKeyType_Bool::StaticClass())
		{
			Out.Kind = FBForgeValueSnapshot::EKind::Bool;
			Out.BoolValue = Blackboard.GetValueAsBool(KeyName);
			return true;
		}
		if (Key.SelectedKeyType == UBlackboardKeyType_Int::StaticClass())
		{
			Out.Kind = FBForgeValueSnapshot::EKind::Number;
			Out.NumberValue = Blackboard.GetValueAsInt(KeyName);
			return true;
		}
		if (Key.SelectedKeyType == UBlackboardKeyType_Float::StaticClass())
		{
			Out.Kind = FBForgeValueSnapshot::EKind::Number;
			Out.NumberValue = Blackboard.GetValueAsFloat(KeyName);
			return true;
		}
		if (Key.SelectedKeyType == UBlackboardKeyType_String::StaticClass())
		{
			Out.Kind = FBForgeValueSnapshot::EKind::String;
			Out.StringValue = Blackboard.GetValueAsString(KeyName);
			return true;
		}
		if (Key.SelectedKeyType == UBlackboardKeyType_Name::StaticClass())
		{
			Out.Kind = FBForgeValueSnapshot::EKind::Name;
			Out.NameValue = Blackboard.GetValueAsName(KeyName);
			return true;
		}
		if (Key.SelectedKeyType == UBlackboardKeyType_Vector::StaticClass())
		{
			Out.Kind = FBForgeValueSnapshot::EKind::Vector;
			Out.VectorValue = Blackboard.GetValueAsVector(KeyName);
			return true;
		}
		if (Key.SelectedKeyType == UBlackboardKeyType_Rotator::StaticClass())
		{
			Out.Kind = FBForgeValueSnapshot::EKind::Rotator;
			Out.RotatorValue = Blackboard.GetValueAsRotator(KeyName);
			return true;
		}
		if (Key.SelectedKeyType == UBlackboardKeyType_Object::StaticClass())
		{
			Out.Kind = FBForgeValueSnapshot::EKind::Object;
			Out.ObjectValue = Blackboard.GetValueAsObject(KeyName);
			return true;
		}
		return false;
	}

	bool ApplySnapshot(UBlackboardComponent& Blackboard, const FBlackboardKeySelector& Destination, const FBForgeValueSnapshot& In)
	{
		const FName KeyName = Destination.SelectedKeyName;
		if (KeyName.IsNone())
		{
			return false;
		}
		if (Destination.SelectedKeyType == UBlackboardKeyType_Bool::StaticClass())
		{
			if (In.Kind == FBForgeValueSnapshot::EKind::Bool)
			{
				Blackboard.SetValueAsBool(KeyName, In.BoolValue);
				return true;
			}
			if (In.Kind == FBForgeValueSnapshot::EKind::Number)
			{
				Blackboard.SetValueAsBool(KeyName, In.NumberValue != 0.0);
				return true;
			}
			return false;
		}
		if (Destination.SelectedKeyType == UBlackboardKeyType_Int::StaticClass())
		{
			if (In.Kind == FBForgeValueSnapshot::EKind::Number)
			{
				Blackboard.SetValueAsInt(KeyName, FMath::RoundToInt(In.NumberValue));
				return true;
			}
			if (In.Kind == FBForgeValueSnapshot::EKind::Bool)
			{
				Blackboard.SetValueAsInt(KeyName, In.BoolValue ? 1 : 0);
				return true;
			}
			return false;
		}
		if (Destination.SelectedKeyType == UBlackboardKeyType_Float::StaticClass())
		{
			if (In.Kind == FBForgeValueSnapshot::EKind::Number)
			{
				Blackboard.SetValueAsFloat(KeyName, static_cast<float>(In.NumberValue));
				return true;
			}
			return false;
		}
		if (Destination.SelectedKeyType == UBlackboardKeyType_String::StaticClass())
		{
			if (In.Kind == FBForgeValueSnapshot::EKind::String)
			{
				Blackboard.SetValueAsString(KeyName, In.StringValue);
				return true;
			}
			if (In.Kind == FBForgeValueSnapshot::EKind::Name)
			{
				Blackboard.SetValueAsString(KeyName, In.NameValue.ToString());
				return true;
			}
			return false;
		}
		if (Destination.SelectedKeyType == UBlackboardKeyType_Name::StaticClass())
		{
			if (In.Kind == FBForgeValueSnapshot::EKind::Name)
			{
				Blackboard.SetValueAsName(KeyName, In.NameValue);
				return true;
			}
			if (In.Kind == FBForgeValueSnapshot::EKind::String)
			{
				Blackboard.SetValueAsName(KeyName, FName(*In.StringValue));
				return true;
			}
			return false;
		}
		if (Destination.SelectedKeyType == UBlackboardKeyType_Vector::StaticClass())
		{
			if (In.Kind == FBForgeValueSnapshot::EKind::Vector)
			{
				Blackboard.SetValueAsVector(KeyName, In.VectorValue);
				return true;
			}
			return false;
		}
		if (Destination.SelectedKeyType == UBlackboardKeyType_Rotator::StaticClass())
		{
			if (In.Kind == FBForgeValueSnapshot::EKind::Rotator)
			{
				Blackboard.SetValueAsRotator(KeyName, In.RotatorValue);
				return true;
			}
			return false;
		}
		if (Destination.SelectedKeyType == UBlackboardKeyType_Object::StaticClass())
		{
			if (In.Kind == FBForgeValueSnapshot::EKind::Object)
			{
				Blackboard.SetValueAsObject(KeyName, In.ObjectValue.Get());
				return true;
			}
			return false;
		}
		return false;
	}

	bool ResolveLocationFromKey(const UBlackboardComponent& Blackboard, const FBlackboardKeySelector& Key, FVector& OutLocation)
	{
		const FName KeyName = Key.SelectedKeyName;
		if (KeyName.IsNone())
		{
			return false;
		}
		if (Key.SelectedKeyType == UBlackboardKeyType_Object::StaticClass())
		{
			const AActor* Actor = Cast<const AActor>(Blackboard.GetValueAsObject(KeyName));
			if (Actor != nullptr)
			{
				OutLocation = Actor->GetActorLocation();
				return true;
			}
			return false;
		}
		if (Key.SelectedKeyType == UBlackboardKeyType_Vector::StaticClass())
		{
			OutLocation = Blackboard.GetValueAsVector(KeyName);
			return true;
		}
		return false;
	}

	const TCHAR* ToString(EBTForgeArithmeticOperation Operation)
	{
		switch (Operation)
		{
		case EBTForgeArithmeticOperation::Add: return TEXT("+");
		case EBTForgeArithmeticOperation::Subtract: return TEXT("-");
		case EBTForgeArithmeticOperation::Multiply: return TEXT("*");
		case EBTForgeArithmeticOperation::Divide: return TEXT("/");
		case EBTForgeArithmeticOperation::Min: return TEXT("min");
		case EBTForgeArithmeticOperation::Max: return TEXT("max");
		default: return TEXT("?");
		}
	}

	const TCHAR* ToString(EBTForgeFloatComparison Operator)
	{
		switch (Operator)
		{
		case EBTForgeFloatComparison::Less: return TEXT("<");
		case EBTForgeFloatComparison::LessOrEqual: return TEXT("<=");
		case EBTForgeFloatComparison::Greater: return TEXT(">");
		case EBTForgeFloatComparison::GreaterOrEqual: return TEXT(">=");
		case EBTForgeFloatComparison::Equal: return TEXT("==");
		case EBTForgeFloatComparison::NotEqual: return TEXT("!=");
		default: return TEXT("?");
		}
	}

	/** Resolve-or-invalidate shared by every node with key selectors. Optional
	 *  selectors (SelectedKeyName None) must stay unresolved: ResolveSelectedKey
	 *  on an unset selector runs InitSelection, which silently binds the first
	 *  compatible blackboard key and turns "not configured" into "wrong key". */
	void ResolveSelector(FBlackboardKeySelector& Selector, const UBlackboardData* BlackboardAsset)
	{
		if (BlackboardAsset != nullptr && !Selector.SelectedKeyName.IsNone())
		{
			Selector.ResolveSelectedKey(*BlackboardAsset);
		}
		else
		{
			Selector.InvalidateResolvedKey();
		}
	}
}

//////////////////////////////////////////////////////////////////////////
// UBTForgeTask_ComputeValue

UBTForgeTask_ComputeValue::UBTForgeTask_ComputeValue()
{
	NodeName = TEXT("BT Forge Compute");
}

void UBTForgeTask_ComputeValue::InitializeFromAsset(UBehaviorTree& Asset)
{
	Super::InitializeFromAsset(Asset);
	const UBlackboardData* BlackboardAsset = GetBlackboardAsset();
	ResolveSelector(LeftKey, BlackboardAsset);
	ResolveSelector(RightKey, BlackboardAsset);
	ResolveSelector(ResultKey, BlackboardAsset);
}

EBTNodeResult::Type UBTForgeTask_ComputeValue::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (Blackboard == nullptr)
	{
		UE_LOG(LogBTForge, Warning, TEXT("Compute: no blackboard component"));
		return EBTNodeResult::Failed;
	}
	if (LeftKey.SelectedKeyName.IsNone() || ResultKey.SelectedKeyName.IsNone())
	{
		UE_LOG(LogBTForge, Warning, TEXT("Compute: LeftKey and ResultKey must be set"));
		return EBTNodeResult::Failed;
	}

	double Left = 0.0;
	if (!ReadNumericValue(*Blackboard, LeftKey, Left))
	{
		UE_LOG(LogBTForge, Warning, TEXT("Compute: '%s' is not a Float/Int key"), *LeftKey.SelectedKeyName.ToString());
		return EBTNodeResult::Failed;
	}

	double Right = RightConstant;
	if (!RightKey.SelectedKeyName.IsNone() && !ReadNumericValue(*Blackboard, RightKey, Right))
	{
		UE_LOG(LogBTForge, Warning, TEXT("Compute: '%s' is not a Float/Int key"), *RightKey.SelectedKeyName.ToString());
		return EBTNodeResult::Failed;
	}

	double Outcome = 0.0;
	switch (Operation)
	{
	case EBTForgeArithmeticOperation::Add: Outcome = Left + Right; break;
	case EBTForgeArithmeticOperation::Subtract: Outcome = Left - Right; break;
	case EBTForgeArithmeticOperation::Multiply: Outcome = Left * Right; break;
	case EBTForgeArithmeticOperation::Divide:
		if (FMath::IsNearlyZero(Right))
		{
			UE_LOG(LogBTForge, Warning, TEXT("Compute: division by zero (%s / %s)"), *LeftKey.SelectedKeyName.ToString(), *RightKey.SelectedKeyName.ToString());
			return EBTNodeResult::Failed;
		}
		Outcome = Left / Right;
		break;
	case EBTForgeArithmeticOperation::Min: Outcome = FMath::Min(Left, Right); break;
	case EBTForgeArithmeticOperation::Max: Outcome = FMath::Max(Left, Right); break;
	default:
		return EBTNodeResult::Failed;
	}

	if (ResultKey.SelectedKeyType == UBlackboardKeyType_Int::StaticClass())
	{
		Blackboard->SetValueAsInt(ResultKey.SelectedKeyName, FMath::RoundToInt(Outcome));
	}
	else if (ResultKey.SelectedKeyType == UBlackboardKeyType_Float::StaticClass())
	{
		Blackboard->SetValueAsFloat(ResultKey.SelectedKeyName, static_cast<float>(Outcome));
	}
	else
	{
		UE_LOG(LogBTForge, Warning, TEXT("Compute: ResultKey '%s' is not a Float/Int key"), *ResultKey.SelectedKeyName.ToString());
		return EBTNodeResult::Failed;
	}
	return EBTNodeResult::Succeeded;
}

FString UBTForgeTask_ComputeValue::GetStaticDescription() const
{
	const FString Right = RightKey.SelectedKeyName.IsNone()
		? FString::Printf(TEXT("%.2f"), RightConstant)
		: RightKey.SelectedKeyName.ToString();
	return FString::Printf(TEXT("%s: %s %s %s -> %s"),
		*Super::GetStaticDescription(),
		*LeftKey.SelectedKeyName.ToString(),
		ToString(Operation),
		*Right,
		*ResultKey.SelectedKeyName.ToString());
}

//////////////////////////////////////////////////////////////////////////
// UBTForgeTask_LogMessage

UBTForgeTask_LogMessage::UBTForgeTask_LogMessage()
{
	NodeName = TEXT("BT Forge Log");
}

void UBTForgeTask_LogMessage::InitializeFromAsset(UBehaviorTree& Asset)
{
	Super::InitializeFromAsset(Asset);
	ResolveSelector(KeyToLog, GetBlackboardAsset());
}

namespace
{
	FString DescribeKeyValueText(const UBlackboardComponent& Blackboard, const FBlackboardKeySelector& Key)
	{
		const FName KeyName = Key.SelectedKeyName;
		if (KeyName.IsNone())
		{
			return FString();
		}
		if (Key.SelectedKeyType == UBlackboardKeyType_Bool::StaticClass())
		{
			return FString::Printf(TEXT("%s=%s"), *KeyName.ToString(), Blackboard.GetValueAsBool(KeyName) ? TEXT("true") : TEXT("false"));
		}
		if (Key.SelectedKeyType == UBlackboardKeyType_Int::StaticClass())
		{
			return FString::Printf(TEXT("%s=%d"), *KeyName.ToString(), Blackboard.GetValueAsInt(KeyName));
		}
		if (Key.SelectedKeyType == UBlackboardKeyType_Float::StaticClass())
		{
			return FString::Printf(TEXT("%s=%.3f"), *KeyName.ToString(), Blackboard.GetValueAsFloat(KeyName));
		}
		if (Key.SelectedKeyType == UBlackboardKeyType_String::StaticClass())
		{
			return FString::Printf(TEXT("%s=\"%s\""), *KeyName.ToString(), *Blackboard.GetValueAsString(KeyName));
		}
		if (Key.SelectedKeyType == UBlackboardKeyType_Name::StaticClass())
		{
			return FString::Printf(TEXT("%s=%s"), *KeyName.ToString(), *Blackboard.GetValueAsName(KeyName).ToString());
		}
		if (Key.SelectedKeyType == UBlackboardKeyType_Vector::StaticClass())
		{
			return FString::Printf(TEXT("%s=%s"), *KeyName.ToString(), *Blackboard.GetValueAsVector(KeyName).ToString());
		}
		if (Key.SelectedKeyType == UBlackboardKeyType_Rotator::StaticClass())
		{
			return FString::Printf(TEXT("%s=%s"), *KeyName.ToString(), *Blackboard.GetValueAsRotator(KeyName).ToString());
		}
		if (Key.SelectedKeyType == UBlackboardKeyType_Object::StaticClass())
		{
			const UObject* Value = Blackboard.GetValueAsObject(KeyName);
			return FString::Printf(TEXT("%s=%s"), *KeyName.ToString(), Value ? *Value->GetName() : TEXT("None"));
		}
		return KeyName.ToString();
	}
}

EBTNodeResult::Type UBTForgeTask_LogMessage::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	FString Line = Message;
	const UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (Blackboard != nullptr && !KeyToLog.SelectedKeyName.IsNone())
	{
		Line += TEXT(" [") + DescribeKeyValueText(*Blackboard, KeyToLog) + TEXT("]");
	}

	switch (Severity)
	{
	case EBTForgeLogSeverity::Warning:
		UE_LOG(LogBTForge, Warning, TEXT("%s"), *Line);
		break;
	case EBTForgeLogSeverity::Error:
		UE_LOG(LogBTForge, Error, TEXT("%s"), *Line);
		break;
	case EBTForgeLogSeverity::Log:
	default:
		UE_LOG(LogBTForge, Log, TEXT("%s"), *Line);
		break;
	}
	return EBTNodeResult::Succeeded;
}

FString UBTForgeTask_LogMessage::GetStaticDescription() const
{
	const FString KeyPart = KeyToLog.SelectedKeyName.IsNone() ? FString() : FString::Printf(TEXT(" [%s]"), *KeyToLog.SelectedKeyName.ToString());
	return FString::Printf(TEXT("%s: '%s'%s (%s)"), *Super::GetStaticDescription(), *Message, *KeyPart, *UEnum::GetValueAsString(Severity));
}

//////////////////////////////////////////////////////////////////////////
// UBTForgeService_SetBlackboardValue

UBTForgeService_SetBlackboardValue::UBTForgeService_SetBlackboardValue()
{
	NodeName = TEXT("BT Forge Set Value");
	Interval = 0.5f;
	RandomDeviation = 0.0f;
	INIT_SERVICE_NODE_NOTIFY_FLAGS();
}

void UBTForgeService_SetBlackboardValue::InitializeFromAsset(UBehaviorTree& Asset)
{
	Super::InitializeFromAsset(Asset);
	const UBlackboardData* BlackboardAsset = GetBlackboardAsset();
	ResolveSelector(KeyToSet, BlackboardAsset);
	ResolveSelector(SourceKey, BlackboardAsset);
}

void UBTForgeService_SetBlackboardValue::TickNode(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds)
{
	Super::TickNode(OwnerComp, NodeMemory, DeltaSeconds);

	UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (Blackboard == nullptr || KeyToSet.SelectedKeyName.IsNone())
	{
		return;
	}

	if (!SourceKey.SelectedKeyName.IsNone())
	{
		FBForgeValueSnapshot Snapshot;
		if (SnapshotKey(*Blackboard, SourceKey, Snapshot) && ApplySnapshot(*Blackboard, KeyToSet, Snapshot))
		{
			return;
		}
		UE_LOG(LogBTForge, Warning, TEXT("Set Value service: cannot copy '%s' into '%s' (incompatible types)"),
			*SourceKey.SelectedKeyName.ToString(), *KeyToSet.SelectedKeyName.ToString());
		return;
	}

	const FName KeyName = KeyToSet.SelectedKeyName;
	if (KeyToSet.SelectedKeyType == UBlackboardKeyType_Float::StaticClass())
	{
		Blackboard->SetValueAsFloat(KeyName, static_cast<float>(Constant));
	}
	else if (KeyToSet.SelectedKeyType == UBlackboardKeyType_Int::StaticClass())
	{
		Blackboard->SetValueAsInt(KeyName, FMath::RoundToInt(Constant));
	}
	else if (KeyToSet.SelectedKeyType == UBlackboardKeyType_Bool::StaticClass())
	{
		Blackboard->SetValueAsBool(KeyName, bConstantBool);
	}
	else if (KeyToSet.SelectedKeyType == UBlackboardKeyType_String::StaticClass())
	{
		Blackboard->SetValueAsString(KeyName, ConstantString);
	}
	else if (KeyToSet.SelectedKeyType == UBlackboardKeyType_Name::StaticClass())
	{
		Blackboard->SetValueAsName(KeyName, FName(*ConstantString));
	}
	else if (KeyToSet.SelectedKeyType == UBlackboardKeyType_Vector::StaticClass())
	{
		Blackboard->SetValueAsVector(KeyName, ConstantVector);
	}
	else if (KeyToSet.SelectedKeyType == UBlackboardKeyType_Rotator::StaticClass())
	{
		Blackboard->SetValueAsRotator(KeyName, ConstantRotator);
	}
	else
	{
		// Object/Class keys have no meaningful constant; copy from SourceKey instead.
		UE_LOG(LogBTForge, Verbose, TEXT("Set Value service: key '%s' needs a SourceKey (no constant for Object/Class keys)"), *KeyName.ToString());
	}
}

FString UBTForgeService_SetBlackboardValue::GetStaticDescription() const
{
	const FString ValueText = SourceKey.SelectedKeyName.IsNone()
		? FString::Printf(TEXT("%.2f"), Constant)
		: FString::Printf(TEXT("copy of %s"), *SourceKey.SelectedKeyName.ToString());
	return FString::Printf(TEXT("%s: %s = %s (every %.2fs)"),
		*Super::GetStaticDescription(), *KeyToSet.SelectedKeyName.ToString(), *ValueText, Interval);
}

//////////////////////////////////////////////////////////////////////////
// UBTForgeService_UpdateDistance

UBTForgeService_UpdateDistance::UBTForgeService_UpdateDistance()
{
	NodeName = TEXT("BT Forge Update Distance");
	Interval = 0.25f;
	RandomDeviation = 0.0f;
	INIT_SERVICE_NODE_NOTIFY_FLAGS();
}

void UBTForgeService_UpdateDistance::InitializeFromAsset(UBehaviorTree& Asset)
{
	Super::InitializeFromAsset(Asset);
	const UBlackboardData* BlackboardAsset = GetBlackboardAsset();
	ResolveSelector(SourceKey, BlackboardAsset);
	ResolveSelector(TargetKey, BlackboardAsset);
	ResolveSelector(ResultKey, BlackboardAsset);
}

void UBTForgeService_UpdateDistance::TickNode(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds)
{
	Super::TickNode(OwnerComp, NodeMemory, DeltaSeconds);

	UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (Blackboard == nullptr || ResultKey.SelectedKeyName.IsNone())
	{
		return;
	}

	FVector SourceLocation = FVector::ZeroVector;
	FVector TargetLocation = FVector::ZeroVector;
	if (!ResolveLocationFromKey(*Blackboard, SourceKey, SourceLocation) || !ResolveLocationFromKey(*Blackboard, TargetKey, TargetLocation))
	{
		UE_LOG(LogBTForge, Verbose, TEXT("Update Distance service: '%s' or '%s' has no location (Object actor or Vector key required)"),
			*SourceKey.SelectedKeyName.ToString(), *TargetKey.SelectedKeyName.ToString());
		return;
	}
	Blackboard->SetValueAsFloat(ResultKey.SelectedKeyName, static_cast<float>(FVector::Dist(SourceLocation, TargetLocation)));
}

FString UBTForgeService_UpdateDistance::GetStaticDescription() const
{
	return FString::Printf(TEXT("%s: %s = dist(%s, %s) (every %.2fs)"),
		*Super::GetStaticDescription(), *ResultKey.SelectedKeyName.ToString(),
		*SourceKey.SelectedKeyName.ToString(), *TargetKey.SelectedKeyName.ToString(), Interval);
}

//////////////////////////////////////////////////////////////////////////
// UBTForgeDecorator_FloatCompare

UBTForgeDecorator_FloatCompare::UBTForgeDecorator_FloatCompare()
{
	NodeName = TEXT("BT Forge Float Compare");
	Operator = EBTForgeFloatComparison::Less;
	INIT_DECORATOR_NODE_NOTIFY_FLAGS();
}

void UBTForgeDecorator_FloatCompare::InitializeFromAsset(UBehaviorTree& Asset)
{
	Super::InitializeFromAsset(Asset);
	const UBlackboardData* BlackboardAsset = GetBlackboardAsset();
	ResolveSelector(BlackboardKey, BlackboardAsset);
	ResolveSelector(CompareToKey, BlackboardAsset);
}

	bool UBTForgeDecorator_FloatCompare::CalculateRawConditionValue(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) const
	{
		const UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
		if (Blackboard == nullptr)
		{
			return false;
		}

		double Left = 0.0;
		if (!ReadNumericValue(*Blackboard, BlackboardKey, Left))
		{
			return false;
		}

		double Right = CompareToValue;
		if (!CompareToKey.SelectedKeyName.IsNone() && !ReadNumericValue(*Blackboard, CompareToKey, Right))
		{
			return false;
		}

	switch (Operator)
	{
	case EBTForgeFloatComparison::Less: return Left < Right;
	case EBTForgeFloatComparison::LessOrEqual: return Left <= Right;
	case EBTForgeFloatComparison::Greater: return Left > Right;
	case EBTForgeFloatComparison::GreaterOrEqual: return Left >= Right;
	case EBTForgeFloatComparison::Equal: return FMath::IsNearlyEqual(Left, Right);
	case EBTForgeFloatComparison::NotEqual: return !FMath::IsNearlyEqual(Left, Right);
	default: return false;
	}
}

void UBTForgeDecorator_FloatCompare::OnBecomeRelevant(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (Blackboard != nullptr)
	{
		Blackboard->RegisterObserver(BlackboardKey.GetSelectedKeyID(), this, FOnBlackboardChangeNotification::CreateUObject(this, &UBTForgeDecorator_FloatCompare::OnBlackboardKeyValueChange));
		if (!CompareToKey.SelectedKeyName.IsNone())
		{
			Blackboard->RegisterObserver(CompareToKey.GetSelectedKeyID(), this, FOnBlackboardChangeNotification::CreateUObject(this, &UBTForgeDecorator_FloatCompare::OnBlackboardKeyValueChange));
		}
	}
}

void UBTForgeDecorator_FloatCompare::OnCeaseRelevant(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (Blackboard != nullptr)
	{
		Blackboard->UnregisterObserversFrom(this);
	}
}

EBlackboardNotificationResult UBTForgeDecorator_FloatCompare::OnBlackboardKeyValueChange(const UBlackboardComponent& Blackboard, FBlackboard::FKey ChangedKeyID)
{
	UBehaviorTreeComponent* BehaviorComp = static_cast<UBehaviorTreeComponent*>(Blackboard.GetBrainComponent());
	if (BehaviorComp == nullptr)
	{
		return EBlackboardNotificationResult::RemoveObserver;
	}
	if (BlackboardKey.GetSelectedKeyID() == ChangedKeyID || CompareToKey.GetSelectedKeyID() == ChangedKeyID)
	{
		BehaviorComp->RequestExecution(this);
	}
	return EBlackboardNotificationResult::ContinueObserving;
}

FString UBTForgeDecorator_FloatCompare::GetStaticDescription() const
{
	const FString Right = CompareToKey.SelectedKeyName.IsNone()
		? FString::Printf(TEXT("%.2f"), CompareToValue)
		: CompareToKey.SelectedKeyName.ToString();
	return FString::Printf(TEXT("%s: %s %s %s"), *Super::GetStaticDescription(),
		*BlackboardKey.SelectedKeyName.ToString(), ToString(Operator), *Right);
}

//////////////////////////////////////////////////////////////////////////
// UBTForgeDecorator_DistanceCheck

UBTForgeDecorator_DistanceCheck::UBTForgeDecorator_DistanceCheck()
{
	NodeName = TEXT("BT Forge Distance Check");
	MinDistance = -1.0;
	MaxDistance = -1.0;
	INIT_DECORATOR_NODE_NOTIFY_FLAGS();
}

void UBTForgeDecorator_DistanceCheck::InitializeFromAsset(UBehaviorTree& Asset)
{
	Super::InitializeFromAsset(Asset);
	const UBlackboardData* BlackboardAsset = GetBlackboardAsset();
	ResolveSelector(SourceKey, BlackboardAsset);
	ResolveSelector(TargetKey, BlackboardAsset);
}

bool UBTForgeDecorator_DistanceCheck::CalculateRawConditionValue(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) const
{
	const UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (Blackboard == nullptr)
	{
		return false;
	}

	FVector SourceLocation = FVector::ZeroVector;
	FVector TargetLocation = FVector::ZeroVector;
	if (!ResolveLocationFromKey(*Blackboard, SourceKey, SourceLocation) || !ResolveLocationFromKey(*Blackboard, TargetKey, TargetLocation))
	{
		UE_LOG(LogBTForge, Warning, TEXT("Distance Check: '%s' or '%s' has no location (Object actor or Vector key required) - failing closed"),
			*SourceKey.SelectedKeyName.ToString(), *TargetKey.SelectedKeyName.ToString());
		return false;
	}

	const double Distance = static_cast<double>(FVector::Dist(SourceLocation, TargetLocation));
	if (MinDistance >= 0.0 && Distance < MinDistance)
	{
		return false;
	}
	if (MaxDistance >= 0.0 && Distance > MaxDistance)
	{
		return false;
	}
	return true;
}

void UBTForgeDecorator_DistanceCheck::OnBecomeRelevant(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (Blackboard != nullptr)
	{
		Blackboard->RegisterObserver(SourceKey.GetSelectedKeyID(), this, FOnBlackboardChangeNotification::CreateUObject(this, &UBTForgeDecorator_DistanceCheck::OnBlackboardKeyValueChange));
		Blackboard->RegisterObserver(TargetKey.GetSelectedKeyID(), this, FOnBlackboardChangeNotification::CreateUObject(this, &UBTForgeDecorator_DistanceCheck::OnBlackboardKeyValueChange));
	}
}

void UBTForgeDecorator_DistanceCheck::OnCeaseRelevant(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (Blackboard != nullptr)
	{
		Blackboard->UnregisterObserversFrom(this);
	}
}

EBlackboardNotificationResult UBTForgeDecorator_DistanceCheck::OnBlackboardKeyValueChange(const UBlackboardComponent& Blackboard, FBlackboard::FKey ChangedKeyID)
{
	UBehaviorTreeComponent* BehaviorComp = static_cast<UBehaviorTreeComponent*>(Blackboard.GetBrainComponent());
	if (BehaviorComp == nullptr)
	{
		return EBlackboardNotificationResult::RemoveObserver;
	}
	if (SourceKey.GetSelectedKeyID() == ChangedKeyID || TargetKey.GetSelectedKeyID() == ChangedKeyID)
	{
		BehaviorComp->RequestExecution(this);
	}
	return EBlackboardNotificationResult::ContinueObserving;
}

FString UBTForgeDecorator_DistanceCheck::GetStaticDescription() const
{
	FString Range;
	if (MinDistance >= 0.0 && MaxDistance >= 0.0)
	{
		Range = FString::Printf(TEXT("[%.0f, %.0f]"), MinDistance, MaxDistance);
	}
	else if (MinDistance >= 0.0)
	{
		Range = FString::Printf(TEXT(">= %.0f"), MinDistance);
	}
	else if (MaxDistance >= 0.0)
	{
		Range = FString::Printf(TEXT("<= %.0f"), MaxDistance);
	}
	else
	{
		Range = TEXT("(no bounds - fails closed)");
	}
	return FString::Printf(TEXT("%s: dist(%s, %s) %s"), *Super::GetStaticDescription(),
		*SourceKey.SelectedKeyName.ToString(), *TargetKey.SelectedKeyName.ToString(), *Range);
}

//////////////////////////////////////////////////////////////////////////
// UBTForgeDecorator_Chance

UBTForgeDecorator_Chance::UBTForgeDecorator_Chance()
{
	NodeName = TEXT("BT Forge Chance");
	Probability = 50.0;
}

bool UBTForgeDecorator_Chance::CalculateRawConditionValue(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) const
{
	if (Probability >= 100.0)
	{
		return true;
	}
	if (Probability <= 0.0)
	{
		return false;
	}
	return FMath::FRand() * 100.0 < Probability;
}

FString UBTForgeDecorator_Chance::GetStaticDescription() const
{
	return FString::Printf(TEXT("%s: %.0f%%"), *Super::GetStaticDescription(), Probability);
}
