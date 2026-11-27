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

    def ask(self, method, params=None, *, error=False):
        self.request_id += 1
        request_id = f"request-{self.request_id}"
        self.send({"id": request_id, "method": method, "params": params or {}})
        response = self.receive(lambda m: m.get("id") == request_id)
        assert ("error" in response) == error, response
        return response["error" if error else "result"]

    def notify(self, method, params=None, fragments=False):
        self.send({"method": method, "params": params or {}}, fragments)

    def open(self, uri, text, fragments=False):
        self.notify("textDocument/didOpen",
                    {"textDocument": {"uri": uri, "languageId": "fut", "version": 1, "text": text}},
                    fragments)

    def change(self, uri, version, text):
        self.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": version},
                                               "contentChanges": [{"text": text}]})

    def doc(self, method, uri, line=None, character=None, *, error=False, **params):
        params["textDocument"] = {"uri": uri}
        if line is not None:
            params["position"] = {"line": line, "character": character}
        return self.ask(f"textDocument/{method}", params, error=error)

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

    # Two edits arriving together debounce into one check. Checking neither loads externs nor runs code.
    fixed = (bad.replace("missing", "3") + f'extern "{folder / "missing.dylib"}" fn outside(): void\n'
             'test "check only" { throw "must not run tests" }\nprint(note)\nthrow "must not execute"\n')
    client.change(local, 2, bad + "oops\n")
    client.change(local, 3, fixed)
    assert client.diagnostics(local, 3) == []
    assert not any(m.get("params", {}).get("version") == 2 for m in client.pending), client.pending
    client.notify("textDocument/didSave", {"textDocument": {"uri": local}})
    assert client.diagnostics(local, 3) == []
    assert client.doc("hover", local, 0, 5)["contents"]["value"] == "fn twice(x)"
    items = {i["label"]: i for i in client.doc("completion", local, 0, 0)}
    assert items["fn"]["kind"] == 14 and items["twice"]["kind"] == 3 and items["note"]["kind"] == 6, items

    multiple = (folder / "multiple.fut").as_uri()
    broken = """fn broken() {
  var damaged = )
  print(missing)
}
fn target(x) { return x }
fn later() {
  return target()
}
"""
    expected = [("syntax", 1, 16, 17), ("undefined-name", 2, 8, 15), ("arity", 6, 9, 15)]

    def check_errors(uri, version, expected):
        diagnostics = client.diagnostics(uri, version)
        assert [(d["code"], d["range"]) for d in diagnostics] == [
            (code, {"start": {"line": line, "character": start}, "end": {"line": line, "character": end}})
            for code, line, start, end in expected
        ], diagnostics

    client.open(multiple, broken)
    check_errors(multiple, 1, expected)
    client.change(multiple, 2, broken.replace("damaged = )", "damaged = 1"))
    check_errors(multiple, 2, expected[1:])

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
    lib.write_text("pub fn fromLib() { return badName }\nfn other() { return alsoBad }\n")
    client.open(lib.as_uri(), lib.read_text())
    check_errors(lib.as_uri(), 1, [("undefined-name", 0, 26, 33), ("undefined-name", 1, 20, 27)])
    client.open(root.as_uri(), '\n  import "./inner.fut"\nimport "./inner.fut"\n')
    imported = client.diagnostics(root.as_uri(), 1)
    assert [d["range"]["start"] for d in imported] == [{"line": 1, "character": 2}, {"line": 2, "character": 0}], imported
    for d in imported:
        [related] = d["relatedInformation"]
        assert related["location"] == {
            "uri": lib.as_uri(),
            "range": {"start": {"line": 0, "character": 26}, "end": {"line": 0, "character": 33}},
        }, d
    assert any(i["label"] == "fromLib" for i in client.doc("completion", root.as_uri(), 0, 0))
    client.change(lib.as_uri(), 2, "pub fn fromLib() { return 42 }\n")
    assert client.diagnostics(lib.as_uri(), 2) == []
    assert client.diagnostics(root.as_uri(), 1) == []
    client.notify("textDocument/didClose", {"textDocument": {"uri": lib.as_uri()}})
    assert client.diagnostics(lib.as_uri(), None) == []
    assert [d["code"] for d in client.diagnostics(root.as_uri(), 1)] == ["undefined-name", "undefined-name"]

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


