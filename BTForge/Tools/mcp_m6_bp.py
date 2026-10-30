#!/usr/bin/env python3
"""M6 blueprint node generation (data-only subclasses): creation, catalog,
spec integration, and PIE behavior checks.

The M6 deliverable (part 1) is create_node_blueprint: a data-only Blueprint
node asset whose generated class derives from a BT node base class and whose
per-project defaults live on the class CDO. This suite proves:

  creation - the tool makes a compiled /Game/..._C class from a parent class
             path + CDO params, refuses duplicates/bad parents (Actor,
             composite, abstract, unknown), rejects unknown params, and leaves
             nothing behind when it fails;
  catalog  - the new class shows up as a blueprint node entry (AssetRegistry);
  specs    - validate_behavior_tree accepts the class path as a node type;
  runtime  - a tree built with the BP task logs the CDO-configured message at
             the CDO-configured severity, and a BP service writes its
             CDO-configured key every tick (PIE blackboard assertions).

Usage:
  python mcp_m6_bp.py [--official] [--result-file PATH] [--keep]

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
    "create_node_blueprint": "BTForgeOfficial.BTForgeToolset.CreateNodeBlueprint",
}
REQUIRED_TOOLS = list(OFFICIAL_NAMES.keys())
FAILURES = []
SESSION = [None]
NEXT_ID = [0]
TEMPLATE_MAP = "/Engine/Maps/Templates/Template_Default"
RESULT_FILE = "C:/UEProject/LyraStarterGame/Saved/BTForgeVerify/last_result.json"


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


def assert_board(label_prefix, result, key, expected):
    got = (result.get("blackboardFinal") or {}).get(key)
    try:
        ok = abs(float(got) - float(expected)) <= 0.51
    except (TypeError, ValueError):
        ok = str(got) == str(expected)
    check(f"{label_prefix} blackboard {key}=={expected}", ok,
          f"got={got!r} board={json.dumps(result.get('blackboardFinal'))[:300]}")


def create_bp(tag, asset, parent, params, expect_ok=True):
    r = call_tool("create_node_blueprint", {
        "AssetPath": asset,
        "ParentClassPath": parent,
        "ParamsJson": json.dumps(params) if params else "",
    })
    if expect_ok:
        check(f"m6 {tag} created", r.get("ok") is True and str(r.get("classPath", "")).endswith("_C"),
              json.dumps(r)[:400])
    else:
        check(f"m6 {tag} rejected", r.get("ok") is False, json.dumps(r)[:300])
    return r


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
    bp_task = "/Game/AI/BTA_M6Log_" + stamp
    bp_svc = "/Game/AI/BTA_M6Svc_" + stamp
    bt_asset = "/Game/AI/BT_M6_" + stamp

    # ---------------------------------------------------------------- 0. handshake
    rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                       "clientInfo": {"name": "m6-bp", "version": "1"}})
    rpc("notifications/initialized", None)
    listing = rpc("tools/list", None)
    names = [t["name"] for t in listing.get("result", {}).get("tools", [])]
    expected = [OFFICIAL_NAMES[n] for n in REQUIRED_TOOLS] if OFFICIAL[0] else REQUIRED_TOOLS
    missing = [n for n in expected if n not in names]
    check("m6 all 12 tools listed", not missing, f"missing={missing} have={names[:14]}")

    # ---------------------------------------------------------------- 1. creation
    created = create_bp("bp task", bp_task, "/Script/BTForgeRuntime.BTForgeTask_LogMessage",
                        {"Message": "m6 bp checkpoint", "Severity": "Warning"})
    task_class = created.get("classPath") or ""
    check("m6 bp task parent echoed", str(created.get("parent", "")).endswith("BTForgeTask_LogMessage"),
          str(created.get("parent")))
    check("m6 bp task appliedParams", created.get("appliedParams") == 2, str(created.get("appliedParams")))

    svc_created = create_bp("bp service", bp_svc, "/Script/BTForgeRuntime.BTForgeService_SetBlackboardValue",
                            {"KeyToSet": "Mark", "Constant": 9, "Interval": 0.1})
    svc_class = svc_created.get("classPath") or ""

    # ---------------------------------------------------------------- 2. rejections
    dup = "/Game/AI/BTA_M6Dup_" + stamp
    create_bp("dup of bp task", dup, "/Script/BTForgeRuntime.BTForgeTask_LogMessage",
              {"Message": "x"}, expect_ok=True)
    create_bp("duplicate", dup, "/Script/BTForgeRuntime.BTForgeTask_LogMessage", {"Message": "y"}, expect_ok=False)
    create_bp("actor parent", "/Game/AI/BTA_M6Bad_" + stamp, "/Script/Engine.Actor", {}, expect_ok=False)
    create_bp("composite parent", "/Game/AI/BTA_M6Bad2_" + stamp, "/Script/AIModule.BTComposite_Sequence", {}, expect_ok=False)
    create_bp("abstract parent", "/Game/AI/BTA_M6Bad3_" + stamp, "/Script/AIModule.BTTaskNode", {}, expect_ok=False)
    create_bp("unknown parent", "/Game/AI/BTA_M6Bad4_" + stamp, "/Script/AIModule.DoesNotExist", {}, expect_ok=False)
    not_bpable = create_bp("non-blueprintable parent", "/Game/AI/BTA_M6Bad6_" + stamp,
                           "/Script/AIModule.BTTask_Wait", {}, expect_ok=False)
    check("m6 non-blueprintable error is actionable", "Blueprintable" in str(not_bpable.get("error", "")),
          json.dumps(not_bpable)[:200])
    bad_param = create_bp("unknown param", "/Game/AI/BTA_M6Bad5_" + stamp,
                          "/Script/BTForgeRuntime.BTForgeTask_LogMessage", {"NoSuchParam": 1}, expect_ok=False)
    check("m6 unknown-param error names the param", "NoSuchParam" in str(bad_param.get("error", "")),
          json.dumps(bad_param)[:200])
    # failure must not leave the asset behind: same path now succeeds
    retry = create_bp("cleanup after failure", "/Game/AI/BTA_M6Bad5_" + stamp,
                      "/Script/BTForgeRuntime.BTForgeTask_LogMessage", {"Message": "ok"}, expect_ok=True)

    # ---------------------------------------------------------------- 3. catalog
    catalog_class = None
    for _ in range(20):  # AssetRegistry updates land shortly after save
        catalog = call_tool("get_capability_catalog", {"SectionJson": json.dumps({"section": "bt"})})
        bp_entries = [e for e in ((catalog.get("bt") or {}).get("blueprintNodes") or [])
                      if str(e.get("assetPath", "")).startswith("/Game/AI/BTA_M6")]
        if bp_task.split("/")[-1] in [e.get("name") for e in bp_entries]:
            catalog_class = bp_entries
            break
        time.sleep(0.5)
    check("m6 catalog lists created blueprint nodes", catalog_class is not None,
          json.dumps(catalog)[:200] if catalog is None else "not found after 10s")
    if catalog_class:
        task_entry = next((e for e in catalog_class if e.get("name") == bp_task.split("/")[-1]), None)
        check("m6 catalog classifies bp task", (task_entry or {}).get("kind") == "task",
              json.dumps(task_entry))
        check("m6 catalog classPath usable as type", (task_entry or {}).get("classPath") == task_class,
              f"{(task_entry or {}).get('classPath')} vs {task_class}")

    # ---------------------------------------------------------------- 4. spec integration + PIE: BP task
    blackboard = {"keys": [{"name": "Mark", "type": "Int"}]}
    tree = {"type": "Sequence", "name": "Root", "children": [
        {"type": task_class, "name": "BpCheckpoint"},
        {"type": "Wait", "params": {"WaitTime": 0.3}},
    ]}
    dry = call_tool("validate_behavior_tree", {"TreeJson": json.dumps(tree),
                                               "BlackboardJson": json.dumps(blackboard)})
    check("m6 dry-run accepts bp class as node type", dry.get("ok") is True, json.dumps(dry)[:400])

    created_bt = call_tool("create_behavior_tree_asset",
                           {"AssetPath": bt_asset, "BlackboardJson": json.dumps(blackboard)})
    built = call_tool("build_behavior_tree", {"AssetPath": bt_asset, "TreeJson": json.dumps(tree)})
    check("m6 bt built with bp node", created_bt.get("ok") is True and built.get("ok") is True,
          json.dumps(built)[:300])

    result = run_verify({"assetPath": bt_asset})
    checks = {c.get("name"): c for c in (result.get("checks") or [])}
    print(f"  -> status={result.get('status')} pass={result.get('pass')}", flush=True)
    check("m6 bp task run finished", result.get("status") in ("done", "failed"), json.dumps(result)[:400])
    check("m6 bp task run passes", result.get("pass") is True, json.dumps(result.get("checks"))[:400])
    check("m6 CDO severity drives runtime logWarnings",
          any("m6 bp checkpoint" in w for w in (result.get("logWarnings") or [])),
          json.dumps(result.get("logWarnings"))[:400])

    # ---------------------------------------------------------------- 5. PIE: BP service
    svc_tree = {"type": "Sequence", "name": "Root",
                "services": [{"type": svc_class}],
                "children": [{"type": "Wait", "params": {"WaitTime": 1.0}}]}
    built_svc = call_tool("build_behavior_tree", {"AssetPath": bt_asset, "TreeJson": json.dumps(svc_tree)})
    check("m6 bt rebuilt with bp service", built_svc.get("ok") is True, json.dumps(built_svc)[:300])
    result = run_verify({"assetPath": bt_asset, "blackboardInit": {"Mark": 0}, "durationSec": 8})
    print(f"  -> status={result.get('status')} pass={result.get('pass')}", flush=True)
    check("m6 bp service run finished", result.get("status") in ("done", "failed"), json.dumps(result)[:400])
    check("m6 bp service run passes", result.get("pass") is True, json.dumps(result.get("checks"))[:400])
    assert_board("m6 bp service", result, "Mark", 9)

    # ---------------------------------------------------------------- 6. summary
    print(f"\n[artifacts] BP={bp_task} BP_SVC={bp_svc} BT={bt_asset} "
          f"DUP={dup} BTA_M6Bad5=/Game/AI/BTA_M6Bad5_{stamp}", flush=True)
    if not keep:
        print("[cleanup] close the editor, then delete those assets "
              "(open assets cannot be removed safely).", flush=True)
    if FAILURES:
        print(f"\nRESULT: FAIL ({len(FAILURES)} checks failed: {FAILURES})", flush=True)
        sys.exit(4)
    print("\nRESULT: PASS (M6 blueprint node generation: creation -> catalog -> spec -> PIE)", flush=True)


if __name__ == "__main__":
    main()
