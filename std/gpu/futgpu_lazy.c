// libfutgpu.dylib: what std/gpu binds. Each fg_* forwards to libfutgpu_dawn.dylib (futgpu.cpp over Dawn), which is
// loaded on the first call, not when the program is compiled. Loading Dawn starts Metal's threads and initializes
// Objective-C classes, and a process that has done that can't safely fork: `fut test` forks for each test, so the
// children, not the parent, must be the first to touch the GPU. This file links nothing but libSystem.
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define API __attribute__((visibility("default")))
static void* lib;
static char why[1200];

static void* load(void) {
  if (lib) return lib;
  Dl_info me;
  char path[1024];
  if (!dladdr((void*)load, &me) || !me.dli_fname) return snprintf(why, sizeof why, "cannot find libfutgpu.dylib's directory"), NULL;
  snprintf(path, sizeof path, "%s", me.dli_fname);
  char* slash = strrchr(path, '/');
  snprintf(slash ? slash + 1 : path, sizeof path - (slash ? (size_t)(slash + 1 - path) : 0), "libfutgpu_dawn.dylib");
  if (!(lib = dlopen(path, RTLD_NOW | RTLD_LOCAL))) snprintf(why, sizeof why, "cannot load %s (python3 build.py gpu builds it): %s", path, dlerror());
  return lib;
}
static void* fn(const char* name) {
  void* l = load();
  void* f = l ? dlsym(l, name) : NULL;
  if (l && !f) snprintf(why, sizeof why, "libfutgpu_dawn.dylib has no %s", name);
  return f;
}
#define FORWARD(R, FAIL, name, params, args)            \
  API R name params {                                   \
    static R(*f) params;                                \
    if (!f && !(f = (R(*) params)fn(#name))) return FAIL; \
    return f args;                                      \
  }
#define FORWARD_VOID(name, params, args)               \
  API void name params {                                \
    static void(*f) params;                             \
    if (!f && !(f = (void(*) params)fn(#name))) return; \
    f args;                                             \
  }

API const char* fg_error(void) {
  static const char* (*f)(void);
  if (!f && !(f = (const char* (*)(void))fn("fg_error"))) {
    static char last[sizeof why];
    memcpy(last, why, sizeof why), why[0] = 0;  // read once, like the library's own errors
    return last;
  }
  return f();
}
FORWARD(int64_t, 0, fg_open, (int w, int h, int headless), (w, h, headless))
FORWARD(int64_t, 0, fg_buffer, (int64_t g, const void* data, int64_t bytes, int usage), (g, data, bytes, usage))
FORWARD_VOID(fg_write, (int64_t g, int64_t buf, const void* data, int64_t bytes), (g, buf, data, bytes))
FORWARD(int64_t, -1, fg_read, (int64_t g, int64_t buf, void* out, int64_t bytes), (g, buf, out, bytes))
FORWARD(int64_t, 0, fg_shader, (int64_t g, const char* wgsl), (g, wgsl))
FORWARD(int64_t, 0, fg_compute, (int64_t g, int64_t shader, const char* entry), (g, shader, entry))
FORWARD(int64_t, 0, fg_render, (int64_t g, int64_t shader, const char* vs, const char* fs, const char* attrs, int topology),
        (g, shader, vs, fs, attrs, topology))
FORWARD(int64_t, 0, fg_bind, (int64_t g, int64_t p, int64_t b0, int64_t b1, int64_t b2, int64_t b3, int64_t b4), (g, p, b0, b1, b2, b3, b4))
FORWARD_VOID(fg_dispatch, (int64_t g, int64_t p, int64_t bind, int x, int y, int z), (g, p, bind, x, y, z))
FORWARD(int, 0, fg_frame, (int64_t g, int rgb), (g, rgb))
FORWARD_VOID(fg_draw, (int64_t g, int64_t p, int64_t bind, int64_t vbuf, int count), (g, p, bind, vbuf, count))
FORWARD_VOID(fg_present, (int64_t g), (g))
FORWARD(int64_t, -1, fg_pixels, (int64_t g, void* out), (g, out))
FORWARD_VOID(fg_release, (int64_t h), (h))
