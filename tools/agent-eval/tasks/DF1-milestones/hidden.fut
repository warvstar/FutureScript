test "hidden: addMilestone" {
  var p = demoProject()
  expect p.milestones.len() == 0
  var beta = p.addMilestone("beta", ["frontend", "backend"])
  expect beta.name == "beta"
  expect beta.taskIds == ["frontend", "backend"]
  var launch = p.addMilestone("launch", ["release"])
  expect p.milestones.len() == 2
  expect p.milestones[0].name == "beta" && p.milestones[1].name == "launch"
  expect isa(launch, Milestone)
  var m = Milestone("x", ["docs"])
  expect m.name == "x"
  expect m.taskIds == ["docs"]
}

test "hidden: addMilestone errors" {
  var p = demoProject()
  p.addMilestone("beta", ["frontend"])
  expect throws { p.addMilestone("beta", ["docs"]) } == "duplicate milestone beta"
  expect throws { p.addMilestone("beta", []) } == "duplicate milestone beta"
  expect throws { p.addMilestone("empty", []) } == "milestone empty has no tasks"
  expect throws { p.addMilestone("demo", ["docs", "qa", "zz"]) } == "milestone demo: no task qa"
  expect p.milestones.len() == 1
  p.addMilestone("demo", ["docs"])
  expect p.milestones.len() == 2
}

test "hidden: milestoneDay" {
  var p = demoProject()
  var beta = p.addMilestone("beta", ["frontend", "backend"])
  var early = p.addMilestone("early", ["design", "docs"])
  var all = p.addMilestone("all", ["release", "design"])
  expect milestoneDay(p, beta) == 8
  expect milestoneDay(p, early) == 4
  expect milestoneDay(p, all) == 11
  markDone(p, "design")
  markDone(p, "backend")
  expect milestoneDay(p, beta) == 8
  p.find("frontend").durationDays = 6
  expect milestoneDay(p, beta) == 9
}

test "hidden: milestoneReport" {
  var p = demoProject()
  expect milestoneReport(p) == ""
  p.addMilestone("beta", ["frontend", "backend"])
  expect milestoneReport(p) == "beta: day 8, 0/2 done"
  p.addMilestone("docs out", ["docs"])
  p.addMilestone("launch", ["release", "integrate", "docs"])
  markDone(p, "docs")
  markDone(p, "backend")
  expect milestoneReport(p) == "beta: day 8, 1/2 done\ndocs out: reached\nlaunch: day 11, 1/3 done"
  markDone(p, "frontend")
  expect milestoneReport(p) == "beta: reached\ndocs out: reached\nlaunch: day 11, 1/3 done"
}

test "hidden: printReport with milestones" {
  var p = demoProject()
  markDone(p, "design")
  var head = "Website: 1/6 done, 13 days left, 11 days end to end\ncritical path: design > backend > integrate > release\n" + statusTable(p) + "\n"
  expect prints { printReport(p) } == head
  p.addMilestone("beta", ["frontend", "backend"])
  p.addMilestone("kickoff", ["design"])
  expect prints { printReport(p) } == head + "beta: day 8, 0/2 done\nkickoff: reached\n"
}
