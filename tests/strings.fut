// String and list helpers: slice, find, split, trim, join, parseNum.
print("  a,bb,,c  ".trim().split(","))
print("hello".slice(1, 3) + "|" + "hello".slice(3, 99) + "|" + "hello".slice(4, 2) + "|" + "hello".slice(-5, 1))
print(["hello".find("ll"), "hello".find("z"), "".find("")])
print([1, "x", nil, [2]].join("-"))
print([[1, 2, 3, 4].slice(1, 3), [1].slice(0, 0)])
print([parseNum(" 12.5 "), parseNum("1e3"), parseNum("x1"), parseNum(""), parseNum("-3"), parseNum("2x")])
print("a\nb".split("\n").len())
try { "a".split("") } catch e { print(e) }
try { "abc".slice(0.5, 1) } catch e { print(e) }
// expect: ["a", "bb", "", "c"]
// expect: el|lo||h
// expect: [2, -1, 0]
// expect: 1-x-nil-[2]
// expect: [[2, 3], []]
// expect: [12.5, 1000, nil, nil, -3, nil]
// expect: 2
// expect: split() needs a separator that isn't empty
// expect: slice(): an index must be a whole number, got 0.5
