// Numbers. The runtime's own functions (extern "fut"): the same results, to the last bit, on every platform (arm64,
// wasm), within 1 ulp of exact, and exact where that is representable (pow(10, 3), log2(8), cbrt(27)). They perform nothing.

pub extern "fut" fn pow(num, num): num !
pub extern "fut" fn exp(num): num !
pub extern "fut" fn log(num): num !
pub extern "fut" fn log2(num): num !
pub extern "fut" fn log10(num): num !
pub extern "fut" fn cbrt(num): num !
pub extern "fut" fn hypot(num, num): num !
pub extern "fut" fn sin(num): num !
pub extern "fut" fn cos(num): num !
pub extern "fut" fn tan(num): num !
pub extern "fut" fn asin(num): num !
pub extern "fut" fn acos(num): num !
pub extern "fut" fn atan(num): num !
pub extern "fut" fn atan2(num, num): num !
pub extern "fut" fn ceil(num): num !
pub extern "fut" fn trunc(num): num !

pub var pi = 3.141592653589793
pub var e = 2.718281828459045

// |n|.
pub fn abs(n: num): num !
  ensures result >= 0
{
  if n < 0 { return -n }
  return n
}

// The smaller of a and b.
pub fn min(a: num, b: num): num !
  ensures result <= a && result <= b
{
  if b < a { return b }
  return a
}

// The larger of a and b.
pub fn max(a: num, b: num): num !
  ensures result >= a && result >= b
{
  if b > a { return b }
  return a
}

// n limited to [lo, hi].
pub fn clamp(n: num, lo: num, hi: num): num !
  requires lo <= hi
  ensures result >= lo && result <= hi
{
  if n < lo { return lo }
  if n > hi { return hi }
  return n
}

// a blended into b by t: a at 0, b at 1, as WGSL's mix. Numbers, or vectors with t a number or a vector of their size.
pub fn mix(a, b, t) ! {
  return a * (1 - t) + b * t
}

// 0 at or below lo, 1 at or above hi, and an S-shaped rise between, as WGSL's smoothstep.
pub fn smoothstep(lo: num, hi: num, x: num): num ! {
  var t = clamp((x - lo) / (hi - lo), 0, 1)
  return t * t * (3 - 2 * t)
}

// n's fraction: n - floor(n), from 0 up to 1, as WGSL's fract. A number, or a vector (each of its numbers').
pub fn fract(n) ! {
  return n - floor(n)
}

// -1, 0 or 1.
pub fn sign(n: num): num ! {
  if n > 0 { return 1 }
  if n < 0 { return -1 }
  return 0
}

// n to the nearest whole number, halves away from zero.
pub fn round(n: num): num !
  ensures abs(result - n) <= 0.5
{
  if n < 0 { return -floor(-n + 0.5) }
  return floor(n + 0.5)
}

// Is n not a number (0 / 0)?
pub fn isNaN(n: num): bool ! {
  return n != n
}
