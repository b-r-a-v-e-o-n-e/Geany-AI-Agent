#!/usr/bin/env python3
"""Conversation mock (Anthropic or OpenAI format) that REJECTS invalid histories. Usage: mock_chat.py PORT PROJ"""
import json, sys
from http.server import BaseHTTPRequestHandler, HTTPServer
port, proj = int(sys.argv[1]), sys.argv[2]

def validate_anthropic(msgs):
    prev = None
    for i, m in enumerate(msgs):
        if m["role"] == prev: return f"two consecutive {prev} messages at {i}"
        prev = m["role"]
        if m["role"] == "assistant" and isinstance(m["content"], list):
            ids = [b["id"] for b in m["content"] if b.get("type") == "tool_use"]
            if ids:
                nxt = msgs[i + 1] if i + 1 < len(msgs) else None
                if not nxt or not isinstance(nxt["content"], list): return f"tool_use without tool_result at {i}"
                got = [b["tool_use_id"] for b in nxt["content"] if b.get("type") == "tool_result"]
                if sorted(got) != sorted(ids): return f"tool_result mismatch at {i}"
                if any(b.get("type") == "text" for b in nxt["content"][:len(got)]): return "text before tool_result"
    return None

def validate_openai(msgs):
    for i, m in enumerate(msgs):
        if m["role"] == "assistant" and m.get("tool_calls"):
            ids = [c["id"] for c in m["tool_calls"]]
            j = i + 1; got = []
            while j < len(msgs) and msgs[j]["role"] == "tool": got.append(msgs[j]["tool_call_id"]); j += 1
            if sorted(got) != sorted(ids): return f"tool message mismatch at {i}"
    return None

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def reply(self, code, obj):
        b = json.dumps(obj).encode()
        self.send_response(code); self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        msgs = body["messages"]; oa = self.path.endswith("/chat/completions")
        bad = (validate_openai if oa else validate_anthropic)(msgs)
        if bad: return self.reply(400, {"error": {"message": "INVALID HISTORY: " + bad}})
        # find the newest plain user text
        last = msgs[-1]; text = ""
        if oa:
            if last["role"] == "user": text = last["content"]
            tool_res = last["role"] == "tool"
        else:
            tool_res = isinstance(last["content"], list) and any(b.get("type") == "tool_result" for b in last["content"])
            if isinstance(last["content"], str): text = last["content"]
            elif isinstance(last["content"], list): text = " ".join(b.get("text", "") for b in last["content"] if b.get("type") == "text")
        n = len(msgs)
        if text.startswith("Task:") and not tool_res:        # first message of a conversation
            call = ("list_dir", {"path": proj}, "c1")
        elif tool_res and not text.strip() and "list_dir" in json.dumps(msgs[-3:]) and "ack" not in json.dumps(msgs[-2:]):
            call = ("task_complete", {"summary": "first done"}, "c2")
        else:
            call = None
        if call:
            name, inp, cid = call
            if oa:
                return self.reply(200, {"usage": {"prompt_tokens": 1, "completion_tokens": 1}, "choices": [{"finish_reason": "tool_calls", "message": {
                    "role": "assistant", "content": None, "tool_calls": [{"id": f"{cid}_{n}", "type": "function", "function": {"name": name, "arguments": json.dumps(inp)}}]}}]})
            return self.reply(200, {"type": "message", "role": "assistant", "stop_reason": "tool_use", "usage": {"input_tokens": 1, "output_tokens": 1},
                                    "content": [{"type": "tool_use", "id": f"{cid}_{n}", "name": name, "input": inp}]})
        ack = "ack: " + text.strip()[-60:] + f" [{n} msgs]"
        if oa:
            return self.reply(200, {"usage": {"prompt_tokens": 1, "completion_tokens": 1}, "choices": [{"finish_reason": "stop", "message": {"role": "assistant", "content": ack}}]})
        self.reply(200, {"type": "message", "role": "assistant", "stop_reason": "end_turn", "usage": {"input_tokens": 1, "output_tokens": 1}, "content": [{"type": "text", "text": ack}]})
HTTPServer(("127.0.0.1", port), H).serve_forever()
