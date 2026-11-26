#!/usr/bin/env python3
"""fut against C (clang -O2): each benchmark is the same kernel in bench/<name>.fut and bench/<name>.c, run 5 times
in one process. Reports the best run (warmed up, tiered) and the first (cold), checks that every implementation
computed the same result, and writes bench/results.md.

    python3 bench/run.py [names...]      # all benchmarks by default
"""
import re, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BENCH, OUT = ROOT / "bench", ROOT / "build" / "bench"
NAMES = ["fib", "leibniz", "nbody", "binarytrees", "sieve", "maps", "mandelbrot"]
THREADS = int(subprocess.run(["sysctl", "-n", "hw.perflevel0.physicalcpu"], capture_output=True, text=True).stdout or 8)
HARNESS = """
fn main() {
  var best = 1000000000
  var first = 0
  for r in 0..5 {
    var t = clock()
    var c = kernel()
    var ms = (clock() - t) * 1000
    if r == 0 { first = ms print("check {c}") }
    if ms < best { best = ms }
  }
  print("first {first}")
  print("best {best}")
}
"""


def sh(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=600, **kw)
    if r.returncode:
        sys.exit(f"failed: {' '.join(map(str, cmd))}\n{r.stdout}{r.stderr}")
    return r.stdout


def parse(out):
    v = dict(re.findall(r"^(check|first|best) (\S+)$", out, re.M))
    return v["check"], float(v["first"]), float(v["best"])


def same(a, b):
    try:
        x, y = float(a), float(b)
        return x == y or abs(x - y) <= 1e-9 * max(abs(x), abs(y))
    except ValueError:
        return a == b


def main():
    names = sys.argv[1:] or NAMES
    OUT.mkdir(parents=True, exist_ok=True)
    sh([sys.executable, str(ROOT / "build.py")])
    sh([sys.executable, str(ROOT / "build.py"), "wasm"])
    rows = []
    for n in names:
        impls = {}
        sh(["clang", "-O2", "-o", OUT / n, BENCH / f"{n}.c", "-lm"])
        impls["C"] = [OUT / n]
        if n == "mandelbrot":
            sh(["clang", "-O2", f"-DTHREADS={THREADS}", "-o", OUT / f"{n}_mt", BENCH / f"{n}.c", "-lm"])
            impls[f"C x{THREADS} threads"] = [OUT / f"{n}_mt"]
        prog = OUT / f"{n}.fut"
        prog.write_text((BENCH / f"{n}.fut").read_text() + HARNESS)
        impls["fut"] = [ROOT / "build" / "fut", prog]
        impls["fut wasm"] = ["node", ROOT / "web" / "run.mjs", prog]
        res = {k: parse(sh(cmd)) for k, cmd in impls.items()}
        want = res["C"][0]
        for k, (check, first, best) in res.items():
            ok = same(check, want)
            rows.append((n, k, best, first, best / res["C"][2], ok, check))
            print(f"{n:12} {k:16} best {best:9.1f} ms  first {first:9.1f} ms  {best / res['C'][2]:6.2f}x C  {'ok' if ok else 'WRONG ' + check}")
    lines = ["| Benchmark | Implementation | Best (ms) | First run (ms) | vs C |", "|---|---|---:|---:|---:|"]
    for n, k, best, first, ratio, ok, check in rows:
        lines.append(f"| {n} | {k} | {best:.1f} | {first:.1f} | {ratio:.2f}x{'' if ok else ' (WRONG: ' + check + ')'} |")
    cpu = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True, text=True).stdout.strip()
    (BENCH / "results.md").write_text(f"Measured on {cpu}; C is clang -O2. Best of 5 runs in one process.\n\n" + "\n".join(lines) + "\n")
    if not all(r[5] for r in rows):
        sys.exit("some implementation computed a different result")


if __name__ == "__main__":
    main()
