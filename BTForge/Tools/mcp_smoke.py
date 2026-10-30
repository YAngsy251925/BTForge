#!/usr/bin/env python3
"""BTForge MCP smoke test (works for both endpoints).

Default targets BTForge's self-contained server (http://127.0.0.1:8090/mcp,
flat snake_case tool names). With --official it targets Epic's 5.8
ModelContextProtocol server (http://127.0.0.1:8000/mcp) and the
BTForgeOfficial.BTForgeToolset.* tool names instead.

Flow: initialize -> tools/list -> create -> build -> inspect -> verify.
Exit codes: 0 pass, 2 port timeout, 3 tools missing, 4 verification failed.

Usage:
  python mcp_smoke.py                          # BTForge server on :8090
  python mcp_smoke.py --official               # Epic server on :8000
  python mcp_smoke.py --url http://127.0.0.1:8091/mcp --asset /Game/AI/BT_X
  python mcp_smoke.py --objects                # pass Tree/Blackboard as real JSON objects (BTForge server only)
"""
import argparse
import json
import socket
import sys
import time
import urllib.error
import urllib.request

FLAT_TOOLS = {
    "create": "create_behavior_tree_asset",
    "build": "build_behavior_tree",
    "inspect": "inspect_behavior_tree",
}
OFFICIAL_TOOLS = {
    "create": "BTForgeOfficial.BTForgeToolset.CreateBehaviorTreeAsset",
    "build": "BTForgeOfficial.BTForgeToolset.BuildBehaviorTree",
    "inspect": "BTForgeOfficial.BTForgeToolset.InspectBehaviorTree",
}

TREE = {
    "type": "Selector",
    "name": "Root",
    "children": [
        {
            "type": "Sequence",
            "name": "IdleLoop",
            "children": [
                {"type": "Wait", "name": "ShortPause",
                 "params": {"WaitTime": 2.0, "RandomDeviation": 1.0}},
                {"type": "Wait", "name": "LongPause",
                 "params": {"WaitTime": 4.0}},
            ],
        },
        {"type": "Wait", "name": "Fallback", "params": {"WaitTime": 1.5}},
    ],
}
BLACKBOARD = {"keys": [
    {"name": "HomeLocation", "type": "Vector"},
    {"name": "TargetActor", "type": "Object"},
    {"name": "AggroLevel", "type": "Int"},
]}

# L2 tree: RunEQS (EQS asset reference + enum + result blackboard key)
# feeding a MoveTo (blackboard key goal). Lyra only (needs its EQS asset).
TREE_L2 = {
    "type": "Selector", "name": "Root",
    "children": [
        {"type": "Sequence", "name": "Patrol",
         "children": [
             {"type": "RunEQS", "name": "FindGoal",
              "params": {
                  "BlackboardKey": "MoveGoal",
                  "EQSRequest": {
                      "QueryTemplate": "/ShooterCore/Bot/EQS/EQS_FindTarget.EQS_FindTarget",
                      "RunMode": "SingleResult"}}},
             {"type": "MoveTo", "name": "Go",
              "params": {"BlackboardKey": "MoveGoal", "AcceptableRadius": 50}},
         ]},
        {"type": "Wait", "name": "Idle", "params": {"WaitTime": 1.0}},
    ],
}
BLACKBOARD_L2 = {"keys": [{"name": "MoveGoal", "type": "Vector"}]}

FAILURES = []


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
                print(f"[ok] port {port} is up", flush=True)
                return True
        except OSError:
            time.sleep(3)
    print(f"[timeout] port {port} never opened within {timeout}s", flush=True)
    return False


class McpClient:
    def __init__(self, url):
        self.url = url
        self.session = None
        self.next_id = 0

    def post(self, payload):
        req = urllib.request.Request(self.url, data=json.dumps(payload).encode(), method="POST")
        req.add_header("Content-Type", "application/json")
        req.add_header("Accept", "application/json, text/event-stream")
        if self.session:
            req.add_header("Mcp-Session-Id", self.session)
        try:
            resp = urllib.request.urlopen(req, timeout=120)
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
        except urllib.error.HTTPError as exc:
            return {"http_error": exc.code, "detail": exc.read().decode("utf-8", "replace")[:300]}
        except Exception as exc:  # noqa: BLE001
            return {"transport_error": str(exc)}

    def rpc(self, method, params=None):
        self.next_id += 1
        payload = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            payload["params"] = params
        return self.post(payload)

    def notify(self, method):
        self.post({"jsonrpc": "2.0", "method": method})

    def call_tool(self, name, arguments):
        resp = self.rpc("tools/call", {"name": name, "arguments": arguments})
        if not isinstance(resp, dict):
            return None, "(no response)"
        if resp.get("error"):
            return None, f"RPC ERROR: {resp['error']}"
        result = resp.get("result", {})
        text = "\n".join(c.get("text", "") for c in result.get("content", []))
        return result, text


