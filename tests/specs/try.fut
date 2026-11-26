// try/catch/throw: runtime errors and thrown values are caught by the innermost try, at any call depth
fn risky(x) { if x > 2 { throw "too big: {x}" } return x * 10 }
fn safe(x) {
  var n = 0
  try { n = 1  n = risky(x) } catch e { print("caught {e} with n={n}") return -1 }
  return n
}
fn nested() { try { try { throw "inner" } catch e { throw e + "!" } } catch e { return e } }
fn early() { try { return "returned" } catch e { } }
fn deep(n) { if n == 0 { throw "deep" } return deep(n - 1) }
fn swallow() { try { throw 1 } catch { } return "ok" }

test "a throw from a call is caught, and the catch sees the latest locals" {
  expect safe(1) == 10
  var r = nil
  expect prints { r = safe(5) } == "caught too big: 5 with n=1\n"
  expect r == -1
}
test "runtime errors are caught as their message" {
  expect prints { try { nil.x } catch e { print(e) } } == "nil has no field 'x'\n"
}
test "any value can be thrown" {
  expect prints { try { throw {code: 42} } catch e { print(e.code) } } == "42\n"
}
test "try in a loop" {
  var count = 0
  for i in 0..5 { try { if i % 2 == 0 { throw i } count += 1 } catch e { count += 10 } }
  expect count == 32
}
test "rethrow from a catch reaches the outer try" {
  expect nested() == "inner!"
}
test "return and break leave a try cleanly" {
  expect early() == "returned"
  for i in 0..3 { try { if i == 1 { break } } catch e { } }
  expect prints { try { throw "after break" } catch e { print(e) } } == "after break\n"
}
test "a throw unwinds through many calls" {
  expect prints { try { deep(50) } catch e { print(e) } } == "deep\n"
}
test "a throw in a fiber is raised again in its resumer" {
  var g = fiber(fn() { yield 1 throw "in fiber" })
  g.resume()
  expect prints { try { g.resume() } catch e { print("from fiber: {e}") } } == "from fiber: in fiber\n"
}
test "the catch name is optional" {
  expect swallow() == "ok"
}
test "an uncaught throw stops the program" {
  // progs/try_uncaught.fut: print("before") throw "uncaught" print("after")
  var r = run("progs/try_uncaught.fut")
  expect r.code == 1
  expect r.out == "before\n"
  expect r.err.has("runtime error: uncaught\n")
  expect r.err.has("progs/try_uncaught.fut:2)")
}
