test "format precision and scientific notation" {
  var s = "café"
  expect "{3.14159:.2}|{7:.2f}|{2.75:.0}" == "3.14|7.00|3"
  expect "{125:.2e}|{125:e}|{7:f}" == "1.25e+02|1.250000e+02|7.000000"
  expect "{s:.3}|{s:.5}|{s:.0}" == "caf|café|"
  expect len("{s:.4}") == 4
}

test "format width, alignment and fill" {
  var s = "hi"
  expect "{s:5}|{s:<5}|{7:>3}|{s:^7}" == "hi   |hi   |  7|  hi   "
  expect "{7:4}|{true:6}|{s:1}|{s:}" == "   7|true  |hi|hi"
  expect "{7:0>4}|{-7:0>4}|{-7:04}|{7:+04}" == "0007|00-7|-007|+007"
  expect "{s:*^6}|{s::>5}|{s:>5.1}" == "**hi**|:::hi|    h"
}

test "format whole numbers, grouping and signs" {
  expect "{255:x}|{255:X}|{10:b}|{15:o}|{-255:x}" == "ff|FF|1010|17|-ff"
  expect "{0:b}|{18446744073709551616:x}" == "0|10000000000000000"
  expect "{1234567:,}|{-1234.25:,}|{1234.5:+,.2}" == "1,234,567|-1,234.25|+1,234.50"
  expect "{0:+}|{4:+}|{-4:+}" == "+0|+4|-4"
}

fn formatIdentity(x) { return x }

test "format follows the entire expression" {
  expect "{formatIdentity({n: 4}).n + [1, 2][0]:04}" == "0005"
  expect "{formatIdentity(7) == 7:6}" == "true  "
}

test "format debug interpolation keeps the expression source" {
  var x = 1.25
  var calls = 0
  var f = fn() { calls += 1; return calls }
  expect "{x=}|{x=:.2}|{x  +  1=:.1}" == "x = 1.25|x = 1.25|x  +  1 = 2.2"
  expect "{ x = :>5}" == " x  =  1.25"
  expect "{f()=}|{f()=}" == "f() = 1|f() = 2"
}

test "invalid format specs are compile errors" {
  var bad = run({source: "fn unused() \{ return \"\{1:wat}\" }"}).err
  expect bad.has("invalid format spec 'wat'")
  expect bad.has("precision")
  expect bad.has("[syntax]")
  for spec in [".", "65536", ".1001", ".2x", ",b", "\{<8"] {
    expect run({source: "var x = \"\{1:{spec}}\""}).err.has("invalid format spec '" + spec + "'")
  }
}

test "incompatible format values are runtime errors" {
  var fraction = run({source: "var x = \"\{1.5:x}\""}).err
  expect fraction.has("runtime error: format 'x' needs a finite whole number [argument]")
  expect run({source: "var x = \"\{true:+}\""}).err.has("format needs a number, got bool [argument]")
  expect run({source: "var x = \"\{[1]:.2}\""}).err.has("format precision needs a number or string, got list [argument]")
}
