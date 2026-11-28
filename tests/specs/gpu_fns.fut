// gpu fn: a function that also runs on the GPU. On the CPU it is an ordinary function; its WGSL (wgsl(f)) is what
// std/gpu runs. Its values are numbers (f32 on the GPU), u32 whole numbers, bools and vectors: vec2f, vec3f, vec4f,
// which on the CPU are f32s of 2, 3 and 4. It reads its file's gpu vars: numbers, and tables of them, that never change.
import "std/math" as math
fn err(f) { try { f() } catch e { return "{e}" } return "" }
fn compileErr(src) { return run({source: src}).err }

test "vectors: made, read, swizzled, and computed elementwise" {
  var a = vec2f(1, 2)
  var b = vec2f(3, 5)
  expect a + b == vec2f(4, 7)  // (a top-level == compares contents; inside && it asks: the same object?)
  expect a * 2 == vec2f(2, 4)
  expect 1 - a == vec2f(0, -1)
  expect -b == vec2f(-3, -5)
  expect b % 2 == vec2f(1, 1)
  expect b / a == vec2f(3, 2.5)
  expect a.x == 1 && a.y == 2
  expect a.yx == vec2f(2, 1)
  var c = vec4f(1, 2, 3, 4)
  expect c.rgb == vec3f(1, 2, 3)
  expect c.wzyx == vec4f(4, 3, 2, 1)
  expect c.a == 4
  expect vec3f(3, 0, 4).length() == 5 && a.dot(b) == 13
  expect vec2f(3, 4).normalize() == vec2f(0.6, 0.8)
  expect vec3f(1, 0, 0).cross(vec3f(0, 1, 0)) == vec3f(0, 0, 1)
  expect err(fn() { a.z }).has(".z of an f32s of 2")
  expect err(fn() { a + c }).has("f32s of different lengths: 2 and 4")
  expect err(fn() { a.q }).has("has no field 'q'")
}

test "vec2f, vec3f and vec4f are types: an f32s of that length" {
  var f = fn(v: vec3f): vec3f { return v * 2 }
  expect f(vec3f(1, 2, 3)) == vec3f(2, 4, 6)
  expect err(fn() { f(vec2f(1, 2)) }).has("must be vec3f")
  expect err(fn() { f(f32s(3)) }) == ""
}

gpu fn wave(x: num, t: num): num {
  var y = math.sin(x * 2 + t) ** 2
  for i in 0..3 { y += i % 2 }
  return x > 0 ? y : -y
}
gpu fn shade(uv: vec2f, t: num): vec4f {
  var c = uv * 0.5 + vec2f(wave(uv.x, t), 0)
  if c.length() > 1 { c = c.normalize() } else if c.x < 0 { c = -c }
  return vec4f(c.x, c.y, 0, 1)
}

test "a gpu fn runs on the CPU like any fn" {
  expect wave(0.5, 0) > 1.7 && wave(-0.5, 0) < -1.7
  expect shade(vec2f(0, 0), 0) == vec4f(1, 0, 0, 1)  // wave(0, 0) is -1, and c = -c makes it 1
}

test "wgsl(f): its WGSL, with the gpu fns it calls, and its signature" {
  var w = wgsl(shade)
  expect w.entry == "f_shade" && w.result == "vec4f"
  expect w.names == ["uv", "t"]
  expect w.types == ["vec2f", "f32"]
  expect w.wgsl.has("fn f_shade(p_uv: vec2f, p_t: f32) -> vec4f \{")
  expect w.wgsl.has("fn f_wave(p_x: f32, p_t: f32) -> f32 \{")
  expect w.wgsl.has("pow(sin(((l_x * 2.0) + l_t)), 2.0)")                  // ** is pow; numbers are f32
  expect w.wgsl.has("select((-l_y), l_y, (l_x > 0.0))")                   // ?: is select
  expect w.wgsl.has("(l_i - 2.0 * floor(l_i / 2.0))")                     // % keeps the divisor's sign, as fut's does
  expect w.wgsl.has("normalize(l_c)") && w.wgsl.has("length(l_c)")       // vector methods are WGSL's builtins
  expect err(fn() { wgsl(err) }).has("err is not a gpu fn")
}

test "what the GPU doesn't have is a compile error, saying what to write" {
  expect compileErr("gpu fn f(x): num \{ return x }").has("give each parameter of a gpu fn a type: x: num [gpu]")
  expect compileErr("var K = 2\ngpu fn f(x: num): num \{ return x * K }").has("pass K in as a parameter")
  expect compileErr("gpu fn f(x: num): num \{ print(x) return x }").has("a gpu fn's statements are var, assignment, if")
  expect compileErr("gpu fn f(x: num): num \{ return f(x) }").has("can't call itself")
  expect compileErr("gpu fn f(v: vec2f): vec2f \{ v.x = 1 return v }").has("vectors don't change in place")
  expect compileErr("gpu fn f(x: num): str \{ return \"\" }").has("not str")
  expect compileErr("fn h(x) \{ return x }\ngpu fn f(x: num): num \{ return h(x) }").has("h is not a gpu fn of this file")
  expect compileErr("import \"std/math\" as math\ngpu fn f(x: num): num \{ return math.hypot(x, x) }").has("not math.hypot")
}