def parse_tool_result(text):
    """Parse the library JSON from a tool result; unwrap the official
    endpoint's {"returnValue": "<json>"} wrapper if present."""
    try:
        outer = json.loads(text)
    except (json.JSONDecodeError, TypeError):
        return {}
    if isinstance(outer, dict) and isinstance(outer.get("returnValue"), str):
        try:
            return json.loads(outer["returnValue"])
        except json.JSONDecodeError:
            return {}
    return outer if isinstance(outer, dict) else {}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default=None, help="MCP endpoint URL")
    parser.add_argument("--asset", default=None, help="asset path, e.g. /Game/AI/BT_Smoke")
    parser.add_argument("--official", action="store_true",
                        help="target Epic's 5.8 MCP server on :8000 with BTForgeOfficial tool names")
    parser.add_argument("--objects", action="store_true",
                        help="send BlackboardJson/TreeJson as real JSON objects (BTForge server only)")
    parser.add_argument("--l2", action="store_true",
                        help="run the L2 write-path suite (RunEQS+MoveTo tree and negative cases; Lyra only)")
    parser.add_argument("--l3", action="store_true",
                        help="run the L3 suite (services + decorators; Lyra only)")
    parser.add_argument("--l35", action="store_true",
                        help="run the L3.5 suite (blueprint class instantiation; Lyra only)")
    args = parser.parse_args()

    url = args.url or ("http://127.0.0.1:8000/mcp" if args.official else "http://127.0.0.1:8090/mcp")
    tools = OFFICIAL_TOOLS if args.official else FLAT_TOOLS
    asset = args.asset or ("/Game/AI/BT_Smoke" + ("_Official" if args.official else ""))
    host = url.split("//", 1)[1].split(":", 1)[0]
    port = int(url.rsplit(":", 1)[1].split("/", 1)[0])

    print(f"endpoint: {url}\nasset:    {asset}\ntools:    {tools['create']} ...", flush=True)
    if not wait_port(host, port):
        sys.exit(2)

    client = McpClient(url)
    init = client.rpc("initialize", {
        "protocolVersion": "2024-11-05",
        "capabilities": {},
        "clientInfo": {"name": "btforge-smoke", "version": "2.0"},
    })
    server_info = (init or {}).get("result", {}).get("serverInfo", {}) if isinstance(init, dict) else {}
    print(f"[init] server: {server_info.get('name', '?')} {server_info.get('version', '?')}", flush=True)
    client.notify("notifications/initialized")

    listing = client.rpc("tools/list")
    names = [t["name"] for t in listing.get("result", {}).get("tools", [])] if isinstance(listing, dict) else []
    print(f"[tools] {len(names)} exposed: {', '.join(names)}", flush=True)
    missing = [n for n in tools.values() if n not in names]
    if missing:
        print(f"[fatal] missing tools: {missing}", flush=True)
        sys.exit(3)

    def spec(value):
        # --objects exercises the BTForge server's object coercion; other tools want strings.
        return value if args.objects and not args.official else json.dumps(value)

    _, text = client.call_tool(tools["create"], {"AssetPath": asset, "BlackboardJson": spec(BLACKBOARD)})
    created = parse_tool_result(text)
    print(f"create -> {text}", flush=True)
    check("create ok", created.get("ok") is True, text)
    check("blackboard key count", created.get("keyCount") == len(BLACKBOARD["keys"]), text)

    _, text = client.call_tool(tools["build"], {"AssetPath": asset, "TreeJson": spec(TREE)})
    built = parse_tool_result(text)
    print(f"build  -> {text}", flush=True)
    check("build ok", built.get("ok") is True, text)
    check("node count == 5", built.get("nodeCount") == 5, text)
    check("root is Selector", built.get("root") == "BTComposite_Selector", text)

    _, text = client.call_tool(tools["inspect"], {"AssetPath": asset})
    inspected = parse_tool_result(text)
    print(f"inspect-> {text[:400]}", flush=True)
    root = inspected.get("root") or {}
    check("inspect ok", inspected.get("ok") is True, text)
    key_names = [k.get("name") for k in inspected.get("blackboardKeys", [])]
    # The engine adds a built-in SelfActor key; require every user key to exist.
    check("inspect blackboard keys", all(k["name"] in key_names for k in BLACKBOARD["keys"]), str(key_names))
    check("inspect root type", root.get("type") == "BTComposite_Selector")
    check("inspect root has 2 children", len(root.get("children", [])) == 2)
    idle = root.get("children", [{}])[0]
    check("IdleLoop is Sequence", idle.get("params", {}).get("NodeName") == "IdleLoop"
          and idle.get("type") == "BTComposite_Sequence")
    waits = idle.get("children", [])
    check("IdleLoop Wait times", len(waits) == 2
          and waits[0].get("params", {}).get("WaitTime") == 2.0
          and waits[0].get("params", {}).get("RandomDeviation") == 1.0
          and waits[1].get("params", {}).get("WaitTime") == 4.0)

    if FAILURES:
        print(f"\nRESULT: FAIL ({len(FAILURES)} checks failed: {FAILURES})", flush=True)
        sys.exit(4)
    print("\nRESULT: PASS (create/build/inspect verified end to end)", flush=True)
    if args.l2:
        run_l2(client, tools)
    if args.l3:
        run_l3(client, tools)
    if args.l35:
        run_l35(client, tools)


