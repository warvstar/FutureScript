import "util.fut"
class Vec {
  fn init(x, y) { self.x = x self.y = y }
  fn add(o) { return Vec(self.x + o.x, self.y + o.y) }
  fn str() { return "({self.x}, {self.y})" }
}
var origin = Vec(0, 0)
