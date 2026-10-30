#include "BTForgeMcpServer.h"

#include "Async/Async.h"
#include "BTForgeBlueprintLibrary.h"
#include "BTForgeCatalogLibrary.h"
#include "BTForgeEQSLibrary.h"
#include "BTForgeReferenceLibrary.h"
#include "BTForgeVerifyLibrary.h"
#include "BTForgeLibrary.h"
#include "Dom/JsonObject.h"
#include "HttpPath.h"
#include "HttpServerConstants.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "Misc/ConfigCacheIni.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY(LogBTForgeMcp);

namespace
{
	constexpr TCHAR JsonRpcVersion[] = TEXT("2.0");

	// JSON-RPC 2.0 reserved error codes.
	constexpr int32 ErrParse = -32700;
	constexpr int32 ErrInvalidRequest = -32600;
	constexpr int32 ErrMethodNotFound = -32601;
	constexpr int32 ErrInvalidParams = -32602;

	FString JsonToString(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		auto Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	bool ParseJsonString(const FString& Text, TSharedPtr<FJsonObject>& OutObject)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
	}

	TUniquePtr<FHttpServerResponse> MakeHttpJson(const FString& Body, EHttpServerResponseCodes Code)
	{
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
		Response->Code = Code;
		return Response;
	}

	TUniquePtr<FHttpServerResponse> MakeHttpEmpty(EHttpServerResponseCodes Code)
	{
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(FString(), TEXT("application/json"));
		Response->Code = Code;
		return Response;
	}

