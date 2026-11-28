
test "hidden: overbooked" {
  var p = demoProject()
  expect overbooked(p, 0) == []
  assign(p, "backend", "bo")
  assign(p, "frontend", "ann")
  assign(p, "integrate", "ann")
  expect overbooked(p, 1) == ["ann", "bo"]
  expect overbooked(p, 1.5) == ["bo"]
  expect overbooked(p, 2) == []
  markDone(p, "backend")
  expect overbooked(p, 1) == ["ann"]
}

test "hidden: weeksBusy" {
  var p = demoProject()
  assign(p, "backend", "bo")
  assign(p, "docs", "ann")
  expect weeksBusy(p, p.person("bo")) == 2
  expect weeksBusy(p, p.person("ann")) == 0.2
}
