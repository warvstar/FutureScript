// A shopper's cart: one line per SKU, with the price and weight captured when it was added.
import "catalog.fut"

pub class CartLine {
  var sku: str
  var name: str
  var qty: num
  var unitCents: num
  var weightGrams: num
  fn init(v: Variant, qty: num) {
    self.sku = v.sku
    self.name = v.name
    self.qty = qty
    self.unitCents = v.priceCents
    self.weightGrams = v.weightGrams
  }
  fn totalCents(): num { return self.unitCents * self.qty }
  fn totalGrams(): num { return self.weightGrams * self.qty }
}

pub class Cart {
  var lines: list<CartLine> = []
  var code: str? = nil
}

// The cart's line for this SKU, or nil.
pub fn lineFor(cart: Cart, sku: str): CartLine? ! {
  for l in cart.lines {
    if l.sku == sku { return l }
  }
  return nil
}

// Add qty units of v; a SKU already in the cart gets its quantity raised.
pub fn addItem(cart: Cart, v: Variant, qty: num): CartLine ! {
  if qty <= 0 { throw "quantity must be positive" }
  var existing = lineFor(cart, v.sku)
  if existing != nil {
    existing.qty += qty
    return existing
  }
  var line = CartLine(v, qty)
  cart.lines.push(line)
  return line
}

// Drop the line for sku. Returns whether there was one.
pub fn removeItem(cart: Cart, sku: str): bool ! {
  for i in 0..cart.lines.len() {
    if cart.lines[i].sku == sku {
      var kept = cart.lines.slice(0, i) + cart.lines.slice(i + 1, cart.lines.len())
      cart.lines = kept
      return true
    }
  }
  return false
}

// Set a line's quantity; 0 removes it.
pub fn setQuantity(cart: Cart, sku: str, qty: num) ! {
  if qty < 0 { throw "quantity can't be negative" }
  if qty == 0 {
    removeItem(cart, sku)
    return
  }
  var line = lineFor(cart, sku)
  if line == nil { throw "{sku} is not in the cart" }
  line.qty = qty
}

pub fn itemCount(cart: Cart): num ! {
  var n = 0
  for l in cart.lines { n += l.qty }
  return n
}

pub fn subtotalCents(cart: Cart): num ! {
  var t = 0
  for l in cart.lines { t += l.totalCents() }
  return t
}

pub fn cartGrams(cart: Cart): num ! {
  var g = 0
  for l in cart.lines { g += l.totalGrams() }
  return g
}

// Put a line's units back on the shelf, e.g. when an order is cancelled.
pub fn returnToStock(cat: Catalog, line: CartLine) ! {
  var v = findVariant(cat, line.sku)
  if v == nil { throw "{line.sku} is no longer sold" }
  restock(v, line.qty)
}

// SKUs of the lines the shelf can't cover right now, in cart order.
pub fn unavailableLines(cat: Catalog, cart: Cart): list<str> ! {
  var out = []
  for l in cart.lines {
    var v = findVariant(cat, l.sku)
    if v == nil || v.stock < l.qty { out.push(l.sku) }
  }
  return out
}

test "cart lines" {
  var tee = Variant("TEE-M", "Tee, M", 1800, 10, 200)
  var cap = Variant("CAP", "Cap", 1500, 4, 120)
  var cart = Cart()
  addItem(cart, tee, 2)
  addItem(cart, cap, 1)
  addItem(cart, tee, 1)
  expect cart.lines.len() == 2
  expect lineFor(cart, "TEE-M").qty == 3
  expect itemCount(cart) == 4
  expect subtotalCents(cart) == 3 * 1800 + 1500
  expect cartGrams(cart) == 3 * 200 + 120
  setQuantity(cart, "CAP", 0)
  expect lineFor(cart, "CAP") == nil
  expect !removeItem(cart, "CAP")
  expect throws { addItem(cart, cap, 0) }
}