def run_l2(client, tools):
    """L2 write path: RunEQS (asset ref + enum + result key) -> MoveTo (key goal),
    plus negative cases proving the validation contract."""
    # Timestamped on purpose: a fixed name makes the second run of this suite fail
    # with "already exists" on a machine where the first run's assets were kept.
    asset = "/Game/AI/BT_Smoke_L2_" + time.strftime("%H%M%S")
    print(f"\n=== L2: create {asset} (MoveGoal blackboard) ===", flush=True)
    _, text = client.call_tool(tools["create"], {"AssetPath": asset, "BlackboardJson": json.dumps(BLACKBOARD_L2)})
    created = parse_tool_result(text)
    check("L2 create ok", created.get("ok") is True, text)

    _, text = client.call_tool(tools["build"], {"AssetPath": asset, "TreeJson": json.dumps(TREE_L2)})
    built = parse_tool_result(text)
    print(f"L2 build -> {text}", flush=True)
    check("L2 build ok (RunEQS+MoveTo accepted)", built.get("ok") is True, text)
    check("L2 node count == 5", built.get("nodeCount") == 5, text)

    _, text = client.call_tool(tools["inspect"], {"AssetPath": asset})
    inspected = parse_tool_result(text)
    root = inspected.get("root") or {}
    patrol = (root.get("children") or [{}])[0]
    patrol_children = patrol.get("children") or []
    check("L2 inspect ok", inspected.get("ok") is True, text)
    check("L2 Patrol is Sequence", patrol.get("type") == "BTComposite_Sequence", text)
    check("L2 has RunEQSQuery task", patrol_children and patrol_children[0].get("type") == "BTTask_RunEQSQuery", text)
    check("L2 has MoveTo task", len(patrol_children) > 1 and patrol_children[1].get("type") == "BTTask_MoveTo", text)
    if len(patrol_children) > 1:
        check("L2 MoveTo AcceptableRadius == 50",
              patrol_children[1].get("params", {}).get("AcceptableRadius") == 50, text)

    # Negative cases: the validation contract must reject, not corrupt.
    _, text = client.call_tool(tools["build"], {"AssetPath": asset, "TreeJson": json.dumps(
        {"type": "Selector", "children": [{"type": "MoveTo", "params": {"BlackboardKey": "NoSuchKey"}}]})})
    bad_key = parse_tool_result(text)
    print(f"L2 negative (unknown key) -> {text}", flush=True)
    check("L2 rejects unknown blackboard key",
          bad_key.get("ok") is False and "unknown blackboard key" in bad_key.get("error", ""), text)

    _, text = client.call_tool(tools["build"], {"AssetPath": asset, "TreeJson": json.dumps(
        {"type": "Selector", "children": [{"type": "RunEQS", "params": {
            "EQSRequest": {"QueryTemplate": "/Game/Nope/MissingAsset.MissingAsset"}}}]})})
    bad_asset = parse_tool_result(text)
    print(f"L2 negative (missing asset) -> {text}", flush=True)
    check("L2 rejects missing EQS asset",
          bad_asset.get("ok") is False and "asset not found" in bad_asset.get("error", ""), text)

    _, text = client.call_tool(tools["inspect"], {"AssetPath": asset})
    still = parse_tool_result(text)
    root_still = still.get("root") or {}
    check("L2 tree unchanged after rejected builds",
          len(root_still.get("children", [])) == 2
          and len((root_still.get("children") or [{}])[0].get("children", [])) == 2, text)

    if FAILURES:
        print(f"\nRESULT: FAIL ({len(FAILURES)} checks failed: {FAILURES})", flush=True)
        sys.exit(4)
    print("\nRESULT: PASS (L2 write path + validation contract verified)", flush=True)




