// Buffers: f64s, f32s, u32s, i32s and u8s hold numbers as 64- or 32-bit floats, 32-bit whole numbers or bytes, for
// speed, the GPU and C, outside every heap. Their length is fixed; each store is checked (and an f32 rounded), a `ptr`
// parameter of an extern fn gets their bytes, and sending one to another isolate moves it.
fn err(f) { try { f() } catch e { return "{e}" } return "" }

test "made from a length, a list or another buffer" {
  expect f32s(3) == f32s([0, 0, 0])
  expect u32s([1, 2]).len() == 2 && len(u8s(5)) == 5
  expect i32s(u32s([7])) == i32s([7])
  expect "{f32s([1, 0.1, 2.5])}" == "f32s[1, 0.1, 2.5]"
  expect err(fn() { f32s(-1) }).has("expects a length")
  expect err(fn() { f32s("x") }).has("a length, a list or a buffer")
}

test "indexing, iterating and slicing" {
  var b = f32s(4)
  b[1] = 0.5
  b[2] = 0.1
  expect b[1] == 0.5 && b[2] != 0.1 && b[2] == f32s([0.1])[0]  // stored as an f32
  var t = 0
  for x in b { t += x }
  expect t > 0.59 && t < 0.61
  expect b.slice(1, 3) == f32s([0.5, 0.1])  // (a top-level == compares contents)
  expect b.slice(3, 1).len() == 0
  expect err(fn() { b[4] = 1 }).has("index 4 out of range (len 4)")
}

test "stores are checked" {
  var u = u32s(1)
  u[0] = 4294967295
  expect err(fn() { u[0] = -1 }).has("cannot store -1 in u32s: it holds whole numbers from 0 to 4294967295")
  expect err(fn() { u8s(1)[0] = 256 }).has("from 0 to 255")
  expect err(fn() { i32s(1)[0] = 1.5 }).has("whole numbers")
  expect err(fn() { f32s(1)[0] = "x" }).has("cannot store string in f32s")
  expect err(fn() { u8s([1, "2"]) }) != ""
}

fn id(v) { return v }
fn total(xs: f32s): num { var t = 0 for x in xs { t += x } return t }

test "a buffer type in annotations" {
  expect total(f32s([1, 2])) == 3
  expect err(fn() { total(u32s([1])) }).has("must be f32s, got")
  expect err(fn() { total(id([1])) }).has("must be f32s")  // total([1]) itself is a compile error
}

extern fn memset(ptr, int, long): ptr

test "an extern's ptr parameter gets the bytes" {
  var m = u8s(4)
  memset(m, 9, 3)
  expect m == u8s([9, 9, 9, 0])
}

test "an atomic block that throws puts its writes back" {
  var b = u32s([1, 2])
  try { atomic { b[0] = 5 throw "no" } } catch e { }
  expect b == u32s([1, 2])
}

test "JSON gets their numbers" {
  expect json(f32s([1.5, 2])) == "[1.5,2]"
  expect json(f64s([0.5, 3])) == "[0.5,3]"
}

test "an f32s in a hot loop: compiled inline, with NaN and the edges still exact" {
  var b = f32s(1000)
  for r in 0..3 { for i in 0..1000 { b[i] = b[i] + i } }
  expect b[999] == 2997 && b[0] == 0
  b[1] = 0 / 0
  expect b[1] != b[1]  // a NaN stays a NaN
  b[2] = 1e40
  expect b[2] == 1 / 0  // too big for an f32: infinity
  expect throws { b[1000] }
  expect throws { b[-1] = 1 }
  expect throws { b[0] = nil }
}

fn errorOf(f) {
  try { f() } catch e { return e }
  return nil
}
fn fillWith(b, x) { return b.fill(x) }
var G = f64s(2)
fn readG() { return errorOf(fn() { return G[0] }) }

test "f64s hold any number, and fill, toList, bytes" {
  var b = f64s(3)
  b[1] = 0.1
  b[2] += 1
  expect b[1] == 0.1 && b[2] == 1
  expect "{b}" == "f64s[0, 0.1, 1]"
  expect b.toList() == [0, 0.1, 1]
  expect b.bytes() == 24 && u8s(3).bytes() == 3
  expect u8s(2).fill(7) == u8s([7, 7])
  expect err(fn() { i32s(2).fill(0.5) }).has("whole numbers")
}

test "every type indexes inline in hot loops, reads and checked stores" {
  var d = f64s(500)
  var w = i32s(500)
  var u = u8s(500)
  var q = u32s(500)
  var s = 0
  for k in 0..100 {
    for i in 0..500 { d[i] = i * 0.5  w[i] = i - 250  u[i] = i % 256  q[i] = i * 8000000 }
    s = 0
    for i in 0..500 { s += d[i] + w[i] + u[i] + q[i] }
  }
  expect s == 62375 - 250 + 62286 + 998000000000
  var errs = 0
  for k in 0..1000 {
    if errorOf(fn() { u[0] = 256 + k % 3 }) != nil { errs += 1 }
    if errorOf(fn() { q[0] = -1 - k % 2 }) != nil { errs += 1 }
    if errorOf(fn() { w[0] = 0.5 }) != nil { errs += 1 }
  }
  expect errs == 3000
  q[0] = 4294967295
  u[0] = -0
  expect q[0] == 4294967295 && u[0] == 0
}

test "sending one moves it: the sender's handle is dead" {
  var b = f64s([1, 2, 3])
  var t = spawn fillWith(b, 9)
  expect errorOf(fn() { return b[0] }).has("was sent to another isolate")
  expect errorOf(fn() { return b.len() }).has("was sent")
  var back = t.join()
  expect back == f64s([9, 9, 9])
  expect t.join() == back
  var c = channel(2)
  var u = u8s([1, 2])
  c.send({a: u, b: u})
  var m = c.recv()
  expect m.a[1] == 2
  expect errorOf(fn() { c.send(u) }).has("was sent")
  expect errorOf(fn() { atomic { c.send(m.a) } }).has("can't be sent inside atomic")
  expect m.a[0] == 1
}

test "a spawned isolate's globals don't take their buffers" {
  expect (spawn readG()).join().has("belongs to another isolate")
  G[0] = 5
  expect G[0] == 5
}
