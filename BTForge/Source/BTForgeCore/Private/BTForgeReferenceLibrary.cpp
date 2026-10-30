#include "BTForgeReferenceLibrary.h"

#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/BlackboardData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "ProjectDescriptor.h"
#include "Misc/EngineVersion.h"
#include "Misc/PackageName.h"
#include "ModuleDescriptor.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace
{
	constexpr int32 MaxDepth = 16;
	constexpr int32 MaxRefs = 600;

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

	struct FRefRecord
	{
		FString JsonPath;   // where the reference lives (spec path or reflection path)
		FString Ref;        // object path as written / stored
		FString Kind;       // class | object | soft-class | soft-object
	};

	enum class ERefSeverity : uint8
	{
		Ok,
		Warning,
		Error,
	};

	struct FRefVerdict
	{
		FString Domain;      // native-module | project-content | plugin-content | engine-content | transient | invalid
		FString Module;
		FString Plugin;
		bool bEditorOnly = false;
		bool bBlueprint = false;
		bool bResolved = false;
		bool bExistsOnDisk = false;
		bool bCookSafe = true;
		ERefSeverity Severity = ERefSeverity::Ok;
		FString Reason;
	};

	struct FModuleInfo
	{
		bool bKnown = false;
		bool bEditorOnly = false;
		FString Plugin;
		FString Why;
	};

	/** Engine modules that never exist in a cooked build. Plugin modules are
	 *  classified from their descriptor instead; this list only covers built-in
	 *  editor modules, which have no runtime-readable type information. */
	bool IsWellKnownEditorModule(const FString& ModuleName)
	{
		static const TSet<FString> EditorModules =
		{
			TEXT("UnrealEd"), TEXT("UnrealEdMessages"), TEXT("AssetTools"), TEXT("Kismet"),
			TEXT("KismetCompiler"), TEXT("BlueprintGraph"), TEXT("GraphEditor"), TEXT("LevelEditor"),
			TEXT("DetailCustomizations"), TEXT("PropertyEditor"), TEXT("EditorStyle"), TEXT("ToolMenus"),
			TEXT("MaterialEditor"), TEXT("AnimGraph"), TEXT("Sequencer"), TEXT("WorldBrowser"),
			TEXT("ContentBrowser"), TEXT("BehaviorTreeEditor"), TEXT("EnvironmentQueryEditor"),
			TEXT("AIGraph"), TEXT("GameplayDebuggerEditor"), TEXT("AssetRegistry"), TEXT("DerivedDataCache"),
		};
		return EditorModules.Contains(ModuleName);
	}

	const FModuleInfo& LookupModuleInfo(const FString& ModuleName)
	{
		static TMap<FString, FModuleInfo> Cache;
		if (const FModuleInfo* Cached = Cache.Find(ModuleName))
		{
			return *Cached;
		}

		FModuleInfo Info;
		// Plugin modules carry their host type in the descriptor; that is the same
		// evidence UBT uses to decide whether the module exists in a game build.
		for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
		{
			for (const FModuleDescriptor& Module : Plugin->GetDescriptor().Modules)
			{
				if (Module.Name.ToString() != ModuleName)
				{
					continue;
				}
				Info.bKnown = true;
				Info.Plugin = Plugin->GetName();
				const bool bEditorHost = Module.Type == EHostType::Editor || Module.Type == EHostType::EditorNoCommandlet;
				const bool bGameTargetAllowed = Module.TargetAllowList.Num() == 0
					|| Module.TargetAllowList.Contains(EBuildTargetType::Game);
				Info.bEditorOnly = bEditorHost || !bGameTargetAllowed;
				Info.Why = bEditorHost ? TEXT("plugin module declared Type=Editor")
					: (bGameTargetAllowed ? FString() : TEXT("plugin module TargetAllowList excludes Game"));
				break;
			}
			if (Info.bKnown)
			{
				break;
			}
		}

		// Project modules live in the .uproject descriptor, not in any plugin, so a
	// Type=Editor project module used to be classified as a runtime module and the
	// cook verdict passed for code that a game build cannot load.
	if (!Info.bKnown)
	{
		if (const FProjectDescriptor* Project = IProjectManager::Get().GetCurrentProject())
		{
			for (const FModuleDescriptor& Module : Project->Modules)
			{
				if (Module.Name.ToString() != ModuleName)
				{
					continue;
				}
				Info.bKnown = true;
				const bool bEditorHost = Module.Type == EHostType::Editor || Module.Type == EHostType::EditorNoCommandlet;
				const bool bGameTargetAllowed = Module.TargetAllowList.Num() == 0
					|| Module.TargetAllowList.Contains(EBuildTargetType::Game);
				Info.bEditorOnly = bEditorHost || !bGameTargetAllowed;
				Info.Why = bEditorHost ? TEXT("project module declared Type=Editor")
					: (bGameTargetAllowed ? FString() : TEXT("project module TargetAllowList excludes Game"));
				break;
			}
		}
	}

	if (!Info.bKnown && IsWellKnownEditorModule(ModuleName))
		{
			Info.bEditorOnly = true;
			Info.Why = TEXT("built-in editor-only module");
		}

		return Cache.Add(ModuleName, Info);
	}

	FString FirstPathSegment(const FString& Path)
	{
		if (!Path.StartsWith(TEXT("/")))
		{
			return FString();
		}
		const int32 Next = Path.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
		return Next == INDEX_NONE ? Path.Mid(1) : Path.Mid(1, Next - 1);
	}

	FString ScriptModuleFromPath(const FString& Path)
	{
		// /Script/<Module>.<Class>
		const FString Prefix = TEXT("/Script/");
		if (!Path.StartsWith(Prefix))
		{
			return FString();
		}
		FString Rest = Path.Mid(Prefix.Len());
		int32 Dot = INDEX_NONE;
		if (Rest.FindChar(TEXT('.'), Dot))
		{
			return Rest.Left(Dot);
		}
		return Rest;
	}

	FString PackageNameOf(const FString& ObjectPath)
	{
		return FPackageName::ObjectPathToPackageName(ObjectPath);
	}

	bool PathLooksLikeObject(const FString& In)
	{
		const FString Value = In.TrimStartAndEnd();
		if (!Value.StartsWith(TEXT("/")) || Value.Len() < 4 || Value.Contains(TEXT(" ")))
		{
			return false;
		}
		// /Game/AI/BT_X, /Script/AIModule.BTTask_Wait, /Game/AI/BT_X.BT_X_C
		return Value.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1) != INDEX_NONE;
	}

	FString GuessKind(const FString& Path)
	{
		if (Path.StartsWith(TEXT("/Script/")) || Path.EndsWith(TEXT("_C")))
		{
			return TEXT("class");
		}
		return TEXT("asset");
	}

	// ---------------------------------------------------------------- classification

	FRefVerdict ClassifyReference(const FString& Ref, UObject* ResolvedObject, const UClass* ResolvedClass)
	{
		FRefVerdict Verdict;
		const FString Path = Ref.TrimStartAndEnd();

		if (Path.IsEmpty())
		{
			Verdict.Domain = TEXT("invalid");
			Verdict.Severity = ERefSeverity::Error;
			Verdict.bCookSafe = false;
			Verdict.Reason = TEXT("empty reference");
			return Verdict;
		}
		if (Path.StartsWith(TEXT("/Temp/")) || Path.StartsWith(TEXT("/Transient")) || Path.Contains(TEXT("REINST")))
		{
			Verdict.Domain = TEXT("transient");
			Verdict.Severity = ERefSeverity::Error;
			Verdict.bCookSafe = false;
			Verdict.Reason = TEXT("transient/reinstanced object cannot be serialized as an asset reference");
			return Verdict;
		}

		const UClass* ClassToInspect = ResolvedClass;
		if (ClassToInspect == nullptr && ResolvedObject != nullptr)
		{
			ClassToInspect = ResolvedObject->GetClass();
			if (const UClass* AsClass = Cast<UClass>(ResolvedObject))
			{
				ClassToInspect = AsClass;
			}
		}
		Verdict.bResolved = ClassToInspect != nullptr || ResolvedObject != nullptr;
		if (ClassToInspect != nullptr)
		{
			Verdict.bBlueprint = ClassToInspect->HasAnyClassFlags(CLASS_CompiledFromBlueprint);
		}

		if (Path.StartsWith(TEXT("/Script/")))
		{
			Verdict.Domain = TEXT("native-module");
			Verdict.Module = ScriptModuleFromPath(Path);
			const FModuleInfo& Info = LookupModuleInfo(Verdict.Module);
			Verdict.Plugin = Info.Plugin;
			Verdict.bEditorOnly = Info.bEditorOnly;
			if (!Verdict.bResolved)
			{
				Verdict.Severity = ERefSeverity::Error;
				Verdict.bCookSafe = false;
				Verdict.Reason = FString::Printf(TEXT("class %s is not present in this build"), *Path);
			}
			else if (Info.bEditorOnly)
			{
				Verdict.Severity = ERefSeverity::Error;
				Verdict.bCookSafe = false;
				Verdict.Reason = FString::Printf(TEXT("module %s is editor-only (%s); a cooked build cannot load it"),
					*Verdict.Module, *Info.Why);
			}
			else
			{
				Verdict.Reason = Info.Why.IsEmpty() ? TEXT("runtime module") : Info.Why;
			}
		}
		else
		{
			const FString Mount = FirstPathSegment(Path);
			if (Mount == TEXT("Game"))
			{
				Verdict.Domain = TEXT("project-content");
			}
			else if (Mount == TEXT("Engine"))
			{
				Verdict.Domain = TEXT("engine-content");
			}
			else if (!Mount.IsEmpty())
			{
				Verdict.Domain = TEXT("plugin-content");
				Verdict.Plugin = Mount;
				// Confirm the mount belongs to an enabled (mounted) plugin.
				bool bMounted = false;
				const FString MountPrefix = FString::Printf(TEXT("/%s/"), *Mount);
				for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
				{
					const FString MountedRoot = Plugin->GetMountedAssetPath();
					if (!MountedRoot.IsEmpty() && MountedRoot.StartsWith(MountPrefix, ESearchCase::IgnoreCase))
					{
						// An enabled plugin declaring this mount is the strongest evidence
						// available on 5.4-5.5 (IPlugin::IsMounted arrived later); a plugin
						// whose content is not actually mounted still fails the
						// DoesPackageExist check below.
						bMounted = true;
						Verdict.Plugin = Plugin->GetName();
						break;
					}
				}
				if (!bMounted)
				{
					Verdict.Severity = ERefSeverity::Warning;
					Verdict.Reason = FString::Printf(TEXT("plugin content root /%s is not mounted by an enabled plugin"), *Mount);
				}
			}
			else
			{
				Verdict.Domain = TEXT("invalid");
				Verdict.Severity = ERefSeverity::Error;
				Verdict.bCookSafe = false;
				Verdict.Reason = TEXT("not a package path");
				return Verdict;
			}

			// Report the disk state for every content reference, loaded or not. A "resolved"
			// reference can still have no package behind it (transient CDO, in-memory
			// asset) and callers read existsOnDisk as "is there a file for this path".
			const FString PackageName = PackageNameOf(Path);
			Verdict.bExistsOnDisk = !PackageName.IsEmpty() && FPackageName::DoesPackageExist(PackageName);
			if (!Verdict.bResolved)
			{
				if (!Verdict.bExistsOnDisk)
				{
					Verdict.Severity = ERefSeverity::Error;
					Verdict.bCookSafe = false;
					Verdict.Reason = FString::Printf(TEXT("package %s does not exist"), *PackageName);
				}
				else if (Verdict.Severity == ERefSeverity::Ok)
				{
					Verdict.Reason = TEXT("exists on disk (not loaded)");
				}
			}
			if (Verdict.Domain == TEXT("engine-content") && Verdict.Severity == ERefSeverity::Ok)
			{
				Verdict.Severity = ERefSeverity::Warning;
				Verdict.Reason = TEXT("engine content is only cooked when something references it; prefer project or plugin content");
			}
		}

#if WITH_EDITOR
		// The cooker's own opinion, when we actually hold the object.
		const UObject* ObjectToCheck = ResolvedObject;
		if (ClassToInspect != nullptr && ResolvedObject == nullptr)
		{
			ObjectToCheck = ClassToInspect;
		}
		if (ObjectToCheck != nullptr && IsEditorOnlyObject(ObjectToCheck, /*bCheckRecursive=*/true))
		{
			Verdict.bEditorOnly = true;
			Verdict.Severity = ERefSeverity::Error;
			Verdict.bCookSafe = false;
			Verdict.Reason = TEXT("IsEditorOnlyObject: stripped from cooked builds");
		}
#endif

		if (Verdict.Severity == ERefSeverity::Ok && Verdict.Reason.IsEmpty())
		{
			Verdict.Reason = TEXT("resolves and survives cooking");
		}
		return Verdict;
	}

	// ---------------------------------------------------------------- reflection walk

	void CollectFromValue(const FProperty* Property, void* ValuePtr, const FString& JsonPath,
		TArray<FRefRecord>& Out, int32 Depth);

	/** Containers that own their children are walked explicitly below (BT tree,
	 *  EQS options); reflecting them as plain object properties would report the
	 *  same node twice under two different paths. */
	bool ShouldSkipProperty(const UObject* Owner, const FProperty* Property)
	{
		const FString Name = Property->GetName();
		if (Owner != nullptr && Owner->IsA<UBTCompositeNode>())
		{
			return Name == TEXT("Children") || Name == TEXT("Services") || Name == TEXT("DecoratorOps");
		}
		if (Owner != nullptr && Owner->IsA<UEnvQueryOption>())
		{
			return Name == TEXT("Generator") || Name == TEXT("Tests");
		}
		return false;
	}

	void CollectFromObject(UObject* Object, const FString& JsonPath, TArray<FRefRecord>& Out,
		TSet<UObject*>& Visited, int32 Depth)
	{
		if (Object == nullptr || Depth > MaxDepth || Out.Num() >= MaxRefs || Visited.Contains(Object))
		{
			return;
		}
		Visited.Add(Object);

		for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
		{
			const FProperty* Property = *It;
			if (ShouldSkipProperty(Object, Property))
			{
				continue;
			}
			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
			CollectFromValue(Property, ValuePtr, FString::Printf(TEXT("%s.%s"), *JsonPath, *Property->GetName()), Out, Depth + 1);
			if (Out.Num() >= MaxRefs)
			{
				return;
			}
		}
	}

	void CollectFromValue(const FProperty* Property, void* ValuePtr, const FString& JsonPath,
		TArray<FRefRecord>& Out, int32 Depth)
	{
		if (Property == nullptr || ValuePtr == nullptr || Depth > MaxDepth || Out.Num() >= MaxRefs)
		{
			return;
		}

		// Only strong/soft object and class references are asset dependencies;
		// weak/interface/lazy references are ignored on purpose.
		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			if (CastField<FWeakObjectProperty>(Property) != nullptr || CastField<FInterfaceProperty>(Property) != nullptr
				|| CastField<FLazyObjectProperty>(Property) != nullptr)
			{
				return;
			}
			if (CastField<FSoftClassProperty>(Property) != nullptr || CastField<FSoftObjectProperty>(Property) != nullptr)
			{
				const FSoftObjectPtr* SoftPtr = static_cast<const FSoftObjectPtr*>(ValuePtr);
				const FString RefPath = SoftPtr->ToSoftObjectPath().ToString();
				if (!RefPath.IsEmpty())
				{
					Out.Add({ JsonPath, RefPath,
						CastField<FSoftClassProperty>(Property) != nullptr ? TEXT("soft-class") : TEXT("soft-object") });
				}
				return;
			}
			UObject* Value = ObjectProperty->GetObjectPropertyValue(ValuePtr);
			if (Value != nullptr)
			{
				const bool bIsClass = Value->IsA<UClass>();
				Out.Add({ JsonPath, Value->GetPathName(), bIsClass ? TEXT("class") : TEXT("object") });
			}
			return;
		}

		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
			{
				CollectFromValue(*It, It->ContainerPtrToValuePtr<void>(ValuePtr),
					FString::Printf(TEXT("%s.%s"), *JsonPath, *It->GetName()), Out, Depth + 1);
			}
			return;
		}

		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
			for (int32 Index = 0; Index < Helper.Num() && Out.Num() < MaxRefs; ++Index)
			{
				CollectFromValue(ArrayProperty->Inner, Helper.GetRawPtr(Index),
					FString::Printf(TEXT("%s[%d]"), *JsonPath, Index), Out, Depth + 1);
			}
			return;
		}

		if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			FScriptSetHelper Helper(SetProperty, ValuePtr);
			for (int32 Index = 0; Index < Helper.Num() && Out.Num() < MaxRefs; ++Index)
			{
				CollectFromValue(SetProperty->ElementProp, Helper.GetElementPtr(Index),
					FString::Printf(TEXT("%s{%d}"), *JsonPath, Index), Out, Depth + 1);
			}
			return;
		}

		if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			FScriptMapHelper Helper(MapProperty, ValuePtr);
			for (int32 Index = 0; Index < Helper.Num() && Out.Num() < MaxRefs; ++Index)
			{
				CollectFromValue(MapProperty->KeyProp, Helper.GetKeyPtr(Index),
					FString::Printf(TEXT("%s{%d}.key"), *JsonPath, Index), Out, Depth + 1);
				CollectFromValue(MapProperty->ValueProp, Helper.GetValuePtr(Index),
					FString::Printf(TEXT("%s{%d}.value"), *JsonPath, Index), Out, Depth + 1);
			}
			return;
		}
	}

	void CollectBehaviorTreeNode(UObject* Node, const FString& JsonPath, TArray<FRefRecord>& Out,
		TSet<UObject*>& Visited, int32 Depth)
	{
		if (Node == nullptr || Depth > MaxDepth || Out.Num() >= MaxRefs || Visited.Contains(Node))
		{
			return;
		}
		Out.Add({ JsonPath + TEXT(".$class"), Node->GetClass()->GetPathName(), TEXT("class") });

		// CollectFromObject owns the Visited bookkeeping (it marks the object as
		// walked). Adding the node here first would make the property walk return
		// immediately and silently drop every node parameter reference.
		CollectFromObject(Node, JsonPath, Out, Visited, Depth + 1);

		if (UBTCompositeNode* Composite = Cast<UBTCompositeNode>(Node))
		{
			for (int32 ServiceIndex = 0; ServiceIndex < Composite->Services.Num(); ++ServiceIndex)
			{
				CollectBehaviorTreeNode(Composite->Services[ServiceIndex], FString::Printf(TEXT("%s.services[%d]"), *JsonPath, ServiceIndex),
					Out, Visited, Depth + 1);
			}
			for (int32 ChildIndex = 0; ChildIndex < Composite->Children.Num(); ++ChildIndex)
			{
				const FBTCompositeChild& Child = Composite->Children[ChildIndex];
				const FString ChildPath = FString::Printf(TEXT("%s.children[%d]"), *JsonPath, ChildIndex);
				CollectBehaviorTreeNode(Child.ChildComposite, ChildPath + TEXT(".composite"), Out, Visited, Depth + 1);
				CollectBehaviorTreeNode(Child.ChildTask, ChildPath + TEXT(".task"), Out, Visited, Depth + 1);
				for (int32 DecoratorIndex = 0; DecoratorIndex < Child.Decorators.Num(); ++DecoratorIndex)
				{
					CollectBehaviorTreeNode(Child.Decorators[DecoratorIndex],
						FString::Printf(TEXT("%s.decorators[%d]"), *ChildPath, DecoratorIndex), Out, Visited, Depth + 1);
				}
			}
		}
	}

	// ---------------------------------------------------------------- spec scan

	void ScanJsonValue(const TSharedPtr<FJsonValue>& Value, const FString& JsonPath, TArray<FRefRecord>& Out, int32 Depth)
	{
		if (!Value.IsValid() || Depth > MaxDepth || Out.Num() >= MaxRefs)
		{
			return;
		}
		switch (Value->Type)
		{
		case EJson::Object:
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			if (Object.IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
				{
					const FString ChildPath = JsonPath.IsEmpty() ? Pair.Key : FString::Printf(TEXT("%s.%s"), *JsonPath, *Pair.Key);
					ScanJsonValue(Pair.Value, ChildPath, Out, Depth + 1);
				}
			}
			break;
		}
		case EJson::Array:
		{
			const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
			for (int32 Index = 0; Index < Array.Num(); ++Index)
			{
				ScanJsonValue(Array[Index], FString::Printf(TEXT("%s[%d]"), *JsonPath, Index), Out, Depth + 1);
			}
			break;
		}
		case EJson::String:
		{
			const FString Text = Value->AsString();
			if (PathLooksLikeObject(Text))
			{
				Out.Add({ JsonPath, Text.TrimStartAndEnd(), GuessKind(Text.TrimStartAndEnd()) });
			}
			break;
		}
		default:
			break;
		}
	}
}

