test "hidden: lagDays in earliest" {
  var p = demoProject()
  expect p.find("design").lagDays == 0
  expect Task("x", "X", 1, []).lagDays == 0
  p.find("frontend").lagDays = 2
  var s = earliest(p)
  expect s["frontend"] == {start: 5, finish: 9}
  expect s["integrate"] == {start: 9, finish: 11}
  expect s["release"] == {start: 11, finish: 12}
  expect s["backend"] == {start: 3, finish: 8}
  expect projectLength(p) == 12
  p.find("design").lagDays = 1
  s = earliest(p)
  expect s["design"] == {start: 1, finish: 4}
  expect s["docs"] == {start: 4, finish: 5}
  p.find("release").lagDays = 0.5
  expect earliest(p)["release"] == {start: 12.5, finish: 13.5}
  expect summary(p) == "Website: 0/6 done, 16 days left, 13.5 days end to end"
}

test "hidden: lagDays in criticalPath" {
  var p = demoProject()
  p.find("frontend").lagDays = 2
  expect criticalPath(p) == ["design", "frontend", "integrate", "release"]
  p.find("frontend").lagDays = 0
  p.find("docs").lagDays = 9
  expect criticalPath(p) == ["design", "docs", "release"]
  p.find("design").lagDays = 2
  expect criticalPath(p) == ["design", "docs", "release"]
  p.find("docs").lagDays = 0
  p.find("integrate").lagDays = 3
  expect criticalPath(p) == ["design", "backend", "integrate", "release"]
  expect projectLength(p) == 16
}

test "hidden: lagDays in gantt" {
  var p = Project("Mini")
  p.add(Task("a", "A", 2, []))
  p.add(Task("b", "B", 1, ["a"]))
  p.find("b").lagDays = 2
  expect gantt(p) == "a           |##   |\nb           |    #|"
}

test "hidden: formatTask with lag" {
  var p = demoProject()
  p.find("frontend").lagDays = 2
  expect formatTask(p.find("frontend")) == "[ ] frontend    Build pages (4 days, unassigned, waits 2 days)"
  assign(p, "docs", "bo")
  markDone(p, "docs")
  p.find("docs").lagDays = 1
  expect formatTask(p.find("docs")) == "[x] docs        Write docs (1 day, bo, waits 1 day)"
  expect formatTask(p.find("design")) == "[ ] design      Design pages (3 days, unassigned)"
}

test "hidden: whatIf" {
  var p = demoProject()
  expect whatIf(p, "frontend", 3) == ["frontend: day 7 -> day 10", "integrate: day 10 -> day 12", "release: day 11 -> day 13"]
  expect whatIf(p, "frontend", 1) == ["frontend: day 7 -> day 8"]
  expect whatIf(p, "docs", 3) == ["docs: day 4 -> day 7"]
  expect whatIf(p, "docs", 7) == ["docs: day 4 -> day 11", "release: day 11 -> day 12"]
  expect whatIf(p, "release", 2) == ["release: day 11 -> day 13"]
  expect whatIf(p, "design", 1) == ["design: day 3 -> day 4", "backend: day 8 -> day 9", "frontend: day 7 -> day 8", "integrate: day 10 -> day 11", "docs: day 4 -> day 5", "release: day 11 -> day 12"]
  expect whatIf(p, "backend", 0) == []
}

test "hidden: whatIf adds to an existing lag and leaves the project alone" {
  var p = demoProject()
  p.find("frontend").lagDays = 1
  expect whatIf(p, "frontend", 1) == ["frontend: day 8 -> day 9", "integrate: day 10 -> day 11", "release: day 11 -> day 12"]
  expect p.find("frontend").lagDays == 1
  expect earliest(p)["release"] == {start: 10, finish: 11}
  expect whatIf(p, "frontend", 1) == ["frontend: day 8 -> day 9", "integrate: day 10 -> day 11", "release: day 11 -> day 12"]
  expect throws { whatIf(p, "qa", 1) } == "no task qa in Website"
  expect p.find("frontend").lagDays == 1
}
