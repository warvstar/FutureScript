// list[i] and list[i] = v: the inline fast path and the runtime helper must agree on every edge.
fn get(l, i) { return l[i] }
fn put(l, i, v) { l[i] = v return l }

test "in range, including a fraction (truncated) and -0" {
  var l = [10, 20, 30]
  expect get(l, 0) == 10
  expect get(l, 2) == 30
  expect get(l, 1.9) == 20
  expect get(l, -0.0) == 10
  expect put(l, 2.5, 7) == [10, 20, 7]
}
test "out of range and non-numbers fall through to the error" {
  var l = [1, 2]
  expect throws { get(l, 2) } == "index 2 out of range (len 2)"
  expect throws { get(l, -1) } == "index -1 out of range (len 2)"
  expect throws { get(l, -0.5) } == "index -0.5 out of range (len 2)"
  expect throws { get(l, "0") } == "index 0 out of range (len 2)"
  expect throws { get(l, nil) } == "index nil out of range (len 2)"
  expect throws { put(l, 5, 1) } == "index 5 out of range (len 2)"
  expect throws { get([], 0) } == "index 0 out of range (len 0)"
}
test "maps, strings and other values still go to the helper" {
  expect get({a: 1}, "a") == 1
  expect get("hey", 1) == "e"
  expect put({}, "k", 2) == {k: 2}
  expect throws { get(5, 0) } == "cannot index number"
}
test "a list that grew is read from its new storage" {
  var l = []
  for i in 0..1000 { l.push(i) put(l, i, i * 2) }
  var s = 0
  for i in 0..1000 { s += get(l, i) }
  expect s == 999000
}
test "stores inside atomic roll back" {
  var l = [1, 2, 3]
  for i in 0..3 { put(l, i, 0) }  // the store site has run outside a transaction
  try { atomic { put(l, 0, 9) put(l, 1, 9) throw "no" } } catch e { }
  expect l == [0, 0, 0]
}
