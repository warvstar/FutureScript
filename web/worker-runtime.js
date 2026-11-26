// fut in a Cloudflare Worker. A Worker can't compile WebAssembly at run time: only the modules uploaded with it can be
// instantiated. So web/worker.mjs runs each program's compile ahead of time, under Node, and keeps every module the
// compiler emits; here createFut's module() hook hands back the uploaded module with the same bytes, in the same order.
// The compile is deterministic (same files, same steps, same heap), so it emits the same bytes; tier 2, which
// compiles while the program runs, is off. A module that doesn't match fails loudly.
//
// createWorker(bundle) -> the Worker's { fetch }. bundle (written by web/worker.mjs):
//   wasm: fut.wasm (a WebAssembly.Module)       modules: { key: WebAssembly.Module }  (moduleKey of its bytes)
//   files: { "/app/path": text }                the sources the compiles read (nothing else is readable)
//   serve: { main, keys } | null                the program whose fn serve(req) answers requests; keys: its modules
//   tests: [{ main, keys, names }]              test files: POST or GET /__fut/test runs them (see runTests)
//   children: { id: { keys } }                  the programs the tests start with run() (see runChild)
import { createFut, FutError } from "./fut.js";

export const ROOT = "/app";  // where fut sees the bundle's files (the same path at build time)
export function moduleKey(b) {  // length and FNV-1a: install() is synchronous, so no crypto.subtle
  let h = 0x811c9dc5;
  for (let i = 0; i < b.length; i++) h = Math.imul(h ^ b[i], 0x01000193);
  return b.length + "-" + (h >>> 0).toString(16).padStart(8, "0");
}
const enc = new TextEncoder(), dec = new TextDecoder();

// The options both sides give createFut: whatever could change the heap before or during a compile must match.
export function futOptions({ wasm, read, module, write, fetch, run, interp, tryModule }) {
  return { wasm, read, module, write, fetch, run, interp, tryModule, verbose: false, noTier: true, heapMB: 32 };  // 128 MB per isolate
}

// run(): a child program, in a fresh instance. A Worker has no processes, and can't compile, so the build runs the
// tests that call run() and compiles each child they start ahead of time, by id: its args, and its file or source.
// ctx: { wasm, read(path), module(id, path): createFut's module() hook for that child, or null if it has none;
// interp, tryModule: for the interpreting Worker, which compiles nothing and so needs no module() at all }.
const sourceKey = (s) => moduleKey(enc.encode(s));
export const childId = (args, file, source) => JSON.stringify([args, source != null ? "source " + sourceKey(source) : file]);
export async function runChild(ctx, args, file, source) {
  const id = childId(args, file, source), path = source != null ? `/tmp/fut-run-${sourceKey(source)}.fut` : file;
  const read = (p) => (source != null && p == path ? enc.encode(source) : ctx.read(p)), out = [], err = [];
  const write = (fd, b) => (fd == 2 ? err : out).push(dec.decode(b));
  let code = 1;
  try {
    if (args.length && args[0] != "test") throw new Error(`run() with args ${JSON.stringify(args)}: a Worker runs a program, or its tests (args: ["test", ...])`);
    const module = ctx.module(id, path);
    if (!module) throw new Error(`run(${source != null ? "a source" : file}): not compiled ahead of time. The build runs the tests to find the programs they run(); one that changes from run to run can't be known`);
    const fut = await createFut(futOptions({ wasm: ctx.wasm, read, module, write, run: (a, f, s) => runChild(ctx, a, f, s), interp: ctx.interp, tryModule: ctx.tryModule }));
    const fi = args.indexOf("--filter");
    code = args[0] == "test" ? await fut.test(path, args.includes("--json"), fi > 0 ? args[fi + 1] : "") : await fut.run(path, false);
  } catch (e) { err.push(`fut: ${e?.message ?? e}\n`); }
  return { code, out: out.join(""), err: err.join(""), timedOut: false };
}
const bundleCtx = (bundle, read) => ({  // a bundle compiled ahead of time
  wasm: bundle.wasm, read,
  module: (id, path) => (bundle.children?.[id] ? aheadOfTime(bundle.modules, bundle.children[id].keys, path) : null),
});

