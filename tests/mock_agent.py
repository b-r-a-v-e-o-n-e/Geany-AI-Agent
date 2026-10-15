#!/usr/bin/env python3
"""Scripted agent flow, Anthropic or OpenAI format. Usage: mock_agent.py PORT PROJ FORMAT"""
import json, sys
from http.server import BaseHTTPRequestHandler, HTTPServer
port, proj, fmt = int(sys.argv[1]), sys.argv[2], sys.argv[3]
state = {"n": 0}
problems = []
HELLO = '#include <cstdio>\nint main(){ std::printf("Hello agent\\n"); return 0; }\n'
steps = [
 ("check_environment", {"commands": ["g++", "definitely-not-a-cmd"], "packages": ["no-such-pkg-xyz"], "pkgconfig": ["no-such-module"]}),
 ("install_packages", {"packages": ["fake-dev-pkg"], "update": False}),
 ("write_file", {"path": proj + "/hello.cpp", "content": HELLO}),
 ("run_command", {"command": "g++ hello.cpp -o hello && ./hello", "cwd": proj}),
 ("edit_file", {"path": proj + "/hello.cpp", "old_str": "Hello agent", "new_str": "Hello edited"}),
 ("search_files", {"path": proj, "pattern": "Hello (edited|agent)", "glob": "*.cpp"}),
 ("run_command", {"command": "sudo ls /", "cwd": proj}),
 ("run_command", {"command": "rm -rf /", "cwd": proj}),
 ("run_command", {"command": "ls", "cwd": "/etc"}),
 ("run_command", {"command": "sleep 30", "cwd": proj, "timeout": 1}),
]
def check(i, text):
    t = text or ""
    ok = {
     0: lambda: '"is_root"' in t and "definitely-not-a-cmd" in t and "no-such-pkg-xyz" in t,
     1: lambda: "exit code: 0" in t,
     2: lambda: "file created on disk" in t,
     3: lambda: "exit code: 0" in t and "Hello agent" in t,
     4: lambda: "file updated on disk" in t,
     5: lambda: "hello.cpp:2" in t and "Hello edited" in t,
     6: lambda: "blocked by the safety policy" in t,
     7: lambda: "blocked by the safety policy" in t,
     8: lambda: "not permitted" in t,
     9: lambda: "timed out" in t,
    }[i]()
    if not ok: problems.append(f"step {i}: {t[:200]!r}")
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        msgs = body["messages"]
        names = [t["name"] if fmt == "anthropic" else t["function"]["name"] for t in body["tools"]]
        for need in ("run_command", "install_packages", "check_environment", "edit_file", "search_files"):
            if need not in names: problems.append("missing tool " + need)
        n = state["n"]
        if n > 0:
            if fmt == "anthropic":
                res = [b for b in msgs[-1]["content"] if b.get("type") == "tool_result"]
                check(n - 1, res[0]["content"] if res else "")
            else:
                check(n - 1, msgs[-1]["content"])
        state["n"] += 1
        if n < len(steps):
            name, inp = steps[n]
            cid = f"call_{n}"
            if fmt == "anthropic":
                out = {"type": "message", "role": "assistant", "stop_reason": "tool_use", "usage": {"input_tokens": 1, "output_tokens": 1},
                       "content": [{"type": "tool_use", "id": cid, "name": name, "input": inp}]}
            else:
                out = {"usage": {"prompt_tokens": 1, "completion_tokens": 1}, "choices": [{"finish_reason": "tool_calls", "message": {
                       "role": "assistant", "content": None, "tool_calls": [{"id": cid, "type": "function", "function": {"name": name, "arguments": json.dumps(inp)}}]}}]}
        else:
            summ = "OK" if not problems else "BAD: " + "; ".join(problems)
            if fmt == "anthropic":
                out = {"type": "message", "role": "assistant", "stop_reason": "tool_use", "usage": {"input_tokens": 1, "output_tokens": 1},
                       "content": [{"type": "tool_use", "id": "done", "name": "task_complete", "input": {"summary": summ}}]}
            else:
                out = {"usage": {"prompt_tokens": 1, "completion_tokens": 1}, "choices": [{"finish_reason": "tool_calls", "message": {
                       "role": "assistant", "content": None, "tool_calls": [{"id": "done", "type": "function", "function": {"name": "task_complete", "arguments": json.dumps({"summary": summ})}}]}}]}
        b = json.dumps(out).encode()
        self.send_response(200); self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
HTTPServer(("127.0.0.1", port), H).serve_forever()
