#!/usr/bin/env python3
"""fut's std/ui beside React, Solid and plain DOM in Chrome: js-framework-benchmark's keyed table operations.

  cd bench/web && npm ci && python3 run.py [--rounds 10] [--apps fut,react,solid,vanilla] [--threads] [--memory]

Each operation is timed from the click until the page has painted it (its DOM shows the change, then a frame); the
median of the rounds (or --stat min), after one round of warm-up. Needs Python's Playwright, Chrome and built wasm
files. --threads includes fut-mt.wasm (the program on a worker). --memory samples Chrome RSS on separate runs."""
import argparse, asyncio, statistics, subprocess, sys, time
from contextlib import asynccontextmanager
from pathlib import Path
from playwright.async_api import async_playwright

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
PORT = 8123
APPS = ("vanilla", "solid", "react", "fut")
CHROME_ARGS = ["--disable-renderer-backgrounding"]

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
  const holds = () => {{ try {{ return cond(); }} catch {{ return false; }} }};  // (a page part-way through a change)
  while (!holds()) {{ if (performance.now() - t > limit) throw new Error('timed out: ' + cond); await tick(); }}
}};
window.painted = () => new Promise((r) => requestAnimationFrame(() => (waiting.push(r), ch.port2.postMessage(0))));  // after the frame
"""

async def measure(page, op):
    _, (ready, n), act, done = op
    await page.evaluate(f"""async () => {{  // (new rows: a list the same length as before is not yet ready)
      const was = firstId(); click('#{ready}'); await until(() => {COUNT} == {n} && ({n} == 0 || firstId() != was)); await painted(); }}""")
    await page.wait_for_timeout(50)
    return await page.evaluate(f"""async () => {{
      const rs = {ROWS}, before = {{ first: firstId(), r998: rs[998]?.firstChild.textContent }};
      const t = performance.now();
      {act};
      await until(() => {done});
      await painted();
      return performance.now() - t;
    }}""")

@asynccontextmanager
async def open_app(browser, app):
    page = await browser.new_page(viewport={"width": 1200, "height": 900})
    errors = []
    page.on("pageerror", lambda e: errors.append(str(e)))
    page.on("console", lambda m: m.type == "error" and "Failed to load resource" not in m.text and errors.append(m.text))  # fut probes for files
    query = "fut&threads=1" if app == "fut-mt" else app
    url = f"http://127.0.0.1:{PORT}/bench/web/page.html?app={query}"
    try:
        t = time.perf_counter()
        await page.goto(url, timeout=60000)
        await page.wait_for_selector("#run", timeout=60000)
        start = (time.perf_counter() - t) * 1000
        await page.evaluate(HELPERS)
        yield page, start
    except Exception as e:
        raise RuntimeError(f"{app}: {e}\nBrowser errors: {errors[:3]}") from e
    finally:
        await page.close()
    if errors:
        raise RuntimeError(f"{app}: {errors[:3]}")


async def run_app(browser, app, a):
    async with open_app(browser, app) as (page, start):
        times = {op[0]: [] for op in OPS}
        for r in range(a.rounds + 1):
            for op in OPS:
                ms = await measure(page, op)
                if r: times[op[0]].append(ms)
                if a.v: print(f"    {app} {op[0]}: {ms:.1f} ms", file=sys.stderr, flush=True)
    pick = min if a.stat == "min" else statistics.median
    return start, {k: pick(v) for k, v in times.items()}


async def chrome_rss(cdp):
    procs = (await cdp.send("SystemInfo.getProcessInfo"))["processInfo"]
    pids = ",".join(str(p["id"]) for p in procs)
    rss = subprocess.check_output(["ps", "-o", "rss=", "-p", pids], text=True)
    return sum(int(kib) for kib in rss.split()) / 1024


async def memory_app(chromium, app):
    browser = await chromium.launch(channel="chrome", args=CHROME_ARGS)
    try:
        cdp = await browser.new_browser_cdp_session()
        async with open_app(browser, app) as (page, _):
            await page.evaluate("() => painted()")
            await page.wait_for_timeout(250)
            empty = await chrome_rss(cdp)
            await page.evaluate(f"async () => {{ click('#runlots'); await until(() => {COUNT} == 10000); await painted(); }}")
            await page.wait_for_timeout(250)
            full = await chrome_rss(cdp)
        return empty, full
    finally:
        await browser.close()


async def run_repo(chromium, root, apps, a):
    subprocess.run(["node", "build.mjs"], cwd=root / "bench" / "web", check=True)
    server = await asyncio.create_subprocess_exec("node", "web/serve.mjs", str(PORT), cwd=root, stdout=asyncio.subprocess.PIPE)
    try:
        ready = await asyncio.wait_for(server.stdout.readline(), timeout=10)
        if not ready.startswith(b"fut: http://"):
            raise RuntimeError(f"the benchmark server did not start on port {PORT}")
        browser = await chromium.launch(channel="chrome", args=CHROME_ARGS)
        version = browser.version
        try:
            timings = {}
            for app in apps:
                timings[app] = await run_app(browser, app, a)
                print(f"  {app} timings done", file=sys.stderr, flush=True)
        finally:
            await browser.close()
        memory = {}
        if a.memory:
            for app in apps:
                memory[app] = await memory_app(chromium, app)
                print(f"  {app} memory done", file=sys.stderr, flush=True)
        return version, timings, memory
    finally:
        if server.returncode is None:
            server.terminate()
            await server.wait()

async def main(a):
    async with async_playwright() as p:
        baseline = None
        if a.baseline:
            print(f"Baseline in {a.baseline}", file=sys.stderr, flush=True)
            baseline = await run_repo(p.chromium, a.baseline.resolve(), a.apps, a)
        print(f"Candidate in {ROOT}", file=sys.stderr, flush=True)
        candidate = await run_repo(p.chromium, ROOT, a.apps, a)
    report = markdown(a, candidate, baseline)
    print(report, end="")
    output = a.output
    if output is None and set(APPS) <= set(a.apps) and not baseline:
        output = HERE / "results.md"  # a partial run or comparison doesn't replace the checked-in table
    if output:
        output.write_text(report)


def table(title, apps, rows):
    lines = [f"| {title} | " + " | ".join(apps) + " |", "|---|" + "---:|" * len(apps)]
    for name, values in rows:
        lines.append(f"| {name} | " + " | ".join(values) + " |")
    return "\n".join(lines)


def markdown(a, candidate, baseline):
    version, timings, memory = candidate
    ops = [op[0] for op in OPS]
    apps = a.apps
    rows = []
    for op in ops:
        values = [f"{timings[app][1][op]:.1f}" for app in apps]
        if "vanilla" in timings:
            c = timings["vanilla"][1][op]
            values = [f"{v} ({timings[app][1][op] / c:.2f}×)" for app, v in zip(apps, values)]
        rows.append((op, values))
    if "vanilla" in timings:
        ratios = [statistics.geometric_mean(timings[app][1][op] / timings["vanilla"][1][op] for op in ops) for app in apps]
        rows.append(("× plain DOM (geometric mean)", [f"{ratio:.2f}" for ratio in ratios]))
    rows.append(("page ready", [f"{timings[app][0]:.0f}" for app in apps]))
    sections = [f"Headless Chrome {version}, {a.stat} of {a.rounds} rounds after one warm-up. "
                "Operations run from click to the frame that shows the change. Ratios are relative to plain DOM.",
                table("Operation (ms)", apps, rows)]
    if memory:
        rows = [("empty table", [f"{memory[app][0]:.1f}" for app in apps]),
                ("10,000 rows", [f"{memory[app][1]:.1f}" for app in apps]),
                ("growth", [f"{memory[app][1] - memory[app][0]:+.1f}" for app in apps])]
        sections += ["Memory uses a fresh browser per app and sums Chrome process RSS 250 ms after each painted state. "
                     "It includes the renderer and worker, and shared pages can count in more than one process.",
                     table("Memory (MiB)", apps, rows)]
    if baseline:
        base_version, base_times, base_memory = baseline
        if base_version != version:
            raise ValueError(f"Chrome changed between runs: {base_version} to {version}")
        rows = [(op, [f"{(timings[app][1][op] / base_times[app][1][op] - 1):+.1%}" for app in apps]) for op in ops]
        rows.append(("page ready", [f"{(timings[app][0] / base_times[app][0] - 1):+.1%}" for app in apps]))
        sections += ["Changes from the base checkout measured on the same runner. Positive numbers are increases.",
                     table("Timing change", apps, rows)]
        if memory:
            rows = [(label, [f"{memory[app][i] - base_memory[app][i]:+.1f}" for app in apps])
                    for i, label in enumerate(("empty table", "10,000 rows"))]
            sections.append(table("Memory change (MiB)", apps, rows))
    return "\n\n".join(sections) + "\n"

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=10)
    ap.add_argument("--apps", default=",".join(APPS))
    ap.add_argument("--threads", action="store_true")
    ap.add_argument("--memory", action="store_true", help="measure Chrome's resident memory in separate runs")
    ap.add_argument("--baseline", type=Path, help="compare with another checkout with its own built wasm and npm dependencies")
    ap.add_argument("--output", type=Path, help="write the Markdown report here")
    ap.add_argument("-v", action="store_true", help="each measurement as it's taken")
    ap.add_argument("--stat", choices=["median", "min"], default="median", help="min: the fairer one on a busy machine")
    a = ap.parse_args()
    a.apps = a.apps.split(",")
    if a.rounds < 1:
        ap.error("--rounds must be at least 1")
    if not set(a.apps) <= set(APPS) or len(a.apps) != len(set(a.apps)):
        ap.error("--apps must list each selected app once (vanilla,solid,react,fut)")
    if a.threads:
        a.apps.append("fut-mt")
    asyncio.run(main(a))
