// fut's WebAssembly host, shared by Node (web/run.mjs) and the browser (web/index.html). It instantiates build/fut.wasm
// (the compiler and runtime), installs the modules that compiler emits into the shared function table, suspends and
// resumes fiber stacks with JSPI, and drives the program: run it, reload changed files at safepoints, rerun main().
//
// createFut(opts) options:
//   wasm: bytes of fut.wasm           read(path): file bytes or null      write(fd, bytes): stdout (1) / stderr (2)
//   verbose: bool                     getenv(name): string or undefined   window(w, h): bool, present(rgba, w, h): Promise
//   run(args, file, source, timeout): {code, out, err, timedOut} -- fut in a child process (run()), if possible
//   save(path, text): bool -- write a file (text null: remove it), for `fut api`
export class Restart extends Error {}  // unwinds to the driver, which reruns main()
export class Exit extends Error { constructor(code) { super("exit " + code), (this.code = code); } }

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
  const externs = [];
  function externLookup(lib, name) {
    let f;
    if (!lib) f = libc[name] ?? Math[name] ?? globalThis[name];
    else {
      let o = globalThis;
      for (const k of lib.split(".")) if ((o = o?.[k]) == null) return -2;
      f = o[name]?.bind?.(o);
    }
    return typeof f == "function" ? externs.push(f) - 1 : -1;
  }
  function ffi(fn, sigP, args, ret) {
    const sig = cstr(sigP), d = dv(), a = [];
    for (let i = 0; i < sig.length; i++) {
      const x = d.getFloat64(args + i * 8, true), t = sig[i];
      a.push(t == "s" ? cstr(x) : t == "i" ? x | 0 : t == "l" ? Math.trunc(x) : x);
    }
    const r = externs[fn](...a);
    if (ret == 115 /* s */) return r == null ? -1 : ((held = enc.encode(String(r))), held.length);
    return ret == 118 /* v */ ? 0 : Number(r);
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
    if (e instanceof RangeError && /call stack/i.test(e.message)) return "stack overflow";
    return "internal error: " + (e?.message ?? e);
  }
  function errorText(e) {  // the message, a newline, the trace
    let msg, t = "";
    if (e instanceof WebAssembly.Exception && e.is(tag)) {
      const v = e.getArg(tag, 0);
      (msg = str(v)), (t = (fiberTraces.get(v) ?? "") + trace(e));
    } else (msg = describe(e)), opts.verbose && !(e instanceof RangeError) && (t = String(e.stack) + "\n");
    return fiberTraces.clear(), msg + "\n" + t;
  }
  const report = (e) => write(2, "runtime error: " + errorText(e));

  // ---- the program's events: changed files and typed lines, handled at the next safepoint ----
  let fileChanged = false, inbox = [], replBuf = "", inMain = false, wake = null, path = "", mainChanged = false;
  const poke = () => { if (ex) dv().setUint32(pollAt, dv().getUint32(pollAt, true) | 1, true); wake?.(), (wake = null); };
  let pollAt = 0;
  async function call(slot) {  // run code; on an error the shadow stack pointer is put back, and transactions opened since roll back
    const sp = spG.value, txn = ex.fut_txn_depth();
    try { return await callP(slot); } catch (e) { spG.value = sp, ex.fut_txn_unwind(txn); throw e; }
  }
  async function evalRepl(more) {  // compile and run replBuf, unless it is unfinished and `more` may follow
    const s = withStr(replBuf, (p) => withStr("<repl>", (n) => ex.fut_compile(n, 0, 1, p)));
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
  function load(reload) {
    const t0 = performance.now(), s = withStr(path, (p) => ex.fut_compile(p, reload, 0, 0));
    if (!s) return 0;
    mainChanged = !!ex.fut_main_changed();
    opts.files?.(cstr(ex.fut_info(1)).split("\n").filter(Boolean));
    if (reload) write(2, `[fut] ${cstr(ex.fut_info(0))} (${(performance.now() - t0).toFixed(2)} ms, ${ex.fut_live() >> 10} KB code)\n`);
    return s;
  }
  async function hostPoll() {  // at a safepoint (not inside atomic { }): typed lines, then a reload if a file changed
    await evalInbox();
    if (!fileChanged) return;
    fileChanged = false;
    const c = load(true);
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

  // ---- instantiate ----
  const S = (f) => new WebAssembly.Suspending(f);
  const env = {};
  const host = {
    install(mod, len, ops, nops) {
      if (mod) {
        const bytes = u8().slice(mod, mod + len);
        opts.dump?.(bytes);
        const inst = new WebAssembly.Instance(new WebAssembly.Module(bytes), { env });
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
    ffi,
    resume: S(resume), yield: S(yieldTo), fiber_gone: (f) => fibers.delete(f),
    sleep: S((s) => new Promise((r) => setTimeout(r, Math.max(0, s * 1000)))),
    poll: S(hostPoll),
    run(argsP, srcP, timeout) {  // run(): fut in a child process, if the host can (opts.run)
      const args = cstr(argsP).split("\n"), file = args.pop();
      const r = opts.run?.(args, file, srcP ? cstr(srcP) : null, timeout);
      if (!r) return -1;
      const out = enc.encode(r.out), err = enc.encode(r.err), head = enc.encode(`${r.code} ${r.timedOut ? 1 : 0} ${out.length}\n`);
      held = new Uint8Array(head.length + out.length + err.length);
      return held.set(head), held.set(out, head.length), held.set(err, head.length + out.length), held.length;
    },
    window: (w, h) => (opts.window?.(w, h) ? 1 : 0),
    present: S(async (px, w, h) => {
      const src = new Uint8Array(mem.buffer, px, w * h * 4), rgba = new Uint8ClampedArray(w * h * 4);
      for (let i = 0; i < src.length; i += 4) (rgba[i] = src[i + 2]), (rgba[i + 1] = src[i + 1]), (rgba[i + 2] = src[i]), (rgba[i + 3] = 255);
      return (await opts.present(rgba, w, h)) ? 1 : 0;
    }),
  };
  const inst = await WebAssembly.instantiate(await WebAssembly.compile(opts.wasm), { host, wasi_snapshot_preview1: wasi });
  ex = inst.exports, (mem = ex.memory), (table = ex.__indirect_function_table), (spG = ex.__stack_pointer);
  Object.assign(env, { memory: mem, table, sp: spG, tag });
  const callP = WebAssembly.promising(ex.fut_call);
  fiberStart = WebAssembly.promising(ex.fut_fiber_start);
  ex._initialize(), ex.fut_init(opts.verbose ? 1 : 0), (pollAt = ex.fut_poll());

  return {
    exports: ex,
    changed() { (fileChanged = true), poke(); },   // a watched file changed
    type(line) { inbox.push(line), poke(); },      // a line typed into the running program
    async run(file, watch) {  // exit status
      path = file;
      try {
        let c = load(false), loaded = !!c, st = loaded ? await start(c) : 1;
        while (st && watch) {  // after an error: wait for a fix, patch it in, rerun main()
          write(2, "[fut] waiting for changes...\n"), await waitChange();
          if ((c = load(loaded))) (loaded = true), (st = await start(c));
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
        withStr(filter ?? "", (p) => ex.fut_tests_begin(json ? 1 : 0, p));
        const c = withStr(path, (p) => ex.fut_compile(p, 0, 0, 0, 1));
        if (!c) return 2;
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
    api(file, check) {  // fut api [--check]: write (or just report) each compiled file's .api; exit status
      path = file;
      if (!withStr(path, (p) => ex.fut_compile(p, 0, 0, 0, 0))) return 2;
      const n = withStr(path, (p) => ex.fut_api(check ? 0 : 2, p));
      return check && n ? 1 : 0;
    },
    async line(l) {  // REPL without a program: returns the next prompt
      (replBuf += l), await evalRepl(l.length > 1);
      return replBuf ? ". " : "> ";
    },
  };
}
