#!/usr/bin/env node
// Run fut on WebAssembly in Node: node web/run.mjs [-w | -v] [file.fut]   (no file: REPL). Same flags as build/fut,
// including `node web/run.mjs test [--json] [--filter text] file.fut`.
import { readFileSync, statSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { Worker } from "node:worker_threads";
import { createFut } from "./fut.js";
import { nodeOptions } from "./node.mjs";

let file = "", watch = false, verbose = false, json = false, filter = "", check = false, pub = false, only = "";
const testFiles = [];
const argv = process.argv.slice(2), testing = argv[0] == "test", api = argv[0] == "api", map = argv[0] == "map", explain = argv[0] == "explain";
const usage = "usage: node web/run.mjs [-w | -v] [--seed n] [file.fut]   (no file: REPL)\n       node web/run.mjs test [--json] [--filter text] file.fut\n       node web/run.mjs api [--check] file.fut\n       node web/run.mjs map [--pub] [--json] file.fut [package | file]\n       node web/run.mjs explain [code]\n";
let seed = 0, threads = 0;  // --seed n: which interleaving of a program's isolates; --threads n: the pool's size (fut-mt.wasm)
for (let i = testing || api || map ? 1 : 0; i < argv.length && !explain; i++) {
  const a = argv[i];
  if (map && a == "--pub") pub = true;
  else if (map && a == "--json") json = true;
  else if (map && file && a[0] != "-") only = a;
  else if (api && a == "--check") check = true;
  else if (testing && a == "--json") json = true;
  else if (testing && a == "--filter" && i + 1 < argv.length) filter = argv[++i];
  else if (testing && a == "--timeout" && i + 1 < argv.length) i++;  // tests share one process here: no per-test timeout
  else if (testing && a == "--changed") {}  // ... and no traces: every test runs
  else if (a == "--seed" && i + 1 < argv.length) seed = +argv[++i];
  else if (a == "--threads" && i + 1 < argv.length) threads = +argv[++i];
  else if (a == "-w" && !testing) watch = verbose = true;
  else if (a == "-v") verbose = true;
  else if (a[0] == "-") process.stderr.write(usage), process.exit(2);
  else { file = resolve(a); if (testing) testFiles.push(file); }
}
if (testFiles.length > 1) {
  let status = 0;
  for (const path of testFiles) {
    const args = ["test", ...(json ? ["--json"] : []), "--filter", filter, path];
    const child = spawnSync(process.execPath, [fileURLToPath(import.meta.url), ...args], { stdio: "inherit" });
    status = Math.max(status, child.status ?? 2);
  }
  process.exit(status);
}
let watched = [file], rewatch = () => {};
const pending = new Int32Array(new SharedArrayBuffer(4));  // the watcher's messages not yet read (see opts.pending)
// FUT_WASM: another build of fut.wasm (build/fut-mt.wasm, the threads build: isolates run on workers)
const wasmPath = process.env.FUT_WASM ? resolve(process.env.FUT_WASM) : fileURLToPath(new URL("../build/fut.wasm", import.meta.url));
const fut = await createFut({
  ...nodeOptions({
    std: process.env.FUT_STD || fileURLToPath(new URL("../std", import.meta.url)),
    verbose,
    seed: seed || (testing ? 1 : 0),  // a test's isolates interleave the same way every run
    threads,
    interp: !!process.env.FUT_INTERP,  // FUT_INTERP=1: interpret the compiled code (as a static Cloudflare Worker would)
  }),
  wasm: readFileSync(wasmPath),
  exit: (code) => process.exit(code),
  files: (f) => ((watched = f), rewatch()),
  pending,
  dump: process.env.FUT_DUMP && ((n) => (b) => writeFileSync(`${process.env.FUT_DUMP}/m${n++}.wasm`, b))(0),  // debugging: emitted modules
});

const lines = [];  // stdin, split into lines (each keeps its "\n")
let waiting = null, ended = false, partial = "";
function listen(onLine) {
  process.stdin.setEncoding("utf8");
  process.stdin.on("data", (d) => {
    partial += d;
    for (let i; (i = partial.indexOf("\n")) >= 0; partial = partial.slice(i + 1)) onLine(partial.slice(0, i + 1));
  });
  process.stdin.on("end", () => onLine(null));
}

if (explain) process.exit(fut.explain(argv[1]));
if (testing || api || map) {
  if (!file) process.stderr.write(usage), process.exit(2);
  process.exit(testing ? await fut.test(file, json, filter) : api ? await fut.api(file, check) : await fut.map(file, pub, json, only));
}
if (!file) {  // REPL
  listen((l) => { if (l == null) ended = true; else lines.push(l); waiting?.(), (waiting = null); });
  let prompt = "> ";
  for (;;) {
    process.stdout.write(prompt);
    while (!lines.length && !ended) await new Promise((r) => (waiting = r));
    if (!lines.length) process.stdout.write("\n"), process.exit(0);
    prompt = await fut.line(lines.shift());
  }
}
// Poll modification times, like the native watcher, on a thread of its own: this one may be waiting (fut-mt.wasm's
// main isolate, blocked on a channel), so the watcher sets the program's poll bit and counts its message, which the
// program reads at its next poll, or while it waits (see waitFor). Lines typed run in the program.
const watcher = `
  const { parentPort, workerData: { pending, memory, pollAt } } = require("node:worker_threads");
  const { statSync } = require("node:fs");
  let files = [], last = "";
  const stamp = () => files.map((f) => { try { return statSync(f).mtimeMs; } catch { return 0; } }).join();
  parentPort.on("message", (f) => ((files = f), (last = stamp())));  // the files a reload compiled: changes count from now
  setInterval(() => {
    const s = stamp();
    if (s == last) return;
    last = s;
    setTimeout(() => {
      last = stamp(), Atomics.add(pending, 0, 1);
      if (memory) Atomics.or(new Uint32Array(memory.buffer), pollAt >> 2, 1);
      parentPort.postMessage("changed");
    }, 30);
  }, 100);`;
if (watch) {
  const shared = fut.memory.buffer instanceof SharedArrayBuffer;
  const w = new Worker(watcher, { eval: true, workerData: { pending, memory: shared ? fut.memory : null, pollAt: fut.exports.fut_poll() } });
  w.unref(), w.on("message", () => (Atomics.sub(pending, 0, 1), fut.changed()));
  (rewatch = () => w.postMessage(watched))();
  listen((l) => l != null && fut.type(l));
}
const st = await fut.run(file, watch);
process.exit(st);
