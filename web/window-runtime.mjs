// Browser host for a Fut window application, optionally connected to a session service.
export async function runWindowSite({version, main, client, follow, gpu = false, required = []}) {
  const at = p => new URL(p + "?v=" + encodeURIComponent(version), import.meta.url);
  const load = p => import(at(p));
  const {createFut} = await load("./fut.js");
  const canvas = document.getElementById("screen"), gpuCanvas = document.getElementById("gpu");
  const boot = document.getElementById("boot"), problem = document.getElementById("live");
  const ctx = canvas.getContext("2d"), touch = matchMedia("(pointer: coarse)").matches;
  const requirements = [];
  if (gpu && !navigator.gpu) requirements.push("WebGPU");
  if (typeof WebAssembly.Suspending !== "function") requirements.push("WebAssembly JSPI");
  if (requirements.length) {
    boot.textContent = "This browser needs " + requirements.join(" and ") + " to run this application.";
    return;
  }
  const options = {touch, query: Object.fromEntries(new URLSearchParams(location.search))};
  let config = {}, session, attached = false, closing = false;
  const enc = new TextEncoder();
  const Client = client ? (await load(client.module))[client.export] : null;
  let status = "";
  function changedStatus(text, retry) { status = JSON.stringify({text, retry}); }
  function start() {
    if (Client && !session) session = new Client(changedStatus);
    if (touch && config.fullscreen) document.documentElement.requestFullscreen?.({navigationUI:"hide"})?.catch(() => {});
  }
  function stop() { closing = true; session?.stop(); session = null; }
  function readStorage(path) {
    try { return localStorage.getItem("fut:" + path); } catch { return null; }
  }
  function get(path) {
    if (path === "@window/info") return JSON.stringify(options);
    if (path === "@window/status") return status;
    if (path === config.wheel?.file) {
      const steps = wheelDelta;
      wheelDelta = 0;
      fut.exports.fut_key(config.wheel.signal, 0);
      return String(steps);
    }
    const service = config.session;
    if (path === service?.attach) {
      if (attached) session?.reset();
      attached = true;
      return null;
    }
    if (path === service?.input) return session?.snapshot();
    if (config.storage?.includes(path) || config.readOnly?.includes(path)) return readStorage(path);
    if (Object.hasOwn(config.files ?? {}, path)) return config.files[path];
    return sources[path];
  }
  function put(path, text) {
    if (path === "@window/configure") { config = JSON.parse(text); return true; }
    if (path === "@window/action") {
      if (text === "start") start();
      else if (text === "retry") { session?.retry(); shown().focus(); }
      else if (text === "exit") { stop(); location.reload(); }
      else return false;
      return true;
    }
    if (path === config.session?.output && text != null) {
      if (closing) return true;
      if (!session) return false;
      session.submit(JSON.parse(text));
      return true;
    }
    if (!config.storage?.includes(path) || text == null) return false;
    try { localStorage.setItem("fut:" + path, text); return true; } catch { return false; }
  }
  let windowOpen = false, gpuPresent = null;
  const shown = () => (gpuPresent ? gpuCanvas : canvas);
  function fit() {  // the window's aspect, as large as the screen allows, centred
    const c = shown();
    if (!c.width) return;
    const W = document.documentElement.clientWidth, H = document.documentElement.clientHeight, s = Math.min(W / c.width, H / c.height);
    for (const x of [canvas, gpuCanvas]) {
      x.style.width = c.width * s + "px", x.style.height = c.height * s + "px";
      x.style.left = (W - c.width * s) / 2 + "px", x.style.top = (H - c.height * s) / 2 + "px";
    }
  }
  addEventListener("resize", fit);
  // Wake a window waiting for animation when an edited source arrives.
  let resumeFrame;
  const host = {
    std: "/std", verbose: false,
    write: (fd, b) => (fd == 2 ? console.error : console.log)(new TextDecoder().decode(b).replace(/\n$/, "")),
    window(w, h) {
      for (const c of [canvas, gpuCanvas]) c.width = w, c.height = h;
      windowOpen = true, (shown().style.display = "block"), fit();
      return true;
    },
    async present(rgba, w, h) {
      if (gpuPresent) gpuPresent();
      else ctx.putImageData(new ImageData(rgba, w, h), 0, 0);
      await new Promise(resolve => {
        const frame = requestAnimationFrame(resolve);
        resumeFrame = () => { cancelAnimationFrame(frame); resolve(); };
      });
      resumeFrame = null;
      return true;
    },
    screen: {  // std/gpu draws the window on a canvas of its own, in the pixel buffer's place
      take(present) {
        if (!windowOpen) return null;
        gpuPresent = present, (canvas.style.display = "none"), (gpuCanvas.style.display = "block"), fit();
        gpuCanvas.focus({preventScroll:true});
        return gpuCanvas;
      },
      give() { gpuPresent = null, (gpuCanvas.style.display = "none"), windowOpen && (canvas.style.display = "block"); },
    },
  };


  let wheelDelta = 0;
  function addWheelDelta(steps) {
    if (!config.wheel) return;
    wheelDelta = Math.max(-config.wheel.limit, Math.min(config.wheel.limit, wheelDelta + steps));
    fut.exports.fut_key(config.wheel.signal, 1);
  }

  const wasmURL = at("fut.wasm");
  const [files, wasm] = await Promise.all([
    fetch(at("files.json")).then(r => r.json()),
    WebAssembly.compileStreaming(fetch(wasmURL)).catch(async () => WebAssembly.compile(await (await fetch(wasmURL)).arrayBuffer())),
  ]);
  let sources = {...files};
  const fut = await createFut({
    ...host, wasm,
    problem(text) { problem.textContent = text || ""; problem.hidden = !text; },
    read(path) { const value = get(path); return value == null ? null : enc.encode(value); },
    save: put, importModule: p => load("." + p),
  });
  let running = false;
  if (follow) {
    const subscribe = (await load(follow.module))[follow.export];
    await subscribe(files, () => {
      if (!required.every(([path, line]) => files[path]?.split("\n").includes(line))) return;
      sources = {...files};
      if (running) { fut.changed(); if (!document.hidden) resumeFrame?.(); }
    });
  }
  window.fut = fut;
  const at2 = (e) => { const c = shown(), r = c.getBoundingClientRect(); return [(e.clientX - r.left) * c.width / r.width, (e.clientY - r.top) * c.height / r.height]; };
  function point(e, active) {
    if (e.pointerType === "touch") {
      fut.exports.fut_touch(e.pointerId, ...at2(e), Number(active));
    } else {
      fut.exports.fut_mouse(...at2(e), Number(active && (e.buttons & 1) !== 0));
    }
  }
  for (const c of [canvas, gpuCanvas]) {
    c.addEventListener("wheel", e => {
      if (!windowOpen || !config.wheel || !e.deltaY) return;
      const unit = e.deltaMode === 1 ? 16 : e.deltaMode === 2 ? c.clientHeight : 1;
      const steps = e.deltaY * unit * (e.ctrlKey ? config.wheel.pinch : config.wheel.scale);
      addWheelDelta(steps);
      e.preventDefault();
    }, { passive: false });
    c.addEventListener("pointerdown", (e) => {
      try { c.setPointerCapture(e.pointerId); } catch {}
      point(e, true);
      c.focus();
      e.preventDefault();
    });
    c.addEventListener("pointermove", (e) => { point(e,true); e.preventDefault(); });
    for (const t of ["pointerup", "pointercancel"]) c.addEventListener(t, (e) => point(e,false));
    c.addEventListener("contextmenu", (e) => e.preventDefault());
  }
  // screensize() reports the viewport in CSS pixels after each resize.
  const sized = () => fut.exports.fut_screen(document.documentElement.clientWidth, document.documentElement.clientHeight);
  addEventListener("resize", sized), sized();
  const named = { ArrowUp: 0xf700, ArrowDown: 0xf701, ArrowLeft: 0xf702, ArrowRight: 0xf703, Enter: 13, Backspace: 127, Escape: 27, Tab: 9, " ": 32 };
  const code = (e) => named[e.key] ?? (e.key.length == 1 ? e.key.toLowerCase().charCodeAt(0) : -1);
  const heldKeys = new Map();
  for (const [type, d] of [["keydown", 1], ["keyup", 0]])
    addEventListener(type, e => {
      const physical = e.code || e.key;
      if (!windowOpen || (document.querySelector("dialog[open]") && (d || !heldKeys.has(physical)))) return;
      if (e.target instanceof HTMLButtonElement && !heldKeys.has(physical)) return;
      const k = heldKeys.get(physical) ?? code(e);
      if (k < 0 || config.ignoredKeys?.includes(k)) return;
      if (d) {
        if (windowOpen && !heldKeys.has(physical)) {
          const step = config.wheel?.keys?.[k];
          if (step) addWheelDelta(step);
        }
        heldKeys.set(physical, k);
      } else heldKeys.delete(physical);
      fut.exports.fut_key(k, d);
      e.preventDefault();
    });
  addEventListener("blur", () => {
    for (const k of heldKeys.values()) fut.exports.fut_key(k, 0);
    heldKeys.clear();
    wheelDelta = 0;
    if (config.wheel) fut.exports.fut_key(config.wheel.signal, 0);
  });


  addEventListener("pagehide", stop);
  addEventListener("pageshow", e => { if (e.persisted) location.reload(); });
  running = true;
  boot.hidden = true;
  try {
    const code = await fut.run(main, true);
    if (code === 0) { if (config.reloadOnFinish) location.reload(); }
    else { boot.hidden = false; boot.textContent = "The application stopped (" + code + "). Reload to try again."; }
  } finally { stop(); }
}
