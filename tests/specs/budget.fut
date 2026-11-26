// budget(n, f): f() runs at most n steps, a step being a loop iteration or a call. The count is the same on every
// platform and in every run, warm or cold, so a program cut off by a budget is cut off at the same point everywhere.
import "std/lists"

fn spin(n) { var i = 0 while i < n { i += 1 } return i }
fn deep(n) { if n == 0 { return 0 } return deep(n - 1) + 1 }
fn fan(d) { if d == 0 { return 1 } return fan(d - 1) + fan(d - 1) }
class P { var x = 0 fn bump(n) { for i in 0..n { self.x += i } return self.x } }
fn fib(n) { if n < 2 { return n } return fib(n - 1) + fib(n - 2) }
fn work() {  // calls, methods, closures called back by std, maps, a fiber
  var l = []
  for i in 0..50 { l.push((i * 37) % 11) }
  l = lists.sortBy(l, fn(a) { return -a })
  var m = {}
  for k in l { m[str(k)] = k }
  var t = 0
  for k in m { t += m[k] }
  var f = fiber(fn(x) { var y = x while true { y = yield y * 2 } })
  for i in 0..5 { t += f.send(i) }
  return P().bump(20) + fib(12) + t
}
fn fails(f) { try { f() } catch e { return e } return "" }
var RAN = "ran out of steps: the budget was "

test "a step is a loop iteration or a call" {
  expect budget(100, fn() { return spin(10) }) == 10
  expect budget(100, fn() { spin(10) return stepCount() }) == 12  // the closure, spin, 10 iterations
  expect budget(100, fn() { return stepCount() }) == 1
  expect budget(1000, fn() { return budget(1000, fn() { return spin(3) }) + stepCount() }) == 3 + 6
}

test "the same count every run: cold, warm, in either build" {
  var counts = []
  for r in 0..300 { counts.push(budget(1e9, fn() { work() return stepCount() })) }
  expect counts[0] == 1269
  expect lists.all(counts, fn(c) { return c == 1269 })
}

test "running out raises at the step that ran out" {
  expect throws { budget(50, fn() { spin(1000) }) } == RAN + "50 (each loop iteration and each call is a step)"
  expect fails(fn() { budget(50, fn() { deep(1000) }) }).has(RAN + "50")
  expect fails(fn() { budget(5000, fn() { fan(20) }) }).has(RAN + "5000")
  expect budget(12, fn() { return spin(10) }) == 10
  expect fails(fn() { budget(11, fn() { spin(10) }) }).has(RAN + "11")
  expect budget(1, fn() { return len(str([1, 2, 3])) }) == 9  // a builtin's own work is no step, only fut code it calls
}

test "an inner budget takes at most what the outer has left" {
  expect fails(fn() { budget(1000, fn() { budget(5000, fn() { spin(2000) }) }) }).has(RAN + "1000")
  expect fails(fn() { budget(5000, fn() { budget(1000, fn() { spin(2000) }) }) }).has(RAN + "1000")
}

test "code inside a budget can't get past running out" {
  var caught = []
  var e = fails(fn() { budget(100, fn() { for i in 0..10 { try { spin(1000) } catch e { caught.push(e) } } }) })
  expect e.has(RAN + "100") && caught.len() == 1  // the loop's next iteration was a step too
}

test "leaving a budget gives back what it didn't use" {
  var used = budget(100, fn() {
    try { budget(10, fn() { spin(100) }) } catch e { }
    return stepCount()
  })
  expect used == 12  // its closure, then the inner budget's 11 (its closure, spin, 9 iterations: the 11th ran out)
  var after = budget(100, fn() {
    budget(50, fn() { return spin(3) })
    return stepCount()
  })
  expect after == 1 + 5
}

test "a fiber can't yield out of a budget it opened" {
  var f = fiber(fn() { budget(100, fn() { yield 1 }) })
  expect fails(fn() { f.resume() }).has("yield inside budget()")
  var g = fiber(fn() { while true { yield spin(5) } })
  expect budget(100, fn() { return g.resume() + g.resume() }) == 10
}

test "arguments" {
  expect throws { budget(-1, fn() { }) } == "budget() expects a whole number of steps, got -1"
  expect throws { budget(1.5, fn() { }) } == "budget() expects a whole number of steps, got 1.5"
  expect throws { budget(10, 3) } == "budget() expects a function to run, got number"
  expect throws { budget(10, fn(x) { }) } == "budget()'s function must take no parameters"
}
