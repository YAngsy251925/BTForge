#include "BTForgeEnvQueryContext_Blackboard.h"

#include "AIController.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "EnvironmentQuery/EnvQueryTypes.h"
#include "EnvironmentQuery/Items/EnvQueryItemType_Actor.h"
#include "EnvironmentQuery/Items/EnvQueryItemType_Point.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Controller.h"

void UBTForgeEnvQueryContext_Blackboard::ProvideContext(FEnvQueryInstance& QueryInstance, FEnvQueryContextData& ContextData) const
{
	if (BlackboardKey.IsNone())
	{
		return;
	}

	UObject* QueryOwner = QueryInstance.Owner.Get();
	if (QueryOwner == nullptr)
	{
		return;
	}

	// BT EQS nodes usually pass the AIController as owner; plain actors also
	// work when they carry a BlackboardComponent.
	UBlackboardComponent* BlackboardComp = nullptr;
	AActor* OwnerActor = nullptr;
	if (AAIController* Controller = Cast<AAIController>(QueryOwner))
	{
		BlackboardComp = Controller->GetBlackboardComponent();
		OwnerActor = Controller->GetPawn();
	}
	else if (AActor* Actor = Cast<AActor>(QueryOwner))
	{
		BlackboardComp = Actor->FindComponentByClass<UBlackboardComponent>();
		OwnerActor = Actor;
	}
	if (BlackboardComp == nullptr)
	{
		return;
	}

	const UBlackboardData* BlackboardAsset = BlackboardComp->GetBlackboardAsset();
	if (BlackboardAsset == nullptr)
	{
		return;
	}
	const FBlackboard::FKey KeyID = BlackboardAsset->GetKeyID(BlackboardKey);
	const FBlackboardEntry* Entry = KeyID != FBlackboard::InvalidKey ? BlackboardAsset->GetKey(KeyID) : nullptr;
	if (Entry == nullptr || Entry->KeyType == nullptr)
	{
		return;
	}

	const UClass* KeyClass = Entry->KeyType->GetClass();
	if (KeyClass->IsChildOf(UBlackboardKeyType_Object::StaticClass()))
	{
		if (AActor* TargetActor = Cast<AActor>(BlackboardComp->GetValueAsObject(BlackboardKey)))
		{
			UEnvQueryItemType_Actor::SetContextHelper(ContextData, TargetActor);
		}
		return;
	}

	if (KeyClass->IsChildOf(UBlackboardKeyType_Vector::StaticClass()))
	{
		UEnvQueryItemType_Point::SetContextHelper(ContextData, BlackboardComp->GetValueAsVector(BlackboardKey));
		return;
	}

	if (KeyClass->IsChildOf(UBlackboardKeyType_Rotator::StaticClass()))
	{
		if (OwnerActor != nullptr)
		{
			const FRotator Rotation = BlackboardComp->GetValueAsRotator(BlackboardKey);
			UEnvQueryItemType_Point::SetContextHelper(ContextData, OwnerActor->GetActorLocation() + Rotation.Vector() * 100.0f);
		}
		return;
	}
}
