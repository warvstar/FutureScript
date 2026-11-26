// Locals that start as numbers live in FP registers; they must still hold any value safely (GC included).
fn churn() { var junk = 0 for i in 0..200000 { junk = [i, str(i)] } return junk }
fn keep() {
  var s = 0
  s = [1, 2, "three"]   // a heap pointer in an FP register
  var t = 0.5
  t += "x"              // string concatenation through the slow path
  churn()               // many collections while s and t are live
  return "{s} {t}"
}
fn nested() { var n = 0 for a in 0..3 { for b in 0..3 { for c in 0..3 { for d in 0..3 { for e in 0..3 { n += 1 } } } } } return n }
fn many() { var a = 1 var b = 2 var c = 3 var d = 4 var e = 5 var f = 6 var g = 7 var h = 8 var i = 9 var j = 10
  for k in 0..2 { a += b b += c c += d d += e e += f f += g g += h h += i i += j j += k } return a + b + c + d + e + f + g + h + i + j }
fn assigned() { var t = 0 for i in 0..5 { i = i * 2 t += i } return t }
fn mixed() { var s = 0 for i in 0..4 { s -= i s *= 2 s /= 1 } return s }
fn rec(n) { var acc = n if n > 0 { acc += rec(n - 1) } return acc }
fn slowpaths() { var t = 1 var u = 2 u = 5 t += "a" var w = 3 var z = [] w += "b" w += 1 return "{t} {w}" }
fn lits(x) { return [x + 0, x * 2, x - 0.1, x / 3, x == 0, 0 < x] }
fn slits(x) { return [x + 0, x + 2.5, x + 1000000, x == 0] }
fn direct() { var s = 0 s = ["kept", 1] var n = 0 for i in 0..300000 { n = str(i) } return "{s} {n}" }
fn f(x) { return [x + 1, x * 2, x - 0.5, x / 3, x + 31, x * 0.125, x + 0.1, x - 17, x * -2, x + 0] }

test "heap values in FP registers survive collections" {
  expect keep() == "[1, 2, \"three\"] 0.5x"
  expect direct() == "[\"kept\", 1] 299999"
}
test "loops and many register locals" {
  expect nested() == 243
  expect many() == 216
  expect assigned() == 20
  expect mixed() == -22
  expect rec(10) == 55
}
test "slow paths for non-numbers" { expect slowpaths() == "1a 3b1" }
test "arithmetic with literal operands" {
  expect lits(6) == [6, 12, 5.9, 2, false, true]
  expect slits("s") == ["s0", "s2.5", "s1000000", false]
  expect f(8) == [9, 16, 7.5, 2.6666666666666665, 39, 1, 8.1, -9, -16, 8]
}