def navigation(client, folder):
    caps = client.ask("initialize")["capabilities"]
    assert all(caps[k] for k in ("definitionProvider", "referencesProvider", "documentHighlightProvider"))
    assert caps["renameProvider"]["prepareProvider"]
    lib = folder / "nav # library.fut"
    app = folder / "nav app.fut"
    other = folder / "nav other.fut"
    library = """pub var count: num = 3
pub fn increment(n: num): num { return n + count }
pub class Bucket {
  var value: num = 0
  fn read(): num { return self.value }
  fn twice(): num { return self.read() * 2 }
}
pub enum Shape { Circle(radius: num), Empty }
pub type Size = num
pub effect Log { fn emit(value: num) }
"""
    program = """import "./nav # library.fut" as m
import "std/lists" as l
var count = 12
fn inspect(arg: m.Bucket, measure: m.Size): num {
  var obj = m.Bucket()
  var typed: m.Bucket = obj
  var shape: m.Shape.Circle = m.Shape.Circle(1)
  var scale: num = 2
  { var count = 5; print(count) }
  fn capture(x: num) { return x + scale }
  print("🦊 {count} {m.count} {capture(1)}")
  print(typed.value, obj.read(), arg.read(), m.Bucket.read, shape.radius)
  m.Log.emit(measure)
  return m.increment(count)
}
var sorted = l.sort([3, 1])
"""
    sibling = 'import "./nav # library.fut"\nvar answer = increment(count)\n'
    # Disk holds an older declaration. Navigation and rename must use the open buffer.
    lib.write_text("\n\npub var count = 0\n")
    client.open(lib.as_uri(), library)
    client.open(app.as_uri(), program)
    client.open(other.as_uri(), sibling)
    for path in (lib, app, other):
        assert client.diagnostics(path.as_uri(), 1) == [], path

    def site(text, line, word, after=""):
        row = text.splitlines()[line]
        start = row.index(word, row.index(after) + len(after) if after else 0)
        return line, len(row[:start].encode("utf-16-le")) // 2

    def range_for(text, line, word, after=""):
        _, col = site(text, line, word, after)
        return {"start": {"line": line, "character": col},
                "end": {"line": line, "character": col + len(word)}}

    def loc(path, text, line, word, after=""):
        return {"uri": path.as_uri(), "range": range_for(text, line, word, after)}

    def request(method, path, text, line, word, after="", **params):
        return client.doc(method, path.as_uri(), *site(text, line, word, after), **params)

    def definition(line, word, target, content, dest, after=""):
        return request("definition", app, program, line, word, after) == loc(target, content, dest, word)

    assert definition(13, "count", app, program, 2)
    assert definition(8, "count", app, program, 8, "print(")
    assert definition(9, "scale", app, program, 7)
    assert definition(12, "measure", app, program, 3)
    assert definition(4, "Bucket", lib, library, 2)
    assert definition(3, "Size", lib, library, 8)
    assert definition(6, "Circle", lib, library, 7, "= ")
    assert definition(11, "radius", lib, library, 7)
    assert definition(11, "value", lib, library, 3)
    assert definition(11, "read", lib, library, 4, "obj.")
    assert definition(11, "read", lib, library, 4, "arg.")
    assert definition(11, "read", lib, library, 4, "Bucket.")
    assert definition(12, "Log", lib, library, 9)
    assert definition(12, "emit", lib, library, 9)
    assert request("definition", lib, library, 4, "value") == loc(lib, library, 3, "value")
    assert request("definition", lib, library, 5, "read") == loc(lib, library, 4, "read")
    assert request("definition", app, program, 10, "m", "{") == loc(app, program, 0, "m", " as ")
    assert definition(10, "count", lib, library, 0, "m.")
    std = ROOT / "std" / "lists" / "lists.fut"
    std_definition = request("definition", app, program, 15, "sort", "l.")
    assert std_definition["uri"] == std.as_uri(), std_definition
    std_line = std_definition["range"]["start"]["line"]
    assert std.read_text().splitlines()[std_line].startswith("pub fn sort(")

    for line, word, after, expected in (
        (3, "arg", "(", "arg: m.Bucket"),
        (7, "scale", "", "var scale: num"),
        (9, "scale", "", "var scale: num"),
        (12, "measure", "", "measure: m.Size"),
        (9, "x", "return ", "x: num"),
        (10, "count", "m.", "var count: num"),
        (11, "typed", "", "var typed: m.Bucket"),
        (13, "increment", "", "fn increment(n: num): num"),
    ):
        assert request("hover", app, program, line, word, after)["contents"]["value"] == expected

    references = [loc(app, program, 10, "count", "m."), loc(lib, library, 1, "count"), loc(other, sibling, 1, "count")]
    for include in (False, True):
        found = request("references", lib, library, 0, "count", context={"includeDeclaration": include})
        expected = references + ([loc(lib, library, 0, "count")] if include else [])
        assert sorted(found, key=str) == sorted(expected, key=str), found
    assert request("documentHighlight", app, program, 10, "count") == [
        {"range": range_for(program, 2, "count"), "kind": 3},
        {"range": range_for(program, 10, "count"), "kind": 2},
        {"range": range_for(program, 13, "count"), "kind": 2},
    ]
    assert request("prepareRename", app, program, 10, "count", "m.") == {
        "range": range_for(program, 10, "count", "m."), "placeholder": "count"}

    rename = request("rename", lib, library, 0, "count", newName="total")["changes"]
    assert rename == {
        app.as_uri(): [{"range": range_for(program, 10, "count", "m."), "newText": "total"}],
        lib.as_uri(): [{"range": range_for(library, line, "count"), "newText": "total"} for line in (0, 1)],
        other.as_uri(): [{"range": range_for(sibling, 1, "count"), "newText": "total"}],
    }, rename
    rename = request("rename", app, program, 2, "count", newName="tally")["changes"]
    assert rename == {
        app.as_uri(): [{"range": range_for(program, line, "count"), "newText": "tally"} for line in (2, 10, 13)],
    }, rename
    assert "collid" in request("rename", app, program, 7, "scale", newName="x", error=True)["message"]
    assert "keyword" in request("rename", lib, library, 0, "count", newName="while", error=True)["message"]
    assert "keyword" in request("rename", lib, library, 0, "count", newName="match", error=True)["message"]
    assert "standard library" in request("rename", app, program, 15, "sort", "l.", newName="order", error=True)["message"]
    assert "standard library" in request("prepareRename", app, program, 15, "sort", "l.", error=True)["message"]

    # Check a changed import immediately, before the diagnostics debounce fires.
    client.change(lib.as_uri(), 2, "\n" + library)
    assert definition(10, "count", lib, "\n" + library, 1, "m.")
    broken = program + "var unrelated = missing\n"
    client.change(app.as_uri(), 2, broken)
    assert client.diagnostics(app.as_uri(), 2)
    assert request("definition", app, broken, 13, "increment") == loc(lib, "\n" + library, 2, "increment")
    assert request("definition", app, broken, 16, "missing") is None

    # Open dependencies take precedence, and a closed dependency is rechecked if its disk version changes.
    client.notify("textDocument/didClose", {"textDocument": {"uri": lib.as_uri()}})
    assert client.diagnostics(lib.as_uri(), None) == []
    lib.write_text("\n\n" + library)
    target = request("definition", app, broken, 10, "count", "m.")
    assert target == loc(lib.resolve(), "\n\n" + library, 2, "count"), target

    scope = folder / "scope.fut"
    scoped = """var total = 0
fn set(a: num, b: num)
  requires a >= 0
{
  total = a
  var a = 4
  total += a
}
import "std/lists"
var spare = 1
"""
    client.open(scope.as_uri(), scoped)
    assert client.diagnostics(scope.as_uri(), 1) == []
    assert request("definition", scope, scoped, 2, "a", "requires ") == loc(scope, scoped, 1, "a")
    assert "collid" in request("rename", scope, scoped, 1, "a", newName="b", error=True)["message"]
    assert "collid" in request("rename", scope, scoped, 9, "spare", newName="lists", error=True)["message"]
    assert request("documentHighlight", scope, scoped, 0, "total") == [
        {"range": range_for(scoped, line, "total"), "kind": 3} for line in (0, 4, 6)
    ]

    model = folder / "model.fut"
    nested = """class Pointer { var hover }
class Other { var hover }
class Base { var pointer: Pointer }
class Game : Base {}
fn use(g: Game, o: Other, unknown) {
  var p = g.pointer
  p.hover = g.pointer.hover
  o.hover = 1
  p = unknown
  p.hover = 2
}
"""
    client.open(model.as_uri(), nested)
    assert client.diagnostics(model.as_uri(), 1) == []
    field = loc(model, nested, 0, "hover")
    assert request("definition", model, nested, 6, "hover", "g.pointer.") == field
    assert request("definition", model, nested, 6, "hover") == field
    edits = request("rename", model, nested, 0, "hover", newName="hovered")["changes"][model.as_uri()]
    assert edits == [{"range": range_for(nested, line, "hover", after), "newText": "hovered"}
                     for line, after in [(0, ""), (6, ""), (6, "g.pointer.")]], edits


