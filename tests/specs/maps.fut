// Maps: name/string/number keys, missing keys, compound assignment, printing in insertion order.
var m = {name: "fut", "ver": 2, 3: "three"}

test "literal keys, missing keys, update and printing" {
  expect m["name"] + " " + str(m["ver"]) == "fut 2"
  expect m[3] == "three"
  expect m["missing"] == nil
  m["ver"] += 1
  m.extra = 1 == 1
  expect m == {name: "fut", ver: 3, 3: "three", extra: true}
  expect prints { print(m) } == "\{\"name\": \"fut\", \"ver\": 3, 3: \"three\", \"extra\": true}\n"
  expect m.name + m.missing == "futnil"
}
