#!/usr/bin/env node
// Run fut on WebAssembly in Node: node web/run.mjs [-w | -v] [file.fut]   (no file: REPL). Same flags as build/fut,
// including `node web/run.mjs test [--json] [--filter text] file.fut`.
import { readFileSync, statSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
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
if (watch) {  // poll modification times, like the native watcher; lines typed run in the program
  const stamp = () => watched.map((f) => { try { return statSync(f).mtimeMs; } catch { return 0; } }).join();
  let last = stamp();
  rewatch = () => (last = stamp());  // the files a reload compiled: changes are counted from now
  setInterval(() => {
    const s = stamp();
    if (s != last) setTimeout(() => ((last = stamp()), fut.changed()), 30), (last = s);
  }, 100);
  listen((l) => l != null && fut.type(l));
}
const st = await fut.run(file, watch);
process.exit(st);
