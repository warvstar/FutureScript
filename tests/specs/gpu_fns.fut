// gpu fn: a function that also runs on the GPU. On the CPU it is an ordinary function; its WGSL (wgsl(f)) is what
// std/gpu runs. Its values are numbers (f32 on the GPU), bools and vectors: vec2f, vec3f, vec4f, which on the CPU are
// f32s of 2, 3 and 4.
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
  expect compileErr("gpu fn f(x: num): num \{ return x & 1 }").has("no bitwise operators")
  expect compileErr("gpu fn f(x: num): str \{ return \"\" }").has("not str")
  expect compileErr("fn h(x) \{ return x }\ngpu fn f(x: num): num \{ return h(x) }").has("h is not a gpu fn of this file")
  expect compileErr("import \"std/math\" as math\ngpu fn f(x: num): num \{ return math.hypot(x, x) }").has("not math.hypot")
}
