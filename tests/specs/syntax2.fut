fn syntaxErr(src) { return run({source: src}).err }
fn explode() { throw "unexpected evaluation" }
fn accepted(x) requires x && false ? false : true { return x }

test "ternary is lazy, right-associative and below logical operators" {
  var choice = true ? 3 : explode()
  expect choice == 3
  expect (false ? explode() : 4) == 4
  expect (true || false ? 1 : 2) == 1
  expect (false ? 1 : true ? 2 : 3) == 2
  expect (true ? false ? 1 : 2 : 3) == 2
  expect (0 ? 1 : 2) == 1
  expect "{true ? 2 : 3}" == "2"
  var m = {(true ? "x" : "y"): 2}
  expect m.x == 2
  if false ? false : true { choice = 5 }
  expect choice == 5
  expect false ? false : true
  expect false == nil ? false : true
  var number: num = (choice == 5 ? 42 : 99) + 1
  expect number == 43
  expect accepted(false) == false
}

test "power binds before unary minus and associates to the right" {
  expect 2 ** 3 ** 2 == 512
  expect -2 ** 2 == -4
  expect (-2) ** 2 == 4
  expect 2 ** -2 == 0.25
  expect 2 ** -2 ** 2 == 0.0625
  expect 3 * 2 ** 2 + 1 == 13
  expect 1 + 3 ** 2 == 10
  expect [2][0] ** 3 == 8
  expect 9 ** 0.5 == 3
  expect syntaxErr("var p = 2 ** \"x\"").has("'**' expects numbers, got number and string [operand]")
}

test "optional chains skip the entire postfix chain on nil" {
  var a = nil
  expect a?.b.c == nil
  expect a?.b[explode()] == nil
  expect a?.m(explode()).b == nil
  a = {b: {c: 2}, m: fn(x) { return {b: x} }}
  expect a?.b.c == 2
  expect a?.m(7).b == 7
  expect a?.missing?.c == nil
  expect throws { a?.missing.c }
  expect throws { false?.x }
  var typed: {name: str}? = nil
  expect typed?.name == nil
  typed = {name: "fut"}
  expect typed?.name == "fut"
  expect syntaxErr("var x: \{name: str}? = nil\nx?.nmae").has("'nmae' is not a field")
  expect syntaxErr("class C \{ var name = \"fut\" }\nvar c: C? = nil\nc?.nmae").has("'nmae' is not a field")
}

test "slices on strings and lists use clamped exclusive ends" {
  var l = [0, 1, 2, 3]
  expect l[1:3] == [1, 2]
  expect l[2:] == [2, 3]
  expect l[:2] == [0, 1]
  expect l[:] == l
  expect l[-3:99] == l
  expect l[3:1] == []
  expect l[true ? 1 : 2 : 3] == [1, 2]
  expect "abcd"[1:3] == "bc"
  expect "abcd"[2:] == "cd"
  expect "abcd"[:2] == "ab"
  expect "abcd"[:] == "abcd"
  expect "abcd"[-3:99] == "abcd"
  expect "abcd"[99:] == ""
  expect syntaxErr("var l = [1]\nl[:] = [2]").has("cannot assign to this [misplaced]")
  expect syntaxErr("var m = \{a: 1}\nm[:]").has("cannot slice map; use a string or list [index]")
}

fn makeRecursive(seed) {
  fn factorial(n) { return n <= 1 ? seed : n * factorial(n - 1) }
  seed = 20
  return factorial
}

test "nested named functions recurse through the returned closure" {
  var f = makeRecursive(1)
  expect f(5) == 120
  expect makeRecursive(2)(5) == 240
  fn indirect(n) {
    var next = fn(i) { return indirect(i) }
    return n == 0 ? 0 : 1 + next(n - 1)
  }
  var saved = indirect
  indirect = nil
  expect saved(5) == 5
  expect syntaxErr("fn f() \{ class C \{ } }").has("'class' is only allowed at top level [misplaced]")
}

test "comparison and default-argument errors give rewrites" {
  expect syntaxErr("var x = 1 < 2 < 3").has("chained comparison: write a < b && b < c [syntax]")
  expect syntaxErr("var x = 1 == 2 < 3").has("chained comparison")
  expect syntaxErr("var x = 1 >= 2 != 3").has("chained comparison")
  expect syntaxErr("if 1 <= 2 > 3 \{ }").has("chained comparison")
  expect syntaxErr("expect 1 > 2 == 3").has("chained comparison")
  expect syntaxErr("fn f(a, b = 1) \{ }").has("fut has no default arguments; pass a map of options")
  expect syntaxErr("fn f() \{ fn g(a: num = 1) \{ } }").has("fut has no default arguments")
  expect (1 < 2) == true
  expect (1 == 2) != true
}

test "runtime errors name the corresponding builtin or library function" {
  expect syntaxErr("int(\"3\")").has("use parseNum(s) to parse a string [argument]")
  expect syntaxErr("[2, 1].sort()").has("import \"std/lists\" and use lists.sort(l) [no-method]")
  for method in ["map", "filter", "reduce", "reverse"] {
    expect syntaxErr("[].{method}()").has("import \"std/lists\" and use lists.{method}")
  }
  expect syntaxErr("\"fut\".upper()").has("import \"std/strings\" and use strings.upper")
  expect syntaxErr("var l = [1]\nl[-1]").has("use l[l.len() - 1]")
}

test "syntax errors suggest fut spellings and preserve ordinary identifiers" {
  expect syntaxErr("var x = 1\nx?").has("condition ? yes : no")
  expect syntaxErr("1 === 1").has("use '=='")
  expect syntaxErr("1 !== 1").has("use '!='")
  expect syntaxErr("true and false").has("use '&&'")
  expect syntaxErr("true or false").has("use '||'")
  expect syntaxErr("if true and false \{ }").has("use '&&'")
  expect syntaxErr("not false").has("use '!'")
  expect syntaxErr("if true \{ } elif false \{ }").has("use 'else if'")
  for word in ["null", "None", "undefined"] { expect syntaxErr(word).has("use 'nil'") }
  for word in ["let", "const"] { expect syntaxErr("{word} x = 1").has("declare a variable with 'var'") }
  for word in ["function", "def"] { expect syntaxErr("{word} f() \{ }").has("declare a function with 'fn'") }
  expect syntaxErr("var f = x => x").has("write a lambda as fn(x)")
  expect syntaxErr("var i = 1\ni++").has("use '+= 1'")
  var null = 1
  var and = 2
  fn function(const) { return const }
  expect function(null + and) == 3
}
test "keywords are map keys and field names: {class: x}, m.class" {
  var m = {class: "note", fn: 1, if: 2, nil: 3, true: 4}
  expect m.class == "note" && m["fn"] == 1 && m.if == 2 && m[nil] == 3 && m[true] == 4
  m.class = "warn"
  expect m["class"] == "warn"
}
test "a name from a module imported with a name says where it is; two imports can't share a name" {
  expect run({source: "import \"std/lists\"\nprint(sort([2, 1]))"}).err.has("undefined name 'sort': it is in lists, so write lists.sort [undefined-name]")
  expect run({source: "import \"std/strings\" as s\nprint(lines(\"a\"))"}).err.has("it is in s, so write s.lines")
  expect run({source: "import \"std/lists\"\nimport \"std/strings\" as lists\nprint(1)"}).err.has("'lists' already names another import in this file: give one of them a different name with 'as' [import]")
  expect run({source: "import \"std/lists\"\nimport \"std/lists\"\nprint(lists.sort([2, 1]))"}).out == "[1, 2]\n"
}
