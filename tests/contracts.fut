// Contracts: `requires` on a function, `invariant` in a class. Both are checked at run time and perform nothing.
class Account {
  var balance: num = 0
  var limit: num = 100
  invariant self.balance >= 0
  invariant self.balance <= self.limit
  fn deposit(n: num): num requires n > 0 { self.balance += n return self.balance }
  fn withdraw(n: num): num
    requires n > 0, n <= self.balance
  {
    self.balance -= n
    return self.balance
  }
  fn juggle() { self.balance = -1 self.settle() }   // broken in the middle is fine: whole again when it returns
  fn settle() { self.balance = 0 }
  fn spoil() { self.balance = -5 }
}
class Savings : Account {
  var floor: num = 10
  invariant self.balance == 0 || self.balance >= self.floor  // and Account's
}
var a = Account()
print(a.deposit(50))                             // expect: 50
print(a.withdraw(20))                            // expect: 30
try { a.withdraw(40) } catch e { print(e) }      // expect: Account.withdraw() requires n <= self.balance (n is 40, self.balance is 30)
try { a.withdraw(-1) } catch e { print(e) }      // expect: Account.withdraw() requires n > 0 (n is -1)
a.juggle()
print(a.balance)                                 // expect: 0
try { a.spoil() } catch e { print(e) }           // expect: Account's invariant self.balance >= 0 does not hold (self.balance is -5)
a.balance = 5                                    // a store from outside is checked too
try { a.balance = 500 } catch e { print(e) }     // expect: Account's invariant self.balance <= self.limit does not hold (self.balance is 500, self.limit is 100)
var s = Savings()
s.deposit(50)
try { s.withdraw(45) } catch e { print(e) }      // expect: Savings's invariant self.balance == 0 || self.balance >= self.floor does not hold
try { s.balance = -1 } catch e { print(e) }      // expect: Savings's invariant self.balance == 0 || self.balance >= self.floor does not hold
// With atomic, a call that breaks an invariant changes nothing.
var t = Savings()
t.deposit(50)
try { atomic { t.withdraw(45) } } catch e { print("still {t.balance}") }   // expect: still 50
// Contracts perform nothing: no output, clock, randomness, or an effect of the program's.
fn noisy(x) requires print("checking") == nil { return x }
try { noisy(1) } catch e { print(e) }            // expect: print() performs io, which noisy()'s requires does not declare (it declares no effects)
// A failed requires in a test is the code's error, not a failed expect: expect throws sees it.
expect throws { a.withdraw(1000) }
fn half(n: num): num requires n % 2 == 0 { return n / 2 }
var sum = 0
for i in 0..3000 { sum += half(i * 2) }          // hot: tier 2 compiles the requires too
print(sum)                                       // expect: 4498500
class Pair { var lo = 0 var hi = 1 invariant self.lo <= self.hi fn set(l, h) { self.lo = l self.hi = h } }
var p = Pair()
p.set(5, 9)
try { p.set(9, 5) } catch e { print(e) }         // expect: Pair's invariant self.lo <= self.hi does not hold (self.lo is 9, self.hi is 5)