TREE_L3 = {
    "type": "Selector", "name": "Root",
    "children": [
        {"type": "Sequence", "name": "Guarded",
         "decorators": [{"type": "Blackboard", "params": {"BlackboardKey": "TargetActor"}}],
         "children": [
             {"type": "MoveTo", "params": {"BlackboardKey": "TargetActor", "AcceptableRadius": 80}},
         ]},
        {"type": "Sequence", "name": "Served",
         "services": [{"type": "RunEQS", "params": {
             "BlackboardKey": "MoveGoal",
             "EQSRequest": {"QueryTemplate": "/Game/AI/EQS_FindWeapon.EQS_FindWeapon",
                            "RunMode": "SingleResult"}}}],
         "children": [
             {"type": "MoveTo", "params": {"BlackboardKey": "MoveGoal"}},
         ]},
    ],
}
BLACKBOARD_L3 = {"keys": [{"name": "MoveGoal", "type": "Vector"},
                          {"name": "TargetActor", "type": "Object"}]}


def run_l3(client, tools):
    """L3 write path: services on composites + decorators on child slots (AND)."""
    import time as _t
    asset = "/Game/AI/BT_Smoke_L3_" + str(int(_t.time()))
    print(f"\n=== L3: create {asset} ===", flush=True)
    _, text = client.call_tool(tools["create"], {"AssetPath": asset, "BlackboardJson": json.dumps(BLACKBOARD_L3)})
    created = parse_tool_result(text)
    check("L3 create ok", created.get("ok") is True, text)

    _, text = client.call_tool(tools["build"], {"AssetPath": asset, "TreeJson": json.dumps(TREE_L3)})
    built = parse_tool_result(text)
    print(f"L3 build -> {text}", flush=True)
    check("L3 build ok (service+decorator tree accepted)", built.get("ok") is True, text)
    check("L3 node count == 7 (root+2seq+2moveto+1svc+1dec)", built.get("nodeCount") == 7, text)

    _, text = client.call_tool(tools["inspect"], {"AssetPath": asset})
    inspected = parse_tool_result(text)
    root = inspected.get("root") or {}
    children = root.get("children") or []
    check("L3 inspect ok", inspected.get("ok") is True, text)
    if len(children) == 2:
        guarded, served = children[0], children[1]
        decs = guarded.get("decorators") or []
        svcs = served.get("services") or []
        check("L3 Guarded has 1 Blackboard decorator",
              len(decs) == 1 and decs[0].get("type") == "BTDecorator_Blackboard", text)
        check("L3 Served has 1 RunEQS service",
              len(svcs) == 1 and svcs[0].get("type") == "BTService_RunEQS", text)
        check("L3 service has no decorators", not served.get("decorators"), text)
        check("L3 decorator has no services", not guarded.get("services"), text)
    else:
        check("L3 root has 2 children", False, text)

    _, text = client.call_tool(tools["build"], {"AssetPath": asset, "TreeJson": json.dumps(
        {"type": "Selector", "children": [{"type": "Wait", "decorators": [{"type": "Nope"}]}]})})
    bad_aux = parse_tool_result(text)
    check("L3 rejects unknown decorator type",
          bad_aux.get("ok") is False and "unknown decorator" in bad_aux.get("error", ""), text)
    _, text = client.call_tool(tools["build"], {"AssetPath": asset, "TreeJson": json.dumps(
        {"type": "Selector", "children": [{"type": "Wait", "decorators": [{"type": "Blackboard", "params": {"BlackboardKey": "NoSuch"}}]}]})})
    bad_key = parse_tool_result(text)
    check("L3 rejects unknown key in decorator",
          bad_key.get("ok") is False and "unknown blackboard key" in bad_key.get("error", ""), text)

    if FAILURES:
        print(f"\nRESULT: FAIL ({len(FAILURES)} checks failed: {FAILURES})", flush=True)
        sys.exit(4)
    print("\nRESULT: PASS (L3 services+decorators verified)", flush=True)


