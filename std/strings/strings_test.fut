import "./strings.fut"

test "replace, count" {
  expect replace("a-b--c", "-", "+") == "a+b++c"
  expect count("banana", "an") == 2
  expect throws { replace("x", "", "y") }
}
test "starts, ends" {
  expect startsWith("hello", "he") && !startsWith("he", "hello")
  expect endsWith("hello", "lo") && !endsWith("lo", "hello") && endsWith("x", "")
}
test "repeat, pad" {
  expect repeat("ab", 3) == "ababab" && repeat("x", 0) == ""
  expect padStart("7", 3, "0") == "007" && padEnd("ab", 4, ".") == "ab.." && padStart("long", 2, " ") == "long"
}
test "case, reverse" {
  expect upper("abC1-z") == "ABC1-Z" && lower("XyZ!") == "xyz!"
  expect reverse("abc") == "cba"
}
test "lines, words" {
  expect lines("x\r\ny\n\nz") == ["x", "y", "", "z"]
  expect lines("") == []
  expect words("  a b\tc \n") == ["a", "b", "c"]
  expect lines("bar\nfur\r\nok\r") == ["bar", "fur", "ok"]   // only a carriage return goes, not a final r
  expect words("error free\r\n") == ["error", "free"]
}
test "isInt, parseInt" {
  for s in ["0", "7", "42", "-3", "007", "-0", "1234567890"] { expect isInt(s) }
  for s in ["", "-", "+1", " 1", "1 ", "1.5", "1e3", "--1", "1-", "0x10", "é", "12a", "a12", "1\n"] { expect !isInt(s) }
  expect parseInt("42") == 42 && parseInt("-17") == -17 && parseInt("007") == 7
  expect parseInt("") == nil && parseInt("4.2") == nil && parseInt(" 4") == nil && parseInt("+4") == nil
}
