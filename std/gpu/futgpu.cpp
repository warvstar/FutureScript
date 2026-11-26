// futgpu: a flat C API over Dawn (WebGPU) for std/gpu, loaded by fut with dlopen (`c gpu libfutgpu.dylib`).
// Built by `python3 build.py gpu`. Handles are int64 ids into one registry: never reused, so a released or foreign
// handle is an error, not a crash. Every call that can fail returns 0 (handles) or -1 (counts) and leaves a message
// for fg_error(). Each call that makes or submits something runs inside its own error scopes, so a validation error
// is reported by the call that caused it. Calls are serialized by one lock (isolates may call from their threads).
// Window mode reaches AppKit through the Objective-C runtime, as src/gfx.cpp does, and does not link against fut:
// fut_on_present is looked up with dlsym.
#include <webgpu/webgpu.h>
#include <dlfcn.h>
#include <pthread.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define API extern "C" __attribute__((visibility("default")))

// ---- errors
static thread_local std::string err;  // this thread's last error, until fg_error reads it
static std::mutex pendMu;
static std::string pending;  // uncaptured errors and device loss, from callbacks that may run on any thread

static std::string str(WGPUStringView s) {
  if (!s.data) return "";
  std::string r(s.data, s.length == WGPU_STRLEN ? strlen(s.data) : s.length);
  while (!r.empty() && (r.back() == '\n' || r.back() == ' ')) r.pop_back();
  return r;
}
static WGPUStringView sv(const char* s) { return {s && *s ? s : nullptr, WGPU_STRLEN}; }  // "" = undefined (a default entry point)
static bool fail(std::string m) { return err = std::move(m), false; }
static void note(std::string m) {
  std::lock_guard l(pendMu);
  pending += (pending.empty() ? "" : "\n") + m;
}

// ---- the Objective-C runtime (window mode)
using id = void*;
extern "C" id objc_msgSend(), objc_getClass(const char*), sel_registerName(const char*);
extern "C" void* objc_autoreleasePoolPush();
extern "C" void objc_autoreleasePoolPop(void*);
template <class R = id, class... A> static R msg(id o, const char* sel, A... a) {
  return ((R(*)(id, void*, A...))(void*)objc_msgSend)(o, sel_registerName(sel), a...);
}
struct Rect { double x, y, w, h; };
struct Pool {
  void* p = objc_autoreleasePoolPush();
  ~Pool() { objc_autoreleasePoolPop(p); }
};

// ---- the registry
enum Kind { CTX = 1, BUF, SHADER, COMPUTE, RENDER, BIND };
struct Ctx {
  WGPUInstance inst = nullptr;
  WGPUAdapter adapter = nullptr;
  WGPUDevice dev = nullptr;
  WGPUQueue queue = nullptr;
  WGPUTextureFormat format = WGPUTextureFormat_RGBA8Unorm;
  bool headless = true;
  uint32_t w = 0, h = 0;
  WGPUTexture target = nullptr;  // headless: the offscreen target
  WGPUTextureView targetView = nullptr;
  WGPUSurface surface = nullptr;  // window
  id win = nullptr, view = nullptr, layer = nullptr, oldLayer = nullptr;
  double scale = 0;
  bool frame = false, cleared = false;  // a frame is open; its texture was cleared (by its first pass)
  WGPUColor clear{0, 0, 0, 1};
  WGPUTexture tex = nullptr;  // the frame's texture (window: the surface's, owned until present)
  WGPUTextureView texView = nullptr;
  WGPUTexture depth = nullptr;  // every pass's depth buffer, the frame's size (pipelines made with depth test it)
  WGPUTextureView depthView = nullptr;
  uint32_t dw = 0, dh = 0;
};
struct Obj {
  Kind kind;
  int64_t g;  // the context it belongs to (itself for a context)
  void* p;
  uint64_t size = 0;  // buffers
};
static std::recursive_mutex mu;  // recursive: fut's present() calls back into us on the thread that may hold it
static std::unordered_map<int64_t, Obj> objs;
static int64_t nextId = 1;
#define LOCK std::lock_guard<std::recursive_mutex> lock_(mu)

static int64_t add(Kind k, int64_t g, void* p, uint64_t size = 0) {
  int64_t h = nextId++;
  objs[h] = {k, k == CTX ? h : g, p, size};
  return h;
}
static Ctx* ctx(int64_t g) {
  auto it = objs.find(g);
  if (it == objs.end() || it->second.kind != CTX) return fail("not an open GPU context (released, or never opened)"), nullptr;
  return (Ctx*)it->second.p;
}
static const char* kindName(Kind k) {
  static const char* names[] = {"", "context", "buffer", "shader", "compute pipeline", "render pipeline", "bind group"};
  return names[k];
}
static Obj* get(int64_t g, int64_t h, Kind k, Kind k2 = (Kind)0) {  // h must be a live k (or k2) of context g
  auto it = objs.find(h);
  std::string want = std::string(kindName(k)) + (k2 ? std::string(" or ") + kindName(k2) : "");
  if (it == objs.end()) return fail("not a " + want + " (" + (h > 0 && h < nextId ? "released" : "no such handle") + ")"), nullptr;
  if (it->second.kind != k && it->second.kind != k2) return fail(std::string("expected a ") + want + ", got a " + kindName(it->second.kind)), nullptr;
  if (it->second.g != g) return fail(std::string("this ") + kindName(it->second.kind) + " belongs to another GPU context"), nullptr;
  return &it->second;
}
static void releaseObj(const Obj& o) {
  switch (o.kind) {
    case BUF: wgpuBufferRelease((WGPUBuffer)o.p); break;
    case SHADER: wgpuShaderModuleRelease((WGPUShaderModule)o.p); break;
    case COMPUTE: wgpuComputePipelineRelease((WGPUComputePipeline)o.p); break;
    case RENDER: wgpuRenderPipelineRelease((WGPURenderPipeline)o.p); break;
    case BIND: wgpuBindGroupRelease((WGPUBindGroup)o.p); break;
    case CTX: break;
  }
}

