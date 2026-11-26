// freeze(v): a deep copy that never changes. Every isolate reads the same one in place, so sending a frozen value
// sends a pointer, however big it is. thaw(v) gives a copy you may change. frozenCount() is how many frozen objects
// are alive: each goes when nothing holds it (no isolate, message or other frozen object).
fn err(src) { return run({source: src}).err }
fn errorOf(f) {
  try { f() } catch e { return "{e}" }
  return nil
}
class P { fn init() { self.x = 1  self.y = [2, 3] } }
fn sum(xs) { var s = 0  for x in xs { s += x }  return s }
fn churn() { var n = 0  for i in 0..400000 { n = [i, i] }  return n }

test "a frozen copy reads as the original, and never changes" {
  var src = {name: "a", xs: [1, 2, [3]], p: P()}
  var f = freeze(src)
  expect isFrozen(f) && !isFrozen(src)
  expect f.name == "a"
  expect f.xs[2][0] == 3
  expect f.p.y[1] == 3
  src.xs.push(4)
  expect f.xs.len() == 3
  expect errorOf(fn() { f.xs.push(4) }).has("frozen")
  expect errorOf(fn() { f.xs[0] = 9 }).has("frozen")
  expect errorOf(fn() { f.name = "b" }).has("frozen")
  expect errorOf(fn() { f.p.x = 2 }).has("frozen")
  expect errorOf(fn() { f.p.y.pop() }).has("frozen")
  expect f.xs[0] == 1 && f.p.x == 1
  expect freeze(f) == f
  expect freeze(3) == 3 && freeze("s") == "s" && freeze(nil) == nil
  expect isFrozen(sum)
}
test "a shared part stays shared, and thaw gives a copy you may change" {
  var part = [1, 2]
  var f = freeze([part, part])
  expect f[0] == f[1]
  var t = thaw(f)
  expect !isFrozen(t) && !isFrozen(t[0])
  t[0].push(3)
  expect t[1].len() == 3
  expect f[0].len() == 2
}
test "buffers and closures freeze too" {
  var b = f64s([1.5, 2.5])
  var f = freeze(b)
  b[0] = 9
  expect f[0] == 1.5 && f.len() == 2
  expect errorOf(fn() { f[1] = 0 }).has("frozen")
  expect errorOf(fn() { f.fill(0) }).has("frozen")
  var k = 10
  var add = freeze(fn(x) { return x + k })
  expect add(1) == 11
  var t = thaw(f)
  t[1] = 7
  expect t[1] == 7 && f[1] == 2.5
}
test "what freeze can't copy" {
  var xs = [1]
  xs.push(xs)
  expect errorOf(fn() { freeze(xs) }).has("contains itself")
  xs[0] = 2
  expect xs[0] == 2 && !isFrozen(xs)
  var shared = [5]
  var ok = [shared, [shared]]
  expect errorOf(fn() { freeze([ok, channel(1)]) }).has("can't copy")
  var f = freeze(ok)
  expect f[0] == f[1][0] && f[0][0] == 5
  expect errorOf(fn() { freeze([channel(1)]) }).has("can't copy")
  expect err("var xs = [1]\nxs.push(xs)\nfreeze(xs)").has(" [frozen]\n")
}
fn total(f) { return sum(f.xs) + f.p.x }
fn grab(c) { return c.recv().xs.len() }
test "isolates read a frozen value in place" {
  var f = freeze({xs: [1, 2, 3], p: P()})
  var ts = []
  for i in 0..4 { ts.push(spawn total(f)) }
  for t in ts { expect t.join() == 7 }
  var c = channel(1)
  var t = spawn grab(c)
  c.send(f)
  expect t.join() == 3
}
// (in a process of its own: frozenCount() counts every isolate's)
var Counting = "fn churn() \{ var n = 0  for i in 0..400000 \{ n = [i, i] }  return n }
fn freezeMany(n) \{
  var keep = freeze([1, [2]])
  for i in 0..n \{ var x = freeze([i, [i], \{k: \"v\{i}\"}]) }
  return keep
}
var made = freezeMany(2000)
print(frozenCount() > 8000)
churn()
var t = spawn freezeMany(2000)
var got = t.join()
churn()
print(made[1][0], got[1][0], frozenCount() < 50)"
test "a frozen value goes when nothing holds it" {
  expect run({source: Counting}).out == "true\n2 2 true\n"
}
