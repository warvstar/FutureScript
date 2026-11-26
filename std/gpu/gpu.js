// std/gpu in the browser: the flat API of futgpu.cpp (the native library, over Dawn) over the browser's WebGPU, with
// the same semantics. Handles are numbers into one table, never reused, so a released or foreign handle is an error,
// not a crash. Every call that can fail returns 0 (handles) or -1 (counts) and leaves a message for fg_error().
// What WebGPU does asynchronously (the adapter and device, mapping a buffer, a shader's messages, error scopes) the
// calls await: a Promise from an extern suspends the fut code calling it (JSPI; in the threads build the page awaits
// it while the worker waits), so each is synchronous for fut. Calls that return nothing (fg_write, fg_draw, ...)
// don't wait for their error scopes: fg_error() does, so gpu.fut's check right after each still names its error.
//
// fut.pkg declares it main (`js gpu ./gpu.js main`): in the threads build it runs on the page's thread, which has the
// window's canvas. A ptr argument is an address in fut's memory (shared memory in the threads build, which the page
// sees too); fut.js and browser.js put that memory, and the page's window (screen), in `fut` below.
//
// The window: fg_open takes the page's window (screen.take: a canvas of our own in place of the pixel buffer's, input
// wired the same) and draws each frame into a texture of its own, copied to the canvas when fut's present() shows it
// (or the next fg_frame or fg_present), all in one task. Drawing straight into the canvas's texture would break when
// a frame's awaits (fg_error) let the page present it, and expire it, between two draws. The surface format is the
// browser's preferred one, a plain 8-bit (not sRGB) format as natively, so shaders' colors look the same.
export const fut = {};  // filled in by the host (fut.js / browser.js): {memory: WebAssembly.Memory, screen}

const VERTEX = 32, INDEX = 16, UNIFORM = 64, STORAGE = 128, COPY_SRC = 4, COPY_DST = 8, MAP_READ = 1;  // GPUBufferUsage
const RENDER_ATTACHMENT = 16, TEX_COPY_SRC = 1, TEX_COPY_DST = 2, TEXTURE_BINDING = 4;  // GPUTextureUsage

// ---- errors
let err = "", notes = "";
const inflight = new Set();  // error scopes of calls that returned before they settled
const fail = (m) => ((err = m), false);
const note = (m) => (notes += (notes ? "\n" : "") + m);
const message = (e) => String(e?.message ?? e).replace(/[\n ]+$/, "");

// ---- the registry
const CTX = 1, BUFFER = 2, SHADER = 3, COMPUTE = 4, RENDER = 5, BIND = 6;
const kindNames = ["", "context", "buffer", "shader", "compute pipeline", "render pipeline", "bind group"];
const objs = new Map();  // handle -> {kind, g (its context's handle), p (the WebGPU object, or a context), size}
let nextId = 1, windowCtx = 0;
function add(kind, g, p, size = 0) {
  const h = nextId++;
  objs.set(h, { kind, g: kind == CTX ? h : g, p, size });
  return h;
}
function ctx(g) {
  const o = objs.get(g);
  if (!o || o.kind != CTX) return fail("not an open GPU context (released, or never opened)"), null;
  return o.p;
}
function get(g, h, k, k2 = 0) {  // h must be a live k (or k2) of context g
  const o = objs.get(h), want = kindNames[k] + (k2 ? " or " + kindNames[k2] : "");
  if (!o) return fail(`not a ${want} (${h > 0 && h < nextId ? "released" : "no such handle"})`), null;
  if (o.kind != k && o.kind != k2) return fail(`expected a ${want}, got a ${kindNames[o.kind]}`), null;
  if (o.g != g) return fail(`this ${kindNames[o.kind]} belongs to another GPU context`), null;
  return o;
}
function releaseObj(o) {
  if (o.kind == BUFFER) o.p.destroy();
}

// ---- fut's memory: a ptr is an address in it, good for the call
function memory(what) {
  if (!fut.memory) throw new Error(`${what}: this host doesn't give JavaScript modules fut's memory`);
  return fut.memory.buffer;
}
function bytesAt(p, n, what) {  // a copy where WebGPU may not take a view of it (shared memory)
  const b = memory(what), v = new Uint8Array(b, p, n);
  return b instanceof ArrayBuffer ? v : v.slice();
}

