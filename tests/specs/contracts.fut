// Contract failures are errors of the code under test, not failed expects: a test that expects one passes.
class Stack {
  var items = nil
  invariant self.items == nil || self.items.len() <= 3
  fn init() { self.items = [] }
  fn push(x) { self.items.push(x) return self }
  fn pop() requires self.items.len() > 0 { return self.items.pop() }
}
test "pop on empty is refused" {
  expect throws { Stack().pop() } == "Stack.pop() requires self.items.len() > 0 (self.items.len() is 0)"
}
test "a caught contract failure leaves the test passing" {
  try { Stack().push(1).push(2).push(3).push(4) } catch { }
  expect Stack().push(1).pop() == 1
}
