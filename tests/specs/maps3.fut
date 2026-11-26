// Map index under churn: growth, removal (deleted slots reused), number keys, -0 == 0, m.name keys.
var m = {}
var ok = true
var z = {}
var s = ""

test "number keys through growth, removal and string-key churn" {
  for i in 0..5000 { m[i] = i * 2 }
  for i in 0..5000 { if i % 3 != 0 { m.remove(i) } }
  expect len(m) == 1667
  for i in 0..5000 { if m.has(i) != (i % 3 == 0) { ok = false } }
  expect ok == true
  for r in 0..20 { for i in 0..100 { m["k{i}"] = r } for i in 0..100 { m.remove("k{i}") } }
  expect len(m) == 1667
  expect m[2997] == 5994
  expect m[2998] == nil
}
test "-0 is the same key as 0, and m.name is m[\"name\"]" {
  z[0] = "zero"
  expect z[-0] == "zero"
  expect z.has(0 * -1)
  z.name = "fut"
  var nm = z["name"]
  expect nm == "fut" && z.name == "fut" && len(z) == 2
}
test "string iteration over multibyte text, and + with numbers and nil" {
  for c in "héllo" { s = s + c }
  expect s == "héllo"
  expect len(s) == 6
  expect "ab" + 1 + nil == "ab1nil"
}
test "hex literals" {
  expect 0xff + 0X10 + 0xA == 281
}
