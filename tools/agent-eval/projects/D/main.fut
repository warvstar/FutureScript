// Website launch planner: the project, its schedule and reports.
import "tasks.fut"
import "graph.fut"
import "schedule.fut"
import "report.fut"

// Checks a project before planning it: every dep exists and nothing waits on itself.
fn problems(p: Project): list<str> {
  var out = []
  for bad in unknownDeps(p) { out.push("unknown dep: {bad}") }
  var cycle = findCycle(p)
  if cycle.len() > 0 { out.push("cycle: {cycle.join(" -> ")}") }
  return out
}

test "a clean project has no problems" {
  expect problems(demoProject()) == []
}

test "problems are listed" {
  var p = demoProject()
  p.add(Task("party", "Launch party", 1, ["venue"]))
  p.find("design").deps = ["release"]
  expect problems(p) == ["unknown dep: party -> venue", "cycle: design -> release -> docs -> design"]
}

test "plan a week of work" {
  var p = demoProject()
  autoAssign(p)
  markDone(p, "design")
  expect remainingDays(p) == 13
  expect criticalPath(p).len() == 4
  expect summary(p) == "Website: 1/6 done, 13 days left, 11 days end to end"
}
