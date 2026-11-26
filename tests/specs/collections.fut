// Specs for the built-in collections and strings; `python3 build.py test` runs every file here with `fut test`.
test "list push, pop, len and concatenation" {
  var l = [1, 2]
  l.push(3)
  expect l == [1, 2, 3]
  expect l.pop() == 3
  expect l.len() == 2
  expect l + [9] == [1, 2, 9]
  expect throws { [].pop() } == "pop() from empty list"
}
test "map keys keep insertion order; removal moves the last entry" {
  var m = {a: 1, b: 2, c: 3}
  expect m.keys() == ["a", "b", "c"]
  expect m.remove("a") == 1
  expect m.keys() == ["c", "b"]
  expect m.has("b") && !m.has("a")
  expect m.missing == nil
}
test "nested data compares by contents" {
  var order = {id: 7, lines: [{sku: "x", qty: 2}]}
  order.lines.push({sku: "y", qty: 1})
  expect order == {lines: [{sku: "x", qty: 2}, {sku: "y", qty: 1}], id: 7}
}
test "strings: interpolation, comparison, length" {
  var n = 3
  expect "n={n * 2}" == "n=6"
  expect "abc" < "abd"
  expect "héllo".len() == 6   // bytes, not characters
  expect str(1.5) + str(2) == "1.52"
}
