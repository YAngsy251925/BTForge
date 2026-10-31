# Changelog

All notable changes to BT Forge are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [2.7.0] — 2026-10-31

### Added

- **Key dataflow analysis** (shared by `validate_behavior_tree` and
  `build_behavior_tree`): every node's `FBlackboardKeySelector` bindings are read
  through reflection and classified as read/write against a capability table, then
  reported as a `dataflow` block:
  - `keys[]` - each key with `writtenBy` / `readBy` paths;
  - `deadGates[]` - keys read by a decorator or task but never written (fatal when
    every node's effects are known; reported only when unknown-effects nodes exist);
  - `orphanWriters[]`, `scopeWarnings[]` (a service only ticks while its own branch
    is active), `abortWarnings[]` (`FlowAbortMode=None` on a key that changes);
  - `unknownEffects[]` - unregistered Blueprint classes that carry key selectors.
- **Spec declarations**: `externalKeys` (`{key: reason}`) marks keys fed from
  outside the tree (tree root, or BlackboardJson for validate); `nodeEffects`
  (`{ClassName: {writes:[...], reads:[...]}}`) teaches the analyzer project nodes.
- **`inspect_behavior_tree` reports key bindings** per node/service/decorator
  (`"keys": {"BlackboardKey": {"name": "MoveGoal", "type": "BlackboardKeyType_Object"}}`),
  resolved against the tree's blackboard instead of walking selector internals.
- **Blackboard Object keys: declarable base class**: the key spec accepts
  `"base"` (short class name or full object path) on Object keys; the class is
  resolved with existence checking, non-Object keys with `base` are rejected,
  and `inspect_behavior_tree` reports the resolved base per key.
- **`repair_graph` (13th tool)**: drops a BT asset's stale editor graph and saves
  the asset so the Behavior Tree editor rebuilds the graph from the data chain on
  next open. After a `build_behavior_tree` from a headless or cross-process
  session the serialized graph still points at the replaced node instances and
  the editor shows every node as "class not found" while the tree itself still
  runs; this closes the rebuild-then-open loop without a manual console command.
  Refuses while the asset is open in an editor (an open graph would win and
  overwrite the repair on its next save); `build_behavior_tree`'s description now
  cross-references it. Previously editor-console-only (`BTForge.RepairGraph`,
  which keeps working).

### Fixed

- `validate_behavior_tree` reads `externalKeys` / `nodeEffects` from the blackboard
  spec as well as from the tree root, so the same declaration works for validate and
  build.
- `build_behavior_tree` no longer fails on "dead gates" when the tree contains
  unregistered Blueprint nodes - their effects are unknown, so the finding is a
  warning until `nodeEffects` is declared.
- `Tools/mcp_smoke.py`: L2 uses a MoveTo-compatible Object result key for the actor
  item query, a timestamped asset name, and covers dead-gate rejection + external
  declaration + inspect key readback.

## [2.6.2] — 2026-10-31

### Fixed

- **Duplicate blackboard keys**: `UBlackboardData::PostInitProperties` already adds the
  engine's `SelfActor` key to every new blackboard, and `create_behavior_tree_asset`
  appended the spec's keys on top without looking at what was there — a spec that
  declared `SelfActor` produced a second entry with that name. `GetKeyID` resolved to
  the built-in one, the duplicate showed up in every `inspect_behavior_tree`, and its
  declared type was silently ignored. Specs that re-declare an existing key are now
  rejected with an actionable error naming the built-in key, and duplicate detection is
  case-insensitive (FName), so `selfactor` no longer slips past `SelfActor`.
- **`validate_behavior_tree` did not know about engine-added keys**: its transient
  blackboard has `SelfActor`, but the set used to validate node key references was built
  from the spec only, so a spec that used `SelfActor` was rejected as "unknown
  blackboard key" while `build_behavior_tree` accepted it. The transient blackboard's
  keys now seed that set, so validate and build agree.

### Added

- **`blackboardInit` can drive actor-keyed branches**: Object keys accept the
  runtime-only tokens `$FixturePawn` / `$FixtureController` (an asset path cannot name
  a PIE-spawned actor), so a test can exercise "TargetEnemy is set" chains instead of
  only the stateless fallback.
- **Verify result reports the fixture pawn's path**: `pawnStart` / `pawnEnd` /
  `pawnStartCaptured` alongside `pawnDisplacement`, so a test can pick a *reachable*
  goal (spawn + offset) instead of guessing coordinates on a nav-meshed map.

### Docs

- README (EN/ZH), `create_behavior_tree_asset`'s MCP description and both header docs
  state that `SelfActor` is built in and must not be declared.
- `Tools/mcp_smoke.py` covers it: "no duplicate blackboard keys", "re-declaring
  SelfActor is rejected", "validate accepts the built-in SelfActor key".

## [2.6.1] — 2026-10-31

Correctness and contract pass: seven paths that could wedge a session or report
success for work that never happened, plus the "silent drop" class of bugs
(wrong-shaped spec fields were treated as absent). No API surface was removed;
input that used to be silently ignored is now rejected with an exact reason.

### Fixed

- **Verify session could wedge the manager forever**: while Running, a vanished
  fixture `UBehaviorTreeComponent` made the ticker return without ever ending the
  session, so every later `start_verify` was refused with "another verify session
  is in flight" until the editor restarted. The ticker now fails the session after
  a 5 s fixture grace window.
- **Orphan PIE sessions**: `AbortToIdle` now cancels a still-queued play-session
  request (mirroring `Shutdown`), so a start deadline that fires during a long map
  load no longer brings up a PIE session nobody owns. The cancel is skipped while a
  session is actually playing - the engine keeps the "request queued" flag set for
  a while after the session starts, and cancelling then tears down a live PIE
  mid-frame (reproduced as a `TOptional::GetValue` assertion inside the editor tick).
- **Blackboard key selectors wrote the wrong key type**: `FBlackboard::FKey` is an
  id across the whole blackboard chain, so indexing `UBlackboardData::Keys` with it
  picked the wrong entry (or none) whenever the blackboard has a parent. Now uses
  `GetKey()`.
- **Half-created assets**: a failed BehaviorTree creation left the already-created
  Blackboard behind (retrying the same path then failed with "already exists"), and
  a failed EQS/blueprint save left an unsaved package that vanished on restart while
  the tool had reported `ok:true`. All creation paths now roll back and report.
- **Silently ignored tool arguments**: wrong-shaped `BlackboardJson`,
  `SectionJson`, `ParamsJson` and `RecipeJson` used to fall back to "not provided";
  they are now tool errors. `blackboardInit`/`queryParams` accept an object or a JSON
  string and reject anything else.
- **Wrong-shaped spec fields**: `"params": "x"`, `"children": 5`, `"keys": 5`,
  `"generator": {...}, "params": "x"` are errors instead of "no params / no children
  / no keys".
- **EQS could not write a plain uint8 property** (it was always routed to the enum
  writer and rejected). Byte properties without an enum now write as numbers.
- **Blueprint recipes ignored refused pin connections**: an unwired `FinishExecute`
  produced a task that sat InProgress forever while creation reported success.

### Changed

- **Runtime nodes fail loudly instead of silently succeeding**: `ComputeValue` and
  `UpdateDistance` verify the key exists on the runtime blackboard (a CDO default can
  name a key that no longer exists), `UpdateDistance` requires a Float `ResultKey` and
  writes `-1` when the source location cannot be resolved (a stale distance used to
  gate decorators forever), and `LogMessage` reports `<missing key>` instead of `0`.
  Key-type checks are subclass-aware (`IsChildOf`), so project `UBlackboardKeyType_*`
  subclasses work like the engine's own getters.
- **`inspect_references`**: reports `cookSafe` (documented name) next to
  `consumableByGameBuild`, computes `existsOnDisk` for resolved references too,
  classifies **project** modules from the `.uproject` descriptor (a `Type=Editor`
  project module no longer passes the cook gate), and flags `summary.truncated` +
  downgrades the verdict instead of silently stopping at 600 references.
- **`get_capability_catalog`**: sorts before applying the per-section cap (output is
  reproducible), sorts EQS queries, and rejects a malformed `SectionJson` instead of
  returning the full catalog.
- **Verify results on disk**: one `result_<sessionId>.json` per run plus an atomic
  `last_result.json` (write-to-temp then rename); a poller can no longer read a
  half-written file, and a second run no longer overwrites the first run's evidence.
  Failed sessions now carry `checks[]`/`checksPassed`/`checksTotal` like every other
  terminal result.
- **Official endpoint parity**: `ValidateBehaviorTree.BlackboardJson` and
  `GetCapabilityCatalog.SectionJson` now carry defaults (the schema generator treats
  empty string defaults as "no default"), and the `BuildBehaviorTree` description
  covers services, decorators and class paths.

### Docs

- README (EN/ZH): the tool table lists all twelve tools (it claimed eleven and was
  missing `create_node_blueprint`), two merged Markdown rows/bullets are fixed, the
  known-limitations list no longer says Blueprint nodes cannot be created or that the
  EQS context subclass is an "M6 first step", and the runtime suite is quoted as the
  34 checks it actually reports.

### Verified

- UE 5.8 Lyra, built-in endpoint (`:8090`): base / L2 / L3.5 / M1-M2 / M3 (34) /
  M4 / M5 / M6 **all PASS**.
- UE 5.5 blank project: compiles with **zero source changes**; base / M1-M2 /
  M3 (34) **PASS** (L2/L3.5 need Lyra content).

## [2.6.0] — 2026-10-30

M6 (part 2): constrained event-graph recipes — `create_node_blueprint` gains
`RecipeJson` and synthesizes a working K2 event graph into generated node
Blueprints. The milestone planned for part 2 is now complete; the roadmap item
is retired.

### Added

- **`RecipeJson` on `create_node_blueprint`** (both endpoints): the recipe
  `"call_function"` generates a fixed graph into the blueprint's event graph —
  **Event ReceiveExecuteAI (tasks) / ReceiveActivationAI (services) →
  CallFunction(Function, self = OwnerController/ControlledPawn pin) →
  FinishExecute(true) (tasks)**. The target object comes from the AI event's
  own output pins, so no getters or casts are needed. Synthesis uses the
  engine's canonical placement sequence (`CreateNewGuid` → `PostPlacedNewNode`
  → `AllocateDefaultPins` → `AddNode`) and `UEdGraphSchema_K2` connections;
  validation happens before any asset is created:
  - parent must be `UBTTask_BlueprintBase` / `UBTService_BlueprintBase`;
  - function must exist on `AAIController` (controller target) or `APawn`
    (pawn target), be BlueprintCallable, non-pure, non-static, non-deprecated,
    and take zero parameters;
  - the whole asset is deleted on any failure.
