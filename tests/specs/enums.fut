// enum: a closed set of variants, each with its own fields; match takes a value apart by variant.
enum Shape {
  Circle(r: num)
  Rect(w: num, h: num)
  Empty
}
enum Tree {
  Leaf
  Node(left: Tree, value, right: Tree)
}

fn area(s: Shape): num {
  match s {
    Circle(r) { return 3 * r * r }
    Rect(w, h) { return w * h }
    Empty { return 0 }
  }
}
fn insert(t: Tree, v): Tree {
  match t {
    Leaf { return Tree.Node(Tree.Leaf, v, Tree.Leaf) }
    Node(l, x, r) {
      if v < x { return Tree.Node(insert(l, v), x, r) }
      return Tree.Node(l, x, insert(r, v))
    }
  }
}
fn walk(t: Tree, out) {
  match t {
    Node(l, x, r) { walk(l, out) out.push(x) walk(r, out) }
    else { }
  }
  return out
}
type Method = "GET" | "POST"
enum Req {
  Get(url: str, headers: map<str, str>)
  Post(at: {x: num, y: num}, m: Method, tags: list<str>)
}
fn err(src) { return run({source: src}).err }

test "variants are made by name, print as made, and compare by value" {
  expect area(Shape.Circle(2)) == 12
  expect area(Shape.Rect(3, 4)) == 12
  expect area(Shape.Empty) == 0
  expect str(Shape.Rect(3, 4)) == "Shape.Rect(3, 4)"
  expect str(Shape.Empty) == "Shape.Empty"
  expect str([Tree.Node(Tree.Leaf, "a", Tree.Leaf)]) == "[Tree.Node(Tree.Leaf, \"a\", Tree.Leaf)]"
  expect Shape.Rect(1, 2) == Shape.Rect(1, 2)
  expect Shape.Rect(1, 2) != Shape.Rect(2, 1)
  expect Shape.Empty == Shape.Empty
  expect Shape.Circle(1) != Shape.Empty
  expect Shape.Circle(5).r == 5
  var t = Tree.Leaf
  for v in [5, 2, 8, 1, 9, 3] { t = insert(t, v) }
  expect walk(t, []) == [1, 2, 3, 5, 8, 9]
  expect insert(Tree.Leaf, 1) == Tree.Node(Tree.Leaf, 1, Tree.Leaf)
}

test "an enum and its variants are types" {
  var s: Shape = Shape.Circle(1)
  var c: Shape.Circle = Shape.Circle(2)
  s = c
  expect throws { c = Shape.Empty } == "var 'c' must be Shape.Circle, got Shape.Empty"
  expect throws { Shape.Circle("x") } == "field 'r' of Shape.Circle must be num, got the string \"x\""
}

test "fields of any type: generics, shapes, named types" {
  var r = Req.Post({x: 1, y: 2}, "POST", ["a"])
  match r { Post(at, m, _) { expect [at.x, m] == [1, "POST"] } Get { } }
  expect throws { Req.Post({x: 1, y: 2}, "PUT", []) } == "field 'm' of Req.Post must be Method (\"GET\" | \"POST\"), got the string \"PUT\""
  expect str(Req.Get("u", {a: "b"})) == "Req.Get(\"u\", \{a: \"b\"})"
}

test "arms: _ skips a field, a bare name skips them all, else takes the rest" {
  var seen = []
  for s in [Shape.Rect(3, 4), Shape.Circle(1), Shape.Empty] {
    match s {
      Shape.Rect(_, h) { seen.push(h) }
      Circle { seen.push("c") }
      else { seen.push("other") }
    }
  }
  expect seen == [4, "c", "other"]
}

test "match in loops: break and continue, and numbers proven before stay proven" {
  var n = 0
  var total = 0
  for s in [Shape.Circle(1), Shape.Empty, Shape.Rect(2, 2), Shape.Circle(9)] {
    n = n * 1
    match s {
      Empty { continue }
      Circle(r) { if r > 5 { break } total += r }
      Rect(w, h) { total += w * h }
    }
    n += 1
  }
  expect [n, total] == [2, 5]
}

test "a value that is not one of the variants raises" {
  var v = 3
  expect throws { match v { Circle(r) { } Rect(w, h) { } Empty { } } } == "match on Shape got 3, which is not one of its variants"
}

test "compile errors" {
  var e = "enum E \{ A(x) B }\n"
  expect err(e + "fn f(v) \{ match v \{ A(x) \{ } } }").has("match on E misses B: add an arm for each, or else \{ } [match]")
  expect err(e + "fn f(v) \{ match v \{ A(x) \{ } A(y) \{ } else \{ } } }").has("this match already has an arm for A [duplicate]")
  expect err(e + "fn f(v) \{ match v \{ A(x, y) \{ } else \{ } } }").has("E.A has 1 fields (x), and the arm names 2 [arity]")
  expect err(e + "print(E.C)").has("enum E has no variant 'C' (it has A, B) [undefined-name]")
  expect err(e + "print(E.A(1, 2))").has("E.A expects 1 arguments (x), got 2 [arity]")
  expect err(e + "print(E.A)").has("E.A has fields: make one with E.A(x) [arity]")
  expect err(e + "print(E.B())").has("E.B has no fields: write it without parentheses [arity]")
  expect err(e + "print(E())").has("E is an enum: make one of its variants, E.A [class]")
  expect err(e + "class F : E \{ }").has("'E' belongs to an enum: its variants are its only classes (add one to the enum) [class]")
  expect err(e + "enum G \{ A }\nfn f(v) \{ match v \{ A \{ } else \{ } } }").has("'A' is a variant of several enums")
  expect err("enum E \{ A A }").has("duplicate variant 'A' [duplicate]")
}

test "match is an ordinary name where one is defined" {
  var match = 2
  expect match + 1 == 3
}
