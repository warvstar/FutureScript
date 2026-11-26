// Effects: a function declares what it may perform after its result type (`! io, clock`; `!` alone: nothing).
// The declaration is checked at run time: on entry (the caller must be allowed the effects) and wherever an
// operation is performed. Handlers answer operations in place: `handle { } with print(s) { }`.
import "../mods/audit.fut" as audit
extern fn hypot(num, num): num

fn save(name): bool ! io { print("saved {name}") return true }
fn double(x: num): num ! { return x * 2 }
fn stamp() ! clock, random { return clock() >= 0 && random() < 1 }
fn helper() { print("from helper") }            // undeclared: runs under whatever its caller allows
fn quiet() ! { helper() }
fn timed() ! clock { return save("t") }
fn measure() ! io { return clock() }
fn tellTime() ! io, clock { return clock() }
fn ffi() ! { return hypot(3, 4) }

test "a declared function may perform what it declares" {
  expect prints { expect save("a") } == "saved a\n"
  expect double(4) == 8
  expect stamp()
}
test "performing an undeclared effect is an error where it happens, naming the declaration" {
  expect throws { quiet() } == "print() performs io, which quiet() does not declare (it declares no effects)"
  expect throws { measure() } == "clock() performs clock, which measure() does not declare (it declares ! io)"
  expect throws { ffi() } == "hypot() performs io, which ffi() does not declare (it declares no effects)"
}
test "calling a function that declares more than the caller may do is an error on entry" {
  expect throws { timed() } == "save() declares io, but it is called from inside timed(), which does not (it declares ! clock)"
}
test "what is allowed comes back after a return and after a throw" {
  expect tellTime() >= 0
  try { quiet() } catch e { }
  expect prints { print("still allowed") } == "still allowed\n"
  var inner = fn() ! { throw "out" }
  var outer = fn() ! clock {
    try { inner() } catch e { }
    return clock()
  }
  expect outer() >= 0
}
test "undeclared code is unchecked" {
  expect prints { helper() } == "from helper\n"
}

// ---- handlers ----
var clockLog = []
fn now() ! clock { return clock() }
test "a handler's result is the operation's result" {
  var t = 0
  handle { t = now() } with clock() { return 1000 }
  expect t == 1000
}
test "handlers make print, clock, sleep and random deterministic" {
  var out = []
  var r = handle_all(out)
  expect out == ["saved x", "slept 5"]
  expect r == [42, 0.5]
}
fn handle_all(out) {
  var got = []
  handle {
    save("x")
    sleep(5)
    got.push(clock())
    got.push(random())
  } with print(s) { out.push(s) } with sleep(s) { out.push("slept {s}") } with clock() { return 42 } with random() { return 0.5 }
  return got
}
test "the nearest handler answers; a handler's own operations go to the handlers outside it" {
  var out = []
  handle {
    handle { print("inner") } with print(s) { print("[{s}]") }
  } with print(s) { out.push(s) }
  expect out == ["[inner]"]
}
test "a handler that throws aborts the operation like any throw" {
  var after = false
  expect throws { handle { print("x") after = true } with print(s) { throw "refused {s}" } } == "refused x"
  expect after == false
  expect prints { print("handlers are gone") } == "handlers are gone\n"
}
test "return and break leave a handle body, removing its handler" {
  expect early() == 1
  for i in 0..3 { handle { if i == 1 { break } } with clock() { return 0 } }
  expect clock() > 0
}
fn early() { handle { return 1 } with clock() { return 0 } }
fn selfContained() ! {
  var out = []
  handle { print("kept") helper() } with print(s) { out.push(s) }
  return out
}
test "a function declared ! may print inside a handler it installs itself" {
  expect selfContained() == ["kept", "from helper"]
}
test "a handler outside a function that declares the effect replaces it there (tests fake the outside world)" {
  var out = []
  handle { save("mock") } with print(s) { out.push(s) }
  expect out == ["saved mock"]
}
fn sealed() ! { print("sealed") }
test "a handler outside a function that does not declare the effect does not make it allowed" {
  expect throws { handle { sealed() } with print(s) { } } == "print() performs io, which sealed() does not declare (it declares no effects)"
}
fn napper() ! clock { sleep(100) return clock() }
fn faked() ! {
  var t = 0
  handle { t = napper() } with clock() { return 7 } with sleep(s) { }
  return t
}
test "a handle that answers every operation of an effect allows that effect inside it" {
  expect faked() == 7
}
fn halfFaked() ! { handle { napper() } with clock() { return 7 } }
test "answering only some operations does not" {
  expect throws { halfFaked() } == "napper() declares clock, but it is called from inside halfFaked(), which does not (it declares no effects)"
}

