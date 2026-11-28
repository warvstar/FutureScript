// Where a value's type is known (a literal, a constructor, something declared) and a declared type can never take it,
// the check would fail every time it ran: a compile error. Anything the compiler can't see stays a run-time check.

fn compileErr(src) { return run({source: src}).err }

test "an argument of a direct call" {
  expect compileErr("fn half(n: num): num \{ return n / 2 }\nfn f() \{ half(\"x\") }").has("argument 'n' of half() must be num, got a string [type]")
  expect compileErr("fn f() \{ later(nil) }\nfn later(p: P) \{ }\nclass P \{ var x = 0 }").has("argument 'p' of later() must be P, got nil")
  expect compileErr("class P \{ }\nclass Q \{ }\nfn g(p: P) \{ }\nfn f() \{ g(Q()) }").has("argument 'p' of g() must be P, got an instance of Q")
  expect compileErr("fn g(s: str) \{ }\nfn f(n: num) \{ g(n) }").has("argument 's' of g() must be str, got a number")
  expect compileErr("fn g(x: num | str) \{ }\nfn f() \{ g(true) }").has("argument 'x' of g() must be num | str, got a bool")
  expect compileErr("fn g(m: \"GET\" | \"POST\") \{ }\nfn f() \{ g(1) }").has("must be \"GET\" | \"POST\", got a number")
  expect compileErr("type U = \{name: str}\nfn g(u: U) \{ }\nfn f() \{ g(3) }").has("argument 'u' of g() must be U, got a number")
}
test "a result, and a store to a typed var or field" {
  expect compileErr("fn f(): num \{ return \"x\" }").has("the result of f() must be num, got a string [type]")
  expect compileErr("fn f(): num \{ return }").has("the result of f() must be num, got nil")
  expect compileErr("fn f() \{ var s: str = true }").has("var 's' must be str, got a bool")
  expect compileErr("var lvl: num = 1\nfn f() \{ lvl = \"x\" }").has("var 'lvl' must be num, got a string")
  expect compileErr("fn f(n: num) \{ n = [1] }").has("argument 'n' of f() must be num, got a list")
  expect compileErr("class P \{ var x: num = 0 fn set() \{ self.x = \"s\" } }").has("field 'x' of P must be num, got a string")
  expect compileErr("class P \{ var x: num = 0 }\nfn f() \{ var p = P() p.x = \{} }").has("field 'x' of P must be num, got a map")
}
test "no error where some value could pass" {
  expect compileErr("class P \{ }\nclass Q : P \{ }\nfn g(p: P) \{ }\nfn f() \{ g(Q()) }") == ""       // a subclass
  expect compileErr("fn g(n: num?) \{ }\nfn f() \{ g(nil) }") == ""                                 // optional
  expect compileErr("fn g(x: num | str) \{ }\nfn f() \{ g(1) g(\"a\") }") == ""                     // a union that has it
  expect compileErr("type U = \{name: str}\nfn g(u: U) \{ }\nfn f() \{ g(\{name: \"a\"}) }") == ""  // a map may be the shape
  expect compileErr("fn g(n: num) \{ }\nfn f(v) \{ g(v) g(v + 1) g(v.x) }") == ""                   // not known
  expect compileErr("fn g(n: num) \{ }\nfn f(v: num?) \{ g(v) }") == ""                             // may be a number
  expect compileErr("fn g(n: num) \{ }\nfn f(s) \{ var x = \"a\" x = s g(x) }") == ""               // assigned again
  expect compileErr("fn f(): num? \{ return }") == ""
}
