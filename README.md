# BT Forge

**Deterministic Behavior Tree / Blackboard / EQS builder for Unreal Engine, driven by AI agents over MCP.**

BT Forge turns a JSON spec (written by an LLM or a human) into real, editable Unreal assets:

- **Behavior Trees + Blackboards** — composites, tasks, services, decorators, blackboard-key references, asset references, and project Blueprint node classes.
- **Environment Queries (EQS)** — options with native generators/tests, scoring, contexts, and project class paths.
- **Validation and discovery** — dry-run validation with exact JSON paths, plus a capability catalog so an agent picks real project classes instead of guessing them.

Engine target: **UE 5.4 – 5.8**, zero engine-version conditionals. Verified on **5.5** (blank project) and **5.8** (Lyra; built-in server and Epic's official MCP server).

> 中文文档见 [README.zh-CN.md](README.zh-CN.md).

---

## Why this exists

Behavior Tree assets are normally authored by hand in the editor. That is slow, hard to review, and hard for an AI agent to do reliably. Runtime-LLM-driven NPCs are not a good fit either (latency, non-determinism, combat must be reviewable/designable).

The workflow this project targets:

```
design document / natural language intent
        │  (LLM composes a spec, guided by the capability catalog)
        ▼
   JSON spec ──► BT Forge ──► real .uasset assets (BT / BB / EQS)
        ▲                        │
        └── inspect / validate ◄──┘        (feedback loop with exact JSON paths)
```

The agent stays a **planner and spec author**; the deterministic writer owns asset creation; a validator/inspector owns correctness feedback. This keeps generated AI reviewable, diffable via git, and reproducible.

---

## Features

### Eleven MCP tools

| Tool | Purpose |
|---|---|
| `create_behavior_tree_asset` | Create a Blackboard + BehaviorTree asset pair |
| `build_behavior_tree` | Write the BT data chain from a JSON spec |
| `inspect_behavior_tree` | Read a tree back as JSON (structure + scalar params) |
| `validate_behavior_tree` | Validate a tree + blackboard spec without creating assets |
| `create_eqs_asset` | Create a `UEnvQuery` (EQS) asset from a JSON spec |
| `inspect_eqs_asset` | Read an EQS query back as JSON (options/generators/tests/params) |
| `validate_eqs_spec` | Validate an EQS spec without creating assets |
| `get_capability_catalog` | Inventory of available BT node classes, EQS assets/contexts, spec aliases and JSON parameter shapes |
| `inspect_references` | Reference / content-domain policy: every reference's domain and whether it can survive a cook (asset or spec mode) |
| `start_verify` | Runtime verification: starts an in-process PIE session and actually runs a generated BT/EQS asset, sampling execution over time |
| `get_verify_result` | Polls a verify session; returns machine-readable checks (started/running/finished-in-window/no-log-errors), execution timeline and final blackboard/EQS results |

### Spec coverage

- **BT nodes**: composites `Sequence`/`Selector`; tasks `Wait`, `MoveTo`, `RunEQS`; services `RunEQS`; decorator `Blackboard`. Any non-abstract `UBTCompositeNode` / `UBTTaskNode` / `UBTService` / `UBTDecorator` **class path** is also accepted — including project Blueprint classes such as `/Game/AI/BTS_Shoot.BTS_Shoot_C`.
- **Blackboard keys**: Bool, Int, Float, String, Name, Vector, Rotator, Object.
- **EQS generators**: `SimpleGrid`, `CurrentLocation`, `OnCircle`, `Donut`, `Cone`, `PathingGrid`, `ActorsOfClass`, `PerceivedActors` (+ any non-abstract `UEnvQueryGenerator` class path).
- **EQS tests**: `Distance`, `Dot`, `Trace`, `Pathfinding`, `Random`, `Overlap`, `Project`, `Volume` (+ any non-abstract `UEnvQueryTest` class path).
- **Parameters** are written through Unreal reflection. Unknown parameter names are rejected with the allowed list; enum values are validated with the full value list; class references are loaded and type-checked; `FAIDataProvider*` wrappers accept a bare number for ergonomics.

### Design guarantees

- **Validate before create**: the whole spec is built into transient objects first. Any error means nothing is written (or the just-created asset is deleted).
- **Errors are actionable**: `{"ok":false,"error":"<json path>: <reason>"}` so an agent can repair the spec without guessing.
- **Data-only writes**: BT/EQS editor graphs are left empty on purpose and rebuilt by the engine when the asset is opened, so the writer stays free of graph-editing APIs and version-sensitive editor code.
- **Two MCP endpoints**: a self-contained HTTP server on `:8090` for any engine version, and (5.8+) an adapter that registers the same tools on Epic's official `ModelContextProtocol` server on `:8000`.
- **No Epic source code**: the MCP layer is an original implementation of the public MCP specification; the official Experimental plugins are not copied or redistributed.

---

## Architecture

### Repository layout

```
BTForge/                      ← plugin: UE 5.4 – 5.8
├── BTForge.uplugin
├── Source/
│   ├── BTForgeRuntime/       Runtime: node/context classes referenced by generated assets
│   ├── BTForgeCore/          Editor: BT/BB/EQS writers, catalog, validation
│   └── BTForgeMCP/           Editor: self-contained MCP server (JSON-RPC 2.0 over HTTP)
└── Tools/                    smoke tests (python)

BTForgeOfficial/              ← plugin: 5.8+ only
└── Source/BTForgeOfficial/   Adapter: registers the same tools via ToolsetRegistry
```

### Module view

```
                 5.8 only
        ┌─────────────────────────┐
        │ BTForgeOfficial         │───► Epic ModelContextProtocol (:8000)
        │ (UToolsetDefinition)    │
        └──────────┬──────────────┘
                   │ delegates
┌──────────────────▼──────────────┐
│ BTForgeCore (Editor)            │  specs → assets; inspect; validate; catalog
└───────┬─────────────────┬───────┘
        │                 │
┌───────▼───────┐  ┌──────▼─────────────┐
│ BTForgeMCP    │  │ BTForgeRuntime     │  classes referenced BY the generated
│ (:8090)       │  │ (Runtime module)   │  assets (must exist in cooked builds)
└───────────────┘  └────────────────────┘
```

**Why a Runtime module?** Generated BT/EQS assets hard-reference node and context classes at cook time. Classes that only live in an Editor module work in PIE and then break in a packaged build. The correct split is "who is referenced" (Runtime), not "who creates" (Editor).

**Why two plugins?** The 5.8 official toolset API (`UToolsetDefinition`, `ToolsetRegistry`, `ModelContextProtocol`) does not exist on older engines, and Epic's plugin source is `NoRedist`. Keeping the adapter in a separate plugin means: older engines just don't install it, and the main plugin stays dependency-free and distributable.

### How writing works

- **BT/BB**: writes `UBehaviorTree::RootNode`, `UBTCompositeNode::Children`, blackboard `Keys`, service/decorator arrays. Opening the asset auto-spawns graph nodes from the data chain (`UBehaviorTreeGraph::SpawnMissingNodes`).
- **EQS**: writes `UEnvQuery::Options` with inline `UEnvQueryOption` + generator/test objects. Opening the asset rebuilds the EQS graph from `Options` (`UEnvironmentQueryGraph::SpawnMissingNodes`).
- **Parameters**: reflection writers for numbers, booleans, strings, names, enums, nested structs, class references and object references, with type checks and precise errors.

### Workflow

```
natural language / design doc
   → LLM calls get_capability_catalog (real class paths, parameter JSON shapes)
   → LLM writes spec
   → validate_* (no assets) or create_* + build_*
   → inspect_* (verify what was written)
   → start_verify + get_verify_result (run it in PIE; pass = AND of checks[])
   → behavior tree used by AI controllers in game
```

---

## Quick start

### 1. Install the plugin

Copy `BTForge/` into `<YourProject>/Plugins/BTForge/`. On UE 5.8, also copy `BTForgeOfficial/` into `<YourProject>/Plugins/BTForgeOfficial/`.

Enable **BT Forge** (and **BT Forge Official** on 5.8) in the Plugins window.

### 2. Start the MCP server

Console commands:

| Command | Meaning |
|---|---|
| `BTForge.MCP.StartServer [port]` | Start the built-in server (default `8090`) |
| `BTForge.MCP.StopServer` | Stop it |
| `BTForge.MCP.Status` | Report state |

Optional auto-start in `<Project>/Saved/Config/WindowsEditor/EditorPerProjectUserSettings.ini`:

```ini
[BTForgeMCP]
bAutoStartServer=True
ServerPort=8090
```

### 3. Point your MCP client at it

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

On 5.8 with `BTForgeOfficial`, the same tools are also exposed through Epic's server at `http://127.0.0.1:8000/mcp`.

### 4. Verify

```bash
python BTForge/Tools/mcp_smoke.py             # BT core suite
python BTForge/Tools/mcp_smoke.py --l2        # + MoveTo/RunEQS write path
python BTForge/Tools/mcp_m1m2_smoke.py        # EQS builder + catalog + validate-only (35 checks)
python BTForge/Tools/mcp_m1m2_smoke.py --official  # same suite through Epic's :8000
python BTForge/Tools/mcp_m3_smoke.py          # runtime verification in PIE (34 checks)
python BTForge/Tools/mcp_m3_smoke.py --official    # same suite through Epic's :8000
python BTForge/Tools/mcp_m3_smoke.py --map /ShooterCore/Maps/L_ShooterGym  # EQS on a nav-meshed map
python BTForge/Tools/mcp_m4_goldenpath.py        # M4 golden path (catalog -> references -> PIE)
python BTForge/Tools/mcp_m4_goldenpath.py --official
python BTForge/Tools/mcp_m4_goldenpath.py --map /ShooterCore/Maps/L_ShooterGym --expect-movement 300
python BTForge/Tools/mcp_m5_nodes.py             # M5 native node library (catalog -> specs -> 8 PIE runs)
python BTForge/Tools/mcp_m5_nodes.py --official
python BTForge/Tools/mcp_m6_bp.py                # M6 blueprint node generation (creation -> catalog -> PIE)
python BTForge/Tools/mcp_m6_bp.py --official
```

---

## Spec reference (short)

### Behavior Tree

```json
{
  "type": "Selector", "name": "Root",
  "children": [
    {"type": "Sequence", "name": "Patrol", "children": [
      {"type": "RunEQS", "name": "FindGoal",
       "params": {"BlackboardKey": "MoveGoal",
                  "EQSRequest": {"QueryTemplate": "/Game/AI/EQS_FindWeapon",
                                 "RunMode": "SingleResult"}}},
      {"type": "MoveTo", "params": {"BlackboardKey": "MoveGoal", "AcceptableRadius": 100}},
      {"type": "Wait", "params": {"WaitTime": 3.0, "RandomDeviation": 1.0}}
    ]}
  ]
}
```

Services and decorators attach structurally:

```json
{"type": "Sequence", "services": [{"type": "RunEQS", "params": {"EQSRequest": {"QueryTemplate": "/Game/AI/EQS_AIPerceptionEnemy"}}}],
 "children": [{"type": "MoveTo", "decorators": [{"type": "Blackboard", "params": {"BlackboardKey": "TargetEnemy"}}]}]}
```

### EQS

```json
{
  "options": [{
    "optionName": "FindCover",
    "generator": {"type": "SimpleGrid",
                  "params": {"GridSize": 1500, "SpaceBetween": 200}},
    "tests": [{
      "type": "Distance",
      "params": {"TestPurpose": "Score",
                 "ScoringEquation": "InverseLinear",
                 "ScoringFactor": 1.0,
                 "DistanceTo": "/Script/AIModule.EnvQueryContext_Querier"}
    }]
  }]
}
```

Notes that matter in practice:

- `TestPurpose` defaults to `FilterAndScore` with a `Range[0,0]` filter — a pure scoring test must set `TestPurpose: "Score"`, otherwise all items are filtered out.
- Parameter names are exact UPROPERTY names. Call `get_capability_catalog` to see names, C++ types, and **JSON shapes** (`boolean`, `number`, `enum: ...`, `class path (...)`).
- EQS references contexts by **class**, not by instance. `BTForgeRuntime` ships `BTForgeEnvQueryContext_Blackboard` (`/Script/BTForgeRuntime.BTForgeEnvQueryContext_Blackboard`); its `BlackboardKey` value comes from the class default (per-query key configuration needs a Blueprint subclass — see roadmap).

### Native node library (M5)

`BTForgeRuntime` ships seven parameterized native nodes — the common behaviors that
used to require Blueprint authoring. They appear in `get_capability_catalog` with
full parameter schemas and accept short aliases in specs:

| Alias | Class | What it does |
|---|---|---|
| `ComputeValue` | `BTForgeTask_ComputeValue` | `LeftKey` <op> (`RightKey` or `RightConstant`) → `ResultKey`; ops Add/Subtract/Multiply/Divide/Min/Max; Float/Int keys |
| `Log` | `BTForgeTask_LogMessage` | checkpoint message (+ one key value) to `LogBTForge`; `Severity` `Error` deliberately fails a verify run, `Warning` shows up in `logWarnings` |
| `SetValue` (service) | `BTForgeService_SetBlackboardValue` | periodic typed write: per-type constants, or a copy of any compatible `SourceKey` |
| `UpdateDistance` (service) | `BTForgeService_UpdateDistance` | periodic `dist(SourceKey, TargetKey)` (Object-actor or Vector keys) → Float `ResultKey` |
| `FloatCompare` (decorator) | `BTForgeDecorator_FloatCompare` | `BlackboardKey` <op> (`CompareToKey` or `CompareToValue`), six operators; observes its keys and aborts the branch on change |
| `DistanceCheck` (decorator) | `BTForgeDecorator_DistanceCheck` | range gate `dist(SourceKey, TargetKey)` in `[MinDistance, MaxDistance]`; a negative bound disables that side; no bounds = fail-closed |
| `Chance` (decorator) | `BTForgeDecorator_Chance` | passes with `Probability` percent (exact at 0/100) |

Example — keep a `DistToTarget` key fresh and gate an attack branch on it:

```json
{"type": "Selector", "services": [
   {"type": "UpdateDistance", "params": {"SourceKey": "SelfActor", "TargetKey": "TargetEnemy", "ResultKey": "DistToTarget"}}],
 "children": [
   {"type": "Sequence",
    "decorators": [{"type": "FloatCompare", "params": {"BlackboardKey": "DistToTarget", "Operator": "LessOrEqual", "CompareToValue": 800}}],
    "children": [{"type": "MoveTo", "params": {"BlackboardKey": "TargetEnemy", "AcceptableRadius": 150}}]},
   {"type": "Log", "params": {"Message": "target out of range", "Severity": "Warning"}}]}
```

Engine-native nodes (the engine's own `Set-key` tasks on 5.6+ take key-bound
values via `{"Value": {"Key": "SomeKey"}}`) and Blueprint node classes appear in
the same catalog and mix freely with these aliases.

### Blueprint node generation (M6)

`create_node_blueprint` generates a data-only Blueprint node asset from a spec:
a compiled `/Game/..._C` class derived from a BT node base class, with
per-project defaults on the class CDO — no Blueprint authoring, no event graph.
The BTForge node classes are `Blueprintable`, so they are natural parents:

```json
{"AssetPath": "/Game/AI/BTA_LogShot",
 "ParentClassPath": "/Script/BTForgeRuntime.BTForgeTask_LogMessage",
 "ParamsJson": "{\"Message\":\"shot fired\",\"Severity\":\"Warning\"}"}
```

The returned `classPath` works as a node `type` in `build_behavior_tree`, the
capability catalog lists the new class automatically, and PIE verification
proves the defaults drive runtime behavior. Engine `*BlueprintBase` classes are
also valid parents; engine-native BT classes are not `Blueprintable` and are
rejected with an explanation.

---

## Development progress

| Milestone | Status | Notes |
|---|---|---|
| v0 — toolset-based BT builder | ✅ 2026-10-20 | 3 tools registered via Epic's `ToolsetRegistry` (5.8) |
| Cross-version architecture | ✅ 2026-10-23 | Self-contained MCP server; `BTForge` + `BTForgeOfficial` split; UE 5.4–5.8 target |
| UE 5.5 verification | ✅ 2026-10-24 | Blank project compiles with zero source changes; found two 5.8-masked bugs (startup timing, HTTP listener flag) |
| L1 — inspect readback | ✅ 2026-10-27 | Services/decorators/structs readback; crash forensics for reflection pointer arithmetic |
| L2 — write expansion | ✅ 2026-10-27 | `MoveTo`, `RunEQS`; key/reference/enum/struct/array writers |
| L3 — services & decorators | ✅ 2026-10-28 | `Blackboard` decorator, `RunEQS` service; AND semantics via engine-native empty ops |
| L3.5 — Blueprint node classes | ✅ 2026-10-28 | `type` accepts `/Game/..._C` and `/Script/...`; base-class/abstract validation |
| **M1 — EQS builder** | ✅ 2026-10-28 | `UEnvQuery` creation from spec; runtime module split; runtime blackboard context |
| **M2 — catalog + validate-only** | ✅ 2026-10-28 | Capability catalog, dry-run validation for BT and EQS, 8 tools total |
| **M3 — runtime verification** | ✅ 2026-10-29 | In-process PIE: `StartTree(SingleRun/Looped)` + execution timeline sampling; EQS instant query with result assertions; two-call async protocol; 2.2.1 hardens `pass` aggregation, PIE start semantics, EQS valid-item counting and the pawn fixture |
| **M4 — golden path E2E** | ✅ 2026-10-30 | `inspect_references` (reference / content-domain policy, asset + spec mode), `pawnDisplacement` + `expectedMinPawnDisplacement`, `Tools/mcp_m4_goldenpath.py`: catalog → spec → asset → references → PIE, with a broken-build → repair loop and an opt-in nav-meshed movement phase. 11 tools |
| **M5 — native node library** | ✅ 2026-10-30 | 7 parameterized native service/decorator/task classes in `BTForgeRuntime` (ComputeValue, Log, SetValue, UpdateDistance, FloatCompare, DistanceCheck, Chance) + spec aliases exported as the catalog's authoritative tables; `Tools/mcp_m5_nodes.py` verifies every behavior in PIE. 11 tools |
| **M6 — Blueprint node generation (part 1)** | ✅ 2026-10-30 | `create_node_blueprint`: data-only Blueprint node assets (compiled `_C` class + CDO defaults, no event graph), precise parent validation, catalog auto-listing, PIE-verified CDO behavior; `Tools/mcp_m6_bp.py`. 12 tools |
| M6 — Blueprint node generation (part 2) | ⏳ planned | Constrained event-graph recipes |

### Verification matrix (current release)

| Environment | Result |
|---|---|
| UE 5.8, built-in server (`:8090`) | BT core suite PASS; M1/M2 suite **35/35 PASS**; M3 runtime suite **35/35 PASS**; M4 golden path **PASS** (22 checks, 29 with movement); M5 node-library suite **PASS** (8 PIE behavior runs); M6 blueprint-generation suite **PASS** (2 PIE runs) |
| UE 5.8, official MCP server (`:8000`) | M1/M2 suite **35/35 PASS**; M3 runtime suite **35/35 PASS**; M4 golden path **PASS**; M5 + M6 suites **PASS**; 12 tools registered in the official toolset |
| UE 5.5, blank project | compiles with zero source changes; M1/M2 suite **35/35 PASS**; M3 runtime suite **35/35 PASS**; M4 golden path **PASS**; M5 + M6 suites **PASS** |
| UE 5.4 / 5.6 / 5.7 | not yet tested (APIs chosen for stability; no version conditionals) |

---

## Known limitations

- **EQS contexts are class references** (`TSubclassOf<UEnvQueryContext>`). Per-query configuration of a native context (e.g. `BlackboardKey`) needs a Blueprint subclass carrying the class defaults. Planned as an M6 first step.
- **EQS parameter arrays are not writable yet** (e.g. `Composite` generator's child generators); non-empty arrays are rejected with a clear error.
- **Blueprint node classes can be used but not created yet** — `build_behavior_tree` instantiates existing `_C` classes (L3.5); generating BTS Blueprint assets is M6.
- **Reference policy scope** (M4): built-in engine modules are classified from a documented editor-module list (they expose no runtime-readable host type); plugin modules use their descriptors, and `IsEditorOnlyObject` backs both up. `/Engine` content is reported as a warning, not an error.
- **`pass` on a project map can be false from the project's own log errors** (`no_log_errors` counts everything logged during PIE, e.g. Lyra's GameplayCue script errors). The M4 golden path therefore asserts movement evidence on a project map and reports that noise instead of hiding it.
- **`inspect_behavior_tree` still reports scalar params only**; object references inside structs (e.g. `EQSRequest.QueryTemplate`) are reported by `inspect_references` with exact JSON paths.- **Runtime verification has boundaries** (M3): `start_verify` needs the editor process and runs one session at a time - it refuses to start while another PIE/SIE session is running and only ever stops sessions it started. With `bareGameMode` and no `mapPath` it falls back to the engine template map (project maps such as Lyra assert on a bare GameMode). The fixture spawns an `ADefaultPawn` by default (`fixturePawn`), but navigation tasks still need a NavMesh in the target map. The EQS path is a single synchronous step loop, so asynchronous generators are not exercised.
- **Blueprint node entries in the catalog have no parameter schema** (only native classes do), to avoid loading every Blueprint CDO.
- **Unmounted GameFeature/plugin content may be missing from the catalog** (AssetRegistry sees mounted content; the tool reports a note about this).
- **5.4/5.6/5.7 are untested.** Only 5.5 and 5.8 were available on the development machine.

---

## License

[MIT](LICENSE) — free to use in commercial and personal projects.
