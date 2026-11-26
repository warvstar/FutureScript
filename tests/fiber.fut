// Fibers: functions on their own stacks; yield (from any depth) switches back to the resumer.
fn count(n) { for i in 0..n { yield i } return "end" }
var g = fiber(fn() { return count(2) })
print(g.resume())                  // expect: 0
print(g.resume())                  // expect: 1
print(g.done())                    // expect: false
print(g.resume())                  // expect: end
print(g.done())                    // expect: true
var acc = fiber(fn(first) { var total = first while true { total += yield total } })
print(acc.send(10))                // expect: 10
print(acc.send(5))                 // expect: 15
var fibs = []
for x in fiber(fn() { var a = 0 var b = 1 for i in 0..8 { yield a var t = a + b a = b b = t } }) { fibs.push(x) }
print(fibs)                        // expect: [0, 1, 1, 2, 3, 5, 8, 13]
var fs = []                        // many fibers, with garbage made while they are switched out
for i in 0..200 { var k = i fs.push(fiber(fn() { var l = [] for j in 0..50 { l.push("s{j}") yield len(l) } return k })) }
var sum = 0
for r in 0..60 { for f in fs { if !f.done() { sum += f.resume() } } }
print(sum)                         // expect: 274900
fn deep(n) { if n == 0 { return yield "bottom" } return deep(n - 1) }
var d = fiber(fn() { return deep(100) })
print(d.resume())                  // expect: bottom
print(d.send("back up"))           // expect: back up
var bad = fiber(fn() { yield 1 return nil.x })
print(bad.resume())                // expect: 1
bad.resume()                       // error: nil has no field 'x'
