// A wrong name.x on a module is a compile error (file:line:col and a caret),
// so the program (progs/modules_err_missing.fut) runs in a child process.
test "a missing module member is a compile error" {
  var r = run("progs/modules_err_missing.fut")
  expect r.code == 1
  expect r.out == ""   // nothing ran
  expect r.err.has("progs/modules_err_missing.fut:2:12: error: module 'geom' has no 'nope' [undefined-name]\n")
  expect r.err.has("\n  print(geom.nope)\n             ^\n")
}
