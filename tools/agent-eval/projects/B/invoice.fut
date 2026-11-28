// Turning a cart into an invoice, and the invoice into text.
import "cart.fut"
import "pricing.fut"

pub class InvoiceLine {
  var label: str
  var qty: num
  var cents: num
  fn init(label: str, qty: num, cents: num) {
    self.label = label
    self.qty = qty
    self.cents = cents
  }
}

pub class Invoice {
  var number: num
  var region: str
  var lines: list<InvoiceLine> = []
  var subtotal: num = 0
  var discount: num = 0
  var shipping: num = 0
  var tax: num = 0
  var total: num = 0
  fn init(number: num, region: str) {
    self.number = number
    self.region = region
  }
}

// Tax is charged on the discounted subtotal; shipping is not taxed.
// An unknown discount code is ignored.
pub fn buildInvoice(number: num, cart: Cart, region: str): Invoice ! {
  var inv = Invoice(number, region)
  for l in cart.lines {
    inv.lines.push(InvoiceLine(l.name, l.qty, l.totalCents()))
  }
  inv.subtotal = subtotalCents(cart)
  inv.shipping = shippingCents(cartGrams(cart), region)
  var d = cart.code == nil ? nil : lookupCode(cart.code)
  if d != nil {
    inv.discount = discountCents(d, inv.subtotal)
    if d == Discount.FreeShipping { inv.shipping = 0 }
  }
  inv.tax = taxCents(inv.subtotal - inv.discount, region)
  inv.total = inv.subtotal - inv.discount + inv.shipping + inv.tax
  return inv
}

// 1234 -> "$12.34", -500 -> "-$5.00"
pub fn formatMoney(cents: num): str ! {
  var sign = cents < 0 ? "-" : ""
  var c = cents < 0 ? -cents : cents
  return "{sign}${int(c / 100)}.{c % 100:02}"
}

// Under 1000 g in grams ("350 g"), from 1000 g up in kilograms with two decimals ("1.25 kg").
pub fn formatKg(grams: num): str ! {
  if grams < 1000 { return "{grams} g" }
  return "{grams / 1000:.2} kg"
}

fn row(label: str, amount: str): str ! {
  return "{label:<24}{amount:>10}"
}

pub fn formatInvoice(inv: Invoice): str ! {
  var out = "Invoice #{inv.number}\n"
  for l in inv.lines {
    out += row("{l.qty} x {l.label}", formatMoney(l.cents)) + "\n"
  }
  out += row("Subtotal", formatMoney(inv.subtotal)) + "\n"
  if inv.discount > 0 { out += row("Discount", formatMoney(-inv.discount)) + "\n" }
  out += row("Shipping", formatMoney(inv.shipping)) + "\n"
  out += row("Tax ({inv.region})", formatMoney(inv.tax)) + "\n"
  out += row("Total", formatMoney(inv.total))
  return out
}

pub fn printInvoice(inv: Invoice) ! io {
  print(formatInvoice(inv))
}

// What the warehouse picks: one line per SKU with its quantity and weight, then the parcel weight.
pub fn packingSlip(cart: Cart): str ! {
  var out = ""
  for l in cart.lines {
    out += "{l.sku:<12}x{l.qty:<4}{formatKg(l.totalGrams())}\n"
  }
  out += "Parcel: {formatKg(cartGrams(cart))}"
  return out
}

test "money" {
  expect formatMoney(1234) == "$12.34"
  expect formatMoney(5) == "$0.05"
  expect formatMoney(-500) == "-$5.00"
  expect formatKg(350) == "350 g"
  expect formatKg(1250) == "1.25 kg"
}
