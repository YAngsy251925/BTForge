#!/usr/bin/env python3
"""Probe the editor MCP server tools/list for specific toolset names."""
import json
import sys
import urllib.error
import urllib.request

BASE = "http://127.0.0.1:8000/mcp"
SESSION = [None]


def post(payload):
    req = urllib.request.Request(BASE, data=json.dumps(payload).encode(), method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Accept", "application/json, text/event-stream")
    if SESSION[0]:
        req.add_header("Mcp-Session-Id", SESSION[0])
    resp = urllib.request.urlopen(req, timeout=60)
    sid = resp.headers.get("Mcp-Session-Id")
    if sid:
        SESSION[0] = sid
    body = resp.read().decode("utf-8", "replace")
    if "text/event-stream" in resp.headers.get("Content-Type", ""):
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


def main():
    post({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
        "protocolVersion": "2024-11-05", "capabilities": {},
        "clientInfo": {"name": "probe", "version": "0"}}})
    post({"jsonrpc": "2.0", "method": "notifications/initialized"})
    resp = post({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
    tools = resp.get("result", {}).get("tools", [])
    print("total:", len(tools), flush=True)
    keywords = sys.argv[1:] or ["UMGToolSet", "SlateInspector", "BTForge",
                                "umg_toolset", "slate_inspector", "btforge",
                                "EditorApp", "CreateWidgetBlueprint", "PressKey", "StartPIE"]
    for kw in keywords:
        hits = [t["name"] for t in tools if kw.lower() in t["name"].lower()]
        for h in hits[:4]:
            print(f"{kw} -> {h}", flush=True)
        if not hits:
            print(f"{kw} -> (none)", flush=True)


if __name__ == "__main__":
    main()
