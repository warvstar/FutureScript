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

var AT_Z = "
struct U \{ color: vec4f }
@group(0) @binding(0) var<uniform> u: U;
@vertex fn vs(@location(0) pos: vec3f) -> @builtin(position) vec4f \{ return vec4f(pos, 1.0); }
@fragment fn fs() -> @location(0) vec4f \{ return u.color; }"

test "depth: with depth: true, a nearer triangle drawn first stays in front" {
  var V = gpu.layout({pos: "vec3f"})
  var U = gpu.layout({color: "vec4f"})
  var near = g.uniform(U)
  near.set({color: [0, 1, 0, 1]})
  var far = g.uniform(U)
  far.set({color: [0, 0, 1, 1]})
  var tri = fn(z) { return g.vertices(V.pack([{pos: [-1, -1, z]}, {pos: [3, -1, z]}, {pos: [-1, 3, z]}], false)) }
  var a = g.render({shader: AT_Z, layout: V, bind: [near], depth: true})
  var b = g.render({shader: AT_Z, layout: V, bind: [far], depth: true})
  g.frame(0).draw(a, tri(0.2), 3).draw(b, tri(0.8), 3)
  var px = g.pixels()
  expect [px[0], px[1], px[2]] == [0, 255, 0]       // the near one, though drawn first
  var c = g.render({shader: AT_Z, layout: V, bind: [far]})  // without depth: the last draw wins, as before
  g.frame(0).draw(a, tri(0.2), 3).draw(c, tri(0.8), 3)
  px = g.pixels()
  expect [px[0], px[1], px[2]] == [0, 0, 255]
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

gpu var STEPS: list<u32> = [0x10, 0x3f, 0xff]
gpu fn ink(i: u32): u32 { return i }
gpu fn byRow(x: num, row: u32): num { return x + row }
gpu fn bits(x: num, row: num): num {  // u32s, bitwise operators and a table on the GPU
  var u: u32 = x
  return ((u << 2) ^ STEPS[row]) & 0xff
}

type Spin = {p: vec3f, w: num}
gpu fn spun(v: vec4f, a: num): vec4f {  // a matrix, a struct and v.z += on the GPU
  var c = math.cos(a)
  var s = math.sin(a)
  var m = mat3x3f(vec3f(c, s, 0), vec3f(-s, c, 0), vec3f(0, 0, 1))
  var q: Spin = {p: m * v.xyz, w: v.w}
  q = {p: q.p * m * m, w: q.w + 1}
  var r = q.p
  r.z += q.w
  return vec4f(r.x, r.y, r.z, q.w)
}

gpu fn sorted(x: num): num {  // match (a switch), vector shorthands, unpack4x8unorm, mix and smoothstep on the GPU
  var k: u32 = x
  var r = vec2f(0)
  match k {
    0 { r = vec2f(1, 0) }
    1, 2 { r = vec2f(math.mix(2, 4, x / 4), 1) }
    5..9 { r = unpack4x8unorm(k * 0x10203).xy * 8 }
    else { r = vec2f(math.smoothstep(0, 20, x), -1) }
  }
  return vec3f(r, 1).x + r.y
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
  var bs = f32s([0, 1, 7, 200])
  g.map(bits, bs, [1])
  expect bs == f32s([bits(0, 1), bits(1, 1), bits(7, 1), bits(200, 1)])
  expect bs[2] == 35
  var sp = f32s([1, 2, 3, 4, -1, 0.5, 2, 0])
  g.map(spun, sp, [0.7])
  var c0 = spun(vec4f(1, 2, 3, 4), 0.7)
  var c1 = spun(vec4f(-1, 0.5, 2, 0), 0.7)
  for i in 0..4 { expect math.abs(sp[i] - c0[i]) < 1e-5 && math.abs(sp[4 + i] - c1[i]) < 1e-5 }
  var ks = f32s(12)
  for i in 0..12 { ks[i] = i }
  g.map(sorted, ks, [])
  for i in 0..12 { expect math.abs(ks[i] - sorted(i)) < 1e-5 }
  expect sorted(0) == 1 && sorted(2) == 4
  expect math.abs(sorted(9) - (math.smoothstep(0, 20, 9) - 1)) < 1e-6  // r holds f32s
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
  expect err(fn() { g.map(ink, f32s(4), [1]) }).has("ink takes and returns num, vec2f or vec4f")
  expect err(fn() { g.map(byRow, f32s(4), [1]) }).has("byRow's parameter row is a u32: the GPU takes num and vector args")
}
