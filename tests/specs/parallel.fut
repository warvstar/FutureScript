// parallel for i in a..b { body }: the body runs once for each i, on the isolates of the pool, and the loop ends
// when every iteration has. The body writes its results to buffers, each only at [i]; what it reads from outside is
// a frozen copy, and it performs no effects. So the result is the one a plain for gives, however the iterations ran.
fn errorOf(f) {
  try { f() } catch e { return e }
  return nil
}
fn err(src) { return run({source: src}).err }
fn sq(x) { return x * x }
class P { var x = 1 }
fn setAt(b, i) { b[i] = 5 }
var Scale = 3
var Hits = 0
fn hit() { Hits += 1 }
var Out = f64s(4)

test "each iteration writes its own element, and reads anything from outside" {
  var n = 1000
  var inp = f64s(n)
  for i in 0..n { inp[i] = i }
  var out = f64s(n)
  var w = i32s(n)
  var u = u8s(n)
  var xs = [10, 20]
  var p = P()
  parallel for i in 0..n {
    out[i] = sq(inp[i]) * Scale + xs[1] + p.x
    w[i] = i - 500
    u[i] = i % 256
  }
  var ok = true
  for i in 0..n { if out[i] != i * i * 3 + 21 || w[i] != i - 500 || u[i] != i % 256 { ok = false } }
  expect ok
  parallel for i in 0..4 { Out[i] = i + out.len() }
  expect Out.toList() == [1000, 1001, 1002, 1003]
  parallel for i in 3..3 { Out[i] = 9 }
  parallel for i in 5..2 { Out[i] = 9 }
  expect Out[3] == 1003
}
test "the body's own values are its to change" {
  var out = f64s(4)
  parallel for i in 0..4 {
    var mine = [i]
    mine.push(i)
    mine[0] = 9
    var t = 0
    for k in 0..i { t += k }
    out[i] = mine[0] + mine[1] + t
  }
  expect out.toList() == [9, 10, 12, 15]
}
test "an error is the one at the lowest i that fails, every run" {
  var out = f64s(100)
  for r in 0..5 {
    expect errorOf(fn() { parallel for i in 0..100 { out[i] = 1  if i % 10 == 7 { throw "boom " + str(i) } } }) == "parallel for, at i = 7: boom 7"
  }
  expect errorOf(fn() { parallel for i in 0..100 { if i >= 50 { out[i] = [].missing() } } }).has("parallel for, at i = 50: list has no method 'missing'")
  expect errorOf(fn() { parallel for i in 0.5..10 { } }).has("whole numbers")
}
test "the body can't change what it reads from outside" {
  var out = f64s(4)
  var ro = f64s(4)
  var xs = [1]
  var m = {a: 1}
  var p = P()
  expect errorOf(fn() { parallel for i in 0..4 { xs.push(1) } }).has("can't change this list, from outside it")
  expect errorOf(fn() { parallel for i in 0..4 { out[i] = m.remove("a") } }).has("can't change this map")
  expect errorOf(fn() { parallel for i in 0..4 { p.x = 2 } }).has("can't change this instance")
  expect errorOf(fn() { parallel for i in 0..4 { setAt(ro, i) } }).has("can't change this buffer")
  expect errorOf(fn() { parallel for i in 0..4 { hit() } }).has("changed the global 'Hits'")
  expect xs == [1]
  expect m == {a: 1}
  expect p.x == 1 && ro[0] == 0 && Hits == 0
}
test "a buffer it writes, it reaches only at [i]" {
  var out = f64s(4)
  expect errorOf(fn() { parallel for i in 0..4 { out[i + 1] = 2 } }).has("at i = 0: a parallel for's body writes this buffer, so it may reach it only at [i] (here i is 0, and the index 1)")
  expect errorOf(fn() { parallel for i in 0..4 { out[i] = out[0] } }).has("at i = 1:")
  expect errorOf(fn() { parallel for i in 0..4 { out[i] = 1  for x in out { } } }).has("only at [i]")
  expect errorOf(fn() { parallel for i in 0..4 { out[i] = out.len() } }) == nil && out[3] == 4
  var xs = [1, 2, 3, 4]
  expect errorOf(fn() { parallel for i in 0..4 { xs[i] = 1 } }).has("writes xs[...], and xs is list: the body writes only buffers")
}
test "the body performs no effects, and nests no parallel for" {
  expect errorOf(fn() { parallel for i in 0..4 { print(i) } }).has("print() performs io, which a parallel for's body can't")
  expect errorOf(fn() { parallel for i in 0..4 { parallel for j in 0..2 { } } }).has("a parallel for inside a parallel for's body")
  expect err("atomic \{ parallel for i in 0..4 \{ } }").has("parallel for inside atomic")
}
test "the body can't assign outside variables, return, or break" {
  expect err("var s = 0\nparallel for i in 0..4 \{ s += i }").has("can't assign 's', from outside it")
  expect err("fn f() \{ parallel for i in 0..4 \{ return 1 } }").has("can't return")
  expect err("parallel for i in 0..4 \{ break }").has("can't break")
  expect err("var b = f64s(2)\nparallel for i in 0..2 \{ b[i] = i }\nprint(b)") == ""
}
