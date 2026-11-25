import "lib/geom.fut"
import "lib/util.fut"   // already loaded: no-op
print(Vec(1, 2).add(Vec(3, 4)).str())   // expect: (4, 6)
print(origin.str())                     // expect: (0, 0)
print(twice(fn(x) { return x * 3 }, 2)) // expect: 18
