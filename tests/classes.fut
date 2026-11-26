class Shape {
  fn init(name) { self.name = name }
  fn area() { return 0 }
  fn describe() { return self.name + " with area " + str(self.area()) }
}
class Rect : Shape {
  fn init(w, h) { super.init("rect") self.w = w self.h = h }
  override fn area() { return self.w * self.h }
}
class Square : Rect {
  fn init(s) { super.init(s, s) self.name = "square" }
}
print(Rect(2, 3).describe())     // expect: rect with area 6
print(Square(4).describe())      // expect: square with area 16
var r = Rect(1, 1)
r.w += 4
print(r.area())                  // expect: 5
class Counter { fn inc() { self.n = self.n + 1 return self } }
var c = Counter()
c.n = 0
print(c.inc().inc().n)           // expect: 2
print(r)                         // expect: Rect{name: "rect", w: 5, h: 1}