- **`Tools/mcp_m6_bp.py`** extends to recipes: positive task recipe (chain
  continues into a blackboard marker: `Seq == 1`), positive service recipe
  (activation fires harmlessly), and six rejection cases with exact reasons.

### Fixed

- **Abstract BlueprintBase parents were rejected**: "derive and implement" is
  the standard pattern and every engine `*BlueprintBase` is `Abstract +
  Blueprintable` — the concrete-parent check was removed (the
  `CanCreateBlueprintOfClass` probe remains as the real gate). The engine BT
  node bases are still rejected, now with the Blueprintable explanation.
- **Official endpoint marked `RecipeJson` as required**: the ToolsetRegistry
  schema generator treats an *empty* CPP-default string as "no default"
  (`ParameterDefaultString.IsEmpty()`), so the default is now `"{}"` — which
  the library accepts alongside `""` as "no recipe". (The generator requires
  every parameter that has neither schema metadata "default" nor an
  `FOptionalProperty` type.)

### Verified

- UE 5.8, built-in endpoint (`:8090`): M6 suite **PASS** (recipe task proves
  the synthesized graph executes end to end; service recipe fires on
  activation), M5 suite **PASS**, M3 suite **35/35**, M1/M2 suite **35/35**,
  base suite PASS, M4 golden path **PASS**.
