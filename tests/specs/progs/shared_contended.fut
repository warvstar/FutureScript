// shared.fut runs this: 8 isolates in parallel conflict on the same shared vars. Each block that conflicts runs
// again with its locals as they were, so no update is lost and a local counts each block once.
shared var Hits = 0
shared var Pairs = [0, 0]
fn work(n) {
  var mine = 0
  for i in 0..n {
    atomic {
      mine += 1
      Hits += 1
      Pairs = [Pairs[0] + 1, Pairs[1] + 1]
    }
  }
  return mine
}
var ts = []
for i in 0..8 { ts.push(spawn work(3000)) }
var total = 0
for t in ts { total += t.join() }
print(Hits, total, Pairs[0] == Pairs[1])
