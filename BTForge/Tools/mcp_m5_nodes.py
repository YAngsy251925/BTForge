#!/usr/bin/env python3
"""M5 native node library: catalog, spec aliases, and PIE behavior checks.

The M5 deliverable is a library of parameterized native BT nodes shipped in
BTForgeRuntime (no Blueprint authoring needed). This suite proves:

  catalog   - all 7 classes appear as native nodes with editable params, and
              their spec aliases are listed in specTypes (from the same tables
              the parser accepts, so catalog and whitelist cannot drift);
  specs     - validate-only dry-runs accept the aliases (including nested
              params and the engine's own Set-key task used as an interop
              control), and reject unknown aliases/keys with precise errors;
  runtime   - one PIE run per behavior:
              S1 ComputeValue -> Log -> engine Set-key copy   (blackboard)
              S2 FloatCompare branch selection                (blackboard)
              S3 Chance 100% / 0% branches                    (blackboard)
              S4 UpdateDistance service + DistanceCheck gate  (blackboard)
              S5 FloatCompare threshold not met -> fallback   (blackboard)
              S6 deliberate Severity=Error log fails the run  (logErrors)
              S7 SetValue service periodic write              (blackboard)
              plus a Severity=Warning run that must NOT fail (logWarnings).

Usage:
  python mcp_m5_nodes.py [--official] [--result-file PATH] [--keep]

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
    "inspect_references": "BTForgeOfficial.BTForgeToolset.InspectReferences",
}
REQUIRED_TOOLS = list(OFFICIAL_NAMES.keys())
FAILURES = []
SESSION = [None]
NEXT_ID = [0]
TEMPLATE_MAP = "/Engine/Maps/Templates/Template_Default"
RESULT_FILE = "C:/UEProject/LyraStarterGame/Saved/BTForgeVerify/last_result.json"

RT = "/Script/BTForgeRuntime."
# Catalog "name" values are UClass::GetName() (U/A prefix stripped).
M5_CLASSES = {
    "BTForgeTask_ComputeValue": "task",
    "BTForgeTask_LogMessage": "task",
    "BTForgeService_SetBlackboardValue": "service",
    "BTForgeService_UpdateDistance": "service",
    "BTForgeDecorator_FloatCompare": "decorator",
    "BTForgeDecorator_DistanceCheck": "decorator",
    "BTForgeDecorator_Chance": "decorator",
}


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


def run_verify(options, attempts=5):
    opts = dict(options)
    opts.setdefault("kind", "bt")
    opts.setdefault("mode", "SingleRun")
    opts.setdefault("durationSec", 8)
    opts.setdefault("startDeadlineSec", 120)
    opts.setdefault("bareGameMode", True)
    opts.setdefault("mapPath", TEMPLATE_MAP)
    opts.setdefault("fixturePawn", True)
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


def assert_run(label_prefix, result, expect_pass=True, board_expect=None, log_hints=None):
    """Shared assertions for one PIE run. board_expect: {key: value} on
    blackboardFinal (numeric with tolerance). log_hints: (any_error_substring,
    any_warning_substring)."""
    checks = {c.get("name"): c for c in (result.get("checks") or [])}
    print(f"  -> status={result.get('status')} pass={result.get('pass')} "
          f"checksPassed={result.get('checksPassed')}/{result.get('checksTotal')}", flush=True)
    check(f"{label_prefix} run finished", result.get("status") in ("done", "failed"), json.dumps(result)[:400])
    if expect_pass:
        check(f"{label_prefix} pass", result.get("pass") is True,
              json.dumps(result.get("checks"))[:500])
    board = result.get("blackboardFinal") or {}
    for key, expected in (board_expect or {}).items():
        got = board.get(key)
        try:
            got_num = float(got)
            ok = abs(got_num - float(expected)) <= 0.51
        except (TypeError, ValueError):
            ok = str(got) == str(expected)
        check(f"{label_prefix} blackboard {key}=={expected}", ok,
              f"got={got!r} board={json.dumps(board)[:300]}")
    if log_hints:
        err_hint, warn_hint = log_hints
        if err_hint:
            check(f"{label_prefix} logErrors contain '{err_hint}'",
                  any(err_hint in e for e in (result.get("logErrors") or [])),
                  json.dumps(result.get("logErrors"))[:400])
        if warn_hint:
            check(f"{label_prefix} logWarnings contain '{warn_hint}'",
                  any(warn_hint in w for w in (result.get("logWarnings") or [])),
                  json.dumps(result.get("logWarnings"))[:400])
    return result


def main():
    official = "--official" in sys.argv
    keep = "--keep" in sys.argv
    global RESULT_FILE
    if "--result-file" in sys.argv:
        RESULT_FILE = sys.argv[sys.argv.index("--result-file") + 1]

    if official:
        globals()["BASE"] = "http://127.0.0.1:8000/mcp"
        OFFICIAL[0] = True

    stamp = str(int(time.time()))
    assets = {}

    # ---------------------------------------------------------------- 0. handshake
    rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                       "clientInfo": {"name": "m5-nodes", "version": "1"}})
    rpc("notifications/initialized", None)
    listing = rpc("tools/list", None)
    names = [t["name"] for t in listing.get("result", {}).get("tools", [])]
    expected = [OFFICIAL_NAMES[n] for n in REQUIRED_TOOLS] if OFFICIAL[0] else REQUIRED_TOOLS
    missing = [n for n in expected if n not in names]
    check("m5 all 11 tools listed", not missing, f"missing={missing} have={names[:12]}")

    # ---------------------------------------------------------------- 1. catalog
    catalog = call_tool("get_capability_catalog", {"SectionJson": json.dumps({"section": "bt"})})
    check("m5 catalog ok", catalog.get("ok") is True, json.dumps(catalog)[:300])
    bt = catalog.get("bt") or {}
    spec_types = bt.get("specTypes") or {}
    aliases = {
        "nodeTypes": [a.get("alias") for a in (spec_types.get("nodeTypes") or [])],
        "serviceTypes": [a.get("alias") for a in (spec_types.get("serviceTypes") or [])],
        "decoratorTypes": [a.get("alias") for a in (spec_types.get("decoratorTypes") or [])],
    }
    check("m5 specTypes nodeTypes list M5 aliases",
          "ComputeValue" in aliases["nodeTypes"] and "Log" in aliases["nodeTypes"],
          str(aliases["nodeTypes"]))
    check("m5 specTypes serviceTypes list M5 aliases",
          "SetValue" in aliases["serviceTypes"] and "UpdateDistance" in aliases["serviceTypes"],
          str(aliases["serviceTypes"]))
    check("m5 specTypes decoratorTypes list M5 aliases",
          all(a in aliases["decoratorTypes"] for a in ("FloatCompare", "DistanceCheck", "Chance")),
          str(aliases["decoratorTypes"]))

    native_nodes = {e.get("name"): e for e in (bt.get("nativeNodes") or [])}
    missing_classes = [name for name in M5_CLASSES if name not in native_nodes]
    check("m5 catalog lists all 7 M5 classes", not missing_classes,
          f"missing={missing_classes}")
    wrong_kind = [n for n, kind in M5_CLASSES.items()
                  if n in native_nodes and native_nodes[n].get("kind") != kind]
    check("m5 M5 classes classified correctly", not wrong_kind, str(wrong_kind))
    check("m5 M5 classes carry classPath",
          all((native_nodes.get(n) or {}).get("classPath", "").startswith(RT)
              for n in M5_CLASSES if n in native_nodes),
          json.dumps({n: (native_nodes.get(n) or {}).get("classPath") for n in M5_CLASSES}))

    def params_of(name):
        return {p.get("name"): p for p in ((native_nodes.get(name) or {}).get("editableParams") or [])}

    # The engine's Set-key task family is new in UE 5.6+; the interop control in
    # S1 is only included when the catalog shows the engine actually has it.
    has_engine_setkey = "BTTask_SetKeyValueFloat" in native_nodes

    compute_params = params_of("BTForgeTask_ComputeValue")
    check("m5 ComputeValue param schema",
          all(k in compute_params for k in ("Operation", "LeftKey", "RightKey", "RightConstant", "ResultKey")),
          str(sorted(compute_params)))
    check("m5 ComputeValue Operation json shape is an enum",
          "Add" in (compute_params.get("Operation") or {}).get("json", "")
          and "Max" in (compute_params.get("Operation") or {}).get("json", ""),
          str((compute_params.get("Operation") or {}).get("json")))
    check("m5 selector params advertise key-name shape",
          "blackboard key name" in (compute_params.get("LeftKey") or {}).get("json", ""),
          str((compute_params.get("LeftKey") or {}).get("json")))
    floatcmp_params = params_of("BTForgeDecorator_FloatCompare")
    check("m5 FloatCompare param schema",
          all(k in floatcmp_params for k in ("BlackboardKey", "Operator", "CompareToKey", "CompareToValue")),
          str(sorted(floatcmp_params)))
    check("m5 Chance Probability clamped number",
          (params_of("BTForgeDecorator_Chance").get("Probability") or {}).get("type") == "double",
          str(params_of("BTForgeDecorator_Chance").get("Probability")))

    # ---------------------------------------------------------------- 2. spec dry-runs
    blackboard = {"keys": [
        {"name": "A", "type": "Float"},
        {"name": "B", "type": "Float"},
        {"name": "Result", "type": "Float"},
        {"name": "Echo", "type": "Float"},
        {"name": "Mark", "type": "Int"},
        {"name": "SelfPt", "type": "Vector"},
        {"name": "FarPt", "type": "Vector"},
        {"name": "Dist", "type": "Float"},
    ]}

    def set_int(mark_value, source_key=None):
        params = {"BlackboardKey": "Mark"}
        params["Value"] = {"Key": source_key} if source_key else mark_value
        return {"type": "/Script/AIModule.BTTask_SetKeyValueInt32", "name": "SetMark", "params": params}

    def mark_seq(mark_value, decorators=None, wait=0.5):
        """Branch that marks its execution by writing Mark while it runs. Uses
        the M5 SetValue service (available on every supported engine version),
        not the 5.6+-only engine Set-key tasks."""
        node = {"type": "Sequence",
                "services": [{"type": "SetValue", "params": {
                    "KeyToSet": "Mark", "Constant": mark_value, "Interval": 0.1}}],
                "children": [{"type": "Wait", "params": {"WaitTime": wait}}]}
        if decorators:
            node["decorators"] = decorators
        return node

    s1_children = [
        {"type": "ComputeValue", "name": "Add", "params": {
            "LeftKey": "A", "Operation": "Add", "RightConstant": 40, "ResultKey": "Result"}},
        {"type": "Log", "name": "Checkpoint", "params": {
            "Message": "m5 computed", "KeyToLog": "Result", "Severity": "Log"}},
    ]
    if has_engine_setkey:
        # interop control: the engine's own Set-key task with a key-bound
        # FValueOrBBKey value (Float source -> Float destination).
        s1_children.append({"type": "/Script/AIModule.BTTask_SetKeyValueFloat",
                            "name": "SetEcho",
                            "params": {"BlackboardKey": "Echo", "Value": {"Key": "Result"}}})
    s1_children.append({"type": "Wait", "params": {"WaitTime": 0.3}})
    s1_tree = {"type": "Sequence", "name": "Root", "children": s1_children,
               "externalKeys": {"A": "blackboardInit"}}
    dry = call_tool("validate_behavior_tree", {"TreeJson": json.dumps(s1_tree),
                                               "BlackboardJson": json.dumps(blackboard)})
    check("m5 dry-run S1 (alias ComputeValue/Log + engine Set-key)",
          dry.get("ok") is True, json.dumps(dry)[:400])

    # interop control proves the *existing* generic writer handles the engine's
    # FValueOrBBKey nested struct: Value written as a key-bound object.
    check("m5 dry-run reports node count",
          dry.get("nodeCount") == (5 if has_engine_setkey else 4), str(dry.get("nodeCount")))

    bad_alias = json.loads(json.dumps(s1_tree))
    bad_alias["children"][0]["type"] = "Nonsense"
    bad = call_tool("validate_behavior_tree", {"TreeJson": json.dumps(bad_alias),
                                               "BlackboardJson": json.dumps(blackboard)})
    check("m5 unknown alias rejected", bad.get("ok") is False and "Nonsense" in (bad.get("error") or ""),
          json.dumps(bad)[:300])

    bad_key = json.loads(json.dumps(s1_tree))
    bad_key["children"][0]["params"]["LeftKey"] = "NoSuchKey"
    bad = call_tool("validate_behavior_tree", {"TreeJson": json.dumps(bad_key),
                                               "BlackboardJson": json.dumps(blackboard)})
    check("m5 unknown blackboard key rejected with key list",
          bad.get("ok") is False and "NoSuchKey" in (bad.get("error") or ""),
          json.dumps(bad)[:300])

    s4_dry_tree = {"type": "Selector", "name": "Root",
                   "services": [{"type": "UpdateDistance", "params": {
                       "SourceKey": "SelfPt", "TargetKey": "FarPt", "ResultKey": "Dist"}}],
                   "children": [
        mark_seq(5, decorators=[{"type": "DistanceCheck", "params": {
            "SourceKey": "SelfPt", "TargetKey": "FarPt", "MinDistance": 100, "MaxDistance": 1000}}]),
        mark_seq(6),
    ]}
    s4_dry_tree["externalKeys"] = {"SelfPt": "blackboardInit", "FarPt": "blackboardInit"}
    dry = call_tool("validate_behavior_tree", {"TreeJson": json.dumps(s4_dry_tree),
                                               "BlackboardJson": json.dumps(blackboard)})
    check("m5 dry-run S4 (service on composite, decorator on child)",
          dry.get("ok") is True, json.dumps(dry)[:400])

    # ---------------------------------------------------------------- 2b. key dataflow analysis
    df_bb = {"keys": [{"name": "Gate", "type": "Float"}, {"name": "Orphan", "type": "Int"}]}
    dead_tree = {"type": "Selector", "children": [
        {"type": "Sequence", "decorators": [{"type": "FloatCompare", "params": {
            "BlackboardKey": "Gate", "Operator": "Less", "CompareToValue": 10}}],
         "children": [{"type": "Wait", "params": {"WaitTime": 0.2}}]}]}
    r = call_tool("validate_behavior_tree", {"TreeJson": json.dumps(dead_tree),
                                             "BlackboardJson": json.dumps(df_bb)})
    df = r.get("dataflow") or {}
    check("m5 dataflow dead gate fails validation", r.get("ok") is False and "dead gate" in str(r.get("error", "")),
          json.dumps(r)[:300])
    check("m5 dataflow dead gate listed with path", len(df.get("deadGates") or []) == 1
          and "Gate" in json.dumps(df.get("deadGates")), json.dumps(df.get("deadGates"))[:200])

    exempt_tree = json.loads(json.dumps(dead_tree))
    exempt_tree["externalKeys"] = {"Gate": "written by the spawner before RunBehaviorTree"}
    r = call_tool("validate_behavior_tree", {"TreeJson": json.dumps(exempt_tree),
                                             "BlackboardJson": json.dumps(df_bb)})
    df = r.get("dataflow") or {}
    gate_report = next((k for k in (df.get("keys") or []) if k.get("name") == "Gate"), {})
    check("m5 dataflow externalKeys exempts the gate", r.get("ok") is True and gate_report.get("external") is True,
          json.dumps(r)[:300])

    orphan_tree = {"type": "Sequence", "services": [{"type": "SetValue", "params": {
        "KeyToSet": "Orphan", "Constant": 3, "Interval": 0.5}}],
        "children": [{"type": "Wait", "params": {"WaitTime": 0.2}}]}
    r = call_tool("validate_behavior_tree", {"TreeJson": json.dumps(orphan_tree),
                                             "BlackboardJson": json.dumps(df_bb)})
    df = r.get("dataflow") or {}
    check("m5 dataflow orphan writer is a warning, build still ok",
          r.get("ok") is True and len(df.get("orphanWriters") or []) == 1,
          json.dumps(df.get("orphanWriters"))[:200])

    scope_tree = {"type": "Selector", "children": [
        {"type": "Sequence", "decorators": [{"type": "FloatCompare", "params": {
            "BlackboardKey": "Orphan", "Operator": "Less", "CompareToValue": 10}}],
         "children": [{"type": "Wait", "params": {"WaitTime": 0.2}}]},
        {"type": "Sequence", "services": [{"type": "SetValue", "params": {
            "KeyToSet": "Orphan", "Constant": 3, "Interval": 0.1}}],
         "children": [{"type": "Wait", "params": {"WaitTime": 0.2}}]},
    ]}
    r = call_tool("validate_behavior_tree", {"TreeJson": json.dumps(scope_tree),
                                             "BlackboardJson": json.dumps(df_bb)})
    df = r.get("dataflow") or {}
    check("m5 dataflow sibling-branch writer raises scope warning",
          r.get("ok") is True and len(df.get("scopeWarnings") or []) >= 1,
          json.dumps(df.get("scopeWarnings"))[:300])

    # inspect readback: bound key names are visible again
    insp_tree = {"type": "Sequence", "children": [
        {"type": "ComputeValue", "params": {"LeftKey": "Gate", "Operation": "Add",
                                            "RightConstant": 1, "ResultKey": "Orphan"}}]}
    r = call_tool("validate_behavior_tree", {"TreeJson": json.dumps(insp_tree),
                                             "BlackboardJson": json.dumps(df_bb)})
    # (inspect readback is asserted on a real asset below, after S1 builds)

    # ---------------------------------------------------------------- 3. PIE runs
    def create_and_build(tag, tree):
        asset = f"/Game/AI/BT_M5{tag}_{stamp}"
        created = call_tool("create_behavior_tree_asset",
                            {"AssetPath": asset, "BlackboardJson": json.dumps(blackboard)})
        if not created.get("ok"):
            check(f"m5 S{tag} create", False, json.dumps(created)[:300])
            return asset
        built = call_tool("build_behavior_tree", {"AssetPath": asset, "TreeJson": json.dumps(tree)})
        check(f"m5 S{tag} build", built.get("ok") is True, json.dumps(built)[:300])
        assets[tag] = asset
        return asset

    # S1: compute 10 + 40 -> Result, log it, engine Set-key copies Result to Echo.
    asset = create_and_build(1, s1_tree)
    result = run_verify({"assetPath": asset, "blackboardInit": {"A": 10},
                         "expectedActiveTask": "Wait"})
    s1_board = {"Result": 50}
    if has_engine_setkey:
        s1_board["Echo"] = 50
    assert_run("m5 S1", result, board_expect=s1_board)
    # selector readback (re-enabled in P1): inspect shows the bound key names
    insp = call_tool("inspect_behavior_tree", {"AssetPath": asset})
    insp_params = json.dumps(insp)
    check("m5 inspect readback shows bound key names",
          '"LeftKey": "A"' in insp_params and '"ResultKey": "Result"' in insp_params,
          insp_params[:300])

    # S2: FloatCompare (Result >= 50) picks the first branch.
    s2_tree = {"type": "Selector", "name": "Root", "children": [
        mark_seq(1, decorators=[{"type": "FloatCompare", "params": {
            "BlackboardKey": "Result", "Operator": "GreaterOrEqual", "CompareToValue": 50}}]),
        mark_seq(2),
    ], "externalKeys": {"Result": "blackboardInit"}}
    asset = create_and_build(2, s2_tree)
    result = run_verify({"assetPath": asset, "blackboardInit": {"Result": 50}})
    assert_run("m5 S2", result, board_expect={"Mark": 1})

    # S3: Chance 100% passes, Chance 0% must never run.
    s3_tree = {"type": "Selector", "name": "Root", "children": [
        mark_seq(3, decorators=[{"type": "Chance", "params": {"Probability": 100}}]),
        mark_seq(4, decorators=[{"type": "Chance", "params": {"Probability": 0}}]),
    ]}
    asset = create_and_build(3, s3_tree)
    result = run_verify({"assetPath": asset, "blackboardInit": {"Mark": 0}})
    assert_run("m5 S3", result, board_expect={"Mark": 3})

    # S4: distance 600 in [100,1000] -> branch runs; service keeps Dist fresh.
    s4_tree = {"type": "Selector", "name": "Root",
               "services": [{"type": "UpdateDistance", "params": {
                   "SourceKey": "SelfPt", "TargetKey": "FarPt", "ResultKey": "Dist"}}],
               "children": [
        mark_seq(5, decorators=[{"type": "DistanceCheck", "params": {
            "SourceKey": "SelfPt", "TargetKey": "FarPt", "MinDistance": 100, "MaxDistance": 1000}}],
            wait=1.0),
        mark_seq(6),
    ]}
    s4_tree["externalKeys"] = {"SelfPt": "blackboardInit", "FarPt": "blackboardInit"}
    asset = create_and_build(4, s4_tree)
    result = run_verify({"assetPath": asset,
                         "blackboardInit": {"SelfPt": [0, 0, 100], "FarPt": [600, 0, 100]},
                         "durationSec": 8})
    assert_run("m5 S4", result, board_expect={"Mark": 5, "Dist": 600})

    # S5: threshold not met -> selector falls through to the undecorated branch.
    s5_tree = {"type": "Selector", "name": "Root", "children": [
        mark_seq(7, decorators=[{"type": "FloatCompare", "params": {
            "BlackboardKey": "Dist", "Operator": "Less", "CompareToValue": 100}}]),
        mark_seq(8),
    ]}
    s5_tree["externalKeys"] = {"Dist": "blackboardInit"}
    asset = create_and_build(5, s5_tree)
    result = run_verify({"assetPath": asset,
                         "blackboardInit": {"Dist": 600,
                                            "SelfPt": [0, 0, 100], "FarPt": [600, 0, 100]}})
    assert_run("m5 S5", result, board_expect={"Mark": 8})

    # S6: a Severity=Error checkpoint must fail the run through no_log_errors.
    s6_tree = {"type": "Sequence", "name": "Root", "children": [
        {"type": "Log", "name": "Boom", "params": {
            "Message": "m5 deliberate error", "Severity": "Error"}},
        {"type": "Wait", "params": {"WaitTime": 0.3}},
    ]}
    asset = create_and_build(6, s6_tree)
    result = run_verify({"assetPath": asset})
    assert_run("m5 S6", result, expect_pass=False, log_hints=("m5 deliberate error", None))

    # S6b: Severity=Warning must NOT fail the run but must appear in logWarnings.
    s6b_tree = {"type": "Sequence", "name": "Root", "children": [
        {"type": "Log", "name": "Warn", "params": {
            "Message": "m5 deliberate warning", "Severity": "Warning"}},
        {"type": "Wait", "params": {"WaitTime": 0.3}},
    ]}
    asset = create_and_build("6b", s6b_tree)
    result = run_verify({"assetPath": asset})
    assert_run("m5 S6b", result, expect_pass=True, log_hints=(None, "m5 deliberate warning"))

    # S7: SetValue service writes a constant while the branch runs.
    s7_tree = {"type": "Sequence", "name": "Root",
               "services": [{"type": "SetValue", "params": {
                   "KeyToSet": "Result", "Constant": 7, "bConstantBool": False}}],
               "children": [{"type": "Wait", "params": {"WaitTime": 1.0}}]}
    asset = create_and_build(7, s7_tree)
    result = run_verify({"assetPath": asset, "blackboardInit": {"Result": 0}, "durationSec": 8})
    assert_run("m5 S7", result, board_expect={"Result": 7})

    # ---------------------------------------------------------------- 4. reference policy over an M5 asset
    if 4 in assets:
        refs = call_tool("inspect_references", {"RequestJson": json.dumps({"assetPath": assets[4]})})
        modules = ((refs.get("summary") or {}).get("byModule") or {})
        check("m5 references verdict ok", refs.get("ok") is True and refs.get("verdict") == "ok",
              json.dumps(refs.get("errors"))[:300])
        check("m5 references see BTForgeRuntime module",
              (modules.get("BTForgeRuntime") or 0) >= 1, json.dumps(modules))

    # ---------------------------------------------------------------- 5. summary
    print(f"\n[artifacts] {assets}", flush=True)
    if not keep:
        print("[cleanup] close the editor, then delete those assets "
              "(open assets cannot be removed safely).", flush=True)
    if FAILURES:
        print(f"\nRESULT: FAIL ({len(FAILURES)} checks failed: {FAILURES})", flush=True)
        sys.exit(4)
    print("\nRESULT: PASS (M5 node library: catalog -> specs -> 8 PIE behavior runs)", flush=True)


if __name__ == "__main__":
    main()
