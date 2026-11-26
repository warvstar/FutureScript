// fut's WebAssembly host, shared by Node (web/run.mjs) and the browser (web/index.html). It instantiates build/fut.wasm
// (the compiler and runtime), installs the modules that compiler emits into the shared function table, suspends and
// resumes fiber stacks with JSPI, and drives the program: run it, reload changed files at safepoints, rerun main().
//
// createFut(opts) options:
//   wasm: fut.wasm (its bytes, or a WebAssembly.Module)
//   module(bytes): the WebAssembly.Module for a module the compiler emitted (default: compile it; a Cloudflare Worker,
//     which can't compile code, finds the one compiled ahead of time -- web/worker.mjs)
//   noTier: bool -- no tier 2, so nothing compiles once the program has loaded
//   fetch(url, init): the platform's fetch, for fut's fetch() and fetchAll() (default: globalThis.fetch)
//           read(path): file bytes or null      write(fd, bytes): stdout (1) / stderr (2)
//   verbose: bool                     getenv(name): string or undefined   window(w, h): bool, present(rgba, w, h): Promise
//   run(args, file, source, timeout): {code, out, err, timedOut}, or a Promise of it -- fut in a child process (run()), if possible
//   save(path, text): bool -- write a file (text null: remove it), for `fut api`
//   importModule(path, stamp): Promise of a JS module's namespace (stamp: change it to load the file again)
//   stamp(path): anything that changes when the file does (a modification time), for reloading modules
//   std: the directory of the standard library's packages (import "std/strings")
export class Restart extends Error {}  // unwinds to the driver, which reruns main()
export class Exit extends Error { constructor(code) { super("exit " + code), (this.code = code); } }
export class FutError extends Error { constructor(text) { super(text.split("\n")[0]), (this.text = text); } }  // from apply()

