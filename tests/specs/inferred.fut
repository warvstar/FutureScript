// A local made by C(...) or Enum.Variant(...) has that class until it is assigned again, so x.name is checked against
// the class's declared fields and methods at compile time, as if it were declared x: C.

fn compileErr(src) { return run({source: src}).err }
var pointer = "class Pointer \{ var hover: bool = false fn move() \{ } }\n"

test "a typo on a constructed local is a compile error" {
  expect compileErr(pointer + "fn f() \{ var p = Pointer() p.hovre = true }").has("'hovre' is not a field of Pointer (p is Pointer: hover, move); did you mean 'hover'? [no-field]")
  expect compileErr(pointer + "fn f() \{ var p = Pointer() p.mvoe() }").has("'mvoe' is not a method or field of Pointer (p is Pointer: hover, move); did you mean 'move'? [no-field]")
  expect compileErr(pointer + "fn f() \{ return Pointer().hovre }").has("'hovre' is not a field of Pointer")
  expect compileErr(pointer + "fn f() \{ var p = (Pointer()) return p.hovre }").has("'hovre' is not a field of Pointer")
}
test "an inherited method is listed once" {
  var src = "class A \{ var n = 0 fn go() \{ } }\nclass B : A \{ override fn go() \{ } }\nfn f() \{ var b = B() b.gone() }"
  expect compileErr(src).has("(b is B: go, n); did you mean 'go'?")
}
test "enum variants" {
  var src = "enum Shape \{ Circle(r: num) Empty }\nfn f() \{ var c = Shape.Circle(1) return c.rr }"
  expect compileErr(src).has("'rr' is not a field of Shape.Circle")
}
test "no error where the local may hold something else" {
  expect compileErr(pointer + "fn f(q) \{ var p = Pointer() p = q return p.anything }") == ""                    // assigned again
  expect compileErr(pointer + "fn f(q) \{ var p = Pointer() while p != nil \{ p.nope p = q } }") == ""          // ... in a loop
  expect compileErr(pointer + "fn f(c) \{ var p = c ? Pointer() : nil return p?.nope }") == ""                  // not just a constructor
  expect compileErr("class Open \{ fn init() \{ self.a = 1 } }\nfn f() \{ var o = Open() return o.b }") == ""   // undeclared fields: open
  expect compileErr(pointer + "var p = Pointer()\nfn f() \{ return p.nope }") == ""                              // a global may be assigned anywhere
}
test "the right names pass" {
  expect compileErr(pointer + "fn f() \{ var p = Pointer() p.hover = true p.move() return p.hover }") == ""
}