// A module() hook that serves a program's compile from the uploaded modules, checking each against keys (in order).
export function aheadOfTime(modules, keys, what) {
  let next = 0;
  return (bytes) => {
    const k = moduleKey(bytes), want = keys[next];
    if (k !== want || !modules[k])
      throw new Error(`${what}: module ${next} (${k}) is not one compiled ahead of time (expected ${want ?? "no more modules"}). ` +
        "A Worker can't compile code: rebuild the bundle with web/worker.mjs (did the program, fut.wasm or fut.js change?)");
    return next++, modules[k];
  };
}

function lineWriter(log) {  // write(fd, bytes) -> console, a line at a time
  const part = ["", "", ""];
  return (fd, b) => {
    const s = part[fd] + dec.decode(b), lines = s.split("\n");
    part[fd] = lines.pop();
    for (const l of lines) log(fd, l);
  };
}
const logLine = (fd, l) => (fd == 2 ? console.error(l) : console.log(l));

export function createWorker(bundle) {
  const read = (p) => (p in bundle.files ? enc.encode(bundle.files[p]) : null);
  let served = null;  // the serve program's instance, created by the first request: { fut, failed }

  async function bootServe() {
    const { main, keys } = bundle.serve, err = [];
    const write = lineWriter((fd, l) => (fd == 2 && err.push(l), logLine(fd, l)));
    try {
      const fut = await createFut(futOptions({ wasm: bundle.wasm, read, module: aheadOfTime(bundle.modules, keys, main), write }));
      const st = await fut.start(main);
      if (st) return { failed: `${main} did not ${st == 2 ? "compile" : "load"}:\n${err.join("\n")}` };
      if (!fut.has("serve")) return { failed: `${main} defines no fn serve(req)` };
      return { fut };
    } catch (e) { return { failed: `${main} did not load: ${e?.message ?? e}` }; }
  }

  async function serve(request) {
    if (!bundle.serve) return new Response("this bundle has no program to serve requests (only tests: /__fut/test)\n", { status: 404 });
    served ??= bootServe();
    const s = await served;
    if (s.failed) return console.error(s.failed), new Response(s.failed + "\n", { status: 500 });
    const req = { method: request.method, url: request.url, headers: Object.fromEntries(request.headers), body: await request.text() };
    let v;
    try { v = await s.fut.apply("serve", [req]); } catch (e) {
      console.error("runtime error: " + (e instanceof FutError ? e.text : e?.stack ?? e));
      return new Response("internal error\n", { status: 500 });
    }
    return toResponse(v);
  }

  // Each test file runs in a fresh instance, as `fut test --json` would run it; the response is its lines (each
  // file's summary gets "file"), then a summary of them all. Query (GET) or JSON body (POST): file (repeatable) or
  // files, to run some of the bundle's files (by path below /app, or name); filter, to run the tests whose names
  // contain it.
  async function runTests(request) {
    const url = new URL(request.url);
    let files = url.searchParams.getAll("file"), filter = url.searchParams.get("filter") ?? "";
    if (request.method == "POST") {
      const b = await request.json().catch(() => ({}));
      files = b.files ?? files, filter = b.filter ?? filter;
    }
    const picked = files.length ? bundle.tests.filter((t) => files.some((f) => t.main == f || t.main == ROOT + "/" + f || t.main.endsWith("/" + f))) : bundle.tests;
    if (files.length && !picked.length) return new Response(`none of the bundle's test files match ${files.join(", ")}\n`, { status: 404 });
    const t0 = Date.now(), total = { type: "summary", files: picked.length, passed: 0, failed: 0, errors: 0, pending: 0, ms: 0 };
    let body = "", status = 0;
    for (const t of picked) {
      const r = await runTestFile(bundle, t, filter);
      body += summarize(r.lines, t.main, total), (status = Math.max(status, r.status));
    }
    total.ms = Date.now() - t0, total.status = status;
    return new Response(body + JSON.stringify(total) + "\n", { headers: { "content-type": "application/x-ndjson" } });
  }

  return {
    async fetch(request) {
      const path = new URL(request.url).pathname;
      if (path == "/__fut/test" && bundle.tests?.length) return runTests(request);
      if (path == "/__fut/tests" && bundle.tests?.length)
        return Response.json(bundle.tests.map((t) => ({ file: t.main, tests: t.names })));
      return serve(request);
    },
  };
}

