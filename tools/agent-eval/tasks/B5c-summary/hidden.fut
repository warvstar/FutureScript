
test "hidden order summary" {
  var cat = sampleCatalog()
  expect orderSummary(cartOf(cat, [["LAMP", 1], ["TEE-M", 1]]), "NY") == "2 items, 1.80 kg, shipping $8.99"
  expect orderSummary(cartOf(cat, [["MUG-BLU", 1]]), "CA") == "1 item, 350 g, shipping $4.99"
  expect orderSummary(Cart(), "CA") == "0 items, 0 g, shipping $0.00"
  var cart = Cart()
  addItem(cart, Variant("SOCK", "Wool socks", 900, 20, 250), 2)
  expect orderSummary(cart, "TX") == "2 items, 500 g, shipping $4.99"
  expect orderSummary(cart, "INTL") == "2 items, 500 g, shipping $19.99"
}
