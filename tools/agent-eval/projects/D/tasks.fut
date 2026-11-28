// Tasks, the people who do them, and the project that holds both.

pub class Person {
  var name: str
  var capacityPerWeek: num   // days of work they can take on per week
  fn init(name: str, capacityPerWeek: num) {
    self.name = name
    self.capacityPerWeek = capacityPerWeek
  }
}

pub class Task {
  var id: str
  var title: str
  var durationDays: num
  var deps: list<str> = []
  var assignee: str? = nil
  var done: bool = false
  fn init(id: str, title: str, durationDays: num, deps: list<str>) {
    self.id = id
    self.title = title
    self.durationDays = durationDays
    self.deps = deps
  }
}

pub class Project {
  var name: str
  var tasks: list<Task> = []
  var team: list<Person> = []
  fn init(name: str) { self.name = name }
  fn add(t: Task): Task {
    if self.find(t.id) != nil { throw "duplicate task id {t.id}" }
    self.tasks.push(t)
    return t
  }
  fn hire(who: Person): Person {
    self.team.push(who)
    return who
  }
  fn find(id: str): Task? {
    for t in self.tasks { if t.id == id { return t } }
    return nil
  }
  fn person(name: str): Person? {
    for who in self.team { if who.name == name { return who } }
    return nil
  }
}

// The task with this id, or an error naming it.
pub fn taskOf(p: Project, id: str): Task {
  var t = p.find(id)
  if t == nil { throw "no task {id} in {p.name}" }
  return t
}

pub fn markDone(p: Project, id: str) {
  taskOf(p, id).done = true
}

pub fn assign(p: Project, id: str, name: str) {
  if p.person(name) == nil { throw "{name} is not on the {p.name} team" }
  taskOf(p, id).assignee = name
}

pub fn remainingDays(p: Project): num ! {
  var sum = 0
  for t in p.tasks { if !t.done { sum += t.durationDays } }
  return sum
}

// Can t start now: it isn't done and every task it depends on is?
pub fn isReady(p: Project, t: Task): bool {
  if t.done { return false }
  for d in t.deps {
    var dep = p.find(d)
    if dep == nil || !dep.done { return false }
  }
  return true
}

// The ids of the first n tasks that are ready to start, in project order.
pub fn nextUp(p: Project, n: num): list<str> {
  var out = []
  for t in p.tasks {
    if out.len() >= n { break }
    if isReady(p, t) { out.push(t.id) }
  }
  return out
}

// A team from text like "Ann: 5, Bo: 2.5": each name with the days a week
// they can work, more than 0 and at most 7.
pub fn parseTeam(spec: str): list<Person> {
  var team = []
  for part in spec.split(",") {
    if part.trim() == "" { continue }
    var bits = part.split(":")
    if bits.len() != 2 { throw "bad team entry '{part.trim()}'" }
    var days = parseNum(bits[1].trim())
    if days == nil { throw "bad capacity for {bits[0].trim()}" }
    var person = Person(bits[0].trim(), days)
    if person.capacityPerWeek <= 0 || person.capacityPerWeek > 7 {
      throw "{person.name} can't work {days} days a week"
    }
    team.push(person)
  }
  return team
}

pub fn teamCapacity(team: list<Person>): num ! {
  var sum = 0
  for who in team { sum += who.capacityPerWeek }
  return sum
}

// The project the tests share: a small website launch.
pub fn demoProject(): Project {
  var p = Project("Website")
  p.add(Task("design", "Design pages", 3, []))
  p.add(Task("backend", "Build API", 5, ["design"]))
  p.add(Task("frontend", "Build pages", 4, ["design"]))
  p.add(Task("integrate", "Wire pages to API", 2, ["frontend", "backend"]))
  p.add(Task("docs", "Write docs", 1, ["design"]))
  p.add(Task("release", "Ship it", 1, ["docs", "integrate"]))
  p.hire(Person("ann", 5))
  p.hire(Person("bo", 2.5))
  return p
}

test "find and taskOf" {
  var p = demoProject()
  expect p.find("docs").title == "Write docs"
  expect p.find("nope") == nil
  expect throws { taskOf(p, "nope") } == "no task nope in Website"
  expect throws { p.add(Task("docs", "Again", 1, [])) } == "duplicate task id docs"
}

test "assign checks the team" {
  var p = demoProject()
  assign(p, "backend", "ann")
  expect p.find("backend").assignee == "ann"
  expect throws { assign(p, "backend", "cy") } == "cy is not on the Website team"
}

test "days" {
  var p = demoProject()
  expect remainingDays(p) == 16
  markDone(p, "design")
  markDone(p, "docs")
  expect remainingDays(p) == 12
}

test "ready tasks" {
  var p = demoProject()
  expect isReady(p, p.find("design"))
  expect !isReady(p, p.find("backend"))
  markDone(p, "design")
  expect isReady(p, p.find("backend")) && !isReady(p, p.find("design"))
}

test "team capacity" {
  var p = demoProject()
  expect teamCapacity(p.team) == 7.5
}
