fn fib(n) { if n < 2 { return n } return fib(n - 1) + fib(n - 2) }

class Vec {
  fn init(x, y) { self.x = x self.y = y }
  fn add(o) { return Vec(self.x + o.x, self.y + o.y) }
}

var t = clock()
print("fib(32) = " + str(fib(32)) + " in " + str(floor((clock() - t) * 1000)) + " ms")

t = clock()
var v = Vec(0, 0)
var d = Vec(1, 2)
for i in 0..1000000 { v = v.add(d) }
print("1M allocs+calls: (" + str(v.x) + ", " + str(v.y) + ") in " + str(floor((clock() - t) * 1000)) + " ms")
