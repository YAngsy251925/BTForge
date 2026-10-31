#include "BTForgeVerifySession.h"

#include "AIController.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTreeDelegates.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/BlackboardData.h"
#include "Containers/Ticker.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryManager.h"
#include "EnvironmentQuery/EnvQueryTypes.h"
#include "GameFramework/Actor.h"
#include "GameFramework/DefaultPawn.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/Pawn.h"
#include "HAL/PlatformTime.h"
#include "HttpServerModule.h"
#include "Logging/LogMacros.h"
#include "Logging/LogVerbosity.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/OutputDevice.h"
#include "Misc/Paths.h"
#include "PlayInEditorDataTypes.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "UnrealEd.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogBTForgeVerify, Log, All);

namespace
{
	/** Map used when bareGameMode is on and the caller pinned none: the project's
	 *  current map may be a Lyra map whose Experience actors assert on a bare
	 *  AGameModeBase, so never assume "current map" in that mode. */
	const TCHAR* const BareGameModeFallbackMap = TEXT("/Engine/Maps/Templates/Template_Default");

	/** Persist result JSON to disk: the HTTP endpoint can go dark around PIE
	 *  transitions, so clients poll the file instead of relying on the server. */
	void SaveResultToDisk(const FString& SessionId, const FString& ResultJson)
	{
		const FString Dir = FPaths::ProjectSavedDir() / TEXT("BTForgeVerify");
		FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*Dir);

		// Write-then-rename, and one file per session: a poller must never read a
		// half-written JSON, and a second verify run must not overwrite the evidence of
		// the first (last_result.json stays as the "latest run" pointer for simple clients).
		const FString TempPath = Dir / TEXT("last_result.tmp");
		const FString SessionPath = Dir / FString::Printf(TEXT("result_%s.json"), *SessionId);
		const FString LastPath = Dir / TEXT("last_result.json");
		if (!FFileHelper::SaveStringToFile(ResultJson, *TempPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			// Saved/ can be read-only or full; the HTTP path often goes dark around PIE
			// teardown, so a silent loss here would leave the caller with nothing.
			UE_LOG(LogBTForgeVerify, Warning, TEXT("could not write the verify result to %s (disk full or read-only Saved dir)"), *TempPath);
			return;
		}
		IFileManager& FileManager = IFileManager::Get();
		FileManager.Copy(*SessionPath, *TempPath, /*bReplace=*/true, /*bEvenIfReadOnly=*/true);
		FileManager.Move(*LastPath, *TempPath, /*bReplace=*/true, /*bEvenIfReadOnly=*/true);
	}

