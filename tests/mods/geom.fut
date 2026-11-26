var PI = 3.14159
var calls = 0
fn square(x) { calls += 1 return x * x }
fn area(r) { return PI * square(r) }
class Circle {
  fn init(r) { self.r = r }
  fn area() { return area(self.r) }
}
fn main() { print("geom main must not run") }
