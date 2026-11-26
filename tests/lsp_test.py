#!/usr/bin/env python3
"""Drive the native fut language server as an editor would."""
import json
import queue
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class Client:
    def __init__(self, exe, log):
        self.process = subprocess.Popen([str(exe), "lsp"], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=log, cwd=ROOT)
        self.inbox = queue.Queue()
        self.pending = []
        self.request_id = 0
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        stream = self.process.stdout
        try:
            while header := stream.readline():
                name, size = header.split(b":", 1)
                assert name == b"Content-Length", header
                assert stream.readline() == b"\r\n"
                self.inbox.put(json.loads(stream.read(int(size))))
            self.inbox.put(EOFError("language server closed its stdout"))
        except Exception as error:
            self.inbox.put(error)

    def send(self, message, fragments=False):
        body = json.dumps({"jsonrpc": "2.0", **message}, ensure_ascii=fragments).encode()
        header = f"Content-Length: {len(body)}\r\n\r\n".encode()
        stream = self.process.stdin
        pieces = (header[:4], header[4:] + body[:5], body[5:]) if fragments else (header + body,)
        for part in pieces:
            stream.write(part)
            stream.flush()

    def receive(self, match):
        for i, message in enumerate(self.pending):
            if match(message):
                return self.pending.pop(i)
        deadline = time.monotonic() + 8
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(f"timed out waiting for a message, pending {self.pending}")
            try:
                message = self.inbox.get(timeout=remaining)
            except queue.Empty:
                raise AssertionError(f"timed out waiting for a message, pending {self.pending}") from None
            if isinstance(message, Exception):
                raise message
            if match(message):
                return message
            self.pending.append(message)

    def ask(self, method, params=None):
        self.request_id += 1
        request_id = f"request-{self.request_id}"
        self.send({"id": request_id, "method": method, "params": params or {}})
        return self.receive(lambda m: m.get("id") == request_id)["result"]

    def notify(self, method, params=None, fragments=False):
        self.send({"method": method, "params": params or {}}, fragments)

    def open(self, uri, text, fragments=False):
        self.notify("textDocument/didOpen",
                    {"textDocument": {"uri": uri, "languageId": "fut", "version": 1, "text": text}},
                    fragments)

    def change(self, uri, version, text):
        self.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": version},
                                               "contentChanges": [{"text": text}]})

    def doc(self, method, uri, line=None, character=None):
        params = {"textDocument": {"uri": uri}}
        if line is not None:
            params["position"] = {"line": line, "character": character}
        return self.ask(f"textDocument/{method}", params)

    def diagnostics(self, uri, version):
        message = self.receive(lambda m: m.get("method") == "textDocument/publishDiagnostics"
                               and m["params"]["uri"] == uri and m["params"].get("version") == version)
        return message["params"]["diagnostics"]

    def stop(self):
        assert self.ask("shutdown") is None
        self.notify("exit")
        assert self.process.wait(timeout=5) == 0