// ---- error scopes: f's first error ("" if none), a JS exception (a malformed descriptor) included
function scope(c, f) {
  c.dev.pushErrorScope("internal"), c.dev.pushErrorScope("out-of-memory"), c.dev.pushErrorScope("validation");
  let thrown = "";
  try { f(); } catch (e) { thrown = message(e) || "GPU error"; }
  const pops = [0, 1, 2].map(() => c.dev.popErrorScope().then((e) => (e ? message(e) || "GPU error" : ""), (e) => "error scope: " + message(e)));
  return Promise.all(pops).then((ms) => thrown || ms.find((m) => m) || "");
}
async function scoped(c, f) {  // true, or false and the error
  const m = await scope(c, f);
  return m ? fail(m) : true;
}
function later(c, f, failed) {  // f, its error left for fg_error (failed(): what to undo if it made one); a Promise of it
  const p = scope(c, f).then((m) => (m && (fail(m), failed?.()), m));
  return inflight.add(p), p.finally(() => inflight.delete(p)), p;
}
function submit(c, enc) { c.queue.submit([enc.finish()]); }
async function mapRead(st) {
  try { await st.mapAsync(1 /* GPUMapMode.READ */); return true; } catch (e) { return fail("mapping for reading failed: " + message(e)); }
}
const staging = (c, size) => c.dev.createBuffer({ size, usage: MAP_READ | COPY_DST });

// ---- frames
function target(c, w, h) {  // the texture frames are drawn into: headless, the picture; a window, copied to its canvas
  if (c.target && c.w == w && c.h == h) return;
  c.target?.destroy(), c.depth?.destroy();
  c.w = w, c.h = h;
  c.target = c.dev.createTexture({ size: [w, h], format: c.format, usage: RENDER_ATTACHMENT | TEX_COPY_SRC | TEX_COPY_DST | TEXTURE_BINDING });
  c.view = c.target.createView();
  c.depth = c.dev.createTexture({ size: [w, h], format: "depth24plus", usage: RENDER_ATTACHMENT });  // pipelines made with depth test it
  c.depthView = c.depth.createView();
}
function pass(c, pipe, bind, vbuf, count) {  // one render pass, submitted: the frame's first clears it
  const enc = c.dev.createCommandEncoder();
  const load = c.cleared ? "load" : "clear";
  const p = enc.beginRenderPass({
    colorAttachments: [{ view: c.view, loadOp: load, storeOp: "store", clearValue: c.clear }],
    depthStencilAttachment: { view: c.depthView, depthLoadOp: load, depthStoreOp: "store", depthClearValue: 1 },
  });
  if (pipe) {
    p.setPipeline(pipe);
    if (bind) p.setBindGroup(0, bind);
    if (vbuf) p.setVertexBuffer(0, vbuf);
    p.draw(count);
  }
  p.end(), submit(c, enc);
}
function clearFrame(c) {  // a frame with no draws still clears; a Promise of its error, if it did
  if (!c.frame || c.cleared) return null;
  const p = later(c, () => pass(c, null, null, null, 0));
  return (c.cleared = true), p;
}
function endFrame(c) {  // a window's: the frame to its canvas, which the page shows at its next rendering
  if (!c.frame || c.headless) return;
  clearFrame(c);
  later(c, () => {
    const tex = c.context.getCurrentTexture(), enc = c.dev.createCommandEncoder();
    enc.copyTextureToTexture({ texture: c.target }, { texture: tex }, [Math.min(c.w, tex.width), Math.min(c.h, tex.height)]);
    submit(c, enc);
  });
  c.frame = false;
}

// ---- opening and closing
function closeCtx(c) {
  if (c.screen) c.screen.give();
  c.target?.destroy();
  c.context?.unconfigure();
  c.closed = true, c.dev?.destroy();
}
async function openDevice(c) {
  const gpu = globalThis.navigator?.gpu;
  if (!gpu) {
    return fail("WebGPU isn't available here: " + (globalThis.document ? "this browser has no navigator.gpu (Chrome 113+ on a page from localhost or https, or a browser with WebGPU turned on)"
      : "it needs a browser (node web/serve.mjs, then open the page), or the native build"));
  }
  if (!c.headless && !fut.screen) return fail("this page has no window for the GPU to draw (fut.js's screen option)");
  const adapter = await gpu.requestAdapter({ powerPreference: "high-performance" }).catch(() => null);
  if (!adapter) return fail("no GPU adapter");
  const limits = {};  // all the adapter offers (big storage buffers for compute)
  for (const k in adapter.limits) if (typeof adapter.limits[k] == "number") limits[k] = adapter.limits[k];
  let dev = await adapter.requestDevice({ requiredLimits: limits }).catch(() => null);
  if (!dev) dev = await adapter.requestDevice().catch((e) => fail("could not open the GPU device: " + message(e)) && null);
  if (!dev) return false;
  c.dev = dev, c.queue = dev.queue;
  dev.lost.then((i) => !c.closed && i.reason != "destroyed" && note("GPU device lost: " + message(i.message)));
  dev.addEventListener("uncapturederror", (e) => note(message(e.error?.message ?? e)));
  if (c.headless) return scoped(c, () => target(c, c.w, c.h));
  c.format = gpu.getPreferredCanvasFormat();  // bgra8unorm or rgba8unorm: not sRGB, as natively
  const canvas = fut.screen.take(() => { const o = objs.get(windowCtx); if (o?.p === c) endFrame(c); });  // fut's present() shows our frames
  if (!canvas) return fail("call window(w, h) before gpu.open()");
  c.screen = fut.screen;
  c.context = canvas.getContext("webgpu");
  if (!c.context) return fail("the window's canvas has no WebGPU context");
  c.canvas = canvas;
  return scoped(c, () => {
    c.context.configure({ device: dev, format: c.format, alphaMode: "opaque", usage: RENDER_ATTACHMENT | TEX_COPY_DST });
    target(c, canvas.width, canvas.height);
  });
}

