#include "BTForgeBlueprintLibrary.h"

#include "AIController.h"
#include "AssetToolsModule.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/Decorators/BTDecorator_BlueprintBase.h"
#include "BehaviorTree/Services/BTService_BlueprintBase.h"
#include "BehaviorTree/Tasks/BTTask_BlueprintBase.h"
#include "BTForgeLibrary.h"
#include "Dom/JsonObject.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Factories/BlueprintFactory.h"
#include "GameFramework/Pawn.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Package.h"

namespace
{
	FString JsonToString(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		auto Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	FString MakeErrorJson(const FString& Message)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetBoolField(TEXT("ok"), false);
		Object->SetStringField(TEXT("error"), Message);
		return JsonToString(Object.ToSharedRef());
	}

	/** "/Game/AI/BTA_Log" -> package "/Game/AI", name "BTA_Log". */
	bool SplitAssetPath(const FString& AssetPath, FString& OutPackagePath, FString& OutAssetName)
	{
		if (!AssetPath.StartsWith(TEXT("/Game/")) || AssetPath.EndsWith(TEXT("/")))
		{
			return false;
		}
		FString Path = AssetPath;
		int32 LastSlash = INDEX_NONE;
		if (!Path.FindLastChar(TEXT('/'), LastSlash) || LastSlash <= 0)
		{
			return false;
		}
		OutAssetName = Path.Mid(LastSlash + 1);
		OutPackagePath = Path.Left(LastSlash);
		return OutAssetName.Len() > 0 && OutPackagePath.Len() > 5; // at least "/Game/X"
	}

	/** Remove the half-created asset so a failed call leaves nothing behind. */
	void DeleteHalfCreatedAsset(const FString& AssetPath)
	{
		if (GEditor)
		{
			if (UEditorAssetSubsystem* AssetSubsystem = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>())
			{
				AssetSubsystem->DeleteAsset(AssetPath);
			}
		}
	}

	/**
	 * A constrained event-graph recipe (M6 part 2). The graph shape is fixed;
	 * the only degrees of freedom are the ones in this struct, and each one is
	 * validated before any asset is created.
	 */
	struct FBForgeRecipe
	{
		FName Function;
		UClass* TargetBaseClass = nullptr; // AAIController or APawn
		FName TargetEventPin;              // OwnerController or ControlledPawn
		FName BlueprintEvent;              // ReceiveExecuteAI / ReceiveActivationAI
		bool bIsTask = false;
	};

	/** Validate RecipeJson against the parent class; nothing has been created yet. */
	bool ParseRecipe(const FString& RecipeJson, UClass* ParentClass, FBForgeRecipe& Out, FString& OutError)
	{
		TSharedPtr<FJsonObject> Spec;
		{
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RecipeJson);
			if (!FJsonSerializer::Deserialize(Reader, Spec) || !Spec.IsValid())
			{
				OutError = TEXT("RecipeJson is not a valid JSON object, e.g. {\"recipe\":\"call_function\",\"target\":\"controller\",\"function\":\"ForceNetUpdate\"}");
				return false;
			}
		}
		FString Kind;
		Spec->TryGetStringField(TEXT("recipe"), Kind);
		if (!Kind.Equals(TEXT("call_function")))
		{
			OutError = TEXT("unknown recipe '") + Kind + TEXT("'. The only recipe in this release is \"call_function\"");
			return false;
		}

		Out.bIsTask = ParentClass->IsChildOf(UBTTask_BlueprintBase::StaticClass());
		const bool bIsService = ParentClass->IsChildOf(UBTService_BlueprintBase::StaticClass());
		if (!Out.bIsTask && !bIsService)
		{
			OutError = TEXT("event-graph recipes require a UBTTask_BlueprintBase or UBTService_BlueprintBase parent; '")
				+ ParentClass->GetPathName() + TEXT("' is not one");
			return false;
		}

