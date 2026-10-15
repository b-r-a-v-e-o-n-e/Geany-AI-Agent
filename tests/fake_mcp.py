#!/usr/bin/env python3
"""Tiny fake MCP server for tests. stdio: fake_mcp.py   |   HTTP: fake_mcp.py --http PORT [--sse]"""
import json, sys

TOOLS = [
    {"name": "echo", "description": "Echo the text back", "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}}, "required": ["text"]}},
    {"name": "boom", "description": "Always fails", "inputSchema": {"type": "object"}},
]

def handle(msg):
    """Returns a response dict or None (notification)."""
    m, i = msg.get("method"), msg.get("id")
    if i is None:
        return None
    if m == "initialize":
        return {"jsonrpc": "2.0", "id": i, "result": {"protocolVersion": "2025-03-26", "capabilities": {"tools": {}}, "serverInfo": {"name": "fake", "version": "1"}}}
    if m == "tools/list":
        return {"jsonrpc": "2.0", "id": i, "result": {"tools": TOOLS}}
    if m == "tools/call":
        p = msg["params"]
        if p["name"] == "echo":
            return {"jsonrpc": "2.0", "id": i, "result": {"content": [{"type": "text", "text": "echo: " + p["arguments"].get("text", "")}]}}
        return {"jsonrpc": "2.0", "id": i, "result": {"isError": True, "content": [{"type": "text", "text": "boom!"}]}}
    return {"jsonrpc": "2.0", "id": i, "error": {"code": -32601, "message": "Method not found"}}

if len(sys.argv) > 2 and sys.argv[1] == "--http":
    from http.server import BaseHTTPRequestHandler, HTTPServer
    sse = "--sse" in sys.argv
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a): pass
        def do_POST(self):
            msg = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            if self.headers.get("Authorization") != "Bearer sekret":
                self.send_response(401); self.end_headers(); return
            r = handle(msg)
            if r is None:
                self.send_response(202); self.send_header("Content-Length", "0"); self.end_headers(); return
            if sse:
                b = ("event: message\ndata: " + json.dumps(r) + "\n\n").encode(); ct = "text/event-stream"
            else:
                b = json.dumps(r).encode(); ct = "application/json"
            self.send_response(200); self.send_header("Content-Type", ct); self.send_header("Mcp-Session-Id", "s1")
            self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    HTTPServer(("127.0.0.1", int(sys.argv[2])), H).serve_forever()
else:
    for line in sys.stdin:
        line = line.strip()
        if not line: continue
        r = handle(json.loads(line))
        if r is not None:
            sys.stdout.write(json.dumps(r) + "\n"); sys.stdout.flush()
