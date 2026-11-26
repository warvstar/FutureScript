#!/usr/bin/env python3
"""A stand-in for the Messages API, to dry-run the swarm for free: python3 swarm/mock.py [port].
Planner: two tasks. Approver: approves all. Worker: writes a module and a test, runs the tests, calls done.
Reviewer: rejects a task once, then approves."""
import json, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

reviews = {}


def reply(content, stop="tool_use"):
    return {"id": "msg_mock", "type": "message", "role": "assistant", "model": "mock", "content": content,
            "stop_reason": stop, "usage": {"input_tokens": 1000, "output_tokens": 200, "cache_read_input_tokens": 500}}


def use(name, inp, i=0):
    return {"type": "tool_use", "id": f"toolu_{name}_{i}", "name": name, "input": inp}


class H(BaseHTTPRequestHandler):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["content-length"])))
        tools = [t["name"] for t in body.get("tools", [])]
        msgs = body["messages"]
        first = msgs[0]["content"] if isinstance(msgs[0]["content"], str) else ""
        if "propose" in tools:
            out = reply([use("propose", {"tasks": [
                {"title": f"module {k}", "detail": f"pub fn f{k}(x) returning x + {k}, with a test", "files": [f"src/m{k}.fut"]}
                for k in (1, 2)]})])
        elif "decide" in tools:
            out = reply([use("decide", {"decisions": [{"index": 0, "approve": True, "reason": "ok"},
                                                      {"index": 1, "approve": True, "reason": "ok", "amendment": "keep it small"}]})])
        elif "verdict" in tools:
            key = first.split(":")[0]
            reviews[key] = reviews.get(key, 0) + 1
            ok = reviews[key] > 1
            out = reply([use("verdict", {"approve": ok, "feedback": "add a second expect" if not ok else "good"})])
        else:
            k = "1" if "module 1" in first else "2"
            turns = sum(1 for m in msgs if m["role"] == "assistant")
            if turns == 0:
                out = reply([{"type": "text", "text": "Writing it."},
                             use("write", {"path": f"src/m{k}.fut", "content": f"pub fn f{k}(x) {{ return x + {k} }}\n"}, 1),
                             use("write", {"path": f"tests/m{k}_test.fut",
                                           "content": f'import "../src/m{k}.fut"\ntest "f{k}" {{ expect f{k}(1) == {1 + int(k)} }}\n'}, 2),
                             use("edit", {"path": "all.fut", "old": "project\n", "new": f"project\nimport \"src/m{k}.fut\"\n"}, 3)])
            elif turns == 1:
                out = reply([use("test", {}, 4), use("map", {}, 5)])
            elif turns == 2:
                out = reply([use("done", {"summary": f"added f{k}"}, 6)])
            else:
                out = reply([use("done", {"summary": f"added f{k}, again"}, 7 + turns)])
        data = json.dumps(out).encode()
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, format, *args):
        pass


ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 8787), H).serve_forever()
