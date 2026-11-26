// Instance fields: an inline cache across classes, fields added late, compound assignment.
class P { fn init(x) { self.x = x } }
class Q : P { fn init(x) { super.init(x) self.y = x * 2 } }
var ps = [P(1), Q(2), P(3), Q(4)]
var t = 0
var p = P(1)

test "one field-read site sees two classes" {
  for p in ps { t += p.x }          // one site, two classes: the inline cache refills
  expect t == 10
}
test "fields can be added any time and updated in place" {
  p.late = 5                        // fields can be added any time
  expect p.late + P(2).x == 7
  p.x += 1
  expect p.x == 2
}