		FString Target = TEXT("controller");
		Spec->TryGetStringField(TEXT("target"), Target);
		if (Target.Equals(TEXT("pawn"), ESearchCase::IgnoreCase))
		{
			Out.TargetBaseClass = APawn::StaticClass();
			Out.TargetEventPin = TEXT("ControlledPawn");
		}
		else if (Target.Equals(TEXT("controller"), ESearchCase::IgnoreCase))
		{
			Out.TargetBaseClass = AAIController::StaticClass();
			Out.TargetEventPin = TEXT("OwnerController");
		}
		else
		{
			OutError = TEXT("unknown recipe target '") + Target + TEXT("'. Allowed: \"controller\", \"pawn\"");
			return false;
		}

		FString FunctionName;
		Spec->TryGetStringField(TEXT("function"), FunctionName);
		if (FunctionName.IsEmpty())
		{
			OutError = TEXT("recipe needs a \"function\" name, e.g. \"ForceNetUpdate\"");
			return false;
		}
		Out.Function = FName(*FunctionName);
		const UFunction* Function = Out.TargetBaseClass->FindFunctionByName(Out.Function);
		if (Function == nullptr)
		{
			OutError = FString::Printf(TEXT("function '%s' not found on %s; recipes call zero-parameter BlueprintCallable functions of the target"),
				*FunctionName, *Out.TargetBaseClass->GetName());
			return false;
		}
		if (Function->GetBoolMetaData(TEXT("DeprecatedFunction")))
		{
			OutError = FString::Printf(TEXT("function '%s' is deprecated"), *FunctionName);
			return false;
		}
		if (Function->HasAnyFunctionFlags(FUNC_Static))
		{
			OutError = FString::Printf(TEXT("function '%s' is static; recipes call instance functions on the %s"),
				*FunctionName, *Out.TargetBaseClass->GetName());
			return false;
		}
		if (!Function->HasAnyFunctionFlags(FUNC_BlueprintCallable))
		{
			OutError = FString::Printf(TEXT("function '%s' is not BlueprintCallable; recipes can only wire functions the blueprint VM may call"), *FunctionName);
			return false;
		}
		if (Function->HasAnyFunctionFlags(FUNC_BlueprintPure))
		{
			OutError = FString::Printf(TEXT("function '%s' is pure; recipes wire the execution chain, and pure functions have no exec pins"), *FunctionName);
			return false;
		}
		if (Function->NumParms != 0)
		{
			OutError = FString::Printf(TEXT("function '%s' takes %d parameter(s); this recipe only wires zero-parameter functions"), *FunctionName, Function->NumParms);
			return false;
		}

		// The AI event variant is what the runtime calls for an AIController
		// owner; it carries OwnerController/ControlledPawn output pins.
		Out.BlueprintEvent = Out.bIsTask ? FName(TEXT("ReceiveExecuteAI")) : FName(TEXT("ReceiveActivationAI"));
		return true;
	}

	/** Place a K2 node into the graph following the engine's own sequence. */
	void PlaceK2Node(UEdGraph* Graph, UK2Node* Node)
	{
		Node->CreateNewGuid();
		Node->PostPlacedNewNode();
		Node->SetFlags(RF_Transactional);
		Node->AllocateDefaultPins();
		Graph->AddNode(Node);
	}

	/** Synthesize the recipe's event graph. Fixed shape: event -> call -> (task: finish). */
	bool SynthesizeRecipeGraph(UBlueprint* Blueprint, const FBForgeRecipe& Recipe, FString& OutError)
	{
		UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
		if (Graph == nullptr)
		{
			OutError = TEXT("blueprint has no event graph");
			return false;
		}
		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

		UK2Node_Event* EventNode = NewObject<UK2Node_Event>(Graph);
		EventNode->EventReference.SetExternalMember(Recipe.BlueprintEvent, Recipe.bIsTask
			? TSubclassOf<UObject>(UBTTask_BlueprintBase::StaticClass())
			: TSubclassOf<UObject>(UBTService_BlueprintBase::StaticClass()));
		EventNode->bOverrideFunction = true;
		PlaceK2Node(Graph, EventNode);

		UEdGraphPin* ExecCursor = EventNode->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
		UEdGraphPin* TargetPin = EventNode->FindPin(Recipe.TargetEventPin, EGPD_Output);
		if (ExecCursor == nullptr || TargetPin == nullptr)
		{
			OutError = TEXT("event node is missing its exec/output pins");
			return false;
		}

		UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Graph);
		CallNode->FunctionReference.SetExternalMember(Recipe.Function, Recipe.TargetBaseClass);
		PlaceK2Node(Graph, CallNode);
		UEdGraphPin* CallExec = CallNode->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
		UEdGraphPin* CallSelf = CallNode->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
		if (CallExec == nullptr || CallSelf == nullptr)
		{
			OutError = FString::Printf(TEXT("'%s' has no exec/self pins (not callable?)"), *Recipe.Function.ToString());
			return false;
		}
		Schema->TryCreateConnection(ExecCursor, CallExec);
		Schema->TryCreateConnection(CallSelf, TargetPin);
		ExecCursor = CallNode->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);

		if (Recipe.bIsTask)
		{
			// Tasks stay active until FinishExecute; the recipe finishes with success.
			UK2Node_CallFunction* FinishNode = NewObject<UK2Node_CallFunction>(Graph);
			FinishNode->FunctionReference.SetExternalMember(TEXT("FinishExecute"), UBTTask_BlueprintBase::StaticClass());
			PlaceK2Node(Graph, FinishNode);
			UEdGraphPin* FinishExec = FinishNode->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
			if (FinishExec != nullptr && ExecCursor != nullptr)
			{
				Schema->TryCreateConnection(ExecCursor, FinishExec);
			}
			// Unconnected self pin defaults to Self; pin the success literal.
			if (UEdGraphPin* SuccessPin = FinishNode->FindPin(TEXT("bSuccess"), EGPD_Input))
			{
				SuccessPin->DefaultValue = TEXT("true");
			}
		}
		return true;
	}
}