def run_l35(client, tools):
    """L3.5: project/blueprint node classes referenced by object path."""
    # Tree node inventory before asserting: Root + Seq + MoveTo + Wait + svc = 5.
    svc = "/ShooterCore/Bot/Services/BTS_SetFocus.BTS_SetFocus_C"
    tree = {"type": "Selector", "name": "Root", "children": [
        {"type": "Sequence", "name": "Focused",
         "services": [{"type": svc, "params": {"Interval": 0.5, "TargetEnnemy": "TargetActor"}}],
         "children": [{"type": "MoveTo", "params": {"BlackboardKey": "TargetActor"}}]},
        {"type": "Wait", "params": {"WaitTime": 1.0}},
    ]}
    asset = "/Game/AI/BT_Smoke_L35_" + str(int(time.time()))
    print(f"\n=== L3.5: create {asset} (blueprint service class) ===", flush=True)
    _, text = client.call_tool(tools["create"], {"AssetPath": asset, "BlackboardJson": json.dumps(
        {"keys": [{"name": "TargetActor", "type": "Object"}]})})
    check("L3.5 create ok", parse_tool_result(text).get("ok") is True, text)

    _, text = client.call_tool(tools["build"], {"AssetPath": asset, "TreeJson": json.dumps(tree)})
    built = parse_tool_result(text)
    print(f"L3.5 build -> {text}", flush=True)
    check("L3.5 build ok (blueprint service instantiated)", built.get("ok") is True, text)
    check("L3.5 node count == 5 (root+seq+moveto+wait+svc)", built.get("nodeCount") == 5, text)

    _, text = client.call_tool(tools["inspect"], {"AssetPath": asset})
    inspected = parse_tool_result(text)
    focused = (inspected.get("root") or {}).get("children", [{}])[0]
    svcs = focused.get("services") or []
    check("L3.5 inspect ok", inspected.get("ok") is True, text)
    check("L3.5 service type is BTS_SetFocus_C",
          len(svcs) == 1 and svcs[0].get("type") == "BTS_SetFocus_C", text)
    if svcs:
        check("L3.5 service Interval == 0.5",
              svcs[0].get("params", {}).get("Interval") == 0.5, text)

    def negative(label, kind, payload, expect):
        # services must sit on a composite to reach the class-resolution path
        _, text = client.call_tool(tools["build"], {"AssetPath": asset, "TreeJson": json.dumps(
            {"type": "Selector", "children": [{"type": "Sequence", "children": [{"type": "Wait"}], kind: [payload]}]})})
        r = parse_tool_result(text)
        check(label, r.get("ok") is False and expect in r.get("error", ""), text)

    negative("L3.5 rejects missing class path", "services",
             {"type": "/Game/Nope/Missing.Missing_C"}, "could not load")
    negative("L3.5 rejects non-class asset", "services",
             {"type": "/Game/AI/EQS_FindWeapon.EQS_FindWeapon"}, "could not load")
    negative("L3.5 rejects abstract class", "services",
             {"type": "/Script/AIModule.BTService"}, "abstract")
    negative("L3.5 rejects wrong base class", "services",
             {"type": "/ShooterCore/Bot/BT/BTDecorator_AllowedToFire.BTDecorator_AllowedToFire_C",
              "params": {}}, "expected a class derived from BTService")

    if FAILURES:
        print(f"\nRESULT: FAIL ({len(FAILURES)} checks failed: {FAILURES})", flush=True)
        sys.exit(4)
    print("\nRESULT: PASS (L3.5 blueprint class instantiation verified)", flush=True)


if __name__ == "__main__":
    main()
