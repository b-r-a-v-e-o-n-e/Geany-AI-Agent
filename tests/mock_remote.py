#!/usr/bin/env python3
"""Anthropic-format mock that scripts a remote-target run: env check, install, write, build, run, complete. Usage: mock_remote.py PORT"""
import json, re, sys
from http.server import BaseHTTPRequestHandler, HTTPServer
port = int(sys.argv[1])

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        msgs = body["messages"]
        first = msgs[0]["content"]
        first = first if isinstance(first, str) else " ".join(b.get("text", "") for b in first)
        root = re.search(r"Permitted directories:\n  (\S+)", first).group(1)
        step = sum(1 for m in msgs if m["role"] == "assistant")
        seen_remote = "EXECUTION TARGET" in first
        steps = [
            ("check_environment", {"commands": ["gcc", "make"]}),
            ("install_packages", {"packages": ["cowsay"]}),
            ("write_file", {"path": root + "/hello.c", "content": '#include <stdio.h>\nint main(){puts("hello from the vm");return 0;}\n'}),
            ("run_command", {"command": "gcc -o hello hello.c && ./hello && uname -m"}),
            ("task_complete", {"summary": "built on the VM; remote=%s" % seen_remote}),
        ]
        if step >= len(steps):
            name, inp = "task_complete", {"summary": "done"}
        else:
            name, inp = steps[step]
        out = {"type": "message", "role": "assistant", "stop_reason": "tool_use", "usage": {"input_tokens": 1, "output_tokens": 1},
               "content": [{"type": "tool_use", "id": "t%d" % step, "name": name, "input": inp}]}
        b = json.dumps(out).encode()
        self.send_response(200); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
HTTPServer(("127.0.0.1", port), H).serve_forever()
