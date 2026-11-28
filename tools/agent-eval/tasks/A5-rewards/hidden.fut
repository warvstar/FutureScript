
test "hidden: collectRewards" {
  var p = Player("Ana")
  p.xp = 6
  p.hp = 12
  var slime = makeMonster("slime", 1)
  var wolf = makeMonster("wolf", 1)
  slime.hp = 0
  expect collectRewards(p, [slime, wolf]) == {gold: 2, xp: 4, levels: 1}
  expect p.gold == 2 && p.xp == 10 && p.level == 2
  expect p.maxHp == 35 && p.hp == 35 && p.atk == 7 && p.def == 3
  expect collectRewards(p, [wolf]) == {gold: 0, xp: 0, levels: 0}
  expect p.gold == 2 && p.level == 2
}

test "hidden: gainXp" {
  var q = Player("Bo")
  expect gainXp(q, 9) == 0
  expect q.level == 1
  expect gainXp(q, 31) == 2
  expect q.level == 3 && q.xp == 40 && q.maxHp == 40
}
