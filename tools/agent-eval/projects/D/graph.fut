// The dependency graph: who waits on whom, cycles, and an order to do things in.
import "tasks.fut"

// Each task id -> the ids of the tasks that list it as a dep, in project order.
pub fn dependents(p: Project): map ! {
  var out = {}
  for t in p.tasks { out[t.id] = [] }
  for t in p.tasks {
    for d in t.deps {
      if out.has(d) { out[d].push(t.id) }
    }
  }
  return out
}

// "task -> dep" for every dep that names no task in the project.
pub fn unknownDeps(p: Project): list<str> ! {
  var out = []
  for t in p.tasks {
    for d in t.deps {
      if p.find(d) == nil { out.push("{t.id} -> {d}") }
    }
  }
  return out
}

// A cycle as a path that ends where it starts (["a", "b", "a"]), or [] when there is none.
pub fn findCycle(p: Project): list<str> ! {
  var state = {}   // id -> "open" while on the path, "closed" once finished
  var path = []
  var found = []
  fn visit(id) {
    if found.len() > 0 { return }
    if state[id] == "closed" { return }
    if state[id] == "open" {
      var i = path.len() - 1
      while path[i] != id { i -= 1 }
      for k in i..path.len() { found.push(path[k]) }
      found.push(id)
      return
    }
    state[id] = "open"
    path.push(id)
    var t = p.find(id)
    if t != nil { for d in t.deps { visit(d) } }
    path.pop()
    state[id] = "closed"
  }
  for t in p.tasks { visit(t.id) }
  return found
}

// Every task id, each after all of its deps. Among tasks that are free at
// the same time, the one added to the project first comes first.
pub fn topoOrder(p: Project): list<str> ! {
  var cycle = findCycle(p)
  if cycle.len() > 0 { throw "cycle: {cycle.join(" -> ")}" }
  var placed = {}
  var order = []
  while order.len() < p.tasks.len() {
    for t in p.tasks {
      if placed.has(t.id) { continue }
      var free = true
      for d in t.deps {
        if !placed.has(d) && p.find(d) != nil { free = false }
      }
      if free {
        placed[t.id] = true
        order.push(t.id)
        break
      }
    }
  }
  return order
}

test "dependents" {
  var deps = dependents(demoProject())
  expect deps["design"] == ["backend", "frontend", "docs"]
  expect deps["integrate"] == ["release"]
  expect deps["release"] == []
}

test "unknown deps" {
  var p = demoProject()
  expect unknownDeps(p) == []
  p.add(Task("launch", "Launch party", 1, ["release", "venue"]))
  expect unknownDeps(p) == ["launch -> venue"]
}

test "cycles" {
  var p = demoProject()
  expect findCycle(p) == []
  p.find("design").deps = ["release"]
  expect findCycle(p) == ["design", "release", "docs", "design"]
  expect throws { topoOrder(p) } == "cycle: design -> release -> docs -> design"
}

test "topological order" {
  expect topoOrder(demoProject()) == ["design", "backend", "frontend", "integrate", "docs", "release"]
}
