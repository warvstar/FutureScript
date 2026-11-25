fn fib(n) { if n < 2 { return n } return fib(n - 1) + fib(n - 2) }
print(fib(20))                       // expect: 6765
var s = 0
for i in 0..10 { if i == 3 { continue } if i == 6 { break } s += i }
print(s)                             // expect: 12
var n = 0
while n < 5 { n += 1 }
print(n)                             // expect: 5
fn sign(v) { if v < 0 { return "neg" } else if v == 0 { return "zero" } else { return "pos" } }
print(sign(-2) + sign(0) + sign(9))  // expect: negzeropos
fn later() { return early() * 2 }    // forward reference
fn early() { return 21 }
print(later())                       // expect: 42
var f = fib
print(f(10))                         // expect: 55
fn noret() { }
print(noret())                       // expect: nil
