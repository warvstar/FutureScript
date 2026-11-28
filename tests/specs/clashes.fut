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
test "a constructor's arguments, and a method's on an instance of a known class" {
  var cls = "class E \{ var t: str  var n: num  fn init(t: str, n: num) \{ self.t = t self.n = n } fn put(x: str) \{ self.t = x } }\n"
  expect compileErr(cls + "fn f(e: E) \{ E(e.n, e.t) }").has("argument 't' of E() must be str, got a number [type]")
  expect compileErr(cls + "fn f() \{ E(\"a\") }").has("E() expects 2 arguments (t: str, n: num), got 1 [arity]")
  expect compileErr(cls + "class F : E \{ }\nfn f() \{ F(1, 2) }").has("argument 't' of F() must be str, got a number")  // the init it inherits
  expect compileErr("class A \{ var x = 1 }\nfn f() \{ A(1) }").has("A() takes no arguments (it has no init), got 1 [arity]")
  expect compileErr(cls + "fn f(e: E) \{ e.put(e.n) }").has("argument 'x' of E.put() must be str, got a number [type]")
  expect compileErr(cls + "fn f() \{ var e = E(\"a\", 1) e.put(\"b\", 2) }").has("E.put() expects 1 argument (x: str), got 2 [arity]")
  expect compileErr(cls + "fn f(e: E?) \{ e?.put(1) }") == ""                                  // may be nil: the call is skipped
  expect compileErr(cls + "fn f(e) \{ e.put(1) }") == ""                                       // not known
}
test "a value bound by a match arm has its variant field's type" {
  var en = "enum Node \{\n  Heading(level: num, text: str)\n  Raw(x)\n}\nfn half(n: num): num \{ return n / 2 }\n"
  expect compileErr(en + "fn f(n: Node) \{ match n \{ Heading(level, text) \{ half(text) } Raw(x) \{ } } }").has("argument 'n' of half() must be num, got a string")
  expect compileErr(en + "fn f(n: Node) \{ match n \{ Heading(_, text) \{ text = 2 half(text) } Raw(x) \{ half(x) } } }") == ""  // assigned again; untyped
}
test "a list or a string has a fixed set of methods and no fields" {
  expect compileErr("fn f() \{ var out = [] out.has(1) }").has("list has no method 'has' (a list's methods: push, pop, len, slice, join); import \"std/lists\" and use lists.contains(out, v) [no-method]")
  expect compileErr("fn f(xs: list<num>) \{ xs.count() }").has("use lists.count(xs, ...)")
  expect compileErr("fn f(s: str) \{ s.length }").has("string has no field 'length' (a string has methods but no fields); use s.len() or len(s) [no-field]")
  expect compileErr("fn f() \{ \"a b\".includes(\"a\") }").has("string has no method 'includes'")
  expect compileErr("fn f() \{ var s = \"a\" s.upper() }").has("strings.upper(s) [no-method]")
  expect compileErr("fn f(s) \{ var x = [] x = s x.trim() }") == ""                     // assigned again
  expect compileErr("fn f(s: str?) \{ s?.foo() }") == ""                                // may be nil
  expect compileErr("fn f() \{ var m = \{} m.foo() m.k }") == ""                        // a map's keys are its fields
  expect run({source: "var l = [1]\nl.has(1)"}).err.has("list has no method 'has'; import \"std/lists\" and use lists.contains(l, v)")  // the same hint when it runs
}
