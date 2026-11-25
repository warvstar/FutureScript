fn sum10(a, b, c, d, e, f, g, h, i, j) { return a + b + c + d + e + f + g + h + i + j }
print(sum10(1, 2, 3, 4, 5, 6, 7, 8, 9, 10))   // expect: 55
var s = sum10
print(s(1, 1, 1, 1, 1, 1, 1, 1, 1, 100))      // expect: 109
class M { fn many(a, b, c, d, e, f, g, h, i) { return i - a } }
print(M().many(1, 2, 3, 4, 5, 6, 7, 8, 9))    // expect: 8
print(fn(a, b, c, d, e, f, g, h, i, j, k) { return k }(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 42))  // expect: 42
