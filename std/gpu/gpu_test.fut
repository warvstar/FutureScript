// std/gpu on a real GPU, headless: compute, a draw read back as pixels, layouts, and errors that name what is wrong.
import "./gpu.fut" as gpu

fn err(f) { try { f() } catch e { return "{e}" } return "" }
var g = gpu.open({headless: true, width: 64, height: 64})

var DOUBLE = "
@group(0) @binding(0) var<storage, read_write> xs: array<f32>;
@compute @workgroup_size(64) fn main(@builtin(global_invocation_id) id: vec3u) \{
  if (id.x < arrayLength(&xs)) \{ xs[id.x] = xs[id.x] * 2.0; }
}"

test "compute: a shader doubles every number" {
  var xs = f32s(1000)
  for i in 0..1000 { xs[i] = i }
  var buf = g.storage(xs)
  var p = g.compute({shader: DOUBLE, bind: [buf]})
  g.run(p, 1000)
  var out = buf.read(nil)
  expect out.len() == 1000
  expect out[0] == 0 && out[1] == 2 && out[999] == 1998
  buf.free()
}

var SOLID = "
struct U \{ color: vec4f }
@group(0) @binding(0) var<uniform> u: U;
@vertex fn vs(@location(0) pos: vec2f) -> @builtin(position) vec4f \{ return vec4f(pos, 0.0, 1.0); }
@fragment fn fs() -> @location(0) vec4f \{ return u.color; }"

test "render: a triangle in a uniform's color, read back as pixels" {
  var V = gpu.layout({pos: "vec2f"})
  var U = gpu.layout({color: "vec4f"})
  var u = g.uniform(U)
  u.set({color: [0, 1, 0, 1]})
  var p = g.render({shader: SOLID, layout: V, bind: [u]})
  var verts = g.vertices(V.pack([{pos: [-1, -1]}, {pos: [1, -1]}, {pos: [-1, 1]}], false))  // the lower-left half
  g.frame(0xff0000).draw(p, verts, 3)
  var px = g.pixels()
  expect px.len() == 64 * 64 * 4
  var at = fn(x, y) { var i = (y * 64 + x) * 4 return [px[i], px[i + 1], px[i + 2], px[i + 3]] }
  expect at(2, 61) == [0, 255, 0, 255]   // inside: the uniform's green
  expect at(61, 2) == [255, 0, 0, 255]   // outside: the frame's red
}

test "layouts: vertices packed tight, structs aligned as WGSL lays them out" {
  var L = gpu.layout({pos: "vec2f", color: "vec3f", w: "f32"})
  expect L.stride == 6 && L.size == 8 && L.attrs == "float32x2 float32x3 float32"
  expect L.structAt == [0, 4, 7]  // a vec3f starts on 4 floats, and the f32 after it fills its fourth
  expect L.pack([{pos: [1, 2], color: [3, 4, 5], w: 6}], false) == f32s([1, 2, 3, 4, 5, 6])
  expect L.pack([{pos: [1, 2], color: [3, 4, 5], w: 6}], true) == f32s([1, 2, 0, 0, 3, 4, 5, 6])
  expect gpu.layout({a: "f32", b: "vec4f"}).size == 8
  expect err(fn() { L.pack([{pos: [1], color: [3, 4, 5], w: 6}], false) }).has("[0].pos must be a list of 2 numbers (vec2f)")
  expect err(fn() { gpu.layout({p: "vec9"}) }).has("field 'p' is vec9")
}

test "errors name the problem instead of crashing" {
  expect err(fn() { g.shader("fn oops( \{") }).has("gpu shader:")
  var buf = g.storage(f32s(4))
  expect err(fn() { g.compute({shader: DOUBLE, entry: "nope"}) }).has("gpu compute pipeline")
  expect err(fn() { buf.write(f32s(5)) }).has("writing 20 bytes into a buffer of 16")
}

// gpu fns: the same function on the CPU and, through map and paint, on the GPU
import "std/math" as math
gpu fn scale(x: num, k: num, add: num): num { return x * k + add }
gpu fn spin(v: vec2f, a: num): vec2f {
  var c = math.cos(a)
  var s = math.sin(a)
  return vec2f(v.x * c - v.y * s, v.x * s + v.y * c)
}
gpu fn collatz(x: num): num {  // loops, ifs and % on the GPU
  var n = x
  var steps = 0
  while n != 1 {
    if n % 2 == 0 { n = n / 2 } else { n = 3 * n + 1 }
    steps += 1
  }
  return steps
}
gpu fn bands(uv: vec2f, color: vec3f, rows: num): vec4f {
  var row = floor(uv.y * rows)
  return row % 2 == 0 ? vec4f(color.x, color.y, color.z, 1) : vec4f(0, 0, 0, 1)
}

test "map: a gpu fn over each element, on the GPU, as on the CPU" {
  var xs = f32s([1, 2, 3, 4])
  g.map(scale, xs, [3, 0.5])
  expect xs == f32s([scale(1, 3, 0.5), scale(2, 3, 0.5), scale(3, 3, 0.5), scale(4, 3, 0.5)])
  var ns = f32s(30)
  for i in 0..30 { ns[i] = i + 1 }
  g.map(collatz, ns, [])
  expect ns[0] == collatz(1) && ns[26] == collatz(27) && ns[26] == 111
  var vs = f32s([1, 0, 0, 1])                 // two vec2fs
  var buf = g.storage(vs)                     // a Buffer stays on the GPU between maps
  g.map(spin, buf, [math.pi / 4])
  g.map(spin, buf, [math.pi / 4])
  var out = buf.read(nil)
  var cpu = spin(spin(vec2f(1, 0), math.pi / 4), math.pi / 4)
  expect math.abs(out[0] - cpu.x) < 1e-6 && math.abs(out[1] - cpu.y) < 1e-6 && math.abs(out[1] - 1) < 1e-6
  buf.free()
}

test "paint: a gpu fn colors each pixel" {
  g.frame(0).paint(bands, [vec3f(1, 0.5, 0), 4])
  var px = g.pixels()
  var at = fn(x, y) { var i = (y * 64 + x) * 4 return [px[i], px[i + 1], px[i + 2], px[i + 3]] }
  expect at(10, 2) == [255, 128, 0, 255]  // row band 0
  expect at(10, 20) == [0, 0, 0, 255]     // band 1
  expect at(50, 40) == [255, 128, 0, 255] // band 2
  var c = bands(vec2f(10.5 / 64, 2.5 / 64), vec3f(1, 0.5, 0), 4) * 255
  expect c == vec4f(255, 127.5, 0, 255)   // the CPU agrees (the GPU rounds 127.5 to 128)
}

test "map and paint say what is wrong with a gpu fn's shape or args" {
  expect err(fn() { g.map(bands, f32s(4), []) }).has("bands takes and returns num, vec2f or vec4f")
  expect err(fn() { g.map(scale, f32s(4), [1]) }).has("scale takes 2 args after its first parameter, got 1")
  expect err(fn() { g.frame(0).paint(scale, [1, 2]) }).has("takes uv: vec2f")
  expect err(fn() { g.map(err, f32s(4), []) }).has("err is not a gpu fn")
}