// ---- waiting and error scopes
static bool wait(Ctx* c, WGPUFuture f) {
  WGPUFutureWaitInfo w{f, false};
  WGPUWaitStatus s = wgpuInstanceWaitAny(c->inst, 1, &w, UINT64_MAX);
  return s == WGPUWaitStatus_Success && w.completed ? true : fail("waiting on the GPU failed");
}
static void push(Ctx* c) {
  wgpuDevicePushErrorScope(c->dev, WGPUErrorFilter_Internal);
  wgpuDevicePushErrorScope(c->dev, WGPUErrorFilter_OutOfMemory);
  wgpuDevicePushErrorScope(c->dev, WGPUErrorFilter_Validation);
}
static std::string pop(Ctx* c) {  // the first error of the three scopes, "" if none
  std::string first;
  for (int i = 0; i < 3; i++) {
    std::string m;
    WGPUPopErrorScopeCallbackInfo cb{nullptr, WGPUCallbackMode_WaitAnyOnly,
                                     [](WGPUPopErrorScopeStatus s, WGPUErrorType t, WGPUStringView msg, void* u, void*) {
                                       if (s != WGPUPopErrorScopeStatus_Success) *(std::string*)u = "error scope: " + str(msg);
                                       else if (t != WGPUErrorType_NoError) *(std::string*)u = str(msg);
                                       if (s == WGPUPopErrorScopeStatus_Success && t != WGPUErrorType_NoError && ((std::string*)u)->empty())
                                         *(std::string*)u = "GPU error";
                                     },
                                     &m, nullptr};
    if (!wait(c, wgpuDevicePopErrorScope(c->dev, cb)) && m.empty()) m = err;
    if (first.empty()) first = m;
  }
  return first;
}
template <class F> static bool scoped(Ctx* c, F f) {  // run f inside error scopes; false (and the error) if it made one
  push(c);
  bool ok = f();
  std::string m = pop(c);
  return !m.empty() ? fail(m) : ok;
}
static bool submit(Ctx* c, WGPUCommandEncoder enc) {
  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
  wgpuQueueSubmit(c->queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd), wgpuCommandEncoderRelease(enc);
  return true;
}
static bool mapRead(Ctx* c, WGPUBuffer b, uint64_t size) {
  std::string m;
  bool ok = false;
  WGPUBufferMapCallbackInfo cb{nullptr, WGPUCallbackMode_WaitAnyOnly,
                               [](WGPUMapAsyncStatus s, WGPUStringView msg, void* u1, void* u2) {
                                 *(bool*)u1 = s == WGPUMapAsyncStatus_Success;
                                 if (s != WGPUMapAsyncStatus_Success) *(std::string*)u2 = str(msg);
                               },
                               &ok, &m};
  if (!wait(c, wgpuBufferMapAsync(b, WGPUMapMode_Read, 0, size, cb))) return false;
  return ok ? true : fail("mapping for reading failed: " + m);
}
static WGPUBuffer staging(Ctx* c, uint64_t size) {
  WGPUBufferDescriptor d = WGPU_BUFFER_DESCRIPTOR_INIT;
  d.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst, d.size = size;
  return wgpuDeviceCreateBuffer(c->dev, &d);
}

