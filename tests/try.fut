// try/catch/throw: runtime errors and thrown values are caught by the innermost try, at any call depth
fn risky(x) { if x > 2 { throw "too big: {x}" } return x * 10 }
fn safe(x) {
  var n = 0
  try { n = 1  n = risky(x) } catch e { print("caught {e} with n={n}") return -1 }
  return n
}
print(safe(1))                     // expect: 10
print(safe(5))                     // expect: caught too big: 5 with n=1
                                   // expect: -1
try { nil.x } catch e { print(e) } // expect: nil has no field 'x'
try { throw {code: 42} } catch e { print(e.code) }   // expect: 42
var count = 0
for i in 0..5 { try { if i % 2 == 0 { throw i } count += 1 } catch e { count += 10 } }
print(count)                       // expect: 32
fn nested() { try { try { throw "inner" } catch e { throw e + "!" } } catch e { return e } }
print(nested())                    // expect: inner!
fn early() { try { return "returned" } catch e { } }
print(early())                     // expect: returned
for i in 0..3 { try { if i == 1 { break } } catch e { } }
try { throw "after break" } catch e { print(e) }     // expect: after break
fn deep(n) { if n == 0 { throw "deep" } return deep(n - 1) }
try { deep(50) } catch e { print(e) }                // expect: deep
var g = fiber(fn() { yield 1 throw "in fiber" })
g.resume()
try { g.resume() } catch e { print("from fiber: {e}") }   // expect: from fiber: in fiber
fn swallow() { try { throw 1 } catch { } return "ok" }
print(swallow())                   // expect: ok
throw "uncaught"                   // error: runtime error: uncaught
