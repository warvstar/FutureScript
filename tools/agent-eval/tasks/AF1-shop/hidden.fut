test "hidden: shop stock and prices" {
  var shop = Shop("Bram's")
  expect shop.stockOf("Potion") == 0 && shop.priceOf("Potion") == nil
  expect shop.listing() == []
  shop.restock(potion(), 2)
  shop.restock(Item("Axe", "weapon", 4, 5, 30), 1)
  shop.restock(Item("Potion", "potion", 99, 9, 99), 1)
  expect shop.stockOf("Potion") == 3 && shop.stockOf("Axe") == 1
  expect shop.priceOf("Potion") == 15 && shop.priceOf("Axe") == 45
  expect shop.listing() == ["Potion x3 - 15g", "Axe x1 - 45g"]
  shop.restock(Item("Gel", "junk", 0, 1, 5), 1)
  expect shop.priceOf("Gel") == 7
  shop.restock(Item("Gel", "junk", 0, 1, 5), 1)
  expect shop.priceOf("Gel") == 5
}

test "hidden: buying" {
  var shop = Shop("Bram's")
  shop.restock(potion(), 2)
  shop.restock(Item("Axe", "weapon", 4, 5, 30), 1)
  shop.restock(Item("Anvil", "junk", 0, 30, 1), 1)
  var p = Player("Ana")
  p.gold = 40
  expect shop.buy(p, "Axe") == "not enough gold"
  expect shop.buy(p, "Bow") == "out of stock"
  expect shop.buy(p, "Anvil") == "too heavy"
  expect p.gold == 40 && p.inventory.len() == 0 && shop.stockOf("Anvil") == 1
  var broke = Player("Bo")
  expect shop.buy(broke, "Anvil") == "not enough gold"
  expect shop.buy(p, "Potion") == "ok"
  expect p.gold == 25 && shop.stockOf("Potion") == 1
  expect shop.buy(p, "Potion") == "ok"
  expect p.gold == 3 && shop.stockOf("Potion") == 0
  expect shop.buy(p, "Potion") == "out of stock"
  expect p.inventory.len() == 2 && p.inventory[0] != p.inventory[1]
  var got = p.inventory[0]
  expect got.name == "Potion" && got.kind == "potion" && got.power == 10 && got.weight == 1 && got.value == 15
  expect shop.listing() == ["Axe x1 - 45g", "Anvil x1 - 1g"]
  shop.restock(potion(), 4)
  expect shop.listing() == ["Potion x4 - 15g", "Axe x1 - 45g", "Anvil x1 - 1g"]
}

test "hidden: selling" {
  var shop = Shop("Bram's")
  var p = Player("Ana")
  var pelt = Item("Wolf Pelt", "junk", 0, 3, 11)
  var other = Item("Wolf Pelt", "junk", 0, 3, 11)
  addItem(p, pelt)
  expect shop.sell(p, other) == 0
  expect p.gold == 0 && p.inventory.len() == 1 && shop.stockOf("Wolf Pelt") == 0
  expect shop.sell(p, pelt) == 5
  expect p.gold == 5 && p.inventory.len() == 0
  expect shop.stockOf("Wolf Pelt") == 1 && shop.priceOf("Wolf Pelt") == 16
  expect shop.sell(p, pelt) == 0
  p.gold = 20
  expect shop.buy(p, "Wolf Pelt") == "ok"
  expect p.gold == 4 && p.inventory[0] != pelt && p.inventory[0].value == 11
}
