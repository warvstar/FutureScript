fn bad(x) { return x.nope }
fn outer(x) { return bad(x) }
print("before")
outer(1)
print("after")
