test "hidden: adding statuses" {
  var m = makeMonster("slime", 1)
  expect m.statuses == []
  addStatus(m, Status.Stun(1))
  addStatus(m, Status.Poison(3, 2))
  expect m.statuses == [Status.Stun(1), Status.Poison(3, 2)]
  addStatus(m, Status.Stun(4))
  expect m.statuses == [Status.Stun(4), Status.Poison(3, 2)]
  addStatus(m, Status.Poison(1, 5))
  expect m.statuses == [Status.Stun(4), Status.Poison(1, 5)]
  var p = Player("Ana")
  expect p.statuses == []
  addStatus(p, Status.Poison(2, 1))
  expect p.statuses == [Status.Poison(2, 1)]
  expect m.statuses.len() == 2
}

test "hidden: ticking statuses" {
  var p = Player("Ana")
  expect tickStatuses(p) == {log: [], canAct: true}
  expect p.hp == 30
  addStatus(p, Status.Poison(4, 2))
  addStatus(p, Status.Stun(1))
  expect tickStatuses(p) == {log: ["Ana takes 4 poison damage", "Ana is stunned"], canAct: false}
  expect p.hp == 26
  expect p.statuses == [Status.Poison(4, 1)]
  expect tickStatuses(p) == {log: ["Ana takes 4 poison damage"], canAct: true}
  expect p.hp == 22
  expect p.statuses == []
  expect tickStatuses(p) == {log: [], canAct: true}
  expect p.hp == 22

  var m = makeMonster("slime", 1)
  m.hp = 3
  addStatus(m, Status.Poison(5, 3))
  expect tickStatuses(m) == {log: ["slime takes 3 poison damage"], canAct: false}
  expect m.hp == 0
  expect m.statuses == [Status.Poison(5, 2)]
  expect tickStatuses(m) == {log: ["slime takes 0 poison damage"], canAct: false}
}

test "hidden: statuses in battle" {
  seed(4)
  var p = Player("Ana")
  p.def = 50
  var wolf = makeMonster("wolf", 1)
  wolf.def = 50
  wolf.hp = 1000
  addStatus(p, Status.Stun(2))
  addStatus(wolf, Status.Poison(4, 1))
  var r = simulateBattle(p, [wolf], 3)
  expect !r.won && r.rounds == 3
  expect r.log.len() == 7
  expect r.log[0] == "wolf takes 4 poison damage" && r.log[1].has("wolf hits Ana")
  expect r.log[2] == "Ana is stunned" && r.log[3].has("wolf hits Ana")
  expect r.log[4] == "Ana is stunned" && r.log[5].has("wolf hits Ana") && r.log[6].has("Ana hits wolf")
  expect p.statuses == []
  expect wolf.statuses == []

  var q = Player("Bo")
  q.hp = 3
  var slime = makeMonster("slime", 1)
  slime.speed = 9
  addStatus(slime, Status.Stun(5))
  addStatus(q, Status.Poison(10, 2))
  var r2 = simulateBattle(q, [slime], 5)
  expect !r2.won && r2.rounds == 1
  expect r2.log == ["slime is stunned", "Bo takes 3 poison damage"]
  expect q.hp == 0
}
