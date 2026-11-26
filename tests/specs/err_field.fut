// Reading a field an instance has no slot for is an error, even after another instance grew the class layout.
class P { fn init() { self.a = 1 } }

test "a field added to one instance is missing on another" {
  var p = P()
  var q = P()
  q.b = 2               // grows the class layout; p has no slot for b
  expect q.b == 2
  expect throws { p.b } == "instance has no field 'b'"
}
test "uncaught, it stops the program with a trace" {
  var r = run("progs/err_field_grown.fut")
  expect r.code == 1
  expect r.out == "before\n"
  expect r.err.has("runtime error: instance has no field 'b' [no-field]\n  at <top> (")
  expect r.err.has("progs/err_field_grown.fut:6)")
}
