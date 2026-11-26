import "./lists.fut"

test "sort, stable sortBy" {
  expect sort([3, 1, 2, 1]) == [1, 1, 2, 3]
  expect sort(["b", "a"]) == ["a", "b"]
  expect sort([]) == []
  expect sortBy([{k: 2, v: "a"}, {k: 1, v: "b"}, {k: 2, v: "c"}], fn(x) { return x.k }) == [{k: 1, v: "b"}, {k: 2, v: "a"}, {k: 2, v: "c"}]
  var l = [2, 1]
  sort(l)
  expect l == [2, 1]
}
test "map, filter, reduce" {
  expect map([1, 2], fn(x) { return x * 10 }) == [10, 20]
  expect filter([1, 2, 3, 4], fn(x) { return x % 2 == 0 }) == [2, 4]
  expect reduce([1, 2, 3], 0, fn(a, x) { return a + x }) == 6
}
test "find, any, all, count" {
  expect find([1, 5, 7], fn(x) { return x > 4 }) == 5
  expect find([1], fn(x) { return x > 4 }) == nil
  expect any([1, 5], fn(x) { return x > 4 }) && !all([1, 5], fn(x) { return x > 4 })
  expect count([1, 5, 6], fn(x) { return x > 4 }) == 2
}
test "search, reshape" {
  expect indexOf(["a", "b"], "b") == 1 && indexOf([], 1) == -1 && contains([1, 2], 2)
  expect reverse([1, 2, 3]) == [3, 2, 1]
  expect unique([1, 1, "a", 2, "a"]) == [1, "a", 2]
  expect flatten([[1, 2], [], [3]]) == [1, 2, 3]
  expect range(2, 5) == [2, 3, 4]
}
test "sum, min, max" {
  expect sum([1, 2, 3.5]) == 6.5 && sum([]) == 0
  expect min([3, 1, 2]) == 1 && max([3, 1, 2]) == 3 && min([]) == nil
}
test "sort is n log n" {
  expect grows allocs { n -> sort(range(0, n)) } like "n"
}
