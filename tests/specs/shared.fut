// shared var: one variable for every isolate (other globals are copied into each). It holds frozen values, and is
// assigned only inside atomic { }, whose blocks are then transactions across isolates: each reads one consistent
// state, and commits only if no other isolate changed what it read meanwhile; otherwise it runs again, as if it had
// started later. Outside atomic, a read gives the latest committed value.
fn err(src) { return run({source: src}).err }
// (the programs that change shared vars run in processes of their own: `build.py test` runs each spec in several
// isolates at once, which would share them)
test "a shared var is every isolate's; it holds frozen values; it changes only inside atomic, all or nothing" {
  expect run("progs/shared_basics.fut").out == "done\n"
}
test "conflicts run again: no update is lost" {
  expect run("progs/shared_contended.fut").out == "24000 24000 true\n"
}
test "errors" {
  expect err("shared var X = 1\nX = 2").has(" [atomic]\n")
  expect err("fn f() \{ shared var X = 1 }").has("only allowed at top level")
  expect err("shared var X = 0\nvar out = f64s(2)\nparallel for i in 0..2 \{ out[i] = X }").has(" [parallel]\n")
}
