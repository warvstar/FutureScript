var a = [1, 2, 3]
a.push(4)
print(a)                // expect: [1, 2, 3, 4]
print(len(a))           // expect: 4
a[0] = 10
a[1] += 5
print(a[0] + a[1])      // expect: 17
print(a.pop())          // expect: 4
var t = 0
for v in a { t += v }
print(t)                // expect: 20
print([1] + [2, "x"])   // expect: [1, 2, "x"]
var s = ""
for ch in "hey" { s = ch + s }
print(s)                // expect: yeh
var grid = [[1, 2], [3, 4]]
print(grid[1][0])       // expect: 3
var big = []
for i in 0..100000 { big.push(str(i)) }  // churn the GC
print(len(big))         // expect: 100000
print(big[99999])       // expect: 99999
