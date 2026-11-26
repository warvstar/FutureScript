// Map index under churn: growth, removal (deleted slots reused), number keys, -0 == 0, m.name keys.
var m = {}
for i in 0..5000 { m[i] = i * 2 }
for i in 0..5000 { if i % 3 != 0 { m.remove(i) } }
print(len(m))                        // expect: 1667
var ok = true
for i in 0..5000 { if m.has(i) != (i % 3 == 0) { ok = false } }
print(ok)                            // expect: true
for r in 0..20 { for i in 0..100 { m["k{i}"] = r } for i in 0..100 { m.remove("k{i}") } }
print(len(m))                        // expect: 1667
print("{m[2997]} {m[2998]}")          // expect: 5994 nil
var z = {}
z[0] = "zero"
print("{z[-0]} {z.has(0 * -1)}")      // expect: zero true
z.name = "fut"
var nm = z["name"]
print("{nm} {z.name} {len(z)}")         // expect: fut fut 2
var s = ""
for c in "héllo" { s = s + c }
print("{s} {len(s)} " + ("ab" + 1 + nil)) // expect: héllo 6 ab1nil
print(0xff + 0X10 + 0xA)                // expect: 281
