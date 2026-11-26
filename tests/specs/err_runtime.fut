// An uncaught runtime error prints its message and a stack trace, innermost call first, and stops the program.
// The program (progs/err_runtime_trace.fut) runs in a child process:
//   fn bad(x) { return x.nope }
//   fn outer(x) { return bad(x) }
//   print("before") outer(1) print("after")
fn bad(x) { return x.nope }
fn outer(x) { return bad(x) }

test "the error is the message, thrown from the innermost call" {
  expect throws { outer(1) } == "number has no field 'nope'"
}
test "uncaught: message, trace through each call, nothing after it runs" {
  var r = run("progs/err_runtime_trace.fut")
  expect r.code == 1
  expect r.out == "before\n"
  expect r.err.has("runtime error: number has no field 'nope'\n")
  expect r.err.has("  at bad (")
  expect r.err.has("progs/err_runtime_trace.fut:1)\n  at outer (")
  expect r.err.has("progs/err_runtime_trace.fut:2)\n  at <top> (")
  expect r.err.has("progs/err_runtime_trace.fut:4)")
}
