#!/usr/bin/env node
// A fut program with a page (std/ui) as a static site, for any host that serves files:
//
//   node web/site.mjs [--root dir] [--title text] [--copy path]... [--no-prerender] app.fut outdir
//
// outdir gets index.html, fut.js and fut.wasm (build/fut-small.wasm, the compiler and runtime: the page compiles the program as it loads,
// as the dev page does), files.json (every source the program reads: its own files and std's, one request), and the
// JavaScript modules it uses (std/ui's dom.js) at their paths. --root: the directory whose files the program may read
// (the main file's, by default); --copy: a file or directory below root the page fetches itself (data, images), copied
// to the same path. In the page, the root is /app and std is /std: ui.load("data/x.json") stays relative to the page.
// The page's first view is prerendered into index.html (--no-prerender: not), so it shows before fut.wasm has loaded.
import { copyFileSync, cpSync, mkdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { dirname, join, relative, resolve, sep } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { createFut } from "./fut.js";

const repo = fileURLToPath(new URL("..", import.meta.url));
const usage = "usage: node web/site.mjs [--root dir] [--title text] [--copy path]... [--no-prerender] app.fut outdir\n";
const die = (m) => (process.stderr.write(m.endsWith("\n") ? m : m + "\n"), process.exit(2));
let root = null, title = null, prerender = true;
const copies = [], rest = [];
const argv = process.argv.slice(2);
for (let i = 0; i < argv.length; i++) {
  const a = argv[i];
  if (a == "--root" && i + 1 < argv.length) root = resolve(argv[++i]);
  else if (a == "--title" && i + 1 < argv.length) title = argv[++i];
  else if (a == "--copy" && i + 1 < argv.length) copies.push(resolve(argv[++i]));
  else if (a == "--no-prerender") prerender = false;
  else if (a[0] == "-") die(usage);
  else rest.push(a);
}
if (rest.length != 2) die(usage);
const file = resolve(rest[0]), out = resolve(rest[1]);
root ??= dirname(file);
const inside = (p, dir) => !relative(dir, p).startsWith("..") && !relative(dir, p).startsWith(sep);
if (!inside(file, root)) die(`${rest[0]} is not below --root ${root}`);
const std = join(repo, "std");
// fut-small.wasm (python3 build.py wasm): the compiler built for size, the runtime as fut.wasm's
const wasmFile = join(repo, "build", "fut-small.wasm");
try { statSync(wasmFile); } catch { die("no build/fut-small.wasm: python3 build.py wasm makes it"); }

// Where the page sees a file (/app/..., /std/...), and where it is here.
const ROOT = "/app", STD = "/std";
const local = (p) => p.startsWith(ROOT + "/") ? join(root, p.slice(ROOT.length + 1)) : p.startsWith(STD + "/") ? join(std, p.slice(STD.length + 1)) : null;
const pagePath = (abs) => inside(abs, root) ? ROOT + "/" + relative(root, abs).split(sep).join("/") : null;
const main = pagePath(file);

// Compile it here, as the page will, keeping what it reads and the JavaScript modules it loads.
const files = {}, modules = new Set(), err = [];
const fut = await createFut({
  wasm: readFileSync(wasmFile),
  std: STD,
  verbose: false,
  read(p) {
    const at = local(p);
    if (!at) return null;
    let b;
    try { b = readFileSync(at); } catch { return null; }
    if (!p.endsWith(".js")) files[p] = b.toString("utf8");
    return b;
  },
  importModule: (p) => (modules.add(p), import(pathToFileURL(local(p) ?? p).href)),
  write: (fd, b) => err.push(Buffer.from(b).toString()),
});
if (!(await fut.compile(main, false))) die(`${rest[0]} did not compile:\n${err.join("")}`);

// The page as it first shows, run here on std/ui's document of its own, so the site shows it before fut.wasm has
// loaded (fetches stay loading). fut then builds the page again over it, and its style sheets replace these.
let shown = "", sheets = "";
const domFile = join(std, "ui", "dom.js");
if (prerender && modules.has(STD + "/ui/dom.js")) {
  const dom = await import(pathToFileURL(domFile).href);  // (the instance the program's externs use)
  dom.prerender();
  err.length = 0;
  const st = await Promise.race([fut.run(main, false), new Promise((r) => setTimeout(() => r("time"), 10000))]);
  const body = dom.query("body");
  if (st === 0) {
    shown = body.childNodes.map((n) => dom.html(n)).join("");
    for (const s of dom.query("head")?.childNodes ?? []) sheets += `<style data-prerendered>${s.textContent.replace(/<\/style/gi, "<\\/style")}</style>\n`;
  } else process.stderr.write(`site: not prerendered (${st === "time" ? "its top level ran for 10 s" : "it failed under Node: " + err.join("").trim()})\n`);
}

rmSync(out, { recursive: true, force: true });
mkdirSync(out, { recursive: true });
copyFileSync(join(repo, "web", "fut.js"), join(out, "fut.js"));
copyFileSync(wasmFile, join(out, "fut.wasm"));
writeFileSync(join(out, "files.json"), JSON.stringify(files));
for (const m of modules) {
  const at = local(m);
  if (!at) die(`a JavaScript module outside the root and std: ${m}`);
  mkdirSync(dirname(join(out, m)), { recursive: true }), copyFileSync(at, join(out, m.slice(1)));
}
for (const c of copies) {
  if (!inside(c, root)) die(`--copy ${relative(process.cwd(), c)}: not below the root ${root}`);
  const to = join(out, relative(root, c));
  mkdirSync(dirname(to), { recursive: true });
  statSync(c).isDirectory() ? cpSync(c, to, { recursive: true }) : copyFileSync(c, to);
}
const esc = (s) => s.replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);
title ??= file.split(sep).pop().replace(/\.fut$/, "");
writeFileSync(join(out, "index.html"), `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>${esc(title)}</title>
<link rel="icon" href="data:,">
<link rel="modulepreload" href="fut.js">
<link rel="preload" href="fut.wasm" as="fetch" crossorigin>
<link rel="preload" href="files.json" as="fetch" crossorigin>
${sheets}</head>
<body>
<div id="ui">${shown}</div>
<script type="module">
// fut: compile ${main} (files.json holds its sources) and run it; its page goes into #ui
import { createFut } from "./fut.js";
const at = (p) => new URL(p, import.meta.url);
const compile = (r) => WebAssembly.compileStreaming(r).catch(async () => WebAssembly.compile(await (await fetch(at("fut.wasm"))).arrayBuffer()));
const [files, wasm] = await Promise.all([fetch(at("files.json")).then((r) => r.json()), fetch(at("fut.wasm")).then(compile)]);
const enc = new TextEncoder(), dec = new TextDecoder();
const fut = await createFut({
  wasm, std: "/std", serve: true, verbose: false,
  read: (p) => (files[p] == null ? null : enc.encode(files[p])),
  importModule: (p) => import(at("." + p)),
  write: (fd, b) => (fd == 2 ? console.error : console.log)(dec.decode(b).replace(/\\n$/, "")),
});
await fut.run(${JSON.stringify(main)}, true);
</script>
</body>
</html>
`);
const size = (p) => statSync(join(out, p)).size;
const kb = (n) => (n / 1024).toFixed(0) + " KB";
console.log(`${relative(process.cwd(), out) || "."}: index.html, fut.wasm ${kb(size("fut.wasm"))}, fut.js ${kb(size("fut.js"))}, ` +
  `files.json ${kb(size("files.json"))} (${Object.keys(files).length} sources)` + (shown ? `, the first view prerendered (${kb(shown.length + sheets.length)})` : "") + (modules.size ? `, ${[...modules].map((m) => m.slice(1)).join(", ")}` : ""));
