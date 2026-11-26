// Calling a known function with the wrong number of arguments is a compile error (file:line:col and a caret),
// so the program (progs/err_arity_call.fut) runs in a child process.
test "wrong argument count is a compile error at the call" {
  var r = run("progs/err_arity_call.fut")
  expect r.code == 1
  expect r.out == ""   // nothing ran
  expect r.err.has("progs/err_arity_call.fut:2:7: error: 'f' expects 2 arguments, got 1 [arity]\n")
  expect r.err.has("\n  print(f(1))\n        ^\n")
}
