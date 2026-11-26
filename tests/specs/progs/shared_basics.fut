// shared.fut runs this in a process of its own: a shared var is every isolate's, so the isolates that `build.py
// test` runs each spec in would share these too.
shared var Count = 0
shared var Names = []
shared var Config = {size: 1}
fn bump(n) { for i in 0..n { atomic { Count += 1 } }  return n }
fn add(name) { atomic { Names = Names + [name] } }
fn first() { atomic { for x in Names { if x == "b" { return x } } } }
fn plusOne() { atomic { Count += 1 } }
fn errorOf(f) {
  try { f() } catch e { return "{e}" }
  return nil
}
fn check(what, ok) { if !ok { print("FAILED: {what}") } }

// every isolate's
var ts = []
for i in 0..4 { ts.push(spawn bump(100)) }
for t in ts { t.join() }
check("4 isolates add 100 each", Count == 400)
for n in ["a", "b", "c"] { spawn add(n).join() }
check("names from 3 isolates", Names.len() == 3)
check("return from inside atomic", first() == "b")

// frozen: assigning freezes a copy
var c = {size: 2, tags: ["x"]}
atomic { Config = c }
c.size = 3
check("a copy", Config.size == 2)
check("frozen", isFrozen(Config) && isFrozen(Config.tags))
check("changing it raises", errorOf(fn() { Config.size = 4 }).has("frozen"))
var mine = thaw(Config)
mine.size = 5
atomic { Config = mine }
check("thawed, changed, assigned", Config.size == 5)

// assigned only inside atomic, and rolled back with it
check("outside atomic", errorOf(fn() { Count = 1 }).has("only inside atomic"))
var n = Count
check("an error", errorOf(fn() { atomic { Count = -1  plusOne()  var x = nil  x.y } }) != nil)
check("rolled back", Count == n)
atomic { Count = 7  atomic { Count += 1 }  plusOne() }
check("nested blocks join", Count == 9)
for i in 0..3 { atomic { if i == 1 { continue }  Count += 10 } }
check("continue commits", Count == 29)
print("done")