	/** Escape a string for embedding inside a JSON string literal.
	 *  Order matters: the backslash has to be escaped first, otherwise a value that
	 *  already contains \" gets its new escape sequence re-escaped and produces
	 *  invalid JSON (the old implementation did exactly that). */
	FString JsonEscape(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len() + 8);
		for (const TCHAR Char : In)
		{
			switch (Char)
			{
			case TEXT('\\'): Out += TEXT("\\\\"); break;
			case TEXT('"'):  Out += TEXT("\\\""); break;
			case TEXT('\n'): Out += TEXT("\\n"); break;
			case TEXT('\r'): Out += TEXT("\\r"); break;
			case TEXT('\t'): Out += TEXT("\\t"); break;
			default:
				if (Char < 0x20)
				{
					Out += FString::Printf(TEXT("\\u%04x"), static_cast<int32>(Char));
				}
				else
				{
					Out.AppendChar(Char);
				}
				break;
			}
		}
		return Out;
	}

	FString JsonStringArray(const TArray<FString>& Values)
	{
		TArray<FString> Quoted;
		Quoted.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Quoted.Add(FString::Printf(TEXT("\"%s\""), *JsonEscape(Value)));
		}
		return FString(TEXT("[")) + FString::Join(Quoted, TEXT(",")) + TEXT("]");
	}

	/** checks[] collector: the single source of truth for the top-level pass flag. */
	struct FBTForgeCheckList
	{
		TArray<FString> Entries;
		int32 Total = 0;
		int32 Passed = 0;
		TArray<FString> FailedNames;

		void Add(bool bPass, const FString& Name, const FString& Detail)
		{
			++Total;
			if (bPass)
			{
				++Passed;
			}
			else
			{
				FailedNames.Add(Name);
			}
			Entries.Add(FString::Printf(TEXT("{\"name\":\"%s\",\"pass\":%s,\"detail\":\"%s\"}"),
				*Name, bPass ? TEXT("true") : TEXT("false"), *JsonEscape(Detail)));
		}

		/** pass = every check passed (and there is at least one: an empty run proves nothing). */
		bool AllPass() const { return Total > 0 && Passed == Total; }
		FString Join() const { return FString::Join(Entries, TEXT(",")); }
		FString Summary() const
		{
			return FString::Printf(TEXT("%d/%d checks passed%s"), Passed, Total,
				FailedNames.Num() ? *FString::Printf(TEXT("; failed: %s"), *FString::Join(FailedNames, TEXT(","))) : TEXT(""));
		}
	};

	bool ReadVector3(const TSharedPtr<FJsonValue>& Value, FVector& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Value->TryGetArray(Array) && Array != nullptr && Array->Num() == 3)
		{
			Out = FVector((*Array)[0]->AsNumber(), (*Array)[1]->AsNumber(), (*Array)[2]->AsNumber());
			return true;
		}
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (Value->TryGetObject(Object) && Object != nullptr && Object->IsValid())
		{
			double X = 0.0, Y = 0.0, Z = 0.0;
			const bool bHasX = (*Object)->TryGetNumberField(TEXT("X"), X) || (*Object)->TryGetNumberField(TEXT("x"), X);
			if (bHasX)
			{
				(*Object)->TryGetNumberField(TEXT("Y"), Y);
				(*Object)->TryGetNumberField(TEXT("y"), Y);
				(*Object)->TryGetNumberField(TEXT("Z"), Z);
				(*Object)->TryGetNumberField(TEXT("z"), Z);
				Out = FVector(X, Y, Z);
				return true;
			}
		}
		return false;
	}

	/** Runtime actors of the current fixture, for the "$FixturePawn"/"$FixtureController"
	 *  blackboardInit tokens (actor keys cannot be set from an asset path). */
	struct FBTForgeFixtureActors
	{
		UObject* Pawn = nullptr;
		UObject* Controller = nullptr;
	};

	/** Type-directed blackboard write: the old code funnelled every value through
	 *  SetValueAsFloat/Bool/String, which silently no-ops on Int/Enum/Vector/Rotator/
	 *  Class keys. Every unsupported shape is now an explicit error instead. */
	bool WriteBlackboardValue(UBlackboardComponent& Blackboard, const FName KeyName,
		const UBlackboardKeyType* KeyType, const TSharedPtr<FJsonValue>& Value,
		const FBTForgeFixtureActors& FixtureActors, FString& OutError)
	{
		if (!Value.IsValid())
		{
			OutError = FString::Printf(TEXT("blackboardInit.%s has a null JSON value"), *KeyName.ToString());
			return false;
		}

		// Dispatch on the JSON *type*, never on TryGet* probing: FJsonValueString
		// overrides TryGetBool/TryGetNumber(int32..int64) to return true for ANY
		// string ("hello" becomes bool false + int 0), which is exactly how the
		// old value sniffing lost strings.
		auto AsNumber = [&Value](double& Out)
		{
			if (Value->Type == EJson::Number) { Out = Value->AsNumber(); return true; }
			if (Value->Type == EJson::String)
			{
				const FString& Raw = Value->AsString();
				if (FDefaultValueHelper::IsStringValidFloat(Raw)) { Out = FCString::Atod(*Raw); return true; }
			}
			return false;
		};
		auto AsBool = [&Value](bool& Out)
		{
			if (Value->Type == EJson::Boolean) { Out = Value->AsBool(); return true; }
			if (Value->Type == EJson::Number) { Out = !FMath::IsNearlyZero(Value->AsNumber()); return true; }
			if (Value->Type == EJson::String)
			{
				const FString& Raw = Value->AsString();
				if (Raw.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Raw == TEXT("1")) { Out = true; return true; }
				if (Raw.Equals(TEXT("false"), ESearchCase::IgnoreCase) || Raw == TEXT("0")) { Out = false; return true; }
			}
			return false;
		};
		auto AsText = [&Value](FString& Out)
		{
			if (Value->Type == EJson::String) { Out = Value->AsString(); return true; }
			return false;
		};

		auto Fail = [&](const TCHAR* Expected)
		{
			OutError = FString::Printf(TEXT("blackboardInit.%s expects %s (key type %s)"),
				*KeyName.ToString(), Expected, KeyType ? *KeyType->GetClass()->GetName() : TEXT("unknown"));
			return false;
		};

		if (KeyType == nullptr)
		{
			OutError = FString::Printf(TEXT("blackboardInit.%s: key has no key type"), *KeyName.ToString());
			return false;
		}

		double Number = 0.0;
		bool BoolValue = false;
		FString Text;
		const bool bIsNumber = AsNumber(Number);
		const bool bIsBool = AsBool(BoolValue);
		const bool bIsText = AsText(Text);

		if (KeyType->IsA<UBlackboardKeyType_Int>())
		{
			if (bIsNumber) { Blackboard.SetValueAsInt(KeyName, FMath::RoundToInt(Number)); return true; }
			return Fail(TEXT("a number"));
		}
		if (KeyType->IsA<UBlackboardKeyType_Float>())
		{
			if (bIsNumber) { Blackboard.SetValueAsFloat(KeyName, static_cast<float>(Number)); return true; }
			return Fail(TEXT("a number"));
		}
		if (KeyType->IsA<UBlackboardKeyType_Bool>())
		{
			if (bIsBool) { Blackboard.SetValueAsBool(KeyName, BoolValue); return true; }
			return Fail(TEXT("true/false"));
		}
		if (KeyType->IsA<UBlackboardKeyType_Enum>())
		{
			const UBlackboardKeyType_Enum* EnumKey = static_cast<const UBlackboardKeyType_Enum*>(KeyType);
			// EnumName (a C++ enum) takes priority over EnumType per the key type's own
			// contract (BlackboardKeyType_Enum.h), and in that case EnumType is null.
			const UEnum* EnumDef = EnumKey->EnumType;
			if (EnumDef == nullptr && EnumKey->bIsEnumNameValid && !EnumKey->EnumName.IsEmpty())
			{
				EnumDef = FindFirstObject<UEnum>(*EnumKey->EnumName, EFindFirstObjectOptions::NativeFirst);
			}
			if (bIsNumber)
			{
				const int64 Requested = FMath::RoundToInt(Number);
				if (EnumDef != nullptr && EnumDef->GetMaxEnumValue() > 0
					&& (Requested < 0 || Requested > EnumDef->GetMaxEnumValue()))
				{
					OutError = FString::Printf(TEXT("blackboardInit.%s: %lld is outside enum %s [0, %lld]"),
						*KeyName.ToString(), Requested, *EnumDef->GetName(), EnumDef->GetMaxEnumValue());
					return false;
				}
				Blackboard.SetValueAsEnum(KeyName, static_cast<uint8>(FMath::Clamp<int64>(Requested, 0, 255)));
				return true;
			}
			if (bIsText && EnumDef != nullptr)
			{
				int64 EnumValue = EnumDef->GetValueByName(FName(*Text));
				if (EnumValue == INDEX_NONE)
				{
					EnumValue = EnumDef->GetValueByName(FName(*(EnumDef->GetName() + TEXT("::") + Text)));
				}
				if (EnumValue != INDEX_NONE)
				{
					Blackboard.SetValueAsEnum(KeyName, static_cast<uint8>(EnumValue));
					return true;
				}
				OutError = FString::Printf(TEXT("blackboardInit.%s: '%s' is not a value of enum %s"),
					*KeyName.ToString(), *Text, *EnumDef->GetName());
				return false;
			}
			return Fail(TEXT("a number or an enum value name"));
		}
		if (KeyType->IsA<UBlackboardKeyType_String>())
		{
			if (bIsText) { Blackboard.SetValueAsString(KeyName, Text); return true; }
			return Fail(TEXT("a string"));
		}
		if (KeyType->IsA<UBlackboardKeyType_Name>())
		{
			if (bIsText) { Blackboard.SetValueAsName(KeyName, FName(*Text)); return true; }
			return Fail(TEXT("a string"));
		}
		if (KeyType->IsA<UBlackboardKeyType_Vector>())
		{
			FVector Vector;
			if (ReadVector3(Value, Vector)) { Blackboard.SetValueAsVector(KeyName, Vector); return true; }
			return Fail(TEXT("[x,y,z] or {\"X\":n,\"Y\":n,\"Z\":n}"));
		}
		if (KeyType->IsA<UBlackboardKeyType_Rotator>())
		{
			FVector Rotator;
			if (ReadVector3(Value, Rotator))
			{
				Blackboard.SetValueAsRotator(KeyName, FRotator(Rotator.X, Rotator.Y, Rotator.Z));
				return true;
			}
			return Fail(TEXT("[pitch,yaw,roll] or {\"X\":n,\"Y\":n,\"Z\":n}"));
		}
		if (KeyType->IsA<UBlackboardKeyType_Object>())
		{
			if (Value->Type == EJson::Null) { Blackboard.SetValueAsObject(KeyName, nullptr); return true; }
			if (!bIsText) { return Fail(TEXT("an object path string or null")); }
			if (Text.IsEmpty()) { Blackboard.SetValueAsObject(KeyName, nullptr); return true; }
			// Fixture actors only exist at runtime, so they cannot be named by an asset
			// path. These tokens let a test drive actor-keyed branches (e.g. a combat
			// branch gated on "TargetEnemy is set") without project wiring.
			if (Text.Equals(TEXT("$FixturePawn"), ESearchCase::IgnoreCase))
			{
				if (FixtureActors.Pawn == nullptr)
				{
					OutError = FString::Printf(TEXT("blackboardInit.%s: $FixturePawn needs fixturePawn=true"), *KeyName.ToString());
					return false;
				}
				Blackboard.SetValueAsObject(KeyName, FixtureActors.Pawn);
				return true;
			}
			if (Text.Equals(TEXT("$FixtureController"), ESearchCase::IgnoreCase))
			{
				if (FixtureActors.Controller == nullptr)
				{
					OutError = FString::Printf(TEXT("blackboardInit.%s: no fixture controller in this session"), *KeyName.ToString());
					return false;
				}
				Blackboard.SetValueAsObject(KeyName, FixtureActors.Controller);
				return true;
			}
			UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *Text);
			if (Object == nullptr)
			{
				OutError = FString::Printf(TEXT("blackboardInit.%s: could not load object '%s'"),
					*KeyName.ToString(), *Text);
				return false;
			}
			Blackboard.SetValueAsObject(KeyName, Object);
			return true;
		}
		if (KeyType->IsA<UBlackboardKeyType_Class>())
		{
			if (Value->Type == EJson::Null) { Blackboard.SetValueAsClass(KeyName, nullptr); return true; }
			if (!bIsText) { return Fail(TEXT("a class path string or null")); }
			UClass* Class = StaticLoadClass(UObject::StaticClass(), nullptr, *Text);
			if (Class == nullptr)
			{
				OutError = FString::Printf(TEXT("blackboardInit.%s: could not load class '%s'"),
					*KeyName.ToString(), *Text);
				return false;
			}
			Blackboard.SetValueAsClass(KeyName, Class);
			return true;
		}

		OutError = FString::Printf(TEXT("blackboardInit.%s: unsupported blackboard key type %s"),
			*KeyName.ToString(), *KeyType->GetClass()->GetName());
		return false;
	}

	class FBTForgeLogCapture : public FOutputDevice
	{
	public:
		/** GLog (FOutputDeviceRedirector) dispatches to output devices under a *read*
		 *  lock (OutputDeviceRedirector.cpp), so worker threads can call Serialize()
		 *  concurrently while PIE starts. Without this lock the TArray pushes race
		 *  (heap corruption), and the game-thread copy could read a torn array and
		 *  drop a real error - which would make no_log_errors pass falsely. */
		FCriticalSection Mutex;
		TArray<FString> Errors;
		TArray<FString> Warnings;

		void Reset()
		{
			FScopeLock Lock(&Mutex);
			Errors.Reset();
			Warnings.Reset();
		}

		void CopyTo(TArray<FString>& OutErrors, TArray<FString>& OutWarnings)
		{
			FScopeLock Lock(&Mutex);
			OutErrors = Errors;
			OutWarnings = Warnings;
		}

		void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			// Known editor noise around in-process PIE (not tree-related).
			if (Category.ToString().StartsWith(TEXT("LogPixelStreaming")))
			{
				return;
			}
			if (Verbosity == ELogVerbosity::Error)
			{
				FScopeLock Lock(&Mutex);
				if (Errors.Num() < 20)
				{
					Errors.Add(FString::Printf(TEXT("[%s] %s"), *Category.ToString(), V));
				}
				else if (Errors.Num() == 20)
				{
					Errors.Add(TEXT("(more errors truncated)"));
				}
			}
			else if (Verbosity == ELogVerbosity::Warning && Warnings.Num() < 1000)
			{
				FScopeLock Lock(&Mutex);
				// Warnings are reported, not failed on: unrelated editor noise
				// (network probes, DDC) would make the check flaky.
				if (Warnings.Num() < 20)
				{
					Warnings.Add(FString::Printf(TEXT("[%s] %s"), *Category.ToString(), V));
				}
			}
		}
	};

	/** One capture instance for the process: it is registered/unregistered per session. */
	FBTForgeLogCapture& GetLogCapture()
	{
		static FBTForgeLogCapture Capture;
		return Capture;
	}

	/** Object-valued verify options: a real JSON object or a JSON string holding one.
	 *  Anything else is an error - silently ignoring it would verify against the
	 *  default values while the caller believes the option was applied. */
	bool TryReadObjectOption(const TSharedPtr<FJsonObject>& Root, const TCHAR* Field,
		TSharedPtr<FJsonObject>& Out, FString& OutError)
	{
		Out.Reset();
		if (!Root.IsValid() || !Root->HasField(Field))
		{
			return true;
		}
		const TSharedPtr<FJsonValue>* Value = Root->Values.Find(Field);
		if (Value == nullptr || !Value->IsValid())
		{
			OutError = FString::Printf(TEXT("%s: expected an object or a JSON string"), Field);
			return false;
		}
		FString Text;
		if ((*Value)->TryGetString(Text))
		{
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
			if (!FJsonSerializer::Deserialize(Reader, Out) || !Out.IsValid())
			{
				OutError = FString::Printf(TEXT("%s: string payload is not a JSON object"), Field);
				return false;
			}
			return true;
		}
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Root->TryGetObjectField(Field, Object) || Object == nullptr || !Object->IsValid())
		{
			OutError = FString::Printf(TEXT("%s: expected an object or a JSON string"), Field);
			return false;
		}
		Out = *Object;
		return true;
	}
}

