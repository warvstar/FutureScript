
test "hidden invoice lines know their sku" {
  var cat = sampleCatalog()
  var inv = buildInvoice(60, cartOf(cat, [["TEE-S", 1], ["LAMP", 1]]), "CA")
  expect inv.lines[0].sku == "TEE-S"
  expect inv.lines[1].sku == "LAMP"
  expect inv.lines[0].returned == 0
  expect inv.credits.len() == 0
  var note = CreditNote(7, 60)
  expect note.number == 7
  expect note.invoice == 60
  expect note.lines.len() == 0
}

test "hidden returns restock and refund" {
  var cat = sampleCatalog()
  var cart = cartOf(cat, [["MUG-BLU", 3], ["TEE-S", 2]])
  var inv = checkout(cat, cart, 61, "WA")
  expect findVariant(cat, "MUG-BLU").stock == 5
  var note = returnItems(cat, inv, [["MUG-BLU", 2]], 900)
  expect note.number == 900
  expect note.invoice == 61
  expect note.lines.len() == 1
  expect note.lines[0].label == "Enamel mug, blue"
  expect note.lines[0].qty == 2
  expect note.lines[0].cents == 2400
  expect note.goods == 2400
  expect note.tax == 156
  expect note.total == 2556
  expect findVariant(cat, "MUG-BLU").stock == 7
  expect inv.lines[0].returned == 2
  var second = returnItems(cat, inv, [["TEE-S", 1], ["MUG-BLU", 1]], 901)
  expect second.lines.len() == 2
  expect second.lines[0].label == "Organic tee, S"
  expect second.lines[0].cents == 1800
  expect second.lines[1].cents == 1200
  expect second.goods == 3000
  expect second.tax == 195
  expect second.total == 3195
  expect inv.lines[0].returned == 3
  expect inv.lines[1].returned == 1
  expect inv.credits.len() == 2
  expect inv.credits[0].number == 900
  expect inv.credits[1].number == 901
  expect findVariant(cat, "MUG-BLU").stock == 8
  expect findVariant(cat, "TEE-S").stock == 11
  expect throws { returnItems(cat, inv, [["MUG-BLU", 1]], 902) }
  expect inv.total == 3600 + 3600 + 899 + 468
}

test "hidden returns with a discount" {
  var cat = sampleCatalog()
  var cart = cartOf(cat, [["LAMP", 1], ["MUG-BLU", 1]])
  cart.code = "FIVEOFF"
  var inv = checkout(cat, cart, 62, "TX")
  expect inv.subtotal == 5700
  expect inv.discount == 500
  var note = returnItems(cat, inv, [["MUG-BLU", 1]], 910)
  expect note.goods == 1200
  expect note.tax == 68
  expect note.total == 1095 + 68
  var pct = cartOf(cat, [["TEE-S", 2]])
  pct.code = "SPRING25"
  var inv2 = checkout(cat, pct, 63, "OR")
  var n2 = returnItems(cat, inv2, [["TEE-S", 2]], 911)
  expect n2.goods == 3600
  expect n2.tax == 0
  expect n2.total == 2700
}

test "hidden returns are all or nothing" {
  var cat = sampleCatalog()
  var cart = cartOf(cat, [["TEE-M", 2], ["MUG-BLU", 1]])
  var inv = checkout(cat, cart, 64, "NY")
  expect throws { returnItems(cat, inv, [["TEE-M", 1], ["MUG-BLU", 2]], 920) }
  expect throws { returnItems(cat, inv, [["TEE-M", 1], ["LAMP", 1]], 920) }
  expect throws { returnItems(cat, inv, [["TEE-M", 1], ["MUG-BLU", 0]], 920) }
  expect throws { returnItems(cat, inv, [["TEE-M", -1]], 920) }
  expect findVariant(cat, "TEE-M").stock == 1
  expect findVariant(cat, "MUG-BLU").stock == 7
  expect inv.lines[0].returned == 0
  expect inv.lines[1].returned == 0
  expect inv.credits.len() == 0
  cat.products = [cat.products[0]]
  expect throws { returnItems(cat, inv, [["TEE-M", 1], ["MUG-BLU", 1]], 921) }
  expect findVariant(cat, "TEE-M").stock == 1
  expect inv.lines[0].returned == 0
  expect inv.credits.len() == 0
  var ok = returnItems(cat, inv, [["TEE-M", 2]], 922)
  expect ok.total == 3600 + 144
  expect findVariant(cat, "TEE-M").stock == 3
  expect inv.credits.len() == 1
}
