// String interpolation: expressions, nested braces, escaping, and lists/calls inside.
var x = 3
var name = "fut"

test "interpolated expressions" {
  expect "{name} has {x + 1} things" == "fut has 4 things"
  expect "{x}" == "3"
  expect "list {[1, 2]} map {len({a: 1})}" == "list [1, 2] map 1"
}
test "a map literal inside an interpolation" {
  expect "nested {{1: 2}}" == "nested \{1: 2}"
}
test "an escaped brace is not interpolated" {
  expect "escaped \{x}".len() == 11   // "escaped {x}", not "escaped 3"
  expect prints { print("escaped \{x}") } == "escaped \{x}\n"
}
test "strings inside an interpolation" {
  var xs = ["a", "b"]
  expect "{xs.join(", ")}!" == "a, b!"
  expect "{"in {"deep"}"} end" == "in deep end"
  expect "{x > 2 ? "big" : "small"} {"\{"}" == "big \{"
  expect "{"\"quoted\""}".len() == 8
}
test "escapes: \\n \\t \\r \\0 \\\" \\\\ \\{ \\} and \\u{hex}" {
  expect "\r".byte(0) == 13 && "\n".byte(0) == 10 && "\t".byte(0) == 9 && "\0".byte(0) == 0
  expect "\"\\\{\}" == char(34) + char(92) + char(123) + char(125)
  expect "\u{41}\u{e9}\u{20AC}\u{1F600}" == "Aé€😀"
  expect "\u{1F600}".len() == 4 && "\u{0}" == "\0"
  expect "{x}\u{2D}{x}" == "3-3"
}
test "an unknown escape is a compile error, not the bare letter" {
  for e in ["\\q", "\\x41", "\\u41", "\\u\{}", "\\u\{D800}", "\\u\{110000}", "\\u\{1234567}"] {
    var r = run({source: "print(\"a" + e + "b\")"})
    expect r.code != 0 && r.err.has(" [syntax]\n")
  }
  expect run({source: "print(\"\\q\")"}).err.has("unknown escape '\\q'")
  expect run({source: "class A \{ var s = \"\\q\" }"}).err.has("unknown escape '\\q'")
}
test "byte and char" {
  expect "abc".byte(0) == 97 && "é".byte(0) == 0xC3 && "é".byte(1) == 0xA9
  expect char(97) == "a" && char(0xE9) == "é" && char(0x1F600) == "😀" && char(0) == "\0"
  expect "{["a\rb\u{1b}"]}" == "[\"a\\rb\\u\{1b}\"]"
}
