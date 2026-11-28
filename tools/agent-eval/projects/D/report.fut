import "std/strings"
import "tasks.fut"
import "graph.fut"
import "schedule.fut"

pub fn formatDays(n: num): str ! {
  return n == 1 ? "1 day" : "{n} days"
}

// "[x] design    Design pages (3 days, ann)"; [ ] while not done.
pub fn formatTask(t: Task): str ! {
  var box = t.done ? "[x]" : "[ ]"
  var who = t.assignee == nil ? "unassigned" : t.assignee
  return "{box} {strings.padEnd(t.id, 12, " ")}{t.title} ({formatDays(t.durationDays)}, {who})"
}

// One formatTask line per task, in topological order.
pub fn statusTable(p: Project): str ! {
  var lines = []
  for id in topoOrder(p) { lines.push(formatTask(taskOf(p, id))) }
  return lines.join("\n")
}

// A bar per task, one column per day: "backend      |   #####   |".
pub fn gantt(p: Project): str ! {
  var spans = earliest(p)
  var width = projectLength(p)
  var rows = []
  for id in topoOrder(p) {
    var span = spans[id]
    var bar = strings.repeat(" ", span.start) + strings.repeat("#", span.finish - span.start)
    rows.push("{strings.padEnd(id, 12, " ")}|{strings.padEnd(bar, width, " ")}|")
  }
  return rows.join("\n")
}

// "Website: 2/6 done, 12 days left, 11 days end to end"
pub fn summary(p: Project): str ! {
  var done = 0
  for t in p.tasks { if t.done { done += 1 } }
  return "{p.name}: {done}/{p.tasks.len()} done, {formatDays(remainingDays(p))} left, {formatDays(projectLength(p))} end to end"
}

pub fn printReport(p: Project) ! io {
  print(summary(p))
  print("critical path: {criticalPath(p).join(" > ")}")
  print(statusTable(p))
}

// The summary with the time it was made, in whole seconds since 1970.
pub fn stampedReport(p: Project): str ! clock {
  return "{summary(p)} (as of {int(time())})"
}

test "format days and tasks" {
  expect formatDays(1) == "1 day" && formatDays(2.5) == "2.5 days"
  var p = demoProject()
  assign(p, "design", "ann")
  markDone(p, "design")
  expect formatTask(p.find("design")) == "[x] design      Design pages (3 days, ann)"
  expect formatTask(p.find("docs")) == "[ ] docs        Write docs (1 day, unassigned)"
}

test "summary" {
  var p = demoProject()
  markDone(p, "design")
  markDone(p, "docs")
  expect summary(p) == "Website: 2/6 done, 12 days left, 11 days end to end"
}

test "printed report" {
  var p = demoProject()
  markDone(p, "design")
  expect prints { printReport(p) } == "Website: 1/6 done, 13 days left, 11 days end to end\ncritical path: design > backend > integrate > release\n" + statusTable(p) + "\n"
}

test "stamped report" {
  var p = demoProject()
  var s = ""
  handle { s = stampedReport(p) } with time() { return 1700000000.5 }
  expect s == "Website: 0/6 done, 16 days left, 11 days end to end (as of 1700000000)"
}