// ---- frames
static bool viewSize(Ctx* c, uint32_t& w, uint32_t& h) {  // the content view's size in pixels; updates the layer's scale
  Rect b = msg<Rect>(c->view, "bounds");
  double s = msg<double>(c->win, "backingScaleFactor");
  if (s <= 0) s = 1;
  if (s != c->scale) c->scale = s, msg<void>(c->layer, "setContentsScale:", s);
  w = (uint32_t)std::max(1l, lround(b.w * s)), h = (uint32_t)std::max(1l, lround(b.h * s));
  return true;
}
static void configure(Ctx* c, uint32_t w, uint32_t h) {
  WGPUSurfaceConfiguration cfg = WGPU_SURFACE_CONFIGURATION_INIT;
  cfg.device = c->dev, cfg.format = c->format, cfg.usage = WGPUTextureUsage_RenderAttachment;
  cfg.width = w, cfg.height = h, cfg.presentMode = WGPUPresentMode_Fifo, cfg.alphaMode = WGPUCompositeAlphaMode_Auto;
  wgpuSurfaceConfigure(c->surface, &cfg);
  c->w = w, c->h = h;
}
static void ensureDepth(Ctx* c) {  // a depth buffer the size of the frame
  if (c->depth && c->dw == c->w && c->dh == c->h) return;
  if (c->depthView) wgpuTextureViewRelease(c->depthView);
  if (c->depth) wgpuTextureRelease(c->depth);
  WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
  td.size = {c->w, c->h, 1}, td.format = WGPUTextureFormat_Depth24Plus, td.usage = WGPUTextureUsage_RenderAttachment;
  c->depth = wgpuDeviceCreateTexture(c->dev, &td);
  c->depthView = wgpuTextureCreateView(c->depth, nullptr);
  c->dw = c->w, c->dh = c->h;
}
static bool pass(Ctx* c, WGPURenderPipeline pipe, WGPUBindGroup bind, WGPUBuffer vbuf, uint32_t count) {  // one render pass, submitted
  ensureDepth(c);
  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(c->dev, nullptr);
  WGPURenderPassColorAttachment a = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
  a.view = c->texView, a.loadOp = c->cleared ? WGPULoadOp_Load : WGPULoadOp_Clear, a.storeOp = WGPUStoreOp_Store, a.clearValue = c->clear;
  WGPURenderPassDepthStencilAttachment da = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
  da.view = c->depthView, da.depthLoadOp = c->cleared ? WGPULoadOp_Load : WGPULoadOp_Clear, da.depthStoreOp = WGPUStoreOp_Store;
  da.depthClearValue = 1;
  WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
  rp.colorAttachmentCount = 1, rp.colorAttachments = &a, rp.depthStencilAttachment = &da;
  WGPURenderPassEncoder p = wgpuCommandEncoderBeginRenderPass(enc, &rp);
  if (pipe) {
    wgpuRenderPassEncoderSetPipeline(p, pipe);
    if (bind) wgpuRenderPassEncoderSetBindGroup(p, 0, bind, 0, nullptr);
    if (vbuf) wgpuRenderPassEncoderSetVertexBuffer(p, 0, vbuf, 0, WGPU_WHOLE_SIZE);
    wgpuRenderPassEncoderDraw(p, count, 1, 0, 0);
  }
  wgpuRenderPassEncoderEnd(p), wgpuRenderPassEncoderRelease(p);
  return submit(c, enc);
}
static bool clearFrame(Ctx* c) {  // a frame with no draws still clears
  if (!c->frame || c->cleared) return true;
  bool ok = scoped(c, [&] { return pass(c, nullptr, nullptr, nullptr, 0); });
  return c->cleared = ok, ok;
}
static void endFrame(Ctx* c) {  // window: present and give the texture back
  if (!c->frame || c->headless) return;
  clearFrame(c);
  {
    Pool pool;
    wgpuSurfacePresent(c->surface);
  }
  wgpuTextureViewRelease(c->texView), wgpuTextureRelease(c->tex);
  c->texView = nullptr, c->tex = nullptr, c->frame = false;
}
static int64_t windowCtx;  // the context drawing to fut's window (one at a time)
using OnPresent = void (*)(void (*)(void*), void*);
static OnPresent futOnPresent() { return (OnPresent)dlsym(RTLD_DEFAULT, "fut_on_present"); }  // fut's, when it has it
static void onPresent(void* arg) {  // registered with fut's present(): each frame it shows is ours
  LOCK;
  auto it = objs.find((int64_t)(intptr_t)arg);
  if (it != objs.end() && it->second.kind == CTX) endFrame((Ctx*)it->second.p);
}

