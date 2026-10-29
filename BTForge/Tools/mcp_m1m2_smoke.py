#!/usr/bin/env python3
"""BTForge M1/M2 smoke test: EQS builder + catalog + validate-only tools.

Targets the self-contained MCP server (http://127.0.0.1:8090/mcp).
Flow: initialize -> tools/list -> catalog -> validate(BT/EQS) -> create EQS ->
inspect EQS -> build a BT that references the generated EQS -> inspect BT.

Exit codes: 0 pass, 2 port timeout, 3 tools missing, 4 verification failed.
Usage: python mcp_m1m2_smoke.py [--url ...] [--keep-assets]
"""
import argparse
import json
import socket
import sys
import time
import urllib.request

URL = "http://127.0.0.1:8090/mcp"
FAILURES = []

# 5.8 official endpoint uses <Plugin>.<Class>.<Function> names and wraps the
# FString return value in {"returnValue": "..."}.
OFFICIAL_PREFIX = "BTForgeOfficial.BTForgeToolset."
OFFICIAL_NAMES = {
    "create_behavior_tree_asset": "CreateBehaviorTreeAsset",
    "build_behavior_tree": "BuildBehaviorTree",
    "inspect_behavior_tree": "InspectBehaviorTree",
    "validate_behavior_tree": "ValidateBehaviorTree",
    "create_eqs_asset": "CreateEQSAsset",
    "inspect_eqs_asset": "InspectEQSAsset",
    "validate_eqs_spec": "ValidateEQSSpec",
    "get_capability_catalog": "GetCapabilityCatalog",
}


def check(label, ok, detail=""):
    print(f"[{'ok' if ok else 'FAIL'}] {label}" + (f" -- {detail}" if detail and not ok else ""), flush=True)
    if not ok:
        FAILURES.append(label)
    return ok


def wait_port(host, port, timeout=900):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=2):
                return True
        except OSError:
            time.sleep(3)
    return False


class McpClient:
    def __init__(self, url, official=False):
        self.url = url
        self.official = official
        self.session = None
        self.next_id = 0

    def post(self, payload):
        req = urllib.request.Request(self.url, data=json.dumps(payload).encode(), method="POST")
        req.add_header("Content-Type", "application/json")
        req.add_header("Accept", "application/json, text/event-stream")
        if self.session:
            req.add_header("Mcp-Session-Id", self.session)
        resp = urllib.request.urlopen(req, timeout=180)
        sid = resp.headers.get("Mcp-Session-Id")
        if sid:
            self.session = sid
        body = resp.read().decode("utf-8", "replace")
        ctype = resp.headers.get("Content-Type", "")
        if "text/event-stream" in ctype:
            for line in body.splitlines():
                if line.startswith("data:"):
                    text = line[5:].strip()
                    if text:
                        try:
                            return json.loads(text)
                        except json.JSONDecodeError:
                            pass
            return None
        return json.loads(body) if body.strip() else None

    def rpc(self, method, params=None):
        self.next_id += 1
        payload = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            payload["params"] = params
        return self.post(payload)

    def call_tool(self, name, arguments):
        actual = OFFICIAL_PREFIX + OFFICIAL_NAMES[name] if self.official else name
        if self.official:
            # The official ToolsetRegistry binds FString params as JSON strings
            # and (without UFUNCTION defaults) requires every param to be sent.
            converted = {}
            for key, value in arguments.items():
                converted[key] = json.dumps(value) if isinstance(value, (dict, list)) else value
            if name == "validate_behavior_tree":
                converted.setdefault("BlackboardJson", "")
            if name == "get_capability_catalog":
                converted.setdefault("SectionJson", "")
            arguments = converted
        resp = self.rpc("tools/call", {"name": actual, "arguments": arguments})
        if not isinstance(resp, dict):
            return None, "(no response)"
        if resp.get("error"):
            return None, f"RPC ERROR: {resp['error']}"
        result = resp.get("result", {})
        text = "\n".join(c.get("text", "") for c in result.get("content", []))
        try:
            parsed = json.loads(text)
        except (json.JSONDecodeError, TypeError):
            return None, text
        if isinstance(parsed, dict) and isinstance(parsed.get("returnValue"), str):
            try:
                parsed = json.loads(parsed["returnValue"])
            except json.JSONDecodeError:
                return None, text
        return parsed, text


