// std/gpu on a real GPU, headless: compute, a draw read back as pixels, layouts, and errors that name what is wrong.
import "std/gpu"

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
  expect L.stride == 6 && L.size == 12 && L.structAt == [0, 4, 7] && L.attrs == "float32x2 float32x3 float32"
  expect L.pack([{pos: [1, 2], color: [3, 4, 5], w: 6}], false) == f32s([1, 2, 3, 4, 5, 6])
  expect L.pack([{pos: [1, 2], color: [3, 4, 5], w: 6}], true) == f32s([1, 2, 0, 0, 3, 4, 5, 6, 0, 0, 0, 0])
  expect err(fn() { L.pack([{pos: [1], color: [3, 4, 5], w: 6}], false) }).has("[0].pos must be a list of 2 numbers (vec2f)")
  expect err(fn() { gpu.layout({p: "vec9"}) }).has("field 'p' is vec9")
}

test "errors name the problem instead of crashing" {
  expect err(fn() { g.shader("fn oops( \{") }).has("gpu shader:")
  var buf = g.storage(f32s(4))
  expect err(fn() { g.compute({shader: DOUBLE, entry: "nope"}) }).has("gpu compute pipeline")
  expect err(fn() { buf.write(f32s(5)) }).has("writing 20 bytes into a buffer of 16")
}
