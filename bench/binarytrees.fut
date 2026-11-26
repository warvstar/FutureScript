class Node { fn init(l, r) { self.l = l self.r = r } }
fn make(d) { if d == 0 { return Node(nil, nil) } return Node(make(d - 1), make(d - 1)) }
fn check(n) { if n.l == nil { return 1 } return 1 + check(n.l) + check(n.r) }
fn kernel() {
  var max = 16
  var total = check(make(max + 1))
  var long = make(max)
  var d = 4
  while d <= max {
    var iters = 1 << (max - d + 4)
    for i in 0..iters { total += check(make(d)) }
    d += 2
  }
  return total + check(long)
}
