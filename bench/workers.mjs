#!/usr/bin/env node
// fut tests on Cloudflare Workers, two ways, in workerd (wrangler dev, local):
//   AOT     test files compiled ahead of time into a bundle (web/worker.mjs build --tests): one deploy per code version
//   interp  one static Worker (web/worker.mjs build --interp) that compiles what it is sent and interprets the code
// Measures per-file latency, a whole suite per request, a mid-size project (examples/store), CPU-bound kernels
// (bench/*.fut) and concurrent throughput; writes bench/workers.md.
//   WRANGLER=path/to/wrangler node bench/workers.mjs        (default: wrangler on PATH)
import { spawn, spawnSync } from "node:child_process";
import { mkdirSync, readFileSync, readdirSync, writeFileSync } from "node:fs";
import { basename, join, relative, resolve, sep } from "node:path";

const repo = resolve(new URL("..", import.meta.url).pathname), out = join(repo, "build", "wbench");
const wrangler = process.env.WRANGLER ?? "wrangler", W = join(repo, "web", "worker.mjs");
const median = (xs) => [...xs].sort((a, b) => a - b)[xs.length >> 1];
const node = (...a) => {
  const t0 = performance.now(), r = spawnSync("node", [W, ...a], { encoding: "utf8" });
  if (r.status) throw new Error(`worker.mjs ${a.join(" ")}: ${r.stderr}`);
  return performance.now() - t0;
};
function collect(root) {  // as worker.mjs send: what a program may read below root
  const files = {};
  const walk = (d) => {
    for (const e of readdirSync(d, { withFileTypes: true })) {
      const p = join(d, e.name);
      if (e.isDirectory()) walk(p);
      else if (/\.(fut|api)$|^fut\.pkg$/.test(e.name)) files[relative(root, p).split(sep).join("/")] = readFileSync(p, "utf8");
    }
  };
  return walk(root), files;
}

const loadStart = (await import("node:os")).loadavg().map((x) => x.toFixed(1)).join(" ");
// ---- inputs ----
mkdirSync(join(out, "cpu"), { recursive: true });
const KERNELS = ["fib", "sieve", "nbody", "binarytrees", "maps", "mandelbrot", "leibniz"];
for (const k of KERNELS) writeFileSync(join(out, "cpu", k + ".fut"), readFileSync(join(repo, "bench", k + ".fut"), "utf8") + `\ntest "${k}" { kernel() }\n`);
const specs = readdirSync(join(repo, "tests", "specs")).filter((f) => f.endsWith(".fut")).sort().map((f) => join(repo, "tests", "specs", f));
const storeTests = [join(repo, "examples", "store", "store_test.fut")];
const build = {
  specs: node("build", "--tests", "--root", join(repo, "tests"), join(out, "specs"), ...specs),
  store: node("build", "--tests", join(out, "store"), ...storeTests),
  cpu: node("build", "--tests", join(out, "cpuaot"), ...KERNELS.map((k) => join(out, "cpu", k + ".fut"))),
  interp: node("build", "--interp", join(out, "interp")),
};
const size = (d) => JSON.parse(readFileSync(join(d, "manifest.json"), "utf8")).shards[0];
const interpKb = ["fut.wasm", "fut.js", "runtime.js", "try.wasm", "worker.js"].reduce((n, f) => n + readFileSync(join(out, "interp", f)).length, 0) / 1024;

// ---- servers ----
const servers = [];
async function serve(dir, port) {
  const p = spawn(wrangler, ["dev", "--port", String(port), "--inspector-port", String(port + 1000)], { cwd: dir, stdio: ["ignore", "pipe", "pipe"] });
  servers.push(p);
  let log = "";
  p.stdout.on("data", (d) => (log += d)), p.stderr.on("data", (d) => (log += d));
  const t0 = performance.now();
  while (!log.includes("Ready on")) {
    if (p.exitCode != null || performance.now() - t0 > 60000) throw new Error(`wrangler dev in ${dir}: ${log}`);
    await new Promise((r) => setTimeout(r, 100));
  }
  return `http://127.0.0.1:${port}`;
}
process.on("exit", () => servers.forEach((p) => p.kill()));
const aotSpecs = await serve(join(out, "specs", "shard-0"), 8771), aotStore = await serve(join(out, "store", "shard-0"), 8772);
const aotCpu = await serve(join(out, "cpuaot", "shard-0"), 8773), interp = await serve(join(out, "interp"), 8774);

