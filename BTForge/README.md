# BTForge

Deterministic Behavior Tree / Blackboard / EQS builder driven by AI agents over
MCP (Model Context Protocol). The agent produces a JSON spec; BTForge writes
real `.uasset` assets: Behavior Trees, Blackboards and Environment Queries.
One codebase, **UE 5.4 through 5.8**.

## Layout (two plugins)

| Plugin | Engine | What it provides |
|---|---|---|
| `BTForge/` | 5.4 – 5.8 | `BTForgeRuntime` (runtime node/context classes referenced by generated assets, no editor deps) + `BTForgeCore` (builder logic, editor-only) + `BTForgeMCP` (self-contained MCP server, original code, no Epic source) |
| `BTForgeOfficial/` | 5.8+ only | Registers the same tools on Epic's `ModelContextProtocol` server (port 8000) via `ToolsetRegistry`. Don't install on older engines. |

`BTForgeRuntime` exists because generated BT/EQS assets hard-reference node and
context classes at cook time: classes in an editor-only module would work in PIE
and break in a packaged build.

The MCP protocol layer in `BTForgeMCP` is written from the public MCP
specification (Apache-2.0). It contains no Epic engine source code, so the
plugin can be distributed publicly on any engine version (the official
Experimental plugins are `NoRedist` and cannot be redistributed).

## Tools

| Tool (BTForge server) | Official endpoint name (5.8) | Purpose |
|---|---|---|
| `create_behavior_tree_asset` | `BTForgeOfficial.BTForgeToolset.CreateBehaviorTreeAsset` | BT + Blackboard asset pair |
| `build_behavior_tree` | `...BuildBehaviorTree` | Write the tree data chain from JSON |
| `inspect_behavior_tree` | `...InspectBehaviorTree` | Read a tree back as JSON |
| `validate_behavior_tree` | `...ValidateBehaviorTree` | Build into transient objects, no asset written |
| `create_eqs_asset` | `...CreateEQSAsset` | Create a `UEnvQuery` from JSON |
| `inspect_eqs_asset` | `...InspectEQSAsset` | Read an EQS query back as JSON |
| `validate_eqs_spec` | `...ValidateEQSSpec` | Validate an EQS spec, no asset written |
| `get_capability_catalog` | `...GetCapabilityCatalog` | Inventory of node classes / EQS assets / contexts |
| `start_verify` | `...StartVerify` | Run a generated BT/EQS asset in a real in-process PIE session |
| `get_verify_result` | `...GetVerifyResult` | Poll that session: `pass` + `checks[]` + timeline + final state |
| `inspect_references` | `...InspectReferences` | Reference / content-domain report: can every reference survive a cook? |

Every tool returns JSON: `{"ok":true,...}` or `{"ok":false,"error":<message
with a JSON path>}` so the agent can self-correct. Node whitelist (L2):
composites `Sequence`/`Selector`; tasks `Wait`, `MoveTo` (blackboard-key goal +
AcceptableRadius), `RunEQS` (EQS asset reference + RunMode + result blackboard key);
blackboard key types Bool, Int, Float, String, Name, Vector, Rotator, Object.
Blackboard key names and asset references are validated before anything is written.

```json
{"type":"Selector","name":"Root","children":[
  {"type":"Sequence","name":"IdleLoop","children":[
    {"type":"Wait","name":"ShortPause","params":{"WaitTime":2.0,"RandomDeviation":1.0}}]},
  {"type":"Wait","name":"Fallback","params":{"WaitTime":5.0}}]}
```

### EQS specs

```json
{"options":[
  {"optionName":"FindCover",
   "generator":{"type":"SimpleGrid","params":{"GridSize":1500,"SpaceBetween":200}},
   "tests":[
     {"type":"Distance","params":{"TestPurpose":"Score","ScoringEquation":"InverseLinear",
                                 "ScoringFactor":1.0,
                                 "DistanceTo":"/Script/AIModule.EnvQueryContext_Querier"}}]}]}
```