// ---------------------------------------------------------------- options

bool FBTForgeVerifyOptions::Parse(const FString& OptionsJson, FString& OutError)
{
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(OptionsJson);
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = TEXT("OptionsJson is not valid JSON");
		return false;
	}

	Kind = Root->HasField(TEXT("kind")) ? Root->GetStringField(TEXT("kind")) : FString();
	if (Kind != TEXT("bt") && Kind != TEXT("eqs"))
	{
		OutError = TEXT("kind must be \"bt\" or \"eqs\"");
		return false;
	}
	AssetPath = Root->HasField(TEXT("assetPath")) ? Root->GetStringField(TEXT("assetPath")) : FString();
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("assetPath is required");
		return false;
	}
	MapPath = Root->HasField(TEXT("mapPath")) ? Root->GetStringField(TEXT("mapPath")) : FString();
	Mode = Root->HasField(TEXT("mode")) ? Root->GetStringField(TEXT("mode")) : FString();
	if (Mode.IsEmpty())
	{
		Mode = TEXT("SingleRun");
	}
	if (Kind == TEXT("bt") && Mode != TEXT("SingleRun") && Mode != TEXT("Looped"))
	{
		OutError = TEXT("mode must be \"SingleRun\" or \"Looped\"");
		return false;
	}
	// Explicit-but-invalid numbers are an error, not "use the default": silently
	// widening toleranceSec:0 (or disabling expectedDurationSec:0) would report pass
	// against a weaker contract than the caller asked for.
	double Number = 0.0;
	if (Root->HasField(TEXT("durationSec")))
	{
		if (!Root->TryGetNumberField(TEXT("durationSec"), Number) || Number <= 0.0)
		{
			OutError = TEXT("durationSec must be a number > 0");
			return false;
		}
		DurationSec = Number;
	}
	if (Root->HasField(TEXT("startDeadlineSec")))
	{
		if (!Root->TryGetNumberField(TEXT("startDeadlineSec"), Number) || Number <= 0.0)
		{
			OutError = TEXT("startDeadlineSec must be a number > 0");
			return false;
		}
		StartDeadlineSec = Number;
	}
	if (Root->HasField(TEXT("toleranceSec")))
	{
		if (!Root->TryGetNumberField(TEXT("toleranceSec"), Number) || Number < 0.0)
		{
			OutError = TEXT("toleranceSec must be a number >= 0");
			return false;
		}
		ToleranceSec = Number;
	}
	if (Root->HasField(TEXT("expectedDurationSec")))
	{
		if (!Root->TryGetNumberField(TEXT("expectedDurationSec"), Number) || Number < 0.0)
		{
			OutError = TEXT("expectedDurationSec must be a number >= 0");
			return false;
		}
		ExpectedDurationSec = Number;
	}
	if (Root->HasField(TEXT("expectedMinPawnDisplacement")))
	{
		if (!Root->TryGetNumberField(TEXT("expectedMinPawnDisplacement"), Number) || Number < 0.0)
		{
			OutError = TEXT("expectedMinPawnDisplacement must be a number >= 0");
			return false;
		}
		MinPawnDisplacement = Number;
	}
	ExpectedActiveTask = Root->HasField(TEXT("expectedActiveTask")) ? Root->GetStringField(TEXT("expectedActiveTask")) : FString();
	// Object-valued options accept a real object or a JSON string, and reject anything
	// else: the MCP layer passes user JSON through as text, but a caller typing
	// "blackboardInit": "x" must not silently verify against the default key values.
	if (!TryReadObjectOption(Root, TEXT("blackboardInit"), BlackboardInit, OutError))
	{
		return false;
	}
	QueryRunMode = Root->HasField(TEXT("queryRunMode")) ? Root->GetStringField(TEXT("queryRunMode")) : FString();
	BareGameMode = true;
	if (Root->HasField(TEXT("bareGameMode")))
	{
		Root->TryGetBoolField(TEXT("bareGameMode"), BareGameMode);
	}
	GameModeClassPath = Root->HasField(TEXT("gameModeClassPath")) ? Root->GetStringField(TEXT("gameModeClassPath")) : FString();
	FixturePawn = true;
	if (Root->HasField(TEXT("fixturePawn")))
	{
		Root->TryGetBoolField(TEXT("fixturePawn"), FixturePawn);
	}
	if (!TryReadObjectOption(Root, TEXT("queryParams"), QueryParams, OutError))
	{
		return false;
	}
	return true;
}

// ---------------------------------------------------------------- manager

FBTForgeVerifyManager& FBTForgeVerifyManager::Get()
{
	static FBTForgeVerifyManager Singleton;
	return Singleton;
}

