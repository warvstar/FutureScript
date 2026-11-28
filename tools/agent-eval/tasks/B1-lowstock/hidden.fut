
test "hidden low stock" {
  var cat = sampleCatalog()
  expect lowStock(cat, 3) == ["TEE-M", "TEE-L", "LAMP"]
  expect lowStock(cat, 0) == ["TEE-L"]
  expect lowStock(cat, 100) == ["TEE-S", "TEE-M", "TEE-L", "MUG-BLU", "LAMP"]
  expect lowStock(Catalog(), 5) == []
}

test "hidden shipping quote" {
  var q = quoteShipping(6000, "INTL")
  expect q.region == "INTL" && q.grams == 6000
  expect q.costCents == shippingCents(6000, "INTL")
  expect q.days == 11
}
