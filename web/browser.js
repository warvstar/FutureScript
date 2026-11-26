// fut-mt.wasm (the threads build) in a browser page: every thread of fut, the program's root too, is a worker
// (web/fut-worker.js); the page's thread can't wait (Atomics.wait), and theirs do. The page starts the threads they
// ask for (a worker starts only when the thread that made it is free, and theirs may be waiting), shows their output
// and canvas, and runs the JavaScript they proxy to it (opts.proxy in fut.js): calls come in on each worker's channel
// while the worker waits; a fut function the page's JavaScript calls during one runs on that worker at once (the
// page spins until it returns), and one it calls later (an event's handler) is queued on the root, which runs it at
// its next safepoint. Needs a cross-origin isolated page (SharedArrayBuffer: web/serve.mjs sends the headers) and
// JSPI.
//
// startFut(o): o = {wasm (fut-mt.wasm's bytes), std, verbose, serve, proxy, poolSize, seed, write(fd, bytes),
// files(paths), window(w, h), present(rgba, w, h): Promise} -> {run(file, watch), line(l), type(l), changed(path),
// key(code, down), mouse(x, y, down)}, each sent to the root.
import { CH, FUT, chanGet, chanPut, jsonCodec } from "./fut.js";

const FRESH = 2 ** 31 + 2 ** 30;  // the root's own ids for the page's objects (fresh calls); the page's are below

