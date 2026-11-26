import "../lib/geom.fut" as geom

class RenderPoint {
  var x
  var missing = 3
  var y
  fn init() { self.y = [2] self.x = 1 }
}

test "shortest round-tripping numbers" {
  expect prints { print(0.1 + 0.2, 1 / 3, 100000000, 1e-6, 1e-7, 1e21, -0) } ==
    "0.30000000000000004 0.3333333333333333 100000000 0.000001 1e-7 1e21 -0\n"
  expect str(1000000000000000100) == "1000000000000000100"
  expect 1000000000000000100 == 1.0000000000000001e18
  expect str(5e-324) == "5e-324"
  expect str(1.7976931348623157e308) == "1.7976931348623157e308"
}

test "shared rendering of containers and instances" {
  var p = RenderPoint()
  var v = {a: 1, "b c": "s", n: nil, "if": true, point: p}
  var want = "\{a: 1, \"b c\": \"s\", n: nil, \"if\": true, point: RenderPoint\{x: 1, y: [2]}}"
  expect str(v) == want
  expect "{v}" == want
  expect prints { print(v) } == want + "\n"
  expect str(geom.Vec(2, 3)) == "geom.Vec\{x: 2, y: 3}"
}

test "nested strings escape Fut string syntax" {
  var s = "\"\\\{\n\t\0"
  expect str([s]) == "[\"\\\"\\\\\\\{\\n\\t\\0\"]"
  expect prints { print(s) } == s + "\n"
}

test "cyclic and shared data" {
  var c = [1]
  c.push(c)
  var m = {}
  m.back = m
  var p = RenderPoint()
  p.y = p
  var shared = [2]
  expect prints { print("before") print(c, m, p, [shared, shared]) } ==
    "before\n[1, <cycle>] \{back: <cycle>} RenderPoint\{x: 1, y: <cycle>} [[2], [2]]\n"
  var other = [1]
  other.push(other)
  expect c == other
}

test "print joins an argument window before invoking one handler" {
  var out = []
  var alias = print
  handle {
    print("raw", [1, "x"], nil, true, 4, 5, 6, 7, 8, 9)
    print(0.1 + 0.2)
    alias({n: nil})
  } with print(s) { out.push(s) }
  expect out == ["raw [1, \"x\"] nil true 4 5 6 7 8 9", "0.30000000000000004", "\{n: nil}"]
  expect run({source: "print()"}).err.has("expects at least 1 argument, got 0 [arity]")
  expect throws { alias(1, 2) } == "print() expects 1 arguments, got 2"
  var print = fn(a, b) { return a + b }
  expect print(2, 3) == 5
}
