// Graphics: one window showing a pixel buffer. Drawing is plain memory writes; present() shows the frame and
// handles the window's events. Natively AppKit is reached through the Objective-C runtime and loaded on first use,
// so programs that never open a window pay nothing for it; in wasm the host draws the buffer on a canvas.
#include "fut.h"
#include <algorithm>
#include <time.h>
struct Pt { double x, y; };
static std::vector<u32> px;  // 0x00RRGGBB, top row first
static int W, H;
static bool down[0x10000], mouseDown;  // keys by (lowercased) character; arrows are 0xF700..0xF703
static Pt mouse;
#define NUM(v, f) (isNum(v) ? num(v) : (ERR(f "() expects numbers"), 0.0))
static int clip(double v, int hi) { return v > 0 ? (v < hi ? (int)v : hi) : 0; }  // NaN -> 0
#define COLOR(c, f) (isNum(c) ? clip(num(c), 0xffffff) : (ERR(f "() expects a 0xRRGGBB color"), 0))
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

#if __wasm__
extern "C" __attribute__((import_module("host"), import_name("window"))) int host_window(int w, int h);  // false: no display
extern "C" __attribute__((import_module("host"), import_name("present"))) int host_present(const u32* px, int w, int h);  // suspends a frame
extern "C" __attribute__((export_name("fut_key"))) void futKey(int c, int isDown) { down[lower(c) & 0xffff] = isDown; }
extern "C" __attribute__((export_name("fut_mouse"))) void futMouse(double x, double y, int isDown) { mouse = {x, y}, mouseDown = isDown; }
static V n_window(V w, V h) {
  W = clip(NUM(w, "window"), 4096), H = clip(NUM(h, "window"), 4096);
  if (!host_window(W, H)) ERR("window() needs a display: run this in a browser (node web/serve.mjs)");
  return px.assign((size_t)W * H, 0), NIL;
}
static V n_present() {
  if (px.empty()) ERR("present() before window()");
  return fflush(stdout), boxb(host_present(px.data(), W, H));
}
void gfxPump() {}
#else
#include <dlfcn.h>
extern "C" void *objc_msgSend(), *objc_getClass(const char*), *sel_registerName(const char*);  // the Objective-C runtime
using id = void*;
struct Rect { Pt o, s; };
template <class R = id, class... A> R msg(id o, const char* sel, A... a) { return ((R(*)(id, void*, A...))(void*)objc_msgSend)(o, sel_registerName(sel), a...); }
static id cls(const char* name) { return objc_getClass(name); }
static id nsstr(const char* s) { return msg(cls("NSString"), "stringWithUTF8String:", s); }
template <class F> F cg(const char* name) { return (F)dlsym(RTLD_DEFAULT, name); }  // CoreGraphics, without its headers

static id app, win, layer;
static void* ctx;

