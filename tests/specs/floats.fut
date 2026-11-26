// Arithmetic reads its operands from the FP registers that locals live in, a left operand that is a local is read
// again after the right one instead of being saved, and -x is a negate. Each case here once went wrong, or could.
var G = 4.0
class Box { fn init(v) { self.v = v } }
fn id(x) { return x }

test "negation: -0, twice, of a field, and of a non-number" {
  var z = 0.0
  expect 1 / -z == -1 / 0
  expect 1 / -(-z) == 1 / 0
  var m = 3.0
  m = -m
  expect m == -3 && -m == 3
  expect -Box(2).v == -2
  var s = "x"
  expect throws { -s } == "bad operands for '-': number and string"
}
test "a left local is read again after a right operand that is not simple" {
  var a = 12.0
  expect a / G == 3          // a global
  expect 0 - a / G == -3     // the same, on the right of another operator
  expect a - id(2) == 10     // a call
  expect a * Box(2).v == 24  // a field
  expect a + [1, 2][1] == 14 // an index
  expect a / (a - 8) == 3    // itself
  var n = 5
  expect n * (n + 1) == 30   // a local that is not in an FP register
}
test "locals in slots, in a function with a try" {
  var a = 1.0 var b = 2.0 var c = 3.0 var d = 4.0 var e = 5.0 var f = 6.0 var g = 7.0 var h = 8.0 var i = 9.0
  var k = 10
  try { k = k * (i - h) + g / (a + b + c - d) } catch err { }
  expect k == 13.5 && -i == -9
}
test "a local in an FP register can take any value" {
  var b = 1.5
  b = b + "s"
  expect b == "1.5s"
  b = 2.0
  expect b / (b + 2) == 0.5
  var q = 1.0
  q = "str"
  expect q + q == "strstr"
  var w = 4.0
  w += w * w
  expect w == 20
  var v = 1.0
  expect throws { for i in 0..3 { v = v * 2 + i  if i == 1 { v = "s" + v } } } == "bad operands for '*': string and number"
}
test "a value from the slow path, then used as a number" {
  var x = 1.0
  var y = x + "" + 2   // a string, made by the runtime
  expect throws { x = x - y } == "bad operands for '-': number and string"
  var p = 0.0
  for o in [Box(1), Box(2)] { p += o.v * o.v }
  expect 0 - p / G == -1.25
}
test "comparisons read the FP registers too" {
  var x = 1.5
  var y = 2.5
  var n = 0
  while x * x + y * y < 100 { x = x * 1.5 y = -y * 1.5 n += 1 }
  expect n == 4 && y > 0
}