- UE 5.8, official endpoint (`:8000`): M6 suite **PASS** (12 tools).
- UE 5.5 blank project: compiles with **zero changes**; M6/M5/M3/M1/M2/base
  suites and the M4 golden path all **PASS**.

### Known limitations (recipe scope)

- One recipe (`call_function`), zero-parameter functions, targets limited to
  the AI controller / its pawn. Constant-argument wiring, function returns
  (decorator condition graphs), and project-class casts are future work.
- A recipe task that destroys the fixture pawn wedges headless PIE (modal
  ensure) — deliberately breaking the verify fixture is out of contract, so
  the suite's "function really ran" evidence is the end-to-end chain marker,
  not a destructive probe.

## [2.5.0] — 2026-10-30

M6 (part 1): Blueprint node generation — the `create_node_blueprint` tool
(12 MCP tools total) creates data-only Blueprint node assets: a compiled
`/Game/..._C` class derived from a BT node base class, with per-project
defaults written onto the class CDO and no event-graph logic. The roadmap's
"constrained event-graph recipes" remain future work.

### Added

- **`create_node_blueprint`** (both endpoints): given an asset path, a
  non-abstract `UBTTaskNode`/`UBTService`/`UBTDecorator`-derived parent class
  path and a params JSON, it builds the Blueprint through
  `UBlueprintFactory` + `FKismetEditorUtilities::CompileBlueprint`, applies the
  params to the generated class CDO, saves, and returns the class path — which
  plugs straight into `build_behavior_tree` as a node `type` (L3.5) and is
  listed by `get_capability_catalog` automatically (AssetRegistry). Fails if
  the asset exists; any failure deletes the half-created asset.
  - Parent validation is precise: base-class domain check (no Blueprint
    composites — they do not exist in the engine), abstract check, and
    `FKismetEditorUtilities::CanCreateBlueprintOfClass` with an actionable
    error (engine BT node bases are MinimalAPI-only and not Blueprintable —
    derive from a BTForge node class or an engine `*BlueprintBase`).
  - Params reuse the spec reflection writers (`UBTForgeLibrary::
    ApplyParamsToObject`): enums by name, class paths, nested structs — now
    exported for reuse. Blackboard key names are accepted without tree
    validation (a CDO is tree-independent; the key resolves per-tree when the
    node is placed).
