// Lists: push/pop/len, index assignment, concatenation, iteration, nesting, and GC churn.
var a = [1, 2, 3]
var t = 0
var s = ""
var grid = [[1, 2], [3, 4]]
var big = []

test "push, len, index assignment, pop and iteration" {
  a.push(4)
  expect a == [1, 2, 3, 4]
  expect prints { print(a) } == "[1, 2, 3, 4]\n"
  expect len(a) == 4
  a[0] = 10
  a[1] += 5
  expect a[0] + a[1] == 17
  expect a.pop() == 4
  for v in a { t += v }
  expect t == 20
}
test "concatenation keeps mixed elements" {
  expect [1] + [2, "x"] == [1, 2, "x"]
  expect prints { print([1] + [2, "x"]) } == "[1, 2, \"x\"]\n"
}
test "iterating a string yields its characters" {
  for ch in "hey" { s = ch + s }
  expect s == "yeh"
}
test "nested lists index" {
  expect grid[1][0] == 3
}
test "a 100000-element list survives GC churn" {
  for i in 0..100000 { big.push(str(i)) }  // churn the GC
  expect len(big) == 100000
  expect big[99999] == "99999"
}
