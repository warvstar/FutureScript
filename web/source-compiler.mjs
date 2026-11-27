import { readFileSync, statSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const repo = fileURLToPath(new URL("..", import.meta.url));
const std = join(repo, "std");

export const wasmFile = join(repo, "build", statSync(join(repo, "build", "fut-small.wasm"), { throwIfNoEntry: false }) ? "fut-small.wasm" : "fut.wasm");
const { createFut } = await import(pathToFileURL(join(repo, "web", "fut.js")).href);

const wasm = await WebAssembly.compile(readFileSync(wasmFile));
export async function sourceCompiler(here, MAIN) {
  // Where the page sees a file (/app/..., /std/...), and where it is here.
  const ROOT = "/app", STD = "/std";
  const local = (p) => p.startsWith(ROOT + "/") ? join(here, p.slice(ROOT.length + 1)) : p.startsWith(STD + "/") ? join(std, p.slice(STD.length + 1)) : null;

  let files = {}, cache = new Map();
  const modules = new Set(), err = [];
  const fut = await createFut({
    wasm, std: STD, verbose: false,
    read(p) {
      if (cache.has(p)) return cache.get(p);
      const at = local(p);
      if (!at) return null;
      let b;
      try { b = readFileSync(at); } catch { return null; }
      if (!p.endsWith(".js")) files[p] = b.toString("utf8");
      cache.set(p, b);
      return b;
    },
    importModule: (p) => (modules.add(p), import(pathToFileURL(local(p) ?? p).href)),
    write: (fd, b) => err.push(Buffer.from(b).toString()),
  });
  // Keep the compiler and its function table warm, as the browser does.
  // The watcher only compiles. It never runs the program or its reload hook.
  const ex = fut.exports;
  const entry = new TextEncoder().encode(MAIN + "\0");
  const address = ex.fut_alloc(entry.length);
  new Uint8Array(fut.memory.buffer).set(entry, address);
  let loaded = false;
  return async () => {
    files = {};
    err.length = 0;
    cache = new Map();
    try {
      if (loaded) {
        const chunk = ex.fut_compile(address, 1, 0, 0, 0);
        if (chunk > 0) {
          ex.fut_retire(chunk);
          return { files, modules, local };
        }
        // New host imports need the JavaScript module-loading path below.
        if (chunk !== -2) throw new Error(err.join(""));
      }
      if (!(await fut.compile(MAIN, false))) throw new Error(err.join(""));
      loaded = true;
      return { files, modules, local };
    } finally { cache = new Map(); }
  };
}