// ---- opening and closing
static void closeCtx(Ctx* c) {
  if (c->frame && !c->headless) wgpuTextureViewRelease(c->texView), wgpuTextureRelease(c->tex);
  if (c->targetView) wgpuTextureViewRelease(c->targetView);
  if (c->target) wgpuTextureRelease(c->target);
  if (c->depthView) wgpuTextureViewRelease(c->depthView);
  if (c->depth) wgpuTextureRelease(c->depth);
  if (c->surface) {
    if (c->w) wgpuSurfaceUnconfigure(c->surface);
    wgpuSurfaceRelease(c->surface);
  }
  if (c->view) {  // give the view its own layer back (fut's pixel buffer draws into it)
    Pool pool;
    msg<void>(c->view, "setLayer:", c->oldLayer), msg<void>(c->view, "setWantsLayer:", true);
    if (c->oldLayer) msg<void>(c->oldLayer, "release");
    msg<void>(c->layer, "release");
  }
  if (c->queue) wgpuQueueRelease(c->queue);
  if (c->dev) wgpuDeviceDestroy(c->dev), wgpuDeviceRelease(c->dev);
  if (c->adapter) wgpuAdapterRelease(c->adapter);
  if (c->inst) wgpuInstanceProcessEvents(c->inst), wgpuInstanceRelease(c->inst);
  delete c;
}
static bool openWindow(Ctx* c) {  // a CAMetalLayer in fut's window (src/gfx.cpp), and a surface on it
  Pool pool;
  id app = objc_getClass("NSApplication") ? msg(objc_getClass("NSApplication"), "sharedApplication") : nullptr;
  id wins = app ? msg(app, "windows") : nullptr;
  if (!wins || !msg<unsigned long>(wins, "count")) return fail("call window(w, h) before gpu.open()");
  id metal = objc_getClass("CAMetalLayer");
  if (!metal) return fail("CAMetalLayer is not available");
  c->win = msg(wins, "objectAtIndex:", 0ul), c->view = msg(c->win, "contentView");
  c->oldLayer = msg(c->view, "layer");
  if (c->oldLayer) msg(c->oldLayer, "retain");  // fut may still hold it (and gets it back when the context is released)
  c->layer = msg(msg(metal, "alloc"), "init");
  msg<void>(c->view, "setLayer:", c->layer), msg<void>(c->view, "setWantsLayer:", true);  // layer-hosting
  WGPUSurfaceSourceMetalLayer src = WGPU_SURFACE_SOURCE_METAL_LAYER_INIT;
  src.layer = c->layer;
  WGPUSurfaceDescriptor sd = WGPU_SURFACE_DESCRIPTOR_INIT;
  sd.nextInChain = &src.chain;
  c->surface = wgpuInstanceCreateSurface(c->inst, &sd);
  return c->surface ? true : fail("could not make a surface for the window");
}
static bool chooseFormat(Ctx* c) {  // the surface's preferred format, a plain (not sRGB) 8-bit one if it offers it
  WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
  if (wgpuSurfaceGetCapabilities(c->surface, c->adapter, &caps) != WGPUStatus_Success || !caps.formatCount)
    return fail("the window's surface reports no formats");
  c->format = caps.formats[0];
  for (size_t i = 0; i < caps.formatCount; i++)
    if (caps.formats[i] == WGPUTextureFormat_BGRA8Unorm || caps.formats[i] == WGPUTextureFormat_RGBA8Unorm) {
      c->format = caps.formats[i];
      break;
    }
  wgpuSurfaceCapabilitiesFreeMembers(caps);
  return true;
}
static bool openDevice(Ctx* c) {
  WGPUInstanceFeatureName feature = WGPUInstanceFeatureName_TimedWaitAny;
  WGPUInstanceDescriptor id_ = WGPU_INSTANCE_DESCRIPTOR_INIT;
  id_.requiredFeatureCount = 1, id_.requiredFeatures = &feature;
  if (!(c->inst = wgpuCreateInstance(&id_))) return fail("could not create a WebGPU instance");
  if (!c->headless && !openWindow(c)) return false;

  WGPURequestAdapterOptions ao = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
  ao.powerPreference = WGPUPowerPreference_HighPerformance, ao.compatibleSurface = c->surface;
  std::string m;
  WGPURequestAdapterCallbackInfo acb{nullptr, WGPUCallbackMode_WaitAnyOnly,
                                     [](WGPURequestAdapterStatus s, WGPUAdapter a, WGPUStringView msg, void* u1, void* u2) {
                                       if (s == WGPURequestAdapterStatus_Success) *(WGPUAdapter*)u1 = a;
                                       else *(std::string*)u2 = str(msg);
                                     },
                                     &c->adapter, &m};
  if (!wait(c, wgpuInstanceRequestAdapter(c->inst, &ao, acb))) return false;
  if (!c->adapter) return fail("no GPU adapter" + (m.empty() ? "" : ": " + m));

  WGPULimits limits = WGPU_LIMITS_INIT;  // all the adapter offers (big storage buffers for compute)
  bool haveLimits = wgpuAdapterGetLimits(c->adapter, &limits) == WGPUStatus_Success;
  WGPUDeviceDescriptor dd = WGPU_DEVICE_DESCRIPTOR_INIT;
  dd.requiredLimits = haveLimits ? &limits : nullptr;
  dd.deviceLostCallbackInfo = {nullptr, WGPUCallbackMode_AllowSpontaneous,
                               [](WGPUDevice const*, WGPUDeviceLostReason r, WGPUStringView msg, void*, void*) {
                                 if (r != WGPUDeviceLostReason_Destroyed && r != WGPUDeviceLostReason_CallbackCancelled)
                                   note("GPU device lost: " + str(msg));
                               },
                               nullptr, nullptr};
  dd.uncapturedErrorCallbackInfo = {nullptr, [](WGPUDevice const*, WGPUErrorType, WGPUStringView msg, void*, void*) { note(str(msg)); },
                                    nullptr, nullptr};
  m.clear();
  WGPURequestDeviceCallbackInfo dcb{nullptr, WGPUCallbackMode_WaitAnyOnly,
                                    [](WGPURequestDeviceStatus s, WGPUDevice d, WGPUStringView msg, void* u1, void* u2) {
                                      if (s == WGPURequestDeviceStatus_Success) *(WGPUDevice*)u1 = d;
                                      else *(std::string*)u2 = str(msg);
                                    },
                                    &c->dev, &m};
  if (!wait(c, wgpuAdapterRequestDevice(c->adapter, &dd, dcb))) return false;
  if (!c->dev) return fail("could not open the GPU device" + (m.empty() ? "" : ": " + m));
  c->queue = wgpuDeviceGetQueue(c->dev);

  if (c->headless) {
    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding;
    td.size = {c->w, c->h, 1}, td.format = WGPUTextureFormat_RGBA8Unorm;
    return scoped(c, [&] {
      c->target = wgpuDeviceCreateTexture(c->dev, &td);
      c->targetView = wgpuTextureCreateView(c->target, nullptr);
      return true;
    });
  }
  if (!chooseFormat(c)) return false;
  Pool pool;
  uint32_t w, h;
  viewSize(c, w, h);
  return scoped(c, [&] { return configure(c, w, h), true; });
}

API const char* fg_error(void) {
  static thread_local std::string shown;
  shown = std::move(err), err.clear();
  std::lock_guard l(pendMu);
  if (!pending.empty()) shown += (shown.empty() ? "" : "\n") + pending, pending.clear();
  return shown.c_str();
}

