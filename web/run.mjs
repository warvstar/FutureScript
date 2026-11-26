#!/usr/bin/env node
// Run fut on WebAssembly in Node: node web/run.mjs [-w | -v] [file.fut]   (no file: REPL). Same flags as build/fut,
// including `node web/run.mjs test [--json] [--filter text] file.fut`.
import { spawnSync } from "node:child_process";
import { readFileSync, statSync, unlinkSync, writeFileSync, writeSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { createFut } from "./fut.js";

let file = "", watch = false, verbose = false, json = false, filter = "";
const argv = process.argv.slice(2), testing = argv[0] == "test";
for (let i = testing ? 1 : 0; i < argv.length; i++) {
  const a = argv[i];
  if (testing && a == "--json") json = true;
  else if (testing && a == "--filter" && i + 1 < argv.length) filter = argv[++i];
  else if (testing && a == "--timeout" && i + 1 < argv.length) i++;  // tests share one process here: no per-test timeout
  else if (a == "-w" && !testing) watch = verbose = true;
  else if (a == "-v") verbose = true;
  else if (a[0] == "-") process.stderr.write("usage: node web/run.mjs [-w | -v] [file.fut]   (no file: REPL)\n       node web/run.mjs test [--json] [--filter text] file.fut\n"), process.exit(2);
  else file = resolve(a);
}
function writeAll(fd, b) {  // stdout may be a non-blocking pipe
  for (let o = 0; o < b.length;) {
    try { o += writeSync(fd, b, o); } catch (e) { if (e.code != "EAGAIN") throw e; }
  }
}
let watched = [file], rewatch = () => {}, runs = 0;
const fut = await createFut({
  wasm: readFileSync(new URL("../build/fut.wasm", import.meta.url)),
  verbose,
  read(p) { try { return readFileSync(p); } catch { return null; } },
  write: writeAll,
  getenv: (n) => process.env[n],
  files: (f) => ((watched = f), rewatch()),
  run(args, file, source, timeout) {  // run(): this script again, in a child process
    let tmp = null;
    if (source != null) writeFileSync((tmp = join(tmpdir(), `fut-run-${process.pid}-${runs++}.fut`)), source), (file = tmp);
    const r = spawnSync(process.execPath, [fileURLToPath(import.meta.url), ...args, file], { timeout: timeout * 1000, stdio: ["ignore", "pipe", "pipe"], encoding: "utf8" });
    if (tmp) unlinkSync(tmp);
    const timedOut = r.error?.code == "ETIMEDOUT";
    return { code: r.status ?? 128 + (r.signal == "SIGKILL" ? 9 : r.signal == "SIGTERM" ? 15 : 6), out: r.stdout ?? "", err: r.stderr ?? "", timedOut };
  },
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

if (testing) {
  if (!file) process.stderr.write("usage: node web/run.mjs test [--json] [--filter text] file.fut\n"), process.exit(2);
  process.exit(await fut.test(file, json, filter));
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
