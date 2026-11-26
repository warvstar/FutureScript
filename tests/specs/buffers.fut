// Packed buffers: f32s, u32s, i32s and u8s hold numbers as 32-bit floats, 32-bit whole numbers or bytes, for the GPU
// and C. Their length is fixed; each store is checked (and an f32 rounded), and a `ptr` parameter of an extern fn gets
// their bytes.
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

fn total(xs: f32s): num { var t = 0 for x in xs { t += x } return t }

test "a buffer type in annotations" {
  expect total(f32s([1, 2])) == 3
  expect err(fn() { total(u32s([1])) }).has("must be f32s, got")
  expect err(fn() { total([1]) }).has("must be f32s")
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

test "sent between isolates, and to JSON, as copies of their numbers" {
  var c = channel(1)
  c.send(u8s([1, 2]))
  expect c.recv() == u8s([1, 2])
  expect json(f32s([1.5, 2])) == "[1.5,2]"
}
