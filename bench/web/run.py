#!/usr/bin/env python3
"""fut's std/ui beside React, Solid and plain DOM in Chrome: js-framework-benchmark's keyed table operations.

  cd bench/web && npm install && python3 run.py [--rounds 10] [--apps fut,react,solid,vanilla] [--threads]

Each operation is timed from the click until the page has painted it (its DOM shows the change, then a frame); the
median of the rounds (or --stat min), after one round of warm-up. Needs Python's playwright and Chrome. --threads: fut-mt.wasm too
(the program on a worker, each DOM call sent to the page's thread)."""
import argparse, asyncio, statistics, subprocess, sys, time, urllib.request
from pathlib import Path
from playwright.async_api import async_playwright

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
PORT = 8123

ROWS = "document.querySelectorAll('#tbody > tr')"
COUNT = f"{ROWS}.length"
OPS = [  # name, how to get ready (a button and the row count it gives), what to do, when it's done
    ("create 1,000 rows", ("clear", 0), "click('#run')", f"{COUNT} == 1000"),
    ("replace 1,000 rows", ("run", 1000), "click('#run')", f"{COUNT} == 1000 && firstId() != before.first"),
    ("update every 10th row", ("run", 1000), "click('#update')", f"{ROWS}[0].textContent.endsWith('!!!x')"),
    ("select a row", ("run", 1000), f"{ROWS}[1].querySelector('.lbl').click()", f"/danger/.test({ROWS}[1].className)"),
    ("swap two rows", ("run", 1000), "click('#swaprows')", f"{ROWS}[1].firstChild.textContent == before.r998"),
    ("remove a row", ("run", 1000), f"{ROWS}[3].querySelector('.remove').click()", f"{COUNT} == 999"),
    ("create 10,000 rows", ("clear", 0), "click('#runlots')", f"{COUNT} == 10000"),
    ("append 1,000 rows", ("run", 1000), "click('#add')", f"{COUNT} == 2000"),
    ("clear 10,000 rows", ("runlots", 10000), "click('#clear')", f"{COUNT} == 0"),
]
HELPERS = f"""
window.click = (s) => document.querySelector(s).click();
window.firstId = () => {ROWS}[0]?.firstChild.textContent;
const ch = new MessageChannel(), waiting = [];
ch.port1.onmessage = () => waiting.shift()();
window.tick = () => new Promise((r) => (waiting.push(r), ch.port2.postMessage(0)));  // a task, not clamped as setTimeout
window.until = async (cond, limit = 60000) => {{
  const t = performance.now();
  while (!cond()) {{ if (performance.now() - t > limit) throw new Error('timed out: ' + cond); await tick(); }}
}};
window.painted = () => new Promise((r) => requestAnimationFrame(() => (waiting.push(r), ch.port2.postMessage(0))));  // after the frame
"""

async def measure(page, op):
    _, (ready, n), act, done = op
    await page.evaluate(f"""async () => {{ click('#{ready}'); await until(() => {COUNT} == {n}); await painted(); }}""")
    await page.wait_for_timeout(50)
    return await page.evaluate(f"""async () => {{
      const rs = {ROWS}, before = {{ first: firstId(), r998: rs[998]?.firstChild.textContent }};
      const t = performance.now();
      {act};
      await until(() => {done});
      await painted();
      return performance.now() - t;
    }}""")

verbose = False
stat = "median"

