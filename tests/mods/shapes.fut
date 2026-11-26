// Imported `as shapes` by privacy.fut and privacy_err.fut.
fn unit() { return 1 }   // private: only this file calls it
pub class Shape {
  fn area() { return 0 }
  fn name() { return "shape" }
}
pub class Square : Shape {
  fn init(s) { self.s = s }
  override fn area() { return self.s * self.s * unit() }
}