- **`Tools/mcp_m6_bp.py`**: creation, duplicate/bad-parent/unknown-param
  rejections (plus the nothing-left-behind check), catalog listing/classification,
  spec dry-run, and two PIE runs proving CDO defaults drive runtime behavior
  (a BP task logs its configured message at its configured severity; a BP
  service writes its configured key every tick).
- The seven M5 node classes are now `Blueprintable` — they were designed as
  extension points, and this is what lets `create_node_blueprint` derive from
  them.

### Verified

- UE 5.8, built-in endpoint (`:8090`): M6 suite **PASS** (35 checks, 2 PIE
  runs), M5 suite **PASS**, M3 suite **35/35**, M1/M2 suite **35/35**, base
  suite PASS, M4 golden path **PASS**.
- UE 5.8, official endpoint (`:8000`): M6 suite **PASS** (12 tools registered).
- UE 5.5 blank project: compiles with **zero changes**; M6/M5/M3/M1/M2/base
  suites and the M4 golden path all **PASS**.

### Known limitations (M6 part 1 scope)

- Data-only: no event-graph generation (roadmap part 2). A generated node runs
  its parent class's behavior with the configured defaults.
- Blueprint parents must be `Blueprintable`; engine-native BT node classes are
  not (use an engine `*BlueprintBase` or a BTForge node class as parent).
- The catalog still shows no parameter schema for Blueprint entries (CDO
  loading per entry is deliberately avoided); `create_node_blueprint`'s result
  echoes what was applied.

## [2.4.0] — 2026-10-30

M5: a native parameterized node library shipped in `BTForgeRuntime` — common
service/decorator/task behaviors that previously required Blueprint authoring,
plus the spec-alias and catalog plumbing that makes them first-class. Still
**11 MCP tools**; the nodes ride the existing builder/verify pipeline.

### Added

- **`BTForgeRuntime` node library** (`BTForgeBehaviorNodes.{h,cpp}`, 7 classes,
  all stateless, all cook-safe, no version conditionals):
  - tasks `UBTForgeTask_ComputeValue` (Left key <op> Right key-or-constant →
    Result key; Add/Subtract/Multiply/Divide/Min/Max; Int results round) and
    `UBTForgeTask_LogMessage` (message + optional key value to `LogBTForge`;
    Severity Log/Warning/Error — Error deliberately fails a verify run's
    `no_log_errors`, Warning surfaces in `logWarnings`);
  - services `UBTForgeService_SetBlackboardValue` (periodic typed write:
    per-type constants, or a copy of any compatible key) and
    `UBTForgeService_UpdateDistance` (periodic distance between two Object/Vector
    keys → Float key — the utility-AI input pattern);
  - decorators `UBTForgeDecorator_FloatCompare` (key vs constant-or-key with
    six operators — the engine's `CompareBBEntries` only does Equal/NotEqual
    between two keys), `UBTForgeDecorator_DistanceCheck` (range gate between
    two location sources, negative bound = disabled, no bounds = fail-closed),
    and `UBTForgeDecorator_Chance` (probability gate, exact at 0/100).
  - The numeric/distance decorators observe their keys and abort the underlying
    branch on change (engine observer pattern).
- **Spec aliases** for the library: `ComputeValue`, `Log`, `SetValue`,
  `UpdateDistance`, `FloatCompare`, `DistanceCheck`, `Chance`. The alias tables
  are now exported from `UBTForgeLibrary` (`GetNodeTypeAliases` /
  `GetAuxTypeAliases`) and the catalog's `specTypes` lists are derived from
  those same tables, so the catalog can no longer drift from the parser
  whitelist (the EQS aliases already worked this way).
- **`Tools/mcp_m5_nodes.py`**: catalog/schema checks, alias + nested-param
  dry-runs (including a version-gated interop control using the engine's own
  `BTTask_SetKeyValueFloat` with a key-bound `FValueOrBBKey` value), and 8 PIE
  behavior runs asserting blackboard outcomes, service ticks, branch selection,
  log-severity effects, and the M4 reference policy over an M5 asset.

### Fixed

- **Key-bound `FValueOrBBKey_*` params were not writable**: a struct param with
  a `DefaultValue` member forced the JSON value through that member, so the
  engine's Set-key tasks (5.6+) could only take constants. An object value is
  now applied member-wise (`{"Value": {"Key": "SomeKey"}}` binds the key).
- **Optional key selectors resolved to a wrong key**: calling
  `ResolveSelectedKey` on a selector whose `SelectedKeyName` is None runs the
  engine's `InitSelection`, which silently binds the *first compatible
  blackboard key* — a "not configured" source became a wrong read. Unset
  selectors are now left unresolved (`InvalidateResolvedKey`).

