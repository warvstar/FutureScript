
test "hidden: bestWeapon" {
  var p = Player("Ana")
  expect bestWeapon(p) == nil
  addItem(p, potion())
  addItem(p, Item("Mail", "armor", 9, 8, 50))
  expect bestWeapon(p) == nil
  var dagger = Item("Dagger", "weapon", 2, 2, 12)
  var axe = Item("Axe", "weapon", 5, 4, 30)
  var club = Item("Club", "weapon", 5, 4, 8)
  addItem(p, dagger)
  addItem(p, axe)
  addItem(p, club)
  var got = bestWeapon(p)
  expect got.name == "Axe"
  expect p.inventory.len() == 5
  equip(p, axe)
  expect bestWeapon(p).name == "Club"
}

test "hidden: bosses" {
  var boss = makeBoss("Ogre King", 2)
  expect boss.name == "Ogre King" && boss.hp == 60 && boss.atk == 10 && boss.def == 4
  expect boss.speed == 3 && boss.xpReward == 50 && boss.goldReward == 80
}
