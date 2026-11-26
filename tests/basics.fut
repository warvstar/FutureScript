// numbers, strings, bools, precedence, compound assignment
print(1 + 2 * 3)          // expect: 7
print((1 + 2) * 3)        // expect: 9
print(7 % 3)              // expect: 1
print(-7 % 3)             // expect: 2
print(10 / 4)             // expect: 2.5
print(-(2 + 3))           // expect: -5
print(0.1 + 0.2)          // expect: 0.30000000000000004
print("a" + "b" + 1)      // expect: ab1
print(1 < 2 && 2 < 3)     // expect: true
print(!nil)               // expect: true
print(nil || "x")         // expect: x
print(false && 1)         // expect: false
print("abc" == "abc")     // expect: true
print("abc" < "abd")      // expect: true
print(1 == 1 && 1 != 2)   // expect: true
var x = 5
x += 10
x *= 2
print(x)                  // expect: 30
print("tab\tq\"\\")       // expect: tab	q"\
var y = x
  - 1                     // a leading '-' starts a new statement, it is not subtraction
print(y)                  // expect: 30
