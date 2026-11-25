var m = {name: "fut", "ver": 2, 3: "three"}
print(m["name"] + " " + str(m["ver"]))  // expect: fut 2
print(m[3])                             // expect: three
print(m["missing"])                     // expect: nil
m["ver"] += 1
m.extra = 1 == 1
print(m)                                // expect: {"name": "fut", "ver": 3, 3: "three", "extra": true}
print(m.name + m.missing)               // expect: futnil