FString FBTForgeVerifyManager::StartVerify(const FString& OptionsJson)
{
	if (Session.State == FBTForgeVerifySession::EState::RequestingPIE || Session.State == FBTForgeVerifySession::EState::Running)
	{
		return TEXT("{\"ok\":false,\"error\":\"another verify session is in flight; poll get_verify_result or wait for it to finish\"}");
	}
	if (GUnrealEd == nullptr)
	{
		return TEXT("{\"ok\":false,\"error\":\"no editor engine (verify needs the editor)\"}");
	}
	// Never hijack a PIE/SIE session the user (or another tool) started: the result
	// would describe their world, and our teardown would kill their session.
	if (GUnrealEd->IsPlaySessionInProgress())
	{
		// Our own teardown is only *queued* at this point (RequestEndPlayMap sets a
		// flag that the next editor tick consumes), so a client that starts the next
		// session right after reading "done" would otherwise be told a foreign PIE is
		// running. Distinguish the two cases so callers can retry instead of giving up.
		const bool bOwnTeardown =
			(Session.State == FBTForgeVerifySession::EState::Done || Session.State == FBTForgeVerifySession::EState::Failed)
			&& GUnrealEd->ShouldEndPlayMap();
		if (bOwnTeardown)
		{
			return TEXT("{\"ok\":false,\"error\":\"teardown_pending\",\"retry\":true,\"hint\":\"the previous verify session is still tearing its PIE session down; retry in a few frames\"}");
		}
		// A PIE request that failed to create its world (bad mapPath, asset errors)
		// leaves the editor claiming a session with no PIE world. That state would
		// otherwise block every future verify until the user presses Stop: clear the
		// queued teardown and let the caller retry.
		if (FindPIEWorld() == nullptr)
		{
			GUnrealEd->RequestEndPlayMap();
			return TEXT("{\"ok\":false,\"error\":\"stale_pie_state\",\"retry\":true,\"hint\":\"the editor reports a PIE session but no PIE world exists (a failed start); the stale session was asked to end, retry shortly\"}");
		}
		return TEXT("{\"ok\":false,\"error\":\"a PIE/SIE session is already running; stop it before starting a verify session (BTForge only stops sessions it started itself)\"}");
	}

	FString ParseError;
	// Parse into a fresh options struct: reusing the previous parse would leak
	// fields that the new request omits (e.g. a stale blackboardInit).
	Session.Options = FBTForgeVerifyOptions();
	if (!Session.Options.Parse(OptionsJson, ParseError))
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *JsonEscape(ParseError));
	}

	UObject* Asset = StaticLoadObject(Session.Options.Kind == TEXT("bt") ? UBehaviorTree::StaticClass() : UEnvQuery::StaticClass(), nullptr, *Session.Options.AssetPath);
	if (Asset == nullptr)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"asset not found: %s\"}"), *JsonEscape(Session.Options.AssetPath));
	}

	// Map choice: a bare AGameModeBase on a Lyra map makes the map's Experience
	// actors check(ExperienceComponent) and crash the editor, so bare mode never
	// falls back to "whatever map is open".
	Session.Options.ResolvedMapPath = Session.Options.MapPath;
	Session.Options.bMapAutoDefaulted = false;
	if (Session.Options.ResolvedMapPath.IsEmpty() && Session.Options.BareGameMode)
	{
		Session.Options.ResolvedMapPath = BareGameModeFallbackMap;
		Session.Options.bMapAutoDefaulted = true;
	}
	// Validate the map before handing it to PIE: a bad path used to fail inside
	// world creation, which leaves the editor claiming a live PIE session with no
	// world and blocks every later verify.
	if (!Session.Options.ResolvedMapPath.IsEmpty())
	{
		const FString MapPackage = FPackageName::ObjectPathToPackageName(Session.Options.ResolvedMapPath);
		if (!FPackageName::DoesPackageExist(MapPackage))
		{
			return FString::Printf(TEXT("{\"ok\":false,\"error\":\"mapPath not found: %s\"}"),
				*JsonEscape(Session.Options.ResolvedMapPath));
		}
	}

	// Reset session state for this run.
	RemovePIEDelegates();
	StopTicker();
	// Ids must never repeat within a process: FPlatformTime::Cycles() is a uint32
	// cycle counter, so `Cycles() & 0x7FFFFFF` wraps every ~0.7 s at 3 GHz - and a
	// repeated id lets a client's on-disk poll match a *previous* result file.
	static uint32 GSessionSequence = 0;
	++GSessionSequence;
	Session.SessionId = FString::Printf(TEXT("V%u_%u"), GSessionSequence, static_cast<uint32>(FPlatformTime::Cycles() & 0x7FFFFFFF));
	Session.State = FBTForgeVerifySession::EState::RequestingPIE;
	Session.Error.Reset();
	Session.bPIERequested = false;
	Session.RequestTime = FPlatformTime::Seconds();
	Session.PIEWorld = nullptr;
	Session.FixtureController = nullptr;
	Session.FixturePawn = nullptr;
	Session.BTComp = nullptr;
	Session.BBComp = nullptr;
	Session.Samples.Reset();
	Session.ActiveTasksSeen.Reset();
	Session.LogErrors.Reset();
	Session.LogWarnings.Reset();
	Session.ResultJson.Reset();
	Session.bSawRunning = false;
	Session.MaxTickDelta = 0.0;
	Session.VerifyDeadline = Session.RequestTime + Session.Options.StartDeadlineSec;

	CaptureLog(true);

	// Subscribe before requesting so we cannot miss the transition.
	SubscribePIEDelegates();

	FRequestPlaySessionParams Params;
	Params.SessionDestination = EPlaySessionDestinationType::InProcess;
	Params.WorldType = EPlaySessionWorldType::PlayInEditor;
	// Pin the play settings: a null EditorPlaySettings makes RequestPlaySession
	// duplicate the project CDO, so a "Multiplayer: 2 clients / separate server /
	// new window" preference would silently produce several PIE worlds and
	// FindPIEWorld could pick the server world for a client-side fixture.
	if (ULevelEditorPlaySettings* Pinned = DuplicateObject(GetMutableDefault<ULevelEditorPlaySettings>(), GetTransientPackage()))
	{
		// PlayNetMode/PlayNumberOfClients/RunUnderOneProcess are private: use the
		// accessors the settings panel itself uses (same API on 5.5 and 5.8).
		Pinned->SetPlayNetMode(EPlayNetMode::PIE_Standalone);
		Pinned->bLaunchSeparateServer = false;
		Pinned->SetPlayNumberOfClients(1);
		Pinned->SetRunUnderOneProcess(true);
		Params.EditorPlaySettings = Pinned;
	}
	// bareGameMode (default true): override with a bare AGameModeBase so project
	// GameMode/Experience machinery does not slow PIE down or require its state.
	// Maps whose actors depend on the project GameMode must run with
	// bareGameMode=false (their normal GameMode then loads normally).
	if (Session.Options.BareGameMode)
	{
		Params.GameModeOverride = AGameModeBase::StaticClass();
	}
	else if (!Session.Options.GameModeClassPath.IsEmpty())
	{
		UClass* GmClass = StaticLoadClass(AGameModeBase::StaticClass(), nullptr, *Session.Options.GameModeClassPath);
		if (GmClass == nullptr)
		{
			// Silently falling back to the map's GameMode would verify a different
			// game mode than the caller pinned. Also leave the manager startable:
			// State was already moved to RequestingPIE above.
			StopTicker();
			RemovePIEDelegates();
			CaptureLog(false);
			Session.State = FBTForgeVerifySession::EState::Failed;
			Session.Error = FString::Printf(TEXT("gameModeClassPath is not a loadable AGameModeBase: %s"),
				*Session.Options.GameModeClassPath);
			return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *JsonEscape(Session.Error));
		}
		Params.GameModeOverride = GmClass;
	}
	if (!Session.Options.ResolvedMapPath.IsEmpty())
	{
		Params.GlobalMapOverride = Session.Options.ResolvedMapPath;
	}
	GUnrealEd->RequestPlaySession(Params);
	Session.bPIERequested = true;

	Session.TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FBTForgeVerifyManager::Tick), 0.1f);

	return FString::Printf(TEXT("{\"ok\":true,\"sessionId\":\"%s\",\"status\":\"pending\",\"mapPath\":\"%s\",\"mapPathAutoDefaulted\":%s}"),
		*Session.SessionId, *JsonEscape(Session.Options.ResolvedMapPath),
		Session.Options.bMapAutoDefaulted ? TEXT("true") : TEXT("false"));
}

FString FBTForgeVerifyManager::GetVerifyResult(const FString& SessionId)
{
	if (Session.SessionId != SessionId || Session.State == FBTForgeVerifySession::EState::Idle)
	{
		return TEXT("{\"ok\":false,\"error\":\"unknown verify session\"}");
	}
	if (Session.State == FBTForgeVerifySession::EState::Done || Session.State == FBTForgeVerifySession::EState::Failed)
	{
		if (!Session.ResultJson.IsEmpty())
		{
			return Session.ResultJson;
		}
		return FString::Printf(TEXT("{\"ok\":true,\"sessionId\":\"%s\",\"status\":\"failed\",\"pass\":false,\"error\":\"session finished without a result payload\"}"),
			*JsonEscape(SessionId));
	}
	const FString Status = Session.State == FBTForgeVerifySession::EState::Running ? TEXT("running") : TEXT("pending");
	return FString::Printf(TEXT("{\"ok\":true,\"sessionId\":\"%s\",\"status\":\"%s\"}"), *SessionId, *Status);
}

void FBTForgeVerifyManager::Shutdown()
{
	if (GUnrealEd && Session.State == FBTForgeVerifySession::EState::RequestingPIE)
	{
		// The request may still be queued (PIE has not started yet): cancellation is
		// the only thing that stops it, otherwise the session starts after the module
		// is gone and its fixture is never created.
		if (GUnrealEd->IsPlaySessionRequestQueued())
		{
			GUnrealEd->CancelRequestPlaySession();
		}
		CaptureLog(false);
	}
	else if (Session.State == FBTForgeVerifySession::EState::Running)
	{
		CaptureLog(false);
		if (GUnrealEd && GUnrealEd->IsPlayingSessionInEditor())
		{
			GUnrealEd->RequestEndPlayMap();
		}
	}
	RemovePIEDelegates();
	StopTicker();
	Session = FBTForgeVerifySession();
}

void FBTForgeVerifyManager::SubscribePIEDelegates()
{
	RemovePIEDelegates();
	Session.PIEStartedHandle = FEditorDelegates::PostPIEStarted.AddStatic(&FBTForgeVerifyManager::HandlePostPIEStarted);
	Session.PIEEndHandle = FEditorDelegates::EndPIE.AddStatic(&FBTForgeVerifyManager::HandlePIEEnded);
}

void FBTForgeVerifyManager::SubscribeTreeStarted()
{
	RemoveTreeStarted();
	Session.TreeStartedHandle = FBehaviorTreeDelegates::OnTreeStarted.AddStatic(&FBTForgeVerifyManager::HandleTreeStarted);
}

void FBTForgeVerifyManager::RemoveTreeStarted()
{
	if (Session.TreeStartedHandle.IsValid())
	{
		FBehaviorTreeDelegates::OnTreeStarted.Remove(Session.TreeStartedHandle);
		Session.TreeStartedHandle.Reset();
	}
}

void FBTForgeVerifyManager::RemovePIEDelegates()
{
	if (Session.PIEStartedHandle.IsValid())
	{
		FEditorDelegates::PostPIEStarted.Remove(Session.PIEStartedHandle);
		Session.PIEStartedHandle.Reset();
	}
	if (Session.PIEEndHandle.IsValid())
	{
		FEditorDelegates::EndPIE.Remove(Session.PIEEndHandle);
		Session.PIEEndHandle.Reset();
	}
	RemoveTreeStarted();
}

void FBTForgeVerifyManager::StopTicker()
{
	if (Session.TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(Session.TickHandle);
		Session.TickHandle.Reset();
	}
}

