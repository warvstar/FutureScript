// When things can happen: earliest start and finish, the critical path, who does what.
import "std/lists"
import "std/math"
import "tasks.fut"
import "graph.fut"

// Task id -> {start, finish} in days from the project start, with every task
// starting as soon as all of its deps have finished.
pub fn earliest(p: Project): map ! {
  var out = {}
  for id in topoOrder(p) {
    var t = taskOf(p, id)
    var start = 0
    for d in t.deps {
      if out.has(d) { start = math.max(start, out[d].finish) }
    }
    out[id] = {start: start, finish: start + t.durationDays}
  }
  return out
}

// Days from the start until the last task finishes.
pub fn projectLength(p: Project): num ! {
  var end = 0
  for span in earliest(p).values() { end = math.max(end, span.finish) }
  return end
}

// The chain of tasks that sets the project length, first task first.
pub fn criticalPath(p: Project): list<str> ! {
  var spans = earliest(p)
  if spans.len() == 0 { return [] }
  var last = nil
  for id in topoOrder(p) {
    if last == nil || spans[id].finish > spans[last].finish { last = id }
  }
  var path = [last]
  var t = taskOf(p, last)
  while spans[t.id].start > 0 {
    var prev = nil
    for d in t.deps {
      if spans.has(d) && spans[d].finish == spans[t.id].start { prev = d break }
    }
    if prev == nil { break }
    path.push(prev)
    t = taskOf(p, prev)
  }
  return lists.reverse(path)
}

// Days of unfinished work assigned to who.
pub fn assignedDays(p: Project, who: Person): num ! {
  var sum = 0
  for t in p.tasks {
    if !t.done && t.assignee == who.name { sum += t.durationDays }
  }
  return sum
}

// Weeks it would take who to finish everything assigned to them.
pub fn weeksBusy(p: Project, who: Person): num ! {
  return assignedDays(p, who) / who.capacityPerWeek
}

// Give every unfinished, unassigned task (in topological order) to whoever
// has the fewest weeks of work so far; ties go to whoever joined the team first.
// Returns "task: person" for each assignment made.
pub fn autoAssign(p: Project): list<str> {
  if p.team.len() == 0 { throw "{p.name} has nobody to assign work to" }
  var load = {}
  for who in p.team { load[who.name] = assignedDays(p, who) / who.capacityPerWeek }
  var made = []
  for id in topoOrder(p) {
    var t = taskOf(p, id)
    if t.done || t.assignee != nil { continue }
    var best = p.team[0]
    for who in p.team {
      if load[who.name] < load[best.name] { best = who }
    }
    t.assignee = best.name
    load[best.name] += t.durationDays / best.capacityPerWeek
    made.push("{id}: {best.name}")
  }
  return made
}

test "earliest start and finish" {
  var spans = earliest(demoProject())
  expect spans["design"] == {start: 0, finish: 3}
  expect spans["frontend"] == {start: 3, finish: 7}
  expect spans["integrate"] == {start: 8, finish: 10}
  expect spans["release"] == {start: 10, finish: 11}
}

test "project length and critical path" {
  var p = demoProject()
  expect projectLength(p) == 11
  expect criticalPath(p) == ["design", "backend", "integrate", "release"]
  expect criticalPath(Project("Empty")) == []
}

test "assigned days" {
  var p = demoProject()
  assign(p, "backend", "bo")
  assign(p, "docs", "bo")
  expect assignedDays(p, p.person("bo")) == 6
  markDone(p, "docs")
  expect assignedDays(p, p.person("bo")) == 5
  expect assignedDays(p, p.person("ann")) == 0
}

test "auto assign" {
  var p = demoProject()
  assign(p, "backend", "ann")
  expect autoAssign(p) == ["design: bo", "frontend: ann", "integrate: bo", "docs: ann", "release: ann"]
  expect throws { autoAssign(Project("Solo")) } == "Solo has nobody to assign work to"
}
