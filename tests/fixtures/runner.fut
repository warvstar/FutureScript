// Fixture for `fut test` (build.py checks the statuses and details it reports).
import "../mods/geom.fut" as geom
class Cart {
  fn init() { self.items = [] }
  fn add(price, qty) { if qty < 0 { throw "negative qty" } self.items.push({price: price, qty: qty}) return self }
  fn total() { var s = 0 for i in self.items { s += i.price * i.qty } return s }
}
var ready = "yes"
fn main() { print("main does not run under fut test") }

test "passes" { expect Cart().add(2, 3).add(5, 1).total() == 11 expect ready == "yes" }
test "fails" {
  print("built")
  var c = Cart().add(2, 3)
  expect c.total() == 7
}
test "throws" { expect throws { Cart().add(1, -1) } == "negative qty" }
test "nothing thrown" { expect throws { Cart() } }
test "pending" { expect discount(Cart(), 0.1) == 0 }
test "errors" { var m = {a: 1} m.a.b }
test "deep" { expect {items: [{qty: 3}]} == {items: [{qty: 4}]} }
test "caught failure still fails" { try { expect 1 == 2 } catch { } }
test "module" { expect geom.area(2) > 0 }
