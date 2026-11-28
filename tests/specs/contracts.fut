// Contracts, written on the declaration: requires is checked on entry (a failure is the caller's fault), ensures on
// exit (the implementation's), with old(expr) as it was on entry and result as the value returned; a class's
// invariant holds whenever an outside call on an instance ends. One clause per line, between the signature and '{'.

class Account {
  var balance = 0
  invariant self.balance >= 0
  fn init(balance: num)
    requires balance >= 0
  {
    self.balance = balance
  }
  fn withdraw(n: num): num !
    requires n > 0
    requires n <= self.balance
    ensures self.balance == old(self.balance) - n
    ensures result == self.balance
  {
    self.balance -= n
    return self.balance
  }
  fn overdraw() { self.balance = -1 }
  fn rebalance() {        // calls its own methods partway through a change: the invariant is not checked there
    self.overdraw()
    self.balance = 0
  }
  fn fail() { self.balance = -5; throw "failed" }
  fn noop() { }
}

test "a contract that holds changes nothing" {
  var a = Account(10)
  expect a.withdraw(3) == 7
  expect a.balance == 7
}

test "requires: the caller's fault, naming the part that failed and its values" {
  var a = Account(10)
  expect throws { a.withdraw(50) } == "the caller broke Account.withdraw()'s contract: requires n <= self.balance (n is 50, self.balance is 10)"
  expect throws { a.withdraw(0) } == "the caller broke Account.withdraw()'s contract: requires n > 0 (n is 0)"
  expect throws { Account(-1) } == "the caller broke Account.init()'s contract: requires balance >= 0 (balance is -1)"
  expect a.balance == 10
}

test "invariant: checked when an outside call ends" {
  var a = Account(10)
  expect throws { a.overdraw() } == "Account.overdraw() broke Account's invariant: self.balance >= 0 (self.balance is -1)"
  var b = Account(10)
  b.rebalance()
  expect b.balance == 0
}

test "invariant: a call that throws is over, so the next one is checked" {
  var a = Account(10)
  expect throws { a.fail() } == "failed"
  expect throws { a.noop() } == "Account.noop() broke Account's invariant: self.balance >= 0 (self.balance is -5)"
}

fn half(x): num   // (untyped: fut test generates no property test, which would find the bug below)
  requires x >= 0
  ensures result * 2 == x
{
  if x > 100 { return 0 }
  return x / 2
}

fn bump(n)
  ensures result == n + 1   // n: the argument as it was passed, though the body assigns it
{
  n += 1
  return n
}

test "ensures: the implementation's fault" {
  expect half(8) == 4
  expect throws { half(200) } == "half() broke its contract: ensures result * 2 == x (result * 2 is 0, x is 200)"
  expect bump(1) == 2
}

test "&& parts are checked one by one; a condition with || is checked as one" {
  expect throws { both(5) } == "the caller broke both()'s contract: requires n < 3 (n is 5)"
  expect throws { pick(5) } == "pick() broke its contract: ensures result == 0 || result > n"
}
fn both(n) requires n > 0 && n < 3 { return n }
fn pick(n) ensures result == 0 || result > n { return 1 }

// Checking a contract can't change what the program does: a clause that calls code runs with no effects allowed,
// and what it changed is put back.
fn first(l) requires l.pop() == 2 { return l.len() }
fn loud(x) requires print(x) == nil { return x }
fn every(l, f) { for x in l { if !f(x) { return false } } return true }
fn positive(l) requires every(l, fn(x) { return x > 0 }) { return true }   // (a lambda's braces in a clause)

test "clauses are rolled back and perform no effects" {
  var l = [1, 2]
  expect first(l) == 2
  expect l == [1, 2]
  expect throws { loud(1) } == "print() performs io, which the contract of loud() does not declare (it declares no effects)"
  expect positive([1, 2])
  expect throws { positive([1, -2]) } == "the caller broke positive()'s contract: requires every(l, fn(x) \{ return x > 0 })"
}

class Stack {
  var items = nil
  var count = 0
  invariant self.size() == self.items.len()
  fn init() { self.items = [] }
  fn size() { return self.count }
  fn top() { return self.items[self.items.len() - 1] }
  fn push(x)
    ensures self.size() == old(self.size()) + 1
    ensures self.top() == x
  {
    self.items.push(x)
    self.count += 1
  }
  fn pop()
    requires self.size() > 0
    ensures self.size() == old(self.size()) - 1
  {
    self.count -= 1
    return self.items.pop()
  }
}

test "clauses may call methods, which see the object mid-call" {
  var s = Stack()
  s.push(1)
  s.push(2)
  expect s.pop() == 2
  expect s.size() == 1
  expect throws { Stack().pop() } == "the caller broke Stack.pop()'s contract: requires self.size() > 0 (self.size() is 0)"
}

// A subclass keeps its superclass's invariants and may add its own; every method checks them all, wherever it is
// defined. A store to a field from outside the object checks them too.
class Purse {
  var cash: num = 0
  var limit: num = 100
  invariant self.cash >= 0
  invariant self.cash <= self.limit
  fn put(n: num)
    requires n > 0
    requires self.cash + n <= self.limit
  {
    self.cash += n
  }
  fn take(n: num)
    requires n > 0
    requires n <= self.cash
  {
    self.cash -= n
  }
}
class Savings : Purse {
  var floor: num = 10
  invariant self.cash == 0 || self.cash >= self.floor
  fn splurge(n) { self.cash += n }   // (untyped: random calls leave it out)
}
class Plain { var x = 0 }

