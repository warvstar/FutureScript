
test "hidden: unassigned" {
  var p = demoProject()
  expect unassigned(p) == ["design", "backend", "frontend", "integrate", "docs", "release"]
  assign(p, "design", "ann")
  markDone(p, "docs")
  expect unassigned(p) == ["backend", "frontend", "integrate", "release"]
  expect unassigned(Project("Empty")) == []
}

test "hidden: parseTeam" {
  var team = parseTeam("Ann: 7, Bo: 0.5")
  expect team.len() == 2
  expect team[0].name == "Ann" && team[0].capacityPerWeek == 7
  expect team[1].name == "Bo" && team[1].capacityPerWeek == 0.5
  expect throws { parseTeam("Cy: 7.5") } == "Cy can't work 7.5 days a week"
}
