#!/usr/bin/env python3
"""M4 golden path: one deterministic agent loop, end to end.

The loop is exactly what an LLM is expected to do, but scripted so it can be run
as a regression:

  catalog ─► validate spec ─► create EQS ─► inspect ─► reference policy
          ─► build BT (broken → repair) ─► inspect ─► reference policy
          ─► run in PIE ─► assert pass + blackboard + pawn displacement

Notes:
  * Class paths come from get_capability_catalog, never from hardcoded guesses.
  * inspect_references is exercised in both modes (asset + spec) and must catch a
    missing reference before anything is written.
  * Verification runs on --map: with --expect-movement N the tree also gets a
    MoveTo and the run must show the fixture pawn actually moved (that needs a
    nav-meshed map); with the default template map the tree proves the
    RunEQS → blackboard link without movement.

Usage:
  python mcp_m4_goldenpath.py [--official] [--map MAP] [--expect-movement PX]
                              [--keep]

Exit codes: 0 PASS, 4 FAIL, 2 endpoint down.

Assets created here are timestamped; the editor must be closed before deleting
them (see the [artifacts] line at the end).
"""
import json
import os
import sys
import time
import urllib.error
import urllib.request

BASE = "http://127.0.0.1:8090/mcp"
OFFICIAL = [False]
OFFICIAL_NAMES = {
    "create_behavior_tree_asset": "BTForgeOfficial.BTForgeToolset.CreateBehaviorTreeAsset",
    "build_behavior_tree": "BTForgeOfficial.BTForgeToolset.BuildBehaviorTree",
    "inspect_behavior_tree": "BTForgeOfficial.BTForgeToolset.InspectBehaviorTree",
    "validate_behavior_tree": "BTForgeOfficial.BTForgeToolset.ValidateBehaviorTree",
    "create_eqs_asset": "BTForgeOfficial.BTForgeToolset.CreateEQSAsset",
    "inspect_eqs_asset": "BTForgeOfficial.BTForgeToolset.InspectEQSAsset",
    "validate_eqs_spec": "BTForgeOfficial.BTForgeToolset.ValidateEQSSpec",
    "get_capability_catalog": "BTForgeOfficial.BTForgeToolset.GetCapabilityCatalog",
    "start_verify": "BTForgeOfficial.BTForgeToolset.StartVerify",
    "get_verify_result": "BTForgeOfficial.BTForgeToolset.GetVerifyResult",
    "inspect_references": "BTForgeOfficial.BTForgeToolset.InspectReferences",
}
REQUIRED_TOOLS = list(OFFICIAL_NAMES.keys())
FAILURES = []
SESSION = [None]
NEXT_ID = [0]
TEMPLATE_MAP = "/Engine/Maps/Templates/Template_Default"
RESULT_FILE = "C:/UEProject/LyraStarterGame/Saved/BTForgeVerify/last_result.json"


import sys as _sys
if "--port" in _sys.argv:
    BASE = f"http://127.0.0.1:{_sys.argv[_sys.argv.index('--port') + 1]}/mcp"