### Verified

- UE 5.8, built-in endpoint (`:8090`): M5 suite **PASS** (8 PIE behavior runs),
  M3 suite **35/35**, M1/M2 suite **35/35**, base suite PASS, M4 golden
  path **PASS**.
- UE 5.8, official endpoint (`:8000`): M5 suite **PASS**, M3 suite **35/35**,
  11 tools registered.
- UE 5.5 blank project: compiles with **zero changes**; M5 suite **PASS**,
  M3 suite **35/35**, M1/M2 suite **35/35**, base suite PASS.

### Known limitations (M5 scope)

- The engine's `UBTTask_SetKeyValue*` family exists only on 5.6+; smoke suites
  must not reference it unconditionally (the M5 suite probes the catalog and
  skips the interop control on engines without it).
- The library is deliberately small: engine natives (`Wait`, `MoveTo`,
  `Cooldown`, `TimeLimit`, `Loop`, `ForceSuccess`, `IsAtLocation`,
  `RotateToFaceBBEntry`, ...) already cover the rest and appear in the catalog
  alongside these classes.
- `LogMessage` writes to the `LogBTForge` category only and appends at most one
  key value; it is a checkpoint, not a logging framework.

## [2.3.0] — 2026-10-30

M4: the golden path end to end (catalog → spec → asset → references → runtime),
plus a reference / content-domain policy that can gate a spec before anything is
written. 11 MCP tools.

### Added

- **`inspect_references`** (both endpoints): a read-only reference and
  content-domain report for a BT/EQS asset *or* a spec.
  - asset mode walks the real objects — BT node trees with services and
    decorators, blackboard keys (including key-type class references), EQS
    options/generators/tests, and nested struct params — and reports each
    reference with a JSON path;
  - spec mode scans `treeJson` / `queryJson` / `blackboardJson` for object paths,
    so missing or forbidden references are caught before `create_*` writes
    anything.
  - Each entry carries `kind`, `domain` (`native-module`, `project-content`,
    `plugin-content`, `engine-content`), `module`, `plugin`, `editorOnly`,
    `blueprint`, `resolved`, `existsOnDisk`, `consumableByGameBuild`, `reason`;
    the report aggregates to `verdict` `ok|warn|fail` with `errors[]`/
    `warnings[]` and `summary.byDomain`/`byModule`.
  - Editor-only detection: plugin descriptors (`Type=Editor`, or a
    `TargetAllowList` without `Game`), the cooker's `IsEditorOnlyObject`, and a
    documented list of built-in editor modules. Transient editor-side defaults
    (blackboard key filters) are counted as `ignoredTransient` instead of noise.
- **Movement evidence in verification**: `expectedMinPawnDisplacement` turns
  "did the movement chain actually run" into a `pawn_moved` check, and results
  now report `pawnDisplacement` (fixture pawn start → end distance).
- **`logWarnings`** array in the verify result (warnings are reported, never
  failed on), alongside `logErrors`.
- **`Tools/mcp_m4_goldenpath.py`**: the deterministic golden path — catalog
  (class paths read from the catalog, never guessed), EQS dry-run, create,
  inspect, reference policy, BT create, a deliberately broken build followed by
  the repair, inspect, reference policy (asset + spec), PIE run with `pass` and a
  real goal value, and (opt-in) a nav-meshed map phase that asserts the fixture
  pawn moved to the EQS-filtered goal. Works on both endpoints.

### Fixed

- **`FBlackboardKeySelector` was written with the key name only**: key
  type/id are what runtime code reads (`UBTTask_RunEQSQuery` ->
  `StoreInBlackboard`, for example), so a tree built by BTForge could run a query
  that produced items and still report a failed task. `InitializeNodesRecursive`
  now resolves `SelectedKeyType`/`SelectedKeyID` from the tree's blackboard
  before `InitializeFromAsset`.
- **A bad `mapPath` wedged the editor**: an unloadable map made PIE fail inside
  world creation, leaving `IsPlaySessionInProgress()` true with no PIE world and
  blocking every later verify. `start_verify` validates the map package up front,
  and a session with no PIE world is now reported as retryable
  `stale_pie_state` (with a queued teardown) instead of "another PIE is running".
- `start_verify` retry hints (`teardown_pending`, `stale_pie_state`) are handled
  by the M3/M4 scripts, so back-to-back sessions no longer need a cooldown.

### Verified

- UE 5.8, built-in endpoint (`:8090`): M4 golden path **PASS** (22 checks, 29 with
  the movement phase on `/ShooterCore/Maps/L_ShooterGym`), M3 suite **35/35**,
  M1/M2 suite **35/35**, base suite PASS.
