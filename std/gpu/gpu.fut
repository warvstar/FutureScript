// The GPU, through WebGPU: Dawn natively (Metal on macOS). Shaders are WGSL. Data goes to the GPU in packed buffers
// (f32s, u32s, i32s, u8s), which a layout can fill from maps.
//
//   var Vertex = gpu.layout({pos: "vec2f", color: "vec3f"})
//   window(640, 480)
//   var g = gpu.open(nil)
//   var tri = g.render({shader: wgsl, vertex: "vs", fragment: "fs", layout: Vertex})
//   var verts = g.vertices(Vertex.pack([{pos: [0, 0.5], color: [1, 0, 0]}, ...], false))
//   while present() { g.frame(0x101020).draw(tri, verts, 3) }
//
// gpu.open({headless: true, width: w, height: h}) draws into a picture of its own instead, which pixels() reads:
// for tests, and for compute alone.

extern gpu fn fg_error(): str
extern gpu fn fg_open(int, int, int): long
extern gpu fn fg_buffer(long, ptr, long, int): long
extern gpu fn fg_write(long, long, ptr, long)
extern gpu fn fg_read(long, long, ptr, long): long
extern gpu fn fg_shader(long, str): long
extern gpu fn fg_compute(long, long, str): long
extern gpu fn fg_render(long, long, str, str, str, int): long
extern gpu fn fg_bind(long, long, long, long, long, long, long): long
extern gpu fn fg_dispatch(long, long, long, int, int, int)
extern gpu fn fg_frame(long, int): int
extern gpu fn fg_draw(long, long, long, long, int)
extern gpu fn fg_present(long)
extern gpu fn fg_pixels(long, ptr): long
extern gpu fn fg_release(long)

fn check(h, what) {
  if h == 0 || h == -1 { throw "{what}: {fg_error()}" }
  return h
}
fn checked(what) {
  var e = fg_error()
  if e != "" { throw "{what}: {e}" }
}

// ---- layouts: the fields of a vertex or a struct, and where each goes ----
var SIZES = {f32: 1, vec2f: 2, vec3f: 3, vec4f: 4, mat4x4f: 16}
var ALIGNS = {f32: 1, vec2f: 2, vec3f: 4, vec4f: 4, mat4x4f: 4}  // WGSL's, in floats, for uniform and storage buffers
var FORMATS = {f32: "float32", vec2f: "float32x2", vec3f: "float32x3", vec4f: "float32x4"}

// {pos: "vec2f", color: "vec3f"}: field names in the shader's order, each an f32, vec2f, vec3f, vec4f or mat4x4f.
// As a vertex the fields are packed tight (stride floats each); as a struct in a uniform or storage buffer they
// follow WGSL's alignment (size floats each, a field at structAt).
pub class Layout {
  var names = []
  var kinds = []
  var vertexAt = []
  var structAt = []
  var stride = 0
  var size = 0
  var attrs = ""
  fn init(spec: map) {
    var align = 1
    for name in spec {
      var k = spec[name]
      if !SIZES.has(k) { throw "gpu.layout(): field '{name}' is {k}: a field is f32, vec2f, vec3f, vec4f or mat4x4f" }
      var a = ALIGNS[k]
      if a > align { align = a }
      self.size = int((self.size + a - 1) / a) * a
      self.names.push(name)
      self.kinds.push(k)
      self.vertexAt.push(self.stride)
      self.structAt.push(self.size)
      self.stride += SIZES[k]
      self.size += SIZES[k]
      if FORMATS.has(k) { self.attrs += (self.attrs == "" ? "" : " ") + FORMATS[k] }
    }
    self.size = int((self.size + align - 1) / align) * align
  }
  // items (maps) as f32s: packed tight as vertices, or aligned as structs (asStruct) for a uniform or storage buffer
  fn pack(items: list, asStruct: bool): f32s {
    var each = asStruct ? self.size : self.stride
    var out = f32s(items.len() * each)
    for i in 0..items.len() {
      var item = items[i]
      for f in 0..self.names.len() {
        var v = item[self.names[f]]
        var at = i * each + (asStruct ? self.structAt[f] : self.vertexAt[f])
        var n = SIZES[self.kinds[f]]
        if n == 1 {
          try { out[at] = v } catch e { throw "[{i}].{self.names[f]} must be a number ({self.kinds[f]}), got {v}" }
        } else {
          var ok = false
          try { ok = v.len() == n } catch e { }
          if !ok { throw "[{i}].{self.names[f]} must be a list of {n} numbers ({self.kinds[f]}), got {v}" }
          for j in 0..n { out[at + j] = v[j] }
        }
      }
    }
    return out
  }
}
pub fn layout(spec: map): Layout { return Layout(spec) }

// ---- buffers, pipelines, frames ----
pub class Buffer {
  var g
  var h = 0
  var bytes = 0
  fn init(g, data: f32s | u32s | i32s | u8s, usage: num) {
    self.g = g
    self.bytes = data.bytes()
    self.h = check(fg_buffer(g.h, data, self.bytes, usage), "gpu buffer")
  }
  // replace its contents, from the start (no more than it holds)
  fn write(data: f32s | u32s | i32s | u8s) {
    if data.bytes() > self.bytes { throw "gpu: writing {data.bytes()} bytes into a buffer of {self.bytes}" }
    fg_write(self.g.h, self.h, data, data.bytes())
    checked("gpu write")
  }
  // its contents, back from the GPU: into a new f32s, or into `into` (a buffer no bigger than this one)
  fn read(into) {
    var out = into == nil ? f32s(self.bytes / 4) : into
    check(fg_read(self.g.h, self.h, out, out.bytes()), "gpu read")
    return out
  }
  fn free() { if self.h != 0 { fg_release(self.h) self.h = 0 } }
}