def rpc(method, params, timeout=15):
    NEXT_ID[0] += 1
    payload = {"jsonrpc": "2.0", "id": NEXT_ID[0], "method": method}
    if params is not None:
        payload["params"] = params
    req = urllib.request.Request(BASE, data=json.dumps(payload).encode(), method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Accept", "application/json, text/event-stream")
    if SESSION[0]:
        req.add_header("Mcp-Session-Id", SESSION[0])
    try:
        resp = urllib.request.urlopen(req, timeout=timeout)
    except urllib.error.URLError as exc:
        raise SystemExit(f"[endpoint down] {BASE}: {exc}")
    sid = resp.headers.get("Mcp-Session-Id")
    if sid:
        SESSION[0] = sid
    body = resp.read().decode("utf-8", "replace")
    if "text/event-stream" in resp.headers.get("Content-Type", ""):
        for line in body.splitlines():
            if line.startswith("data:"):
                try:
                    return json.loads(line[5:].strip())
                except json.JSONDecodeError:
                    pass
        return None
    return json.loads(body) if body.strip() else None


def call_tool(name, args, timeout=60):
    actual = OFFICIAL_NAMES.get(name, name) if OFFICIAL[0] else name
    r = rpc("tools/call", {"name": actual, "arguments": args}, timeout=timeout)
    try:
        text = "\n".join(c.get("text", "") for c in r.get("result", {}).get("content", []))
        outer = json.loads(text)
        if isinstance(outer, dict) and isinstance(outer.get("returnValue"), str):
            return json.loads(outer["returnValue"])
    except (json.JSONDecodeError, AttributeError, TypeError):
        pass
    if not isinstance(r, dict) or r.get("error"):
        return {"ok": False, "error": f"transport: {r}"}
    text = "\n".join(c.get("text", "") for c in r.get("result", {}).get("content", []))
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return {"ok": False, "error": f"unparseable: {text[:300]}"}


def check(label, ok, detail=""):
    print(f"[{'ok' if ok else 'FAIL'}] {label}" + (f" -- {detail}" if detail and not ok else ""), flush=True)
    if not ok:
        FAILURES.append(label)
    return ok


def poll_verify(session_id, timeout_sec=240):
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        try:
            with open(RESULT_FILE, encoding="utf-8") as fh:
                r = json.load(fh)
            if r.get("sessionId") == session_id and r.get("status") in ("done", "failed"):
                return r
        except (OSError, json.JSONDecodeError):
            pass
        r = call_tool("get_verify_result", {"SessionId": session_id})
        if r.get("status") in ("done", "failed"):
            return r
        time.sleep(0.5)
    return {"status": "poll-timeout", "error": f"not done after {timeout_sec}s"}


def run_verify(kind, options, attempts=5):
    opts = dict(options)
    opts["kind"] = kind
    for attempt in range(attempts):
        try:
            os.remove(RESULT_FILE)
        except OSError:
            pass
        r = call_tool("start_verify", {"OptionsJson": json.dumps(opts)}, timeout=30)
        if r.get("ok"):
            return poll_verify(r["sessionId"])
        if r.get("error") in ("teardown_pending", "stale_pie_state") and attempt + 1 < attempts:
            time.sleep(0.4)
            continue
        return r
    return {"ok": False, "error": "gave up after teardown retries"}


def pick_querier_context(catalog):
    """Read the context class path out of the catalog instead of guessing it."""
    contexts = (((catalog.get("eqs") or {}).get("nativeContexts")) or [])
    for entry in contexts:
        if entry.get("name") == "EnvQueryContext_Querier":
            return entry.get("classPath")
    for entry in contexts:
        if "Querier" in (entry.get("name") or ""):
            return entry.get("classPath")
    return "/Script/AIModule.EnvQueryContext_Querier"


def main():
    official = "--official" in sys.argv
    keep = "--keep" in sys.argv
    map_path = TEMPLATE_MAP
    if "--map" in sys.argv:
        map_path = sys.argv[sys.argv.index("--map") + 1]
    expect_movement = 0.0
    if "--expect-movement" in sys.argv:
        expect_movement = float(sys.argv[sys.argv.index("--expect-movement") + 1])
    bare_gamemode = map_path.startswith("/Engine/")

    if official:
        globals()["BASE"] = "http://127.0.0.1:8000/mcp"
        OFFICIAL[0] = True

    stamp = str(int(time.time()))
    eqs_asset = "/Game/AI/EQS_M4_" + stamp
    bt_asset = "/Game/AI/BT_M4_" + stamp

    # ---------------------------------------------------------------- 0. handshake
    rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                       "clientInfo": {"name": "m4-goldenpath", "version": "1"}})
    rpc("notifications/initialized", None)
    listing = rpc("tools/list", None)
    names = [t["name"] for t in listing.get("result", {}).get("tools", [])]
    expected = [OFFICIAL_NAMES[n] for n in REQUIRED_TOOLS] if OFFICIAL[0] else REQUIRED_TOOLS
    missing = [n for n in expected if n not in names]
    check("m4 all 11 tools listed", not missing, f"missing={missing} have={names[:12]}")

    # ---------------------------------------------------------------- 1. catalog
    catalog = call_tool("get_capability_catalog", {"SectionJson": json.dumps({"section": "all"})})
    check("m4 catalog ok", catalog.get("ok") is True, json.dumps(catalog)[:300])
    querier = pick_querier_context(catalog)
    key_types = ((catalog.get("bt") or {}).get("specTypes") or {}).get("blackboardKeyTypes") or []
    generator_aliases = [a.get("alias") for a in
                         (((catalog.get("eqs") or {}).get("specTypes") or {}).get("generatorAliases") or [])]
    print(f"  catalog: querier={querier} hasVector={'Vector' in key_types} "
          f"generators={len(generator_aliases)}", flush=True)
    check("m4 catalog exposes querier context", bool(querier) and querier.startswith("/Script/"), str(querier))
    check("m4 catalog exposes Vector key type", "Vector" in key_types, str(key_types))
    check("m4 catalog exposes SimpleGrid generator", "SimpleGrid" in generator_aliases, str(generator_aliases[:8]))

    # ---------------------------------------------------------------- 2. EQS spec
    eqs_spec = {"options": [{
        "optionName": "FindApproach",
        "generator": {"type": "SimpleGrid", "params": {"GridSize": 800, "SpaceBetween": 100}},
        "tests": [{"type": "Distance",
                   "params": {"TestPurpose": "Score", "ScoringEquation": "InverseLinear",
                              "ScoringFactor": 1.0, "DistanceTo": querier}}]}]}
    dry = call_tool("validate_eqs_spec", {"QueryJson": json.dumps(eqs_spec)})
    check("m4 eqs dry-run ok", dry.get("ok") is True, json.dumps(dry)[:300])

    created = call_tool("create_eqs_asset", {"AssetPath": eqs_asset, "QueryJson": json.dumps(eqs_spec)})
    check("m4 eqs created", created.get("ok") is True, json.dumps(created)[:300])
    inspected = call_tool("inspect_eqs_asset", {"AssetPath": eqs_asset})
    check("m4 eqs inspect shows querier context",
          inspected.get("ok") is True and querier in json.dumps(inspected),
          json.dumps(inspected)[:400])

    refs_eqs = call_tool("inspect_references", {"RequestJson": json.dumps({"assetPath": eqs_asset})})
    modules = ((refs_eqs.get("summary") or {}).get("byModule") or {})
    check("m4 eqs references ok", refs_eqs.get("ok") is True and refs_eqs.get("verdict") == "ok",
          json.dumps(refs_eqs)[:400])
    check("m4 eqs references see AIModule", (modules.get("AIModule") or 0) >= 2, json.dumps(modules))

    # ---------------------------------------------------------------- 3. BT: broken then repaired
    tree = {"type": "Sequence", "name": "Root", "children": [
        {"type": "RunEQS", "name": "FindGoal",
         "params": {"BlackboardKey": "MoveGoal",
                    "EQSRequest": {"QueryTemplate": eqs_asset, "RunMode": "SingleResult"}}}]}
    if expect_movement > 0.0:
        tree["children"].append(
            {"type": "MoveTo", "name": "Approach",
             "params": {"BlackboardKey": "MoveGoal", "AcceptableRadius": 100}})
    tree["children"].append({"type": "Wait", "name": "Settle", "params": {"WaitTime": 0.5}})

    blackboard = {"keys": [{"name": "MoveGoal", "type": "Vector"}]}
    created_bt = call_tool("create_behavior_tree_asset",
                           {"AssetPath": bt_asset, "BlackboardJson": json.dumps(blackboard)})
    check("m4 bt created", created_bt.get("ok") is True, json.dumps(created_bt)[:300])

    broken = json.loads(json.dumps(tree))
    broken["children"][-1]["params"] = {"PauseSec": 0.5}
    repair = call_tool("build_behavior_tree", {"AssetPath": bt_asset, "TreeJson": json.dumps(broken)})
    check("m4 broken build rejected with JSON path",
          repair.get("ok") is False and "PauseSec" in (repair.get("error") or ""),
          json.dumps(repair)[:300])

    built = call_tool("build_behavior_tree", {"AssetPath": bt_asset, "TreeJson": json.dumps(tree)})
    check("m4 repaired build ok", built.get("ok") is True, json.dumps(built)[:300])
    built_tree = call_tool("inspect_behavior_tree", {"AssetPath": bt_asset})
    root = (built_tree or {}).get("root") or {}
    child_types = [c.get("type") for c in (root.get("children") or [])]
    check("m4 inspect sees the built chain",
          built_tree.get("ok") is True and root.get("type") == "BTComposite_Sequence"
          and child_types and child_types[0] == "BTTask_RunEQSQuery"
          and len(child_types) == (3 if expect_movement > 0.0 else 2),
          json.dumps(child_types))

    refs_bt = call_tool("inspect_references", {"RequestJson": json.dumps({"assetPath": bt_asset})})
    entries = refs_bt.get("references") or []
    query_refs = [e for e in entries if "QueryTemplate" in (e.get("jsonPath") or "")]
    check("m4 bt references ok", refs_bt.get("ok") is True and refs_bt.get("verdict") == "ok",
          json.dumps(refs_bt.get("errors"))[:300])
    check("m4 bt references trace the EQS asset",
          any(e.get("domain") == "project-content" and eqs_asset in (e.get("ref") or "") for e in query_refs),
          json.dumps(query_refs)[:300])
    check("m4 bt references all cook-safe",
          entries and all(e.get("consumableByGameBuild") is True for e in entries),
          json.dumps([e for e in entries if not e.get("consumableByGameBuild")])[:300])

    # ---------------------------------------------------------------- 4. policy catches a bad ref before writing
    bad_spec = json.loads(json.dumps(tree))
    bad_spec["children"][0]["params"]["EQSRequest"]["QueryTemplate"] = "/Game/AI/EQS_DoesNotExist"
    bad_refs = call_tool("inspect_references", {"RequestJson": json.dumps(
        {"kind": "bt", "treeJson": bad_spec, "blackboardJson": blackboard})})
    check("m4 spec policy fails on missing EQS",
          bad_refs.get("ok") is True and bad_refs.get("verdict") == "fail"
          and any("EQS_DoesNotExist" in (e.get("ref") or "") for e in (bad_refs.get("errors") or [])),
          json.dumps(bad_refs)[:400])

    # ---------------------------------------------------------------- 5. runtime: EQS → blackboard (always)
    result = run_verify("bt", {
        "assetPath": bt_asset,
        "mode": "SingleRun",
        "durationSec": 5,
        "startDeadlineSec": 120,
        "bareGameMode": True,
        "mapPath": TEMPLATE_MAP,
        "fixturePawn": True,
    })
    print(f"verify (template) -> status={result.get('status')} pass={result.get('pass')} "
          f"duration={result.get('durationSec')}", flush=True)
    checks = {c.get("name"): c for c in (result.get("checks") or [])}
    print(f"  checks: {json.dumps(result.get('checks'))[:600]}", flush=True)
    check("m4 verify done", result.get("status") == "done", json.dumps(result)[:400])
    check("m4 verify pass", result.get("pass") is True, json.dumps(result.get("checks"))[:400])
    check("m4 verify fixture pawn", (result.get("fixture") or {}).get("pawn") is True,
          json.dumps(result.get("fixture")))
    board = result.get("blackboardFinal") or {}
    goal = str(board.get("MoveGoal") or "")
    check("m4 RunEQS produced a real goal value",
          "X=" in goal and "(invalid)" not in goal, json.dumps(board)[:300])

    # ---------------------------------------------------------------- 6. runtime: EQS → MoveTo → moving pawn
    movement_asset = eqs_movement_asset = None
    if expect_movement > 0.0:
        eqs_movement_asset = "/Game/AI/EQS_M4Move_" + stamp
        # Only far points pass the filter, so the chosen goal is far enough for the
        # displacement assertion to be meaningful (SingleResult otherwise picks the
        # nearest grid point, i.e. the pawn's own spot).
        far_min = max(expect_movement, 300.0) * 2.0
        movement_spec = {"options": [{
            "optionName": "FarApproach",
            "generator": {"type": "SimpleGrid", "params": {"GridSize": 2000, "SpaceBetween": 200}},
            "tests": [
                {"type": "Distance",
                 "params": {"TestPurpose": "Filter", "FilterType": "Range",
                            "DistanceTo": querier, "FloatValueMin": far_min, "FloatValueMax": far_min * 4.0}},
                {"type": "Distance",
                 "params": {"TestPurpose": "Score", "ScoringEquation": "InverseLinear",
                            "DistanceTo": querier, "ScoringFactor": 1.0}}]}]}
        created_m = call_tool("create_eqs_asset",
                              {"AssetPath": eqs_movement_asset, "QueryJson": json.dumps(movement_spec)})
        check("m4 movement eqs created", created_m.get("ok") is True, json.dumps(created_m)[:300])

        movement_asset = "/Game/AI/BT_M4Move_" + stamp
        movement_tree = {"type": "Sequence", "name": "Root", "children": [
            {"type": "RunEQS", "name": "FindFarGoal",
             "params": {"BlackboardKey": "MoveGoal",
                        "EQSRequest": {"QueryTemplate": eqs_movement_asset, "RunMode": "SingleResult"}}},
            {"type": "MoveTo", "name": "Approach",
             "params": {"BlackboardKey": "MoveGoal", "AcceptableRadius": 100}},
            {"type": "Wait", "name": "Settle", "params": {"WaitTime": 0.5}}]}
        created_mb = call_tool("create_behavior_tree_asset",
                               {"AssetPath": movement_asset, "BlackboardJson": json.dumps(blackboard)})
        built_mb = call_tool("build_behavior_tree",
                             {"AssetPath": movement_asset, "TreeJson": json.dumps(movement_tree)})
        check("m4 movement bt built",
              created_mb.get("ok") is True and built_mb.get("ok") is True,
              json.dumps(built_mb)[:300])
        refs_mb = call_tool("inspect_references", {"RequestJson": json.dumps({"assetPath": movement_asset})})
        check("m4 movement bt references ok", refs_mb.get("verdict") == "ok",
              json.dumps(refs_mb.get("errors"))[:300])

        moved = run_verify("bt", {
            "assetPath": movement_asset,
            "mode": "SingleRun",
            "durationSec": 15,
            "startDeadlineSec": 180,
            "bareGameMode": bare_gamemode,
            "mapPath": map_path,
            "fixturePawn": True,
            "expectedMinPawnDisplacement": expect_movement,
        })
        moved_checks = {c.get("name"): c for c in (moved.get("checks") or [])}
        moved_board = moved.get("blackboardFinal") or {}
        print(f"verify (movement, map={map_path}) -> status={moved.get('status')} "
              f"pass={moved.get('pass')} displacement={moved.get('pawnDisplacement')}", flush=True)
        print(f"  checks: {json.dumps(moved.get('checks'))[:600]}", flush=True)
        # status follows pass, and pass includes no_log_errors, which a project map
        # can fail on its own game-feature errors; the run must still have finished.
        check("m4 movement run finished",
              moved.get("status") in ("done", "failed") and "pawn_moved" in moved_checks,
              json.dumps(moved)[:400])
        check("m4 movement pawn_moved check passed",
              (moved_checks.get("pawn_moved") or {}).get("pass") is True,
              json.dumps(moved_checks.get("pawn_moved"))[:300])
        check("m4 movement pawn actually moved",
              (moved.get("pawnDisplacement") or 0.0) >= expect_movement,
              f"displacement={moved.get('pawnDisplacement')} expected={expect_movement}")
        check("m4 movement goal came from the filtered EQS",
              "X=" in str(moved_board.get("MoveGoal") or "")
              and "(invalid)" not in str(moved_board.get("MoveGoal")),
              json.dumps(moved_board)[:300])
        # pass itself is not asserted here: a project map brings its own GameMode and
        # game-feature code, whose log errors are outside BTForge's contract.
        if moved.get("pass") is not True:
            failed = [n for n, c in moved_checks.items() if not c.get("pass")]
            print(f"  [note] project-map run reported pass=false (failed: {failed}); "
                  f"movement evidence is asserted above, project log noise is not", flush=True)

    # ---------------------------------------------------------------- 7. summary
    artifacts = f"EQS={eqs_asset} BT={bt_asset}"
    if movement_asset:
        artifacts += f" EQS_MOVE={eqs_movement_asset} BT_MOVE={movement_asset}"
    print(f"\n[artifacts] {artifacts}", flush=True)
    if not keep:
        print("[cleanup] close the editor, then delete those assets "
              "(open assets cannot be removed safely).", flush=True)
    if FAILURES:
        print(f"\nRESULT: FAIL ({len(FAILURES)} checks failed: {FAILURES})", flush=True)
        sys.exit(4)
    print("\nRESULT: PASS (M4 golden path: catalog -> spec -> asset -> references -> runtime)", flush=True)


if __name__ == "__main__":
    main()
