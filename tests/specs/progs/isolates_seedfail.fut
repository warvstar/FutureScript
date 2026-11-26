test "fails after its isolates ran" {
  var t = spawn fn() { return 1 }()
  expect t.join() == 2
}
