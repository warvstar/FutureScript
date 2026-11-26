// Compound types: unions, literals, shapes, list<T> and map<K, V>, named types; the field names a typed variable
// may use, checked as it compiles; and the element types lists and maps keep once a check has passed them.
type Method = "GET" | "POST"
type User = {name: str, age: num, email: str?}
type Tree = {value: num, kids: list<Tree>}
type Scores = map<str, num>

fn verb(m: Method) { return m }
fn age(u: User) { return u.age }
fn depth(t: Tree): num {
  var d = 0
  for k in t.kids { var n = depth(k) if n > d { d = n } }
  return d + 1
}
fn total(s: Scores) { var t = 0 for k in s { t += s[k] } return t }
fn ids(l: list<num>) { return l.len() }
fn either(x: list<num> | list<str>) { return x }
fn maybe(x: num | str | nil) { return x }
fn grid(g: list<list<num>>) { return g }

class Person {
  var name = ""
  var age = 0
  fn init(n, a) { self.name = n  self.age = a }
  fn greet() { return "hi " + self.name }
}
type Greeter = {name: str, greet: fn}
fn hello(g: Greeter) { return g.greet() }

fn err(f) { try { f() } catch e { return "{e}" } return "" }
fn compileErr(src) { return run({source: src}).err }

test "unions and literals" {
  expect verb("GET") == "GET"
  expect err(fn() { verb("PUT") }).has("must be Method (\"GET\" | \"POST\"), got the string \"PUT\"")
  expect maybe(1) == 1 && maybe("a") == "a" && maybe(nil) == nil
  expect err(fn() { maybe(true) }).has("must be num | str | nil")
}

test "shapes: maps and instances, open, with paths in their errors" {
  expect age({name: "ann", age: 3}) == 3
  expect age({name: "ann", age: 3, extra: true}) == 3
  expect age(Person("bo", 4)) == 4
  expect hello(Person("cy", 5)) == "hi cy"
  expect err(fn() { age({name: "ann", agee: 3}) }).has(".age is missing (it must be num); there is a 'agee': a misspelling?")
  expect err(fn() { age({name: "ann", age: 3, email: 7}) }).has(".email must be str?")
  expect depth({value: 1, kids: [{value: 2, kids: []}]}) == 2
  expect err(fn() { depth({value: 1, kids: [{value: "x", kids: []}]}) }).has(".kids[0].value must be num, got the string \"x\"")
}

test "list<T> and map<K, V>" {
  expect ids([1, 2]) == 2
  expect err(fn() { ids([1, "x"]) }).has("[1] must be num, got the string \"x\"")
  expect total({a: 1, b: 2}) == 3
  expect err(fn() { total({a: "1"}) }).has("[\"a\"] must be num")
  var m = {}
  m[1] = 1
  expect err(fn() { total(m) }).has("the key 1 must be str")
}

test "a list or map keeps its element type once checked" {
  var l = [1, 2]
  ids(l)
  l.push(3)
  l[0] = 4
  expect l == [4, 2, 3]
  expect err(fn() { l.push("x") }).has("cannot store the string \"x\" in this list: it has been list<num> since it passed as argument 'l' of ids()")
  expect err(fn() { l[1] = nil }).has("cannot store nil in this list")
  var s = {a: 1}
  total(s)
  s.b = 2
  expect err(fn() { s.c = "3" }).has("cannot store the string \"3\" in this map")
  expect err(fn() { s[1] = 1 }).has("cannot use the number 1 as a key of this map")
  var g = [[1]]
  grid(g)
  var row = [2]
  g.push(row)
  expect err(fn() { row.push("y") }).has("it has been list<num>")  // a list stored in a typed list is typed too
  expect err(fn() { g[0].push("z") }).has("it has been list<num>")
}

test "unions, failed checks and copies don't give a type" {
  var e = []
  either(e)
  e.push("fine")  // list<num> | list<str>: either could be meant
  var bad = [1, "two"]
  expect err(fn() { ids(bad) }) != ""
  bad.push(nil)
  var l = [1]
  ids(l)
  var c = l.slice(0, 1)
  c.push("ok")
  expect c == [1, "ok"]
}

test "a typed variable's fields are checked as it compiles" {
  expect compileErr("type U = \{name: str}\nfn f(u: U) \{ return u.nmae }").has("'nmae' is not a field of U (u is U: name); did you mean 'name'? [no-field]")
  expect compileErr("class A \{ var n = 0 fn go() \{ } }\nfn f(a: A) \{ a.goo() }").has("'goo' is not a method or field of A")
  expect compileErr("type U = \{name: str}\nfn f(u: U) \{ return [u.has(\"name\"), u.keys(), u.name] }") == ""
  expect compileErr("type T = T").has("[unknown-type]") || compileErr("type T = T").has("itself")
  expect compileErr("type U = \{a: num}\nprint(U)").has("[not-a-value]")
}
