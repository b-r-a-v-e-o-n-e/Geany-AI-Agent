#!/usr/bin/env python3
"""Scripted fake Anthropic Messages API. Usage: mock_api.py PORT PROJECT_DIR LOGFILE"""
import json, sys
from http.server import BaseHTTPRequestHandler, HTTPServer
port, proj, logf = int(sys.argv[1]), sys.argv[2], sys.argv[3]
state = {"n": 0, "failed_once": False}
reqs = []

def tu(i, name, inp): return {"type": "tool_use", "id": f"toolu_{i}", "name": name, "input": inp}

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        reqs.append({"get": self.path, "headers": {k.lower(): v for k, v in self.headers.items()}})
        json.dump(reqs, open(logf, "w"))
        if self.path.startswith("/v1/models"):
            if "x-api-key" in self.headers and self.headers["x-api-key"] != "sk-ant-test":
                return self.send(401, {"type": "error", "error": {"type": "authentication_error", "message": "invalid x-api-key"}})
            return self.send(200, {"object": "list", "data": [{"id": "qwen2.5-coder-14b-instruct"}, {"id": "llama-3.1-8b-instruct"}, {"id": "gemma-3-12b"}],
                                   "models": [{"name": "x", "model": "x"}]})
        self.send(404, {"error": "nope"})
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        reqs.append({"headers": {k.lower(): v for k, v in self.headers.items()}, "body": body})
        json.dump(reqs, open(logf, "w"))
        if not state["failed_once"]:
            state["failed_once"] = True
            return self.send(529, {"type": "error", "error": {"type": "overloaded_error", "message": "Overloaded"}})
        state["n"] += 1
        n = state["n"]
        if self.path.endswith("/chat/completions"):
            return self.openai(n, body)
        usage = {"input_tokens": 10, "output_tokens": 5}
        if n == 1:
            c = [{"type": "text", "text": "Let me look."},
                 tu(1, "list_dir", {"path": proj}), tu(2, "read_file", {"path": proj + "/a.c"})]
            return self.send(200, {"type": "message", "role": "assistant", "content": c, "stop_reason": "tool_use", "usage": usage})
        newc = 'int main() {\n  const char *s = "\\u00e9 \\\\ { } [ ] кирилица";\n}\n'
        c = [tu(3, "write_file", {"path": proj + "/a.c", "content": newc}),
             tu(4, "write_file", {"path": proj + "/sub/new.txt", "content": "new\n"}),
             tu(5, "write_file", {"path": "/etc/evil", "content": "x"}),
             tu(6, "read_file", {"path": proj + "/secret/k.txt"}),
             tu(7, "task_complete", {"summary": 'All done — готово "ok"'})]
        self.send(200, {"type": "message", "role": "assistant", "content": c, "stop_reason": "tool_use", "usage": usage})
    def openai(self, n, body):
        usage = {"prompt_tokens": 10, "completion_tokens": 5}
        assert body["messages"][0]["role"] == "system" and body["tools"][0]["type"] == "function"
        def call(i, name, args, id_=True, as_obj=False):
            c = {"type": "function", "function": {"name": name, "arguments": args if as_obj else json.dumps(args)}}
            if id_: c["id"] = f"call_{i}"
            return c
        if n == 1:
            m = {"role": "assistant", "content": None, "tool_calls": [
                call(1, "list_dir", {"path": proj}), call(2, "read_file", {"path": proj + "/a.c"}, id_=False, as_obj=True)]}
            return self.send(200, {"choices": [{"index": 0, "message": m, "finish_reason": "tool_calls"}], "usage": usage})
        newc = 'int main() {\n  const char *s = "\\u00e9 \\\\ { } [ ] кирилица";\n}\n'
        m = {"role": "assistant", "content": "", "tool_calls": [
            call(3, "write_file", {"path": proj + "/a.c", "content": newc}),
            call(4, "write_file", {"path": proj + "/sub/new.txt", "content": "new\n"}),
            call(5, "write_file", {"path": "/etc/evil", "content": "x"}),
            call(6, "read_file", {"path": proj + "/secret/k.txt"}),
            {"id": "call_bad", "type": "function", "function": {"name": "read_file", "arguments": "{not json"}},
            call(7, "task_complete", {"summary": 'All done — готово "ok"'})]}
        return self.send(200, {"choices": [{"index": 0, "message": m, "finish_reason": "tool_calls"}], "usage": usage})
    def send(self, code, obj):
        b = json.dumps(obj, ensure_ascii=(code != 200)).encode()
        self.send_response(code); self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)

HTTPServer(("127.0.0.1", port), H).serve_forever()
