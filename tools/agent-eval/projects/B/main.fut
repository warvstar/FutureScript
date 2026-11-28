// The shop backend: catalog, cart, pricing and invoices, tested end to end.
import "catalog.fut"
import "cart.fut"
import "pricing.fut"
import "invoice.fut"

fn sampleCatalog(): Catalog {
  var tee = Product("tee", "Organic tee")
    .add(Variant("TEE-S", "Organic tee, S", 1800, 12, 180))
    .add(Variant("TEE-M", "Organic tee, M", 1800, 3, 200))
    .add(Variant("TEE-L", "Organic tee, L", 2000, 0, 220))
  var mug = Product("mug", "Enamel mug")
    .add(Variant("MUG-BLU", "Enamel mug, blue", 1200, 8, 350))
  var lamp = Product("lamp", "Desk lamp")
    .add(Variant("LAMP", "Desk lamp", 4500, 2, 1600))
  return Catalog().add(tee).add(mug).add(lamp)
}

// Fill a cart from [sku, qty] pairs.
fn cartOf(cat: Catalog, items: list): Cart {
  var cart = Cart()
  for it in items {
    var v = findVariant(cat, it[0])
    if v == nil { throw "unknown sku {it[0]}" }
    addItem(cart, v, it[1])
  }
  return cart
}

// Take every line off the shelf, all or nothing.
fn checkout(cat: Catalog, cart: Cart, number: num, region: str): Invoice {
  atomic {
    for l in cart.lines { takeStock(findVariant(cat, l.sku), l.qty) }
  }
  return buildInvoice(number, cart, region)
}

test "invoice totals" {
  var cat = sampleCatalog()
  var cart = cartOf(cat, [["TEE-S", 2], ["MUG-BLU", 1]])
  var inv = buildInvoice(1, cart, "CA")
  expect inv.subtotal == 4800
  expect inv.shipping == 899
  expect inv.tax == 348
  expect inv.total == 4800 + 899 + 348
}

test "discounts on invoices" {
  var cat = sampleCatalog()
  var cart = cartOf(cat, [["LAMP", 1]])
  cart.code = "spring25"
  var inv = buildInvoice(2, cart, "NY")
  expect inv.discount == 1125
  expect inv.tax == 135
  cart.code = "SHIPFREE"
  expect buildInvoice(3, cart, "NY").shipping == 0
  cart.code = "nope"
  expect buildInvoice(4, cart, "NY").discount == 0
}

test "checkout takes stock" {
  var cat = sampleCatalog()
  var cart = cartOf(cat, [["TEE-M", 2], ["LAMP", 1]])
  checkout(cat, cart, 5, "TX")
  expect findVariant(cat, "TEE-M").stock == 1
  expect findVariant(cat, "LAMP").stock == 1
  var greedy = cartOf(cat, [["MUG-BLU", 1], ["TEE-M", 2]])
  expect throws { checkout(cat, greedy, 6, "TX") }
  expect findVariant(cat, "MUG-BLU").stock == 8
}

test "invoice text" {
  var cat = sampleCatalog()
  var cart = cartOf(cat, [["MUG-BLU", 2]])
  cart.code = "FIVEOFF"
  var inv = buildInvoice(7, cart, "WA")
  var text = formatInvoice(inv)
  var rows = text.split("\n")
  expect rows[0] == "Invoice #7"
  expect rows[1] == "2 x Enamel mug, blue        $24.00"
  expect text.has("Discount                    -$5.00")
  expect rows[rows.len() - 1] == "Total                       $29.23"
  expect prints { printInvoice(inv) } == text + "\n"
}