Supported generators: `SimpleGrid`, `CurrentLocation`, `OnCircle`, `Donut`,
`Cone`, `PathingGrid`, `ActorsOfClass`, `PerceivedActors`; tests: `Distance`,
`Dot`, `Trace`, `Pathfinding`, `Random`, `Overlap`, `Project`, `Volume`.
Any non-abstract `UEnvQueryGenerator`/`UEnvQueryTest` class path is accepted too.
Parameter names are the exact UPROPERTY names; unknown ones are rejected with
the allowed list. `WorkOnFloatValues` is accepted as a special param (calls
`SetWorkOnFloatValues`). Runtime blackboard-backed context:
`/Script/BTForgeRuntime.BTForgeEnvQueryContext_Blackboard` (property
`BlackboardKey`; note EQS references contexts by class, so the key comes from the
class default unless a subclass overrides it).

Call `get_capability_catalog` first so class paths come from the project instead
of being guessed.

## Runtime verification (M3)

`start_verify` + `get_verify_result` run the asset for real, inside an in-process
PIE session (two calls because PIE startup spans frames; a single blocking call
would deadlock the game thread).

```json
{"kind":"bt","assetPath":"/Game/AI/BT_Sentinel","mode":"SingleRun",
 "expectedDurationSec":1.5,"toleranceSec":0.5,"durationSec":5,
 "blackboardInit":{"MoveGoal":[1000,0,100]},"fixturePawn":true,
 "bareGameMode":true,"mapPath":"/Engine/Maps/Templates/Template_Default"}
```

- **Fixture**: an `AAIController` plus (bt, `fixturePawn` default `true`) a
  possessed `ADefaultPawn`, so `MoveTo`/`RunEQS`/behavior tasks are exercised on a
  real pawn instead of failing silently on a pawn-less controller.
- **Map / GameMode**: `bareGameMode` (default `true`) overrides PIE's GameMode
  with `AGameModeBase`; because project maps (e.g. Lyra) assert on a bare
  GameMode, an empty `mapPath` then defaults to
  `/Engine/Maps/Templates/Template_Default`. Set `bareGameMode:false` (optionally
  with `gameModeClassPath`) to run the project GameMode on its own map.
- **State guard**: refuses to start while a PIE/SIE session is already running -
  BTForge only ever stops sessions it started itself. Its own teardown is queued
  for the next editor tick, so a client that starts the next session immediately
  gets `{"ok":false,"error":"teardown_pending","retry":true}` and should retry
  (the smoke script does).
- **Pinned play settings**: each request runs standalone, single client, in
  process (`EditorPlaySettings` is duplicated and pinned), so project
  multiplayer preferences cannot spawn extra PIE worlds the fixture would not be
  in.
- **Sampling**: `IsRunning`/`DescribeActiveTasks` are sampled from `t=0` (a tree
  that finishes faster than the 0.1 s tick is still seen as started);
  "started" itself comes from `FBehaviorTreeDelegates::OnTreeStarted`, not from
  `IsRunning()` (which is already false for a tree that completes inside
  `StartTree`).
- **blackboardInit** is type-directed: numeric keys take numbers, Bool takes
  booleans, Vector/Rotator take `[x,y,z]` or `{"X":n,"Y":n,"Z":n}`, Object/Class
  take asset paths, Enum takes a value name or number. A shape mismatch is an
  explicit error, never a silent no-op.
- **EQS**: runs the engine's own instant-query sequence
  (`PrepareQueryInstance` -> `RegisterExternalQuery` -> step loop ->
  `UnregisterExternalQuery`). `itemCount` is `NumValidItems` (filtered items do
  not inflate it); `rawItemCount` reports the unfiltered length.

Result contract - one source of truth, no string sniffing:

