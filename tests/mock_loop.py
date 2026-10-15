#!/usr/bin/env python3
"""Anthropic-format mock that repeats the same tool call until the user sends a note. Usage: mock_loop.py PORT PROJ"""
import json, sys
from http.server import BaseHTTPRequestHandler, HTTPServer
port, proj = int(sys.argv[1]), sys.argv[2]
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        last = body["messages"][-1]
        note = ""
        if isinstance(last["content"], list):
            for b in last["content"]:
                if b.get("type") == "text" and "Note from the user" in b["text"]: note = b["text"]
        n = len(body["messages"])
        if note:
            c = [{"type": "tool_use", "id": f"d{n}", "name": "task_complete", "input": {"summary": "GOT: " + note}}]
        else:
            c = [{"type": "tool_use", "id": f"t{n}", "name": "run_command", "input": {"command": "echo same", "cwd": proj}}]
        out = {"type": "message", "role": "assistant", "stop_reason": "tool_use", "usage": {"input_tokens": 1, "output_tokens": 1}, "content": c}
        b = json.dumps(out).encode()
        self.send_response(200); self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
HTTPServer(("127.0.0.1", port), H).serve_forever()