const failures = new Set();  // tests that failed in some run (timing tests can, under load)
async function timed(f) {
  const t0 = performance.now(), r = await f(), text = await r.text(), ms = performance.now() - t0;
  const sum = JSON.parse(text.trim().split("\n").at(-1));
  if (r.status != 200 || !sum.files) throw new Error(`failed: ${text.slice(-600)}`);
  if (sum.failed || sum.errors) for (const l of text.split("\n")) if (/"status":"(fail|error)"/.test(l)) failures.add(JSON.parse(l).name);
  return { ms, sum };
}
const aot = (base, file) => () => fetch(`${base}/__fut/test${file ? "?file=" + encodeURIComponent(file) : ""}`);
const filesOf = { specs: collect(join(repo, "tests")), store: collect(join(repo, "examples", "store")), cpu: collect(join(out, "cpu")) };
const sent = (kind, tests) => () => fetch(`${interp}/run`, { method: "POST", body: JSON.stringify({ files: filesOf[kind], tests }) });

// cold: the first request each Worker serves (its isolate starts, fut.wasm instantiates)
const cold = {
  aot: (await timed(aot(aotSpecs, "specs/bits.fut"))).ms,
  interp: (await timed(sent("specs", ["specs/bits.fut"]))).ms,
};
// per file, warm: median of 3
const perFile = [];
for (const f of specs) {
  const rel = "specs/" + basename(f), a = [], i = [];
  for (let k = 0; k < 3; k++) a.push((await timed(aot(aotSpecs, rel))).ms), i.push((await timed(sent("specs", [rel]))).ms);
  perFile.push({ file: rel, aot: median(a), interp: median(i) });
}
// a whole suite in one request, and the store project: median of 3
const suite = { aot: [], interp: [] }, store = { aot: [], interp: [] };
for (let k = 0; k < 3; k++) {
  suite.aot.push((await timed(aot(aotSpecs))).ms), suite.interp.push((await timed(sent("specs", specs.map((f) => "specs/" + basename(f))))).ms);
  store.aot.push((await timed(aot(aotStore))).ms), store.interp.push((await timed(sent("store", ["store_test.fut"]))).ms);
}
// CPU-bound kernels: one run each (the test's own ms)
const cpu = [];
for (const k of KERNELS) {
  const a = await timed(aot(aotCpu, k + ".fut")), i = await timed(sent("cpu", [k + ".fut"]));
  cpu.push({ k, aot: a.ms, interp: i.ms });
}
// throughput: 24 requests of a mid-size file, 4 at a time (one local isolate: concurrent requests share it)
async function throughput(mk) {
  const t0 = performance.now(), q = Array.from({ length: 24 }, () => mk);
  await Promise.all(Array.from({ length: 4 }, async () => { while (q.length) await timed(q.pop()); }));
  return 24 / ((performance.now() - t0) / 1000);
}
const tput = { aot: await throughput(aot(aotSpecs, "specs/maps.fut")), interp: await throughput(sent("specs", ["specs/maps.fut"])) };
// the AOT path's deploy step, short of uploading: wrangler bundling the shard
const t0 = performance.now();
spawnSync(wrangler, ["deploy", "--dry-run", "--outdir", join(out, "dry")], { cwd: join(out, "specs", "shard-0"), encoding: "utf8" });
const dry = performance.now() - t0;

