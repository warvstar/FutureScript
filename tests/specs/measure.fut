// expect allocs { } counts the objects a block allocates (exact, the same natively and in wasm); expect grows
// { n -> ... } like "n" runs code for sizes that double and checks its time (or allocations) grows no faster.

fn squares(n) { var l = [] for i in 0..n { l.push(i * i) } return l }
fn labels(n) { var l = [] for i in 0..n { l.push("item {i}") } return l }
class Point {
  var x = 0
  var y = 0
  fn init(x, y) { self.x = x  self.y = y }
}
fn first(l) { return l[0] }
fn pairs(n) { var s = 0 for i in 0..n { for j in 0..n { s += i ^ j } } return s }

test "allocs counts objects: lists, strings, instances, closures" {
  expect allocs { var x = 1 + 2 } == 0          // numbers are not objects
  expect allocs { var l = [1, 2, 3] } == 1
  expect allocs { squares(100) } == 1           // one list, however long
  expect allocs { labels(100) } == 101          // and each interpolated string
  expect allocs { for i in 0..10 { Point(i, i) } } == 10
  expect allocs { var f = fn() { return 1 } } == 1
  expect allocs { labels(10) } <= 11
}

test "grows: time" {
  expect grows { n -> squares(n) } like "n"
  expect grows { n -> pairs(n) } like "n^2"
  expect grows { n -> n * 2 } like "1"
  expect grows { n -> first([n]) } like "1"   // millions of calls from the runtime into the block: none may leak stack
}

test "grows allocs: exact counts, so a tight tolerance" {
  expect grows allocs { n -> labels(n) } like "n"
  expect grows allocs { n -> squares(n) } like "1"
}

test "failures say how it grows, with the measurements" {
  var r = run({file: "progs/measure_fail.fut", args: ["test"]})
  expect r.code == 1
  expect r.out.has("allocs \{ ... } is 101\n")
  // under build.py tsan (FUT_UNTIMED) time isn't measured, and the summary says so instead
  expect r.out.has("expect grows \{ ... } like \"n\"\n    it grows like n^2, not n (n = ") || r.out.has("FUT_UNTIMED: expect grows by time not measured")
  expect r.out.has("its allocations grow like n, not 1 (n = 64: 65, 128: 129, 256: 257, 512: 513, 1024: 1025, 2048: 2049)")
}
