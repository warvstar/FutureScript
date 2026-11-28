// A var may declare a type too: checked when it is initialised and at every store, from closures as well. A store
// the compiler can see always fails (a literal, or a declared type that can't fit) is a compile error instead
// (specs/clashes.fut), so these go through opaque(), which hides what they are.
import "mods/geom.fut" as geom
fn opaque(v) { return v }
fn setTotal(n) { total = n }       // above the declaration: still checked
var total: num = 0
var name: str? = nil
var anything: any = 1
anything = "fine"
setTotal(5)
print(total)                                     // expect: 5
try { setTotal("5") } catch e { print(e) }       // expect: var 'total' must be num, got the string "5"
try { total += "x" } catch e { print(e) }        // expect: var 'total' must be num, got the string "5x"
print(total)                                     // expect: 5
name = "ann"
name = nil
try { name = opaque(3) } catch e { print(e) }     // expect: var 'name' must be str?, got the number 3
try { geom.calls = opaque("many") } catch e { print(e) } // expect: var 'geom.calls' must be num, got the string "many"

fn locals() {
  var i: num = 0
  var s: str = "a"
  var xs: list = []
  while i < 3 { i += 1 s += "b" xs.push(i) }
  print("{i} {s} {xs}")                          // expect: 3 abbb [1, 2, 3]
  try { i = opaque("three") } catch e { print(e) } // expect: var 'i' must be num, got the string "three"
  try { s = opaque(xs) } catch e { print(e) }     // expect: var 's' must be str, got a list
  try { i += s } catch e { print(e) }            // expect: var 'i' must be num, got the string "3abbb"
  print(i * 2)                                   // expect: 6
  var bump = fn() { i = opaque(nil) }
  try { bump() } catch e { print(e) }            // expect: var 'i' must be num, got nil
  var set = fn(v) { s = v }
  set("ok")                                      // its own copy (closures capture by value), still typed
  print(s)                                       // expect: abbb
  try { set(1) } catch e { print(e) }            // expect: var 's' must be str, got the number 1
  var i = "again"                                // a redeclaration takes its own type (none)
  i = nil
  print(i)                                       // expect: nil
}
locals()

fn param(n: num, label: str) {
  n = n * 2
  try { n = opaque(label) } catch e { print(e) }   // expect: argument 'n' of param() must be num, got the string "x"
  try { label = opaque(n) } catch e { print(e) }   // expect: argument 'label' of param() must be str, got the number 8
  print(n)                                       // expect: 8
}
param(4, "x")

class Box { var v = 0 }
fn boxes() {
  var b: Box = Box()
  b.v = 2
  try { b = opaque("box") } catch e { print(e) }   // expect: var 'b' must be Box, got the string "box"
  var f: fn = fn() { return b.v }
  print(f())                                     // expect: 2
}
boxes()

fn sum(n) {                                      // a typed number local in a loop stays in a register
  var t: num = 0
  for k in 0..n { t += k }
  return t
}
print(sum(1000))                                 // expect: 499500
