# Changelog

All notable changes to BT Forge are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

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