UWorld* FBTForgeVerifyManager::FindPIEWorld() const
{
	if (GEngine == nullptr)
	{
		return nullptr;
	}
	// Prefer the local standalone/play world: a project Play setting with a separate
	// server would otherwise let the first PIE world be a dedicated server, which has
	// no local player, no fixture target and no navmesh for client-side EQS.
	UWorld* Fallback = nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UWorld* World = Context.World();
		if (World == nullptr || !World->IsPlayInEditor())
		{
			continue;
		}
		if (World->GetNetMode() != NM_DedicatedServer)
		{
			return World;
		}
		if (Fallback == nullptr)
		{
			Fallback = World;
		}
	}
	return Fallback;
}

void FBTForgeVerifyManager::HandleTreeStarted(const UBehaviorTreeComponent& Component, const UBehaviorTree& TreeAsset)
{
	FBTForgeVerifyManager& M = Get();
	if (M.Session.State != FBTForgeVerifySession::EState::Running)
	{
		return;
	}
	if (M.Session.BTComp.Get() == &Component)
	{
		M.Session.bTreeStarted = true;
		M.Session.bSawRunning = true;
	}
}

void FBTForgeVerifyManager::CaptureLog(bool bEnable)
{
	FBTForgeLogCapture& Capture = GetLogCapture();
	if (bEnable)
	{
		Capture.Reset();
		GLog->AddOutputDevice(&Capture);
		Session.LogCapture = &Capture;
	}
	else if (Session.LogCapture)
	{
		GLog->RemoveOutputDevice(Session.LogCapture);
		static_cast<FBTForgeLogCapture*>(Session.LogCapture)->CopyTo(Session.LogErrors, Session.LogWarnings);
		Session.LogCapture = nullptr;
	}
}

void FBTForgeVerifyManager::HandlePostPIEStarted(bool bIsSimulatingInEditor)
{
	FBTForgeVerifyManager& M = Get();
	if (M.Session.State != FBTForgeVerifySession::EState::RequestingPIE)
	{
		return;
	}
	// This bool is Editor.h's FOnPIEEvent payload: bIsSimulatingInEditor
	// (PlayLevel.cpp broadcasts `WorldType == SimulateInEditor`). It says nothing
	// about success - treating it as a failure flag made every Simulate session
	// report "PIE failed to start". Failure detection lives in Tick, which polls
	// !GUnrealEd->IsPlaySessionInProgress().
	if (bIsSimulatingInEditor)
	{
		UE_LOG(LogBTForgeVerify, Verbose, TEXT("PostPIEStarted: Simulate In Editor session (not Play In Editor)"));
	}
	M.OnPIEReady();
}

void FBTForgeVerifyManager::HandlePIEEnded(bool bWasSimulating)
{
	FBTForgeVerifyManager& M = Get();
	if (M.Session.State != FBTForgeVerifySession::EState::Running && M.Session.State != FBTForgeVerifySession::EState::RequestingPIE)
	{
		return;
	}
	// PIE ended under us (user pressed stop, or map change): close the session
	// as failed - the verification was interrupted, results are meaningless.
	M.CaptureLog(false);
	M.StopTicker();
	M.RemovePIEDelegates();
	M.Session.Error = bWasSimulating
		? TEXT("Simulate session ended before verification completed")
		: TEXT("PIE session ended before verification completed");
	M.Session.State = FBTForgeVerifySession::EState::Failed;
	FHttpServerModule::Get().StartAllListeners();
	M.Session.ResultJson = FString::Printf(
		TEXT("{\"ok\":true,\"sessionId\":\"%s\",\"status\":\"failed\",\"kind\":\"%s\",\"pass\":false,\"error\":\"%s\",\"checks\":[],\"logErrors\":%s}"),
		*M.Session.SessionId, *M.Session.Options.Kind, *JsonEscape(M.Session.Error),
		*JsonStringArray(M.Session.LogErrors));
	SaveResultToDisk(M.Session.SessionId, M.Session.ResultJson);
}

void FBTForgeVerifyManager::OnPIEReady()
{
	UWorld* World = FindPIEWorld();
	if (World == nullptr)
	{
		AbortToIdle(TEXT("PIE started but no PIE world found"));
		return;
	}
	Session.PIEWorld = World;
	Session.PIEStartTime = FPlatformTime::Seconds();

	// Fixture: an AIController is required by both paths (BT tree owner and
	// EQS querier - the Querier context resolves to this controller);
	// the BT path also gets a Pawn so movement/behavior tasks are not starved.
	FActorSpawnParameters SP;
	SP.ObjectFlags |= RF_Transient;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AAIController* Controller = World->SpawnActor<AAIController>(AAIController::StaticClass(), FTransform::Identity, SP);
	if (Controller == nullptr)
	{
		AbortToIdle(TEXT("failed to spawn AIController fixture in PIE world"));
		return;
	}
	Session.FixtureController = Controller;

	if (Session.Options.Kind == TEXT("eqs"))
	{
		RunEQSVerification();
		return;
	}

	if (Session.Options.FixturePawn)
	{
		// Deferred spawn so AutoPossessAI cannot hand the pawn to a controller of
		// its own before we possess it ourselves.
		ADefaultPawn* Pawn = World->SpawnActorDeferred<ADefaultPawn>(ADefaultPawn::StaticClass(),
			FTransform(FRotator::ZeroRotator, FVector(0.0, 0.0, 100.0)), nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Pawn != nullptr)
		{
			Pawn->AutoPossessAI = EAutoPossessAI::Disabled;
			Pawn->FinishSpawning(FTransform(FRotator::ZeroRotator, FVector(0.0, 0.0, 100.0)));
		}
		if (Pawn == nullptr)
		{
			AbortToIdle(TEXT("failed to spawn the fixture Pawn in the PIE world"));
			return;
		}
		Controller->Possess(Pawn);
		if (Pawn->GetController() != Controller)
		{
			AbortToIdle(TEXT("failed to possess the fixture Pawn with the fixture AIController"));
			return;
		}
		Session.FixturePawn = Pawn;
	}

	StartBTVerification();
}

void FBTForgeVerifyManager::StartBTVerification()
{
	UWorld* World = Session.PIEWorld.Get();
	UBehaviorTree* Tree = LoadObject<UBehaviorTree>(nullptr, *Session.Options.AssetPath);
	if (World == nullptr || Tree == nullptr)
	{
		AbortToIdle(TEXT("BT asset disappeared before PIE start"));
		return;
	}

	AAIController* Controller = Session.FixtureController.Get();
	if (Controller == nullptr)
	{
		AbortToIdle(TEXT("fixture controller disappeared"));
		return;
	}

	// Possession re-initializes an existing blackboard, so the blackboard is
	// created/initialized only after the fixture pawn is in place.
	UBlackboardComponent* Blackboard = Controller->GetBlackboardComponent();
	if (Tree->BlackboardAsset != nullptr)
	{
		if (!Controller->UseBlackboard(Tree->BlackboardAsset, Blackboard))
		{
			AbortToIdle(TEXT("UseBlackboard failed for the tree's blackboard asset"));
			return;
		}
		// AAIController::InitializeBlackboard fills SelfActor from GetPawn(); when the
		// fixture pawn is not possessed yet that is null and nothing refreshes it, so
		// every SelfActor-based gate (DistanceCheck/UpdateDistance) failed closed in the
		// harness while the same tree worked in game. Set it explicitly.
		if (Blackboard != nullptr && Session.FixturePawn.IsValid()
			&& Blackboard->GetBlackboardAsset() != nullptr
			&& Blackboard->GetBlackboardAsset()->GetKeyID(FBlackboard::KeySelf) != FBlackboard::InvalidKey)
		{
			Blackboard->SetValueAsObject(FBlackboard::KeySelf, Session.FixturePawn.Get());
			UE_LOG(LogBTForgeVerify, Warning, TEXT("SelfActor seeded: pawn=%s blackboard=%s"),
				*GetNameSafe(Session.FixturePawn.Get()), *GetNameSafe(Blackboard));
		}
		else
		{
			UE_LOG(LogBTForgeVerify, Warning, TEXT("SelfActor NOT seeded: blackboard=%s pawnValid=%d bbAsset=%s"),
				*GetNameSafe(Blackboard), Session.FixturePawn.IsValid() ? 1 : 0,
				*GetNameSafe(Blackboard ? Blackboard->GetBlackboardAsset() : nullptr));
		}
	}
	Session.BBComp = Blackboard;

	// Optional key initialization before the tree starts. A requested init with no
	// blackboard at all (tree has no BlackboardAsset) must fail loudly: skipping it
	// would run the tree against uninitialized keys and still report pass.
	if (Session.Options.BlackboardInit.IsValid())
	{
		if (Blackboard == nullptr)
		{
			AbortToIdle(TEXT("blackboardInit was requested but the tree has no blackboard asset to write to"));
			return;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Session.Options.BlackboardInit->Values)
		{
			const FName KeyName(*Pair.Key);
			const FBlackboard::FKey KeyId = Blackboard->GetKeyID(KeyName);
			if (KeyId == FBlackboard::InvalidKey)
			{
				AbortToIdle(FString::Printf(TEXT("blackboardInit references unknown key '%s'"), *Pair.Key));
				return;
			}
			const UBlackboardKeyType* KeyType = Blackboard->GetKeyType(KeyId).GetDefaultObject();
			FBTForgeFixtureActors FixtureActors;
			FixtureActors.Pawn = Session.FixturePawn.Get();
			FixtureActors.Controller = Session.FixtureController.Get();
			FString WriteError;
			if (!WriteBlackboardValue(*Blackboard, KeyName, KeyType, Pair.Value, FixtureActors, WriteError))
			{
				AbortToIdle(WriteError);
				return;
			}
		}
	}

	UBehaviorTreeComponent* BTComponent = NewObject<UBehaviorTreeComponent>(Controller);
	BTComponent->RegisterComponent();
	// A real AI controller owns its brain component; nodes and services that go
	// through GetAIOwner()->GetBrainComponent() would otherwise see nullptr.
	Controller->BrainComponent = BTComponent;
	Session.BTComp = BTComponent;

	const EBTExecutionMode::Type ExecMode = Session.Options.Mode == TEXT("Looped")
		? EBTExecutionMode::Looped
		: EBTExecutionMode::SingleRun;

	Session.State = FBTForgeVerifySession::EState::Running;
	Session.TreeStartTime = FPlatformTime::Seconds();
	Session.VerifyDeadline = Session.TreeStartTime + Session.Options.DurationSec + 5.0;
	Session.bTreeStarted = false;
	Session.bSawRunning = false;
	// M4 golden path evidence: movement tasks can only be judged by where the
	// fixture pawn ended up relative to where the tree started it.
	if (const APawn* Pawn = Session.FixturePawn.Get())
	{
		Session.PawnStartLocation = Pawn->GetActorLocation();
		Session.bPawnStartCaptured = true;
	}

	// OnTreeStarted is the authoritative "started" signal: StartTree executes the
	// first task synchronously, so a tree that finishes immediately already reports
	// IsRunning()==false when StartTree returns (and PushInstance failures leave
	// IsRunning() true). Subscribe right before starting so the broadcast is caught.
	SubscribeTreeStarted();
	BTComponent->StartTree(*Tree, ExecMode);

	const bool bPushed = BTComponent->GetRootTree() == Tree;
	if (!bPushed && !Session.bTreeStarted)
	{
		AbortToIdle(TEXT("StartTree did not push the tree (PushInstance failed: blackboard or tree asset mismatch)"));
		return;
	}
	Session.bSawRunning = Session.bSawRunning || BTComponent->IsRunning() || Session.bTreeStarted;
	RecordSample(0.0);
}

