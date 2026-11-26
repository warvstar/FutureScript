// An invariant sits on its own line; on a line with other members it would swallow them.
class C { var n = 0 invariant self.n >= 0 fn step() { self.n += 1 } }
print(C().n)
// error: an invariant goes on its own line
