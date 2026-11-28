
test "hidden: usePotion" {
  var p = Player("Ana")
  p.hp = 10
  expect !usePotion(p)
  expect p.hp == 10
  var pelt = Item("Wolf Pelt", "junk", 0, 3, 10)
  var big = Item("Big Potion", "potion", 25, 1, 40)
  addItem(p, pelt)
  addItem(p, potion())
  addItem(p, big)
  expect usePotion(p)
  expect p.hp == 20
  expect p.inventory.len() == 2 && p.inventory[0].name == "Wolf Pelt" && p.inventory[1].name == "Big Potion"
  expect usePotion(p)
  expect p.hp == 30 && p.inventory.len() == 1
  p.inventory.push(potion())
  expect !usePotion(p)
  expect p.inventory.len() == 2
}

test "hidden: selling" {
  var p = Player("Ana")
  var pelt = Item("Wolf Pelt", "junk", 0, 3, 10)
  var gel = Item("Slime Gel", "junk", 0, 1, 5)
  addItem(p, pelt)
  addItem(p, gel)
  expect sellItem(p, pelt) == 5
  expect p.gold == 5 && p.inventory.len() == 1
  expect sellItem(p, pelt) == 0
  expect sellItem(p, gel) == 2
  expect p.gold == 7 && p.inventory.len() == 0
}