	FString MakeRpcResult(const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Result)
	{
		TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("jsonrpc"), JsonRpcVersion);
		Body->SetField(TEXT("id"), Id.IsValid() ? Id : MakeShared<FJsonValueNull>());
		Body->SetField(TEXT("result"), MakeShared<FJsonValueObject>(Result));
		return JsonToString(Body);
	}

	FString MakeRpcError(const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& Message)
	{
		TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetNumberField(TEXT("code"), Code);
		Error->SetStringField(TEXT("message"), Message);

		TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("jsonrpc"), JsonRpcVersion);
		Body->SetField(TEXT("id"), Id.IsValid() ? Id : MakeShared<FJsonValueNull>());
		Body->SetObjectField(TEXT("error"), Error);
		return JsonToString(Body);
	}

	/** Anti DNS-rebinding: browsers must come from a loopback origin; native clients send no Origin at all. */
	bool IsOriginAllowed(const FHttpServerRequest& Request)
	{
		for (const TPair<FString, TArray<FString>>& Header : Request.Headers)
		{
			if (Header.Key.Equals(TEXT("origin"), ESearchCase::IgnoreCase))
			{
				for (const FString& Value : Header.Value)
				{
					if (Value.Contains(TEXT("//localhost")) || Value.Contains(TEXT("//127.0.0.1")) || Value.Contains(TEXT("//[::1]")))
					{
						return true;
					}
				}
				return false;
			}
		}
		return true;
	}

	struct FToolDescriptor
	{
		FString Name;
		FString Description;
		const TCHAR* InputSchemaJson; // JSON Schema literal, parsed on demand
	};

	const TArray<FToolDescriptor>& GetToolDescriptors()
	{
		static const TArray<FToolDescriptor> Tools =
		{
			{
				TEXT("create_behavior_tree_asset"),
				TEXT("Create a Blackboard + BehaviorTree asset pair. The tree starts empty (no root); fill it with build_behavior_tree. Fails if either asset already exists; nothing is overwritten."),
				TEXT(R"json({"type":"object","properties":{"AssetPath":{"type":"string","description":"Target BT asset path, e.g. /Game/AI/BT_Sentinel. The blackboard is created next to it as <AssetPath>_BB."},"BlackboardJson":{"type":["object","string"],"description":"Blackboard spec {\"keys\":[{\"name\":\"HomeLocation\",\"type\":\"Vector\"}]}; key types: Bool, Int, Float, String, Name, Vector, Rotator, Object. A real JSON object is accepted directly."}},"required":["AssetPath","BlackboardJson"]})json")
			},
			{
				TEXT("build_behavior_tree"),
				TEXT("Build (or rebuild) the tree of an existing BT asset from a JSON spec. Replaces the whole root; the previous tree is discarded on success. Root must be Sequence/Selector; composites need a non-empty children array; tasks take no children; unknown types or params are rejected with a JSON path. Do not call while PIE is running. Node types: Sequence, Selector, Wait (WaitTime/RandomDeviation), MoveTo (BlackboardKey=goal key name, AcceptableRadius), RunEQS (BlackboardKey=result key, EQSRequest={QueryTemplate: EQS asset path, RunMode, QueryConfig:[{ParamName,Value,BBKey}]}). Composite nodes accept \"services\":[{type,params}] (RunEQS service: same EQSRequest shape) ticking while the branch runs; any child accepts \"decorators\":[{type,params}] gating entry - Blackboard decorator (BlackboardKey, KeyQueryType, FlowAbortMode, NotifyObserver); multiple decorators combine as AND. Any 'type' may instead be a class path like /Game/Folder/Asset.Asset_C or /Script/Module.Class to instantiate project-defined blueprint/native node classes (base-class validated: composites/tasks, UBTService, UBTDecorator; abstract classes rejected). Blackboard key names are validated against the tree's blackboard; asset paths are loaded and type-checked."),
				TEXT(R"json({"type":"object","properties":{"AssetPath":{"type":"string","description":"Existing BT asset path created by create_behavior_tree_asset."},"TreeJson":{"type":["object","string"],"description":"Root node object, e.g. {\"type\":\"Selector\",\"name\":\"Root\",\"children\":[{\"type\":\"Sequence\",\"children\":[{\"type\":\"RunEQS\",\"params\":{\"EQSRequest\":{\"QueryTemplate\":\"/ShooterCore/Bot/EQS/EQS_FindTarget.EQS_FindTarget\",\"RunMode\":\"SingleResultPerQuery\",\"QueryConfig\":[{\"ParamName\":\"Distance\",\"BBKey\":\"MoveGoal\"}]}},\"children\":[]},{\"type\":\"MoveTo\",\"params\":{\"BlackboardKey\":\"MoveGoal\",\"AcceptableRadius\":50}}]},{\"type\":\"Wait\",\"params\":{\"WaitTime\":5.0}}]}. A real JSON object is accepted directly."}},"required":["AssetPath","TreeJson"]})json")
			},
			{
				TEXT("inspect_behavior_tree"),
				TEXT("Read a BT asset back as JSON: blackboard keys plus the full node tree with each node's class, name and scalar property values. Use to verify a build or inspect an existing tree before extending it."),
				TEXT(R"json({"type":"object","properties":{"AssetPath":{"type":"string","description":"Existing BT asset path."}},"required":["AssetPath"]})json")
			},
			{
				TEXT("validate_behavior_tree"),
				TEXT("Validate a Behavior Tree spec and blackboard spec WITHOUT creating or modifying any asset. Builds everything into transient objects (node classes, parameters, services, decorators, blackboard keys) and returns the first error with its JSON path. Use this to iterate on a spec before create/build."),
				TEXT(R"json({"type":"object","properties":{"TreeJson":{"type":["object","string"],"description":"Root node object, same syntax as build_behavior_tree."},"BlackboardJson":{"type":["object","string"],"description":"Optional blackboard spec {\"keys\":[...]}; defaults to no keys."}},"required":["TreeJson"]})json")
			},
			{
				TEXT("create_eqs_asset"),
				TEXT("Create an EQS (UEnvQuery) asset from a JSON spec. Options are built from native generators/tests (or any non-abstract UEnvQueryGenerator/UEnvQueryTest class path). The editor graph is left empty and rebuilt from the Options data when opened. Fails if the asset already exists; nothing is created on validation error. Query JSON: {\"options\":[{\"optionName\":\"...\",\"generator\":{\"type\":\"SimpleGrid\",\"params\":{...}},\"tests\":[{\"type\":\"Distance\",\"params\":{...}}]}],\"bStripFromClientBuilds\":false}. Param names are exact UPROPERTY names; unknown names are rejected with the allowed list. Class references are object paths (e.g. /Script/AIModule.EnvQueryContext_Querier or /Game/.../MyContext.MyContext_C). Runtime blackboard context: /Script/BTForgeRuntime.BTForgeEnvQueryContext_Blackboard with param BlackboardKey."),
				TEXT(R"json({"type":"object","properties":{"AssetPath":{"type":"string","description":"Target EQS asset path, e.g. /Game/AI/EQS_FindCover."},"QueryJson":{"type":["object","string"],"description":"EQS query spec as described in the tool description."}},"required":["AssetPath","QueryJson"]})json")
			},
			{
				TEXT("inspect_eqs_asset"),
				TEXT("Read an EQS query asset back as JSON: options, generator/test classes and their editable parameter values. Use to verify a created query or to inspect an existing one."),
				TEXT(R"json({"type":"object","properties":{"AssetPath":{"type":"string","description":"Existing EQS asset path."}},"required":["AssetPath"]})json")
			},
			{
				TEXT("validate_eqs_spec"),
				TEXT("Validate an EQS query spec WITHOUT creating or modifying any asset. Loads referenced classes, writes all parameters into transient objects and checks generator item types against tests."),
				TEXT(R"json({"type":"object","properties":{"QueryJson":{"type":["object","string"],"description":"EQS query spec (same syntax as create_eqs_asset)."}},"required":["QueryJson"]})json")
			},
			{
				TEXT("get_capability_catalog"),
				TEXT("Return a machine-readable inventory of what can be used in this project: native + project Blueprint BT node classes (with class paths and editable param names), EQS query assets, native/Blueprint EQS contexts, generators and tests, plus the short type aliases accepted by the spec language. Call this before authoring a spec so class paths are real instead of guessed. Read-only."),
				TEXT(R"json({"type":"object","properties":{"SectionJson":{"type":["object","string"],"description":"Optional {\"section\":\"all\"|\"bt\"|\"eqs\"}; defaults to all."}},"required":[]})json")
			},
			{
				TEXT("start_verify"),
				TEXT("Runtime verification (M3): start an in-process PIE session and actually run a generated asset. OptionsJson: {kind:\"bt\"|\"eqs\", assetPath, mapPath?, mode?(bt: SingleRun|Looped), durationSec?, expectedDurationSec?, toleranceSec?, expectedActiveTask?, expectedMinPawnDisplacement?(bt, px), blackboardInit?{key:value}, fixturePawn?(bt, default true), bareGameMode?(default true), gameModeClassPath?, queryRunMode?, queryParams?{name:number}}. Starts PIE with GameModeOverride=AGameModeBase (bareGameMode: mapPath defaults to /Engine/Maps/Templates/Template_Default because a Lyra map asserts on a bare GameMode), spawns an AIController + Pawn fixture, starts the tree (SingleRun finishes naturally; Looped must keep running for durationSec) or runs the EQS query synchronously, samples IsRunning/active tasks from t=0. Fails fast if a PIE/SIE session is already running. Returns {sessionId,status:pending,mapPath}; poll get_verify_result."),
				TEXT(R"json({"type":"object","properties":{"OptionsJson":{"type":["object","string"],"description":"Verification options as described."}},"required":["OptionsJson"]})json")
			},
			{
				TEXT("get_verify_result"),
				TEXT("Poll a runtime verification session started by start_verify. Returns {status:pending|running} until finished, then the full result: pass (top-level boolean = AND of every checks[] entry; status is done only when pass is true), checks[{name,pass,detail}] (pie_started, running_after_start, expected_active_task_seen, finished_gracefully, finished_in_window, kept_running_for_duration, pawn_moved, no_log_errors), samples timeline (from t=0), activeTasksSeen, pawnDisplacement (bt), blackboardFinal (bt) or itemCount(=NumValidItems)/rawItemCount/itemType/topScores (eqs), logErrors list."),
				TEXT(R"json({"type":"object","properties":{"SessionId":{"type":"string","description":"Session id returned by start_verify."}},"required":["SessionId"]})json")
			},
			{
				TEXT("inspect_references"),
				TEXT("Reference / content-domain policy (M4), read-only. Reports every object reference of a BT or EQS asset (or of a spec, before anything is written) with its domain (native module / project content / plugin content / engine content), module, plugin, blueprint flag, and whether it can exist in a cooked build. Returns verdict ok|warn|fail plus errors[]/warnings[] with JSON paths. Use it after build_* and before shipping, or on a spec to catch missing / editor-only references early. RequestJson: {\"assetPath\":\"/Game/AI/BT_X\"} for an existing asset, or {\"kind\":\"bt\",\"treeJson\":{...},\"blackboardJson\":{...}} / {\"kind\":\"eqs\",\"queryJson\":{...}} for a spec."),
				TEXT(R"json({"type":"object","properties":{"RequestJson":{"type":["object","string"],"description":"{\"assetPath\":\"...\"} or {\"kind\":\"bt\",\"treeJson\":{...}} or {\"kind\":\"eqs\",\"queryJson\":{...}}"}},"required":["RequestJson"]})json")
			},
			{
				TEXT("create_node_blueprint"),
				TEXT("Blueprint node generation (M6): create a data-only Blueprint node asset - a generated class derived from a BT node base class (UBTTaskNode/UBTService/UBTDecorator, no Blueprint composites) with ParamsJson written onto the class CDO through the same reflection writers build_behavior_tree uses. No event-graph logic is generated. The returned classPath plugs straight into build_behavior_tree as a node 'type', and the capability catalog lists the new class automatically. Fails if the asset exists; nothing is kept on failure. Blackboard key names in params resolve per-tree when the node is placed."),
				TEXT(R"json({"type":"object","properties":{"AssetPath":{"type":"string","description":"Target blueprint asset path, e.g. /Game/AI/BTA_LogShot."},"ParentClassPath":{"type":"string","description":"Class path of a non-abstract UBTTaskNode/UBTService/UBTDecorator-derived class, e.g. /Script/BTForgeRuntime.BTForgeTask_LogMessage or an existing /Game/..._C."},"ParamsJson":{"type":["object","string"],"description":"{\"PropertyName\": value, ...} applied to the class defaults, e.g. {\"Message\":\"shot fired\",\"Severity\":\"Warning\"}. Optional (empty = engine defaults)."}},"required":["AssetPath","ParentClassPath"]})json")
			},
		};
		return Tools;
	}

	FString MakeToolErrorText(const FString& Message)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetBoolField(TEXT("ok"), false);
		Object->SetStringField(TEXT("error"), Message);
		return JsonToString(Object);
	}

	/** Accept a JSON-encoded string or a real object value where the library expects a JSON text. */
	bool GetToolArgAsJsonText(const TSharedPtr<FJsonObject>& Args, const TCHAR* Name, FString& OutJsonText, FString& OutError)
	{
		const TSharedPtr<FJsonValue>* ValuePtr = Args.IsValid() ? Args->Values.Find(Name) : nullptr;
		if (ValuePtr == nullptr)
		{
			OutError = FString::Printf(TEXT("missing required argument '%s'"), Name);
			return false;
		}
		const TSharedPtr<FJsonValue> Value = *ValuePtr;
		FString AsString;
		if (Value->TryGetString(AsString))
		{
			OutJsonText = AsString;
			return true;
		}
		const TSharedPtr<FJsonObject>* AsObject;
		if (Value->TryGetObject(AsObject) && AsObject->IsValid())
		{
			OutJsonText = JsonToString(AsObject->ToSharedRef());
			return true;
		}
		OutError = FString::Printf(TEXT("argument '%s' must be a string or a JSON object"), Name);
		return false;
	}

	/** Runs on the game thread. Returns the library's JSON text; sets bOutIsToolError from its "ok" field. */
	FString ExecuteToolCall(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, bool& bOutIsToolError)
	{
		bOutIsToolError = true;

		FString AssetPath;
		FString ArgError;
		const bool bHasAssetPath = Args.IsValid() && Args->TryGetStringField(TEXT("AssetPath"), AssetPath);

		FString LibraryResult;
		if (ToolName == TEXT("create_behavior_tree_asset"))
		{
			FString BlackboardJson;
			if (bHasAssetPath && GetToolArgAsJsonText(Args, TEXT("BlackboardJson"), BlackboardJson, ArgError))
			{
				LibraryResult = UBTForgeLibrary::CreateBehaviorTreeAsset(AssetPath, BlackboardJson);
			}
		}
		else if (ToolName == TEXT("build_behavior_tree"))
		{
			FString TreeJson;
			if (bHasAssetPath && GetToolArgAsJsonText(Args, TEXT("TreeJson"), TreeJson, ArgError))
			{
				LibraryResult = UBTForgeLibrary::BuildBehaviorTree(AssetPath, TreeJson);
			}
		}
		else if (ToolName == TEXT("inspect_behavior_tree"))
		{
			if (bHasAssetPath)
			{
				LibraryResult = UBTForgeLibrary::InspectBehaviorTree(AssetPath);
			}
		}
		else if (ToolName == TEXT("validate_behavior_tree"))
		{
			FString TreeJson;
			if (GetToolArgAsJsonText(Args, TEXT("TreeJson"), TreeJson, ArgError))
			{
				FString BlackboardJson;
				if (Args.IsValid() && Args->HasField(TEXT("BlackboardJson")))
				{
					GetToolArgAsJsonText(Args, TEXT("BlackboardJson"), BlackboardJson, ArgError);
				}
				LibraryResult = UBTForgeLibrary::ValidateBehaviorTree(TreeJson, BlackboardJson);
			}
		}
		else if (ToolName == TEXT("create_eqs_asset"))
		{
			FString QueryJson;
			if (bHasAssetPath && GetToolArgAsJsonText(Args, TEXT("QueryJson"), QueryJson, ArgError))
			{
				LibraryResult = UBTForgeEQSLibrary::CreateEQSAsset(AssetPath, QueryJson);
			}
		}
		else if (ToolName == TEXT("inspect_eqs_asset"))
		{
			if (bHasAssetPath)
			{
				LibraryResult = UBTForgeEQSLibrary::InspectEQSAsset(AssetPath);
			}
		}
		else if (ToolName == TEXT("validate_eqs_spec"))
		{
			FString QueryJson;
			if (GetToolArgAsJsonText(Args, TEXT("QueryJson"), QueryJson, ArgError))
			{
				LibraryResult = UBTForgeEQSLibrary::ValidateEQSSpec(QueryJson);
			}
		}
		else if (ToolName == TEXT("get_capability_catalog"))
		{
			FString SectionJson;
			if (Args.IsValid() && Args->HasField(TEXT("SectionJson")))
			{
				GetToolArgAsJsonText(Args, TEXT("SectionJson"), SectionJson, ArgError);
			}
			LibraryResult = UBTForgeCatalogLibrary::GetCapabilityCatalog(SectionJson);
		}
		else if (ToolName == TEXT("inspect_references"))
		{
			// M4 reference / content-domain policy. Read-only; a "fail" verdict is
			// still a successful tool call (data), so isError stays false.
			FString RequestJson;
			if (!GetToolArgAsJsonText(Args, TEXT("RequestJson"), RequestJson, ArgError))
			{
				return MakeToolErrorText(ArgError);
			}
			bOutIsToolError = false;
			return UBTForgeReferenceLibrary::InspectReferences(RequestJson);
		}
		else if (ToolName == TEXT("create_node_blueprint"))
		{
			FString ParentClassPath;
			FString ParamsJson;
			if (bHasAssetPath && Args->TryGetStringField(TEXT("ParentClassPath"), ParentClassPath))
			{
				if (Args->HasField(TEXT("ParamsJson")))
				{
					GetToolArgAsJsonText(Args, TEXT("ParamsJson"), ParamsJson, ArgError);
				}
				LibraryResult = UBTForgeBlueprintLibrary::CreateNodeBlueprint(AssetPath, ParentClassPath, ParamsJson);
			}
		}
		else if (ToolName == TEXT("start_verify") || ToolName == TEXT("get_verify_result"))
		{
			// M3 runtime verification. Includes UE 5.5 in the description because
			// PIE works there too; on any engine the session is polled, never blocked.
			FString OptionsJson;
			if (ToolName == TEXT("start_verify"))
			{
				if (!GetToolArgAsJsonText(Args, TEXT("OptionsJson"), OptionsJson, ArgError))
				{
					return MakeToolErrorText(ArgError);
				}
				LibraryResult = UBTForgeVerifyLibrary::StartVerify(OptionsJson);
				bOutIsToolError = false;
				return LibraryResult;
			}
			else
			{
				FString SessionId;
				if (Args.IsValid() && Args->TryGetStringField(TEXT("SessionId"), SessionId))
				{
					LibraryResult = UBTForgeVerifyLibrary::GetVerifyResult(SessionId);
				}
				else
				{
					return MakeToolErrorText(TEXT("missing required argument 'SessionId'"));
				}
				bOutIsToolError = false; // verify results are data, not tool errors
				return LibraryResult;
			}
		}
		else
		{
			return MakeToolErrorText(FString::Printf(TEXT("unknown tool '%s'"), *ToolName));
		}

		if (LibraryResult.IsEmpty())
		{
			return MakeToolErrorText(TEXT("invalid arguments; check each tool's inputSchema for required fields"));
		}

		TSharedPtr<FJsonObject> Parsed;
		bool bOk = false;
		if (ParseJsonString(LibraryResult, Parsed) && Parsed->TryGetBoolField(TEXT("ok"), bOk))
		{
			bOutIsToolError = !bOk;
		}
		return LibraryResult;
	}

	TSharedRef<FJsonObject> HandleInitialize(const TSharedPtr<FJsonObject>& Params)
	{
		static const TArray<FString> SupportedProtocolVersions =
		{
			TEXT("2024-11-05"), TEXT("2025-03-26"), TEXT("2025-06-18"), TEXT("2025-11-25"),
		};

		FString ClientVersion;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("protocolVersion"), ClientVersion);
		}
		const FString& Negotiated = SupportedProtocolVersions.Contains(ClientVersion) ? ClientVersion : SupportedProtocolVersions[0];

		TSharedRef<FJsonObject> ToolsCapability = MakeShared<FJsonObject>();
		ToolsCapability->SetBoolField(TEXT("listChanged"), false);
		TSharedRef<FJsonObject> Capabilities = MakeShared<FJsonObject>();
		Capabilities->SetObjectField(TEXT("tools"), ToolsCapability);

		TSharedRef<FJsonObject> ServerInfo = MakeShared<FJsonObject>();
		ServerInfo->SetStringField(TEXT("name"), TEXT("BTForge"));
		ServerInfo->SetStringField(TEXT("version"), TEXT("2.5.0"));

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("protocolVersion"), Negotiated);
		Result->SetObjectField(TEXT("capabilities"), Capabilities);
		Result->SetObjectField(TEXT("serverInfo"), ServerInfo);
		return Result;
	}

	TUniquePtr<FHttpServerResponse> ProcessMcpPostBody(const FString& BodyText)
	{
		TSharedPtr<FJsonObject> Rpc;
		if (BodyText.IsEmpty() || !ParseJsonString(BodyText, Rpc))
		{
			return MakeHttpJson(MakeRpcError(nullptr, ErrParse, TEXT("Parse error: body is not valid JSON")), EHttpServerResponseCodes::BadRequest);
		}

		FString JsonRpcField;
		FString Method;
		if (!Rpc->TryGetStringField(TEXT("jsonrpc"), JsonRpcField) || JsonRpcField != JsonRpcVersion ||
			!Rpc->TryGetStringField(TEXT("method"), Method))
		{
			return MakeHttpJson(MakeRpcError(nullptr, ErrInvalidRequest, TEXT("Invalid Request: expected a JSON-RPC 2.0 object with a 'method'")), EHttpServerResponseCodes::BadRequest);
		}

		const TSharedPtr<FJsonValue>* IdPtr = Rpc->Values.Find(TEXT("id"));
		const TSharedPtr<FJsonValue> Id = IdPtr ? *IdPtr : TSharedPtr<FJsonValue>();

		// Notifications (no "id") never get a JSON-RPC response body; acknowledge with 202.
		if (!Id.IsValid())
		{
			return MakeHttpEmpty(EHttpServerResponseCodes::Accepted);
		}

		const TSharedPtr<FJsonObject> Params = Rpc->HasField(TEXT("params")) ? Rpc->GetObjectField(TEXT("params")) : nullptr;

		if (Method == TEXT("initialize"))
		{
			return MakeHttpJson(MakeRpcResult(Id, HandleInitialize(Params)), EHttpServerResponseCodes::Ok);
		}
		if (Method == TEXT("ping"))
		{
			return MakeHttpJson(MakeRpcResult(Id, MakeShared<FJsonObject>()), EHttpServerResponseCodes::Ok);
		}
		if (Method == TEXT("tools/list"))
		{
			TArray<TSharedPtr<FJsonValue>> Tools;
			for (const FToolDescriptor& Descriptor : GetToolDescriptors())
			{
				TSharedRef<FJsonObject> Tool = MakeShared<FJsonObject>();
				Tool->SetStringField(TEXT("name"), Descriptor.Name);
				Tool->SetStringField(TEXT("description"), Descriptor.Description);

				TSharedPtr<FJsonObject> Schema;
				if (ParseJsonString(FString(Descriptor.InputSchemaJson), Schema))
				{
					Tool->SetField(TEXT("inputSchema"), MakeShared<FJsonValueObject>(Schema));
				}
				Tools.Add(MakeShared<FJsonValueObject>(Tool));
			}
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetArrayField(TEXT("tools"), Tools);
			return MakeHttpJson(MakeRpcResult(Id, Result), EHttpServerResponseCodes::Ok);
		}
		if (Method == TEXT("tools/call"))
		{
			FString ToolName;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("name"), ToolName))
			{
				return MakeHttpJson(MakeRpcError(Id, ErrInvalidParams, TEXT("Invalid params: tools/call requires 'name'")), EHttpServerResponseCodes::Ok);
			}
			const TSharedPtr<FJsonObject> Args = (Params.IsValid() && Params->HasField(TEXT("arguments")))
				? Params->GetObjectField(TEXT("arguments"))
				: MakeShared<FJsonObject>();

			bool bIsToolError = true;
			const FString LibraryResult = ExecuteToolCall(ToolName, Args, bIsToolError);

			TSharedRef<FJsonObject> TextContent = MakeShared<FJsonObject>();
			TextContent->SetStringField(TEXT("type"), TEXT("text"));
			TextContent->SetStringField(TEXT("text"), LibraryResult);
			TArray<TSharedPtr<FJsonValue>> Content;
			Content.Add(MakeShared<FJsonValueObject>(TextContent));

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetArrayField(TEXT("content"), Content);
			Result->SetBoolField(TEXT("isError"), bIsToolError);
			return MakeHttpJson(MakeRpcResult(Id, Result), EHttpServerResponseCodes::Ok);
		}
		return MakeHttpJson(MakeRpcError(Id, ErrMethodNotFound, TEXT("Method not found: ") + Method), EHttpServerResponseCodes::Ok);
	}
}

