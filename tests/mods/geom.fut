pub var PI = 3.14159
pub var calls = 0
pub fn square(x) { calls += 1 return x * x }
pub fn area(r) { return PI * square(r) }
pub class Circle {
  fn init(r) { self.r = r }
  fn area() { return area(self.r) }
}
fn main() { print("geom main must not run") }
