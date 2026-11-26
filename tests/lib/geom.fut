import "util.fut"
pub class Vec {
  fn init(x, y) { self.x = x self.y = y }
  fn add(o) { return Vec(self.x + o.x, self.y + o.y) }
  fn str() { return "({self.x}, {self.y})" }
}
pub var origin = Vec(0, 0)