export function fg_error() {  // once the calls still in flight have settled: a Promise of it then
  if (inflight.size) return Promise.all(inflight).then(fg_error);
  let shown = err;
  err = "";
  if (notes) shown += (shown ? "\n" : "") + notes, (notes = "");
  return shown;
}

export async function fg_open(w, h, headless) {
  if (headless && (w <= 0 || h <= 0 || w > 16384 || h > 16384)) return fail("fg_open: the size must be 1..16384 by 1..16384"), 0;
  if (!headless && windowCtx) return fail("the window already has a GPU context: release it first"), 0;
  const c = { headless: !!headless, w: 0, h: 0, format: "rgba8unorm", frame: false, cleared: false, clear: { r: 0, g: 0, b: 0, a: 1 } };
  if (headless) c.w = w, c.h = h;
  if (!(await openDevice(c))) {
    const m = err;
    return closeCtx(c), fail(m), 0;
  }
  const g = add(CTX, 0, c);
  if (!headless) windowCtx = g;
  return g;
}

export function fg_release(hd) {
  const o = objs.get(hd);
  if (!o) return void fail(hd > 0 && hd < nextId ? "already released" : "not a GPU handle");
  objs.delete(hd);
  if (o.kind != CTX) return releaseObj(o);
  for (const [h, x] of objs) if (x.g == hd) releaseObj(x), objs.delete(h);  // everything it made goes with it
  if (hd == windowCtx) windowCtx = 0;
  closeCtx(o.p);
}

export async function fg_buffer(g, data, bytes, usage) {
  const c = ctx(g);
  if (!c) return 0;
  if (bytes < 0) return fail("fg_buffer: a negative size"), 0;
  if (usage & ~15) return fail("fg_buffer: usage bits are 1 vertex, 2 index, 4 uniform, 8 storage"), 0;
  let u = COPY_DST | COPY_SRC;
  if (usage & 1) u |= VERTEX;
  if (usage & 2) u |= INDEX;
  if (usage & 4) u |= UNIFORM;
  if (usage & 8) u |= STORAGE;
  const align = usage & 4 ? 16 : 4, size = Math.max(align, Math.ceil(bytes / align) * align);
  let b = null;
  const ok = await scoped(c, () => {
    b = c.dev.createBuffer({ size, usage: u, mappedAtCreation: !!(data && bytes > 0) });
    if (data && bytes > 0) new Uint8Array(b.getMappedRange()).set(new Uint8Array(memory("fg_buffer"), data, bytes)), b.unmap();
  });
  if (!ok) return b?.destroy(), 0;
  return add(BUFFER, g, b, size);
}

export function fg_write(g, buf, data, bytes) {
  const c = ctx(g), b = c ? get(g, buf, BUFFER) : null;
  if (!b || bytes <= 0) return;
  if (!data) return void fail("fg_write: no data");
  if (bytes > b.size) return void fail(`fg_write: ${bytes} bytes into a buffer of ${b.size}`);
  later(c, () => {
    let src = bytesAt(data, bytes, "fg_write");
    if (bytes % 4) {  // writes are whole words: the last one's tail is zeroed
      const padded = new Uint8Array(Math.ceil(bytes / 4) * 4);
      padded.set(src), (src = padded);
    }
    c.queue.writeBuffer(b.p, 0, src);
  });
}

