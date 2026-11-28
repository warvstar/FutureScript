
test "hidden: tryFlee" {
  var p = Player("Ana")
  var slime = makeMonster("slime", 1)
  var wolf = makeMonster("wolf", 1)
  expect tryFlee(p, [], 0.99)
  expect tryFlee(p, [slime], 0.75)
  expect !tryFlee(p, [slime], 0.85)
  expect !tryFlee(p, [slime, wolf], 0.5)
  expect !tryFlee(p, [wolf, slime], 0.25)
  expect tryFlee(p, [slime, wolf], 0.15)
  wolf.hp = 0
  expect tryFlee(p, [slime, wolf], 0.5)
  expect wolf.hp == 0 && slime.hp == 8
  p.speed = 20
  expect fleeChance(p, slime) == 0.9
  expect tryFlee(p, [slime], 0.89)
  expect !tryFlee(p, [slime], 0.95)
}