// ---- user effects ----
effect Log {
  fn log(msg: str)
  fn level(): num
}
fn work() ! Log {
  Log.log("start")
  if Log.level() > 1 { Log.log("detail") }
  return "done"
}
fn runWork() ! {
  var lines = []
  var r = nil
  handle { r = work() } with Log.log(m) { lines.push(m) } with Log.level() { return 2 }
  return [r, lines]
}
test "an effect's operations are performed by name and answered by the nearest handler" {
  expect runWork() == ["done", ["start", "detail"]]
}
test "performing an operation nothing handles is an error" {
  expect throws { work() } == "Log.log() was performed, but nothing handles it (handle \{ ... } with Log.log(...) \{ ... })"
}
test "an operation's declared types are checked" {
  expect throws { handle { Log.log(5) } with Log.log(m) { } } == "argument 'msg' of Log.log() must be str, got the number 5"
  expect throws { handle { Log.level() } with Log.level() { return "high" } } == "the result of Log.level() must be num, got the string \"high\""
}
test "a module's effect" {
  var lines = []
  var n = 0
  handle { n = audit.deposit(5) } with audit.Audit.record(l) { lines.push(l) } with audit.Audit.count() { return len(lines) }
  expect lines == ["deposit 5"]
  expect n == 1
}

// ---- fibers ----
test "each fiber has its own handlers" {
  var out = []
  var f = fiber(fn() { print("in fiber") yield 1 })
  expect prints { handle { f.resume() } with print(s) { out.push(s) } } == "in fiber\n"
  expect out == []
  var g = fiber(fn() { handle { print("a") yield 1 print("b") } with print(s) { out.push(s) } })
  g.resume()
  print("between")
  g.resume()
  expect out == ["a", "b"]
}
test "a fiber may do what the code that created it may do" {
  var make = fn() ! { return fiber(fn() { print("x") }) }
  var f = make()
  expect throws { f.resume() } == "print() performs io, which the function at line 166 does not declare (it declares no effects)"
}
test "a handler can answer yield in place" {
  var got = []
  handle { got.push(yield 1) got.push(yield 2) } with yield(v) { return v * 10 }
  expect got == [10, 20]
}
fn producer() ! { yield 1 }
test "yield is an effect" {
  var f = fiber(fn() { producer() })
  expect throws { f.resume() } == "yield performs yield, which producer() does not declare (it declares no effects)"
}

// ---- atomic ----
test "io inside atomic { } is an error: it can't be rolled back" {
  expect throws { atomic { print("x") } } == "print() performs io, which can't be undone inside atomic \{ } (do it after the block, or handle it inside the block)"
  expect throws { atomic { save("x") } } == "save() declares io, which can't be undone inside atomic \{ }: it can't be called inside the block"
}
test "unless a handler installed inside the block answers it" {
  var out = []
  try { atomic { handle { print("x") } with print(s) { out.push(s) } throw "fail" } } catch e { }
  expect out == []      // rolled back with the block
  atomic { handle { print("y") } with print(s) { out.push(s) } }
  expect out == ["y"]
  expect throws { handle { atomic { print("z") } } with print(s) { out.push(s) } } == "print() performs io, which can't be undone inside atomic \{ } (do it after the block, or handle it inside the block)"
}
test "other effects are allowed inside atomic { }" {
  atomic { expect clock() > 0 }
}

// ---- odds and ends ----
test "random() is in [0, 1)" {
  for i in 0..100 { var r = random() expect r >= 0 && r < 1 }
}
test "effect, handle and with stay usable as names" {
  var effect = 1
  var handle = 2
  var with = 3
  expect effect + handle + with == 6
}
test "an uncaught effect error names the place it was performed" {
  var r = run("progs/effects_uncaught.fut")
  expect r.code == 1
  expect r.err.has("runtime error: print() performs io, which report() does not declare (it declares ! clock)\n")
  expect r.err.has("progs/effects_uncaught.fut:2)")
}
test "compile errors" {
  var bad = fn(src) { return run({source: src}).err }
  expect bad("fn f() ! net \{ }").has("unknown effect 'net'")
  expect bad("effect E \{ fn op(a) }\nhandle \{ } with E.op(a, b) \{ }").has("a handler for E.op() takes 1 parameters, not 2")
  expect bad("effect E \{ fn op() \{ } }").has("an effect's operations have no body")
  expect bad("handle \{ } with str(x) \{ }").has("'str' is not an effect's operation")
  expect bad("effect E \{ fn op() }\nE.nope()").has("effect 'E' has no operation 'nope'")
  expect bad("effect io \{ }").has("'io' is a builtin effect")
}
