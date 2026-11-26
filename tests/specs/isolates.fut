// Isolates and channels: spawn f(args) runs f in an isolate of its own, join() waits for its result, channel(n)
// carries copies of values between isolates. In wasm too: on one thread (fut.wasm), or on workers (fut-mt.wasm).
fn square(x) { return x * x }
fn worker(jobs, results) ! chan {
  for j in jobs { results.send(j * j) }
}
class Point {
  var x = 0
  var y = 0
  fn init(x, y) { self.x = x  self.y = y }
  fn sum() { return self.x + self.y }
}
var G = [1, 2, 3]
fn sendsUndeclared(c) ! io { c.send(1) }
fn spawnsUndeclared() ! chan { return spawn square(2) }
fn declared(c) ! chan, spawn { c.send(5)  return spawn square(3) }
fn errorOf(f) {
  try { f() } catch e { return e }
  return nil
}

test "spawn, then join for the result (as often as you like)" {
  var t = spawn square(7)
  expect t.join() == 49
  expect t.join() == 49
  expect t.done()
  expect cores() >= 1
}
test "a pool of workers over channels" {
  var jobs = channel(8)
  var results = channel(32)
  var ws = []
  for i in 0..4 { ws.push(spawn worker(jobs, results)) }
  for j in 1..21 { jobs.send(j) }
  jobs.close()
  var total = 0
  for i in 0..20 { total += results.recv() }
  for w in ws { w.join() }
  expect total == 2870
}
test "a send copies: later changes stay behind, sharing and cycles come along" {
  var a = [1, 2]
  var m = {list: a, again: a}
  m["me"] = m
  var c = channel(1)
  c.send(m)
  a.push(3)
  var got = c.recv()
  expect len(got["list"]) == 2
  got["list"].push(9)
  expect len(got["again"]) == 3
  expect got["me"]["list"][2] == 9
  expect len(a) == 3
}
test "instances and closures cross, with their class and code" {
  var c = channel(2)
  c.send(Point(1, 2))
  expect c.recv().sum() == 3
  var k = 10
  c.send(fn(x) { return x + k })
  var f = c.recv()
  expect f(5) == 15
  var t = spawn fn(n) { return Point(n, n) }(4)
  expect t.join().sum() == 8
}
test "an isolate starts with a copy of the globals; its changes stay its own" {
  G.push(4)
  var t = spawn fn() { G.push(99)  return len(G) }()
  expect t.join() == 5
  expect len(G) == 4
}
test "an isolate's error is raised again by join" {
  var t = spawn fn() { throw "boom" }()
  expect throws { t.join() } == "boom"
  var u = spawn fn() { return fiber(fn() { return 1 }) }()
  expect throws { u.join() } == "cannot send a fiber: it runs on its own isolate's stacks"
}
test "channel(0): each send waits for its receiver" {
  var c = channel(0)
  var t = spawn fn(c) { c.send(1)  c.send(2)  return "sent" }(c)
  expect c.recv() == 1
  expect c.recv() == 2
  expect t.join() == "sent"
}
test "close ends a for loop over the channel; after it, recv and send raise" {
  var c = channel(4)
  var t = spawn fn(c) { for i in 0..10 { c.send(i) }  c.close() }(c)
  var s = 0
  for v in c { s += v }
  expect s == 45
  expect throws { c.recv() } == "recv() on a closed channel, and nothing is left in it"
  expect throws { c.send(1) } == "send() on a closed channel"
  expect throws { c.close() } == "close() of a channel already closed"
  expect c.tryRecv() == nil
  t.join()
}
test "the same channel is the same value, however it arrived" {
  var c = channel(2)
  var d = channel(1)
  d.send(c)
  expect d.recv() == c
  var t = spawn fn(c) { return c }(c)
  expect t.join() == c
}
test "waiting when nothing can ever answer is a deadlock, which names each wait" {
  var c = channel(1)
  var e = errorOf(fn() { c.recv() })
  expect e.has("deadlock: every isolate of this program is waiting")
  expect e.has("isolate 1 (main), waits in recv()")
  var a = channel(0)
  var t = spawn fn(a) { a.recv() }(a)
  e = errorOf(fn() { t.join() })
  expect e.has("isolate 1 (main), waits in join()")
  expect e.has("waits in recv()")
}
test "effects: spawn and chan are declared like io" {
  var c = channel(2)
  expect throws { sendsUndeclared(c) } == "send() performs chan, which sendsUndeclared() does not declare (it declares ! io)"
  expect throws { spawnsUndeclared() } == "spawn performs spawn, which spawnsUndeclared() does not declare (it declares ! chan)"
  expect declared(c).join() == 9
  expect c.recv() == 5
}
test "inside atomic: a send waits for the commit, and one rolled back never happens" {
  var c = channel(4)
  atomic {
    c.send(1)
    expect c.tryRecv() == nil
  }
  expect c.recv() == 1
  try { atomic { c.send(2)  throw "no" } } catch e { }
  expect c.tryRecv() == nil
  expect errorOf(fn() { atomic { c.recv() } }).has("recv() inside atomic \{ }")
  expect errorOf(fn() { atomic { spawn square(1) } }).has("spawn inside atomic \{ }")
}
test "a program that deadlocks, or loses a failure, says so" {
  var r = run({file: "progs/isolates_deadlock.fut"})
  expect r.code == 1
  expect r.err.has("deadlock: every isolate of this program is waiting")
  expect r.err.has("isolate 2, spawned at main (")
  r = run({file: "progs/isolates_unjoined.fut"})
  expect r.code == 1
  expect r.out == "main done\n"
  expect r.err.has("isolate 2, spawned at main (")
  expect r.err.has("failed and nothing joined it: lost")
}
test "seeded (fut test, --seed n): the same seed, the same interleaving; other seeds try others" {
  var a = run({file: "progs/isolates_order.fut", args: ["--seed", "7"]})
  expect len(a.out) == 13
  expect run({file: "progs/isolates_order.fut", args: ["--seed", "7"]}).out == a.out
  var seen = {}
  for s in 1..4 { seen[run({file: "progs/isolates_order.fut", args: ["--seed", str(s)]}).out] = true }
  expect len(seen.keys()) > 1
  var r = run({file: "progs/isolates_seedfail.fut", args: ["test"]})
  expect r.out.has("its isolates ran under seed 1: --seed 1 replays the same interleaving")
}
test "pooled (the default): thousands of isolates, and busy ones take turns on the threads" {
  expect run({file: "progs/isolates_many.fut"}).out == "8997000\n"
  expect run({file: "progs/isolates_preempt.fut", args: ["--threads", "2"], timeout: 60}).out == "all done\n"
}
