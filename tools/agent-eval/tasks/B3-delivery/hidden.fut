
test "hidden delivery estimate" {
  var cat = sampleCatalog()
  expect deliveryEstimate(cartOf(cat, [["TEE-S", 2], ["MUG-BLU", 1]]), "CA") == "Arrives in 3 days, $8.99 shipping"
  expect deliveryEstimate(cartOf(cat, [["TEE-S", 2], ["MUG-BLU", 1]]), "INTL") == "Arrives in 10 days, $23.99 shipping"
  expect deliveryEstimate(cartOf(cat, [["LAMP", 4]]), "NY") == "Arrives in 4 days, $21.49 shipping"
  expect deliveryEstimate(Cart(), "CA") == "Nothing to ship"
}
