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
}