void FBTForgeVerifyManager::RecordSample(double Elapsed)
{
	UBehaviorTreeComponent* BTComponent = Session.BTComp.Get();
	if (BTComponent == nullptr)
	{
		return;
	}
	const bool bRunning = BTComponent->IsRunning();
	const FString ActiveTasks = BTComponent->DescribeActiveTasks();
	if (bRunning)
	{
		Session.bSawRunning = true;
	}
	Session.ActiveTasksSeen.Add(ActiveTasks);
	if (Session.Samples.Num() < 400)
	{
		Session.Samples.Add(FString::Printf(TEXT("{\"t\":%.2f,\"running\":%s,\"tasks\":\"%s\"}"),
			Elapsed, bRunning ? TEXT("true") : TEXT("false"), *JsonEscape(ActiveTasks)));
	}
}

void FBTForgeVerifyManager::RunEQSVerification()
{
	UWorld* World = Session.PIEWorld.Get();
	UEnvQuery* Query = LoadObject<UEnvQuery>(nullptr, *Session.Options.AssetPath);
	if (World == nullptr || Query == nullptr)
	{
		AbortToIdle(TEXT("EQS asset disappeared before PIE start"));
		return;
	}

	Session.State = FBTForgeVerifySession::EState::Running;
	Session.TreeStartTime = FPlatformTime::Seconds();

	AAIController* Owner = Session.FixtureController.Get();
	FEnvQueryRequest Request(Query, Owner);
	Request.SetWorldOverride(World);

	if (Session.Options.QueryParams.IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Session.Options.QueryParams->Values)
		{
			double Num = 0.0;
			if (Pair.Value->TryGetNumber(Num))
			{
				Request.SetFloatParam(FName(*Pair.Key), static_cast<float>(Num));
			}
		}
	}

	EEnvQueryRunMode::Type RunMode = EEnvQueryRunMode::AllMatching;
	FString RunModeName = Session.Options.QueryRunMode;
	if (!RunModeName.IsEmpty())
	{
		if (const UEnum* EnumDef = StaticEnum<EEnvQueryRunMode::Type>())
		{
			int64 Value = EnumDef->GetValueByName(FName(*RunModeName));
			if (Value == INDEX_NONE)
			{
				Value = EnumDef->GetValueByName(FName(*(FString("EEnvQueryRunMode::") + RunModeName)));
			}
			if (Value != INDEX_NONE)
			{
				RunMode = static_cast<EEnvQueryRunMode::Type>(Value);
			}
			else
			{
				AbortToIdle(FString::Printf(TEXT("unknown queryRunMode '%s'"), *RunModeName));
				return;
			}
		}
	}

	UEnvQueryManager* Manager = UEnvQueryManager::GetCurrent(World);
	if (Manager == nullptr)
	{
		AbortToIdle(TEXT("no EnvQueryManager in the PIE world"));
		return;
	}
	// Same three-step path as UEnvQueryManager::RunInstantQuery (Prepare -> Register
	// -> step loop -> Unregister), the engine's own runtime-testing reference.
	const TSharedPtr<FEnvQueryInstance> Instance = Manager->PrepareQueryInstance(Request, RunMode);
	int32 Steps = 0;
	if (Instance.IsValid())
	{
		Manager->RegisterExternalQuery(Instance);
		while (Instance->IsFinished() == false && Steps < 1000)
		{
			Instance->ExecuteOneStep(UEnvQueryTypes::UnlimitedStepTime);
			Steps++;
		}
		Manager->UnregisterExternalQuery(Instance);
	}
	const double Elapsed = FPlatformTime::Seconds() - Session.TreeStartTime;
	const FString RunModeLabel = StaticEnum<EEnvQueryRunMode::Type>()
		? StaticEnum<EEnvQueryRunMode::Type>()->GetNameStringByValue(static_cast<int64>(RunMode))
		: FString::FromInt(static_cast<int32>(RunMode));

	if (!Instance.IsValid())
	{
		FailEQS(TEXT("PrepareQueryInstance returned null (query template or world is invalid)"), Steps, RunModeLabel, Elapsed);
		return;
	}

	FBTForgeCheckList Checks;
	// Completion, not "we ran a loop": the step cap or an aborted instance leaves
	// items behind (NumValidItems > 0 after FinalizeGeneration) while the query never
	// finished - reporting that as done would be exactly the false green we fixed.
	const bool bFinished = Instance->IsFinished();
	const bool bSuccessful = Instance->IsSuccessful();
	const bool bStepCapped = !bFinished && Steps >= 1000;
	Checks.Add(bFinished && bSuccessful, TEXT("query_ran"),
		FString::Printf(TEXT("steps=%d, finished=%s, successful=%s, status=%d, runMode=%s%s"),
			Steps, bFinished ? TEXT("true") : TEXT("false"), bSuccessful ? TEXT("true") : TEXT("false"),
			static_cast<int32>(Instance->GetRawStatus()), *RunModeLabel,
			bStepCapped ? TEXT(" (step cap 1000 hit)") : TEXT("")));

	// NumValidItems, not Items.Num(): tests that filter/abort leave discarded
	// entries in Items, so Items.Num() over-reports (and a SingleResult query
	// trims Items to 1 while the valid set differs).
	const int32 ValidItems = Instance->NumValidItems;
	const int32 RawItems = Instance->Items.Num();
	Checks.Add(ValidItems > 0, TEXT("has_results"),
		ValidItems > 0
			? FString::Printf(TEXT("%d valid items (raw %d)"), ValidItems, RawItems)
			: FString::Printf(TEXT("0 valid items (raw %d) - if the query uses pathing, the map may lack a navmesh"), RawItems));

	TArray<FString> Scores;
	for (int32 Index = 0; Index < FMath::Min(ValidItems, 5); Index++)
	{
		Scores.Add(FString::Printf(TEXT("%.3f"), Instance->GetItemScore(Index)));
	}
	const FString ItemTypeName = Instance->ItemType ? Instance->ItemType->GetName() : TEXT("(none)");

	CaptureLog(false);
	StopTicker();
	RemovePIEDelegates();

	const bool bPass = Checks.AllPass();
	Session.State = bPass ? FBTForgeVerifySession::EState::Done : FBTForgeVerifySession::EState::Failed;
	Session.Error = bPass ? FString() : Checks.Summary();
	Session.ResultJson = FString::Printf(
		TEXT("{\"ok\":true,\"sessionId\":\"%s\",\"status\":\"%s\",\"kind\":\"eqs\",\"pass\":%s,")
		TEXT("\"error\":\"%s\",\"checks\":[%s],\"checksPassed\":%d,\"checksTotal\":%d,")
		TEXT("\"queryStatus\":%d,\"queryFinished\":%s,")
		TEXT("\"itemCount\":%d,\"rawItemCount\":%d,\"itemType\":\"%s\",\"topScores\":[%s],")
		TEXT("\"runMode\":\"%s\",\"mapPath\":\"%s\",\"durationMs\":%.1f,\"logErrors\":%s,\"logWarnings\":%d}"),
		*Session.SessionId,
		bPass ? TEXT("done") : TEXT("failed"),
		bPass ? TEXT("true") : TEXT("false"),
		*JsonEscape(Session.Error),
		*Checks.Join(),
		Checks.Passed, Checks.Total,
		static_cast<int32>(Instance->GetRawStatus()),
		Instance->IsFinished() ? TEXT("true") : TEXT("false"),
		ValidItems, RawItems, *ItemTypeName, *FString::Join(Scores, TEXT(",")),
		*RunModeLabel, *JsonEscape(Session.Options.ResolvedMapPath),
		Elapsed * 1000.0,
		*JsonStringArray(Session.LogErrors), Session.LogWarnings.Num());

	SaveResultToDisk(Session.SessionId, Session.ResultJson);
	if (GUnrealEd)
	{
		GUnrealEd->RequestEndPlayMap();
	}
	FHttpServerModule::Get().StartAllListeners();
}

