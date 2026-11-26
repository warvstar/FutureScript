// Tier 2: a function called 1000 times is compiled again, speculating that the arguments that were
// numbers stay numbers (an entry guard falls back to the tier-1 code) and that its recursion returns numbers.
fn add(a, b) { return a + b }
var s = 0
for i in 0..3000 { s = add(s, i) }
print(s)                          // expect: 4498500
print(add("a", "b"))              // expect: ab
print(add(1, 2))                  // expect: 3
fn fib(n) { if n < 2 { return n } return fib(n - 1) + fib(n - 2) }
print(fib(20))                    // expect: 6765
fn f(n) { if n == 0 { return "x" } return f(n - 1) }   // recursion that does not return a number
var r = ""
for i in 0..1500 { r = f(3) }
print(r)                          // expect: x
fn tricky(n) { var x = n for i in 0..2 { if i == 1 { x = "s" } } return x }  // reassigned inside a loop
for i in 0..1100 { tricky(1) }
print(tricky(1))                  // expect: s
fn loopy(n) { var out = 0 var k = n while k > 0 { out += k k -= 1 } return out + n }
for i in 0..1200 { loopy(5) }
print(loopy(5))                   // expect: 20
class C {
  fn init() { self.k = 2 }
  fn scale(x) { return x * self.k }
}
var c = C()
var t = 0
for i in 0..2000 { t += c.scale(i) }
print(t)                          // expect: 3998000
print(c.scale(0.5))               // expect: 1
fn half(x) { return x / 2 }
for i in 0..2000 { half(i) }
print(half(nil))                  // error: bad operands for '/': nil and number