API int64_t fg_open(int w, int h, int headless) {
  LOCK;
  if (headless && (w <= 0 || h <= 0 || w > 16384 || h > 16384)) return fail("fg_open: the size must be 1..16384 by 1..16384"), 0;
  if (!headless && !pthread_main_np()) return fail("gpu.open() on the window must run on the main thread (the main isolate)"), 0;
  if (!headless && windowCtx) return fail("the window already has a GPU context: release it first"), 0;
  Ctx* c = new Ctx;
  c->headless = headless != 0, c->w = headless ? w : 0, c->h = headless ? h : 0;
  if (!openDevice(c)) {
    std::string m = err;
    closeCtx(c);
    return fail(m), 0;
  }
  int64_t g = add(CTX, 0, c);
  if (!c->headless) {
    windowCtx = g;
    if (auto f = futOnPresent()) f(onPresent, (void*)(intptr_t)g);  // fut's present() shows our frames from now on
  }
  return g;
}

API void fg_release(int64_t hd) {
  LOCK;
  auto it = objs.find(hd);
  if (it == objs.end()) return (void)fail(hd > 0 && hd < nextId ? "already released" : "not a GPU handle");
  Obj o = it->second;
  objs.erase(it);
  if (o.kind != CTX) return releaseObj(o);
  for (auto i = objs.begin(); i != objs.end();)  // everything it made goes with it
    if (i->second.g == hd) releaseObj(i->second), i = objs.erase(i);
    else ++i;
  if (hd == windowCtx) {
    windowCtx = 0;
    if (auto f = futOnPresent()) f(nullptr, nullptr);  // and present() shows fut's pixel buffer again
  }
  closeCtx((Ctx*)o.p);
}

API int64_t fg_buffer(int64_t g, const void* data, int64_t bytes, int usage) {
  LOCK;
  Ctx* c = ctx(g);
  if (!c) return 0;
  if (bytes < 0) return fail("fg_buffer: a negative size"), 0;
  if (usage & ~15) return fail("fg_buffer: usage bits are 1 vertex, 2 index, 4 uniform, 8 storage"), 0;
  WGPUBufferDescriptor d = WGPU_BUFFER_DESCRIPTOR_INIT;
  d.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc;
  if (usage & 1) d.usage |= WGPUBufferUsage_Vertex;
  if (usage & 2) d.usage |= WGPUBufferUsage_Index;
  if (usage & 4) d.usage |= WGPUBufferUsage_Uniform;
  if (usage & 8) d.usage |= WGPUBufferUsage_Storage;
  uint64_t align = usage & 4 ? 16 : 4;
  d.size = std::max<uint64_t>(align, ((uint64_t)bytes + align - 1) / align * align);
  d.mappedAtCreation = data && bytes > 0;
  WGPUBuffer b = nullptr;
  bool ok = scoped(c, [&] {
    if (!(b = wgpuDeviceCreateBuffer(c->dev, &d))) return fail("fg_buffer: out of memory");
    if (d.mappedAtCreation) {
      if (void* p = wgpuBufferGetMappedRange(b, 0, d.size)) memcpy(p, data, bytes);
      wgpuBufferUnmap(b);
    }
    return true;
  });
  if (!ok) return b ? wgpuBufferRelease(b) : void(), 0;
  return add(BUF, g, b, d.size);
}

API void fg_write(int64_t g, int64_t buf, const void* data, int64_t bytes) {
  LOCK;
  Ctx* c = ctx(g);
  Obj* b = c ? get(g, buf, BUF) : nullptr;
  if (!b || bytes <= 0) return;
  if (!data) return (void)fail("fg_write: no data");
  if ((uint64_t)bytes > b->size) return (void)fail("fg_write: " + std::to_string(bytes) + " bytes into a buffer of " + std::to_string(b->size));
  scoped(c, [&] {
    if (bytes % 4 == 0) return wgpuQueueWriteBuffer(c->queue, (WGPUBuffer)b->p, 0, data, bytes), true;
    std::vector<uint8_t> padded((bytes + 3) / 4 * 4);  // writes are whole words: the last one's tail is zeroed
    memcpy(padded.data(), data, bytes);
    return wgpuQueueWriteBuffer(c->queue, (WGPUBuffer)b->p, 0, padded.data(), padded.size()), true;
  });
}

API int64_t fg_read(int64_t g, int64_t buf, void* out, int64_t bytes) {
  LOCK;
  Ctx* c = ctx(g);
  Obj* b = c ? get(g, buf, BUF) : nullptr;
  if (!b) return -1;
  if (bytes < 0 || (uint64_t)bytes > b->size) return fail("fg_read: " + std::to_string(bytes) + " bytes from a buffer of " + std::to_string(b->size)), -1;
  if (bytes == 0) return 0;
  if (!out) return fail("fg_read: nowhere to read into"), -1;
  uint64_t size = ((uint64_t)bytes + 3) / 4 * 4;
  WGPUBuffer st = nullptr;
  bool ok = scoped(c, [&] {
    st = staging(c, size);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(c->dev, nullptr);
    wgpuCommandEncoderCopyBufferToBuffer(enc, (WGPUBuffer)b->p, 0, st, 0, size);
    return submit(c, enc);
  });
  ok = ok && mapRead(c, st, size);
  if (ok) {
    const void* p = wgpuBufferGetConstMappedRange(st, 0, size);
    ok = p ? (memcpy(out, p, bytes), true) : fail("fg_read: the mapped buffer has no memory");
    wgpuBufferUnmap(st);
  }
  if (st) wgpuBufferRelease(st);
  return ok ? bytes : -1;
}