export async function fg_read(g, buf, out, bytes) {
  const c = ctx(g), b = c ? get(g, buf, BUFFER) : null;
  if (!b) return -1;
  if (bytes < 0 || bytes > b.size) return fail(`fg_read: ${bytes} bytes from a buffer of ${b.size}`), -1;
  if (bytes == 0) return 0;
  if (!out) return fail("fg_read: nowhere to read into"), -1;
  const size = Math.ceil(bytes / 4) * 4;
  let st = null;
  let ok = await scoped(c, () => {
    st = staging(c, size);
    const enc = c.dev.createCommandEncoder();
    enc.copyBufferToBuffer(b.p, 0, st, 0, size), submit(c, enc);
  });
  ok = ok && (await mapRead(st));
  if (ok) {
    try { new Uint8Array(memory("fg_read"), out, bytes).set(new Uint8Array(st.getMappedRange(0, size), 0, bytes)); }
    catch (e) { ok = fail("fg_read: " + message(e)); }
  }
  st?.destroy();
  return ok ? bytes : -1;
}

export async function fg_shader(g, wgsl) {
  const c = ctx(g);
  if (!c) return 0;
  if (wgsl == null) return fail("fg_shader: no source"), 0;
  let m = null;
  const scopeErr = await scope(c, () => void (m = c.dev.createShaderModule({ code: wgsl })));
  let msgs = "";  // the compiler's messages say where; the scope's error is the fallback
  if (m) {
    const info = await m.getCompilationInfo().catch(() => null);
    for (const e of info?.messages ?? [])
      if (e.type == "error") msgs += (msgs ? "\n" : "") + `${e.lineNum}:${e.linePos}: ${message(e.message)}`;
  }
  if (m && !msgs && !scopeErr) return add(SHADER, g, m);
  return fail(msgs || scopeErr || "fg_shader: could not compile"), 0;
}

export async function fg_compute(g, shader, entry) {
  const c = ctx(g), s = c ? get(g, shader, SHADER) : null;
  if (!s) return 0;
  let p = null;
  const ok = await scoped(c, () => void (p = c.dev.createComputePipeline({ layout: "auto", compute: { module: s.p, entryPoint: entry || undefined } })));
  return ok ? add(COMPUTE, g, p) : 0;
}

const SIZES = {  // vertex formats, and their sizes in bytes
  uint8: 1, uint8x2: 2, uint8x4: 4, sint8: 1, sint8x2: 2, sint8x4: 4, unorm8: 1, unorm8x2: 2, unorm8x4: 4, snorm8: 1, snorm8x2: 2, snorm8x4: 4,
  uint16: 2, uint16x2: 4, uint16x4: 8, sint16: 2, sint16x2: 4, sint16x4: 8, unorm16: 2, unorm16x2: 4, unorm16x4: 8,
  snorm16: 2, snorm16x2: 4, snorm16x4: 8, float16: 2, float16x2: 4, float16x4: 8,
  float32: 4, float32x2: 8, float32x3: 12, float32x4: 16, uint32: 4, uint32x2: 8, uint32x3: 12, uint32x4: 16,
  sint32: 4, sint32x2: 8, sint32x3: 12, sint32x4: 16, "unorm10-10-10-2": 4, "snorm10-10-10-2": 4, "unorm8x4-bgra": 4,
};
const TOPOLOGIES = ["triangle-list", "point-list", "line-list", "triangle-strip", "line-strip"];

export async function fg_render(g, shader, vs, fs, attrs, topology) {
  const c = ctx(g), s = c ? get(g, shader, SHADER) : null;
  if (!s) return 0;
  const depth = (topology & 8) != 0;  // 8: test and write the depth buffer (nearer fragments win)
  topology &= ~8;
  if (!(topology >= 0 && topology <= 4)) return fail("fg_render: topology is 0 triangle-list, 1 point-list, 2 line-list, 3 triangle-strip, 4 line-strip"), 0;
  const va = [];
  let stride = 0;
  for (const f of (attrs ?? "").split(" ").filter(Boolean)) {
    if (!Object.hasOwn(SIZES, f)) return fail(`fg_render: unknown vertex format "${f}" (float32, float32x2, uint32x4, unorm8x4, ...)`), 0;
    va.push({ format: f, offset: stride, shaderLocation: va.length }), (stride += SIZES[f]);
  }
  const d = {
    layout: "auto",
    vertex: { module: s.p, entryPoint: vs || undefined, buffers: va.length ? [{ arrayStride: stride, stepMode: "vertex", attributes: va }] : [] },
    fragment: { module: s.p, entryPoint: fs || undefined, targets: [{ format: c.format }] },
    primitive: { topology: TOPOLOGIES[topology] },
    depthStencil: { format: "depth24plus", depthWriteEnabled: depth, depthCompare: depth ? "less" : "always" },
  };
  let p = null;
  const ok = await scoped(c, () => void (p = c.dev.createRenderPipeline(d)));
  return ok ? add(RENDER, g, p) : 0;
}

