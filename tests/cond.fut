// Fused compare-and-branch conditions, tail returns and the shared epilogue.
fn sign(x) { if x < 0 { return -1 } else if x == 0 { return 0 } return 1 }
fn last(x) { if x > 1 { return "big" } }
fn str2(a, b) { if a < b { return "lt" } return "ge" }
fn eq(a, b) { if a == b { return true } return false }
fn nan(x) { if x != x { return "nan" } if x == x { return "num" } }
class C { fn init(v) { if v > 2 { return } self.v = v } }
fn count(n) { var i = 0 while i < n { i += 1 } return i }
fn mixed(a, b) { if a < b && b < 10 { return "in" } return "out" }
fn nested() { for i in 0..5 { if i >= 3 { return i } } }
print(sign(-5)) // expect: -1
print(sign(0)) // expect: 0
print(sign(7)) // expect: 1
print(last(5)) // expect: big
print(last(0)) // expect: nil
print(str2("a", "b")) // expect: lt
print(str2("b", "a")) // expect: ge
print(eq("x", "x")) // expect: true
print(eq(nil, 1)) // expect: false
print(nan(0 / 0)) // expect: nan
print(nan(3)) // expect: num
print(C(5)) // expect: <C>
print(C(1).v) // expect: 1
print(count(1000)) // expect: 1000
print(mixed(1, 5)) // expect: in
print(mixed(1, 50)) // expect: out
print(nested()) // expect: 3
if 1 + 1 == 2 { print("sum") } // expect: sum
if "a" == "a" { print("streq") } // expect: streq