// ---- report ----
const f1 = (x) => x.toFixed(1), f0 = (x) => x.toFixed(0), ratio = (i, a) => (i / a).toFixed(1) + "x";
const sumA = perFile.reduce((n, r) => n + r.aot, 0), sumI = perFile.reduce((n, r) => n + r.interp, 0);
const slow = [...perFile].sort((x, y) => y.interp / y.aot - x.interp / x.aot).slice(0, 5);
const md = `# fut tests on Cloudflare Workers: compiled ahead of time vs interpreted

Measured with \`node bench/workers.mjs\` in workerd (\`wrangler dev\`, local) on ${process.platform}/${process.arch}, ${new Date().toISOString().slice(0, 10)}.
Latencies are end to end (HTTP request to last byte) on a warm isolate unless marked cold; medians of 3. Load average
at the start: ${loadStart}.

- **AOT**: \`web/worker.mjs build --tests\` compiles each test file ahead of time (under Node) into a bundle of
  precompiled wasm modules. A new code version needs a build and a deploy.
- **interp**: one static Worker (\`web/worker.mjs build --interp\`): each request sends the sources, fut.wasm (which
  V8 compiles once, as it is uploaded) compiles them, and src/interp.cpp interprets the code. Nothing is deployed per
  version.

## Pipeline per code version

| Step | AOT | interp |
|---|---:|---:|
| build bundle: tests/specs (30 files, 161 tests) | ${f0(build.specs)} ms | none (static bundle, built once: ${f0(build.interp)} ms) |
| bundle size | ${f0(size(join(out, "specs")).bytes / 1024)} KB (${f0(size(join(out, "specs")).gzip / 1024)} KB gzipped) | ${f0(interpKb)} KB, fixed |
| wrangler deploy --dry-run (bundling only) | ${f0(dry)} ms | none |
| upload + activate | not measured (needs an account); typically seconds | none |
| request payload | a file list | the sources (tests/: ${f0(JSON.stringify(filesOf.specs).length / 1024)} KB) |

## Latency

| Run | AOT (ms) | interp (ms) | interp / AOT |
|---|---:|---:|---:|
| first request (cold isolate), specs/bits.fut | ${f1(cold.aot)} | ${f1(cold.interp)} | ${ratio(cold.interp, cold.aot)} |
| one spec file, median over the 30 files | ${f1(median(perFile.map((r) => r.aot)))} | ${f1(median(perFile.map((r) => r.interp)))} | ${ratio(median(perFile.map((r) => r.interp)), median(perFile.map((r) => r.aot)))} |
| all 30 spec files, one request each, summed | ${f0(sumA)} | ${f0(sumI)} | ${ratio(sumI, sumA)} |
| all 30 spec files in one request | ${f0(median(suite.aot))} | ${f0(median(suite.interp))} | ${ratio(median(suite.interp), median(suite.aot))} |
| examples/store (14 tests, 4 packages) | ${f0(median(store.aot))} | ${f0(median(store.interp))} | ${ratio(median(store.interp), median(store.aot))} |

Largest interp/AOT ratios per file: ${slow.map((r) => `${r.file} ${f0(r.aot)} vs ${f0(r.interp)} ms`).join("; ")}.

## CPU-bound kernels (bench/*.fut, as one test each)

| Kernel | AOT (ms) | interp (ms) | interp / AOT |
|---|---:|---:|---:|
${cpu.map((r) => `| ${r.k} | ${f0(r.aot)} | ${f0(r.interp)} | ${ratio(r.interp, r.aot)} |`).join("\n")}

## Throughput

24 requests of specs/maps.fut, 4 at a time, one local isolate: AOT ${f1(tput.aot)} req/s, interp ${f1(tput.interp)} req/s.

Tests that failed in some run: ${failures.size ? [...failures].join("; ") : "none"}. Load average at the end: ${(await import("node:os")).loadavg().map((x) => x.toFixed(1)).join(" ")}.

## Per file

| File | AOT (ms) | interp (ms) |
|---|---:|---:|
${perFile.map((r) => `| ${r.file} | ${f1(r.aot)} | ${f1(r.interp)} |`).join("\n")}
`;
writeFileSync(join(repo, "bench", "workers.md"), md);
console.log(md);
process.exit(0);
