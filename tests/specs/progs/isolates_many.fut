// Thousands of isolates: each is a stack and a heap, not a thread.
fn main() {
  var ts = []
  for i in 0..3000 { ts.push(spawn fn(i) { return i * 2 }(i)) }
  var total = 0
  for t in ts { total += t.join() }
  print(total)
}
