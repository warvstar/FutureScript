// A local proven to be a number by an earlier operation skips its type checks; these must stay correct.
fn join(x, c) { if c { var y = x - 1 } return x + "s" }
fn reassigned(x) { var y = x - 1 x = "a" return x + y }
fn andRight(x, c) { if c && x < 1 { print("in") } return x + "!" }
fn loopy() { var x = 1 var out = "" var i = 0 while i < 3 { out = out + (x * 1) x = "s" i += 1 if i < 3 { x = 2 } } return out }
fn elseDead(x, c) { if c { return "early" } else { var y = x * 2 } return x + 1 }
fn strCompare(a, b) { if a < b { return a + b } return b + a }
fn eqProvesNothing(x) { if x == 1 { return 0 } return x + "!" }
fn fibp(n) { if n < 2 { return n } return fibp(n - 1) + fibp(n - 2) }
fn badSub(x) { return x - 1 }

test "a fact from a branch not taken does not hold after it" {
  expect join("a", false) == "as"
  expect elseDead(3, false) == 4
  expect elseDead("q", true) == "early"
}
test "reassignment ends a fact" {
  expect reassigned(5) == "a4"
  expect loopy() == "122"
}
test "the right side of && may not run" {
  expect prints { expect andRight("z", false) == "z!" } == ""
}
test "comparison and equality prove nothing about strings" {
  expect strCompare("x", "y") == "xy"
  expect eqProvesNothing("w") == "w!"
}
test "proven numbers in recursion" { expect fibp(20) == 6765 }
test "an operation that would prove a number still checks its operands" {
  expect throws { badSub("s") } == "bad operands for '-': string and number"
}
