test "hidden: accepting and progress" {
  var log = QuestLog()
  expect log.status() == []
  var early = makeMonster("wolf", 1)
  early.hp = 0
  log.recordKills([early])
  var hunt = Quest("Wolf Hunt", [Goal.Kill("wolf", 3), Goal.Collect("Wolf Pelt", 2)], 30, 10)
  expect !hunt.complete()
  expect log.accept(hunt)
  expect !log.accept(Quest("Wolf Hunt", [Goal.Kill("slime", 1)], 5, 5))
  expect log.status() == ["Wolf Hunt: wolf 0/3, Wolf Pelt 0/2"]
  var w1 = makeMonster("wolf", 1)
  var w2 = makeMonster("wolf", 2)
  var s = makeMonster("slime", 1)
  w1.hp = 0
  s.hp = 0
  log.recordKills([w1, w2, s])
  expect log.status() == ["Wolf Hunt: wolf 1/3, Wolf Pelt 0/2"]
  w2.hp = 0
  log.recordKills([w1, w2])
  expect log.status() == ["Wolf Hunt: wolf 3/3, Wolf Pelt 0/2"]
  log.recordKills([w1])
  expect log.status() == ["Wolf Hunt: wolf 3/3, Wolf Pelt 0/2"]
  var p = Player("Ana")
  expect log.pickUp(p, Item("Wolf Pelt", "junk", 0, 3, 10))
  expect log.pickUp(p, potion())
  expect p.inventory.len() == 2
  expect !hunt.complete()
  expect !log.pickUp(p, Item("Wolf Pelt", "junk", 0, 30, 10))
  expect p.inventory.len() == 2
  expect log.status() == ["Wolf Hunt: wolf 3/3, Wolf Pelt 1/2"]
  expect log.pickUp(p, Item("Wolf Pelt", "junk", 0, 3, 10))
  expect hunt.complete()
  expect log.pickUp(p, Item("Wolf Pelt", "junk", 0, 3, 10))
  expect log.status() == ["Wolf Hunt: wolf 3/3, Wolf Pelt 2/2 (ready)"]
}

test "hidden: shared progress and turning in" {
  var log = QuestLog()
  var p = Player("Ana")
  p.gold = 5
  var a = Quest("Slime Trouble", [Goal.Kill("slime", 2)], 12, 6)
  var b = Quest("Gel Order", [Goal.Collect("Slime Gel", 1), Goal.Kill("slime", 1)], 8, 5)
  var c = Quest("Bones", [Goal.Kill("skeleton", 1)], 50, 50)
  expect log.accept(a) && log.accept(b) && log.accept(c)
  expect log.turnIn(p) == []
  expect p.gold == 5 && p.xp == 0
  var s1 = makeMonster("slime", 1)
  s1.hp = 0
  log.recordKills([s1])
  expect log.status() == ["Slime Trouble: slime 1/2", "Gel Order: Slime Gel 0/1, slime 1/1", "Bones: skeleton 0/1"]
  expect log.pickUp(p, Item("Slime Gel", "junk", 0, 1, 4))
  log.recordKills([s1])
  expect a.complete() && b.complete() && !c.complete()
  expect log.turnIn(p) == ["Slime Trouble", "Gel Order"]
  expect p.gold == 25 && p.xp == 11 && p.level == 2 && p.hp == 35
  expect log.status() == ["Bones: skeleton 0/1"]
  expect log.turnIn(p) == []
  expect !log.accept(Quest("Gel Order", [Goal.Kill("wolf", 1)], 1, 1))
  log.recordKills([s1])
  expect log.status() == ["Bones: skeleton 0/1"]
  expect log.accept(Quest("More Gel", [Goal.Collect("Slime Gel", 1)], 3, 0))
  expect log.status() == ["Bones: skeleton 0/1", "More Gel: Slime Gel 0/1"]
}