API int64_t fg_shader(int64_t g, const char* wgsl) {
  LOCK;
  Ctx* c = ctx(g);
  if (!c) return 0;
  if (!wgsl) return fail("fg_shader: no source"), 0;
  WGPUShaderSourceWGSL src = WGPU_SHADER_SOURCE_WGSL_INIT;
  src.code = {wgsl, WGPU_STRLEN};
  WGPUShaderModuleDescriptor d = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
  d.nextInChain = &src.chain;
  push(c);
  WGPUShaderModule m = wgpuDeviceCreateShaderModule(c->dev, &d);
  std::string scopeErr = pop(c), msgs;  // the compiler's messages say where; the scope's error is the fallback
  WGPUCompilationInfoCallbackInfo cb{nullptr, WGPUCallbackMode_WaitAnyOnly,
                                     [](WGPUCompilationInfoRequestStatus s, WGPUCompilationInfo const* info, void* u, void*) {
                                       if (s != WGPUCompilationInfoRequestStatus_Success || !info) return;
                                       for (size_t i = 0; i < info->messageCount; i++) {
                                         const WGPUCompilationMessage& e = info->messages[i];
                                         if (e.type != WGPUCompilationMessageType_Error) continue;
                                         std::string& s = *(std::string*)u;
                                         s += (s.empty() ? "" : "\n") + std::to_string(e.lineNum) + ":" + std::to_string(e.linePos) + ": " + str(e.message);
                                       }
                                     },
                                     &msgs, nullptr};
  if (m) wait(c, wgpuShaderModuleGetCompilationInfo(m, cb));
  if (m && msgs.empty() && scopeErr.empty()) return add(SHADER, g, m);
  if (m) wgpuShaderModuleRelease(m);
  return fail(!msgs.empty() ? msgs : !scopeErr.empty() ? scopeErr : "fg_shader: could not compile"), 0;
}

API int64_t fg_compute(int64_t g, int64_t shader, const char* entry) {
  LOCK;
  Ctx* c = ctx(g);
  Obj* s = c ? get(g, shader, SHADER) : nullptr;
  if (!s) return 0;
  WGPUComputePipelineDescriptor d = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
  d.compute.module = (WGPUShaderModule)s->p, d.compute.entryPoint = sv(entry);
  WGPUComputePipeline p = nullptr;
  if (!scoped(c, [&] { return (p = wgpuDeviceCreateComputePipeline(c->dev, &d)) != nullptr; }))
    return p ? wgpuComputePipelineRelease(p) : void(), 0;
  return add(COMPUTE, g, p);
}

static bool vertexFormat(const std::string& name, WGPUVertexFormat& f, uint64_t& size) {
  static const struct { const char* n; WGPUVertexFormat f; uint64_t size; } formats[] = {
      {"uint8", WGPUVertexFormat_Uint8, 1}, {"uint8x2", WGPUVertexFormat_Uint8x2, 2}, {"uint8x4", WGPUVertexFormat_Uint8x4, 4},
      {"sint8", WGPUVertexFormat_Sint8, 1}, {"sint8x2", WGPUVertexFormat_Sint8x2, 2}, {"sint8x4", WGPUVertexFormat_Sint8x4, 4},
      {"unorm8", WGPUVertexFormat_Unorm8, 1}, {"unorm8x2", WGPUVertexFormat_Unorm8x2, 2}, {"unorm8x4", WGPUVertexFormat_Unorm8x4, 4},
      {"snorm8", WGPUVertexFormat_Snorm8, 1}, {"snorm8x2", WGPUVertexFormat_Snorm8x2, 2}, {"snorm8x4", WGPUVertexFormat_Snorm8x4, 4},
      {"uint16", WGPUVertexFormat_Uint16, 2}, {"uint16x2", WGPUVertexFormat_Uint16x2, 4}, {"uint16x4", WGPUVertexFormat_Uint16x4, 8},
      {"sint16", WGPUVertexFormat_Sint16, 2}, {"sint16x2", WGPUVertexFormat_Sint16x2, 4}, {"sint16x4", WGPUVertexFormat_Sint16x4, 8},
      {"unorm16", WGPUVertexFormat_Unorm16, 2}, {"unorm16x2", WGPUVertexFormat_Unorm16x2, 4}, {"unorm16x4", WGPUVertexFormat_Unorm16x4, 8},
      {"snorm16", WGPUVertexFormat_Snorm16, 2}, {"snorm16x2", WGPUVertexFormat_Snorm16x2, 4}, {"snorm16x4", WGPUVertexFormat_Snorm16x4, 8},
      {"float16", WGPUVertexFormat_Float16, 2}, {"float16x2", WGPUVertexFormat_Float16x2, 4}, {"float16x4", WGPUVertexFormat_Float16x4, 8},
      {"float32", WGPUVertexFormat_Float32, 4}, {"float32x2", WGPUVertexFormat_Float32x2, 8},
      {"float32x3", WGPUVertexFormat_Float32x3, 12}, {"float32x4", WGPUVertexFormat_Float32x4, 16},
      {"uint32", WGPUVertexFormat_Uint32, 4}, {"uint32x2", WGPUVertexFormat_Uint32x2, 8},
      {"uint32x3", WGPUVertexFormat_Uint32x3, 12}, {"uint32x4", WGPUVertexFormat_Uint32x4, 16},
      {"sint32", WGPUVertexFormat_Sint32, 4}, {"sint32x2", WGPUVertexFormat_Sint32x2, 8},
      {"sint32x3", WGPUVertexFormat_Sint32x3, 12}, {"sint32x4", WGPUVertexFormat_Sint32x4, 16},
      {"unorm10-10-10-2", WGPUVertexFormat_Unorm10_10_10_2, 4}, {"snorm10-10-10-2", WGPUVertexFormat_Snorm10_10_10_2, 4},
      {"unorm8x4-bgra", WGPUVertexFormat_Unorm8x4BGRA, 4}};
  for (auto& e : formats)
    if (name == e.n) return f = e.f, size = e.size, true;
  return fail("fg_render: unknown vertex format \"" + name + "\" (float32, float32x2, uint32x4, unorm8x4, ...)");
}