export async function createFut(opts) {
  const enc = new TextEncoder(), dec = new TextDecoder();
  const tag = new WebAssembly.Tag({ parameters: ["i64"] });
  let ex, mem, table, spG, held = new Uint8Array(0);
  const u8 = () => new Uint8Array(mem.buffer), dv = () => new DataView(mem.buffer);
  const cstr = (p) => { const m = u8(); let e = p; while (m[e]) e++; return dec.decode(m.subarray(p, e)); };
  const write = (fd, s) => opts.write(fd, typeof s == "string" ? enc.encode(s) : s);
  function withStr(s, f) {  // f(pointer to s as a C string)
    const b = enc.encode(s), p = ex.fut_alloc(b.length + 1);
    u8().set(b, p), (u8()[p + b.length] = 0);
    try { return f(p); } finally { ex.fut_free(p); }
  }
  const str = (v) => cstr(ex.fut_str(v));
  Error.stackTraceLimit = 64;

  // ---- WASI: just what the C library uses ----
  const wasi = {
    fd_write(fd, iov, n, nw) {
      const d = dv(), parts = [];
      let total = 0;
      for (let i = 0; i < n; i++) {
        const p = d.getUint32(iov + i * 8, true), l = d.getUint32(iov + i * 8 + 4, true);
        parts.push(u8().slice(p, p + l)), (total += l);
      }
      const all = new Uint8Array(total);
      for (let i = 0, o = 0; i < parts.length; o += parts[i++].length) all.set(parts[i], o);
      write(fd, all), d.setUint32(nw, total, true);
      return 0;
    },
    fd_fdstat_get(fd, p) {  // a character device: stdout is line buffered
      const d = dv();
      d.setUint8(p, 2), d.setUint16(p + 2, 0, true), d.setBigUint64(p + 8, 0n, true), d.setBigUint64(p + 16, 0n, true);
      return 0;
    },
    clock_time_get(id, _prec, p) {
      const ns = id == 0 ? Date.now() * 1e6 : performance.now() * 1e6;
      return dv().setBigUint64(p, BigInt(Math.round(ns)), true), 0;
    },
    fd_close: () => 0, fd_seek: () => 70,
    proc_exit(code) { throw new Exit(code); },
  };

  // ---- extern fn: JS functions. No library: a small libc, then Math, then globals; "a.b": that object's ----
  const libc = {
    strlen: (s) => enc.encode(s).length, abs: Math.abs, labs: Math.abs, atoi: (s) => parseInt(s, 10) || 0,
    atof: (s) => parseFloat(s) || 0, getenv: (n) => opts.getenv?.(n) ?? null, ldexp: (x, e) => x * 2 ** e,
    pow: Math.pow, sqrt: Math.sqrt, floor: Math.floor, ceil: Math.ceil, fabs: Math.abs, fmod: (a, b) => a % b,
  };
  const externs = [], externWasm = [];
  // A package's modules (js jwt ./jwt.js, wasm zlib ./zlib.wasm), by lib ("js:/path"): loaded before the compile that
  // binds them (it asks: fut_info(3)), and again when their file changes; each bound function keeps its index, so a
  // reloaded module's functions replace the old ones under the code already calling them.
  const modules = new Map(), bound = new Map();  // bound: lib + "\n" + name -> extern index
  const pick = (m, file, name) => (typeof m.exports?.[name] == "function" ? m.exports[name]
    : () => { throw new Error(`${name} is not exported by ${file} any more`); });
  async function loadModule(lib) {
    const at = lib.indexOf(":"), kind = lib.slice(0, at), file = lib.slice(at + 1), stamp = opts.stamp?.(file), old = modules.get(lib);
    try {
      let exports;
      if (kind == "js") {
        if (!opts.importModule) throw new Error("this host cannot load JavaScript modules");
        exports = await opts.importModule(file, stamp);
      } else {
        const bytes = opts.read(file);
        if (!bytes) throw new Error("cannot read it");
        exports = (await WebAssembly.instantiate(bytes, {})).instance.exports;
      }
      const m = { exports, stamp, kind };
      modules.set(lib, m);
      for (const [key, i] of bound) if (key.startsWith(lib + "\n")) externs[i] = pick(m, file, key.slice(lib.length + 1));
      if (old) opts.verbose && write(2, `[fut] reloaded ${file}\n`);
      return true;
    } catch (e) {
      write(2, `cannot load module ${file}: ${e?.message ?? e}\n`);
      modules.set(lib, old && !old.failed ? { ...old, stamp } : { failed: true, stamp });  // a broken edit keeps the old one
      return false;
    }
  }
  async function refreshModules() {  // before a compile: modules whose files changed load again
    if (!opts.stamp) return;
    for (const [lib, m] of [...modules]) if (opts.stamp(lib.slice(lib.indexOf(":") + 1)) !== m.stamp) await loadModule(lib);
  }
  async function compileWith(f) {  // f() compiles; when it waits for modules (-2), load them and compile again
    await refreshModules();
    for (;;) {
      const s = f();
      if (s != -2) return s;
      let any = false;
      for (const lib of cstr(ex.fut_info(3)).split("\n").filter(Boolean)) if (!modules.has(lib)) await loadModule(lib), (any = true);
      if (!any) return 0;
    }
  }
  function externLookup(lib, name) {
    let f;
    if (lib.startsWith("js:") || lib.startsWith("wasm:")) {
      const m = modules.get(lib);
      if (!m) return -3;  // not loaded yet: the compile reports it, and compileWith loads it
      if (m.failed) return -2;
      if (typeof m.exports[name] != "function") return -1;
      const key = lib + "\n" + name;
      let i = bound.get(key);
      if (i == null) bound.set(key, (i = externs.push(null) - 1));
      return (externs[i] = m.exports[name]), (externWasm[i] = m.kind == "wasm"), i;
    }
    if (!lib) f = libc[name] ?? Math[name] ?? globalThis[name];
    else {
      let o = globalThis;
      for (const k of lib.split(".")) if ((o = o?.[k]) == null) return -2;
      f = o[name]?.bind?.(o);
    }
    return typeof f == "function" ? externs.push(f) - 1 : -1;
  }
  // Values of type any (and fn) cross as JSON text; what is not data crosses by reference (see jsval.cpp): fut
  // functions as JS functions that call them, JS objects that are not plain data as ids fut holds as Refs.
  const MARK = "\0", FUT = Symbol("fut function");
  const refs = new Map(), refIds = new Map();  // id -> value, value -> id: one id per value while fut holds it
  let refNext = 1;  // never reused, so an id names one value
  const refOf = (v) => { let id = refIds.get(v); if (id == null) refIds.set(v, (id = refNext++)), refs.set(id, v); return id; };
  const unpin = globalThis.FinalizationRegistry ? new FinalizationRegistry((id) => ex.fut_unpin(id)) : { register() {} };  // (older Workers lack it)
  function futFn(id) {
    const f = (...args) => callFut(id, args);
    return (f[FUT] = id), unpin.register(f, id), f;
  }
  const fromFut = (s) => JSON.parse(s, (k, v) => {
    if (!v || typeof v != "object" || Array.isArray(v) || !(MARK in v)) return v;
    return v[MARK] == "fn" ? futFn(v.id) : v[MARK] == "js" ? refs.get(v.id) : Number(v.v);
  });
  const toFut = (v) => JSON.stringify(v, (k, x) => {
    if (typeof x == "number") return Number.isFinite(x) ? x : { [MARK]: "num", v: String(x) };
    if (typeof x == "bigint") return Number(x);
    if (x === undefined) return null;
    if (typeof x == "function") return x[FUT] != null ? { [MARK]: "fn", id: x[FUT] } : { [MARK]: "js", id: refOf(x) };
    if (typeof x == "symbol") return { [MARK]: "js", id: refOf(x) };
    if (x && typeof x == "object" && !Array.isArray(x) && !(MARK in x)) {
      const p = Object.getPrototypeOf(x);
      if (p !== Object.prototype && p !== null) return { [MARK]: "js", id: refOf(x) };  // a class's instance, a DOM node
    }
    return x;
  });
  // A fut function called from JavaScript. During an extern call (fut is waiting for it: a callback, as Array.map
  // makes) it runs right away and returns its result; otherwise it runs at the program's next safepoint, and the
  // call returns a Promise of its result.
  let ffiDepth = 0, pollLater = false;
  const callbacks = [];
  function callFut(id, args) {
    const json = toFut(args);
    if (ffiDepth) return fromFut(cstr(withStr(json, (p) => ex.fut_apply(id, p))));
    return new Promise((res, rej) => (callbacks.push({ id, json, res, rej }), poke()));
  }
  async function runCallbacks() {
    for (const c of callbacks.splice(0)) {
      try {
        const sp = spG.value, txn = ex.fut_txn_depth(), eff = ex.fut_eff_enter();
        try { c.res(fromFut(cstr(await withStrAsync(c.json, (p) => applyP(c.id, p))))); }
        catch (e) { spG.value = sp, ex.fut_txn_unwind(txn); throw e; }
        finally { ex.fut_eff_leave(eff); }
      } catch (e) {
        if (e instanceof Restart || e instanceof Exit) throw e;
        report(e), c.rej(new Error(errorText(e).split("\n")[0]));
      }
    }
  }
  async function withStrAsync(s, f) {
    const b = enc.encode(s), p = ex.fut_alloc(b.length + 1);
    u8().set(b, p), (u8()[p + b.length] = 0);
    try { return await f(p); } finally { ex.fut_free(p); }
  }
  // An extern call. A JS function that returns a Promise suspends the fut code calling it (that fiber, or the
  // program) until it settles, as sleep() does. A JS exception becomes a fut error (try catches it).
  function ffi(fn, sigP, args, ret, failedP) {
    const sig = cstr(sigP), d = dv(), a = [], big = externWasm[fn];
    for (let i = 0; i < sig.length; i++) {
      const x = d.getFloat64(args + i * 8, true), t = sig[i];
      a.push(t == "s" ? cstr(x) : t == "a" || t == "f" ? fromFut(cstr(x)) : t == "i" ? x | 0 : t == "l" ? (big ? BigInt(Math.trunc(x)) : Math.trunc(x)) : x);
    }
    const done = (r) => {
      if (ret == 115 /* s */) return r == null ? -1 : ((held = enc.encode(String(r))), held.length);
      if (ret == 97 /* a */) return (held = enc.encode(toFut(r))), held.length;
      return ret == 118 /* v */ ? 0 : Number(r);
    };
    const failed = (e) => {
      if (e instanceof WebAssembly.Exception || e instanceof Restart || e instanceof Exit) throw e;  // a fut error, from a callback
      dv().setInt32(failedP, 1, true);
      return (held = enc.encode(String(e?.message ?? e))), held.length;
    };
    let r;
    ffiDepth++;
    try { r = externs[fn](...a); } catch (e) { return failed(e); } finally { if (!--ffiDepth && pollLater) (pollLater = false), poke(); }
    if (r && typeof r.then == "function") return Promise.resolve(r).then(done).catch(failed);
    try { return done(r); } catch (e) { return failed(e); }
  }

  // ---- fibers: each runs in its own promising call; resume and yield hand control back and forth ----
  const fibers = new Map(), fiberTraces = new Map();  // trace text by error value, until reported
  let fiberStart;
  function fiberFailed(f, e) {
    if (e instanceof Restart) return ex.fut_fiber_fail(f, 2, 0n);
    if (e instanceof WebAssembly.Exception && e.is(tag)) {
      const v = e.getArg(tag, 0);
      return fiberTraces.set(v, (fiberTraces.get(v) ?? "") + trace(e)), ex.fut_fiber_fail(f, 1, v);
    }
    withStr(describe(e), (p) => ex.fut_fiber_fail(f, 3, BigInt(p)));
  }
  const resume = (f, start) => new Promise((res) => {
    let fb = fibers.get(f);
    if (!fb) fibers.set(f, (fb = {}));
    fb.back = res;
    if (start) fiberStart(f).then(() => fb.back(), (e) => (fiberFailed(f, e), fb.back()));
    else { const c = fb.cont; fb.cont = null, c(); }
  });
  const yieldTo = (f) => new Promise((res) => { const fb = fibers.get(f); fb.cont = res, fb.back(); });

  // ---- stack traces: V8 names our frames name@slot and gives the module offset; fut_where maps it to a line ----
  function trace(e) {
    const fr = [];
    for (const l of String(e.stack ?? "").split("\n")) {
      const m = /@(\d+) \(.*:0x([0-9a-f]+)\)\s*$/.exec(l);
      const d = m && cstr(ex.fut_where(+m[1], parseInt(m[2], 16)));
      if (d) fr.push("  at " + d + "\n");
    }
    if (fr.length > 16) fr.splice(10, fr.length - 15, "  ... more frames\n");
    return fr.join("");
  }
  function describe(e) {
    if (e instanceof RangeError && /call stack/i.test(e.message)) return "stack overflow [stack-overflow]";  // V8's own: the code errors.cpp gives rt.cpp's
    return "internal error: " + (e?.message ?? e);
  }
  function errorText(e) {  // the message, a newline, the trace
    let msg, t = "";
    if (e instanceof WebAssembly.Exception && e.is(tag)) {
      const v = e.getArg(tag, 0);
      (msg = cstr(ex.fut_error_line(v))), (t = (fiberTraces.get(v) ?? "") + trace(e));
    } else (msg = describe(e)), opts.verbose && !(e instanceof RangeError) && (t = String(e.stack) + "\n");
    return fiberTraces.clear(), msg + "\n" + t;
  }
  const report = (e) => write(2, "runtime error: " + errorText(e));

  // ---- the program's events: changed files and typed lines, handled at the next safepoint ----
  let fileChanged = false, inbox = [], replBuf = "", inMain = false, wake = null, path = "", mainChanged = false;
  const poke = () => { if (ex) dv().setUint32(pollAt, dv().getUint32(pollAt, true) | 1, true); wake?.(), (wake = null); };
  let pollAt = 0;
  async function call(slot) {  // run code; on an error the shadow stack pointer is put back, and transactions opened since roll back
    const sp = spG.value, txn = ex.fut_txn_depth(), eff = ex.fut_eff_enter();  // unchecked, no handlers (effects.cpp)
    try { return await callP(slot); } catch (e) { spG.value = sp, ex.fut_txn_unwind(txn); throw e; } finally { ex.fut_eff_leave(eff); }
  }
  async function evalRepl(more) {  // compile and run replBuf, unless it is unfinished and `more` may follow
    const s = await compileWith(() => withStr(replBuf, (p) => withStr("<repl>", (n) => ex.fut_compile(n, 0, 1, p))));
    if (s == -1 && more) return;
    if (s == -1) write(2, cstr(ex.fut_info(2)) + "\n");
    if (s > 0) {
      try { await call(s); } catch (e) { if (e instanceof Restart || e instanceof Exit) throw e; report(e); }
      ex.fut_retire(s);
    }
    replBuf = "";
  }
  async function evalInbox() {
    for (const l of inbox.splice(0)) (replBuf += l), await evalRepl(l.length > 1);  // an empty line gives up on unfinished input
  }
  async function load(reload) {
    const t0 = performance.now(), s = await compileWith(() => withStr(path, (p) => ex.fut_compile(p, reload, 0, 0)));
    if (!s) return 0;
    mainChanged = !!ex.fut_main_changed();
    opts.files?.(cstr(ex.fut_info(1)).split("\n").filter(Boolean));
    if (reload) write(2, `[fut] ${cstr(ex.fut_info(0))} (${(performance.now() - t0).toFixed(2)} ms, ${ex.fut_live() >> 10} KB code)\n`);
    return s;
  }
  async function hostPoll() {  // at a safepoint (not inside atomic { }): typed lines, callbacks, then a reload if a file changed
    await evalInbox();
    await runCallbacks();
    if (!fileChanged) return;
    fileChanged = false;
    const c = await load(true);
    if (!c) return;  // compile error: keep running the old code
    const restart = mainChanged && inMain;
    // The new top level and the reload() hook (which migrates state) run as one transaction. If either fails, the
    // reload is undone: the data they changed, then the code. The program goes on as if the file had not changed.
    ex.fut_txn_begin();
    try {
      await call(c);
      const r = withStr("reload", (p) => ex.fut_fn(p));
      if (r) await call(r);
    } catch (e) {
      if (e instanceof Restart || e instanceof Exit) throw e;
      return report(e), ex.fut_retire(c), ex.fut_reload_failed();
    }
    ex.fut_retire(c), ex.fut_txn_commit();
    if (restart) throw new Restart();
  }
  async function runCode(slot) {  // 0 ok, 1 runtime error, 2 restart requested
    try { return await call(slot), 0; } catch (e) {
      if (e instanceof Restart) return 2;
      if (e instanceof Exit) throw e;
      return report(e), 1;
    }
  }
  async function start(c) {  // the top level, then main() (rerun when a reload changes it)
    let st = await runCode(c);
    ex.fut_retire(c), (inMain = true);
    for (let m; !st && (m = withStr("main", (p) => ex.fut_fn(p))) && (st = await runCode(m)) == 2; st = 0)
      write(2, "[fut] main() changed: restarting it\n");
    return (inMain = false), st;
  }
  const event = () => new Promise((r) => (wake = r));
  async function waitChange() {  // typed lines still run meanwhile, so the stopped program's state can be inspected
    for (;;) {
      await event(), await evalInbox();
      if (fileChanged) return void (fileChanged = false);
    }
  }

  async function httpFetch(f, q) {  // -> {status, body, headers}, or {status: 0, error} when nothing came back
    try {
      const init = { method: q.method ?? "GET", headers: q.headers ?? {}, redirect: "follow", signal: AbortSignal.timeout((q.timeout ?? 60) * 1000) };
      if (q.body != null) init.body = q.body;
      const r = await f(q.url, init), body = init.method == "HEAD" ? "" : await r.text(), headers = {};
      r.headers.forEach((v, k) => (headers[k] = v));
      return { status: r.status, body, headers };
    } catch (e) { return { status: 0, error: String(e?.message ?? e) }; }
  }

  // ---- instantiate ----
  const S = (f) => new WebAssembly.Suspending(f);
  const env = {};
  const host = {
    install(mod, len, ops, nops) {
      if (mod) {
        const bytes = u8().slice(mod, mod + len);
        opts.dump?.(bytes);
        const inst = new WebAssembly.Instance(opts.module ? opts.module(bytes) : new WebAssembly.Module(bytes), { env });
        for (const [k, f] of Object.entries(inst.exports)) table.set(+k, f);
      }
      const o = new Int32Array(mem.buffer, ops, nops * 2).slice();
      for (let i = 0; i < o.length; i += 2) table.set(o[i], o[i + 1] < 0 ? null : table.get(o[i + 1]));
    },
    grow: (n, fill) => table.grow(n, table.get(fill)),
    throw(v) { throw new WebAssembly.Exception(tag, [v], { traceStack: true }); },
    restart() { throw new Restart(); },
    read(p) {
      const b = opts.read(cstr(p));
      return b ? ((held = b), b.length) : -1;
    },
    take(dst) { u8().set(held, dst); },
    save: (p, t) => (opts.save?.(cstr(p), t ? cstr(t) : null) ? 1 : 0),
    extern: (lib, name) => externLookup(cstr(lib), cstr(name)),
    ffi: S(ffi),
    ref_gone(id) { refIds.delete(refs.get(id)), refs.delete(id); },
    resume: S(resume), yield: S(yieldTo), fiber_gone: (f) => fibers.delete(f),
    sleep: S((s) => new Promise((r) => setTimeout(r, Math.max(0, s * 1000)))),
    poll: S(() => (ffiDepth ? void (pollLater = true) : hostPoll())),  // not inside a callback: after the extern call
    run: S(async (argsP, srcP, timeout) => {  // run(): fut in a child process, if the host can (opts.run)
      const args = cstr(argsP).split("\n"), file = args.pop();
      const r = await opts.run?.(args, file, srcP ? cstr(srcP) : null, timeout);
      if (!r) return -1;
      const out = enc.encode(r.out), err = enc.encode(r.err), head = enc.encode(`${r.code} ${r.timedOut ? 1 : 0} ${out.length}\n`);
      held = new Uint8Array(head.length + out.length + err.length);
      return held.set(head), held.set(out, head.length), held.set(err, head.length + out.length), held.length;
    }),
    fetch: S(async (p) => {  // fetch(), fetchAll(): the requests (checked in wasm.cpp) at once; results in order, as sys.cpp's
      const f = opts.fetch ?? globalThis.fetch;
      if (!f) return -1;
      const reqs = JSON.parse(cstr(p)).map((q) => (typeof q == "string" ? { url: q } : q));
      held = enc.encode(JSON.stringify(await Promise.all(reqs.map((q) => httpFetch(f, q)))));
      return held.length;
    }),
    window: (w, h) => (opts.window?.(w, h) ? 1 : 0),
    present: S(async (px, w, h) => {
      const src = new Uint8Array(mem.buffer, px, w * h * 4), rgba = new Uint8ClampedArray(w * h * 4);
      for (let i = 0; i < src.length; i += 4) (rgba[i] = src[i + 2]), (rgba[i + 1] = src[i + 1]), (rgba[i + 2] = src[i]), (rgba[i + 3] = 255);
      return (await opts.present(rgba, w, h)) ? 1 : 0;
    }),
  };
  const wasmMod = opts.wasm instanceof WebAssembly.Module ? opts.wasm : await WebAssembly.compile(opts.wasm);
  const inst = await WebAssembly.instantiate(wasmMod, { host, wasi_snapshot_preview1: wasi });
  ex = inst.exports, (mem = ex.memory), (table = ex.__indirect_function_table), (spG = ex.__stack_pointer);
  Object.assign(env, { memory: mem, table, sp: spG, tag });
  const callP = WebAssembly.promising(ex.fut_call), applyP = WebAssembly.promising(ex.fut_apply);
  fiberStart = WebAssembly.promising(ex.fut_fiber_start);
  ex._initialize(), ex.fut_init((opts.verbose ? 1 : 0) | (opts.noTier ? 2 : 0)), (pollAt = ex.fut_poll());
  if (opts.std) withStr(opts.std, (p) => ex.fut_std(p));  // where the standard library's packages are

  const pinned = new Map();  // name -> id, for apply()
  let applying = Promise.resolve();  // one apply at a time: they share the main shadow stack

  return {
    exports: ex,
    async compile(file, tests) {  // compile only (errors printed), as test() would if tests: did it compile?
      if (!tests) return (path = file), !!(await load(false));
      withStr("", (p) => ex.fut_tests_begin(1, p));
      return !!(await compileWith(() => withStr((path = file), (p) => ex.fut_compile(p, 0, 0, 0, 1))));
    },
    async start(file) {  // compile, then run the top level (not main()): 0 ok, 1 runtime error, 2 compile error
      path = file;
      const c = await load(false);
      if (!c) return 2;
      const st = await runCode(c);
      return ex.fut_retire(c), st;
    },
    checkApi: () => withStr(path, (p) => ex.fut_api(1, p)),  // after compile(file, true): report out-of-date interfaces (JSON); how many
    has: (name) => withStr(name, (p) => ex.fut_pin_fn(p)) >= 0,  // a top-level function name?
    tests: () => Array.from({ length: ex.fut_test_count() }, (_, i) => cstr(ex.fut_test_name(i))),  // after compile(file, true)
    // Call the program's top-level function name with args (JSON data): its result. Calls queue, one at a time (each
    // may wait on fetch() meanwhile). A fut error rejects with a FutError (its text: the message and trace).
    apply(name, args) {
      const go = async () => {
        let id = pinned.get(name);
        if (id == null) {
          if ((id = withStr(name, (p) => ex.fut_pin_fn(p))) < 0) throw new Error(`the program has no function ${name}()`);
          pinned.set(name, id);
        }
        const sp = spG.value, txn = ex.fut_txn_depth(), eff = ex.fut_eff_enter();
        try { return fromFut(cstr(await withStrAsync(toFut(args), (p) => applyP(id, p)))); }
        catch (e) {
          spG.value = sp, ex.fut_txn_unwind(txn);
          throw e instanceof Restart || e instanceof Exit ? e : new FutError(errorText(e));
        } finally { ex.fut_eff_leave(eff); }
      };
      const r = applying.then(go, go);
      return (applying = r.catch(() => {})), r;
    },
    changed() { (fileChanged = true), poke(); },   // a watched file changed
    type(line) { inbox.push(line), poke(); },      // a line typed into the running program
    async run(file, watch) {  // exit status
      path = file;
      try {
        let c = await load(false), loaded = !!c, st = loaded ? await start(c) : 1;
        while (st && watch) {  // after an error: wait for a fix, patch it in, rerun main()
          write(2, "[fut] waiting for changes...\n"), await waitChange();
          if ((c = await load(loaded))) (loaded = true), (st = await start(c));
        }
        return st ? 1 : 0;
      } catch (e) { if (e instanceof Exit) return e.code; throw e; }
    },
    async test(file, json, filter) {  // fut test: the top level, then each test block; exit status
      path = file;
      const errorOf = async (slot) => {  // null, or the uncaught error's message and trace
        try { return await call(slot), null; } catch (e) {
          if (e instanceof Restart || e instanceof Exit) throw e;
          return errorText(e);
        }
      };
      try {
        // The filter is set after the compile: nothing before it may depend on the filter, so the code a test file
        // compiles to doesn't either (a Cloudflare Worker runs it as compiled ahead of time, whatever the filter).
        withStr("", (p) => ex.fut_tests_begin(json ? 1 : 0, p));
        const c = await compileWith(() => withStr(path, (p) => ex.fut_compile(p, 0, 0, 0, 1)));
        if (!c) return 2;
        if (filter) withStr(filter, (p) => ex.fut_tests_begin(json ? 1 : 0, p));
        const top = await errorOf(c);
        ex.fut_retire(c);
        // a public interface out of date fails the run too (reported last, by the status)
        const api = (st) => (withStr(path, (p) => ex.fut_api(json ? 1 : 0, p)) ? Math.max(st, 1) : st);
        if (top != null) return withStr(top, (p) => ex.fut_test_top_error(p)), api(1);
        for (let i = 0, n = ex.fut_test_count(); i < n; i++) {
          const s = ex.fut_test_start(i);
          if (!s) continue;
          const err = await errorOf(s);
          withStr(err ?? "", (p) => ex.fut_test_finish(i, err != null ? 1 : 0, p));
        }
        return api(ex.fut_tests_end());
      } catch (e) { if (e instanceof Exit) return e.code; throw e; }
    },
    async api(file, check) {  // fut api [--check]: write (or just report) each interface; exit status
      path = file;
      if (!(await compileWith(() => withStr(path, (p) => ex.fut_compile(p, 0, 0, 0, 0))))) return 2;
      const n = withStr(path, (p) => ex.fut_api(check ? 0 : 2, p));
      return (check && n) || n < 0 ? 1 : 0;  // n < 0: a breaking change held back until the version moves
    },
    async map(file, pub, json, only) {  // fut map: the packages, files and declarations the program uses; exit status
      path = file;
      if (!(await compileWith(() => withStr(path, (p) => ex.fut_compile(p, 0, 0, 0, 0))))) return 2;
      const n = withStr(only ?? "", (o) => withStr(path, (p) => ex.fut_map(pub ? 1 : 0, json ? 1 : 0, o, p)));
      if (!n && only) write(2, `nothing matches '${only}'\n`);
      return !n && only ? 1 : 0;
    },
    explain(code) { return withStr(code ?? "", (c) => ex.fut_explain(c)); },  // fut explain [code]: an error code's rule and fix
    async line(l) {  // REPL without a program: returns the next prompt
      (replBuf += l), await evalRepl(l.length > 1);
      return replBuf ? ". " : "> ";
    },
  };
}
