
test "hidden warehouses" {
  var w = Warehouse("Depot")
  expect w.name == "Depot"
  expect w.stockOf("TEE-S") == 0
  expect w.put("TEE-S", 2).put("TEE-S", 3) == w
  expect w.stockOf("TEE-S") == 5
  expect throws { w.put("LAMP", 0) }
  expect throws { w.put("LAMP", -2) }
  expect w.stockOf("LAMP") == 0
}

test "hidden split across warehouses" {
  var cat = sampleCatalog()
  var a = Warehouse("A").put("MUG-BLU", 1).put("TEE-M", 1)
  var b = Warehouse("B").put("LAMP", 3)
  var c = Warehouse("C").put("TEE-M", 4).put("MUG-BLU", 5)
  var cart = cartOf(cat, [["TEE-M", 3], ["MUG-BLU", 3]])
  var parts = splitShipments(cart, [a, b, c], "NY")
  expect parts.len() == 2
  expect parts[0].warehouse == "A"
  expect parts[0].units["TEE-M"] == 1
  expect parts[0].units["MUG-BLU"] == 1
  expect parts[0].units.keys() == ["TEE-M", "MUG-BLU"]
  expect parts[0].grams == 550
  expect parts[0].cost == 899
  expect parts[1].warehouse == "C"
  expect parts[1].units.keys() == ["TEE-M", "MUG-BLU"]
  expect parts[1].units["TEE-M"] == 2
  expect parts[1].units["MUG-BLU"] == 2
  expect parts[1].grams == 1100
  expect parts[1].cost == 899
  expect shipmentsCost(parts) == 1798
  expect a.stockOf("TEE-M") == 0
  expect a.stockOf("MUG-BLU") == 0
  expect b.stockOf("LAMP") == 3
  expect c.stockOf("TEE-M") == 2
  expect c.stockOf("MUG-BLU") == 3
  expect describeShipment(parts[0]) == "A: TEE-M x1, MUG-BLU x1 (550 g, $8.99)"
  expect describeShipment(parts[1]) == "C: TEE-M x2, MUG-BLU x2 (1.10 kg, $8.99)"
}

test "hidden one warehouse covers it" {
  var cat = sampleCatalog()
  var near = Warehouse("Near").put("LAMP", 2).put("TEE-S", 9)
  var far = Warehouse("Far").put("LAMP", 9)
  var parts = splitShipments(cartOf(cat, [["LAMP", 2], ["TEE-S", 1]]), [near, far], "INTL")
  expect parts.len() == 1
  expect parts[0].warehouse == "Near"
  expect parts[0].grams == 3380
  expect parts[0].cost == 899 + 500 + 1500
  expect describeShipment(parts[0]) == "Near: LAMP x2, TEE-S x1 (3.38 kg, $28.99)"
  expect far.stockOf("LAMP") == 9
  expect near.stockOf("LAMP") == 0
  expect shipmentsCost(parts) == 2899
  expect shipmentsCost([]) == 0
}

test "hidden split all or nothing" {
  var cat = sampleCatalog()
  var x = Warehouse("X").put("TEE-S", 2).put("MUG-BLU", 1)
  var y = Warehouse("Y").put("MUG-BLU", 1)
  var cart = cartOf(cat, [["TEE-S", 2], ["MUG-BLU", 3]])
  expect throws { splitShipments(cart, [x, y], "CA") }
  expect x.stockOf("TEE-S") == 2
  expect x.stockOf("MUG-BLU") == 1
  expect y.stockOf("MUG-BLU") == 1
  expect throws { splitShipments(cartOf(cat, [["LAMP", 1]]), [], "CA") }
  expect splitShipments(Cart(), [x, y], "CA") == []
  expect splitShipments(Cart(), [], "CA") == []
  expect x.stockOf("TEE-S") == 2
}
