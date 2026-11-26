// A smoke test of libfutgpu, headless: compute, render, and errors. Run by `python3 build.py gpu --check`.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

const char* fg_error(void);
int64_t fg_open(int w, int h, int headless);
int64_t fg_buffer(int64_t g, const void* data, int64_t bytes, int usage);
void fg_write(int64_t g, int64_t buf, const void* data, int64_t bytes);
int64_t fg_read(int64_t g, int64_t buf, void* out, int64_t bytes);
int64_t fg_shader(int64_t g, const char* wgsl);
int64_t fg_compute(int64_t g, int64_t shader, const char* entry);
int64_t fg_render(int64_t g, int64_t shader, const char* vs, const char* fs, const char* attrs, int topology);
int64_t fg_bind(int64_t g, int64_t pipeline, int64_t b0, int64_t b1, int64_t b2, int64_t b3, int64_t b4);
void fg_dispatch(int64_t g, int64_t pipeline, int64_t bind, int x, int y, int z);
int fg_frame(int64_t g, int rgb);
void fg_draw(int64_t g, int64_t pipeline, int64_t bind, int64_t vbuf, int count);
void fg_present(int64_t g);
int64_t fg_pixels(int64_t g, void* out);
void fg_release(int64_t h);

static int failed;
#define CHECK(cond, ...) \
  do { \
    if (!(cond)) failed++, printf("FAIL %s:%d: ", __FILE__, __LINE__), printf(__VA_ARGS__), printf("\n"); \
  } while (0)
#define NOERR(what) \
  do { \
    const char* e_ = fg_error(); \
    CHECK(!*e_, "%s: unexpected error: %s", what, e_); \
  } while (0)

static const char* doubler =
    "@group(0) @binding(0) var<storage, read_write> xs: array<f32>;\n"
    "@compute @workgroup_size(64) fn main(@builtin(global_invocation_id) id: vec3u) {\n"
    "  if (id.x < arrayLength(&xs)) { xs[id.x] = xs[id.x] * 2.0; }\n"
    "}\n";

