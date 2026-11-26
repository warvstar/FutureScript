// Carts, discounts and checkout, on top of the money and stock packages (orders/fut.pkg lists them with dep).
import "money"
import "stock"

// What checkout reports. Callers decide where it goes: a log, the console, a list in a test.
pub effect Audit {
  fn record(event: str)
}

pub class Line {
  var sku = ""
  var qty = 0
  var price = 0  // cents per unit
  fn init(sku: str, qty: num, price: num) {
    self.sku = sku
    self.qty = qty
    self.price = price
  }
}

pub class Cart {
  var lines = nil
  var code: str? = nil  // a discount code
  fn init() { self.lines = [] }
  // A clause costs what it computes, on every call: `ensures self.subtotal() == old(self.subtotal()) + qty * price`
  // would make add() O(lines), and building a cart O(lines^2), which store_test.fut's `expect grows` catches. So
  // these clauses look only at the line added.
  fn add(sku: str, qty: num, price: num)
    requires qty >= 1
    requires qty == floor(qty)
    requires price >= 0
    requires price == floor(price)
    ensures self.lines.len() == old(self.lines.len()) + 1
    ensures last(self.lines).qty * last(self.lines).price == qty * price
  {
    self.lines.push(Line(sku, qty, price))
  }
  fn subtotal(): num {
    var s = 0
    for l in self.lines { s += l.qty * l.price }
    return s
  }
}

// SAVE10: 10% off. BULK: 5% off orders of $100 or more. Anything else: nothing.
pub fn discount(subtotal: num, code: str?): num
  requires subtotal >= 0
  requires subtotal == floor(subtotal)
  ensures result >= 0
  ensures result <= subtotal
  ensures result == floor(result)
{
  if code == "SAVE10" { return money.percent(subtotal, 10) }
  if code == "BULK" && subtotal >= 10000 { return money.percent(subtotal, 5) }
  return 0
}

pub fn total(cart: Cart): num ! {
  var s = cart.subtotal()
  return s - discount(s, cart.code)
}

// Reserves every line or none: one line out of stock rolls back the reservations made for the lines before it.
pub fn checkout(cart: Cart, st: stock.Stock): map ! Audit
  ensures result.total == total(cart)
  ensures result.lines == cart.lines.len()
{
  if cart.lines.len() == 0 { throw "empty cart" }
  atomic {
    for l in cart.lines {
      if st.available(l.sku) < l.qty { throw "out of stock: " + l.sku }
      st.reserve(l.sku, l.qty)
    }
  }
  var t = total(cart)
  Audit.record("order " + money.format(t) + ", " + str(cart.lines.len()) + " lines")
  return {total: t, lines: cart.lines.len()}
}

fn last(l) { return l[l.len() - 1] }