| Field | Meaning |
|---|---|
| `pass` | boolean: the AND of every `checks[]` entry |
| `status` | `done` only when `pass` is `true`, otherwise `failed` |
| `checksPassed` / `checksTotal` | counts behind `pass` (bt and eqs) |
| `checks[]` | `{name,pass,detail}`: `pie_started`, `running_after_start`, `finished_gracefully`, `finished_in_window`, `expected_active_task_seen`, `kept_running_for_duration`, `pawn_moved` (when `expectedMinPawnDisplacement` is set), `no_log_errors` |
| `samples[]` | `{t,running,tasks}` timeline from `t=0` |
| `durationSec`, `frameMaxSec` | measured run length and worst frame seen while running |
| `pawnDisplacement` | how far the fixture pawn moved between tree start and finish (bt) |
| `blackboardFinal` / `itemCount`,`topScores` | final blackboard values (bt) or query result (eqs) |
| `logErrors`, `logWarnings` | captured log lines (warnings are reported, not failed on) |
| `error` | failure reason; also present when `pass` is false because a check failed |

Optional `expectedMinPawnDisplacement` (px) turns "did the movement chain actually
run" into a check instead of something the caller has to eyeball:
`{"expectedMinPawnDisplacement": 300}` adds `pawn_moved` and fails the run when the
pawn ends up closer than that to where the tree started.

Results are also written to `Saved/BTForgeVerify/last_result.json` (PIE teardown
turns the shared HTTP listeners off globally, so file polling survives the
transition).

## Reference / content-domain policy (M4)

`inspect_references` answers one question before (or after) anything is written:
*does every object reference in this asset resolve, and can it survive a cook?*
It is read-only and works in two modes:

```json
{"assetPath":"/Game/AI/BT_Sentinel"}
{"kind":"bt","treeJson":{...},"blackboardJson":{...}}
{"kind":"eqs","queryJson":{...}}
```

- **asset mode** walks the real objects (BT node tree with services/decorators,
  blackboard keys, EQS options/generators/tests, nested struct params) and reports
  each reference with a JSON path, its kind, domain, module, plugin and verdict.
- **spec mode** scans a spec for object-path strings, so the same policy can gate a
  spec *before* `create_*` writes anything.

Verdict (mirrors the verify contract): `fail` when any reference is an error
(missing package, transient/reinstanced object, class from an editor-only module),
`warn` for engine content and unmounted plugin content roots, `ok` otherwise.

Reference fields: `jsonPath`, `ref`, `kind` (`class`/`object`/`soft-class`/
`soft-object`), `domain` (`native-module`, `project-content`, `plugin-content`,
`engine-content`), `module`, `plugin`, `editorOnly`, `blueprint`, `resolved`,
`existsOnDisk`, `consumableByGameBuild`, `reason`. Editor-only detection uses the
plugin descriptors (`Type=Editor` / `TargetAllowList` without `Game`), the
cooker's own `IsEditorOnlyObject`, and a documented list of built-in editor
modules.

## Golden path (M4)

`python Tools/mcp_m4_goldenpath.py` runs the whole agent loop deterministically:
catalog → dry-run → create EQS → inspect → reference policy → create BT (with a
deliberately broken build, then the repair) → inspect → reference policy → run in
PIE → assert `pass`, a real goal value written by RunEQS, and (with
`--map ... --expect-movement N`) that the fixture pawn actually moved to the
EQS-filtered goal. `--official` runs it through Epic's endpoint.

```bash
python Tools/mcp_m4_goldenpath.py
python Tools/mcp_m4_goldenpath.py --official
python Tools/mcp_m4_goldenpath.py --map /ShooterCore/Maps/L_ShooterGym --expect-movement 300
```

## Using the built-in server (all engine versions)