- UE 5.8, official endpoint (`:8000`): M4 golden path **PASS** (11 tools
  registered), M3 suite **35/35**.
- UE 5.5 blank project: compiles with **zero changes**; M4 golden path **PASS**,
  M3 suite **35/35**, M1/M2 suite **35/35**.

### Known limitations (M4 scope)

- Reference policy classifies engine modules from a documented list (built-in
  editor modules have no runtime-readable host type); plugin modules use their
  descriptors.
- `inspect_behavior_tree` still dumps scalar params only — object references are
  reported by `inspect_references` (which has precise JSON paths).
- On a project map the run's `pass` can be false through `no_log_errors` because
  the project's own game code logs errors during PIE; the M4 golden path asserts
  movement evidence there and reports the project noise instead of hiding it.

## [2.2.1] — 2026-10-29

Repository-completeness and review fixes for M3. The `v2.2.0` tag shipped
documentation changes only; **this** release is the first one that actually
contains the runtime-verification sources and the M3 smoke suite.

### Added

- M3 sources that were missing from the repository:
  `BTForgeCore/Private/BTForgeVerifySession.{h,cpp}`,
  `BTForgeCore/Public/BTForgeVerifyLibrary.h`,
  `BTForgeCore/Private/BTForgeVerifyLibrary.cpp`, `Tools/mcp_m3_smoke.py`, the
  plugin `README.md`, and the `StartVerify`/`GetVerifyResult` declarations on
  `UBTForgeToolset` (official endpoint).
- `mcp_m3_smoke.py` now asserts the result contract itself: top-level `pass` vs
  `checks[]` vs `checksPassed/checksTotal` vs `status` (BT **and** EQS),
  `fixture.pawn`, `mapPath` echo, a `t=0` first sample, typed `blackboardInit`
  round-trip (Int/Float/Bool/Vector/String), both `Sequence` children observed,
  and a deliberately failing check (35 checks). It also deletes the previous
  result file before each start and retries `teardown_pending`.
- Result fields `checksPassed`, `checksTotal` (both kinds), `frameMaxSec`,
  `mapPath`, `fixture`, `rawItemCount` (EQS), `queryStatus`/`queryFinished` (EQS).
- `fixturePawn` option (bt, default `true`): the fixture is now an
  `AAIController` **plus** a possessed `ADefaultPawn`, so movement and
  behavior tasks are exercised on a real pawn instead of a pawn-less controller.

### Fixed

- **`pass` was not the AND of `checks[]`**: the BT result derived `pass` from
  `error`/log errors only, so a failed `running_after_start`,
  `expected_active_task_seen` or `finished_in_window` still reported
  `pass:true`; the session state was then sniffed out of the serialized JSON
  (`ResultJson.Contains("\"pass\":true\")`), which any check's `pass:true`
  satisfied. `pass` is now computed once from the check list, `status` is
  derived from that same boolean, and EQS results carry `pass` too.
- **`PostPIEStarted` boolean was misread**: `FEditorDelegates::PostPIEStarted`
  passes `bIsSimulatingInEditor` (`PlayLevel.cpp` broadcasts
  `WorldType == SimulateInEditor`), not a start-failure flag, so every Simulate
  session reported "PIE failed to start" and a real start failure was only
  caught by the 90 s deadline. Start failures are now detected by polling
  `GUnrealEd->IsPlaySessionInProgress()`; the delegate parameter is named and
  treated accordingly.
- **EQS result accuracy**: `itemCount` reports `FEnvQueryInstance::NumValidItems`
  (filtered/discarded entries no longer inflate it) with `rawItemCount` for the
  unfiltered length; the external query is unregistered after the step loop, as
  `UEnvQueryManager::RunInstantQuery` does; a null `PrepareQueryInstance` now
  produces a full failed result instead of an empty payload.
- **Lifecycle leaks**: PIE delegates are removed in every terminal path
  (`FinishSession`, `EndPIE`, `AbortToIdle`, `Shutdown`) and re-subscribed
  instead of accumulating duplicates per session; `BTForgeCore` now implements
  `ShutdownModule` and tears the verify manager down; `AbortToIdle` re-enables
  the HTTP listeners and persists its result like every other terminal path.
- **`JsonEscape` produced invalid JSON** when a value contained `\"` (quotes
  were escaped before backslashes); it now escapes backslash first and maps
  control characters.
- **`blackboardInit` silently no-opped on most key types**: every value went
  through `SetValueAsFloat/Bool/String`. It is now type-directed (Int, Float,
  Bool, Enum by name or number, Vector/Rotator from `[x,y,z]` or `{X,Y,Z}`,
  String, Name, Object/Class by asset path) and rejects a mismatched shape with
  an explicit error. Value shapes are dispatched on `EJson` type instead of
  `TryGet*` probing (`FJsonValueString::TryGetBool` returns `true` for any
  string).