test "invariants: inherited, and checked after an inherited method" {
  var s = Savings()
  s.put(50)
  expect throws { s.take(45) } == "Purse.take() broke Savings's invariant: self.cash == 0 || self.cash >= self.floor"
  var t = Savings()
  expect throws { t.splurge(1000) } == "Savings.splurge() broke Purse's invariant: self.cash <= self.limit (self.cash is 1000, self.limit is 100)"
}

test "invariants: a store to a field from outside is checked" {
  var p = Purse()
  p.cash = 5
  expect throws { p.cash = 500 } == "storing to field 'cash' broke Purse's invariant: self.cash <= self.limit (self.cash is 500, self.limit is 100)"
  var q = Plain()
  q.x = 3   // (a class without invariants: nothing to check)
  expect q.x == 3
}

test "invariants: with atomic, a call that breaks one changes nothing" {
  var s = Savings()
  s.put(50)
  try { atomic { s.take(45) } } catch e { }
  expect s.cash == 50
}

test "a pub declaration's contract may use only pub names (the .api shows it)" {
  var r = run("progs/contract_private.fut")
  expect r.code == 1
  expect r.err.has("contract_private.fut:4:11: error: the contract of spread(), a pub function, can only use what its readers can see, and 'total' is private")
}

fn fill(p, amounts) { for n in amounts { p.cash = n } }   // one store site: its cache fills on the first store
fn setX(o, v) { o.x = v }
class Boxed {
  var x = 0
  invariant self.x >= 0
}

test "invariants: an outside store is checked on the cache's fast path too, and only for classes that have them" {
  var p = Purse()
  expect throws { fill(p, [1, 2, 3, 500]) } == "storing to field 'cash' broke Purse's invariant: self.cash <= self.limit (self.cash is 500, self.limit is 100)"
  var q = Plain()
  var b = Boxed()
  for i in 0..3 { setX(q, i) setX(b, i) }   // the same site, a class without invariants and one with
  expect q.x == 2
  expect throws { setX(b, -1) } == "storing to field 'x' broke Boxed's invariant: self.x >= 0 (self.x is -1)"
  setX(q, -1)
  expect q.x == -1
}

class Guarded {
  var n = 0
  invariant self.n >= 0
  fn bad(why) { self.n = -1 throw why }
  fn recover(why) {        // catches a throw from its own call: it is still the outside call, so it is checked as it ends
    try { self.bad(why) } catch e { }
  }
  fn fix() { self.n = 0 }
  fn pause(v) {            // mid-change while its fiber is paused (untyped parameters: random calls leave these out)
    self.n = -1
    yield v
    self.n = 0
  }
  fn peek() { return self.n }
}

test "invariants: a caught throw inside a method leaves it the outside call" {
  var g = Guarded()
  expect throws { g.recover("bad") } == "Guarded.recover() broke Guarded's invariant: self.n >= 0 (self.n is -1)"
  g.fix()
  expect g.peek() == 0
}

test "invariants: a fiber paused inside a method doesn't make other callers inner" {
  var g = Guarded()
  var f = fiber(fn() { g.pause(1) })
  f.resume()
  expect throws { g.peek() } == "Guarded.peek() broke Guarded's invariant: self.n >= 0 (self.n is -1)"
  f.resume()
  expect g.peek() == 0
}

fn props(src) { return run({source: src, args: ["test"]}).out }
test "property tests try the numbers the function is written with" {
  var out = props("fn ship(g: num): num\n  ensures g > 500 || result == 499\n\{ if g < 500 \{ return 499 } return 899 }")
  expect out.has("ship(500)\n")
}
test "property tests build class arguments with their init" {
  var src = "class P \{ var xp = 0 var name: str fn init(name: str) \{ self.name = name } }\n" +
    "fn grow(p: P, a: num): num\n  ensures result >= 0\n\{ if p.name == \"\" && a == 7 \{ return -1 } return 0 }"
  expect props(src).has("grow(P(\"\"), 7)\n")
  var never = "class Q \{ fn init(n: num) \{ throw \"no\" } }\nfn f(q: Q): num\n  ensures result == 0\n\{ return 0 }"
  expect props(never).has("1 test: 1 passed")  // none could be built: untested, as before, not a failure
}
test "a broken ensures or invariant says the contract states the intent; a broken requires doesn't" {
  var note = "note: the code and its contract disagree. The contract states what the code is meant to do: fix the code"
  expect props("fn ship(g: num): num\n  ensures g > 500 || result == 499\n\{ if g < 500 \{ return 499 } return 899 }").has(note)
  var calls = "fn f(x)\n  ensures result > 0\n\{ return x }\ntest \"t\" \{ f(0) }"
  expect props(calls).has("runtime error: f() broke its contract: ensures result > 0") && props(calls).has(note)
  var invariant = "class C \{\n  var n = 0\n  invariant self.n >= 0\n  fn dec() \{ self.n -= 1 }\n}\ntest \"t\" \{ C().dec() }"
  expect props(invariant).has(note)
  var caller = "fn f(x)\n  requires x > 0\n\{ return x }\ntest \"t\" \{ f(0) }"
  expect props(caller).has("broke f()'s contract") && !props(caller).has("note:")
}