void FBTForgeVerifyManager::FailEQS(const FString& Error, int32 Steps, const FString& RunModeLabel, double Elapsed)
{
	CaptureLog(false);
	StopTicker();
	RemovePIEDelegates();

	FBTForgeCheckList Checks;
	Checks.Add(false, TEXT("query_ran"), Error);
	Checks.Add(false, TEXT("has_results"), TEXT("query never produced an instance"));

	Session.State = FBTForgeVerifySession::EState::Failed;
	Session.Error = Error;
	Session.ResultJson = FString::Printf(
		TEXT("{\"ok\":true,\"sessionId\":\"%s\",\"status\":\"failed\",\"kind\":\"eqs\",\"pass\":false,")
		TEXT("\"error\":\"%s\",\"checks\":[%s],\"checksPassed\":%d,\"checksTotal\":%d,")
		TEXT("\"queryStatus\":-1,\"queryFinished\":false,")
		TEXT("\"itemCount\":0,\"rawItemCount\":0,\"itemType\":\"(none)\",\"topScores\":[],")
		TEXT("\"runMode\":\"%s\",\"mapPath\":\"%s\",\"stepCount\":%d,\"durationMs\":%.1f,\"logErrors\":%s,\"logWarnings\":%d}"),
		*Session.SessionId, *JsonEscape(Error), *Checks.Join(), Checks.Passed, Checks.Total, *RunModeLabel,
		*JsonEscape(Session.Options.ResolvedMapPath), Steps, Elapsed * 1000.0,
		*JsonStringArray(Session.LogErrors), Session.LogWarnings.Num());

	SaveResultToDisk(Session.SessionId, Session.ResultJson);
	if (GUnrealEd)
	{
		GUnrealEd->RequestEndPlayMap();
	}
	FHttpServerModule::Get().StartAllListeners();
}

bool FBTForgeVerifyManager::Tick(float DeltaTime)
{
	FBTForgeVerifyManager& M = Get();
	if (M.Session.State != FBTForgeVerifySession::EState::Running && M.Session.State != FBTForgeVerifySession::EState::RequestingPIE)
	{
		M.Session.TickHandle.Reset();
		return false; // stop ticker
	}

	M.Session.MaxTickDelta = FMath::Max(M.Session.MaxTickDelta, static_cast<double>(DeltaTime));
	const double Now = FPlatformTime::Seconds();

	// PIE startup: deadline, plus a poll for a request that died silently. There is
	// no "start failed" delegate payload to rely on (PostPIEStarted's bool is the
	// Simulate flag), so the engine's own session state is the source of truth.
	if (M.Session.State == FBTForgeVerifySession::EState::RequestingPIE)
	{
		// Startup hitches are not measurement granularity: only frames observed
		// while the tree runs feed the duration window below.
		M.Session.MaxTickDelta = 0.0;
		if (Now > M.Session.VerifyDeadline)
		{
			M.Session.TickHandle.Reset();
			M.AbortToIdle(TEXT("PIE did not start within startDeadlineSec"));
			return false;
		}
		if (GUnrealEd != nullptr && !GUnrealEd->IsPlaySessionInProgress() && (Now - M.Session.RequestTime) > 1.0)
		{
			M.Session.TickHandle.Reset();
			M.AbortToIdle(TEXT("PIE did not start: the play session request was rejected or cancelled before PostPIEStarted"));
			return false;
		}
		return true;
	}

	// BT sampling window.
	UBehaviorTreeComponent* BTComponent = M.Session.BTComp.Get();
	if (BTComponent == nullptr)
	{
		// The fixture's component can vanish while the session is Running: a task
		// destroyed its owner, the controller was removed, or the world was swapped
		// without an EndPIE broadcast. Waiting forever would wedge the manager - this
		// ticker is the only thing that can end the session, so every later
		// start_verify would be refused with "another verify session is in flight"
		// until the editor restarts. Give the fixture a grace window, then fail.
		constexpr double FixtureGraceSec = 5.0;
		if (M.Session.Options.Kind == TEXT("bt")
			&& M.Session.State == FBTForgeVerifySession::EState::Running
			&& (Now - M.Session.PIEStartTime) > FixtureGraceSec)
		{
			M.Session.TickHandle.Reset();
			M.AbortToIdle(TEXT("fixture behavior tree component disappeared before the tree could be sampled"));
			return false;
		}
		return true;
	}
	const double Elapsed = Now - M.Session.TreeStartTime;
	const bool bRunning = BTComponent->IsRunning();
	M.RecordSample(Elapsed);

	const bool bWindowOver = Elapsed >= M.Session.Options.DurationSec;

	if (M.Session.Options.Mode == TEXT("Looped"))
	{
		if (bWindowOver)
		{
			M.FinishSession(bRunning ? FString() : FString(TEXT("Looped tree stopped running before durationSec elapsed")));
			return false;
		}
	}
	else // SingleRun
	{
		if (!bRunning)
		{
			M.FinishSession(FString());
			return false;
		}
		// Completion granularity is one frame, so the allowed overshoot scales with
		// the worst frame time seen while running (bounded: a hitch is not a license
		// to accept a tree that never finishes).
		const double FrameSlack = FMath::Clamp(3.0 * M.Session.MaxTickDelta, 1.0, 2.0);
		const double HardLimit = M.Session.Options.ExpectedDurationSec > 0.0
			? M.Session.Options.ExpectedDurationSec + M.Session.Options.ToleranceSec + FrameSlack + 2.0
			: M.Session.Options.DurationSec + 2.0;
		if (Elapsed > HardLimit)
		{
			M.FinishSession(FString::Printf(TEXT("SingleRun tree did not finish within %.1fs"), HardLimit));
			return false;
		}
	}
	return true;
}

