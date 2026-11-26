// Closures: capture by value, closures over arguments, over self, and over a loop variable.
fn counter() {
  var n = 0
  return fn() { n += 1 return n }     // captured by value; the closure keeps its own copy
}
var c = counter()
var d = counter()
fn adder(k) { return fn(x) { return x + k } }
var add5 = adder(5)
fn compose(f, g) { return fn(x) { return f(g(x)) } }
fn nest(a) { return fn(b) { return fn(c) { return a + b + c } } }
class Btn { fn init(label) { self.label = label } fn handler() { return fn() { return "clicked " + self.label } } }
var fs = []

test "each counter keeps its own copy of n" {
  c()
  c()
  expect c() == 3
  expect d() == 1
}
test "closures over arguments compose and nest" {
  expect add5(10) == 15
  expect compose(add5, fn(x) { return x * 2 })(4) == 13
  expect nest(1)(2)(3) == 6
}
test "a closure made in a method captures self" {
  expect Btn("ok").handler()() == "clicked ok"
}
test "closures made in a loop capture each iteration's value" {
  for i in 0..3 { fs.push(fn() { return i * 10 }) }
  expect fs[2]() == 20
  expect fs[0]() == 0
  expect prints { print(fs[0]) } == "<fn fn@30>\n"   // an anonymous fn prints with its line
}
