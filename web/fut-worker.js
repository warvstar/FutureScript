// A thread of fut-mt.wasm (the threads build) in a browser, on a worker the page started (web/browser.js): the
// root (the program: run, REPL lines, events) or one of the threads it asks for (a pool's, the ticker). Every
// thread is a worker, the root too: the page's own thread can't wait (Atomics.wait), and fut's threads do. What only
// the page has goes there as messages (output, the canvas, starting threads), or as calls it runs while this thread
// waits (opts.main: the JavaScript fut.pkg declares main, as std/ui's DOM functions are; see fut.js).
import { channel, createFut } from "./fut.js";

const ch = channel();
const presents = new Map(), stamps = new Map();
let fut = null, rids = 0, ready, pending = null;
const started = new Promise((r) => (ready = r));
postMessage({ hello: { ctl: ch.ctl, data: ch.data } });

function read(p) {  // fut reads files synchronously
  const x = new XMLHttpRequest();
  x.open("GET", p.replace(/^\/+/, "/"), false), x.overrideMimeType("text/plain; charset=x-user-defined");  // ("//x": another host)
  try { x.send(); } catch { return null; }
  if (x.status != 200) return null;
  const b = new Uint8Array(x.responseText.length);
  for (let i = 0; i < b.length; i++) b[i] = x.responseText.charCodeAt(i) & 0xff;
  return b;
}
function options(cfg, pending) {
  return {
    std: cfg.std, verbose: cfg.verbose, serve: cfg.serve, poolSize: cfg.poolSize, proxy: cfg.proxy, seed: cfg.seed,
    cores: navigator.hardwareConcurrency,
    read,
    write: (fd, b) => postMessage({ write: [fd, b.slice()] }),
    files: (f) => postMessage({ files: f }),
    importModule: (p, stamp) => import(p + "?v=" + stamp),
    stamp: (p) => stamps.get(p) ?? 0,
    window: (w, h) => (postMessage({ window: [w, h] }), true),
    present: (rgba, w, h) => new Promise((res) => {
      const rid = ++rids;
      presents.set(rid, res), postMessage({ present: [rgba, w, h], rid }, [rgba.buffer]);
    }),
    exit: (code) => postMessage({ exit: code }),
    threads: { start: (thread) => postMessage({ spawn: thread }) },  // the page starts it: this thread may be waiting
    main: { ...ch, bell: () => postMessage({ bell: 1 }), post: (m) => postMessage(m) },
    pending,
  };
}
const reply = (rid, p) => Promise.resolve().then(() => p).then((value) => postMessage({ done: rid, value }), (e) => postMessage({ done: rid, err: String(e?.message ?? e) }));

const failed = (e) => postMessage({ write: [2, new TextEncoder().encode(`[fut] ${e?.stack ?? e}\n`)] });
addEventListener("unhandledrejection", (e) => failed(e.reason));
onmessage = (e) => handle(e.data).catch(failed);
async function handle(m) {
  if (m.counted) Atomics.sub(pending, 0, 1);  // (a message the page counted: see opts.pending)
  if (m.init) {
    const { cfg, thread } = m.init;
    if (thread) return void (await createFut({ ...options(cfg, null), wasm: thread.module, thread }));
    pending = m.init.pending;
    fut = await createFut({ ...options(cfg, pending), wasm: m.init.wasm, freshBase: m.init.freshBase });
    return postMessage({ ready: { memory: fut.memory, pollAt: fut.exports.fut_poll() } }), ready();
  }
  if (m.presented) return presents.get(m.presented)?.(true), presents.delete(m.presented);
  await started;
  if (m.run) reply(m.rid, fut.run(...m.run));
  else if (m.line != null) reply(m.rid, fut.line(m.line));
  else if (m.type != null) fut.type(m.type);
  else if (m.changed) stamps.set(m.changed, (stamps.get(m.changed) ?? 0) + 1), fut.changed();
  else if (m.key) fut.exports.fut_key(...m.key);
  else if (m.mouse) fut.exports.fut_mouse(...m.mouse);
  else if (m.call) reply(m.rid, fut.callRaw(m.call.id, m.call.json));
  else if (m.unpin != null) fut.exports.fut_unpin(m.unpin);
}
