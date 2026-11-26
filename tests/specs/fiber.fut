// Fibers: functions on their own stacks; yield (from any depth) switches back to the resumer.
fn count(n) { for i in 0..n { yield i } return "end" }
fn deep(n) { if n == 0 { return yield "bottom" } return deep(n - 1) }

test "resume runs to the next yield; the return value comes last" {
  var g = fiber(fn() { return count(2) })
  expect g.resume() == 0
  expect g.resume() == 1
  expect g.done() == false
  expect g.resume() == "end"
  expect g.done() == true
}
test "send(v) resumes and becomes yield's value" {
  var acc = fiber(fn(first) { var total = first while true { total += yield total } })
  expect acc.send(10) == 10
  expect acc.send(5) == 15
}
test "for-in resumes until the fiber returns" {
  var fibs = []
  for x in fiber(fn() { var a = 0 var b = 1 for i in 0..8 { yield a var t = a + b a = b b = t } }) { fibs.push(x) }
  expect fibs == [0, 1, 1, 2, 3, 5, 8, 13]
}
test "many fibers, with garbage made while they are switched out" {
  var fs = []
  for i in 0..200 { var k = i fs.push(fiber(fn() { var l = [] for j in 0..50 { l.push("s{j}") yield len(l) } return k })) }
  var sum = 0
  for r in 0..60 { for f in fs { if !f.done() { sum += f.resume() } } }
  expect sum == 274900
}
test "yield from deep in a recursion, and send back down to it" {
  var d = fiber(fn() { return deep(100) })
  expect d.resume() == "bottom"
  expect d.send("back up") == "back up"
}
test "an error inside a fiber ends it and is raised again in its resumer" {
  var bad = fiber(fn() { yield 1 return nil.x })
  expect bad.resume() == 1
  expect throws { bad.resume() } == "nil has no field 'x'"
}
