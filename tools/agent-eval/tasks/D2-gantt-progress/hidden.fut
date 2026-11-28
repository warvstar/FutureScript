
test "hidden: percentDone" {
  var p = demoProject()
  expect percentDone(p) == 0
  markDone(p, "design")
  expect percentDone(p) == 19
  markDone(p, "docs")
  expect percentDone(p) == 25
  for t in p.tasks { t.done = true }
  expect percentDone(p) == 100
  expect percentDone(Project("Empty")) == 0
}

test "hidden: gantt" {
  var p = demoProject()
  var want = [
    "design      |###        |",
    "backend     |   #####   |",
    "frontend    |   ####    |",
    "integrate   |        ## |",
    "docs        |   #       |",
    "release     |          #|"
  ]
  expect gantt(p) == want.join("\n")
}
