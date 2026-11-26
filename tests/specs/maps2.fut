// Maps: counting, keys, remove, len, iteration, nested literals, functions as values.
var m = {}
var total = 0
var ops = {inc: fn(x) { return x + 1 }, twice: fn(x) { return x * 2 }}

test "counting words, then keys, remove, len and iteration" {
  for w in ["a", "b", "a", "c", "a"] { if m.has(w) { m[w] += 1 } else { m[w] = 1 } }
  expect m == {a: 3, b: 1, c: 1}
  expect prints { print(m) } == "\{\"a\": 3, \"b\": 1, \"c\": 1}\n"
  expect m.keys() == ["a", "b", "c"]
  expect m.remove("b") == 1
  expect len(m) + m.len() == 4
  for k in m { total += m[k] }
  expect total == 4
}
test "indexing into nested literals" {
  expect {1: {2: [3]}}[1][2][0] == 3
}
test "functions stored in a map are called through it" {
  expect ops.twice(ops.inc(4)) == 10
}
