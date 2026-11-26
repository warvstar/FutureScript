#!/usr/bin/env python3
"""Build and test fut.   python3 build.py [build | debug | wasm | test [--debug | --wasm] | fuzz [N] | clean]"""
import json, os, random, re, shutil, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent
OUT = ROOT / "build" / "fut"
FLAGS = ["-std=c++23", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-missing-field-initializers", "-Wno-invalid-offsetof"]


# The WebAssembly build uses Homebrew's LLVM and WASI libc/libc++ (brew install llvm lld wasi-libc wasi-runtimes).
LLVM = Path("/opt/homebrew/opt/llvm/bin/clang++")
WASM_FLAGS = ["--target=wasm32-wasip1", "--sysroot=/opt/homebrew/share/wasi-sysroot", "-fno-exceptions", "-mexception-handling",
              "-mllvm", "-wasm-enable-sjlj"]
WASM_LINK = ["-mexec-model=reactor", "-lsetjmp", "-Wl,--export=__stack_pointer", "-Wl,--export-table", "-Wl,--growable-table",
             "-Wl,-z,stack-size=8388608", "-Wl,--stack-first"]
NATIVE_ONLY, WASM_ONLY = {"main.cpp", "jit.cpp"}, {"wasm.cpp"}


def build(debug=False, wasm=False):
    """Compile each src/*.cpp to its own object in parallel (only stale ones), then link."""
    out = ROOT / "build" / "fut.wasm" if wasm else OUT.with_name("fut-debug") if debug else OUT
    objdir = ROOT / "build" / ("obj-wasm" if wasm else "obj-debug" if debug else "obj")
    objdir.mkdir(parents=True, exist_ok=True)
    opt = ["-O2", *WASM_FLAGS] if wasm else ["-O1", "-g", "-fsanitize=address,undefined"] if debug else ["-O2"]
    cxx = str(LLVM) if wasm else "clang++"
    skip = NATIVE_ONLY if wasm else WASM_ONLY
    shared = max(p.stat().st_mtime for p in [*(ROOT / "src").glob("*.h"), Path(__file__)])
    objs = [(c, objdir / (c.stem + ".o")) for c in sorted((ROOT / "src").glob("*.cpp")) if c.name not in skip]
    stale = [(c, o) for c, o in objs if not o.exists() or o.stat().st_mtime < max(shared, c.stat().st_mtime)]

    def cc(job):
        print(("wasm " if wasm else "") + "clang++", job[0].name, flush=True)
        return subprocess.run([cxx, *FLAGS, *opt, "-c", str(job[0]), "-o", str(job[1])]).returncode
    with ThreadPoolExecutor(len(stale) or 1) as ex:
        if any(list(ex.map(cc, stale))):
            sys.exit(1)
    if stale or not out.exists():
        link = WASM_LINK if wasm else ["-lobjc"]
        subprocess.run([cxx, *(o for o in opt if o not in ("-mllvm", "-wasm-enable-sjlj")), *(str(o) for _, o in objs), *link, "-o", str(out)], check=True)
    return out


def runner(exe):
    """The command that runs a fut program: the native binary, or the WebAssembly build under Node."""
    return ["node", str(ROOT / "web" / "run.mjs")] if exe.suffix == ".wasm" else [str(exe)]


def test(exe):
    """Each tests/*.fut lists `// expect: <line>` for stdout and optionally `// error: <text>` lines for stderr.
    Each tests/specs/*.fut is run with `fut test` and must pass."""
    failed = 0
    for t in sorted((ROOT / "tests").glob("*.fut")):
        src = t.read_text()
        want = re.findall(r"// expect: ?(.*)", src)
        err = re.findall(r"// error: ?(.*)", src)
        r = subprocess.run([*runner(exe), str(t)], capture_output=True, text=True, timeout=60)
        ok = r.stdout.splitlines() == want and (all(e in r.stderr for e in err) and r.returncode != 0 if err else r.returncode == 0)
        print(f"{'ok  ' if ok else 'FAIL'} {t.name}")
        if not ok:
            failed += 1
            print(f"  want: {want}\n  got:  {r.stdout.splitlines()}\n  stderr: {r.stderr.strip()}")
    for t in sorted((ROOT / "tests" / "specs").glob("*.fut")):  # specs written with test blocks
        r = subprocess.run([*runner(exe), "test", str(t)], capture_output=True, text=True, timeout=60)
        print(f"{'ok  ' if r.returncode == 0 else 'FAIL'} specs/{t.name} ({r.stdout.strip().splitlines()[-1] if r.stdout.strip() else 'no output'})")
        if r.returncode:
            failed += 1
            print("  " + (r.stdout + r.stderr).strip().replace("\n", "\n  "))
    for name, fn in HOT_TESTS + RUNNER_TESTS:
        ok, detail = fn(exe)
        print(f"{'ok  ' if ok else 'FAIL'} {'' if fn in [f for _, f in RUNNER_TESTS] else 'hot reload: '}{name}")
        if not ok:
            failed += 1
            print("  " + detail)
    print("all tests passed" if not failed else f"{failed} failed")
    return failed


def live(exe, files, edits, settle=0.4, timeout=6):
    """Run files[0] with -w, apply edits [(file, text)] one at a time while it runs (file "<stdin>" types the
    text into its live REPL); return (stdout, stderr)."""
    d = ROOT / "build" / "hot"
    d.mkdir(parents=True, exist_ok=True)
    for name, text in files.items():
        (d / name).parent.mkdir(parents=True, exist_ok=True)
        (d / name).write_text(text)
    p = subprocess.Popen([*runner(exe), "-w", str(d / next(iter(files)))], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    for name, text in edits:
        time.sleep(settle)
        if name == "<stdin>":
            p.stdin.write(text), p.stdin.flush()
        else:
            (d / name).write_text(text)
    try:
        out, err = p.communicate(timeout=timeout if p.poll() is None else 1)
    except subprocess.TimeoutExpired:
        p.kill()
        out, err = p.communicate()
    return out, err


def hot_methods_and_main(exe):
    """Patch a method; a syntax error keeps the old code running; editing main() restarts it."""
    prog = ("class Gen {{ fn value() {{ return {v} }} }}\n"
            "fn main() {{ var g = Gen() var last = 0\n"
            "  while true {{ var v = g.value()\n"
            "    if v != last {{ print(\"{{v}}{tag}\") last = v }}\n"
            "    if v == 3 {{ return }}\n"
            "    sleep(0.01) }} }}\n")
    out, err = live(exe, {"a.fut": prog.format(v=1, tag="a")},
                    [("a.fut", prog.format(v=2, tag="a")), ("a.fut", prog.format(v="(", tag="a")),
                     ("a.fut", prog.format(v=2, tag="b")), ("a.fut", prog.format(v=3, tag="b"))])
    ok = out.split() == ["1a", "2a", "2b", "3b"] and "restarting" in err and "error: unexpected" in err
    return ok, f"stdout: {out!r}\n  stderr: {err!r}"


def hot_imports_stale_hook(exe):
    """Edits to an imported file patch in; a no-op save changes nothing; reload() runs after each patch;
    changing a function's parameters while an old frame still calls it is a clean runtime error."""
    main = ("import \"lib.fut\"\n"
            "fn reload() { print(\"hook\") }\n"
            "fn spin() { var last = 0\n"
            "  while true { var r = f(1)\n"
            "    if r != last { print(r) last = r }\n"
            "    sleep(0.01) } }\n"
            "fn main() { spin() }\n")
    out, err = live(exe, {"m.fut": main, "lib.fut": "pub fn f(a) { return a }\n"},
                    [("lib.fut", "pub fn f(a) { return a + 1 }\n"), ("m.fut", main),
                     ("lib.fut", "pub fn f(a, b) { return b }\n"), ("m.fut", main.replace("f(1)", "f(1, 7)")),
                     ("m.fut", main.replace("f(1)", "f(1, 7)") + "\n")], timeout=1)
    # lib change -> patch + hook; no-op save; arity change alone fails to compile (m.fut still calls f(1));
    # fixing the caller patches it, but spin()'s running frame is old code -> stale-call error; the next
    # save reruns main() and the new spin() prints 7.
    ok = (out.split()[:3] == ["1", "hook", "2"] and out.split()[-1] == "7" and "no code changes" in err
          and "expects 2 arguments" in err and "stale call to 'f'" in err)
    return ok, f"stdout: {out!r}\n  stderr: {err!r}"


def hot_repl_and_reset(exe):
    """Lines typed into a running program run in it; editing a var's declaration resets it (other state survives);
    functions defined at the REPL outlive file reloads; after a runtime error the REPL can still inspect state."""
    prog = ("class P {{ fn init(hp) {{ self.hp = hp }} }}\nvar p = P(10)\nvar speed = {s}\nvar hits = 0\n"
            "fn main() {{ var n = 0\n  while n < 100 {{ n += 1 hits += 1 sleep(0.02) }}\n  print(\"end {{p.hp}} {{speed}}\")\n  nil.x }}\n")
    out, err = live(exe, {"g.fut": prog.format(s=5)},
                    [("<stdin>", "p.hp\np.hp = 99\nfn twice(x) {\n  return x * 2 }\n"), ("<stdin>", "twice(speed)\n"),
                     ("g.fut", prog.format(s=7)), ("<stdin>", "twice(speed)\nhits > 0\n")] + [("<stdin>", "")] * 6 +
                    [("<stdin>", "speed + 1\n")], settle=0.3, timeout=1)
    ok = out.split() == ["10", "10", "14", "true", "end", "99", "7", "8"] and "reset speed" in err and "nil has no field" in err
    return ok, f"stdout: {out!r}\n  stderr: {err!r}"


def hot_reclaim(exe):
    """Many reloads of a running program must not grow live machine code without bound."""
    prog = "fn body() {{ var x = {n} {pad} return x }}\nfn main() {{ while true {{ body() sleep(0.005) }} }}\n"
    pad = " ".join(f"x = x + {i}" for i in range(400))
    out, err = live(exe, {"r.fut": prog.format(n=0, pad=pad)},
                    [("r.fut", prog.format(n=i, pad=pad)) for i in range(1, 25)], settle=0.25, timeout=1)
    sizes = [int(m) for m in re.findall(r"(\d+) KB code", err)]
    ok = len(sizes) >= 20 and max(sizes) <= 2 * sizes[0] + 64
    return ok, f"code sizes KB: {sizes}\n  stderr tail: {err[-300:]!r}"


def hot_reload_rollback(exe):
    """A reload whose new top level fails is undone: the data it changed and its code. The next good reload
    initializes the var the failed one added."""
    head = "var hp = 10\nvar hist = []\n"
    main = "fn main() { var last = 0\n  while true { var v = tick()\n    if v != last { print(\"v{v} hp{hp}\") last = v }\n    if v == 3 { return }\n    sleep(0.01) } }\n"
    bad = head + "fn fail() { hp = 0 hist.push(1) throw \"migration failed\" }\nvar boom = fail()\nfn tick() { return 2 }\n" + main
    good = head + "var boom = 5\nfn tick() { return boom - 2 + len(hist) }\n" + main
    out, err = live(exe, {"t.fut": head + "fn tick() { return 1 }\n" + main}, [("t.fut", bad), ("t.fut", good)])
    ok = out.split() == ["v1", "hp10", "v3", "hp10"] and "migration failed" in err and "reload rolled back" in err
    return ok, f"stdout: {out!r}\n  stderr: {err!r}"


def hot_result_type(exe):
    """Callers compiled while a function declared `: num` rely on that; changing its result type recompiles them."""
    prog = ("fn half(n){sig} {{ return {body} }}\n"
            "fn show() {{ return half(4) + 1 }}\n"
            "fn main() {{ var last = 0\n"
            "  while true {{ var v = show()\n"
            "    if v != last {{ print(v) last = v }}\n"
            "    if v == \"h1\" {{ return }}\n"
            "    sleep(0.01) }} }}\n")
    out, err = live(exe, {"r.fut": prog.format(sig=": num", body="n / 2")}, [("r.fut", prog.format(sig=": str", body="\"h\""))])
    ok = out.split() == ["3", "h1"]
    return ok, f"stdout: {out!r}\n  stderr: {err!r}"


def hot_var_type(exe):
    """A top-level var's type is checked at stores from the REPL too; changing it recompiles the functions that store."""
    prog = ("var level{sig}\nfn set(v) {{ level = v }}\n"
            "fn main() {{ while true {{ try {{ set(\"s\") print(\"ok {{level}}\") return }} catch e {{ }} sleep(0.01) }} }}\n")
    out, err = live(exe, {"v.fut": prog.format(sig=": num = 0")},
                    [("<stdin>", "level = \"x\"\nlevel\n"), ("v.fut", prog.format(sig=": str = \"a\""))])
    ok = out.split() == ["0", "ok", "s"] and "var 'level' must be num, got the string \"x\"" in err
    return ok, f"stdout: {out!r}\n  stderr: {err!r}"


def hot_effects(exe):
    """Changing a function's declared effects takes effect at the next call, like a changed result type: a call that
    its old declaration forbade succeeds once the new one allows it."""
    prog = ("fn stamp(){sig} {{ return \"t\" + str(clock() > 0) }}\n"
            "fn main() {{ var last = \"\"\n"
            "  while true {{ var v = \"\"\n"
            "    try {{ v = stamp() }} catch e {{ v = \"denied\" }}\n"
            "    if v != last {{ print(v) last = v }}\n"
            "    if v == \"ttrue\" {{ return }}\n"
            "    sleep(0.01) }} }}\n")
    out, err = live(exe, {"e.fut": prog.format(sig=" ! io")}, [("e.fut", prog.format(sig=" ! io, clock"))])
    ok = out.split() == ["denied", "ttrue"]
    return ok, f"stdout: {out!r}\n  stderr: {err!r}"


def hot_packages(exe):
    """Across packages: a private addition in a dependency recompiles only that package, an edit to its body
    takes effect, and (wasm) an edited JS module replaces the functions bound to it; a broken edit keeps the old."""
    wasm = exe.suffix == ".wasm"
    prog = ('import "geom"\n' + ('extern tag fn tag(str): str\n' if wasm else 'fn tag(s) { return s }\n') +
            'fn main() { var last = ""\n  for i in 0..80 { var v = tag("" + geom.area(1)) if v != last { print(v) last = v } sleep(0.03) } }\n')
    files = {"hp/app/main.fut": prog, "hp/app/fut.pkg": "name app\ndep geom ../geom\n" + ("js tag ./tag.js\n" if wasm else ""),
             "hp/app/tag.js": "export const tag = (s) => s;\n",
             "hp/geom/fut.pkg": "name geom\n", "hp/geom/geom.fut": "pub fn area(r) { return r * 3 }\n"}
    edits = [("hp/geom/geom.fut", "pub fn area(r) { return r * 3 }\nfn extra() { return 1 }\n"),
             ("hp/geom/geom.fut", "pub fn area(r) { return r * 4 }\nfn extra() { return 1 }\n")]
    if wasm:
        edits += [("hp/app/tag.js", "export const tag = (s) => {\n"), ("hp/app/tag.js", "export const tag = (s) => s + '!';\n")]
    out, err = live(exe, files, edits, settle=0.5, timeout=8)
    logs = [l for l in err.splitlines() if l.startswith("[fut] added") or l.startswith("[fut] patched")]
    want = ["3", "4"] + (["4!"] if wasm else [])
    ok = (out.splitlines() == want and logs and "added geom/geom.extra" in logs[0] and "main" not in logs[0]
          and (not wasm or "cannot load module" in err))
    return ok, f"out: {out!r}\n  err: {err!r}"


HOT_TESTS = [("methods, errors, main restart", hot_methods_and_main),
             ("packages: per-package recompiles, JS modules", hot_packages),
             ("a changed effect declaration takes effect", hot_effects),
             ("a changed result type recompiles callers", hot_result_type),
             ("a var's type, from the REPL and changed", hot_var_type),
             ("a failed reload rolls back", hot_reload_rollback),
             ("imports, no-op, hook, stale arity", hot_imports_stale_hook),
             ("live REPL, var reset", hot_repl_and_reset),
             ("code reclamation", hot_reclaim)]


def runner_json(exe):
    """`fut test --json` on tests/fixtures/runner.fut: each test's status, the failure details, captured output."""
    r = subprocess.run([*runner(exe), "test", "--json", str(ROOT / "tests" / "fixtures" / "runner.fut")], capture_output=True, text=True, timeout=60)
    recs = [json.loads(l) for l in r.stdout.splitlines() if l.startswith("{")]
    tests = {t["name"]: t for t in recs if t["type"] == "test"}
    status = {n: t["status"] for n, t in tests.items()}
    want = {"passes": "pass", "fails": "fail", "throws": "pass", "nothing thrown": "fail", "pending": "pending", "errors": "error",
            "deep": "fail", "caught failure still fails": "fail", "module": "pass"}
    f, d = tests.get("fails", {}), tests.get("deep", {})
    ok = (status == want and r.returncode == 1 and "main does not run" not in r.stdout
          and f.get("output") == "built\n" and f["expect"]["line"] == 15 and f["expect"]["left"] == {"expr": "c.total()", "value": "6"}
          and tests["nothing thrown"]["expect"]["reason"] == "nothing was thrown"
          and tests["pending"]["reason"].startswith("undefined name 'discount'")
          and tests["errors"]["message"] == "number has no field 'b'"
          and d["expect"]["diff"] == {"at": ".items[0].qty", "what": "3 vs 4"}
          and recs[-1] == {**recs[-1], "type": "summary", "passed": 3, "failed": 4, "errors": 1, "pending": 1})
    return ok, f"rc={r.returncode} statuses: {status}\n  stdout: {r.stdout[-1500:]!r}\n  stderr: {r.stderr[-500:]!r}"


def runner_text(exe):
    """`fut test` (text) with a filter; a compile error; tests first: all pending, then all passing; todo() bodies."""
    d = ROOT / "build" / "spec"
    d.mkdir(parents=True, exist_ok=True)
    spec = 'test "doubles" { expect double(2) == 4 }\ntest "halves" { expect half(4) == 2 }\n'
    (d / "s.fut").write_text(spec)
    run = lambda *a: subprocess.run([*runner(exe), "test", *a], capture_output=True, text=True, timeout=60)
    a = run(str(d / "s.fut"))
    (d / "s.fut").write_text("fn double(n) { return n * 2 }\nfn half(n) { return n / 2 }\n" + spec)
    b, c = run(str(d / "s.fut")), run("--filter", "dou", str(d / "s.fut"))
    (d / "s.fut").write_text('test "x" { expect 1 == }\n')
    e = run("--json", str(d / "s.fut"))
    err = json.loads(e.stdout.splitlines()[0]) if e.stdout.startswith("{") else {}
    ok = (a.returncode == 1 and sum(l.startswith("pending ") for l in a.stdout.splitlines()) == 2 and "2 tests: 0 passed, 2 pending" in a.stdout
          and b.returncode == 0 and b.stdout.startswith("ok      doubles\nok      halves\n2 tests: 2 passed")
          and c.stdout.startswith("ok      doubles\n1 test: 1 passed")
          and e.returncode == 2 and err.get("type") == "compile_error" and err.get("line") == 1 and err.get("message") == "unexpected '}'")
    # a function whose body is todo() is pending: its signature and contract are written, its code isn't yet
    (d / "s.fut").write_text('class Box {\n  fn open()\n    requires true\n  {\n    todo()\n  }\n}\nfn triple(n)\n  ensures result == n * 3\n{ todo() }\n'
                             'test "t" { expect throws { triple(1) } == "triple() is not implemented yet" }\n')
    p = run(str(d / "s.fut"))
    ok = ok and p.returncode == 1 and p.stdout.startswith(
        "ok      t\npending Box.open() (s.fut:2): Box.open() is not implemented yet\npending triple() (s.fut:8): triple() is not implemented yet\n")
    return ok, f"pending run: {a.stdout!r}\n  passing run: {b.stdout!r}\n  filtered: {c.stdout!r}\n  compile error: {e.stdout!r}\n  specs: {p.stdout!r}"


def runner_api(exe):
    """Interfaces: `fut test` fails until `fut api` writes the .api file, then again when a pub signature changes
    (text and JSON, with the declarations removed and added); private edits and comments elsewhere don't count."""
    d = ROOT / "build" / "api"
    shutil.rmtree(d, ignore_errors=True), d.mkdir(parents=True)
    lib = "// Squares a number.\npub fn sq(x) { return x * x }\nfn helper() { return 1 }\npub class P {\n  var x = 0\n  fn init(x) { self.x = x }\n}\n"
    (d / "lib.fut").write_text(lib)
    (d / "main.fut").write_text('import "lib.fut"\ntest "sq" { expect sq(3) == 9 }\n')
    run = lambda *a: subprocess.run([*runner(exe), *a], capture_output=True, text=True, timeout=60)
    main = str(d / "main.fut")
    a = run("test", main)
    w = run("api", main)
    api = (d / "lib.api").read_text() if (d / "lib.api").exists() else ""
    b = run("test", main)
    (d / "lib.fut").write_text(lib.replace("fn helper()", "fn helper(y)"))  # private: not part of the interface
    c = run("test", main)
    (d / "lib.fut").write_text(lib.replace("pub fn sq(x) { return x * x }", "pub fn sq(x, y) { return x * y }"))
    (d / "main.fut").write_text('import "lib.fut"\ntest "sq" { expect sq(3, 3) == 9 }\n')
    e, j, k = run("test", main), run("test", "--json", main), run("api", "--check", main)
    rec = next((json.loads(l) for l in j.stdout.splitlines() if '"type":"api"' in l), {})
    ok = (a.returncode == 1 and "API     lib.fut: has pub declarations but no lib.api yet" in a.stdout
          and w.returncode == 0 and "wrote lib.api" in w.stdout
          and "// Squares a number.\npub fn sq(x)\n" in api and "pub class P {\n  var x = 0\n  fn init(x)\n}" in api and "helper" not in api
          and b.returncode == 0 and "API" not in b.stdout and c.returncode == 0
          and e.returncode == 1 and "its public interface changed" in e.stdout and "  - fn sq(x)\n  + fn sq(x, y)\n" in e.stdout
          and rec.get("status") == "changed" and rec.get("removed") == ["fn sq(x)"] and rec.get("added") == ["fn sq(x, y)"]
          and k.returncode == 1 and "- fn sq(x)" in k.stdout)
    (d / "lib.fut").write_text(lib.replace("pub fn sq(x)", "pub fn sq(x: num): num"))  # a type is part of the interface
    (d / "main.fut").write_text('import "lib.fut"\ntest "sq" { expect sq(3) == 9 }\n')
    t = run("test", main)
    ok = ok and t.returncode == 1 and "  - fn sq(x)\n  + fn sq(x: num): num\n" in t.stdout
    run("api", main)  # so are effects: adding io to a pub function changes its interface
    (d / "lib.fut").write_text(lib.replace("pub fn sq(x)", "pub fn sq(x: num): num ! io"))
    fx = run("test", main)
    ok = ok and fx.returncode == 1 and "  - fn sq(x: num): num\n  + fn sq(x: num): num ! io\n" in fx.stdout
    t = fx
    return ok, f"missing: {a.stdout!r}\n  api: {w.stdout!r} -> {api!r}\n  after: {b.stdout!r}\n  private edit: {c.stdout!r}\n  changed: {e.stdout!r}\n  json: {j.stdout!r}\n  check: {k.stdout!r}\n  typed: {t.stdout!r}"


def tree(d, files):  # write files {relative path: text} under d, afresh
    shutil.rmtree(d, ignore_errors=True)
    for name, text in files.items():
        (d / name).parent.mkdir(parents=True, exist_ok=True)
        (d / name).write_text(text)


PKG_TREE = {
    "app/fut.pkg": "name app\ndep geom ../geom\n",
    "app/main.fut": 'import "geom"\nimport "geom" as g\nvar v = geom.Vec(3, 4)\nprint(v.len())\nprint(g.area(2))\nprint(geom.unit().x)\n',
    "geom/fut.pkg": "// shapes\nname geom\nversion 0.3.0\n",
    "geom/geom.fut": 'pub import "./vec.fut"\nimport "./util.fut"\npub fn area(r) { return sq(r) * 3 }\nfn hidden() { return 1 }\n',
    "geom/vec.fut": "pub class Vec {\n  fn init(x, y) { self.x = x self.y = y }\n  fn len() { return sqrt(self.x * self.x + self.y * self.y) }\n}\npub fn unit() { return Vec(1, 0) }\n",
    "geom/util.fut": "pub fn sq(x) { return x * x }\n",
}


def runner_packages(exe):
    """Packages: imports by name, the root file's surface (with re-exports), what a dependency can't see, the
    boundary and cycle errors, and one .api per package whose breaking changes (contracts too) need a version bump."""
    d = ROOT / "build" / "pkg"
    tree(d, PKG_TREE)
    run = lambda *a: subprocess.run([*runner(exe), *a], capture_output=True, text=True, timeout=60)
    main, e = str(d / "app" / "main.fut"), d / "app" / "e.fut"
    def err(src, *setup):
        for f, t in setup:
            (d / f).write_text(t)
        e.write_text(src)
        return run(str(e)).stderr
    r = run(main)
    errs = [err('import "geom"\nprint(geom.sq(2))\n'), err('import "geom"\nprint(geom.hidden())\n'),
            err('import "../geom/util.fut"\n'), err('import "nope"\n'),
            err('import "geom"\nvar secret = 1\nprint(geom.area(1))\n', ("geom/util.fut", PKG_TREE["geom/util.fut"] + "pub fn peek() { return secret }\n")),
            err('import "geom"\n', ("geom/util.fut", PKG_TREE["geom/util.fut"]), ("geom/fut.pkg", PKG_TREE["geom/fut.pkg"] + "dep app ../app\n"),
                ("geom/vec.fut", 'import "app"\n' + PKG_TREE["geom/vec.fut"]))]
    tree(d, PKG_TREE)
    loose = d / "loose.fut"
    loose.write_text('import "geom"\n')
    errs.append(run(str(loose)).stderr)
    want = ["is pub inside package geom but not part of its interface", "package geom has no 'hidden' in its interface",
            "is in package geom: import it by name", "package app does not list 'nope'", "not the program's",
            "packages can't depend on each other in a cycle: app -> geom -> app", "is in no package"]
    a = run("api", main)
    api = (d / "geom" / "geom.api").read_text() if (d / "geom" / "geom.api").exists() else ""
    t = run("test", main)
    (d / "geom" / "vec.fut").write_text(PKG_TREE["geom/vec.fut"].replace("pub fn unit", "fn unit"))
    (d / "app" / "main.fut").write_text(PKG_TREE["app/main.fut"].replace("print(geom.unit().x)\n", ""))
    b, held = run("test", main), run("api", main)
    (d / "geom" / "fut.pkg").write_text(PKG_TREE["geom/fut.pkg"].replace("0.3.0", "0.4.0"))
    w = run("api", main)
    # contracts are part of the interface: adding an ensures is compatible, adding a requires is breaking
    geom = (d / "geom" / "geom.fut").read_text()
    (d / "geom" / "geom.fut").write_text(geom.replace("pub fn area(r) {", "pub fn area(r)\n  ensures result >= 0\n{"))
    ens = run("api", main)
    ensApi = (d / "geom" / "geom.api").read_text()
    (d / "geom" / "geom.fut").write_text(geom.replace("pub fn area(r) {", "pub fn area(r)\n  requires r >= 0\n  ensures result >= 0\n{"))
    req = run("test", main)
    ok = (r.stdout.splitlines() == ["5", "12", "1"] and all(x in y for x, y in zip(want, errs))
          and a.returncode == 0 and "package geom 0.3.0" in api and "pub fn unit()" in api and "pub class Vec {" in api
          and "sq" not in api and "hidden" not in api and not (d / "app" / "main.api").exists() and t.returncode == 0
          and b.returncode == 1 and "set 'version 0.4.0'" in b.stdout and held.returncode == 1 and "not writing" in held.stdout
          and w.returncode == 0 and "package geom 0.4.0" in (d / "geom" / "geom.api").read_text()
          and ens.returncode == 0 and "+ area() ensures result >= 0" in ens.stdout and "pub fn area(r)\n  ensures result >= 0\n" in ensApi
          and req.returncode == 1 and "strengthened a contract" in req.stdout and "set 'version 0.5.0'" in req.stdout
          and "  + area() requires r >= 0" in req.stdout)
    return ok, (f"run: {r.stdout!r} {r.stderr!r}\n  errors: {errs!r}\n  api: {a.stdout!r} {api!r}\n  breaking: {b.stdout!r} {held.stdout!r}\n  bumped: {w.stdout!r}"
                f"\n  ensures added: {ens.stdout!r} {ensApi!r}\n  requires added: {req.stdout!r}")


def runner_map(exe):
    """fut map: packages, files and declarations with line ranges (contracts take no room); --pub (the interface), --json, a filter."""
    d = ROOT / "build" / "map"
    tree(d, PKG_TREE)
    run = lambda *a: subprocess.run([*runner(exe), "map", *a], capture_output=True, text=True, timeout=60)
    main = str(d / "app" / "main.fut")
    t, p, j, f, none = run(main), run("--pub", main), run("--json", main), run(main, "geom"), run(main, "nothing")
    recs = [json.loads(l) for l in j.stdout.splitlines() if l.startswith("{")]
    area = next((r for r in recs if r.get("name") == "area"), {})
    (d / "geom" / "geom.fut").write_text(PKG_TREE["geom/geom.fut"].replace("pub fn area(r) {", "pub fn area(r)\n  requires r >= 0\n{"))
    (d / "geom" / "vec.fut").write_text(PKG_TREE["geom/vec.fut"].replace("pub class Vec {\n", "pub class Vec {\n  invariant self.x != nil\n"))
    marked = run(main, "geom").stdout  # contracts take no room: the map shows declarations only
    tree(d, PKG_TREE)
    ln = next((r for r in recs if r.get("name") == "len"), {})
    ok = (t.returncode == 0 and "pkg app 0.0.0  // fut.pkg" in t.stdout and "pkg geom 0.3.0  // ../geom/fut.pkg" in t.stdout
          and "  geom.fut  // root" in t.stdout and "    fn hidden()" in t.stdout and "      fn len()" in t.stdout
          and "pub class Vec  // 1-4" in t.stdout.replace("   ", " ").replace("  //", "  //")
          and "hidden" not in p.stdout and "util.fut" not in p.stdout and "pub fn area(r)" in p.stdout
          and area.get("package") == "geom" and area.get("line") == 3 and area.get("pub") is True
          and ln.get("in") == "Vec" and ln.get("line") == 3 and ln.get("end") == 3
          and "pkg app" not in f.stdout and "pkg geom" in f.stdout and none.returncode == 1
          and re.search(r"pub fn area\(r\) +// 3-5\n", marked) and re.search(r"pub class Vec +// 1-5\n", marked) and "invariant" not in marked)
    return ok, f"text: {t.stdout!r}\n  pub: {p.stdout!r}\n  json: {j.stdout[:400]!r}\n  filtered: {f.stdout!r}\n  contracts marked: {marked!r}"


def runner_modules(exe):
    """Foreign modules in a package: on wasm, a JS module (data copied, functions as callbacks, JS objects as
    handles, Promises suspending the caller, exceptions as fut errors) and a wasm module; natively, a C library,
    and a clear error for a JS-only module."""
    d = ROOT / "build" / "mods"
    wasmb = bytes.fromhex("0061736d01000000011203" "60027f7f017f60027c7c017c60017e017e" "030403000102" "071303036164640000036d756c00010364626c0002"
                          "0a1903" "0700200020016a0b070020002001a20b0700200020007c0b")
    js = ("export const info = (m) => ({ keys: Object.keys(m), n: m.n * 2, list: [1, [2]], ok: true, none: null });\n"
          "export const mapWith = (xs, f) => xs.map((x) => f(x));\n"
          "export const later = (v) => new Promise((r) => setTimeout(() => r(v), 10));\n"
          "export const boom = () => { throw new Error('kaboom'); };\n"
          "class Counter { n = 0; inc() { return ++this.n; } }\n"
          "export const counter = () => new Counter();\nexport const bump = (c) => c.inc();\n")
    tree(d, {"app/fut.pkg": "name app\njs util ./util.js\nwasm m ./m.wasm\nc libm libm.dylib\n", "app/util.js": js})
    (d / "app" / "m.wasm").write_bytes(wasmb)
    run = lambda f: subprocess.run([*runner(exe), str(d / "app" / f)], capture_output=True, text=True, timeout=60)
    if exe.suffix != ".wasm":
        (d / "app" / "c.fut").write_text("extern libm fn cbrt(num): num\nprint(cbrt(27))\n")
        (d / "app" / "j.fut").write_text("extern util fn info(any): any\n")
        c, j = run("c.fut"), run("j.fut")
        ok = c.stdout == "3\n" and j.returncode != 0 and "only the wasm build loads: add a C version" in j.stderr
        return ok, f"c: {c.stdout!r} {c.stderr!r}\n  js: {j.stderr!r}"
    (d / "app" / "main.fut").write_text(
        "extern util fn info(any): any\nextern util fn mapWith(any, fn): any\nextern util fn later(any): any\n"
        "extern util fn boom(): void\nextern util fn counter(): any\nextern util fn bump(any): num\n"
        "extern m fn add(int, int): int\nextern m fn dbl(long): long\n"
        'print(info({"n": 21, "s": "x"}))\nprint(mapWith([1, 2], fn(x) { return x * 10 }))\nprint(later([true, nil]))\n'
        'try { boom() } catch e { print(e) }\nvar c = counter()\nbump(c)\nprint(bump(c))\nprint(c)\n'
        "print(add(2, 3))\nprint(dbl(4000000000))\n")
    r = run("main.fut")
    want = ['{"keys": ["n", "s"], "n": 42, "list": [1, [2]], "ok": true, "none": nil}', "[10, 20]", "[true, nil]",
            "boom(): kaboom", "2", "<js value>", "5", "8000000000"]
    return r.stdout.splitlines() == want, f"got: {r.stdout!r}\n  stderr: {r.stderr!r}"


RUNNER_TESTS = [("fut test --json: statuses, details, output", runner_json),
                ("packages: names, interfaces, boundaries, versions", runner_packages),
                ("fut map: packages, files, declarations", runner_map),
                ("foreign modules: js, wasm, c", runner_modules),
                ("fut test: tests first, filter, compile error, todo()", runner_text),
                ("fut api: interfaces written, checked by fut test", runner_api)]


def fuzz(exe, n=300, seed=None):
    """Mutate the test programs and run them under ASan/UBSan. Errors are fine; crashes and sanitizer reports are not."""
    rng = random.Random(seed if seed is not None else time.time_ns())
    corpus = [p.read_text() for p in sorted((ROOT / "tests").glob("*.fut")) + sorted((ROOT / "tests" / "specs").rglob("*.fut"))
              + sorted((ROOT / "examples").glob("*.fut"))]
    corpus = [c for c in corpus if "window(" not in c and "extern" not in c and "run(" not in c]  # no windows; C calls are unchecked; no child processes
    toks = ["(", ")", "{", "}", "[", "]", ",", ".", ":", "=", "+=", "-", "*", "/", "%", "==", "<", "&&", "!", "..", "\"",
            "\"{x}\"", "fn", "var", "class", "if", "else", "while", "for", "in", "return", "break", "self", "super", "nil",
            "1", "0", "-1", "x", "f", "fn(a) { return a }", "{a: 1}", "\n"]
    crashes = ROOT / "build" / "crashes"
    crashes.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="print_stacktrace=1")

    def mutate(src):
        for _ in range(rng.randint(1, 4)):
            i, j = sorted(rng.randrange(len(src) + 1) for _ in range(2))
            src = rng.choice([lambda: src[:i] + src[j:j + (j - i) % 40], lambda: src[:i] + rng.choice(toks) + src[i:],
                              lambda: src[:i] + src[i:j][:60] + src[i:]])()
        return src

    cases = [mutate(rng.choice(corpus)) for _ in range(n)]

    def one(k):
        f = ROOT / "build" / f"fuzz{k % 64}.fut"
        f.write_text(cases[k])
        try:
            spec = 'test "' in cases[k]  # a spec's test bodies only compile under `fut test` (each test in its own process)
            r = subprocess.run([*runner(exe), *(["test", "--timeout", "1"] if spec else []), str(f)], capture_output=True, text=True, timeout=3, env=env)
        except subprocess.TimeoutExpired:
            return None
        bad = (r.returncode < 0 or "Sanitizer" in r.stderr or re.search(r"\.(cpp|h):\d+:\d+: runtime error", r.stderr)
               or "the test process crashed" in r.stdout)
        if bad:
            (crashes / f"crash{k}.fut").write_text(cases[k])
            return f"case {k}: rc={r.returncode}\n{r.stderr[-1500:]}"
    with ThreadPoolExecutor(8) as ex:
        bad = [b for b in ex.map(one, range(n)) if b]
    for b in bad[:3]:
        print(b)
    print(f"fuzz: {n} cases, {len(bad)} crashes" + (f" (saved to {crashes})" if bad else ""))
    return len(bad)


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "build"
    if cmd == "clean":
        subprocess.run(["rm", "-rf", str(ROOT / "build")])
    elif cmd == "test":
        sys.exit(1 if test(build("--debug" in sys.argv, "--wasm" in sys.argv)) else 0)
    elif cmd == "wasm":
        build(wasm=True)
    elif cmd == "fuzz":
        sys.exit(1 if fuzz(build(True), int(sys.argv[2]) if len(sys.argv) > 2 else 300) else 0)
    else:
        build(cmd == "debug")
