// String interpolation: expressions, nested braces, escaping, and lists/calls inside.
var x = 3
var name = "fut"

test "interpolated expressions" {
  expect "{name} has {x + 1} things" == "fut has 4 things"
  expect "{x}" == "3"
  expect "list {[1, 2]} map {len({a: 1})}" == "list [1, 2] map 1"
}
test "a map literal inside an interpolation" {
  expect "nested {{1: 2}}" == "nested \{1: 2}"
}
test "an escaped brace is not interpolated" {
  expect "escaped \{x}".len() == 11   // "escaped {x}", not "escaped 3"
  expect prints { print("escaped \{x}") } == "escaped \{x}\n"
}
