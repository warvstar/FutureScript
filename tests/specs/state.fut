// state: a variable or field the page follows (std/reactive signals underneath), outside views too.
state n = 1
state items = []
class C { state v = 3 }
fn bump() { n += 2 n *= 3 }
fn f(n) { return n }
var m = {n: 1}
test "state outside views" {
  bump()
  expect n == 9
  var c = C()
  expect c.v == 3
  c.v += 1
  expect c.v == 4
  expect "n is {n}, v is {c.v}" == "n is 9, v is 4"
  expect f(5) == 5
  expect m.n == 1
  items = [1, 2]
  expect items.len() == 2
}
fn g() {
  var total = 0
  for n in [1, 2] { total += n }
  return total
}
fn h() { if true { var n = 7 return n } return 0 }
test "locals shadow state" {
  expect g() == 3
  expect h() == 7
}
class Pt {
  state x
  fn init(x) { self.x = x }
}
test "a parameter named like a field still sets the field" {
  var p = Pt(4)
  expect p.x == 4
  p.x += 1
  expect p.x == 5
}