- **Sub-tick trees looked like "never started"**: sampling started after the
  first 0.1 s tick; a `t=0` sample is taken right after `StartTree` and
  `running_after_start` uses that signal.
- **Duration window**: the upper bound was
  `expected + max(tolerance + 1.5, expected)`. It is now
  `expected + tolerance + clamp(3 x worst frame, 1 s, 2 s)`, and the SingleRun
  hard limit is `durationSec + 2 s` (or `expected + tolerance + slack + 2 s`).
- **Option leakage between sessions**: `FBTForgeVerifyOptions` was reused across
  `start_verify` calls, so a field omitted from the new request kept the previous
  session's value (e.g. a stale `blackboardInit`). Options are reset before each
  parse.
- **Guards**: `start_verify` refuses to start while a PIE/SIE session is already
  running (BTForge never stops a session it did not start), and `bareGameMode`
  with no `mapPath` no longer falls back to the project's current map (a Lyra map
  asserts on a bare GameMode) - it uses
  `/Engine/Maps/Templates/Template_Default` and reports the choice.
- `BTForge.uplugin` / `BTForgeOfficial.uplugin` now report 2.2.1 (they were still
  2.1/2.0 while the code said 2.2.0), and the MCP `initialize` response reports
  `2.2.1`.

### Fixed — second review pass (adversarial read of the fixed code)

- **Log capture was unsynchronized**: `GLog` (an `FOutputDeviceRedirector`)
  dispatches to devices under a *read* lock, so worker threads call
  `Serialize()` during PIE startup; `TArray::Add` racing on `Errors` corrupts the
  heap, and an unlocked copy could drop a real error and let `no_log_errors` pass
  falsely. The capture now has an `FCriticalSection` (`FScopeLock` in
  `Serialize`, locked reset/copy).
- **EQS `query_ran` was hard-coded true**: a query truncated by the 1000-step cap
  or aborted still left `NumValidItems > 0` (it is set at generation time), so it
  reported `pass:true` / `status:"done"` without ever completing. `query_ran` now
  requires `IsFinished() && IsSuccessful()`, and a hit step cap is reported.
- **"Tree started" was read from `IsRunning()`**: `StartTree` executes the first
  task synchronously, so a tree that completes immediately already reports
  `IsRunning()==false` (false failure), while a `PushInstance` failure leaves it
  true (false pass). The manager now subscribes
  `FBehaviorTreeDelegates::OnTreeStarted` for its own component and also checks
  `GetRootTree()` after the start.
- **`blackboardInit` was silently skipped when the tree has no blackboard**: the
  whole block (including the unknown-key check) was guarded by
  `Blackboard != nullptr`. A requested init without a blackboard asset is now an
  explicit failure.
- **Session ids could repeat** (`FPlatformTime::Cycles() & 0x7FFFFFF` wraps every
  ~0.7 s at 3 GHz, and clients poll an on-disk result by id): ids are now
  `V<sequence>_<cycles>`, and the smoke script clears the stale file first.
- **Own teardown tripped the "PIE already running" guard**: `RequestEndPlayMap`
  only *queues* the teardown for the next editor tick. `start_verify` now answers
  `error:"teardown_pending", retry:true` in that case instead of claiming a
  foreign session; `Shutdown` cancels a queued request
  (`CancelRequestPlaySession`) instead of leaving it to start after unload.
- **Play settings were not pinned**: a null `EditorPlaySettings` makes
  `RequestPlaySession` duplicate the project CDO, so "2 clients / separate server
  / new window" preferences produced several PIE worlds and `FindPIEWorld` could
  pick a dedicated server. The request now pins a standalone single-client
  settings object, and `FindPIEWorld` prefers a non-dedicated world.