def view_navigation(client, folder):
    client.ask("initialize")
    uri = (folder / "widget.fut").as_uri()
    source = """state count = 1
fn compute(x) { return x + count }
view Widget(title) {
  state local = 0
  button title on click { local += compute(count) }
  text "🌱 {title}: {local}"
  for item in [1, 2] key item { text "{item}" }
}
fn buildWidget() { return Widget("leaf") }
"""
    client.open(uri, source)
    assert client.diagnostics(uri, 1) == []

    def bounds(line, word, *, last=False):
        row = source.splitlines()[line]
        offset = row.rindex(word) if last else row.index(word)
        col = len(row[:offset].encode("utf-16-le")) // 2
        return {"start": {"line": line, "character": col},
                "end": {"line": line, "character": col + len(word)}}

    def request(method, line, word, *, last=False, **params):
        pos = bounds(line, word, last=last)["start"]
        return client.doc(method, uri, pos["line"], pos["character"], **params)

    for use, word, declaration, last in ((4, "compute", 1, False), (5, "title", 2, False),
                                          (5, "local", 3, False), (6, "item", 6, True),
                                          (8, "Widget", 2, True)):
        assert request("definition", use, word, last=last) == {"uri": uri, "range": bounds(declaration, word)}
    outline = {d["name"]: d["kind"] for d in client.doc("documentSymbol", uri)}
    assert outline["Widget"] == 12 and outline["count"] == 13, outline
    assert request("hover", 8, "Widget", last=True)["contents"]["value"] == "view Widget(title)"
    assert request("documentHighlight", 5, "local") == [
        {"range": bounds(line, "local"), "kind": kind} for line, kind in ((3, 3), (4, 3), (5, 2))
    ]
    rename = request("rename", 5, "title", newName="caption")["changes"]
    assert rename == {uri: [{"range": bounds(line, "title"), "newText": "caption"} for line in (2, 4, 5)]}

    source = source.replace("{title}", "{missing}").replace("compute(count)", "unknown(count)")
    client.change(uri, 2, source)
    diagnostics = client.diagnostics(uri, 2)
    assert [(d["code"], d["range"]) for d in diagnostics] == [
        ("undefined-name", bounds(4, "unknown")), ("undefined-name", bounds(5, "missing"))
    ], diagnostics


def flat_symbols(client):
    client.ask("initialize")
    uri = (ROOT / "examples" / "bench.fut").as_uri()
    outline = client.doc("documentSymbol", uri)
    add = next(d for d in outline if d["name"] == "add")
    assert add["containerName"] == "Vec" and add["location"]["uri"] == uri, add


def main(exe, timings=False):
    with tempfile.TemporaryDirectory(prefix="fut-lsp-") as folder, tempfile.TemporaryFile() as log:
        for run in (lambda c: exercise(c, exe, Path(folder), timings), lambda c: navigation(c, Path(folder)),
                    lambda c: view_navigation(c, Path(folder)), flat_symbols):
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
