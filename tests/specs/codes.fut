// Every error names the rule it broke with a stable code after its message ("... [index]"), and as "code" in --json.
// Each code is raised here at least once, in a child process (an error's code shows where it is reported); build.py
// checks that every code the source uses is registered (errors.cpp) and raised here, or exempt there with a reason.
fn err(src) { return run({source: src}).err }

test "compile errors" {
  expect err("print(").has(" [syntax]\n")
  expect err("print(x)").has("error: undefined name 'x' [undefined-name]\n")
  expect run("progs/codes_private.fut").err.has(" [private]\n")
  expect err("effect E \{ fn op() }\nvar x = E").has(" [not-a-value]\n")
  expect err("fn f(a) \{ }\nf()").has(" [arity]\n")
  expect err("fn f(a: nmu) \{ }").has(" [unknown-type]\n")
  expect err("fn f() ! iox \{ }").has(" [unknown-effect]\n")
  expect err("print(self)").has(" [misplaced]\n")
  expect err("fn f() \{ }\nfn f() \{ }").has(" [duplicate]\n")
  expect err("class A \{ var x = [] }").has(" [class]\n")
  expect err("class A \{ fn m() \{ } }\nclass B : A \{ fn m() \{ } }").has(" [override]\n")
  expect err("enum E \{ A B }\nfn f(e) \{ match e \{ A \{ } } }").has(" [match]\n")
  expect err("import \"nope.fut\"").has(" [import]\n")
  expect err("extern fn no_such_fn_xyz(): num").has(" [extern]\n")
  var ps = "p0"
  for i in 1..65 { ps += ", p{i}" }
  expect err("fn f(" + ps + ") \{ }").has(" [limit]\n")
}

test "runtime errors" {
  expect err("fn f(n: num) \{ }\nf(\"x\")").has(" [type]\n")
  expect err("atomic \{ print(1) }").has(" [atomic]\n")
  expect err("fn f() ! \{ print(1) }\nf()").has(" [effect]\n")
  expect err("effect E \{ fn op() }\nE.op()").has(" [unhandled]\n")
  expect err("var x = 1\nprint(x.nope)").has(" [no-field]\n")
  expect err("print(1 + nil)").has(" [operand]\n")
  expect err("print([].nope())").has(" [no-method]\n")
  expect err("print([][0])").has("index 0 out of range (len 0) [index]\n")
  expect err("for i in 3 \{ }").has(" [not-iterable]\n")
  expect err("var x = 1\nx()").has(" [not-callable]\n")
  expect err("fn f() \{ return f() + 1 }\nf()").has(" [stack-overflow]\n")
  expect err("var f = fiber(fn() \{ })\nf.resume()\nf.resume()").has(" [fiber]\n")
  expect err("len(1)").has(" [argument]\n")
  expect err("present()").has(" [host]\n")
}

test "contracts and expect say whose fault it is" {
  expect err("fn f(n) requires n > 0 \{ return n }\nf(0)").has(" [requires]\n")
  expect err("fn f(n) ensures result > 0 \{ return 0 }\nf(1)").has(" [ensures]\n")
  expect err("class A \{\n  var x = 0\n  invariant self.x >= 0\n  fn dec() \{ self.x -= 1 }\n}\nA().dec()").has(" [invariant]\n")
  expect err("expect 1 == 2").has(" [expect]\n")
}

test "a caught error is its message alone; what the program throws has no code" {
  var got = nil
  try { [][0] } catch e { got = e }
  expect got == "index 0 out of range (len 0)"
  expect err("throw \"mine\"").has("runtime error: mine\n")
}
