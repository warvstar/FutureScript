
test "hidden: blockedBy" {
  var p = demoProject()
  expect blockedBy(p, "design") == ["backend", "frontend", "integrate", "docs", "release"]
  expect blockedBy(p, "frontend") == ["integrate", "release"]
  expect blockedBy(p, "docs") == ["release"]
  expect blockedBy(p, "release") == []
  expect throws { blockedBy(p, "nope") } == "no task nope in Website"
}

test "hidden: parseTeam" {
  var team = parseTeam("Ann: 5, Bo: 2.5")
  expect team.len() == 2
  expect team[0].name == "Ann" && team[0].capacityPerWeek == 5
  expect team[1].name == "Bo" && team[1].capacityPerWeek == 2.5
  expect throws { parseTeam("Cy: 9") } == "Cy can't work 9 days a week"
}
