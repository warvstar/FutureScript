class P { fn init(x) { self.x = x } }
class Q : P { fn init(x) { super.init(x) self.y = x * 2 } }
var ps = [P(1), Q(2), P(3), Q(4)]
var t = 0
for p in ps { t += p.x }          // one site, two classes: the inline cache refills
print(t)                          // expect: 10
var p = P(1)
p.late = 5                        // fields can be added any time
print(p.late + P(2).x)            // expect: 7
p.x += 1
print(p.x)                        // expect: 2
