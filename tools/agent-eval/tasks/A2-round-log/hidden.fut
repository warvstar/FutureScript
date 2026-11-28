
test "hidden: splitGold" {
  expect splitGold(10, 3) == [4, 3, 3]
  expect splitGold(2, 4) == [1, 1, 0, 0]
  expect splitGold(9, 3) == [3, 3, 3]
  expect splitGold(0, 2) == [0, 0]
  expect splitGold(7, 1) == [7]
  expect splitGold(23, 5) == [5, 5, 5, 4, 4]
}

test "hidden: round narration" {
  var p = Player("Ana")
  var m = makeMonster("wolf", 1)
  expect narrateRound(p, m, 4, 3) == ["Ana hits wolf for 4", "wolf hits Ana for 3"]
  expect narrateRound(p, m, 0, 0) == ["Ana misses wolf", "wolf misses Ana"]
}
