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
import { createFut, FutError } from "./fut.js";

export const ROOT = "/app";  // where fut sees the bundle's files (the same path at build time)
export function moduleKey(b) {  // length and FNV-1a: install() is synchronous, so no crypto.subtle
  let h = 0x811c9dc5;
  for (let i = 0; i < b.length; i++) h = Math.imul(h ^ b[i], 0x01000193);
  return b.length + "-" + (h >>> 0).toString(16).padStart(8, "0");
}
const enc = new TextEncoder(), dec = new TextDecoder();

// The options both sides give createFut: whatever could change the heap before or during a compile must match.
export function futOptions({ wasm, read, module, write, fetch }) {
  return { wasm, read, module, write, fetch, verbose: false, noTier: true };
}

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
      for (const line of r.lines) {
        let j;
        try { j = JSON.parse(line); } catch { j = { type: "output", text: line }; }
        if (j.type == "summary") {
          for (const k of ["passed", "failed", "errors", "pending"]) total[k] += j[k] ?? 0;
          j.file = t.main;
        } else if (j.type == "compile_error" || j.type == "error") total.errors++;  // the file's tests didn't run
        body += JSON.stringify(j) + "\n";
      }
      status = Math.max(status, r.status);
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
export async function runTestFile(bundle, t, filter) {
  const out = [], read = (p) => (p in bundle.files ? enc.encode(bundle.files[p]) : null);
  const errs = [], write = (fd, b) => (fd == 1 ? out.push(dec.decode(b)) : errs.push(dec.decode(b)));
  let status;
  try {
    const fut = await createFut(futOptions({ wasm: bundle.wasm, read, module: aheadOfTime(bundle.modules, t.keys, t.main), write }));
    status = await fut.test(t.main, true, filter || "");
  } catch (e) {
    out.push(JSON.stringify({ type: "error", file: t.main, message: "internal error: " + (e?.message ?? e), trace: "" }) + "\n"), (status = 1);
  }
  if (errs.length) out.push(JSON.stringify({ type: "stderr", file: t.main, text: errs.join("") }) + "\n");
  return { status, lines: out.join("").split("\n").filter(Boolean) };
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