export async function fg_bind(g, pipeline, ...bs) {
  const c = ctx(g), p = c ? get(g, pipeline, COMPUTE, RENDER) : null;
  if (!p) return 0;
  const entries = [];
  for (let i = 0; i < 5 && bs[i]; i++) {
    const b = get(g, bs[i], BUFFER);
    if (!b) return fail(`fg_bind: binding ${i}: ${err}`), 0;
    entries.push({ binding: i, resource: { buffer: b.p } });
  }
  let bg = null;
  const ok = await scoped(c, () => void (bg = c.dev.createBindGroup({ layout: p.p.getBindGroupLayout(0), entries })));
  return ok ? add(BIND, g, bg) : 0;
}

export function fg_dispatch(g, pipeline, bind, x, y, z) {
  const c = ctx(g), p = c ? get(g, pipeline, COMPUTE) : null, b = p && bind ? get(g, bind, BIND) : null;
  if (!p || (bind && !b)) return;
  if (x < 0 || y < 0 || z < 0) return void fail("fg_dispatch: negative workgroup counts");
  later(c, () => {
    const enc = c.dev.createCommandEncoder(), pass = enc.beginComputePass();
    pass.setPipeline(p.p);
    if (b) pass.setBindGroup(0, b.p);
    pass.dispatchWorkgroups(x, y, z), pass.end(), submit(c, enc);
  });
}

export function fg_frame(g, rgb) {
  const c = ctx(g);
  if (!c) return 0;
  c.clear = { r: ((rgb >> 16) & 255) / 255, g: ((rgb >> 8) & 255) / 255, b: (rgb & 255) / 255, a: 1 };
  if (!c.headless) {
    endFrame(c);  // a frame left open is shown first
    try { target(c, Math.max(1, c.canvas.width), Math.max(1, c.canvas.height)); }  // the window was resized
    catch (e) { return fail("fg_frame: " + message(e)), 0; }
  }
  c.frame = true, (c.cleared = false);
  return 1;
}

export function fg_draw(g, pipeline, bind, vbuf, count) {
  const c = ctx(g), p = c ? get(g, pipeline, RENDER) : null;
  const b = p && bind ? get(g, bind, BIND) : null, v = p && (!bind || b) && vbuf ? get(g, vbuf, BUFFER) : null;
  if (!p || (bind && !b) || (vbuf && !v)) return;
  if (!c.frame) return void fail("fg_draw: no frame (call fg_frame first)");
  if (count < 0) return void fail("fg_draw: a negative vertex count");
  const first = !c.cleared;
  later(c, () => pass(c, p.p, b?.p, v?.p, count), () => first && c.frame && (c.cleared = false));  // (it didn't clear)
  c.cleared = true;
}

export function fg_present(g) {
  const c = ctx(g);
  if (!c) return;
  if (c.headless) clearFrame(c);
  else endFrame(c);
}

export async function fg_pixels(g, out) {
  const c = ctx(g);
  if (!c) return -1;
  if (!c.headless) return fail("fg_pixels: only a headless context can be read back"), -1;
  if (!out) return fail("fg_pixels: nowhere to copy into"), -1;
  if (await clearFrame(c)) return -1;
  const row = c.w * 4, stride = Math.ceil(row / 256) * 256, size = stride * c.h;  // copies take rows 256-byte aligned
  let st = null;
  let ok = await scoped(c, () => {
    st = staging(c, size);
    const enc = c.dev.createCommandEncoder();
    enc.copyTextureToBuffer({ texture: c.target }, { buffer: st, bytesPerRow: stride, rowsPerImage: c.h }, [c.w, c.h]);
    submit(c, enc);
  });
  ok = ok && (await mapRead(st));
  if (ok) {
    try {
      const src = new Uint8Array(st.getMappedRange(0, size)), dst = new Uint8Array(memory("fg_pixels"), out, row * c.h);
      for (let y = 0; y < c.h; y++) dst.set(src.subarray(y * stride, y * stride + row), y * row);
    } catch (e) { ok = fail("fg_pixels: " + message(e)); }
  }
  st?.destroy();
  return ok ? row * c.h : -1;
}
