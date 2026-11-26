// Calls with many arguments: direct, through a variable, a method, and an immediately-called fn literal.
fn sum10(a, b, c, d, e, f, g, h, i, j) { return a + b + c + d + e + f + g + h + i + j }
var s = sum10
class M { fn many(a, b, c, d, e, f, g, h, i) { return i - a } }

test "ten arguments, direct and through a variable" {
  expect sum10(1, 2, 3, 4, 5, 6, 7, 8, 9, 10) == 55
  expect s(1, 1, 1, 1, 1, 1, 1, 1, 1, 100) == 109
}
test "nine arguments to a method" {
  expect M().many(1, 2, 3, 4, 5, 6, 7, 8, 9) == 8
}
test "eleven arguments to an fn literal" {
  expect fn(a, b, c, d, e, f, g, h, i, j, k) { return k }(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 42) == 42
}