// One test file in a fresh instance: its `fut test --json` lines and exit status (2: did not compile).
export function runTestFile(bundle, t, filter) {
  const read = (p) => (p in bundle.files ? enc.encode(bundle.files[p]) : null);
  return testIn(bundleCtx(bundle, read), aheadOfTime(bundle.modules, t.keys, t.main), t.main, filter);
}
async function testIn(ctx, module, main, filter) {
  const out = [], errs = [], write = (fd, b) => (fd == 1 ? out.push(dec.decode(b)) : errs.push(dec.decode(b)));
  const t = { main };
  let status;
  try {
    const run = (a, f, s) => runChild(ctx, a, f, s);
    const fut = await createFut(futOptions({ wasm: ctx.wasm, read: ctx.read, module, write, run, interp: ctx.interp, tryModule: ctx.tryModule }));
    status = await fut.test(t.main, true, filter || "");
  } catch (e) {
    out.push(JSON.stringify({ type: "error", file: t.main, message: "internal error: " + (e?.message ?? e), trace: "" }) + "\n"), (status = 1);
  }
  if (errs.length) out.push(JSON.stringify({ type: "stderr", file: t.main, text: errs.join("") }) + "\n");
  return { status, lines: out.join("").split("\n").filter(Boolean) };
}

// The interpreting Worker: one static bundle (fut.wasm and the try_call module, both uploaded) that compiles and runs
// whatever the request sends, interpreting the code it compiles (src/interp.cpp). POST /run with JSON
//   { files: { "path": text, ... }, tests: ["path", ...], filter }   -> each test file's `fut test --json` lines, then
//                                                                      a summary (as /__fut/test)
//   { files, main: "path", }                                         -> { code, out, err }: the program run
// Paths are relative (fut sees them below /app). With the secret FUT_TOKEN set (wrangler secret put FUT_TOKEN), every
// request must carry "authorization: Bearer <token>": the Worker runs any code it is sent.
export function createInterpWorker({ wasm, tryModule }) {
  return {
    async fetch(request, env) {
      const path = new URL(request.url).pathname;
      if (request.method == "GET" && env?.PAGES && !path.startsWith("/kv/")) return page(env.PAGES, path);  // public: pages others put there
      if (env?.FUT_TOKEN && !sameText(request.headers.get("authorization") ?? "", "Bearer " + env.FUT_TOKEN))
        return new Response("unauthorized\n", { status: 401 });
      if (path.startsWith("/kv/") && env?.PAGES) {  // GET or PUT /kv/<key>: a JSON document (a swarm's control and scenarios)
        const key = "kv:" + path.slice(4).replace(/[^\w.:-]/g, "");
        if (request.method == "PUT") {
          const text = await request.text();
          try { JSON.parse(text); } catch { return new Response("not JSON\n", { status: 400 }); }
          return await env.PAGES.put(key, text), new Response("ok\n");
        }
        const v = await env.PAGES.get(key);
        return new Response(v ?? "null", { headers: { "content-type": "application/json", "cache-control": "no-store" } });
      }
      if (request.method == "PUT" && path.startsWith("/pages/") && env?.PAGES) {  // PUT /pages/<name>: an HTML page
        const name = path.slice(7).replace(/[^\w.-]/g, "") || "index";
        await env.PAGES.put(name, await request.text(), { metadata: { at: Date.now() } });
        return new Response("ok\n");
      }
      if (request.method != "POST" || new URL(request.url).pathname != "/run")
        return new Response("POST /run {files: {path: text}, tests: [path...] | main: path, filter}\n", { status: 404 });
      const b = await request.json().catch(() => null);
      if (!b?.files || !(b.tests || b.main)) return new Response("expected {files, tests | main}\n", { status: 400 });
      const files = Object.fromEntries(Object.entries(b.files).map(([p, t]) => [ROOT + "/" + p.replace(/^\/+/, ""), t]));
      const ctx = { wasm, tryModule, interp: true, read: (p) => (p in files ? enc.encode(files[p]) : null), module: () => (bytes) => { throw new Error("the interpreter compiled a module"); } };
      if (b.main) return Response.json(await runChild(ctx, [], ROOT + "/" + b.main, null));
      const t0 = Date.now(), total = { type: "summary", files: b.tests.length, passed: 0, failed: 0, errors: 0, pending: 0, ms: 0 };
      let body = "", status = 0;
      for (const f of b.tests) {
        const main = ROOT + "/" + f, r = await testIn(ctx, undefined, main, b.filter);
        body += summarize(r.lines, main, total), (status = Math.max(status, r.status));
      }
      total.ms = Date.now() - t0, total.status = status;
      return new Response(body + JSON.stringify(total) + "\n", { headers: { "content-type": "application/x-ndjson" } });
    },
  };
}
// Pages: with a KV namespace bound as PAGES, GET /<name> serves the HTML stored under name ("/": index), which an
// authorized PUT /pages/<name> stores. A swarm publishes its dashboard this way (swarm.fut, SWARM_DASH_URL). GET and
// PUT /kv/<key> (authorized) read and write JSON documents: the dashboard writes a swarm's control there, the swarm reads it.
async function page(kv, path) {
  const name = path.slice(1).replace(/[^\w.-]/g, "") || "index", html = await kv.get(name);
  if (html == null) return new Response("no page " + name + "\n", { status: 404 });
  return new Response(html, { headers: { "content-type": "text/html; charset=utf-8", "cache-control": "no-store" } });
}
function sameText(a, b) {  // compares in time independent of where they differ
  let d = a.length ^ b.length;
  for (let i = 0; i < b.length; i++) d |= (a.charCodeAt(i) | 0) ^ b.charCodeAt(i);
  return d == 0;
}
function summarize(lines, main, total) {  // a file's lines, its summary given "file"; totals counted
  let body = "";
  for (const line of lines) {
    let j;
    try { j = JSON.parse(line); } catch { j = { type: "output", text: line }; }
    if (j.type == "summary") {
      for (const k of ["passed", "failed", "errors", "pending"]) total[k] += j[k] ?? 0;
      j.file = main;
    } else if (j.type == "compile_error" || j.type == "error") total.errors++;  // the file's tests didn't run
    body += JSON.stringify(j) + "\n";
  }
  return body;
}

// serve()'s result -> a Response: a string is text; a map with status, headers or body is the response itself (a body
// that isn't a string goes as JSON); nil is 204; anything else is JSON.
export function toResponse(v) {
  const isMap = v && typeof v == "object" && !Array.isArray(v);
  if (v == null) return new Response(null, { status: 204 });
  if (typeof v == "string") return new Response(v, { headers: { "content-type": "text/plain; charset=utf-8" } });
  if (!isMap || !("status" in v || "body" in v || "headers" in v)) return Response.json(v);
  const headers = new Headers(v.headers ?? {});
  let body = v.body ?? null;
  if (body != null && typeof body != "string") {
    body = JSON.stringify(body);
    if (!headers.has("content-type")) headers.set("content-type", "application/json");
  }
  return new Response(body, { status: v.status ?? 200, headers });
}