FString UBTForgeReferenceLibrary::InspectReferences(const FString& RequestJson)
{
	TSharedPtr<FJsonObject> Request;
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestJson);
		if (!FJsonSerializer::Deserialize(Reader, Request) || !Request.IsValid())
		{
			return MakeErrorJson(TEXT("request is not valid JSON"));
		}
	}

	TArray<FRefRecord> Records;
	FString Mode;
	FString AssetPath;
	FString Kind;
	UObject* Asset = nullptr;

	TSharedPtr<FJsonObject> RootForWalk;

	if (Request->TryGetStringField(TEXT("assetPath"), AssetPath) && !AssetPath.IsEmpty())
	{
		Mode = TEXT("asset");
		Kind = Request->HasField(TEXT("kind")) ? Request->GetStringField(TEXT("kind")) : FString();
		Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
		if (Asset == nullptr)
		{
			return MakeErrorJson(FString::Printf(TEXT("asset not found: %s"), *AssetPath));
		}
		if (UBehaviorTree* Tree = Cast<UBehaviorTree>(Asset))
		{
			Kind = TEXT("bt");
			if (Tree->BlackboardAsset != nullptr)
			{
				Records.Add({ TEXT("blackboardAsset"), Tree->BlackboardAsset->GetPathName(), TEXT("asset") });
				for (int32 Index = 0; Index < Tree->BlackboardAsset->Keys.Num(); ++Index)
				{
					const FBlackboardEntry& Entry = Tree->BlackboardAsset->Keys[Index];
					if (Entry.KeyType == nullptr)
					{
						continue;
					}
					TSet<UObject*> Visited;
					CollectFromObject(Entry.KeyType, FString::Printf(TEXT("blackboard.keys[%d].%s"), Index, *Entry.EntryName.ToString()),
						Records, Visited, 0);
				}
			}
			TSet<UObject*> Visited;
			CollectBehaviorTreeNode(Tree->RootNode, TEXT("root"), Records, Visited, 0);
		}
		else if (UEnvQuery* Query = Cast<UEnvQuery>(Asset))
		{
			Kind = TEXT("eqs");
			const TArray<UEnvQueryOption*>& Options = Query->GetOptions();
			for (int32 OptionIndex = 0; OptionIndex < Options.Num(); ++OptionIndex)
			{
				UEnvQueryOption* Option = Options[OptionIndex];
				if (Option == nullptr)
				{
					continue;
				}
				const FString OptionPath = FString::Printf(TEXT("options[%d]"), OptionIndex);
				if (Option->Generator != nullptr)
				{
					Records.Add({ OptionPath + TEXT(".generator.$class"), Option->Generator->GetClass()->GetPathName(), TEXT("class") });
					TSet<UObject*> Visited;
					CollectFromObject(Option->Generator, OptionPath + TEXT(".generator"), Records, Visited, 0);
				}
				for (int32 TestIndex = 0; TestIndex < Option->Tests.Num(); ++TestIndex)
				{
					UEnvQueryTest* Test = Option->Tests[TestIndex];
					if (Test == nullptr)
					{
						continue;
					}
					const FString TestPath = FString::Printf(TEXT("%s.tests[%d]"), *OptionPath, TestIndex);
					Records.Add({ TestPath + TEXT(".$class"), Test->GetClass()->GetPathName(), TEXT("class") });
					TSet<UObject*> Visited;
					CollectFromObject(Test, TestPath, Records, Visited, 0);
				}
			}
		}
		else
		{
			return MakeErrorJson(FString::Printf(TEXT("asset %s is neither a BehaviorTree nor an EnvQuery"), *AssetPath));
		}
	}
	else
	{
		Mode = TEXT("spec");
		Kind = Request->HasField(TEXT("kind")) ? Request->GetStringField(TEXT("kind")) : FString();
		const TSharedPtr<FJsonObject>* SpecObject = nullptr;
		if (Kind == TEXT("bt"))
		{
			const TSharedPtr<FJsonValue>* TreeValue = Request->Values.Find(TEXT("treeJson"));
			if (TreeValue == nullptr || !(*TreeValue).IsValid())
			{
				return MakeErrorJson(TEXT("bt spec mode needs treeJson"));
			}
			if ((*TreeValue)->Type == EJson::String)
			{
				TSharedPtr<FJsonObject> Parsed;
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create((*TreeValue)->AsString());
				if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
				{
					return MakeErrorJson(TEXT("treeJson string is not valid JSON"));
				}
				RootForWalk = Parsed;
			}
			else if ((*TreeValue)->TryGetObject(SpecObject) && SpecObject->IsValid())
			{
				RootForWalk = *SpecObject;
			}
			else
			{
				return MakeErrorJson(TEXT("treeJson must be an object or a JSON string"));
			}
			if (Request->HasField(TEXT("blackboardJson")))
			{
				const TSharedPtr<FJsonValue>* BlackboardValue = Request->Values.Find(TEXT("blackboardJson"));
				if (BlackboardValue != nullptr && (*BlackboardValue).IsValid())
				{
					if ((*BlackboardValue)->Type == EJson::String)
					{
						TSharedPtr<FJsonObject> Parsed;
						const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create((*BlackboardValue)->AsString());
						if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
						{
							ScanJsonValue(MakeShared<FJsonValueObject>(Parsed), TEXT("blackboardJson"), Records, 0);
						}
					}
					else if ((*BlackboardValue)->TryGetObject(SpecObject) && SpecObject->IsValid())
					{
						ScanJsonValue(MakeShared<FJsonValueObject>(*SpecObject), TEXT("blackboardJson"), Records, 0);
					}
				}
			}
		}
		else if (Kind == TEXT("eqs"))
		{
			const TSharedPtr<FJsonValue>* QueryValue = Request->Values.Find(TEXT("queryJson"));
			if (QueryValue == nullptr || !(*QueryValue).IsValid())
			{
				return MakeErrorJson(TEXT("eqs spec mode needs queryJson"));
			}
			if ((*QueryValue)->Type == EJson::String)
			{
				TSharedPtr<FJsonObject> Parsed;
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create((*QueryValue)->AsString());
				if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
				{
					return MakeErrorJson(TEXT("queryJson string is not valid JSON"));
				}
				RootForWalk = Parsed;
			}
			else if ((*QueryValue)->TryGetObject(SpecObject) && SpecObject->IsValid())
			{
				RootForWalk = *SpecObject;
			}
			else
			{
				return MakeErrorJson(TEXT("queryJson must be an object or a JSON string"));
			}
		}
		else
		{
			return MakeErrorJson(TEXT("spec mode needs kind \"bt\" (treeJson) or \"eqs\" (queryJson)"));
		}

		if (RootForWalk.IsValid())
		{
			ScanJsonValue(MakeShared<FJsonValueObject>(RootForWalk), FString(), Records, 0);
		}
	}

	// ---------------------------------------------------------------- resolve + classify

	struct FClassifiedRef
	{
		FRefRecord Record;
		FRefVerdict Verdict;
	};

	TArray<FClassifiedRef> Classified;
	Classified.Reserve(Records.Num());
	int32 IgnoredTransient = 0;
	TSet<FString> Seen;
	for (const FRefRecord& Record : Records)
	{
		// Same reference at the same place twice (e.g. class + $class) would only
		// add noise; identical (path, ref) pairs are collapsed.
		const FString DedupKey = Record.JsonPath + TEXT("|") + Record.Ref;
		if (Seen.Contains(DedupKey))
		{
			continue;
		}
		Seen.Add(DedupKey);

		UObject* ResolvedObject = nullptr;
		UClass* ResolvedClass = nullptr;
		if (Record.Kind == TEXT("class") || Record.Kind == TEXT("soft-class"))
		{
			ResolvedClass = LoadObject<UClass>(nullptr, *Record.Ref);
			if (ResolvedClass != nullptr)
			{
				ResolvedObject = ResolvedClass;
			}
		}
		else if (Record.Kind == TEXT("object"))
		{
			ResolvedObject = FindObject<UObject>(nullptr, *Record.Ref);
		}
		else
		{
			// soft references are resolved without loading when possible
			ResolvedObject = FindObject<UObject>(nullptr, *Record.Ref);
		}

		// Transient editor-side defaults (e.g. the AllowedTypes key-type objects a
		// blackboard selector instantiates) are not serialized dependencies; they
		// would drown the report in noise, so they are counted and skipped.
		if (Record.Ref.Contains(TEXT("/Transient")) || Record.Ref.StartsWith(TEXT("/Temp/")))
		{
			++IgnoredTransient;
			continue;
		}

		FClassifiedRef Entry;
		Entry.Record = Record;
		Entry.Verdict = ClassifyReference(Record.Ref, ResolvedObject, ResolvedClass);
		Classified.Add(MoveTemp(Entry));
	}

	Classified.Sort([](const FClassifiedRef& A, const FClassifiedRef& B)
	{
		return A.Record.JsonPath < B.Record.JsonPath;
	});

	// ---------------------------------------------------------------- report

	TArray<TSharedPtr<FJsonValue>> RefEntries;
	TArray<TSharedPtr<FJsonValue>> ErrorEntries;
	TArray<TSharedPtr<FJsonValue>> WarningEntries;
	TMap<FString, int32> ByDomain;
	TMap<FString, int32> ByModule;
	int32 ErrorCount = 0;
	int32 WarningCount = 0;

	for (const FClassifiedRef& Entry : Classified)
	{
		const FRefVerdict& Verdict = Entry.Verdict;
		ByDomain.FindOrAdd(Verdict.Domain.IsEmpty() ? TEXT("unknown") : Verdict.Domain)++;
		if (!Verdict.Module.IsEmpty())
		{
			ByModule.FindOrAdd(Verdict.Module)++;
		}

		TSharedRef<FJsonObject> RefObject = MakeShared<FJsonObject>();
		RefObject->SetStringField(TEXT("jsonPath"), Entry.Record.JsonPath);
		RefObject->SetStringField(TEXT("ref"), Entry.Record.Ref);
		RefObject->SetStringField(TEXT("kind"), Entry.Record.Kind);
		RefObject->SetStringField(TEXT("domain"), Verdict.Domain);
		if (!Verdict.Module.IsEmpty()) { RefObject->SetStringField(TEXT("module"), Verdict.Module); }
		if (!Verdict.Plugin.IsEmpty()) { RefObject->SetStringField(TEXT("plugin"), Verdict.Plugin); }
		RefObject->SetBoolField(TEXT("consumableByGameBuild"), Verdict.bCookSafe);
		// Documented field name (BTForgeReferenceLibrary.h) — same value, so consumers
		// reading either spelling get the cook verdict instead of an empty field.
		RefObject->SetBoolField(TEXT("cookSafe"), Verdict.bCookSafe);
		RefObject->SetBoolField(TEXT("editorOnly"), Verdict.bEditorOnly);
		RefObject->SetBoolField(TEXT("blueprint"), Verdict.bBlueprint);
		RefObject->SetBoolField(TEXT("resolved"), Verdict.bResolved);
		RefObject->SetBoolField(TEXT("existsOnDisk"), Verdict.bExistsOnDisk);
		RefObject->SetStringField(TEXT("reason"), Verdict.Reason);
		RefEntries.Add(MakeShared<FJsonValueObject>(RefObject));

		if (Verdict.Severity == ERefSeverity::Error)
		{
			++ErrorCount;
			TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("jsonPath"), Entry.Record.JsonPath);
			Error->SetStringField(TEXT("ref"), Entry.Record.Ref);
			Error->SetStringField(TEXT("reason"), Verdict.Reason);
			ErrorEntries.Add(MakeShared<FJsonValueObject>(Error));
		}
		else if (Verdict.Severity == ERefSeverity::Warning)
		{
			++WarningCount;
			TSharedRef<FJsonObject> Warning = MakeShared<FJsonObject>();
			Warning->SetStringField(TEXT("jsonPath"), Entry.Record.JsonPath);
			Warning->SetStringField(TEXT("ref"), Entry.Record.Ref);
			Warning->SetStringField(TEXT("reason"), Verdict.Reason);
			WarningEntries.Add(MakeShared<FJsonValueObject>(Warning));
		}
	}

	// The cap is a safety valve for cyclic graphs, not a report size: silently dropping
	// the tail used to let a large tree report verdict "ok" while most of it was never
	// inspected. Say it out loud and downgrade the verdict.
	const bool bTruncated = Classified.Num() >= MaxRefs;
	if (bTruncated && ErrorCount == 0)
	{
		++WarningCount;
		TSharedRef<FJsonObject> TruncationWarning = MakeShared<FJsonObject>();
		TruncationWarning->SetStringField(TEXT("jsonPath"), TEXT("$"));
		TruncationWarning->SetStringField(TEXT("reason"),
			FString::Printf(TEXT("reference list truncated at %d entries; the verdict only covers what was scanned"), MaxRefs));
		WarningEntries.Add(MakeShared<FJsonValueObject>(TruncationWarning));
	}

	const FString VerdictName = ErrorCount > 0 ? TEXT("fail") : (WarningCount > 0 ? TEXT("warn") : TEXT("ok"));

	TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("total"), Classified.Num());
	Summary->SetNumberField(TEXT("ignoredTransient"), IgnoredTransient);
	Summary->SetNumberField(TEXT("errors"), ErrorCount);
	Summary->SetNumberField(TEXT("warnings"), WarningCount);
	Summary->SetBoolField(TEXT("truncated"), bTruncated);
	Summary->SetNumberField(TEXT("maxRefs"), MaxRefs);

	TSharedRef<FJsonObject> DomainObject = MakeShared<FJsonObject>();
	{
		TArray<FString> DomainKeys;
		ByDomain.GetKeys(DomainKeys);
		DomainKeys.Sort();
		for (const FString& Key : DomainKeys)
		{
			DomainObject->SetNumberField(Key, ByDomain.FindRef(Key));
		}
	}
	Summary->SetObjectField(TEXT("byDomain"), DomainObject);

	TSharedRef<FJsonObject> ModuleObject = MakeShared<FJsonObject>();
	{
		TArray<FString> ModuleKeys;
		ByModule.GetKeys(ModuleKeys);
		ModuleKeys.Sort();
		for (const FString& Key : ModuleKeys)
		{
			ModuleObject->SetNumberField(Key, ByModule.FindRef(Key));
		}
	}
	Summary->SetObjectField(TEXT("byModule"), ModuleObject);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("mode"), Mode);
	Result->SetStringField(TEXT("kind"), Kind);
	Result->SetStringField(TEXT("verdict"), VerdictName);
	Result->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
	if (!AssetPath.IsEmpty())
	{
		Result->SetStringField(TEXT("asset"), Asset->GetPathName());
	}
	Result->SetArrayField(TEXT("references"), RefEntries);
	Result->SetArrayField(TEXT("errors"), ErrorEntries);
	Result->SetArrayField(TEXT("warnings"), WarningEntries);
	Result->SetObjectField(TEXT("summary"), Summary);
	Result->SetStringField(TEXT("note"), TEXT("consumableByGameBuild=false means the reference cannot exist in a cooked build: missing package, transient object, or a class from an editor-only module."));

	return JsonToString(Result);
}
