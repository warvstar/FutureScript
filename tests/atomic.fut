// atomic { }: all of its changes to shared state happen, or (if an error leaves it) none do.
class Account {
  fn init(b) { self.balance = b }
}
fn transfer(from, to, n) {
  atomic {
    from.balance -= n
    to.balance += n
    if from.balance < 0 { throw "insufficient funds" }
  }
}
var a = Account(100)
var b = Account(0)
transfer(a, b, 30)
print("{a.balance} {b.balance}")  // expect: 70 30
try { transfer(a, b, 100) } catch e { print(e) }  // expect: insufficient funds
print("{a.balance} {b.balance}")  // expect: 70 30

// lists, maps and globals
var items = [1, 2, 3]
var stock = {apples: 3, pears: 1}
var count = 0
try {
  atomic {
    items.push(4)
    items[0] = 99
    items.pop()
    items.pop()
    stock["apples"] = 0
    stock.remove("pears")
    stock["plums"] = 7
    count = 5
    nil.x  // a runtime error rolls back too
  }
} catch e { print(e) }  // expect: nil has no field 'x'
print(items)  // expect: [1, 2, 3]
print(stock)  // expect: {apples: 3, pears: 1}
print(count)  // expect: 0

// captured variables
fn counter() {
  var n = 0
  return fn() { n += 1 return n }
}
var next = counter()
next()
try { atomic { next() next() throw "no" } } catch e {}
print(next())  // expect: 2

// nested: an inner failure caught inside the outer rolls back only the inner block
var log = []
atomic {
  log.push("outer")
  try { atomic { log.push("inner") throw "x" } } catch e { log.push("caught") }
}
print(log)  // expect: ["outer", "caught"]
// an outer failure rolls back what inner blocks committed
try { atomic { atomic { log.push("kept?") } throw "outer fails" } } catch e {}
print(log)  // expect: ["outer", "caught"]

// return, break and continue commit
fn add(l) {
  atomic {
    l.push(len(l))
    return len(l)
  }
}
var l = []
print(add(l))  // expect: 1
for i in 0..5 {
  atomic {
    if i == 1 { continue }
    l.push(i)
    if i == 3 { break }
  }
}
print(l)  // expect: [0, 0, 2, 3]
try { atomic { l.push(9) throw "x" } } catch e {}
print(l)  // expect: [0, 0, 2, 3]

// a store site whose inline cache was filled outside still saves the instance inside (also after tier 2)
fn setBal(acct, v) { acct.balance = v }
for i in 0..3000 { setBal(b, i) }
try { atomic { for i in 0..3000 { setBal(b, -i) } throw "x" } } catch e {}
print(b.balance)  // expect: 2999

// a field first assigned inside is gone again
var p = Account(1)
try { atomic { p.extra = 1 throw "x" } } catch e {}
try { print(p.extra) } catch e { print(e) }  // expect: instance has no field 'extra'

// an error from deep inside, caught far outside
fn deep(n) { if n == 0 { throw "bottom" } a.balance += 1 deep(n - 1) }
try { atomic { deep(50) } } catch e { print("{e} {a.balance}") }  // expect: bottom 70

// a fiber may not yield inside its own atomic block
fn pause() { yield 1 }
var f = fiber(fn() { atomic { pause() } })
try { f.resume() } catch e { print(e) }  // expect: yield inside atomic { }: other code would run while it is half done (yield after it ends)
// but may be resumed from inside one: its changes belong to that block
var g = fiber(fn() { a.balance = 0 yield 1 })
try { atomic { g.resume() throw "x" } } catch e {}
print(a.balance)  // expect: 70