FBTForgeMcpServer::~FBTForgeMcpServer()
{
	Stop();
}

bool FBTForgeMcpServer::Start(uint32 InPort)
{
	if (PostRoute.IsValid())
	{
		UE_LOG(LogBTForgeMcp, Warning, TEXT("Server already running on port %u; call BTForge.MCP.StopServer first."), Port);
		return false;
	}

	// FHttpServerModule keeps a global "listeners enabled" switch, off by
	// default. On 5.8 Epic's MCP server flips it via StartAllListeners(); on
	// older engines nothing does, and GetHttpRouter would silently return a
	// router whose port never listens. Flip it ourselves, and ask for a hard
	// failure when the bind itself fails.
	FHttpServerModule::Get().StartAllListeners();
	TSharedPtr<IHttpRouter> NewRouter = FHttpServerModule::Get().GetHttpRouter(InPort, /*bFailOnBindFailure=*/true);
	if (!NewRouter.IsValid())
	{
		UE_LOG(LogBTForgeMcp, Error, TEXT("Failed to get an HTTP router for port %u."), InPort);
		return false;
	}

	const FHttpPath McpPath(TEXT("/mcp"));
	PostRoute = NewRouter->BindRoute(McpPath, EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateRaw(this, &FBTForgeMcpServer::HandlePostRequest));
	GetRoute = NewRouter->BindRoute(McpPath, EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FBTForgeMcpServer::HandleUnsupportedVerb));
	DeleteRoute = NewRouter->BindRoute(McpPath, EHttpServerRequestVerbs::VERB_DELETE,
		FHttpRequestHandler::CreateRaw(this, &FBTForgeMcpServer::HandleUnsupportedVerb));

	if (!PostRoute.IsValid())
	{
		UE_LOG(LogBTForgeMcp, Error, TEXT("Failed to bind POST /mcp on port %u."), InPort);
		if (GetRoute.IsValid()) { NewRouter->UnbindRoute(GetRoute); GetRoute.Reset(); }
		if (DeleteRoute.IsValid()) { NewRouter->UnbindRoute(DeleteRoute); DeleteRoute.Reset(); }
		return false;
	}

	Router = NewRouter;
	Port = InPort;
	UE_LOG(LogBTForgeMcp, Display, TEXT("BTForge MCP endpoint: http://127.0.0.1:%u/mcp (JSON-RPC 2.0 over HTTP, stateless)"), Port);
	return true;
}