API int64_t fg_render(int64_t g, int64_t shader, const char* vs, const char* fs, const char* attrs, int topology) {
  LOCK;
  Ctx* c = ctx(g);
  Obj* s = c ? get(g, shader, SHADER) : nullptr;
  if (!s) return 0;
  static const WGPUPrimitiveTopology topologies[] = {WGPUPrimitiveTopology_TriangleList, WGPUPrimitiveTopology_PointList,
                                                     WGPUPrimitiveTopology_LineList, WGPUPrimitiveTopology_TriangleStrip,
                                                     WGPUPrimitiveTopology_LineStrip};
  bool depth = topology & 8;  // 8: test and write the depth buffer (nearer fragments win)
  topology &= ~8;
  if (topology < 0 || topology > 4) return fail("fg_render: topology is 0 triangle-list, 1 point-list, 2 line-list, 3 triangle-strip, 4 line-strip"), 0;
  std::vector<WGPUVertexAttribute> va;
  uint64_t stride = 0;
  for (const char* p = attrs ? attrs : ""; *p;) {
    while (*p == ' ') p++;
    const char* q = p;
    while (*q && *q != ' ') q++;
    if (q == p) break;
    WGPUVertexAttribute a = WGPU_VERTEX_ATTRIBUTE_INIT;
    uint64_t size;
    if (!vertexFormat(std::string(p, q), a.format, size)) return 0;
    a.offset = stride, a.shaderLocation = (uint32_t)va.size(), stride += size;
    va.push_back(a), p = q;
  }
  WGPUVertexBufferLayout vb = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
  vb.stepMode = WGPUVertexStepMode_Vertex, vb.arrayStride = stride, vb.attributeCount = va.size(), vb.attributes = va.data();
  WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
  target.format = c->format;
  WGPUFragmentState frag = WGPU_FRAGMENT_STATE_INIT;
  frag.module = (WGPUShaderModule)s->p, frag.entryPoint = sv(fs), frag.targetCount = 1, frag.targets = &target;
  WGPURenderPipelineDescriptor d = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
  d.vertex.module = (WGPUShaderModule)s->p, d.vertex.entryPoint = sv(vs);
  d.vertex.bufferCount = va.empty() ? 0 : 1, d.vertex.buffers = &vb;
  d.primitive.topology = topologies[topology];
  d.fragment = &frag;
  WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
  ds.format = WGPUTextureFormat_Depth24Plus, ds.depthWriteEnabled = depth ? WGPUOptionalBool_True : WGPUOptionalBool_False;
  ds.depthCompare = depth ? WGPUCompareFunction_Less : WGPUCompareFunction_Always;
  d.depthStencil = &ds;
  WGPURenderPipeline p = nullptr;
  if (!scoped(c, [&] { return (p = wgpuDeviceCreateRenderPipeline(c->dev, &d)) != nullptr; }))
    return p ? wgpuRenderPipelineRelease(p) : void(), 0;
  return add(RENDER, g, p);
}

API int64_t fg_bind(int64_t g, int64_t pipeline, int64_t b0, int64_t b1, int64_t b2, int64_t b3, int64_t b4) {
  LOCK;
  Ctx* c = ctx(g);
  Obj* p = c ? get(g, pipeline, COMPUTE, RENDER) : nullptr;
  if (!p) return 0;
  int64_t bs[] = {b0, b1, b2, b3, b4};
  std::vector<WGPUBindGroupEntry> entries;
  for (uint32_t i = 0; i < 5 && bs[i]; i++) {
    Obj* b = get(g, bs[i], BUF);
    if (!b) return fail("fg_bind: binding " + std::to_string(i) + ": " + err), 0;
    WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
    e.binding = i, e.buffer = (WGPUBuffer)b->p, e.offset = 0, e.size = WGPU_WHOLE_SIZE;
    entries.push_back(e);
  }
  WGPUBindGroup bg = nullptr;
  bool ok = scoped(c, [&] {
    WGPUBindGroupLayout layout = p->kind == COMPUTE ? wgpuComputePipelineGetBindGroupLayout((WGPUComputePipeline)p->p, 0)
                                                    : wgpuRenderPipelineGetBindGroupLayout((WGPURenderPipeline)p->p, 0);
    WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    d.layout = layout, d.entryCount = entries.size(), d.entries = entries.data();
    bg = wgpuDeviceCreateBindGroup(c->dev, &d);
    if (layout) wgpuBindGroupLayoutRelease(layout);
    return bg != nullptr;
  });
  if (!ok) return bg ? wgpuBindGroupRelease(bg) : void(), 0;
  return add(BIND, g, bg);
}

