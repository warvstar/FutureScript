
test "hidden gift card balance" {
  var card = GiftCard("HID-1", 3000)
  expect card.code == "HID-1"
  expect card.redeem(1200) == 1200
  expect card.balance == 1800
  expect card.redeem(0) == 0
  expect card.redeem(5000) == 1800
  expect card.balance == 0
  expect card.redeem(50) == 0
  expect throws { card.redeem(-10) }
  expect throws { GiftCard("HID-2", -1) }
  expect GiftCard("HID-3", 0).balance == 0
}

test "hidden gift cards on an invoice" {
  var cat = sampleCatalog()
  var inv = buildInvoice(40, cartOf(cat, [["LAMP", 1]]), "TX")
  expect inv.total == 4500 + 899 + 281
  expect inv.giftCards == 0
  expect inv.due == 5680
  var a = GiftCard("A", 2000)
  expect applyGiftCard(inv, a) == 2000
  expect a.balance == 0
  expect inv.due == 3680
  expect inv.giftCards == 2000
  var b = GiftCard("B", 5000)
  expect applyGiftCard(inv, b) == 3680
  expect b.balance == 1320
  expect inv.due == 0
  expect inv.giftCards == 5680
  expect applyGiftCard(inv, b) == 0
  expect b.balance == 1320
  expect inv.total == 5680
  expect inv.tax == 281
  expect inv.subtotal == 4500
}

test "hidden pay with cards" {
  var cat = sampleCatalog()
  var inv = buildInvoice(41, cartOf(cat, [["TEE-S", 1], ["MUG-BLU", 1]]), "OR")
  expect inv.due == 3899
  var empty = GiftCard("EMPTY", 0)
  var one = GiftCard("ONE", 1000)
  var two = GiftCard("TWO", 2899)
  var three = GiftCard("THREE", 700)
  expect payWithCards(inv, [empty, one, two, three]) == ["ONE", "TWO"]
  expect inv.due == 0
  expect inv.giftCards == 3899
  expect three.balance == 700
  expect payWithCards(inv, [three]) == []
  var inv2 = buildInvoice(42, cartOf(cat, [["TEE-S", 1]]), "OR")
  expect payWithCards(inv2, [three]) == ["THREE"]
  expect inv2.due == 1800 + 499 - 700
  expect three.balance == 0
  expect payWithCards(inv2, []) == []
}

test "hidden gift card invoice text" {
  var cat = sampleCatalog()
  var cart = cartOf(cat, [["MUG-BLU", 2]])
  cart.code = "FIVEOFF"
  var inv = buildInvoice(43, cart, "WA")
  var before = formatInvoice(inv)
  expect !before.has("Gift cards")
  expect !before.has("Due")
  expect before.split("\n")[before.split("\n").len() - 1] == "Total                       $29.23"
  applyGiftCard(inv, GiftCard("G", 1500))
  var text = formatInvoice(inv)
  expect text.slice(0, before.len()) == before
  var rows = text.split("\n")
  expect rows.len() == before.split("\n").len() + 2
  expect rows[rows.len() - 2] == "Gift cards                 -$15.00"
  expect rows[rows.len() - 1] == "Due                         $14.23"
  applyGiftCard(inv, GiftCard("H", 9999))
  rows = formatInvoice(inv).split("\n")
  expect rows[rows.len() - 2] == "Gift cards                 -$29.23"
  expect rows[rows.len() - 1] == "Due                          $0.00"
}