void FBTForgeVerifyManager::FinishSession(const FString& Error)
{
	UBehaviorTreeComponent* BTComponent = Session.BTComp.Get();
	UBlackboardComponent* Blackboard = Session.BBComp.Get();
	const double Elapsed = FPlatformTime::Seconds() - Session.TreeStartTime;

	CaptureLog(false);
	StopTicker();
	RemovePIEDelegates();

	FBTForgeCheckList Checks;
	Checks.Add(true, TEXT("pie_started"), FString::Printf(TEXT("%.2fs after request"), Session.PIEStartTime - Session.RequestTime));

	Checks.Add(Session.bSawRunning, TEXT("running_after_start"),
		Session.bSawRunning
			? FString::Printf(TEXT("started=%s, observed_running=%s"),
				Session.bTreeStarted ? TEXT("true") : TEXT("false"),
				BTComponent != nullptr && BTComponent->IsRunning() ? TEXT("true") : TEXT("false"))
			: TEXT("tree never reported running"));

	if (!Session.Options.ExpectedActiveTask.IsEmpty())
	{
		bool bSeen = false;
		for (const FString& Seen : Session.ActiveTasksSeen)
		{
			if (Seen.Contains(Session.Options.ExpectedActiveTask)) { bSeen = true; break; }
		}
		Checks.Add(bSeen, TEXT("expected_active_task_seen"),
			bSeen ? TEXT("") : TEXT("expected task never appeared; seen: ") + FString::Join(Session.ActiveTasksSeen.Array(), TEXT(",")));
	}

	const bool bTreeRunning = BTComponent != nullptr && BTComponent->IsRunning();
	if (Session.Options.Mode == TEXT("Looped"))
	{
		Checks.Add(Error.IsEmpty(), TEXT("kept_running_for_duration"), Error);
	}
	else
	{
		Checks.Add(Error.IsEmpty() && !bTreeRunning, TEXT("finished_gracefully"), Error);
		if (Session.Options.ExpectedDurationSec > 0.0)
		{
			// Frame-time jitter and the tree's delayed-completion frames make a tight
			// wall-clock window flaky: the allowance is one frame batch (3x the worst
			// frame seen while running, clamped to [1s, 2s]) on top of tolerance. The
			// measured duration is still reported verbatim for the caller to judge.
			const double FrameSlack = FMath::Clamp(3.0 * Session.MaxTickDelta, 1.0, 2.0);
			const double Upper = Session.Options.ExpectedDurationSec + Session.Options.ToleranceSec + FrameSlack;
			const double Lower = Session.Options.ExpectedDurationSec - Session.Options.ToleranceSec;
			const bool bInWindow = Elapsed >= Lower && Elapsed <= Upper;
			Checks.Add(bInWindow, TEXT("finished_in_window"),
				FString::Printf(TEXT("%.2fs vs expected %.2fs, window [%.2fs, %.2fs] (tolerance %.2fs, frame slack %.2fs)"),
					Elapsed, Session.Options.ExpectedDurationSec, Lower, Upper, Session.Options.ToleranceSec, FrameSlack));
		}
	}

	// M4 golden path: movement evidence. The fixture pawn's start position is
	// captured right before StartTree, so a MoveTo/SmartObject chain can be judged
	// by displacement instead of only by "the tree finished".
	double PawnDisplacement = 0.0;
	FVector PawnEndLocation = Session.PawnStartLocation;
	if (const APawn* Pawn = Session.FixturePawn.Get())
	{
		PawnEndLocation = Pawn->GetActorLocation();
		if (Session.bPawnStartCaptured)
		{
			PawnDisplacement = FVector::Dist(PawnEndLocation, Session.PawnStartLocation);
		}
	}
	if (Session.Options.MinPawnDisplacement >= 0.0)
	{
		Checks.Add(PawnDisplacement >= Session.Options.MinPawnDisplacement, TEXT("pawn_moved"),
			FString::Printf(TEXT("moved %.1f (expected >= %.1f), from (%.0f,%.0f,%.0f) to (%.0f,%.0f,%.0f)"),
				PawnDisplacement, Session.Options.MinPawnDisplacement,
				Session.PawnStartLocation.X, Session.PawnStartLocation.Y, Session.PawnStartLocation.Z,
				PawnEndLocation.X, PawnEndLocation.Y, PawnEndLocation.Z));
	}

	Checks.Add(Session.LogErrors.Num() == 0, TEXT("no_log_errors"),
		Session.LogErrors.Num() ? FString::Join(Session.LogErrors, TEXT(" | ")) : TEXT(""));

	UBlackboardData* BBAsset = nullptr;
	FString BlackboardFinal;
	if (Blackboard != nullptr)
	{
		BBAsset = Blackboard->GetBlackboardAsset();
		TArray<FString> Pairs;
		if (BBAsset != nullptr)
		{
			for (const FBlackboardEntry& Entry : BBAsset->Keys)
			{
				const FBlackboard::FKey KeyId = Blackboard->GetKeyID(Entry.EntryName);
				const uint8* Data = Blackboard->GetKeyRawData(Entry.EntryName);
				if (KeyId == FBlackboard::InvalidKey || Data == nullptr) continue;
				FString Value = TEXT("(unreadable)");
				if (Entry.KeyType && Data)
				{
					Value = Entry.KeyType->WrappedDescribeValue(*Blackboard, Data);
				}
				Pairs.Add(FString::Printf(TEXT("\"%s\":\"%s\""), *JsonEscape(Entry.EntryName.ToString()), *JsonEscape(Value)));
			}
		}
		BlackboardFinal = FString::Join(Pairs, TEXT(","));
	}

	// One source of truth: pass is the AND of every check, and status is derived
	// from that same boolean (never from searching the serialized text).
	const bool bPass = Checks.AllPass();
	Session.State = bPass ? FBTForgeVerifySession::EState::Done : FBTForgeVerifySession::EState::Failed;
	Session.Error = Error.IsEmpty() ? (bPass ? FString() : Checks.Summary()) : Error;

	Session.ResultJson = FString::Printf(
		TEXT("{\"ok\":true,\"sessionId\":\"%s\",\"status\":\"%s\",\"kind\":\"bt\",\"pass\":%s,")
		TEXT("\"error\":\"%s\",\"checks\":[%s],\"checksPassed\":%d,\"checksTotal\":%d,\"samples\":[%s],")
		TEXT("\"activeTasksSeen\":[%s],\"durationSec\":%.2f,\"frameMaxSec\":%.3f,\"pawnDisplacement\":%.1f,")
		TEXT("\"pawnStart\":[%.1f,%.1f,%.1f],\"pawnEnd\":[%.1f,%.1f,%.1f],\"pawnStartCaptured\":%s,")
		TEXT("\"mapPath\":\"%s\",\"fixture\":{\"controller\":%s,\"pawn\":%s},")
		TEXT("\"blackboardFinal\":{%s},\"logErrors\":%s,\"logWarnings\":%s,\"logWarningsCount\":%d,\"elapsedSec\":%.2f}"),
		*Session.SessionId,
		bPass ? TEXT("done") : TEXT("failed"),
		bPass ? TEXT("true") : TEXT("false"),
		*JsonEscape(Session.Error),
		*Checks.Join(),
		Checks.Passed, Checks.Total,
		*FString::Join(Session.Samples, TEXT(",")),
		*JsonStringArray(Session.ActiveTasksSeen.Array()),
		Elapsed,
		Session.MaxTickDelta,
		PawnDisplacement,
		// Start/end are exposed so a test can pick a reachable goal (spawn + offset)
		// instead of guessing a coordinate: the fixture pawn spawns where the map puts it.
		Session.PawnStartLocation.X, Session.PawnStartLocation.Y, Session.PawnStartLocation.Z,
		PawnEndLocation.X, PawnEndLocation.Y, PawnEndLocation.Z,
		Session.bPawnStartCaptured ? TEXT("true") : TEXT("false"),
		*JsonEscape(Session.Options.ResolvedMapPath),
		Session.FixtureController.IsValid() ? TEXT("true") : TEXT("false"),
		Session.FixturePawn.IsValid() ? TEXT("true") : TEXT("false"),
		*BlackboardFinal,
		*JsonStringArray(Session.LogErrors),
		*JsonStringArray(Session.LogWarnings),
		Session.LogWarnings.Num(),
		FPlatformTime::Seconds() - Session.RequestTime);

	SaveResultToDisk(Session.SessionId, Session.ResultJson);
	if (GUnrealEd)
	{
		GUnrealEd->RequestEndPlayMap();
	}
	FHttpServerModule::Get().StartAllListeners(); // PIE teardown disables them globally
}

void FBTForgeVerifyManager::AbortToIdle(const FString& Error)
{
	CaptureLog(false);
	// A request that "timed out" may still be queued (long map load, modal dialog):
	// without cancelling it the PIE session starts *after* failure was reported, with
	// no fixture and no owner, and every later start_verify is refused with
	// "PIE already running". Shutdown() cancels for the same reason.
	//
	// Only when nothing is playing, though: the engine keeps the "request queued"
	// flag set for a while after the session actually started, so cancelling on that
	// flag alone tears down a live PIE mid-frame (observed as a TOptional assertion
	// inside the editor tick).
	if (GUnrealEd && Session.State == FBTForgeVerifySession::EState::RequestingPIE
		&& !GUnrealEd->IsPlayingSessionInEditor()
		&& GUnrealEd->IsPlaySessionRequestQueued())
	{
		GUnrealEd->CancelRequestPlaySession();
	}
	if (GUnrealEd && GUnrealEd->IsPlayingSessionInEditor())
	{
		GUnrealEd->RequestEndPlayMap();
	}
	RemovePIEDelegates();
	StopTicker();

	Session.Error = Error;
	Session.State = FBTForgeVerifySession::EState::Failed;
	// The terminal contract says a result always carries checks[]; an abort used to
	// report an empty list, which read as "nothing was checked" instead of "this failed".
	FBTForgeCheckList Checks;
	Checks.Add(false, TEXT("verify_started"), Error);
	Session.ResultJson = FString::Printf(
		TEXT("{\"ok\":true,\"sessionId\":\"%s\",\"status\":\"failed\",\"kind\":\"%s\",\"pass\":false,\"error\":\"%s\",\"checks\":[%s],\"checksPassed\":%d,\"checksTotal\":%d,\"logErrors\":%s}"),
		*Session.SessionId, *JsonEscape(Session.Options.Kind), *JsonEscape(Error),
		*Checks.Join(), Checks.Passed, Checks.Total,
		*JsonStringArray(Session.LogErrors));
	SaveResultToDisk(Session.SessionId, Session.ResultJson);
	FHttpServerModule::Get().StartAllListeners();
}