- **`gameModeClassPath` load failures were swallowed** (the run then used the
  map's GameMode while reporting done): now an explicit error that also leaves
  the manager startable.
- **`blackboardInit` gaps**: enum keys declared by `EnumName` (EnumType null) now
  resolve, numeric enum values are range-checked, `null` clears Object/Class
  keys, and Bool accepts `"1"/"0"`.
- **Key names in `blackboardFinal` were not JSON-escaped** (a quote in a key made
  the document invalid); one `RequestEndPlayMap` call was unguarded.
- **Numeric options**: an explicit `durationSec:0`, a negative tolerance or
  `expectedDurationSec:0` is now an error instead of silently using the default
  (which had weakened the contract); `toleranceSec:0` is honored.
- The fixture now assigns `AAIController::BrainComponent`, so nodes that read
  `GetBrainComponent()` see a real component.

### Verified

- UE 5.8 built-in endpoint (`:8090`): base suite PASS, M1/M2 suite **35/35**,
  M3 suite **35/35**.
- UE 5.8 official endpoint (`:8000`): M3 suite **35/35**, 10 tools registered.
- UE 5.5 blank project (`C:\UEProject\BTForgeTest55`): the changed sources
  compile with **zero changes**; M3 suite **35/35**, M1/M2 **35/35**, base suite
  PASS (BT SingleRun/Looped, typed blackboard round-trip, aggregation negative,
  EQS itemCount=289 on the engine template map).

## [2.2.0] — 2026-10-29

### Added

- **Runtime verification (M3)**: `start_verify` + `get_verify_result` MCP tools.
  `start_verify` spawns an in-process PIE session (GameModeOverride and map
  configurable, `bareGameMode` for project-independent runs), spawns an
  AIController fixture, starts the tree (`SingleRun` finishes naturally,
  `Looped` must keep running) or executes the EQS query synchronously
  (EQSTestingPawn-style), and samples `IsRunning`/active tasks/blackboard over
  time. `get_verify_result` returns machine-readable
  `checks[{name,pass,detail}]` (pie_started, running_after_start,
  expected_active_task_seen, finished_gracefully, finished_in_window,
  kept_running_for_duration, no_log_errors), a per-0.1s execution timeline,
  final blackboard values (bt) or item count/scores (eqs), and captured
  log errors.
- **`bareGameMode` / `gameModeClassPath` / `mapPath` options** to run
  project-independent verifications on an engine template map.
- Results are persisted to `Saved/BTForgeVerify/last_result.json` (PIE teardown
  disables HTTP listeners globally; file polling keeps the result reachable).
- `Tools/mcp_m3_smoke.py`: full M3 suite (SingleRun duration-window assertion,
  Looped keep-running, unknown-key/missing-asset negatives, EQS result
  assertion) with official-endpoint handshake/name-mapping/unwrapping support.

### Verified

- UE 5.8: built-in endpoint and official endpoint — M3 suite PASS (BT
  SingleRun/Looped/negatives + EQS itemCount=289 on template map).
- UE 5.5: M3 source compiles with zero changes; full M3 suite PASS.

### Fixed

- Silent acceptance of misplaced `services`/`decorators` (on tasks/root) now
  rejected with a location error.
- `FinishedSession` state is set before `RequestEndPlayMap` and HTTP listeners
  are re-enabled after PIE teardown (the endpoint previously went dark right
  when clients polled results).

## [2.1.0] — 2026-10-29

Initial public release. Editor plugin for Unreal Engine 5.4–5.8, plus a 5.8-only
official-endpoint adapter.

### Added

- **Behavior Tree / Blackboard builder** (`create_behavior_tree_asset`,
  `build_behavior_tree`, `inspect_behavior_tree`) with composites, tasks,
  services, decorators, blackboard-key validation, asset references, and
  class-path node instantiation (`/Script/...` native or `/Game/..._C`
  Blueprint classes).
- **EQS builder** (`create_eqs_asset`, `inspect_eqs_asset`): `UEnvQuery` assets
  with options, native generators/tests, scoring fields, contexts, and project
  class paths. Editor graph is rebuilt from data on open.
- **Dry-run validation** (`validate_behavior_tree`, `validate_eqs_spec`): builds
  the whole spec into transient objects without creating assets; errors carry
  exact JSON paths.
- **Capability catalog** (`get_capability_catalog`): native + Blueprint BT node
  classes with editable parameters and JSON shape hints, EQS assets/contexts/
  generators/tests, and spec aliases derived from the parser whitelist.
- **Runtime module `BTForgeRuntime`** so generated assets reference classes that
  survive cooking; ships `BTForgeEnvQueryContext_Blackboard` (blackboard-backed
  EQS context).
- **Self-contained MCP server** (`:8090`, JSON-RPC 2.0 over HTTP) that works on
  every supported engine version, plus `BTForgeOfficial` registering the same
  tools through Epic's `ToolsetRegistry` / `ModelContextProtocol` (`:8000`, 5.8+).
- Smoke test suites: `Tools/mcp_smoke.py` (BT core/L2/L3/L3.5) and
  `Tools/mcp_m1m2_smoke.py` (EQS + catalog + validate-only; `--official` mode).

### Verified

- UE 5.8: built-in endpoint and official endpoint — 35/35 M1/M2 checks; BT core
  and L2 suites pass.
- UE 5.5: blank project compiles with zero source changes; BT core and M1/M2
  suites pass.
- UE 5.4 / 5.6 / 5.7: not tested (no local installation).

### Known limitations

- EQS contexts are class references; per-query native context configuration
  needs a Blueprint subclass (planned).
- EQS parameter arrays and the `Composite` generator are not writable yet.
- Blueprint node classes can be used but not generated (planned).
- In-engine runtime verification (PIE) is planned; current validation proves
  asset correctness only.