async def run_app(browser, app, rounds):
    page = await browser.new_page(viewport={"width": 1200, "height": 900})
    errors = []
    page.on("pageerror", lambda e: errors.append(str(e)))
    page.on("console", lambda m: m.type == "error" and "Failed to load resource" not in m.text and errors.append(m.text))  # (fut probes for files)
    url, t = f"http://127.0.0.1:{PORT}/bench/web/page.html?app={app.replace('-mt', '')}" + ("&threads=1" if app.endswith("-mt") else ""), time.time()
    await page.goto(url)
    try:
        await page.wait_for_selector("#run", timeout=60000)
    except Exception:
        raise SystemExit(f"{app}: the page didn't start: {errors}")
    start = (time.time() - t) * 1000
    await page.evaluate(HELPERS)
    times = {op[0]: [] for op in OPS}
    for r in range(rounds + 1):
        for op in OPS:
            ms = await measure(page, op)
            if r: times[op[0]].append(ms)
            if verbose: print(f"    {app} {op[0]}: {ms:.1f} ms", file=sys.stderr, flush=True)
    await page.close()
    if errors: print(f"{app}: {errors[:3]}", file=sys.stderr)
    pick = min if stat == "min" else statistics.median
    return start, {k: pick(v) for k, v in times.items()}

async def main(a):
    subprocess.run(["node", "build.mjs"], cwd=HERE, check=True)
    server = subprocess.Popen(["node", "web/serve.mjs", str(PORT)], cwd=ROOT, stdout=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try: urllib.request.urlopen(f"http://127.0.0.1:{PORT}/bench/web/page.html"); break
            except OSError: time.sleep(0.1)
        apps = a.apps.split(",") + (["fut-mt"] if a.threads else [])
        res = {}
        async with async_playwright() as p:
            browser = await p.chromium.launch(channel="chrome", args=["--disable-renderer-backgrounding"])
            for app in apps:
                res[app] = await run_app(browser, app, a.rounds)
                print(f"  {app} done", file=sys.stderr)
            await browser.close()
    finally:
        server.terminate()
    write_md(apps, res, a)
    w = max(len(o[0]) for o in OPS)
    print(f"{'ms (' + stat + ' of ' + str(a.rounds) + ')':<{w}}  " + "  ".join(f"{x:>9}" for x in apps))
    for op in OPS:
        print(f"{op[0]:<{w}}  " + "  ".join(f"{res[x][1][op[0]]:>9.1f}" for x in apps))
    if "vanilla" in res:
        g = lambda x: statistics.geometric_mean([res[x][1][o[0]] / res["vanilla"][1][o[0]] for o in OPS])
        print(f"{'× plain DOM (geometric mean)':<{w}}  " + "  ".join(f"{g(x):>9.2f}" for x in apps))
    print(f"{'page ready (ms)':<{w}}  " + "  ".join(f"{res[x][0]:>9.0f}" for x in apps))

def write_md(apps, res, a):  # -> results.md
    rel = lambda x, o: res[x][1][o] / res["vanilla"][1][o] if "vanilla" in res else None
    lines = [f"Chrome, headless, {a.rounds} rounds after one of warm-up; the {a.stat} per operation, from the click to the frame",
             "that shows it (ms). × plain DOM: how many times as long as apps/vanilla.js. `python3 run.py` writes this.", "",
             "| Operation | " + " | ".join(apps) + " |", "|---|" + "---:|" * len(apps)]
    for o, *_ in OPS:
        lines.append(f"| {o} | " + " | ".join(f"{res[x][1][o]:.1f}" + (f" ({rel(x, o):.2f}×)" if rel(x, o) else "") for x in apps) + " |")
    if "vanilla" in res:
        lines.append("| × plain DOM (geometric mean) | " + " | ".join(f"{statistics.geometric_mean([rel(x, o[0]) for o in OPS]):.2f}" for x in apps) + " |")
    lines.append("| page ready | " + " | ".join(f"{res[x][0]:.0f}" for x in apps) + " |")
    (HERE / "results.md").write_text("\n".join(lines) + "\n")

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=10)
    ap.add_argument("--apps", default="vanilla,solid,react,fut")
    ap.add_argument("--threads", action="store_true")
    ap.add_argument("-v", action="store_true", help="each measurement as it's taken")
    ap.add_argument("--stat", choices=["median", "min"], default="median", help="min: the fairer one on a busy machine")
    a = ap.parse_args()
    verbose, stat = a.v, a.stat
    asyncio.run(main(a))