def exercise(client, exe, folder, timings):
    client.send({"id": 17, "method": "textDocument/documentSymbol"})
    assert client.receive(lambda m: m.get("id") == 17)["error"]["code"] == -32002
    result = client.ask("initialize", {"capabilities": {"textDocument": {
        "documentSymbol": {"hierarchicalDocumentSymbolSupport": True},
        "publishDiagnostics": {"relatedInformation": True},
    }}})
    caps = result["capabilities"]
    assert caps["textDocumentSync"]["change"] == 1 and caps["positionEncoding"] == "utf-16", caps
    assert caps["documentSymbolProvider"] and caps["hoverProvider"] and caps["documentFormattingProvider"] and "." in caps["completionProvider"]["triggerCharacters"]
    client.notify("initialized")
    client.send({"id": 18, "method": "textDocument/unknown"})
    assert client.receive(lambda m: m.get("id") == 18)["error"]["code"] == -32601

    local = (folder / "hello # world.fut").as_uri()
    bad = 'fn twice(x) { return x * 2 }\r\nvar note = "🦊"; twice(missing)\r\n'
    client.open(local, bad, fragments=True)
    [diagnostic] = client.diagnostics(local, 1)
    assert diagnostic["code"] == "undefined-name", diagnostic
    assert diagnostic["range"] == {"start": {"line": 1, "character": 23},
                                   "end": {"line": 1, "character": 30}}, diagnostic
    explain = client.doc("hover", local, 1, 25)["contents"]["value"]
    assert "undefined name 'missing'" in explain and "Fix:" in explain, explain
    assert "explanation" in diagnostic["data"], diagnostic

    # Two edits arriving together debounce into one check. The code itself would print and fail if run.
    fixed = bad.replace("missing", "3") + 'print(note)\nthrow "must not execute"\n'
    client.change(local, 2, bad + "oops\n")
    client.change(local, 3, fixed)
    assert client.diagnostics(local, 3) == []
    assert not any(m.get("params", {}).get("version") == 2 for m in client.pending), client.pending
    client.notify("textDocument/didSave", {"textDocument": {"uri": local}})
    assert client.diagnostics(local, 3) == []
    assert client.doc("hover", local, 0, 5)["contents"]["value"] == "fn twice(x)"
    items = {i["label"]: i for i in client.doc("completion", local, 0, 0)}
    assert items["fn"]["kind"] == 14 and items["twice"]["kind"] == 3 and items["note"]["kind"] == 6, items

    bench = ROOT / "examples" / "bench.fut"
    bench_uri = bench.as_uri()
    client.open(bench_uri, bench.read_text())
    assert client.diagnostics(bench_uri, 1) == []
    outline = client.doc("documentSymbol", bench_uri)
    rows = [json.loads(line) for line in subprocess.check_output([str(exe), "map", "--json", str(bench)], text=True, cwd=ROOT).splitlines()]
    expected = [(d.get("in", ""), d["name"], d["line"], d["end"]) for d in rows if "line" in d]

    def flatten(nodes, owner=""):
        for node in nodes:
            rng = node["range"]
            yield owner, node["name"], rng["start"]["line"] + 1, rng["end"]["line"] + 1
            yield from flatten(node.get("children", []), node["name"])

    assert list(flatten(outline)) == expected, outline
    vec = next(d for d in outline if d["name"] == "Vec")
    [init, add] = vec["children"]
    assert [init["name"], add["name"]] == ["init", "add"]
    assert add["selectionRange"]["start"] == {"line": 4, "character": 5}, add
    member_line = bench.read_text().splitlines()[-2]
    members = client.doc("completion", bench_uri, len(bench.read_text().splitlines()) - 2, member_line.index("v.add") + 2)
    assert any(i["label"] == "add" and i["kind"] == 2 for i in members), members

    root, inner, lib = (folder / name for name in ("root.fut", "inner.fut", "lib.fut"))
    inner.write_text('import "./lib.fut"\n')
    lib.write_text("pub fn fromLib() { return badName }\n")
    client.open(lib.as_uri(), lib.read_text())
    [own] = client.diagnostics(lib.as_uri(), 1)
    assert own["range"]["start"] == {"line": 0, "character": 26}, own
    client.open(root.as_uri(), '\n  import "./inner.fut"\n')
    [imported] = client.diagnostics(root.as_uri(), 1)
    assert imported["range"]["start"] == {"line": 1, "character": 2}, imported
    assert imported["relatedInformation"][0]["location"]["uri"] == lib.as_uri(), imported
    assert any(i["label"] == "fromLib" for i in client.doc("completion", root.as_uri(), 0, 0))
    client.change(lib.as_uri(), 2, "pub fn fromLib() { return 42 }\n")
    assert client.diagnostics(lib.as_uri(), 2) == []
    assert client.diagnostics(root.as_uri(), 1) == []
    client.notify("textDocument/didClose", {"textDocument": {"uri": lib.as_uri()}})
    assert client.diagnostics(lib.as_uri(), None) == []
    [again] = client.diagnostics(root.as_uri(), 1)
    assert again["code"] == "undefined-name", again

    # Resolve an import to a newly created buffer before the editor writes it to disk.
    new = (folder / "new.fut").as_uri()
    client.change(root.as_uri(), 2, 'import "./new.fut"\nfromNew()\n')
    assert client.diagnostics(root.as_uri(), 2)[0]["code"] == "import"
    client.open(new, "pub fn fromNew() { return 1 }\nfn hidden() { return nil }\n")
    assert client.diagnostics(new, 1) == []
    assert client.diagnostics(root.as_uri(), 2) == []
    plain = {i["label"] for i in client.doc("completion", root.as_uri(), 1, 0)}
    assert "fromNew" in plain and "hidden" not in plain, plain

    std = (folder / "with-std.fut").as_uri()
    valid_std = 'import "std/lists" as l\nvar sorted = l.sort([3, 1])\n'
    client.open(std, valid_std)
    assert client.diagnostics(std, 1) == []
    client.change(std, 2, valid_std.replace("l.sort([3, 1])", "l."))
    assert client.diagnostics(std, 2)
    items = {i["label"]: i for i in client.doc("completion", std, 1, 15)}
    assert items["sort"]["kind"] == 3 and items["sort"]["detail"].startswith("pub fn sort("), items
    client.change(std, 3, valid_std)
    assert client.diagnostics(std, 3) == []

    # Malformed or very deeply nested source must leave the protocol process alive.
    deep = (folder / "deep.fut").as_uri()
    client.open(deep, "var deep = " + "(" * 60000 + "1" + ")" * 60000 + "\n")
    client.diagnostics(deep, 1)
    client.change(deep, 2, "fn recovered() { return 1 }\n")
    assert client.diagnostics(deep, 2) == []
    assert client.doc("documentSymbol", deep)[0]["name"] == "recovered"

    # Formatting: one whole-buffer edit (its end in UTF-16 units), none for formatted or unlexable text.
    messy = (folder / "messy.fut").as_uri()
    client.open(messy, 'fn f(a,b){\nreturn a+b // 🦊\n}')
    client.diagnostics(messy, 1)
    [edit] = client.doc("formatting", messy)
    assert edit["newText"] == "fn f(a, b) {\n  return a + b // 🦊\n}\n", edit
    assert edit["range"] == {"start": {"line": 0, "character": 0}, "end": {"line": 2, "character": 1}}, edit
    client.change(messy, 2, edit["newText"])
    assert client.doc("formatting", messy) == []
    client.change(messy, 3, 'var s = "unclosed')
    assert client.doc("formatting", messy) == []

    if timings:
        for label, uri, version in [("examples/bench.fut", bench_uri, 1), ("buffer importing std/lists", std, 3)]:
            sample = []
            for _ in range(30):
                begin = time.perf_counter()
                client.notify("textDocument/didSave", {"textDocument": {"uri": uri}})
                assert client.diagnostics(uri, version) == []
                sample.append((time.perf_counter() - begin) * 1000)
            p95 = sorted(sample)[28]
            print(f"{label}: {statistics.median(sample):.2f} ms median, {p95:.2f} ms p95, {max(sample):.2f} ms max (30 saves)")
        sample = []
        for version in range(4, 34):
            begin = time.perf_counter()
            client.change(std, version, valid_std + f"// edit {version}\n")
            assert client.diagnostics(std, version) == []
            sample.append((time.perf_counter() - begin) * 1000)
        print(f"std/lists edits with debounce: {statistics.median(sample):.2f} ms median, {sorted(sample)[28]:.2f} ms p95")


def flat_symbols(client):
    client.ask("initialize")
    uri = (ROOT / "examples" / "bench.fut").as_uri()
    outline = client.doc("documentSymbol", uri)
    add = next(d for d in outline if d["name"] == "add")
    assert add["containerName"] == "Vec" and add["location"]["uri"] == uri, add


def main(exe, timings=False):
    with tempfile.TemporaryDirectory(prefix="fut-lsp-") as folder, tempfile.TemporaryFile() as log:
        for run in (lambda c: exercise(c, exe, Path(folder), timings), flat_symbols):
            client = Client(exe, log)
            try:
                run(client)
                client.stop()
            except Exception:
                log.seek(0)
                print(log.read().decode(errors="replace"), file=sys.stderr)
                raise
            finally:
                if client.process.poll() is None:
                    client.process.kill()
                client.process.wait()
    print("fut lsp stdio session passed")


if __name__ == "__main__":
    main(Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build" / "fut", "--timings" in sys.argv[2:])
