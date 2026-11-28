
test "hidden refund" {
  var cat = sampleCatalog()
  var cart = cartOf(cat, [["TEE-S", 2], ["MUG-BLU", 1], ["LAMP", 1]])
  expect refundLine(cart, "TEE-S") == 3600
  expect lineFor(cart, "TEE-S") == nil
  expect cart.lines.len() == 2
  expect refundLine(cart, "TEE-S") == 0
  expect cart.lines.len() == 2
  expect refundLine(cart, "LAMP") == 4500
  expect subtotalCents(cart) == 1200
}

test "hidden availability" {
  var cat = sampleCatalog()
  var cart = cartOf(cat, [["TEE-M", 3], ["TEE-L", 1], ["LAMP", 3], ["MUG-BLU", 1]])
  expect unavailableLines(cat, cart) == ["TEE-L", "LAMP"]
}