API void fg_dispatch(int64_t g, int64_t pipeline, int64_t bind, int x, int y, int z) {
  LOCK;
  Ctx* c = ctx(g);
  Obj* p = c ? get(g, pipeline, COMPUTE) : nullptr;
  Obj* b = p && bind ? get(g, bind, BIND) : nullptr;
  if (!p || (bind && !b)) return;
  if (x < 0 || y < 0 || z < 0) return (void)fail("fg_dispatch: negative workgroup counts");
  scoped(c, [&] {
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(c->dev, nullptr);
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(enc, nullptr);
    wgpuComputePassEncoderSetPipeline(pass, (WGPUComputePipeline)p->p);
    if (b) wgpuComputePassEncoderSetBindGroup(pass, 0, (WGPUBindGroup)b->p, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(pass, x, y, z);
    wgpuComputePassEncoderEnd(pass), wgpuComputePassEncoderRelease(pass);
    return submit(c, enc);
  });
}

API int fg_frame(int64_t g, int rgb) {
  LOCK;
  Ctx* c = ctx(g);
  if (!c) return 0;
  c->clear = {((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0, 1};
  if (c->headless) return c->tex = c->target, c->texView = c->targetView, c->frame = true, c->cleared = false, 1;
  if (!pthread_main_np()) return fail("fg_frame: the window's frames must be drawn on the main thread"), 0;
  endFrame(c);  // a frame left open is shown first
  Pool pool;
  uint32_t w, h;
  viewSize(c, w, h);
  if (w != c->w || h != c->h) configure(c, w, h);  // the window was resized or moved to another screen
  WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
  for (int tries = 0; tries < 2; tries++) {
    if (st.texture) wgpuTextureRelease(st.texture), st.texture = nullptr;
    wgpuSurfaceGetCurrentTexture(c->surface, &st);
    if (st.status != WGPUSurfaceGetCurrentTextureStatus_Outdated && st.status != WGPUSurfaceGetCurrentTextureStatus_Lost) break;
    configure(c, w, h);
  }
  if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal && st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
    if (st.texture) wgpuTextureRelease(st.texture);
    return fail("fg_frame: could not get the window's next texture (status " + std::to_string((int)st.status) + ")"), 0;
  }
  c->tex = st.texture, c->texView = wgpuTextureCreateView(st.texture, nullptr);
  c->frame = true, c->cleared = false;
  return 1;
}

API void fg_draw(int64_t g, int64_t pipeline, int64_t bind, int64_t vbuf, int count) {
  LOCK;
  Ctx* c = ctx(g);
  Obj* p = c ? get(g, pipeline, RENDER) : nullptr;
  Obj* b = p && bind ? get(g, bind, BIND) : nullptr;
  Obj* v = p && (!bind || b) && vbuf ? get(g, vbuf, BUF) : nullptr;
  if (!p || (bind && !b) || (vbuf && !v)) return;
  if (!c->frame) return (void)fail("fg_draw: no frame (call fg_frame first)");
  if (count < 0) return (void)fail("fg_draw: a negative vertex count");
  if (scoped(c, [&] { return pass(c, (WGPURenderPipeline)p->p, b ? (WGPUBindGroup)b->p : nullptr, v ? (WGPUBuffer)v->p : nullptr, count); }))
    c->cleared = true;
}

API void fg_present(int64_t g) {
  LOCK;
  Ctx* c = ctx(g);
  if (!c) return;
  if (c->headless) clearFrame(c);
  else endFrame(c);
}

API int64_t fg_pixels(int64_t g, void* out) {
  LOCK;
  Ctx* c = ctx(g);
  if (!c) return -1;
  if (!c->headless) return fail("fg_pixels: only a headless context can be read back"), -1;
  if (!out) return fail("fg_pixels: nowhere to copy into"), -1;
  if (!clearFrame(c)) return -1;
  uint32_t row = c->w * 4, stride = (row + 255) / 256 * 256;  // copies take rows 256-byte aligned
  uint64_t size = (uint64_t)stride * c->h;
  WGPUBuffer st = nullptr;
  bool ok = scoped(c, [&] {
    st = staging(c, size);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(c->dev, nullptr);
    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    src.texture = c->target;
    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    dst.buffer = st, dst.layout.bytesPerRow = stride, dst.layout.rowsPerImage = c->h;
    WGPUExtent3D ext{c->w, c->h, 1};
    wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &ext);
    return submit(c, enc);
  });
  ok = ok && mapRead(c, st, size);
  if (ok) {
    auto* p = (const uint8_t*)wgpuBufferGetConstMappedRange(st, 0, size);
    if (p)
      for (uint32_t y = 0; y < c->h; y++) memcpy((uint8_t*)out + (size_t)y * row, p + (size_t)y * stride, row);
    else ok = fail("fg_pixels: the mapped buffer has no memory");
    wgpuBufferUnmap(st);
  }
  if (st) wgpuBufferRelease(st);
  return ok ? (int64_t)row * c->h : -1;
}
