fn bad(x) { return x.nope }
fn outer(x) { return bad(x) }
print("before")   // expect: before
outer(1)
// error: number has no field 'nope'
// error: at bad (
// error: err_runtime.fut:1)
// error: at outer (
