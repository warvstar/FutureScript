#!/usr/bin/env node
// Run fut on WebAssembly in Node: node web/run.mjs [-w | -v] [file.fut]   (no file: REPL). Same flags as build/fut.
import { readFileSync, statSync, writeFileSync, writeSync } from "node:fs";
import { resolve } from "node:path";
import { createFut } from "./fut.js";

let file = "", watch = false, verbose = false;
for (const a of process.argv.slice(2)) {
  if (a == "-w") watch = verbose = true;
  else if (a == "-v") verbose = true;
  else if (a[0] == "-") process.stderr.write("usage: node web/run.mjs [-w | -v] [file.fut]   (no file: REPL)\n"), process.exit(2);
  else file = resolve(a);
}
function writeAll(fd, b) {  // stdout may be a non-blocking pipe
  for (let o = 0; o < b.length;) {
    try { o += writeSync(fd, b, o); } catch (e) { if (e.code != "EAGAIN") throw e; }
  }
}
let watched = [file], rewatch = () => {};
const fut = await createFut({
  wasm: readFileSync(new URL("../build/fut.wasm", import.meta.url)),
  verbose,
  read(p) { try { return readFileSync(p); } catch { return null; } },
  write: writeAll,
  getenv: (n) => process.env[n],
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