Console commands: `BTForge.MCP.StartServer [port]`, `BTForge.MCP.StopServer`,
`BTForge.MCP.Status`. Default port 8090 (Epic's server keeps 8000 on 5.8).
Auto-start opt-in via `EditorPerProjectUserSettings.ini`:

```ini
[BTForgeMCP]
bAutoStartServer=True
ServerPort=8090
```

Client config (Claude Code / ZCode style, `mcpServers`):

```json
{
  "mcpServers": {
    "btforge": {
      "type": "http",
      "url": "http://127.0.0.1:8090/mcp"
    }
  }
}
```

On 5.8 with `BTForgeOfficial` installed, the tools are additionally available
through Epic's server at `http://127.0.0.1:8000/mcp` (`ModelContextProtocol.
StartServer`), alongside the official read-only `AIModuleToolset` tools.

## How building works

The builder writes the **data only**. For BT: `UBehaviorTree::RootNode`,
`UBTCompositeNode::Children`, blackboard `Keys`. For EQS: `UEnvQuery::Options`
with inline `UEnvQueryOption`/generator/test objects. The editor graphs are left
empty on purpose: opening a BT asset auto-spawns graph nodes that reuse the data
chain instances (`UBehaviorTreeGraph::SpawnMissingNodes`, UE 4.23+), and opening
an EQS asset rebuilds its graph from `Options`
(`UEnvironmentQueryGraph::SpawnMissingNodes`). This keeps the writer free of any
graph-editing API and engine-version sensitive surface. Everything is validated
before a single UObject is written; `validate_*` tools run the same validation
into transient objects without creating assets.

## Smoke tests

```
python Tools/mcp_smoke.py                # built-in server on :8090 (BT core + optional L2/L3/L3.5 suites)
python Tools/mcp_smoke.py --objects      # also verifies JSON-object argument coercion
python Tools/mcp_smoke.py --official     # Epic server on :8000 (5.8 + BTForgeOfficial)
python Tools/mcp_m1m2_smoke.py           # EQS builder + catalog + validate-only (8 tools, :8090)
python Tools/mcp_m1m2_smoke.py --official # same suite through Epic's :8000 (5.8 + BTForgeOfficial)
python Tools/mcp_m3_smoke.py             # runtime verification in PIE (35 checks, :8090)
python Tools/mcp_m3_smoke.py --official  # same suite through Epic's :8000
python Tools/mcp_m3_smoke.py --map /ShooterCore/Maps/L_ShooterGym  # EQS on a real, nav-meshed map
python Tools/mcp_m4_goldenpath.py        # M4 golden path (catalog -> build -> references -> PIE)
python Tools/mcp_m4_goldenpath.py --official
python Tools/mcp_m4_goldenpath.py --map /ShooterCore/Maps/L_ShooterGym --expect-movement 300
```

The BT smoke script still uses fixed asset names for its base suite; pass
`--asset /Game/AI/BT_Unique` when re-running against an editor that already
contains a previous run's assets.

## Version notes

- 2.3.0 (current, M4): `inspect_references` (reference / content-domain policy),
  `expectedMinPawnDisplacement` + `pawnDisplacement`, `logWarnings` in the verify
  result, map-path validation and a `stale_pie_state` recovery for a PIE request
  that failed to create its world, `FBlackboardKeySelector` type/id resolution
  when a tree is built (so `RunEQS` results store reliably), and
  `Tools/mcp_m4_goldenpath.py`.
- 2.2.1: M3 runtime verification landed in the repository (the 2.2.0 tag shipped
  documentation only), plus the review fixes: `pass` is the AND of `checks[]` and
  `status` derives from it, `PostPIEStarted`'s boolean is treated as
  `bIsSimulatingInEditor` (start failures are detected by polling the play
  session), EQS reports `NumValidItems` and unregisters its external query,
  blackboard initialization is type-directed, and the fixture includes a pawn.
- 5.4–5.8: `BTForgeRuntime` + `BTForgeCore` + `BTForgeMCP` (zero engine-version
  conditionals; 5.5 compile + smoke verified 2026-10-28, 5.8 verified on both
  endpoints).
- 5.8+: optionally add `BTForgeOfficial` for the official endpoint.
- The MCP server binds with Epic's own posture: loopback use, Origin header
  checked against localhost for browser clients; no auth (like the official
  Experimental server, do not expose it beyond loopback).
