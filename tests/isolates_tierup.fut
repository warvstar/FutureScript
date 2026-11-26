// Isolates on several threads run the same hot functions while one of them compiles each again (tier 2): every
// thread goes on into the new code (a body another thread replaced sends its calls through the stub, caught up).
fn fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2) }
fn work(k) {
  var s = 0
  for i in 0..20 { s += fib(18 + i % 3) }
  s += k * 0
  return s
}
fn main() {
  var ts = []
  for k in 0..8 { ts.push(spawn work(k)) }
  var total = 0
  for t in ts { total += t.join() }
  print(total)
}
// expect: 703560
