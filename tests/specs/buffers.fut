// Buffers: buffer(n) (f64), i32buffer(n), u8buffer(n), or each from a list of numbers. Fixed length, outside every
// heap; sending one (a channel, spawn's arguments, a task's result) moves it instead of copying it.
fn errorOf(f) {
  try { f() } catch e { return e }
  return nil
}
fn fill(b, x) {
  for i in 0..b.len() { b[i] = x }
  return b
}
fn total(b) {
  var s = 0
  for x in b { s += x }
  return s
}
fn store(b, v) { return errorOf(fn() { b[0] = v }) }
var G = buffer(2)
fn readG() { return errorOf(fn() { return G[0] }) }

test "a buffer holds numbers, zeros to start" {
  var b = buffer(3)
  expect b.len() == 3 && len(b) == 3
  expect b.toList() == [0, 0, 0]
  b[1] = 2.5
  b[2] += 1
  expect b[1] == 2.5 && b[2] == 1
  expect str(b) == "buffer([0, 2.5, 1])"
  expect json(b) == "[0,2.5,1]"
  expect total(buffer([1, 2, 3])) == 6
  expect buffer([1, 2]) == buffer([1, 2])  // (as lists: expect compares what they hold; == elsewhere, which one)
  expect str(buffer([1, 2, 3]).slice(1, 3)) == "buffer([2, 3])"
  expect str(buffer(2).fill(7)) == "buffer([7, 7])"
  expect buffer(0).len() == 0
  expect buffer(4).type() == "f64" && i32buffer(1).type() == "i32" && u8buffer(1).type() == "u8"
}
test "indexes are checked, and a buffer holds only numbers" {
  var b = buffer(2)
  expect errorOf(fn() { return b[2] }).has("out of range (len 2)")
  expect errorOf(fn() { return b[-1] }).has("out of range")
  expect store(b, "x").has("a buffer holds numbers, got string")
  expect store(b, nil).has("got nil")
  expect errorOf(fn() { buffer([1, "a"]) }).has("element 1")
  expect errorOf(fn() { buffer(1.5) }).has("whole number")
  expect errorOf(fn() { buffer("x") }).has("a length or a list of numbers")
}
test "i32 and u8 buffers take whole numbers in range, and raise otherwise" {
  var u = u8buffer(1)
  expect store(u, 255) == nil && u[0] == 255
  expect store(u, -0) == nil && u[0] == 0
  expect store(u, 256).has("from 0 to 255, got 256")
  expect store(u, -1).has("got -1")
  expect store(u, 1.5).has("got 1.5")
  expect store(u, 0 / 0) != nil
  var w = i32buffer(1)
  expect store(w, -2147483648) == nil && w[0] == -2147483648
  expect store(w, 2147483647) == nil && w[0] == 2147483647
  expect store(w, 2147483648).has("from -2^31 to 2^31 - 1")
  expect store(w, 2.5) != nil
  expect str(u8buffer([1, 2])) == "u8buffer([1, 2])" && str(i32buffer([-1])) == "i32buffer([-1])"
  expect errorOf(fn() { u8buffer([1, 300]) }).has("element 1")
}
test "hot loops index buffers as compiled code (each type, reads and stores)" {
  var f = buffer(500)
  var w = i32buffer(500)
  var u = u8buffer(500)
  var s = 0
  for k in 0..200 {
    for i in 0..500 { f[i] = i * 0.5  w[i] = i - 250  u[i] = i % 256 }
    s = 0
    for i in 0..500 { s += f[i] + w[i] + u[i] }
  }
  expect s == 62375 - 250 + 62286
  var errs = 0
  for k in 0..2000 { if store(u, 256 + k % 3) != nil { errs += 1 } }
  expect errs == 2000
}
test "a transaction puts a buffer back" {
  var b = i32buffer([1, 2])
  try { atomic { b[0] = 9  b.fill(5)  throw "no" } } catch e { }
  expect b[0] == 1 && b[1] == 2
  atomic { b[1] = 7 }
  expect b[1] == 7
}
test "sending a buffer moves it: the sender's handle is dead" {
  var b = buffer([1, 2, 3])
  var t = spawn fill(b, 9)
  expect errorOf(fn() { return b[0] }).has("was sent to another isolate")
  expect errorOf(fn() { return b.len() }).has("was sent")
  var back = t.join()
  expect str(back) == "buffer([9, 9, 9])"
  expect t.join() == back
  var c = channel(2)
  var u = u8buffer([1, 2])
  c.send({a: u, b: u})
  var m = c.recv()
  expect m.a == m.b && m.a[1] == 2
  expect errorOf(fn() { c.send(u) }).has("was sent")
  expect errorOf(fn() { atomic { c.send(m.a) } }).has("can't be sent inside atomic")
  expect m.a[0] == 1
}
test "a spawned isolate's globals don't take their buffers" {
  expect (spawn readG()).join().has("belongs to another isolate")
  G[0] = 5
  expect G[0] == 5
}
