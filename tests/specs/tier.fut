// Tier 2: a function called 1000 times is compiled again, speculating that the arguments that were
// numbers stay numbers (an entry guard falls back to the tier-1 code) and that its recursion returns numbers.
// The warm-up loops stay in top-level code, which runs once before the tests; the tests then make the calls
// that follow each warm-up in the original program. (Under WebAssembly all tests share one process, so the
// call counts only grow; every function is already past its 1000 calls when a test runs.)
fn add(a, b) { return a + b }
var s = 0
for i in 0..3000 { s = add(s, i) }
fn fib(n) { if n < 2 { return n } return fib(n - 1) + fib(n - 2) }
fn f(n) { if n == 0 { return "x" } return f(n - 1) }   // recursion that does not return a number
var r = ""
for i in 0..1500 { r = f(3) }
fn tricky(n) { var x = n for i in 0..2 { if i == 1 { x = "s" } } return x }  // reassigned inside a loop
for i in 0..1100 { tricky(1) }
fn loopy(n) { var out = 0 var k = n while k > 0 { out += k k -= 1 } return out + n }
for i in 0..1200 { loopy(5) }
class C {
  fn init() { self.k = 2 }
  fn scale(x) { return x * self.k }
}
var c = C()
var t = 0
for i in 0..2000 { t += c.scale(i) }
fn half(x) { return x / 2 }
for i in 0..2000 { half(i) }

test "a hot function with number arguments, then other arguments through its entry guard" {
  expect s == 4498500
  expect add("a", "b") == "ab"
  expect add(1, 2) == 3
}
test "recursion that returns numbers" { expect fib(20) == 6765 }
test "recursion that does not return a number" { expect r == "x" }
test "a local reassigned to a string inside a loop" { expect tricky(1) == "s" }
test "a while loop in a hot function" { expect loopy(5) == 20 }
test "a hot method" {
  expect t == 3998000
  expect c.scale(0.5) == 1
}
test "a hot function given nil still checks its operands" {
  expect throws { half(nil) } == "bad operands for '/': nil and number"
}