void FBTForgeMcpServer::Stop()
{
	if (Router.IsValid())
	{
		if (PostRoute.IsValid()) { Router->UnbindRoute(PostRoute); }
		if (GetRoute.IsValid()) { Router->UnbindRoute(GetRoute); }
		if (DeleteRoute.IsValid()) { Router->UnbindRoute(DeleteRoute); }
		UE_LOG(LogBTForgeMcp, Display, TEXT("BTForge MCP endpoint on port %u stopped."), Port);
	}
	Router.Reset();
	PostRoute.Reset();
	GetRoute.Reset();
	DeleteRoute.Reset();
	Port = 0;
}

bool FBTForgeMcpServer::HandlePostRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (!IsOriginAllowed(Request))
	{
		OnComplete(MakeHttpJson(MakeRpcError(nullptr, ErrInvalidRequest, TEXT("Forbidden: Origin header is not a loopback origin")), EHttpServerResponseCodes::Forbidden));
		return true;
	}

	FString BodyText;
	if (Request.Body.Num() > 0)
	{
		const FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
		BodyText = FString(Converter.Length(), Converter.Get());
	}

	if (IsInGameThread())
	{
		OnComplete(ProcessMcpPostBody(BodyText));
		return true;
	}

	// The engine's HTTPServer currently delivers route handlers on the game
	// thread; marshal defensively so any future threading change cannot reach
	// UObject code off-thread. OnComplete may fire on a later tick.
	AsyncTask(ENamedThreads::GameThread, [BodyText, OnComplete]()
	{
		OnComplete(ProcessMcpPostBody(BodyText));
	});
	return true;
}

bool FBTForgeMcpServer::HandleUnsupportedVerb(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// Streamable-HTTP GET streams and session DELETE are not offered; 405 tells clients to use plain POST.
	OnComplete(MakeHttpEmpty(EHttpServerResponseCodes::BadMethod));
	return true;
}
