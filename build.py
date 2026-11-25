#!/usr/bin/env python3
"""Build and test fut.   python3 build.py [build | debug | test [--debug] | fuzz [N] | clean]"""
import os, random, re, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent
OUT = ROOT / "build" / "fut"
FLAGS = ["-std=c++23", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-missing-field-initializers", "-Wno-invalid-offsetof"]


def build(debug=False):
    srcs = sorted((ROOT / "src").glob("*.cpp"))
    deps = srcs + sorted((ROOT / "src").glob("*.h")) + [Path(__file__)]
    out = OUT.with_name("fut-debug") if debug else OUT
    if out.exists() and out.stat().st_mtime > max(p.stat().st_mtime for p in deps):
        return out
    out.parent.mkdir(exist_ok=True)
    opt = ["-O1", "-g", "-fsanitize=address,undefined"] if debug else ["-O2"]
    cmd = ["clang++", *FLAGS, *opt, *map(str, srcs), "-o", str(out)]
    print(" ".join(cmd))
    subprocess.run(cmd, check=True)
    return out


def test(exe):
    """Each tests/*.fut lists `// expect: <line>` for stdout and optionally `// error: <text>` lines for stderr."""
    failed = 0
    for t in sorted((ROOT / "tests").glob("*.fut")):
        src = t.read_text()
        want = re.findall(r"// expect: ?(.*)", src)
        err = re.findall(r"// error: ?(.*)", src)
        r = subprocess.run([str(exe), str(t)], capture_output=True, text=True, timeout=60)
        ok = r.stdout.splitlines() == want and (all(e in r.stderr for e in err) and r.returncode != 0 if err else r.returncode == 0)
        print(f"{'ok  ' if ok else 'FAIL'} {t.name}")
        if not ok:
            failed += 1
            print(f"  want: {want}\n  got:  {r.stdout.splitlines()}\n  stderr: {r.stderr.strip()}")
    for name, fn in HOT_TESTS:
        ok, detail = fn(exe)
        print(f"{'ok  ' if ok else 'FAIL'} hot reload: {name}")
        if not ok:
            failed += 1
            print("  " + detail)
    print("all tests passed" if not failed else f"{failed} failed")
    return failed


def live(exe, files, edits, settle=0.4, timeout=6):
    """Run files[0] with -w, apply edits [(file, text)] one at a time while it runs; return (stdout, stderr)."""
    d = ROOT / "build" / "hot"
    d.mkdir(parents=True, exist_ok=True)
    for name, text in files.items():
        (d / name).write_text(text)
    p = subprocess.Popen([str(exe), "-w", str(d / next(iter(files)))], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    for name, text in edits:
        time.sleep(settle)
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
    out, err = live(exe, {"m.fut": main, "lib.fut": "fn f(a) { return a }\n"},
                    [("lib.fut", "fn f(a) { return a + 1 }\n"), ("m.fut", main),
                     ("lib.fut", "fn f(a, b) { return b }\n"), ("m.fut", main.replace("f(1)", "f(1, 7)")),
                     ("m.fut", main.replace("f(1)", "f(1, 7)") + "\n")], timeout=1)
    # lib change -> patch + hook; no-op save; arity change alone fails to compile (m.fut still calls f(1));
    # fixing the caller patches it, but spin()'s running frame is old code -> stale-call error; the next
    # save reruns main() and the new spin() prints 7.
    ok = (out.split()[:3] == ["1", "hook", "2"] and out.split()[-1] == "7" and "no code changes" in err
          and "expects 2 arguments" in err and "stale call to 'f'" in err)
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


HOT_TESTS = [("methods, errors, main restart", hot_methods_and_main),
             ("imports, no-op, hook, stale arity", hot_imports_stale_hook),
             ("code reclamation", hot_reclaim)]


def fuzz(exe, n=300, seed=None):
    """Mutate the test programs and run them under ASan/UBSan. Errors are fine; crashes and sanitizer reports are not."""
    rng = random.Random(seed if seed is not None else time.time_ns())
    corpus = [p.read_text() for p in sorted((ROOT / "tests").glob("*.fut")) + sorted((ROOT / "examples").glob("*.fut"))]
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
            r = subprocess.run([str(exe), str(f)], capture_output=True, text=True, timeout=3, env=env)
        except subprocess.TimeoutExpired:
            return None
        bad = r.returncode < 0 or "Sanitizer" in r.stderr or re.search(r"\.(cpp|h):\d+:\d+: runtime error", r.stderr)
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
        sys.exit(1 if test(build("--debug" in sys.argv)) else 0)
    elif cmd == "fuzz":
        sys.exit(1 if fuzz(build(True), int(sys.argv[2]) if len(sys.argv) > 2 else 300) else 0)
    else:
        build(cmd == "debug")
