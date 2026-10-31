#!/usr/bin/env python3
"""M3 runtime-verification smoke: actually run generated assets in PIE.

Requires a running editor (Lyra on 5.8 recommended) with the BTForge server up.
The EQS case needs a map; pass --map to pin one (e.g. /ShooterMaps/Maps/L_ShooterGym).

Contract under test (v2.2.0 hardening):
  * every terminal result carries a top-level "pass" boolean = AND of checks[]
  * "status" is "done" only when "pass" is true ("failed" otherwise)
  * checksPassed/checksTotal agree with that boolean
  * BT fixture = AIController + possessed Pawn (fixture.pawn)
  * samples start at t=0, mapPath is echoed back
  * blackboardInit is type-directed (Int/Float/Bool/Vector/String keys)
  * EQS itemCount is NumValidItems (rawItemCount reports the unfiltered length)

Usage:
  python mcp_m3_smoke.py [--official] [--map /ShooterMaps/Maps/L_ShooterGym]

Exit codes: 0 PASS, 4 FAIL, 2 endpoint down.
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
}
FAILURES = []
SESSION = [None]
NEXT_ID = [0]
TEMPLATE_MAP = "/Engine/Maps/Templates/Template_Default"


def rpc(method, params, timeout=8):
    NEXT_ID[0] += 1
    payload = {"jsonrpc": "2.0", "id": NEXT_ID[0], "method": method}
    if params is not None:
        payload["params"] = params
    req = urllib.request.Request(BASE, data=json.dumps(payload).encode(), method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Accept", "application/json, text/event-stream")
    if SESSION[0]:
        req.add_header("Mcp-Session-Id", SESSION[0])
    resp = urllib.request.urlopen(req, timeout=timeout)
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


import sys as _sys
if "--port" in _sys.argv:
    BASE = "http://127.0.0.1:%s/mcp" % _sys.argv[_sys.argv.index("--port") + 1]


def call_tool(name, args, timeout=30):
    actual = OFFICIAL_NAMES.get(name, name) if OFFICIAL[0] else name
    r = rpc("tools/call", {"name": actual, "arguments": args}, timeout=timeout)
    # Official endpoint wraps FString returns as {"returnValue": "<json>"}.
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
        return {"ok": False, "error": f"unparseable: {text[:200]}"}


RESULT_FILE = "C:/UEProject/LyraStarterGame/Saved/BTForgeVerify/last_result.json"


def poll_verify(session_id, timeout_sec=75):
    """Prefer the on-disk result (the HTTP endpoint can go dark around PIE
    transitions); fall back to polling the endpoint."""
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


def check(label, ok, detail=""):
    print(f"[{'ok' if ok else 'FAIL'}] {label}" + (f" -- {detail}" if detail and not ok else ""), flush=True)
    if not ok:
        FAILURES.append(label)
    return ok


def run_verify(kind, options, timeout_sec=240, attempts=5):
    opts = dict(options)
    opts["kind"] = kind
    for attempt in range(attempts):
        # A stale result file from a previous run must never satisfy this poll
        # (the C++ side also makes session ids unique per process).
        try:
            os.remove(RESULT_FILE)
        except OSError:
            pass
        r = call_tool("start_verify", {"OptionsJson": json.dumps(opts)}, timeout=30)
        if r.get("ok"):
            return poll_verify(r["sessionId"], timeout_sec)
        # Our own teardown is queued for the next editor tick; retry shortly.
        if r.get("error") == "teardown_pending" and attempt + 1 < attempts:
            time.sleep(0.4)
            continue
        return r
    return {"ok": False, "error": "gave up after teardown retries"}


def failed_checks(result):
    return [c.get("name") for c in result.get("checks", []) if not c.get("pass")]


def check_contract(label, result):
    """pass/status/checksPassed must all tell the same story."""
    checks = result.get("checks") or []
    all_pass = all(c.get("pass") for c in checks) if checks else False
    is_bool = isinstance(result.get("pass"), bool)
    has_counts = isinstance(result.get("checksPassed"), int) and isinstance(result.get("checksTotal"), int)
    consistent = (
        is_bool
        and has_counts
        and checks
        and result.get("pass") == all_pass
        and result.get("checksPassed") == result.get("checksTotal")
        and (result.get("status") == "done") == bool(result.get("pass"))
    )
    check(f"{label}: pass/status aggregate", consistent,
          f"pass={result.get('pass')} status={result.get('status')} "
          f"checksPassed={result.get('checksPassed')}/{result.get('checksTotal')} "
          f"checks={json.dumps(checks)[:300]}")


def main():
    official = "--official" in sys.argv
    map_path = TEMPLATE_MAP
    if "--map" in sys.argv:
        map_path = sys.argv[sys.argv.index("--map") + 1]
    if official:
        globals()["BASE"] = "http://127.0.0.1:8000/mcp"
        OFFICIAL[0] = True

    # Official server requires full handshake before tools/call.
    rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                       "clientInfo": {"name": "m3smoke", "version": "1"}})
    rpc("notifications/initialized", None)

    # --- 1. BT SingleRun: two waits, duration assertion --------------------
    asset1 = "/Game/AI/BT_M3_Run1_" + str(int(time.time()))
    r = call_tool("create_behavior_tree_asset", {
        "AssetPath": asset1,
        "BlackboardJson": json.dumps({"keys": []})})
    check("m3 create ok", r.get("ok") is True, json.dumps(r)[:200])
    tree1 = {"type": "Sequence", "name": "Root", "children": [
        {"type": "Wait", "name": "W1", "params": {"WaitTime": 0.5}},
        {"type": "Wait", "name": "W2", "params": {"WaitTime": 1.0}}]}
    r = call_tool("build_behavior_tree", {"AssetPath": asset1, "TreeJson": json.dumps(tree1)})
    check("m3 build ok", r.get("ok") is True, json.dumps(r)[:200])

    result = run_verify("bt", {
        "assetPath": asset1, "mode": "SingleRun",
        "expectedDurationSec": 1.5, "toleranceSec": 0.5,
        "startDeadlineSec": 90, "durationSec": 5,
        "bareGameMode": True, "mapPath": TEMPLATE_MAP})
    print(f"bt SingleRun -> status={result.get('status')} pass={result.get('pass')} "
          f"duration={result.get('durationSec')} frameMax={result.get('frameMaxSec')}", flush=True)
    check("m3 bt SingleRun done", result.get("status") == "done", json.dumps(result)[:300])
    check("m3 bt SingleRun pass", result.get("pass") is True,
          json.dumps(result.get("checks"))[:300])
    check_contract("m3 bt SingleRun", result)
    check("m3 bt fixture pawn", (result.get("fixture") or {}).get("pawn") is True,
          json.dumps(result.get("fixture")))
    check("m3 bt map echoed", str(result.get("mapPath", "")).endswith("Template_Default"),
          str(result.get("mapPath")))
    samples = result.get("samples") or []
    check("m3 bt samples from t=0", bool(samples) and samples[0].get("t") == 0,
          json.dumps(samples[:1]))
    # durationSec is reported verbatim (PIE cold-start inflates wall clock);
    # the window assertion lives server-side as finished_in_window.
    seen_raw = result.get("activeTasksSeen", [])
    seen = " ".join(x if isinstance(x, str) else json.dumps(x) for x in seen_raw)
    # Both children of the Sequence must actually have ticked: accepting either one
    # (or the generic BTTask_Wait) would hide a broken sequence advance.
    check("m3 bt W1 and W2 both seen", "W1" in seen and "W2" in seen, seen)

    # --- 2. BT Looped: must keep running for the window --------------------
    result = run_verify("bt", {
        "assetPath": asset1, "mode": "Looped", "durationSec": 3.0,
        "startDeadlineSec": 90,
        "bareGameMode": True, "mapPath": TEMPLATE_MAP})
    print(f"bt Looped -> status={result.get('status')} pass={result.get('pass')}", flush=True)
    check("m3 bt Looped done", result.get("status") == "done", json.dumps(result)[:300])
    check("m3 bt Looped kept running", result.get("pass") is True,
          json.dumps(result.get("checks"))[:300])
    check_contract("m3 bt Looped", result)

    # --- 3. Typed blackboardInit (Int/Float/Bool/Vector/String) ------------
    asset_bb = "/Game/AI/BT_M3_BB_" + str(int(time.time()))
    r = call_tool("create_behavior_tree_asset", {
        "AssetPath": asset_bb,
        "BlackboardJson": json.dumps({"keys": [
            {"name": "Count", "type": "Int"},
            {"name": "Ratio", "type": "Float"},
            {"name": "Flag", "type": "Bool"},
            {"name": "Point", "type": "Vector"},
            {"name": "Label", "type": "String"}]})})
    check("m3 bb create ok", r.get("ok") is True, json.dumps(r)[:300])
    r = call_tool("build_behavior_tree", {"AssetPath": asset_bb, "TreeJson": json.dumps(
        {"type": "Sequence", "name": "Root", "children": [
            {"type": "Wait", "name": "W", "params": {"WaitTime": 0.3}}]})})
    check("m3 bb build ok", r.get("ok") is True, json.dumps(r)[:300])
    result = run_verify("bt", {
        "assetPath": asset_bb, "mode": "SingleRun", "durationSec": 5,
        "expectedDurationSec": 0.3, "toleranceSec": 0.3,
        "blackboardInit": {"Count": 7, "Ratio": 1.5, "Flag": True,
                           "Point": [10, 20, 30], "Label": "hello"},
        "startDeadlineSec": 90, "bareGameMode": True, "mapPath": TEMPLATE_MAP})
    final = result.get("blackboardFinal") or {}
    print(f"blackboardInit typed -> {json.dumps(final)}", flush=True)
    check("m3 bb typed init pass", result.get("pass") is True,
          json.dumps(result.get("checks"))[:400])
    check("m3 bb int key", "7" in str(final.get("Count")), json.dumps(final))
    check("m3 bb float key", "1.5" in str(final.get("Ratio")), json.dumps(final))
    check("m3 bb bool key", str(final.get("Flag")).lower().startswith("true"), json.dumps(final))
    check("m3 bb vector key", all(tok in str(final.get("Point")) for tok in ("10", "20", "30")),
          json.dumps(final))
    check("m3 bb string key", "hello" in str(final.get("Label")), json.dumps(final))

    # --- 4. Negatives: bad blackboard key / nonexistent asset -------------
    result = run_verify("bt", {
        "assetPath": asset1, "mode": "SingleRun", "durationSec": 3,
        "blackboardInit": {"NoSuchKey": 1}, "startDeadlineSec": 90,
        "bareGameMode": True, "mapPath": TEMPLATE_MAP})
    print(f"bt negative key -> {json.dumps(result)[:160]}", flush=True)
    check("m3 bt negative key fails", result.get("status") == "failed"
          and "unknown key" in result.get("error", ""), json.dumps(result)[:300])
    check("m3 bt negative key pass=false", result.get("pass") is False, json.dumps(result)[:200])

    result = run_verify("bt", {
        "assetPath": asset_bb, "mode": "SingleRun", "durationSec": 5,
        "blackboardInit": {"Count": "not-a-number"},
        "startDeadlineSec": 90, "bareGameMode": True, "mapPath": TEMPLATE_MAP})
    print(f"bt negative type -> {json.dumps(result)[:200]}", flush=True)
    check("m3 bt negative type fails", result.get("status") == "failed"
          and "expects a number" in result.get("error", ""), json.dumps(result)[:300])

    r = call_tool("start_verify", {"OptionsJson": json.dumps({
        "kind": "bt", "assetPath": "/Game/AI/BT_M3_DoesNotExist"})})
    check("m3 negative asset fails sync", r.get("ok") is False
          and "asset not found" in r.get("error", ""), json.dumps(r)[:200])

    # --- 5. A failing check MUST flip pass/status (aggregation bug) -------
    result = run_verify("bt", {
        "assetPath": asset1, "mode": "SingleRun", "durationSec": 5,
        "expectedDurationSec": 0.2, "toleranceSec": 0.1,
        "startDeadlineSec": 90, "bareGameMode": True, "mapPath": TEMPLATE_MAP})
    bad = failed_checks(result)
    print(f"aggregation -> pass={result.get('pass')} status={result.get('status')} "
          f"failed={bad}", flush=True)
    check("m3 aggregate pass=false", result.get("pass") is False
          and result.get("status") == "failed", json.dumps(result)[:300])
    check("m3 aggregate consistency", bool(bad)
          and result.get("checksPassed", 99) < result.get("checksTotal", 0),
          json.dumps(result)[:300])
    check("m3 aggregate window/graceful reported",
          "finished_in_window" in bad or "finished_gracefully" in bad, str(bad))

    # --- 6. EQS: SimpleGrid generator on the real map ----------------------
    eqs_asset = "/Game/AI/EQS_M3_" + str(int(time.time()))
    eqs_spec = {"options": [{
        "optionName": "Grid",
        "generator": {"type": "SimpleGrid",
                      "params": {"GridSize": 800, "SpaceBetween": 100}},
        "tests": [{"type": "Distance",
                   "params": {"TestPurpose": "Score", "ScoringEquation": "InverseLinear",
                              "ScoringFactor": 1.0,
                              "DistanceTo": "/Script/AIModule.EnvQueryContext_Querier"}}]}]}
    r = call_tool("create_eqs_asset", {"AssetPath": eqs_asset, "QueryJson": json.dumps(eqs_spec)})
    check("m3 eqs create ok", r.get("ok") is True, json.dumps(r)[:200])

    result = run_verify("eqs", {
        "assetPath": eqs_asset, "startDeadlineSec": 90,
        "bareGameMode": True, "mapPath": map_path})
    print(f"eqs -> status={result.get('status')} pass={result.get('pass')} "
          f"itemCount={result.get('itemCount')} raw={result.get('rawItemCount')}", flush=True)
    check("m3 eqs done", result.get("status") == "done", json.dumps(result)[:300])
    check("m3 eqs pass true", result.get("pass") is True, json.dumps(result.get("checks"))[:300])
    check_contract("m3 eqs", result)
    check("m3 eqs has results", (result.get("itemCount") or 0) > 0,
          "0 items - navmesh/world context may be missing")
    check("m3 eqs item counts agree",
          (result.get("itemCount") or 0) <= (result.get("rawItemCount") or 0),
          f"itemCount={result.get('itemCount')} raw={result.get('rawItemCount')}")

    # --- 7. Negative: EQS asset that does not exist ------------------------
    r = call_tool("start_verify", {"OptionsJson": json.dumps({
        "kind": "eqs", "assetPath": "/Game/AI/EQS_M3_Missing"})})
    check("m3 eqs negative asset fails", r.get("ok") is False
          and "asset not found" in r.get("error", ""), json.dumps(r)[:200])

    if FAILURES:
        print(f"\nRESULT: FAIL ({len(FAILURES)} checks failed: {FAILURES})", flush=True)
        sys.exit(4)
    print("\nRESULT: PASS (M3 runtime verification suite)", flush=True)


if __name__ == "__main__":
    main()
