// `expect` outside `fut test`: an assertion. `test` blocks are skipped by a normal run, even ones naming undefined things.
test "skipped by a normal run" { expect notDefinedAnywhere() == 1 }
expect 1 + 1 == 2
expect [1, {a: [2, 3]}] == [1, {a: [2, 3]}]   // lists and maps compare by contents in expect
expect {a: 1, b: 2} == {b: 2, a: 1}
expect "b" > "a" && 3 >= 3
expect throws { nil.x } == "nil has no field 'x'"
expect throws { throw {code: 7} } == {code: 7}
fn half(n) { expect n % 2 == 0 return n / 2 }
try { half(3) } catch e { print(e) } // expect: expect failed: n % 2 == 0 (n % 2 is 1)
try { expect [1, [2]] == [1, [3]] } catch e { print(e) } // expect: expect failed: [1, [2]] == [1, [3]] (first difference at [1][0]: 2 vs 3)
try { expect throws { 1 } } catch e { print(e) } // expect: expect failed: throws { ... } (nothing was thrown)
try { var who = nil expect who } catch e { print(e) } // expect: expect failed: who (who is nil)
var total = 0
for i in 0..3000 { total += half(i * 2) }  // hot: compiled again (tier 2) with the expect inside
print(total) // expect: 4498500
// `expect` and `test` are only special where they start a check or a test: they stay usable as names.
fn local() { var expect = 1 expect = 2 return expect }
print(local()) // expect: 2
var test = "t"
print(test) // expect: t
var a = []
a.push(a)
a.push({x: 0.1 + 0.2})
var b = []
b.push(b)
b.push({x: 0.3})
try { expect a == b } catch e { print(e) } // expect: expect failed: a == b (a is [<cycle>, {x: 0.30000000000000004}], b is [<cycle>, {x: 0.3}], first difference at [1].x: 0.30000000000000004 vs 0.3)
fn main() { print("main runs") } // expect: main runs
