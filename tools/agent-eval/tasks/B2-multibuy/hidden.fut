
test "hidden multibuy" {
  var cat = sampleCatalog()
  expect multibuySavings(cartOf(cat, [["TEE-S", 7], ["MUG-BLU", 2]])) == 3600
  expect multibuySavings(cartOf(cat, [["TEE-S", 2], ["TEE-M", 1], ["LAMP", 3]])) == 4500
  expect multibuySavings(cartOf(cat, [["MUG-BLU", 6]])) == 2400
  expect multibuySavings(Cart()) == 0
}

test "hidden return to stock" {
  var cat = sampleCatalog()
  var cart = cartOf(cat, [["MUG-BLU", 3]])
  returnToStock(cat, cart.lines[0])
  expect findVariant(cat, "MUG-BLU").stock == 11
}
