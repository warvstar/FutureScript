// A local proven to be a number by an earlier operation skips its type checks; these must stay correct.
fn join(x, c) { if c { var y = x - 1 } return x + "s" }
print(join("a", false)) // expect: as
fn reassigned(x) { var y = x - 1 x = "a" return x + y }
print(reassigned(5)) // expect: a4
fn andRight(x, c) { if c && x < 1 { print("in") } return x + "!" }
print(andRight("z", false)) // expect: z!
fn loopy() { var x = 1 var out = "" var i = 0 while i < 3 { out = out + (x * 1) x = "s" i += 1 if i < 3 { x = 2 } } return out }
print(loopy()) // expect: 122
fn elseDead(x, c) { if c { return "early" } else { var y = x * 2 } return x + 1 }
print(elseDead(3, false)) // expect: 4
print(elseDead("q", true)) // expect: early
fn strCompare(a, b) { if a < b { return a + b } return b + a }
print(strCompare("x", "y")) // expect: xy
fn eqProvesNothing(x) { if x == 1 { return 0 } return x + "!" }
print(eqProvesNothing("w")) // expect: w!
fn fibp(n) { if n < 2 { return n } return fibp(n - 1) + fibp(n - 2) }
print(fibp(20)) // expect: 6765
fn badSub(x) { return x - 1 }
print(badSub("s"))
// error: bad operands for '-'
