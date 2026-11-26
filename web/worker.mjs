#!/usr/bin/env node
// fut on Cloudflare Workers: build bundles that run fut programs and test files with no code generation at run time
// (see web/worker-runtime.js), and run them under Node the way a Worker would.
//
//   node web/worker.mjs build prog.fut outdir         a Worker whose requests go to prog.fut's fn serve(req)
//   node web/worker.mjs build --tests [--shards N | --max-kb K] [--root dir] [--name w] [--heap-mb M] outdir files...
//                                                     test files, in shards: outdir/shard-<i>/ each, and
//                                                     outdir/manifest.json (which tests are in which shard);
//                                                     Workers named w-0, w-1, ... (the same for every version);
//                                                     a wasm heap of M MB (32; a Worker has 128 MB in all)
//   node web/worker.mjs build --interp [--pages kv-id] outdir   one static Worker that compiles and runs whatever it is sent,
//                                                     interpreting the code (POST /run: see createInterpWorker)
//   node web/worker.mjs send bundle|url [--root dir] (--main file | files...) [--filter text]
//                                                     send sources (every .fut, .api and fut.pkg below root) to an
//                                                     interpreting Worker: its tests, or the program's output
//   node web/worker.mjs run bundle [path] [--method M] [--body text]
//                                                     one request to a bundle, under Node, as in a Worker
//   node web/worker.mjs test outdir|bundle [--filter text] [--json]
//                                                     every shard's tests, under Node, as in Workers
// Each bundle is a directory `npx wrangler dev` (or `deploy`) runs: worker.js, fut.wasm, the modules its programs
// compile to (m/*.wasm), their sources (app/, each as <file>.txt), wrangler.toml, and bundle.json (what worker.js imports, for Node).
import { copyFileSync, existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from "node:fs";
import { basename, dirname, join, relative, resolve, sep } from "node:path";
import { gzipSync } from "node:zlib";
import { createFut, tryCallBytes } from "./fut.js";
import { ROOT, createInterpWorker, createWorker, futOptions, moduleKey, runChild, setHeapMB } from "./worker-runtime.js";

const here = dirname(new URL(import.meta.url).pathname), repo = resolve(here, "..");
const COMPAT_DATE = "2025-09-01";
const usage = readFileSync(new URL(import.meta.url), "utf8").split("\n").slice(3, 20).map((l) => l.slice(3)).join("\n") + "\n";
const die = (m, code = 2) => (process.stderr.write(m.endsWith("\n") ? m : m + "\n"), process.exit(code));

// ---- compiling ahead of time: each program in a fresh instance, as the Worker will ----
const futWasm = readFileSync(join(repo, "build", "fut.wasm"));
let futModule, HEAP_MB = 32;  // the wasm heap the bundle's code is compiled for (--heap-mb)
async function compileProgram(root, file, tests) {
  futModule ??= await WebAssembly.compile(futWasm);
  const main = ROOT + "/" + relative(root, file).split(sep).join("/"), keys = [], modules = new Map(), files = new Map(), err = [];
  const read = (p) => {
    if (!p.startsWith(ROOT + "/")) return null;
    let b;
    try { b = readFileSync(join(root, p.slice(ROOT.length + 1))); } catch { return null; }
    const text = b.toString("utf8");
    if (!Buffer.from(text, "utf8").equals(b)) throw new Error(`${p}: not UTF-8 text (a Worker bundle carries only text sources)`);
    return files.set(p, text), b;
  };
  const module = (b) => {
    const k = moduleKey(b);
    return keys.push(k), modules.set(k, b.slice()), new WebAssembly.Module(b);
  };
  const write = (fd, b) => err.push(Buffer.from(b).toString());
  const fut = await createFut(futOptions({ wasm: futModule, read, module, write }));
  const ok = await fut.compile(main, tests);
  if (!ok) throw new Error(`${relative(process.cwd(), file)} did not compile:\n${err.join("")}`);
  if (!tests && !fut.has("serve")) throw new Error(`${relative(process.cwd(), file)} defines no fn serve(req)`);
  if (tests) fut.checkApi();  // it reads the interface files (.api) fut test checks: they go in the bundle too
  const prog = { main, keys, modules, files, children: new Map(), names: tests ? fut.tests() : undefined };
  if (tests && [...files.values()].some((t) => /\brun\s*\(/.test(t))) await findChildren(prog, read, module);
  return prog;
}
// The programs a test file's tests start with run(): run its tests (here, under Node) and compile each child as the
// Worker will (runChild), keeping their modules too.
async function findChildren(prog, read, module) {
  const ctx = {
    wasm: futModule, read,
    module(id, path) {
      const keys = [];
      prog.children.set(id, { keys });
      return (b) => { const m = module(b); return keys.push(prog.keys.pop()), m; };  // module() noted it in prog.keys
    },
  };
  const fut = await createFut(futOptions({ wasm: futModule, read, module: (b) => new WebAssembly.Module(b), write: () => {}, run: (a, f, s) => runChild(ctx, a, f, s) }));
  await fut.test(prog.main, true, "");
}

// ---- writing a bundle ----
function writeBundle(dir, name, progs, serve) {
  rmSync(dir, { recursive: true, force: true }), mkdirSync(join(dir, "m"), { recursive: true });
  const modules = new Map(), files = new Map(), children = {};
  for (const p of progs) {
    for (const [id, c] of p.children) children[id] = c;
    for (const [k, b] of p.modules) modules.set(k, b);
    for (const [f, t] of p.files) files.set(f, t);
  }
  copyFileSync(join(repo, "build", "fut.wasm"), join(dir, "fut.wasm"));
  copyFileSync(join(here, "fut.js"), join(dir, "fut.js")), copyFileSync(join(here, "worker-runtime.js"), join(dir, "runtime.js"));
  for (const [k, b] of modules) writeFileSync(join(dir, "m", k + ".wasm"), b);
  writeFileSync(join(dir, "try.wasm"), tryCallBytes());  // what catches an isolate's error (spawn)
  for (const [f, t] of files) mkdirSync(dirname(join(dir, f)), { recursive: true }), writeFileSync(join(dir, f + ".txt"), t);
  const keys = [...modules.keys()], paths = [...files.keys()];
  const entry = (p) => ({ main: p.main, keys: p.keys, ...(p.names ? { names: p.names } : {}) });
  const bundle = { serve: serve ? entry(progs[0]) : null, tests: serve ? [] : progs.map(entry), children, modules: keys, files: paths, heapMB: HEAP_MB };
  writeFileSync(join(dir, "bundle.json"), JSON.stringify(bundle, null, 1));
  const js = [
    "// Generated by web/worker.mjs: the fut programs of this bundle, compiled ahead of time (see runtime.js).",
    'import { createWorker } from "./runtime.js";', 'import wasm from "./fut.wasm";', 'import tryModule from "./try.wasm";',
    ...keys.map((k, i) => `import m${i} from "./m/${k}.wasm";`),
    ...paths.map((f, i) => `import f${i} from ".${f}.txt";`),
    `const bundle = ${JSON.stringify({ serve: bundle.serve, tests: bundle.tests, children, heapMB: HEAP_MB })};`,
    `bundle.wasm = wasm, bundle.tryModule = tryModule;`,
    `bundle.modules = { ${keys.map((k, i) => `"${k}": m${i}`).join(", ")} };`,
    `bundle.files = { ${paths.map((f, i) => `${JSON.stringify(f)}: f${i}`).join(", ")} };`,
    "export default createWorker(bundle);", "",
  ];
  writeFileSync(join(dir, "worker.js"), js.join("\n"));
  writeFileSync(join(dir, "wrangler.toml"), [
    "# fut.wasm and m/*.wasm import as compiled modules, app/**/*.txt (the fut sources) as text: wrangler's default rules",
    `name = "${name}"`, 'main = "worker.js"', `compatibility_date = "${COMPAT_DATE}"`,
    ...(serve ? [] : ["[limits]", "cpu_ms = 300000  # a request runs whole test files: the Paid plan's most"]), "",
  ].join("\n"));
  return sizeOf(dir);
}
function sizeOf(dir) {  // bytes, and gzipped as an upload is (each file on its own, roughly as wrangler does)
  let raw = 0, gz = 0;
  const walk = (d) => {
    for (const e of readdirSync(d, { withFileTypes: true })) {
      const p = join(d, e.name);
      if (e.isDirectory()) walk(p);
      else if (e.name != "bundle.json" && e.name != "wrangler.toml") {
        const b = readFileSync(p);
        (raw += b.length), (gz += gzipSync(b, { level: 9 }).length);
      }
    }
  };
  return walk(dir), { bytes: raw, gzip: gz };
}
const workerName = (s) => ("fut-" + s.toLowerCase().replace(/[^a-z0-9-]+/g, "-")).replace(/-+/g, "-").replace(/^-|-$/g, "").slice(0, 60);

async function buildServe(file, out) {
  file = resolve(file);
  const p = await compileProgram(dirname(file), file, false), size = writeBundle(resolve(out), workerName(basename(file, ".fut")), [p], true);
  console.log(`${out}: ${p.keys.length} modules, ${(size.bytes / 1024).toFixed(0)} KB (${(size.gzip / 1024).toFixed(0)} KB gzipped)`);
  console.log(`run it: cd ${out} && npx wrangler dev     (deploy: npx wrangler deploy, logged in to Cloudflare)`);
}

const commonDir = (paths) => paths.map(dirname).reduce((a, b) => { while (!(b + sep).startsWith(a + sep) && a != dirname(a)) a = dirname(a); return a; });
async function buildTests(files, out, { shards, maxKb, root, name }) {
  files = files.map((f) => resolve(f));
  root = resolve(root ?? commonDir(files));
  const progs = [];
  let failed = 0;
  for (const f of files) {
    try { progs.push(await compileProgram(root, f, true)); } catch (e) { failed++, process.stderr.write(e.message + "\n"); }
  }
  if (!progs.length) die("nothing to bundle");
  // Each program's own cost: its modules no other program shares (fut_init's are in every one), and its sources.
  const uses = new Map();
  for (const p of progs) for (const k of p.modules.keys()) uses.set(k, (uses.get(k) ?? 0) + 1);
  const shared = [...uses].filter(([, n]) => n == progs.length && progs.length > 1).map(([k]) => k);
  const cost = (p) => [...p.modules].reduce((n, [k, b]) => n + (shared.includes(k) ? 0 : b.length), 0) + [...p.files.values()].reduce((n, t) => n + t.length, 0);
  let bins;
  if (shards) {  // N shards, balanced by cost (largest first, into the lightest)
    bins = Array.from({ length: Math.min(shards, progs.length) }, () => ({ progs: [], cost: 0 }));
    for (const p of [...progs].sort((a, b) => cost(b) - cost(a))) {
      const b = bins.reduce((x, y) => (y.cost < x.cost ? y : x));
      b.progs.push(p), (b.cost += cost(p));
    }
    for (const b of bins) b.progs.sort((x, y) => progs.indexOf(x) - progs.indexOf(y));
  } else {  // as few shards as fit maxKb (uncompressed, beyond what every shard carries), in the given order
    const budget = (maxKb ?? 8192) * 1024;
    bins = [{ progs: [], cost: 0 }];
    for (const p of progs) {
      let b = bins.at(-1);
      if (b.progs.length && b.cost + cost(p) > budget) bins.push((b = { progs: [], cost: 0 }));
      b.progs.push(p), (b.cost += cost(p));
    }
  }
  out = resolve(out), rmSync(out, { recursive: true, force: true }), mkdirSync(out, { recursive: true });
  const manifest = { root, built: new Date().toISOString(), shards: [] };
  for (const [i, b] of bins.entries()) {
    const dir = join(out, `shard-${i}`), size = writeBundle(dir, name ? `${name}-${i}` : workerName(`${basename(root)}-tests-${i}`), b.progs, false);
    const tests = b.progs.reduce((n, p) => n + p.names.length, 0);
    manifest.shards.push({ dir: `shard-${i}`, ...size, modules: new Set(b.progs.flatMap((p) => p.keys)).size, tests,
      files: b.progs.map((p) => ({ file: p.main, source: relative(root, join(root, p.main.slice(ROOT.length + 1))), tests: p.names, bytes: cost(p) })) });
    console.log(`shard-${i}: ${b.progs.length} files, ${tests} tests, ${(size.bytes / 1024).toFixed(0)} KB (${(size.gzip / 1024).toFixed(0)} KB gzipped)`);
  }
  writeFileSync(join(out, "manifest.json"), JSON.stringify(manifest, null, 1));
  console.log(`${out}/manifest.json: ${progs.length} files in ${bins.length} shard${bins.length == 1 ? "" : "s"}`);
  if (failed) die(`${failed} file${failed == 1 ? "" : "s"} did not compile (left out)`, 1);
}

function buildInterp(out, pagesKv) {  // pagesKv: a KV namespace id, bound as PAGES (GET /<name> serves pages)
  out = resolve(out), rmSync(out, { recursive: true, force: true }), mkdirSync(out, { recursive: true });
  copyFileSync(join(repo, "build", "fut.wasm"), join(out, "fut.wasm"));
  copyFileSync(join(here, "fut.js"), join(out, "fut.js")), copyFileSync(join(here, "worker-runtime.js"), join(out, "runtime.js"));
  writeFileSync(join(out, "try.wasm"), tryCallBytes());
  writeFileSync(join(out, "worker.js"), [
    "// Generated by web/worker.mjs: fut, compiling and interpreting whatever POST /run sends (see runtime.js).",
    'import { createInterpWorker } from "./runtime.js";', 'import wasm from "./fut.wasm";', 'import tryModule from "./try.wasm";',
    "export default createInterpWorker({ wasm, tryModule });", ""].join("\n"));
  writeFileSync(join(out, "bundle.json"), JSON.stringify({ interp: true }));
  writeFileSync(join(out, "wrangler.toml"), [`name = "fut-interp"`, 'main = "worker.js"', `compatibility_date = "${COMPAT_DATE}"`,
    "[limits]", "cpu_ms = 300000  # a request runs whole test files, interpreted: the Paid plan's most", "",
    ...(pagesKv ? ["[[kv_namespaces]]", 'binding = "PAGES"', `id = "${pagesKv}"`, ""] : [])].join("\n"));
  const size = sizeOf(out);
  console.log(`${out}: ${(size.bytes / 1024).toFixed(0)} KB (${(size.gzip / 1024).toFixed(0)} KB gzipped)`);
}
function collect(root) {  // the sources a program may read: .fut, .api and fut.pkg files below root, by relative path
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
async function send(target, { root, main, tests, filter }) {
  root = resolve(root ?? (main ? dirname(main) : commonDir(tests.map((f) => resolve(f)))));
  const rel = (f) => relative(root, resolve(f)).split(sep).join("/");
  const body = JSON.stringify({ files: collect(root), ...(main ? { main: rel(main) } : { tests: tests.map(rel), filter }) });
  let fetchIt;
  if (/^https?:/.test(target)) fetchIt = (r) => fetch(r);
  else {
    const w = await interpWorkerOf(target);
    fetchIt = (r) => w.fetch(r);
  }
  const headers = process.env.FUT_WORKER_TOKEN ? { authorization: "Bearer " + process.env.FUT_WORKER_TOKEN } : {};  // the Worker's FUT_TOKEN
  const t0 = performance.now(), r = await fetchIt(new Request(new URL("/run", /^https?:/.test(target) ? target : "http://localhost"), { method: "POST", body, headers }));
  const text = await r.text();
  process.stdout.write(text.endsWith("\n") ? text : text + "\n");
  process.stderr.write(`${r.status} in ${(performance.now() - t0).toFixed(0)} ms (${(body.length / 1024).toFixed(0)} KB sent)\n`);
  return r.status == 200 ? 0 : 1;
}
async function interpWorkerOf(dir) {
  const wasm = await WebAssembly.compile(readFileSync(join(dir, "fut.wasm"))), tryModule = new WebAssembly.Module(readFileSync(join(dir, "try.wasm")));
  return forbidCodegen(), createInterpWorker({ wasm, tryModule });
}

// ---- running a bundle under Node, as a Worker would: no code generation once the bundle's modules are loaded ----
async function loadBundle(dir) {
  const b = JSON.parse(readFileSync(join(dir, "bundle.json"), "utf8"));
  b.wasm = await WebAssembly.compile(readFileSync(join(dir, "fut.wasm")));
  b.modules = Object.fromEntries(b.modules.map((k) => [k, new WebAssembly.Module(readFileSync(join(dir, "m", k + ".wasm")))]));
  b.files = Object.fromEntries(b.files.map((f) => [f, readFileSync(join(dir, f + ".txt"), "utf8")]));
  if (existsSync(join(dir, "try.wasm"))) b.tryModule = new WebAssembly.Module(readFileSync(join(dir, "try.wasm")));
  return b;
}
function forbidCodegen() {  // as workerd does: "Wasm code generation disallowed by embedder"
  const W = WebAssembly, no = () => { throw new W.CompileError("Wasm code generation disallowed by embedder"); };
  const inst = W.instantiate.bind(W);
  W.Module = new Proxy(W.Module, { construct: no });
  W.compile = async () => no();
  W.instantiate = async (m, imports) => (m instanceof W.Module ? inst(m, imports) : no());
}
async function workerOf(dir) {
  const b = await loadBundle(dir);
  return forbidCodegen(), createWorker(b);
}
async function run(dir, path = "/", method = "GET", body) {
  const w = await workerOf(dir), url = new URL(path, "http://localhost");
  const r = await w.fetch(new Request(url, { method, body: method == "GET" || method == "HEAD" ? undefined : body ?? "" }));
  console.log(r.status, r.headers.get("content-type") ?? ""), process.stdout.write(await r.text() + "\n");
  return r.status < 400 ? 0 : 1;
}
async function test(out, filter, json) {
  const dirs = existsSync(join(out, "manifest.json")) ? JSON.parse(readFileSync(join(out, "manifest.json"), "utf8")).shards.map((s) => join(out, s.dir)) : [out];
  const bundles = [];
  for (const d of dirs) bundles.push(await loadBundle(d));
  forbidCodegen();
  let status = 0, passed = 0, failedN = 0, errors = 0, pending = 0;
  const t0 = performance.now();
  for (const b of bundles) {
    const r = await createWorker(b).fetch(new Request("http://localhost/__fut/test" + (filter ? "?filter=" + encodeURIComponent(filter) : "")));
    for (const line of (await r.text()).split("\n").filter(Boolean)) {
      const j = JSON.parse(line);
      if (j.type == "summary" && j.files != null) {
        status = Math.max(status, j.status), (passed += j.passed), (failedN += j.failed), (errors += j.errors), (pending += j.pending);
        continue;
      }
      if (json) console.log(line);
      else if (j.type == "test" && j.status != "pass") console.log(`${j.status.toUpperCase().padEnd(7)} ${j.name} (${j.file}:${j.line})${j.message ? ": " + j.message : ""}`);
      else if (j.type == "compile_error" || j.type == "error") console.log(`${j.type} ${j.file ?? ""}: ${j.message}`);
      else if (j.type == "stderr" && !json) process.stderr.write(j.text);
    }
  }
  const ms = performance.now() - t0, sum = { type: "summary", shards: bundles.length, passed, failed: failedN, errors, pending, ms: +ms.toFixed(1) };
  console.log(json ? JSON.stringify(sum) : `${bundles.length} shard${bundles.length == 1 ? "" : "s"}: ${passed} passed, ${failedN} failed, ${errors} errors, ${pending} pending (${ms.toFixed(0)} ms)`);
  return status;
}

// ---- the command line ----
const [cmd, ...args] = process.argv.slice(2);
const flag = (name, hasValue) => {
  const i = args.indexOf(name);
  if (i < 0) return undefined;
  return hasValue ? args.splice(i, 2)[1] : (args.splice(i, 1), true);
};
if (cmd == "build") {
  const tests = flag("--tests"), shards = flag("--shards", true), maxKb = flag("--max-kb", true), root = flag("--root", true);
  const pages = flag("--pages", true), name = flag("--name", true), heap = flag("--heap-mb", true);
  if (heap) setHeapMB((HEAP_MB = +heap));
  if (flag("--interp")) {
    if (args.length != 1) die(usage);
    buildInterp(args[0], pages);
  } else if (tests) {
    if (args.length < 2) die(usage);
    await buildTests(args.slice(1), args[0], { shards: shards && +shards, maxKb: maxKb && +maxKb, root, name }).catch((e) => die(e.message));
  } else {
    if (args.length != 2) die(usage);
    await buildServe(args[0], args[1]).catch((e) => die(e.message));
  }
} else if (cmd == "run") {
  const method = flag("--method", true), body = flag("--body", true);
  if (!args.length) die(usage);
  process.exit(await run(resolve(args[0]), args[1], method, body));
} else if (cmd == "send") {
  const root = flag("--root", true), main = flag("--main", true), filter = flag("--filter", true);
  if (!args.length || (!main && args.length < 2)) die(usage);
  process.exit(await send(args[0], { root, main, tests: args.slice(1), filter }));
} else if (cmd == "test") {
  const filter = flag("--filter", true), json = flag("--json");
  if (args.length != 1) die(usage);
  process.exit(await test(resolve(args[0]), filter, json));
} else die(usage);