def main():
    global URL
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default=None)
    parser.add_argument("--official", action="store_true",
                        help="target Epic's 5.8 MCP server on :8000 with BTForgeOfficial tool names")
    args = parser.parse_args()
    official = args.official
    URL = args.url or ("http://127.0.0.1:8000/mcp" if official else "http://127.0.0.1:8090/mcp")
    stamp = time.strftime("%H%M%S")
    eqs_asset = f"/Game/AI/EQS_M1M2Smoke_{stamp}"
    eqs_bbcontext_asset = f"/Game/AI/EQS_M1M2BB_{stamp}"
    bt_asset = f"/Game/AI/BT_M1M2Smoke_{stamp}"

    host = URL.split("//", 1)[1].split(":", 1)[0]
    port = int(URL.rsplit(":", 1)[1].split("/", 1)[0])
    if not wait_port(host, port):
        print("[timeout] MCP port never opened", flush=True)
        sys.exit(2)

    client = McpClient(URL, official=official)
    init = client.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                     "clientInfo": {"name": "m1m2-smoke", "version": "1"}})
    print(f"[init] {json.dumps(init.get('result', {}).get('serverInfo', {}))}", flush=True)
    client.post({"jsonrpc": "2.0", "method": "notifications/initialized"})

    listing = client.rpc("tools/list")
    names = [t["name"] for t in listing.get("result", {}).get("tools", [])]
    print(f"[tools] {len(names)}: {', '.join(names)}", flush=True)
    required = ["create_behavior_tree_asset", "build_behavior_tree", "inspect_behavior_tree",
                "validate_behavior_tree", "create_eqs_asset", "inspect_eqs_asset",
                "validate_eqs_spec", "get_capability_catalog"]
    required_actual = [OFFICIAL_PREFIX + OFFICIAL_NAMES[n] for n in required] if official else required
    missing = [n for n in required_actual if n not in names]
    if missing:
        print(f"[fatal] missing tools: {missing}", flush=True)
        sys.exit(3)

    # ---------- catalog ----------
    catalog, text = client.call_tool("get_capability_catalog", {})
    ok = check("catalog ok", isinstance(catalog, dict) and catalog.get("ok") is True, text[:300])
    if ok:
        bt = catalog.get("bt", {})
        eqs = catalog.get("eqs", {})
        native_paths = {n.get("classPath") for n in bt.get("nativeNodes", [])}
        check("catalog has BTTask_Wait", "/Script/AIModule.BTTask_Wait" in native_paths,
              f"{len(native_paths)} native nodes")
        check("catalog has UEnvQueryContext_Querier",
              any(c.get("classPath") == "/Script/AIModule.EnvQueryContext_Querier" for c in eqs.get("nativeContexts", [])))
        check("catalog has BTForgeRuntime blackboard context",
              any("BTForgeEnvQueryContext_Blackboard" in (c.get("classPath") or "") for c in eqs.get("nativeContexts", [])))
        bp_nodes = bt.get("blueprintNodes", [])
        print(f"[info] blueprint BT nodes: {len(bp_nodes)}; EQS queries: {len(eqs.get('queries', []))}", flush=True)
        check("catalog EQS query list present", isinstance(eqs.get("queries"), list))
        if not eqs.get("queries"):
            print("[info] no EQS assets in this project yet (valid for a blank project)", flush=True)
        if bp_nodes:
            print(f"[info] first blueprint node: {bp_nodes[0]}", flush=True)

        # #4: the native list must not contain loaded blueprint classes (dedupe).
        check("catalog native nodes exclude blueprints",
              all(n.get("source") != "blueprint" for n in bt.get("nativeNodes", [])),
              "nativeNodes still contains blueprint-sourced entries")

        # #3: editable params carry a machine-readable JSON shape hint.
        wait_node = next((n for n in bt.get("nativeNodes", []) if n.get("name") == "BTTask_Wait"), None)
        wait_params = (wait_node or {}).get("editableParams", [])
        wait_time = next((p for p in wait_params if p.get("name") == "WaitTime"), None)
        check("catalog WaitTime has json shape hint",
              wait_time is not None and "number" in (wait_time.get("json") or ""),
              json.dumps(wait_time))

        # #1: aliases are derived from the EQS library tables (objects, not strings).
        aliases = eqs.get("specTypes", {}).get("generatorAliases", [])
        simple_grid = next((a for a in aliases if isinstance(a, dict) and a.get("alias") == "SimpleGrid"), None)
        check("catalog EQS aliases derived with classPath",
              simple_grid is not None and "EnvQueryGenerator_SimpleGrid" in (simple_grid.get("classPath") or ""),
              json.dumps(aliases)[:300])

        grid_gen = next((g for g in eqs.get("generators", []) if g.get("name") == "EnvQueryGenerator_SimpleGrid"), None)
        grid_params = (grid_gen or {}).get("editableParams", [])
        grid_size = next((p for p in grid_params if p.get("name") == "GridSize"), None)
        check("catalog EQS GridSize has json shape hint",
              grid_size is not None and "number" in (grid_size.get("json") or ""),
              json.dumps(grid_size))

    # ---------- BT validate-only ----------
    tree_json = {
        "type": "Selector", "name": "Root",
        "children": [{"type": "Sequence", "name": "Loop",
                      "children": [{"type": "Wait", "params": {"WaitTime": 1.0}}]}],
    }
    blackboard_json = {"keys": [{"name": "HomeLocation", "type": "Vector"},
                                {"name": "TargetEnemy", "type": "Object"}]}
    built, text = client.call_tool("validate_behavior_tree",
                                   {"TreeJson": tree_json, "BlackboardJson": blackboard_json})
    check("validate BT ok", isinstance(built, dict) and built.get("ok") is True, text[:300])
    if isinstance(built, dict) and built.get("ok"):
        check("validate BT nodeCount == 3", built.get("nodeCount") == 3, str(built.get("nodeCount")))
        check("validate BT root", built.get("root") == "BTComposite_Selector", str(built.get("root")))

    bad_tree = {"type": "Selector", "children": []}
    bad, text = client.call_tool("validate_behavior_tree", {"TreeJson": bad_tree})
    check("validate BT rejects empty children", isinstance(bad, dict) and bad.get("ok") is False
          and "children" in (bad.get("error") or ""), text[:200])

    bad_tree2 = {"type": "Selector", "children": [{"type": "Wait", "params": {"WaitTimee": 1.0}}]}
    bad, text = client.call_tool("validate_behavior_tree", {"TreeJson": bad_tree2})
    check("validate BT rejects unknown param", isinstance(bad, dict) and bad.get("ok") is False
          and "unknown param" in (bad.get("error") or ""), text[:200])

    # ---------- EQS validate-only ----------
    eqs_spec = {
        "options": [{
            "optionName": "FindSpot",
            "generator": {"type": "SimpleGrid",
                          "params": {"GridSize": 1500.0, "SpaceBetween": 200.0}},
            "tests": [{
                "type": "Distance",
                "params": {
                    "TestPurpose": "Score",
                    "ScoringEquation": "InverseLinear",
                    "ScoringFactor": 1.0,
                    "DistanceTo": "/Script/AIModule.EnvQueryContext_Querier",
                },
            }],
        }],
    }
    validated, text = client.call_tool("validate_eqs_spec", {"QueryJson": eqs_spec})
    check("validate EQS ok", isinstance(validated, dict) and validated.get("ok") is True, text[:400])
    if isinstance(validated, dict) and validated.get("ok"):
        check("validate EQS optionCount == 1", validated.get("optionCount") == 1, str(validated.get("optionCount")))

    bad_eqs = {"options": [{"generator": {"type": "NoSuchGenerator"}}]}
    bad, text = client.call_tool("validate_eqs_spec", {"QueryJson": bad_eqs})
    check("validate EQS rejects unknown generator", isinstance(bad, dict) and bad.get("ok") is False
          and "unknown generator type" in (bad.get("error") or ""), text[:200])

    bad_eqs2 = {"options": [{"generator": {"type": "SimpleGrid", "params": {"GridSise": 1.0}}}]}
    bad, text = client.call_tool("validate_eqs_spec", {"QueryJson": bad_eqs2})
    check("validate EQS rejects unknown param", isinstance(bad, dict) and bad.get("ok") is False
          and "unknown param" in (bad.get("error") or ""), text[:200])

    # ---------- EQS create + inspect ----------
    created, text = client.call_tool("create_eqs_asset", {"AssetPath": eqs_asset, "QueryJson": eqs_spec})
    check("create EQS ok", isinstance(created, dict) and created.get("ok") is True, text[:400])
    created_ok = isinstance(created, dict) and created.get("ok") is True
    if created_ok:
        check("create EQS optionCount", created.get("optionCount") == 1, str(created.get("optionCount")))
        check("create EQS generator", created.get("options", [{}])[0].get("generator") == "EnvQueryGenerator_SimpleGrid",
              str(created.get("options")))
        inspected, text = client.call_tool("inspect_eqs_asset", {"AssetPath": eqs_asset})
        check("inspect EQS ok", isinstance(inspected, dict) and inspected.get("ok") is True, text[:300])
        if isinstance(inspected, dict) and inspected.get("ok"):
            options = inspected.get("options", [])
            check("inspect EQS one option", len(options) == 1, str(len(options)))
            if options:
                gen = options[0].get("generator", {})
                check("inspect EQS generator type", gen.get("type") == "EnvQueryGenerator_SimpleGrid", str(gen.get("type")))
                check("inspect EQS generator grid size",
                      gen.get("params", {}).get("GridSize") == 1500.0, json.dumps(gen.get("params"))[:200])
                tests = options[0].get("tests", [])
                check("inspect EQS test count", len(tests) == 1, str(tests))
                if tests:
                    check("inspect EQS test type", tests[0].get("type") == "EnvQueryTest_Distance", str(tests[0].get("type")))
                    check("inspect EQS scoring equation",
                          tests[0].get("params", {}).get("ScoringEquation") == "InverseLinear",
                          json.dumps(tests[0].get("params"))[:200])
                    check("inspect EQS context class",
                          "EnvQueryContext_Querier" in str(tests[0].get("params", {}).get("DistanceTo", "")),
                          str(tests[0].get("params", {}).get("DistanceTo")))

    # EQS with the plugin runtime blackboard context class (validates runtime module class path).
    eqs_bb_spec = {
        "options": [{
            "generator": {"type": "CurrentLocation"},
            "tests": [{
                "type": "Distance",
                "params": {
                    "TestPurpose": "Score",
                    "DistanceTo": "/Script/BTForgeRuntime.BTForgeEnvQueryContext_Blackboard",
                },
            }],
        }],
    }
    created2, text = client.call_tool("create_eqs_asset", {"AssetPath": eqs_bbcontext_asset, "QueryJson": eqs_bb_spec})
    check("create EQS with runtime blackboard context", isinstance(created2, dict) and created2.get("ok") is True, text[:400])

    # ---------- BT built from the generated EQS ----------
    if created_ok:
        bb_json = {"keys": [{"name": "MoveGoal", "type": "Vector"}]}
        tree = {
            "type": "Selector", "name": "Root",
            "children": [{
                "type": "Sequence", "name": "Patrol",
                "children": [
                    {"type": "RunEQS", "name": "FindGoal",
                     "params": {"BlackboardKey": "MoveGoal",
                                "EQSRequest": {"QueryTemplate": eqs_asset, "RunMode": "SingleResult"}}},
                    {"type": "MoveTo", "name": "Go", "params": {"BlackboardKey": "MoveGoal", "AcceptableRadius": 100}},
                ],
            }],
        }
        btc, text = client.call_tool("create_behavior_tree_asset", {"AssetPath": bt_asset, "BlackboardJson": bb_json})
        check("create BT with generated EQS ok", isinstance(btc, dict) and btc.get("ok") is True, text[:300])
        built_bt, text = client.call_tool("build_behavior_tree", {"AssetPath": bt_asset, "TreeJson": tree})
        check("build BT referencing generated EQS", isinstance(built_bt, dict) and built_bt.get("ok") is True, text[:400])
        if isinstance(built_bt, dict) and built_bt.get("ok"):
            check("built BT nodeCount == 4", built_bt.get("nodeCount") == 4, str(built_bt.get("nodeCount")))
            inspected_bt, text = client.call_tool("inspect_behavior_tree", {"AssetPath": bt_asset})
            check("inspect built BT ok", isinstance(inspected_bt, dict) and inspected_bt.get("ok") is True, text[:300])

    print(f"\n[artifacts] EQS={eqs_asset} EQS_BB={eqs_bbcontext_asset} BT={bt_asset}", flush=True)
    if FAILURES:
        print(f"\nRESULT: FAIL ({len(FAILURES)} checks: {FAILURES})", flush=True)
        sys.exit(4)
    print("\nRESULT: PASS (M1 EQS builder + catalog + validate-only)", flush=True)


if __name__ == "__main__":
    main()
