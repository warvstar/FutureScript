var m = {}
for w in ["a", "b", "a", "c", "a"] { if m.has(w) { m[w] += 1 } else { m[w] = 1 } }
print(m)                    // expect: {"a": 3, "b": 1, "c": 1}
print(m.keys())             // expect: ["a", "b", "c"]
print(m.remove("b"))        // expect: 1
print(len(m) + m.len())     // expect: 4
var total = 0
for k in m { total += m[k] }
print(total)                // expect: 4
print({1: {2: [3]}}[1][2][0])  // expect: 3
var ops = {inc: fn(x) { return x + 1 }, twice: fn(x) { return x * 2 }}
print(ops.twice(ops.inc(4)))   // expect: 10
