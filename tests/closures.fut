fn counter() {
  var n = 0
  return fn() { n += 1 return n }     // captured by value; the closure keeps its own copy
}
var c = counter()
c()
c()
print(c())                            // expect: 3
var d = counter()
print(d())                            // expect: 1
fn adder(k) { return fn(x) { return x + k } }
var add5 = adder(5)
print(add5(10))                       // expect: 15
fn compose(f, g) { return fn(x) { return f(g(x)) } }
print(compose(add5, fn(x) { return x * 2 })(4))   // expect: 13
fn nest(a) { return fn(b) { return fn(c) { return a + b + c } } }
print(nest(1)(2)(3))                  // expect: 6
class Btn { fn init(label) { self.label = label } fn handler() { return fn() { return "clicked " + self.label } } }
print(Btn("ok").handler()())          // expect: clicked ok
var fs = []
for i in 0..3 { fs.push(fn() { return i * 10 }) }
print(fs[2]())                        // expect: 20
print(fs[0])                          // expect: <fn fn@21>
