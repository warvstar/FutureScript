test "hidden: takeOff and isAway" {
  var p = demoProject()
  var ann = p.person("ann")
  expect !ann.isAway(0)
  ann.takeOff(2, 3)
  expect !ann.isAway(1)
  expect ann.isAway(2) && ann.isAway(3) && ann.isAway(4)
  expect !ann.isAway(5)
  ann.takeOff(4, 3)
  expect ann.isAway(5) && ann.isAway(6) && !ann.isAway(7)
  ann.takeOff(0, 1)
  expect ann.isAway(0)
  expect !p.person("bo").isAway(3)
}

test "hidden: takeOff errors" {
  var bo = demoProject().person("bo")
  expect throws { bo.takeOff(-1, 2) } == "bo: bad time off"
  expect throws { bo.takeOff(1.5, 2) } == "bo: bad time off"
  expect throws { bo.takeOff(3, 0) } == "bo: bad time off"
  expect throws { bo.takeOff(3, 2.5) } == "bo: bad time off"
  expect throws { bo.takeOff(3, -1) } == "bo: bad time off"
  expect throws { Person("cy", 3).takeOff(0, 0) } == "cy: bad time off"
  expect !bo.isAway(3)
}

test "hidden: earliestWithLeave without time off matches earliest" {
  var p = demoProject()
  autoAssign(p)
  var plain = earliest(p)
  var leave = earliestWithLeave(p)
  expect leave.len() == 6
  for id in ["design", "backend", "frontend", "integrate", "docs", "release"] {
    expect leave[id] == plain[id]
  }
  expect earliestWithLeave(Project("Empty")).len() == 0
}

test "hidden: earliestWithLeave" {
  var p = demoProject()
  p.person("bo").takeOff(4, 2)
  assign(p, "backend", "bo")
  var s = earliestWithLeave(p)
  expect s["design"] == {start: 0, finish: 3}
  expect s["backend"] == {start: 3, finish: 10}
  expect s["frontend"] == {start: 3, finish: 7}
  expect s["integrate"] == {start: 10, finish: 12}
  expect s["docs"] == {start: 3, finish: 4}
  expect s["release"] == {start: 12, finish: 13}
  // away when it becomes ready: starts on the first day back
  p.person("ann").takeOff(0, 2)
  assign(p, "design", "ann")
  assign(p, "docs", "ann")
  p.person("ann").takeOff(3, 1)
  s = earliestWithLeave(p)
  expect s["design"] == {start: 2, finish: 6}
  expect s["docs"] == {start: 6, finish: 7}
  expect s["backend"] == {start: 6, finish: 11}
  expect s["frontend"] == {start: 6, finish: 10}
  // done tasks are still scheduled
  markDone(p, "design")
  expect earliestWithLeave(p)["design"] == {start: 2, finish: 6}
}

test "hidden: earliestWithLeave unaffected tasks" {
  var p = demoProject()
  p.person("ann").takeOff(0, 20)
  p.person("bo").takeOff(0, 20)
  p.find("frontend").assignee = "cy"
  p.add(Task("check", "Check links", 0, ["design"]))
  assign(p, "check", "bo")
  var s = earliestWithLeave(p)
  expect s["design"] == {start: 0, finish: 3}
  expect s["frontend"] == {start: 3, finish: 7}
  expect s["check"] == {start: 3, finish: 3}
  expect s["release"] == {start: 10, finish: 11}
}

test "hidden: leaveDelay" {
  var p = demoProject()
  expect leaveDelay(p) == 0
  expect leaveDelay(Project("Empty")) == 0
  p.person("bo").takeOff(4, 2)
  expect leaveDelay(p) == 0
  assign(p, "backend", "bo")
  expect leaveDelay(p) == 2
  assign(p, "docs", "ann")
  p.person("ann").takeOff(3, 7)
  expect leaveDelay(p) == 2
  p.person("ann").takeOff(10, 1)
  expect leaveDelay(p) == 2
  assign(p, "release", "ann")
  p.person("ann").takeOff(12, 2)
  expect leaveDelay(p) == 4
}
