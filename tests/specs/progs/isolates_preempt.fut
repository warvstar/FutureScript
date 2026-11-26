// More busy isolates than threads (run with --threads 2), each spinning until the one after it is done. The last one
// gets a thread only if the pool takes the others' turns away (at their polls): otherwise this never ends.
fn spin(mine, next) ! chan {
  while mine.tryRecv() == nil { }
  if next != nil { next.send(1) }
}
fn main() {
  var n = 4
  var cs = []
  for i in 0..n { cs.push(channel(1)) }
  var ts = []
  for i in 0..n {
    var next = nil
    if i > 0 { next = cs[i - 1] }
    ts.push(spawn spin(cs[i], next))
  }
  cs[n - 1].send(1)
  for t in ts { t.join() }
  print("all done")
}
