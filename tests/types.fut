// Types are optional: where one is written (a parameter, a result, a declared field) it is checked at run time.
import "mods/bank.fut" as bank
var a = bank.Account("ann")
var b = bank.Savings("bob")
a.deposit(50)
bank.transfer(a, b, 20)
print("{a.balance} {b.balance}")                 // expect: 30 20
print(bank.find([a, b], "bob").owner)            // expect: bob
print(bank.find([a, b], "cy"))                   // expect: nil
try { bank.transfer(a, "bob", 5) } catch e { print(e) }   // expect: argument 'to' of bank.transfer() must be bank.Account, got the string "bob"
try { a.deposit("5") } catch e { print(e) }               // expect: argument 'n' of bank.Account.deposit() must be num, got the string "5"
try { a.balance = nil } catch e { print(e) }              // expect: field 'balance' of bank.Account must be num, got nil
a.note = "vip"
a.note = nil                                     // str? allows nil
try { a.note = 1 } catch e { print(e) }                   // expect: field 'note' of bank.Account must be str?, got the number 1
print(a.balance)                                 // expect: 30

fn label(x: any): str { return "<{x}>" }
fn first(items: list, f: fn): any { for i in items { if f(i) { return i } } return nil }
fn flag(b: bool): bool { return !b }
fn keys(m: map): list { return m.keys() }
fn mean(xs: list): num { var s = 0 for x in xs { s += x } return s / len(xs) }
fn wrong(x): num { return x }
fn noReturn(): str { }
fn maybe(x): str? { if x { return "yes" } }
print(label(nil))                                // expect: <nil>
print(first([1, 2, 3], fn(x) { return x > 1 }))  // expect: 2
print(flag(false))                               // expect: true
print(keys({a: 1}))                              // expect: ["a"]
print(mean([1, 2, 3]))                           // expect: 2
print(maybe(false))                              // expect: nil
try { wrong("w") } catch e { print(e) }          // expect: the result of wrong() must be num, got the string "w"
try { noReturn() } catch e { print(e) }          // expect: the result of noReturn() must be str, got nil
try { flag(0) } catch e { print(e) }             // expect: argument 'b' of flag() must be bool, got the number 0
try { first([], 1) } catch e { print(e) }        // expect: argument 'f' of first() must be fn, got the number 1
try { keys([]) } catch e { print(e) }            // expect: argument 'm' of keys() must be map, got a list
var g = fn(n: num): num { return n + 1 }
try { g("x") } catch e { print(e) }              // expect: argument 'n' of the function at line 37 must be num, got the string "x"

// A result is checked as it leaves the function, after its own try and atomic blocks have closed.
fn own(): num { try { return "x" } catch e { return 0 } }
try { own() } catch e { print("caller: {e}") }   // expect: caller: the result of own() must be num, got the string "x"
class Mine : bank.Account { override fn withdraw(n: num): num { return 0 } }   // the same types, however they are written
print(Mine("m").withdraw(5))                     // expect: 0

// A `: num` result is relied on by callers (and by tier 2: `fib` is hot, its recursive calls are numbers).
fn fib(n: num): num { if n < 2 { return n } return fib(n - 1) + fib(n - 2) }
print(fib(25))                                   // expect: 75025
class Pt { var x: num = 0  fn move(d) { self.x += d } }
var p = Pt()
for i in 0..2000 { p.move(1) }                   // a typed field's stores are all checked, also when hot
print(p.x)                                       // expect: 2000
for i in 0..1999 { p.move(1) }
p.move("x")   // error: field 'x' of Pt must be num, got the string "3999x"
