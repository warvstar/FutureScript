// Unbounded recursion is caught as a "stack overflow" error instead of crashing the process.
// The program (progs/err_stack_down.fut) runs in a child process:
//   fn down(n) { return down(n + 1) + 1 }
//   print("before") down(0) print("after")
test "unbounded recursion stops the program with a stack overflow error" {
  var r = run("progs/err_stack_down.fut")
  expect !r.timedOut
  expect r.code == 1
  expect r.out == "before\n"
  expect r.err.has("runtime error: stack overflow\n")
}
