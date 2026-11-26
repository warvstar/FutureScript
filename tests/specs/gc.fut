// The collector under churn: everything kept must survive many collections intact.
class Node { fn init(l, r) { self.l = l self.r = r } }
fn make(d) { if d == 0 { return Node(nil, nil) } return Node(make(d - 1), make(d - 1)) }
fn check(n) { if n.l == nil { return 1 } return 1 + check(n.l) + check(n.r) }
fn trees() {
  var long = make(12)
  var t = 0
  for i in 0..2000 { t += check(make(8)) }
  return "{t} {check(long)}"
}

// A local that starts as a number lives in a d register; an object put there must still be seen.
fn dreg() { var s = 0 s = ["kept", 1] var n = 0 for i in 0..600000 { n = [i] } return "{s} {n}" }

// A class gaining a field after its constructor sites have cached it: later instances get the room inline.
class Grow { fn init(a) { self.a = a } }
fn grow() {
  var first = []
  for i in 0..3 { first.push(Grow(i)) }
  first[0].b = 10
  var later = []
  for i in 0..300000 { var g = Grow(i) g.b = i * 2 if i % 6000 == 0 { later.push(g) } }
  var s = first[0].b
  for g in later { s += g.a + g.b }
  return "{s} {first[2].a} {later.len()}"
}

class Empty {}
fn empties() { var n = 0 for i in 0..400000 { var e = Empty() if e != nil { n += 1 } } return n }

// Large objects (over 16KB) get pages of their own; keep one while churning many.
fn bigs() {
  var piece = "0123456789abcdef"
  var big = piece
  for i in 0..11 { big = big + big }
  var keep = big + "!"
  var n = 0
  for i in 0..1000 { var tmp = big + str(i) n += tmp.len() }
  return "{keep.len()} {n}"
}

// Closures with many captures, kept across collections.
fn closures() {
  var a = 1 var b = 2 var c = 3 var d = 4 var e = 5 var f = 6 var g = 7 var h = 8 var i = 9 var j = 10
  var fs = []
  for k in 0..100000 {
    var fk = fn() { return a + b + c + d + e + f + g + h + i + j + k }
    if k % 10000 == 0 { fs.push(fk) }
  }
  var s = 0
  for fk in fs { s += fk() }
  return s
}

// Objects reachable only from a suspended fiber's stack.
fn fibers() {
  var fb = fiber(fn() {
    var mine = Node(Node(nil, nil), nil)
    var tag = "fiber-owned"
    yield 1
    yield "{tag} {check(mine.l)}"
  })
  fb.resume()
  for i in 0..300000 { var junk = Node(nil, [i]) }
  return fb.resume()
}

// Maps and lists (they own malloc'd storage) die and are finalized; kept ones keep their contents.
fn owners() {
  var kept = {name: "k", items: [1, 2, 3]}
  for i in 0..300000 { var m = {i: i, l: [i, i]} }
  return "{kept.name} {kept.items}"
}

// Each function churns enough to collect many times on its own. Natively every test starts from the heap
// left by top-level code; under WebAssembly the tests share one heap, in order.
test "trees built and dropped while one is kept" { expect trees() == "1022000 8191" }
test "an object in a d register is seen by the collector" { expect dreg() == "[\"kept\", 1] [599999]" }
test "a class gaining a field after its constructor sites cached it" { expect grow() == "22050010 2 50" }
test "empty instances" { expect empties() == 400000 }
test "constructor arity errors" {
  expect throws { Empty(1) } == "Empty takes no arguments (no init)"
  expect throws { Node(1) } == "init expects 2 arguments, got 1"
}
test "a large object kept while many are churned" { expect bigs() == "32769 32770890" }
test "closures with many captures kept across collections" { expect closures() == 450550 }
test "objects reachable only from a suspended fiber" { expect fibers() == "fiber-owned 1" }
test "maps and lists are finalized; kept ones keep their contents" { expect owners() == "k [1, 2, 3]" }
