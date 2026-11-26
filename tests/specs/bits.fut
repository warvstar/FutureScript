// Whole numbers are exact up to 2^53; the bitwise operators work on them in 64 bits.
var h = 7
for c in "abc" { h = (h * 31 + 97) & 0xffff }

test "bitwise operators on whole numbers" {
  expect 5 & 3 == 1
  expect 5 | 8 ^ 1 == 13
  expect 1 << 40 == 1099511627776
  expect ~0 == -1
  expect -16 >> 2 == -4
}
test "int truncates toward zero" { expect int(-3.7) == -3 }
test "precedence is Python's: + binds tighter than &, & tighter than ==" {
  expect (2 + 3 & 6 == 4) == true
}
test "2^53 is exact" { expect 9007199254740992 == 9007199254740992 && str(9007199254740992) == "9007199254740992" }
test "a hash masked to 16 bits in a top-level loop" { expect h == 42714 }
test "a fraction is an error" {
  expect throws { print(1.5 & 1) } == "'&' needs whole numbers, got 1.5 and 1"
}