FString UBTForgeBlueprintLibrary::CreateNodeBlueprint(const FString& AssetPath, const FString& ParentClassPath, const FString& ParamsJson, const FString& RecipeJson)
{
	FString PackagePath;
	FString AssetName;
	if (!SplitAssetPath(AssetPath, PackagePath, AssetName))
	{
		return MakeErrorJson(FString::Printf(TEXT("'%s' is not a valid /Game asset path, e.g. /Game/AI/BTA_LogShot"), *AssetPath));
	}
	if (LoadObject<UBlueprint>(nullptr, *AssetPath) != nullptr || StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath) != nullptr)
	{
		return MakeErrorJson(TEXT("asset already exists at '") + AssetPath + TEXT("'; nothing is overwritten"));
	}
	if (ParentClassPath.IsEmpty())
	{
		return MakeErrorJson(TEXT("ParentClassPath is required, e.g. /Script/BTForgeRuntime.BTForgeTask_LogMessage"));
	}

	UClass* ParentClass = StaticLoadClass(UObject::StaticClass(), nullptr, *ParentClassPath);
	if (ParentClass == nullptr)
	{
		return MakeErrorJson(FString::Printf(TEXT("could not load a class from '%s'. Use an object path like /Script/Module.ClassName (native) or /Game/Folder/Asset.Asset_C (blueprint)"), *ParentClassPath));
	}
	// Abstract parents are FINE - that is the standard "derive and implement"
	// pattern, and every engine *BlueprintBase is abstract (the BlueprintFactory
	// allows abstract parents by meta). Real blockers are checked below.
	// Blueprint composites do not exist in the engine (UBTCompositeNode is not
	// blueprint-creatable), so task/service/decorator are the supported bases.
	if (!ParentClass->IsChildOf(UBTTaskNode::StaticClass())
		&& !ParentClass->IsChildOf(UBTService::StaticClass())
		&& !ParentClass->IsChildOf(UBTDecorator::StaticClass()))
	{
		return MakeErrorJson(FString::Printf(TEXT("'%s' is a %s; blueprint node parents must derive from UBTTaskNode, UBTService or UBTDecorator"), *ParentClassPath, *ParentClass->GetName()));
	}
	// The engine's CreateBlueprint refuses non-Blueprintable parents; engine BT
	// node bases are MinimalAPI-only, so in practice this means a BTForge node
	// class or an engine *BlueprintBase. Say so instead of returning a bare null.
	if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
	{
		return MakeErrorJson(FString::Printf(TEXT("'%s' is not Blueprintable; derive the blueprint from a BTForge node class (/Script/BTForgeRuntime.*) or an engine BlueprintBase class instead"), *ParentClassPath));
	}
	// Validate the recipe before anything is created (validate-first contract).
	// "" and "{}" both mean "no recipe" - the official endpoint injects the
	// CPP default ("{}") when the caller omits the argument, and an empty
	// CPP default would still mark the parameter as required in the tool schema.
	FBForgeRecipe Recipe;
	const FString TrimmedRecipe = RecipeJson.TrimStartAndEnd();
	const bool bHasRecipe = !TrimmedRecipe.IsEmpty() && TrimmedRecipe != TEXT("{}");
	if (bHasRecipe)
	{
		FString RecipeError;
		if (!ParseRecipe(RecipeJson, ParentClass, Recipe, RecipeError))
		{
			return MakeErrorJson(RecipeError);
		}
	}

	FAssetToolsModule& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
	Factory->ParentClass = ParentClass;
	Factory->BlueprintType = BPTYPE_Normal;
	Factory->bSkipClassPicker = true; // never open the class-picker dialog

	UBlueprint* NewBP = Cast<UBlueprint>(AssetTools.Get().CreateAsset(AssetName, PackagePath, UBlueprint::StaticClass(), Factory));
	if (NewBP == nullptr)
	{
		return MakeErrorJson(TEXT("failed to create a blueprint asset at '") + AssetPath + TEXT("'"));
	}

	if (bHasRecipe)
	{
		FString SynthError;
		if (!SynthesizeRecipeGraph(NewBP, Recipe, SynthError))
		{
			DeleteHalfCreatedAsset(AssetPath);
			return MakeErrorJson(SynthError);
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(NewBP);
	}

	// A data-only blueprint still needs a compiled generated class: the BT
	// asset (and a cooked build) reference the BPGC, not the editor asset.
	FCompilerResultsLog CompileResults;
	FKismetEditorUtilities::CompileBlueprint(NewBP, EBlueprintCompileOptions::None, &CompileResults);
	if (NewBP->Status == BS_Error || CompileResults.NumErrors > 0)
	{
		FString FirstError;
		for (const TSharedRef<FTokenizedMessage>& Message : CompileResults.Messages)
		{
			if (Message->GetSeverity() == EMessageSeverity::Error)
			{
				FirstError = Message->ToText().ToString();
				break;
			}
		}
		DeleteHalfCreatedAsset(AssetPath);
		return MakeErrorJson(TEXT("blueprint compile failed") + (FirstError.IsEmpty() ? FString() : (TEXT(": ") + FirstError)));
	}

	UClass* GeneratedClass = NewBP->GeneratedClass.Get();
	if (GeneratedClass == nullptr)
	{
		DeleteHalfCreatedAsset(AssetPath);
		return MakeErrorJson(TEXT("blueprint has no generated class after compile"));
	}

	int32 AppliedCount = 0;
	if (!ParamsJson.IsEmpty())
	{
		FString ApplyError;
		if (!UBTForgeLibrary::ApplyParamsToObject(GeneratedClass->GetDefaultObject(), ParamsJson, AppliedCount, ApplyError))
		{
			DeleteHalfCreatedAsset(AssetPath);
			return MakeErrorJson(ApplyError);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(NewBP);
	if (GEditor)
	{
		if (UEditorAssetSubsystem* AssetSubsystem = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>())
		{
			AssetSubsystem->SaveLoadedAsset(NewBP, false);
		}
		else
		{
			return MakeErrorJson(TEXT("blueprint created but not saved: EditorAssetSubsystem unavailable at ") + AssetPath);
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("assetPath"), AssetPath);
	Result->SetStringField(TEXT("classPath"), GeneratedClass->GetPathName());
	Result->SetStringField(TEXT("parent"), ParentClass->GetPathName());
	Result->SetNumberField(TEXT("appliedParams"), AppliedCount);
	if (bHasRecipe)
	{
		Result->SetStringField(TEXT("recipe"), FString::Printf(TEXT("call_function(%s on %s)"), *Recipe.Function.ToString(), *Recipe.TargetBaseClass->GetName()));
	}
	Result->SetStringField(TEXT("note"), TEXT("use the classPath as a build_behavior_tree node 'type'; blackboard key names in params resolve per-tree at build time"));
	return JsonToString(Result);
}
