// Many isolates busy at once (in parallel, on a pool's threads): each allocates enough to collect its heap many
// times, makes hot functions compile again (tier 2) while the others run them, and sends closures back.
class P {
  var x = 0
  var y = 0
  fn init(x, y) { self.x = x  self.y = y }
  fn len2() { return self.x * self.x + self.y * self.y }
}
fn hot(a, b) { return a * 3 + b }
fn work(id, n, out) ! chan {
  var acc = 0
  var keep = []
  for i in 0..n {
    acc += hot(i, id) % 11
    acc += P(i, id).len2() % 3
    keep.push("s{i}")
    if len(keep) > 1000 { keep = [] }
    if i % 5000 == 0 { out.send(fn(z) { return z + id }) }
  }
  return acc
}
fn main() {
  var out = channel(1000)
  var ts = []
  for k in 0..12 { ts.push(spawn work(k, 60000, out)) }
  var total = 0
  for t in ts { total += t.join() }
  out.close()
  var fs = 0
  for f in out { fs += f(1) }
  print(total)
  print(fs)
}
// expect: 4559993
// expect: 936