static V n_window(V w, V h) {
  W = clip(NUM(w, "window"), 4096), H = clip(NUM(h, "window"), 4096);
  if (!win) {
    dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_LAZY);
    app = msg(cls("NSApplication"), "sharedApplication");
    msg<void>(app, "setActivationPolicy:", 0l), msg<void>(app, "finishLaunching");
    win = msg(msg(cls("NSWindow"), "alloc"), "initWithContentRect:styleMask:backing:defer:", Rect{{200, 200}, {(double)W, (double)H}}, 7ul, 2ul, false);
    msg<void>(win, "setReleasedWhenClosed:", false), msg<void>(win, "setTitle:", nsstr("fut"));
    id view = msg(win, "contentView");
    msg<void>(view, "setWantsLayer:", true);
    layer = msg(view, "layer");
    msg<void>(layer, "setMagnificationFilter:", nsstr("nearest"));  // big crisp pixels when the window is scaled
    msg<void>(win, "makeKeyAndOrderFront:", (id) nullptr), msg<void>(app, "activateIgnoringOtherApps:", true);
  } else msg<void>(win, "setContentSize:", Pt{(double)W, (double)H}), cg<void (*)(void*)>("CGContextRelease")(ctx);
  px.assign((size_t)W * H, 0);
  ctx = cg<void* (*)(void*, size_t, size_t, size_t, size_t, void*, u32)>("CGBitmapContextCreate")(
      px.data(), W, H, 8, W * 4, cg<void* (*)()>("CGColorSpaceCreateDeviceRGB")(), 0x2006);  // 32-bit little-endian, no alpha
  return NIL;
}
void gfxPump() {  // handle pending window events (also called while a stopped program waits for a fix)
  if (!win) return;
  static id mode = nsstr("kCFRunLoopDefaultMode");
  for (id e; (e = msg(app, "nextEventMatchingMask:untilDate:inMode:dequeue:", ~0ull, (id) nullptr, mode, true));) {
    unsigned long t = msg<unsigned long>(e, "type");
    if (t == 10 || t == 11) {  // key down/up: recorded, not passed on (AppKit would beep)
      id s = msg(e, "charactersIgnoringModifiers");
      if (msg<unsigned long>(s, "length")) down[lower(msg<uint16_t>(s, "characterAtIndex:", 0ul))] = t == 10;
      continue;
    }
    if (t == 1 || t == 2) mouseDown = t == 1;
    msg<void>(app, "sendEvent:", e);
  }
  mouse = msg<Pt>(win, "mouseLocationOutsideOfEventStream"), mouse.y = H - mouse.y;
}
static V n_present() {  // show the frame, handle events, wait for the next 1/60 s tick; false once the window is closed
  if (!win) ERR("present() before window()");
  void* img = cg<void* (*)(void*)>("CGBitmapContextCreateImage")(ctx);
  msg<void>(layer, "setContents:", (id)img), cg<void (*)(void*)>("CGImageRelease")(img);
  msg<void>(cls("CATransaction"), "flush"), fflush(stdout), gfxPump();
  static timespec next;
  timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if ((next.tv_nsec += 16666667) >= 1000000000) next.tv_sec++, next.tv_nsec -= 1000000000;
  timespec wait{next.tv_sec - now.tv_sec, next.tv_nsec - now.tv_nsec};
  if (wait.tv_nsec < 0) wait.tv_sec--, wait.tv_nsec += 1000000000;
  if (wait.tv_sec < 0) next = now;  // fell behind: start counting from here
  else nanosleep(&wait, nullptr);
  return boxb(msg<bool>(win, "isVisible"));
}
#endif

static V n_clear(V c) { return std::fill(px.begin(), px.end(), COLOR(c, "clear")), NIL; }
static V n_rect(V x, V y, V w, V h, V c) {
  int x0 = clip(NUM(x, "rect"), W), y0 = clip(NUM(y, "rect"), H), x1 = clip(num(x) + NUM(w, "rect"), W), y1 = clip(num(y) + NUM(h, "rect"), H);
  for (int j = y0, k = COLOR(c, "rect"); j < y1; j++) std::fill(px.data() + j * W + x0, px.data() + j * W + x1, k);
  return NIL;
}
static V n_circle(V x, V y, V r, V c) {
  double cx = NUM(x, "circle"), cy = NUM(y, "circle"), rr = NUM(r, "circle");
  for (int j = clip(cy - rr, H), k = COLOR(c, "circle"); j < clip(cy + rr + 1, H); j++)
    for (int i = clip(cx - rr, W); i < clip(cx + rr + 1, W); i++)
      if ((i + 0.5 - cx) * (i + 0.5 - cx) + (j + 0.5 - cy) * (j + 0.5 - cy) <= rr * rr) px[j * W + i] = k;
  return NIL;
}
static V n_key(V k) {
  if (!is(k, K::Str)) ERR("key() expects a name like \"a\", \"space\" or \"left\"");
  std::string_view s = as<Str>(k)->sv();
  static const std::pair<std::string_view, int> named[] = {{"up", 0xF700}, {"down", 0xF701}, {"left", 0xF702}, {"right", 0xF703},
                                                           {"space", ' '}, {"enter", '\r'}, {"escape", 27}, {"tab", '\t'}};
  int c = s.size() == 1 ? lower((uint8_t)s[0]) : -1;
  for (auto [n, v] : named) c = s == n ? v : c;
  return boxb(c >= 0 && down[c]);
}

void gfxInit() {
  native<n_window>("window"), native<n_present>("present"), native<n_clear>("clear");
  native<n_rect>("rect"), native<n_circle>("circle"), native<n_key>("key");
  native<+[] { return box(mouse.x); }>("mousex"), native<+[] { return box(mouse.y); }>("mousey");
  native<+[] { return boxb(mouseDown); }>("mousedown");
}