// a uniform buffer holding one struct of its layout: set({field: value, ...}) writes it
pub class Uniform : Buffer {
  var layout
  fn set(values: map) { self.write(self.layout.pack([values], true)) }
}

pub class Pipeline {
  var g
  var h = 0
  var bind = 0
  var size = 64
  fn free() {
    if self.bind != 0 { fg_release(self.bind) self.bind = 0 }
    if self.h != 0 { fg_release(self.h) self.h = 0 }
  }
}

pub class Frame {
  var g
  // draw count vertices from buf (nil: the vertex shader makes them from its vertex_index)
  fn draw(p: Pipeline, buf, count: num): Frame {
    fg_draw(self.g.h, p.h, p.bind, buf == nil ? 0 : buf.h, count)
    checked("gpu draw")
    return self
  }
}

pub class Gpu {
  var h = 0
  var headless = false
  var width = 0
  var height = 0
  var shaders = {}  // WGSL text -> its module, so a reloaded program asking again gets the same one
  fn init(o: map) {
    self.headless = o.headless == true
    self.width = o.width || 256
    self.height = o.height || 256
    self.h = check(fg_open(self.width, self.height, self.headless ? 1 : 0), "gpu.open()")
  }
  fn shader(wgsl: str): num {
    if !self.shaders.has(wgsl) { self.shaders[wgsl] = check(fg_shader(self.h, wgsl), "gpu shader") }
    return self.shaders[wgsl]
  }
  // a vertex buffer: layout.pack(items, false), or numbers of your own
  fn vertices(data: f32s | u32s | i32s | u8s): Buffer { return Buffer(self, data, 1) }
  // a storage buffer, which a draw can also read as vertices: layout.pack(items, true), or numbers of your own
  fn storage(data: f32s | u32s | i32s | u8s): Buffer { return Buffer(self, data, 9) }
  // a uniform buffer holding one struct of layout
  fn uniform(layout: Layout): Uniform {
    var u = Uniform(self, f32s(layout.size < 4 ? 4 : layout.size), 4)
    u.layout = layout
    return u
  }
  // {shader: wgsl, entry: "main", bind: [buffers, in binding order], size: its @workgroup_size (64)}
  fn compute(o: map): Pipeline {
    var p = Pipeline()
    p.g = self
    p.size = o.size || 64
    p.h = check(fg_compute(self.h, self.shader(o.shader), o.entry || "main"), "gpu compute pipeline")
    p.bind = bindings(self, p, o.bind)
    return p
  }
  // {shader: wgsl, vertex: "vs", fragment: "fs", layout: its vertices' Layout (nil: no vertex buffer),
  //  bind: [buffers, in binding order], draw: "triangles" | "points" | "lines" | "strip" | "linestrip"}
  fn render(o: map): Pipeline {
    var tops = {triangles: 0, points: 1, lines: 2, strip: 3, linestrip: 4}
    var top = o.draw || "triangles"
    if !tops.has(top) { throw "gpu render pipeline: draw is triangles, points, lines, strip or linestrip, not {top}" }
    var p = Pipeline()
    p.g = self
    var attrs = o.layout == nil ? "" : o.layout.attrs
    p.h = check(fg_render(self.h, self.shader(o.shader), o.vertex || "vs", o.fragment || "fs", attrs, tops[top]), "gpu render pipeline")
    p.bind = bindings(self, p, o.bind)
    return p
  }
  // run a compute pipeline over n items: ceil(n / size) workgroups
  fn run(p: Pipeline, n: num) {
    var groups = int((n + p.size - 1) / p.size)
    if groups > 65535 { throw "gpu run(): {n} items is {groups} workgroups, more than 65535 (a bigger @workgroup_size?)" }
    fg_dispatch(self.h, p.h, p.bind, groups, 1, 1)
    checked("gpu run")
  }
  // begin a frame cleared to 0xRRGGBB; present() shows it (or pixels(), headless)
  fn frame(rgb: num): Frame {
    check(fg_frame(self.h, rgb), "gpu frame")
    var f = Frame()
    f.g = self
    return f
  }
  // headless: the frame as u8s, 4 per pixel (r, g, b, a), rows top first
  fn pixels(): u8s {
    var out = u8s(self.width * self.height * 4)
    check(fg_pixels(self.h, out), "gpu pixels")
    return out
  }
}

fn bindings(g, p, list) {
  if list == nil || list.len() == 0 { return 0 }
  if list.len() > 5 { throw "gpu: at most 5 bindings (got {list.len()})" }
  var h = [0, 0, 0, 0, 0]
  for i in 0..list.len() { h[i] = list[i].h }
  return check(fg_bind(g.h, p.h, h[0], h[1], h[2], h[3], h[4]), "gpu bind group")
}

var opened = {}
// the GPU: on the window (open it with window(w, h) first), or {headless: true, width, height} (256 x 256 by
// default). Opening it again, as a reloaded program does, gives the same one.
pub fn open(opts): Gpu {
  var o = opts || {}
  var key = "{o.headless == true} {o.width} {o.height}"
  if !opened.has(key) { opened[key] = Gpu(o) }
  return opened[key]
}