export async function startFut(o) {
  const cfg = { std: o.std, verbose: o.verbose, serve: o.serve, proxy: o.proxy, poolSize: o.poolSize, seed: o.seed };
  const url = new URL("./fut-worker.js", import.meta.url);
  const threads = new Map();  // worker -> {ch, depth (calls of its being run)}
  const waiting = new Map();  // rid -> {res, rej}
  const pending = new Int32Array(new SharedArrayBuffer(4));
  let rids = 0, root, memory = null, pollAt = 0, gotReady;
  const readyP = new Promise((r) => (gotReady = r));

  // ---- the page's JavaScript values fut holds (ids from 2^31), and which threads hold each ----
  const refs = new Map(), refIds = new Map(), holders = new Map();
  let refNext = 2 ** 31;
  const unpin = new FinalizationRegistry((id) => root.postMessage({ unpin: id }));
  const codecs = new Map();  // worker -> its codec (fut functions it gave the page run there)
  const codec = (w) => {
    let c = codecs.get(w);
    if (!c) {
      const fnOf = (id) => { const f = (...args) => callFut(w, id, args); return (f[FUT] = id), unpin.register(f, id), f; };
      const refOf = (v) => {
        let id = refIds.get(v);
        if (id == null) refIds.set(v, (id = refNext++)), refs.set(id, v), holders.set(id, new Set());
        return holders.get(id).add(w), id;
      };
      codecs.set(w, (c = jsonCodec(fnOf, refOf, (id) => refs.get(id))));
    }
    return c;
  };
  const gone = new Set();  // fresh ids the worker dropped before its queued call reached the page
  function refGone(w, id) {
    if (id >= FRESH && !holders.has(id)) return void gone.add(id);
    const h = holders.get(id);
    if (h && (h.delete(w), !h.size)) holders.delete(id), refIds.delete(refs.get(id)), refs.delete(id);
  }

  // ---- calls from a worker: run here while it waits ----
  const modules = new Map();  // lib -> the module, as the page imported it
  function globalFn(lib, name) {
    let o = globalThis;
    if (lib) for (const k of lib.split(".")) if ((o = o?.[k]) == null) return null;
    const f = o?.[name];
    return typeof f == "function" ? (lib ? f.bind(o) : f) : null;
  }
  function run(w, q) {  // a value, or a Promise of one
    if (q.before) {  // the calls the worker queued (see flush in fut.js), in order; the first failure fails this call
      let failed = null;
      for (const c of q.before) {
        try {
          const r = runCall(w, c);
          if (c.id != null) {  // a fresh object, under the id the worker gave it
            if (gone.delete(c.id)) continue;  // (already dropped there)
            refs.set(c.id, r), refIds.set(r, c.id), holders.set(c.id, new Set([w]));
          }
        } catch (e) { failed ??= `${c.name}(): ${e?.message ?? e}`; }
      }
      if (failed) throw new Error(failed);
    }
    if (q.op == "flush") return 0;
    if (q.op == "load") {
      const file = q.lib.slice(q.lib.indexOf(":") + 1);
      return import(file + "?v=" + (q.stamp ?? 0)).then((m) => {
        modules.set(q.lib, m);
        const names = Object.keys(m).filter((k) => typeof m[k] == "function");
        return { names, fresh: names.filter((k) => m[k].fresh) };
      });
    }
    if (q.op == "has") return !!globalFn(q.lib, q.name);
    return runCall(w, q);
  }
  function runCall(w, q) {
    if (q.id != null) {  // fresh: the object itself (registered by run)
      const f = modules.get(q.lib)?.[q.name], c = codec(w);
      return f(...q.args.map((x, i) => (q.sig[i] == "a" || q.sig[i] == "f" ? c.fromFut(x) : x)));
    }
    const f = q.lib.includes(":") ? modules.get(q.lib)?.[q.name] : globalFn(q.lib, q.name);
    if (typeof f != "function") throw new Error(`${q.name} is not a function on the page`);
    const c = codec(w), a = q.args.map((x, i) => (q.sig[i] == "a" || q.sig[i] == "f" ? c.fromFut(x) : x));
    const done = (r) => (q.ret == 115 /* s */ ? (r == null ? null : String(r)) : q.ret == 97 /* a */ ? c.toFut(r) : q.ret == 118 ? 0 : Number(r));
    const r = f(...a);
    return r && typeof r.then == "function" ? Promise.resolve(r).then(done) : done(r);
  }
  async function serve(w) {  // the worker rang: its call, if it has one waiting
    const t = threads.get(w);
    if (!t || Atomics.compareExchange(t.ch.ctl, 0, CH.REQ, CH.TAKEN) != CH.REQ) return;
    const q = chanGet(t.ch);
    t.depth++;
    let r;
    try { r = { value: await run(w, q) }; } catch (e) { r = { err: String(e?.message ?? e) }; } finally { t.depth--; }
    chanPut(t.ch, CH.REPLY, r);
  }
  function serveNow(w, t) {  // a call made while the page waits on this worker: it can't wait for a Promise
    Atomics.store(t.ch.ctl, 0, CH.TAKEN);
    const q = chanGet(t.ch);
    let r;
    t.depth++;
    try {
      const v = run(w, q);
      r = v && typeof v.then == "function" ? { err: "it returned a Promise, which a call made inside a fut function the page called synchronously can't wait for" } : { value: v };
    } catch (e) { r = { err: String(e?.message ?? e) }; } finally { t.depth--; }
    chanPut(t.ch, CH.REPLY, r);
  }
  // A fut function the page's JavaScript calls. During a call of that worker's: there, now (the page spins until it
  // returns). Otherwise, the root's: queued there, a Promise of its result.
  function callFut(w, id, args) {
    const t = threads.get(w), c = codec(w);
    if (t.depth > 0) {
      chanPut(t.ch, CH.CB, { id, json: c.toFut(args) });
      for (let s; (s = Atomics.load(t.ch.ctl, 0)) != CH.CBREPLY;) if (s == CH.REQ) serveNow(w, t);
      const m = chanGet(t.ch);
      Atomics.store(t.ch.ctl, 0, CH.TAKEN);
      if (m.err != null) throw new Error(m.err);
      return c.fromFut(m.value);
    }
    if (w !== root) return Promise.reject(new Error("JavaScript can call a fut function later (not during an extern call) only if the program's main isolate gave it"));
    return ask({ call: { id, json: c.toFut(args) } }, true).then((v) => c.fromFut(v));
  }
  // A message to the root; counted, it makes the root look at its messages at its next safepoint, even while busy.
  function ask(m, counted) {
    const rid = ++rids;
    if (counted) poke(m);
    return new Promise((res, rej) => (waiting.set(rid, { res, rej }), root.postMessage({ ...m, rid, counted })));
  }
  function poke() {
    Atomics.add(pending, 0, 1);
    if (memory) Atomics.or(new Uint32Array(memory.buffer), pollAt >> 2, 1);
  }
  const tell = (m) => (poke(), root.postMessage({ ...m, counted: true }));

  // ---- workers ----
  function start(init) {
    const w = new Worker(url, { type: "module", name: init.thread ? `fut thread ${init.thread.tid}` : "fut" });
    threads.set(w, { ch: null, depth: 0 });
    w.onmessage = ({ data: m }) => {
      if (m.hello) threads.get(w).ch = m.hello;
      else if (m.bell) serve(w);
      else if (m.write) o.write(...m.write);
      else if (m.spawn) start({ cfg, thread: m.spawn });
      else if (m.files) o.files?.(m.files);
      else if (m.window) o.window?.(...m.window);
      else if (m.present) Promise.resolve(o.present?.(...m.present)).then(() => w.postMessage({ presented: m.rid }));
      else if (m.refGone) refGone(w, m.refGone);
      else if (m.ready) (memory = m.ready.memory), (pollAt = m.ready.pollAt), gotReady();
      else if (m.exit != null) exit(m.exit);
      else if (m.done) {
        const p = waiting.get(m.done);
        waiting.delete(m.done), m.err != null ? p?.rej(new Error(m.err)) : p?.res(m.value);
      }
    };
    w.onerror = (e) => o.write(2, new TextEncoder().encode(`[fut] worker failed: ${e.message}\n`));
    w.postMessage({ init });
    return w;
  }
  let exited;
  const exitP = new Promise((r) => (exited = r));
  function exit(code) {  // the program called exit(), or a thread failed in the host: every thread ends
    for (const w of threads.keys()) w.terminate();
    exited(code);
  }
  root = start({ cfg, pending, wasm: o.wasm, freshBase: FRESH });
  await readyP;
  return {
    run: (file, watch) => Promise.race([ask({ run: [file, watch] }), exitP]),
    line: (l) => ask({ line: l }),
    type: (l) => tell({ type: l }),
    changed: (p) => tell({ changed: p }),
    key: (c, down) => root.postMessage({ key: [c, down] }),
    mouse: (x, y, down) => root.postMessage({ mouse: [x, y, down] }),
  };
}