static const char* solid =  // one triangle that covers the target, colored from a uniform
    "@group(0) @binding(0) var<uniform> color: vec4f;\n"
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {\n"
    "  let p = array(vec2f(-1, -1), vec2f(3, -1), vec2f(-1, 3));\n"
    "  return vec4f(p[i], 0, 1);\n"
    "}\n"
    "@fragment fn fs() -> @location(0) vec4f { return color; }\n";

static const char* tri =  // a vertex-buffer triangle over the left half's top-left corner
    "struct Out { @builtin(position) pos: vec4f, @location(0) color: vec3f }\n"
    "@vertex fn vs(@location(0) p: vec2f, @location(1) c: vec3f) -> Out { return Out(vec4f(p, 0, 1), c); }\n"
    "@fragment fn fs(o: Out) -> @location(0) vec4f { return vec4f(o.color, 1); }\n";

static void compute(int64_t g) {
  float xs[256], ys[256];
  for (int i = 0; i < 256; i++) xs[i] = i * 0.5f - 7;
  int64_t buf = fg_buffer(g, xs, sizeof xs, 8);
  int64_t sh = fg_shader(g, doubler);
  int64_t pipe = fg_compute(g, sh, "main");
  int64_t bind = fg_bind(g, pipe, buf, 0, 0, 0, 0);
  CHECK(buf && sh && pipe && bind, "compute setup: %s", fg_error());
  fg_dispatch(g, pipe, bind, 4, 1, 1);
  NOERR("dispatch");
  CHECK(fg_read(g, buf, ys, sizeof ys) == sizeof ys, "fg_read: %s", fg_error());
  int bad = 0;
  for (int i = 0; i < 256; i++) bad += ys[i] != xs[i] * 2;
  CHECK(!bad, "compute: %d of 256 wrong (ys[1] = %g, want %g)", bad, ys[1], xs[1] * 2);
  fg_dispatch(g, pipe, bind, 4, 1, 1);  // again: each is doubled twice now
  fg_read(g, buf, ys, sizeof ys);
  CHECK(ys[255] == xs[255] * 4, "second dispatch: %g, want %g", ys[255], xs[255] * 4);
  for (int i = 0; i < 256; i++) xs[i] = 1;  // fg_write replaces the contents
  fg_write(g, buf, xs, sizeof xs);
  fg_dispatch(g, pipe, bind, 4, 1, 1);
  fg_read(g, buf, ys, 16);
  CHECK(ys[0] == 2 && ys[3] == 2, "write then dispatch: %g %g", ys[0], ys[3]);
  int64_t odd = fg_buffer(g, NULL, 6, 8);  // zeroed, rounded up to 8
  char zs[6] = {1, 1, 1, 1, 1, 1};
  CHECK(fg_read(g, odd, zs, 6) == 6 && !zs[0] && !zs[5], "zeroed buffer");
  NOERR("compute");
  fg_release(bind), fg_release(pipe), fg_release(sh), fg_release(buf), fg_release(odd);
}

static uint32_t at(const uint8_t* px, int w, int x, int y) {  // 0xRRGGBB at (x, y), top-left origin
  const uint8_t* p = px + (y * w + x) * 4;
  return p[0] << 16 | p[1] << 8 | p[2];
}

static void render(int64_t g) {
  static uint8_t px[64 * 64 * 4];
  float color[4] = {1, 0.5f, 0, 1};
  int64_t uni = fg_buffer(g, color, sizeof color, 4);
  int64_t sh = fg_shader(g, solid);
  int64_t pipe = fg_render(g, sh, "vs", "fs", "", 0);
  int64_t bind = fg_bind(g, pipe, uni, 0, 0, 0, 0);
  CHECK(uni && sh && pipe && bind, "render setup: %s", fg_error());
  CHECK(fg_frame(g, 0x000000) == 1, "fg_frame: %s", fg_error());
  fg_draw(g, pipe, bind, 0, 3);
  NOERR("draw");
  CHECK(fg_pixels(g, px) == sizeof px, "fg_pixels: %s", fg_error());
  CHECK(at(px, 64, 32, 32) == 0xff8000, "center: %06x, want ff8000", at(px, 64, 32, 32));
  CHECK(at(px, 64, 0, 0) == 0xff8000 && at(px, 64, 63, 63) == 0xff8000, "corners: %06x %06x", at(px, 64, 0, 0), at(px, 64, 63, 63));
  CHECK(px[3] == 255, "alpha %d", px[3]);

  fg_frame(g, 0x336699);  // a frame with no draws is cleared
  CHECK(fg_pixels(g, px) == sizeof px && at(px, 64, 10, 50) == 0x336699, "clear only: %06x", at(px, 64, 10, 50));

  // a vertex buffer: one triangle (top-left half of the target, in clip space y up) over a clear
  float verts[] = {-1, 1, 0, 1, 0, 1, 1, 0, 1, 0, -1, -1, 0, 1, 0};
  int64_t vb = fg_buffer(g, verts, sizeof verts, 1);
  int64_t sh2 = fg_shader(g, tri);
  int64_t pipe2 = fg_render(g, sh2, "vs", "fs", "float32x2 float32x3", 0);
  CHECK(vb && sh2 && pipe2, "vertex pipeline: %s", fg_error());
  fg_frame(g, 0x0000ff);
  fg_draw(g, pipe2, 0, vb, 3);
  fg_draw(g, pipe, bind, 0, 0);  // a second pass loads (zero vertices: draws nothing)
  fg_pixels(g, px);
  NOERR("vertex draw");
  CHECK(at(px, 64, 2, 2) == 0x00ff00 && at(px, 64, 61, 61) == 0x0000ff, "vertex buffer: top-left %06x, bottom-right %06x",
        at(px, 64, 2, 2), at(px, 64, 61, 61));
  fg_present(g);
  NOERR("present headless");
  fg_release(vb), fg_release(sh2), fg_release(pipe2);
  fg_release(bind), fg_release(pipe), fg_release(sh), fg_release(uni);
}

static void errors(int64_t g) {
  const char* e;
  fg_error();
  CHECK(fg_shader(g, "fn main( {\n  oops\n}\n") == 0, "a syntax error compiled");
  e = fg_error();
  CHECK(strncmp(e, "1:", 2) == 0 && strstr(e, ": expected"), "syntax error not \"1:col: message\": \"%s\"", e);
  printf("  syntax error: %s\n", e);
  CHECK(fg_shader(g, "@compute @workgroup_size(1) fn main() {\n  let x: f32 = undefined_thing;\n}\n") == 0, "an unknown name compiled");
  e = fg_error();
  CHECK(e[0] == '2' && e[1] == ':', "resolver error not on line 2: \"%s\"", e);
  CHECK(!*fg_error(), "fg_error is cleared by reading it");

  float xs[64] = {0};
  int64_t storage = fg_buffer(g, xs, sizeof xs, 8), uni = fg_buffer(g, xs, 16, 4);
  int64_t sh = fg_shader(g, doubler), pipe = fg_compute(g, sh, "main");
  CHECK(fg_bind(g, pipe, uni, 0, 0, 0, 0) == 0, "a uniform buffer bound as storage was accepted");
  e = fg_error();
  CHECK(*e, "no message for a wrong binding");
  printf("  wrong buffer: %.200s\n", e);
  CHECK(fg_bind(g, pipe, sh, 0, 0, 0, 0) == 0 && *fg_error(), "a shader as a buffer");
  CHECK(fg_bind(g, pipe, 999999, 0, 0, 0, 0) == 0 && *fg_error(), "a bogus handle");
  CHECK(fg_compute(g, sh, "nope") == 0 && *fg_error(), "a missing entry point");
  int64_t freed = fg_buffer(g, NULL, 16, 8);
  fg_release(freed);
  float out[4];
  CHECK(fg_read(g, freed, out, 16) == -1 && strstr(fg_error(), "released"), "reading a released buffer");
  fg_release(freed);
  CHECK(strstr(fg_error(), "released") != NULL, "releasing twice");
  CHECK(fg_read(g, storage, out, 1 << 20) == -1 && *fg_error(), "reading past the end");
  CHECK(fg_render(g, sh, "vs", "fs", "float99", 0) == 0 && strstr(fg_error(), "float99"), "a bad vertex format");
  fg_dispatch(g, pipe, 0, 1, 1, 1);  // no bind group: a validation error at submit
  e = fg_error();
  CHECK(*e, "dispatch without its bind group was accepted");
  printf("  dispatch unbound: %.200s\n", e);
  CHECK(fg_buffer(0, NULL, 4, 8) == 0 && *fg_error(), "no context");
  CHECK(fg_pixels(g, NULL) == -1 && *fg_error(), "pixels into NULL");
  fg_release(pipe), fg_release(sh), fg_release(storage), fg_release(uni);
  NOERR("errors cleanup");
}

int main(void) {
  int64_t g = fg_open(64, 64, 1);
  if (!g) return printf("FAIL fg_open: %s\n", fg_error()), 1;
  compute(g);
  render(g);
  errors(g);
  int64_t buf = fg_buffer(g, NULL, 16, 8);
  fg_release(g);  // the context, and everything left in it
  float out[4];
  CHECK(fg_read(g, buf, out, 16) == -1 && *fg_error(), "a released context");
  fg_release(buf);
  CHECK(*fg_error(), "a buffer of a released context");
  int64_t g2 = fg_open(16, 8, 1);  // and again
  CHECK(g2 && fg_frame(g2, 0x010203) && fg_pixels(g2, (uint8_t[16 * 8 * 4]){0}) == 16 * 8 * 4, "reopen: %s", fg_error());
  CHECK(fg_open(10, 10, 0) == 0 && strstr(fg_error(), "window("), "window mode without a window");
  fg_release(g2);
  printf(failed ? "futgpu: %d failed\n" : "futgpu: all checks passed\n", failed);
  return failed != 0;
}
