// The same program computes the same bits on every platform: std/math is the runtime's own (not libm or JavaScript's
// Math), no multiply-add is fused, and seed() fixes random(). These fingerprints are every bit of thousands of
// results; the native and wasm suites both run this file, so both must produce exactly these numbers.
import "std/math"

fn mant(x) {  // x's exact bits as [exponent, 53-bit mantissa]: halving and doubling are exact
  if x != x { return [9999, 0] }
  if x == 0 { return [0, 0] }
  var a = x
  var s = 1
  if a < 0 { a = -a s = -1 }
  if a > 1e308 * 10 { return [s * 8888, 0] }
  var e = 0
  while a >= 2 { a = a / 2 e += 1 }
  while a < 1 { a = a * 2 e -= 1 }
  return [s * (e + 2000), a * 4503599627370496]
}
var P = 1000000007
fn mix(h, x) {
  var m = mant(x)
  return (h * 31 + m[1] % P + m[0] + 3000) % P
}
fn fingerprint(f, n) {
  var h = 7
  for i in 0..n { h = mix(h, f(i)) }
  return h
}

test "math: every bit of thousands of results" {
  var x = fn(i) { return (i - 2500) * 0.0123456789 + i * 1e-9 }
  expect fingerprint(fn(i) { return math.sin(x(i) * 100) }, 5000) == 417066743
  expect fingerprint(fn(i) { return math.cos(x(i) * 1e7) }, 5000) == 326710294
  expect fingerprint(fn(i) { return math.tan(x(i)) }, 5000) == 291223666
  expect fingerprint(fn(i) { return math.exp(x(i) * 20) }, 5000) == 394584554
  expect fingerprint(fn(i) { return math.log(i * 0.37 + 1e-3) }, 5000) == 298380439
  expect fingerprint(fn(i) { return math.log2(i * 3.7 + 1e-3) + math.log10(i * 0.11 + 2) }, 5000) == 750132313
  expect fingerprint(fn(i) { return math.pow(i * 0.013 + 0.5, x(i)) }, 5000) == 665319785
  expect fingerprint(fn(i) { return math.atan2(x(i), 1 - x(i) * x(i)) + math.atan(x(i) * 9) }, 5000) == 176525685
  expect fingerprint(fn(i) { return math.asin(x(i) / 31) + math.acos(x(i) / 33) }, 5000) == 987101440
  expect fingerprint(fn(i) { return math.cbrt(x(i) * 1e5) + math.hypot(x(i), i) }, 5000) == 806000574
  expect fingerprint(fn(i) { return sqrt(i * 1.1) + floor(x(i)) + (x(i) % 0.7) }, 5000) == 366241205
}

test "exact where exact is representable" {
  expect [math.pow(10, 3), math.pow(2, -3), math.log2(1024), math.log10(1e6), math.cbrt(-27), math.hypot(3, 4)] == [1000, 0.125, 10, 6, -3, 5]
  expect [math.sin(0), math.cos(0), math.log(1), math.exp(0), math.atan2(0, -1)] == [0, 1, 0, 1, math.pi]
}

test "a chaotic loop: any difference in any bit would grow until it showed" {
  var v = 0.123456789
  var w = 0.5
  for i in 0..100000 {
    v = 3.99 * v * (1 - v)
    w = math.sin(w * 3 + v) * 0.9 + math.exp(-v) * 0.1
  }
  expect mix(mix(7, v), w) == 373963793
}

test "seed fixes random()" {
  seed(42)
  var a = fingerprint(fn(i) { return random() }, 1000)
  seed(42)
  expect fingerprint(fn(i) { return random() }, 1000) == a
  expect a == 778270791
  seed(43)
  expect fingerprint(fn(i) { return random() }, 1000) != a
  expect throws { seed(1.5) } == "seed() expects a whole number, got 1.5"
}

test "seed is a random effect" {
  fn() ! random { seed(1) }()
  expect throws { fn() ! { seed(1) }() } != nil
}