test "u32 is a type: a whole number from 0 to 2^32 - 1" {
  var f = fn(x: u32): u32 { return x }
  expect f(0) == 0 && f(4294967295) == 4294967295
  expect err(fn() { f(-1) }).has("must be u32")
  expect err(fn() { f(1.5) }).has("must be u32")
  expect err(fn() { f(4294967296) }).has("must be u32")
}

// Two rows of eight 4-bit inks, as a sprite or a pattern stores them
gpu var INKS: list<list<u32>> = [
  [0x12345678, 0x9abcdef0],
  [0x00000000, 0xffffffff]
]
gpu var MASK: u32 = 15
gpu var WEIGHTS = [0.25, 0.5]  // an f32 table
gpu fn ink(row: u32, i: u32): u32 {
  return (INKS[row][i >> 3] >> ((7 - (i & 7)) * 4)) & MASK
}
gpu fn inkShade(row: u32, i: u32): num {
  var k = ink(row, i)            // a u32 local
  var w = WEIGHTS[i % 2]         // an f32 from a table
  return k == 15 ? 1 : k * w     // u32 * f32 is f32
}

test "u32s and gpu vars: bits on the CPU, as on the GPU" {
  expect ink(0, 0) == 1 && ink(0, 7) == 8 && ink(0, 8) == 9 && ink(0, 15) == 0 && ink(1, 9) == 15
  expect inkShade(0, 2) == 0.75 && inkShade(0, 3) == 2 && inkShade(1, 8) == 1
  expect err(fn() { ink(2, 0) }).has("index")       // a table's rows are 0..len-1, as a list's
  expect err(fn() { INKS[0][0] = 1 }).has("frozen")  // a gpu var never changes
  var w = wgsl(inkShade)
  expect w.types == ["u32", "u32"]
  expect w.result == "f32"
  expect w.wgsl.has("const g_INKS: array<array<u32, 2>, 2> = array<array<u32, 2>, 2>(array<u32, 2>(305419896u, 2596069104u), array<u32, 2>(0u, 4294967295u));")
  expect w.wgsl.has("const g_MASK: u32 = 15u;") && w.wgsl.has("const g_WEIGHTS: array<f32, 2> = array<f32, 2>(0.25, 0.5);")
  expect w.wgsl.has("(g_INKS[l_row][(l_i >> 3u)] >> u32(((7.0 - f32((l_i & 7u))) * 4.0)))")  // - leaves u32s for f32, as fut's numbers
  expect w.wgsl.has("var l_k: u32 = f_ink(l_row, l_i);") && w.wgsl.has("var l_w: f32 = g_WEIGHTS[(l_i % 2u)];")
  expect w.wgsl.has("select((f32(l_k) * l_w), 1.0, (l_k == 15u))")
  expect wgsl(ink).wgsl.has("fn f_ink(p_row: u32, p_i: u32) -> u32 \{")
}

test "u32s and gpu vars: what doesn't mean the same on both is a compile error" {
  expect compileErr("gpu var T = [1, 2]\nfn f() \{ T = [3] }").has("T is a gpu var: it never changes")
  expect compileErr("gpu var T = [1, 2]\ngpu fn f(i: u32): num \{ T = 1 return 0 }").has("T is a gpu var: it never changes")
  expect compileErr("gpu var T = [[1, 2], [3]]\ngpu fn f(i: u32): num \{ return T[i][0] }").has("lists at one depth are one length")
  expect compileErr("gpu var T = [[1, 2], 3]\ngpu fn f(i: u32): num \{ return T[i][0] }").has("numbers or lists, the same at every depth")
  expect compileErr("gpu var T: list<u32> = [1, 0.5]\ngpu fn f(i: u32): u32 \{ return T[i] }").has("0.5 is not a u32")
  expect compileErr("gpu var T: list<u32> = [1, 2]\ngpu fn f(i: u32): u32 \{ return T }").has("T is a table: index it (T[i])")
  expect compileErr("gpu var T = [1, 2]\ngpu fn f(i: u32): num \{ return T[i][0] }").has("T has 1 dimension")
  expect compileErr("gpu var T = len([1])\ngpu fn f(): num \{ return T }").has("written out")
  expect compileErr("gpu fn f(i: u32): u32 \{ var k = i  k = 0.5  return k }").has("k holds a u32")
  expect compileErr("gpu fn f(i: u32): u32 \{ return -1 }").has("-1.0 is not a u32")
  expect compileErr("gpu fn f(x: num): num \{ if x \{ return 1 } return 0 }").has("conditions are bools")
  expect compileErr("gpu fn f(a: vec2f, b: vec2f): bool \{ return a == b }").has("compare their fields")
  expect compileErr("gpu fn f(x: num): num \{ return x.y }").has("f32 has no .y")
}
