
test "hidden: planDay" {
  var p = demoProject()
  expect planDay(p, 3) == ["design (unassigned)"]
  markDone(p, "design")
  assign(p, "backend", "bo")
  expect planDay(p, 2) == ["backend (bo)", "frontend (unassigned)"]
  expect planDay(p, 1) == ["backend (bo)"]
  expect planDay(p, 0) == []
  expect planDay(p, 5) == ["backend (bo)", "frontend (unassigned)", "docs (unassigned)"]
}

test "hidden: nextUp" {
  var p = demoProject()
  markDone(p, "design")
  expect nextUp(p, 2) == ["backend", "frontend"]
  expect nextUp(p, 1) == ["backend"]
}
