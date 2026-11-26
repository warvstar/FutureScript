// Whole numbers are exact up to 2^53; the bitwise operators work on them in 64 bits.
print(5 & 3)                       // expect: 1
print(5 | 8 ^ 1)                   // expect: 13
print(1 << 40)                     // expect: 1099511627776
print(~0)                          // expect: -1
print(-16 >> 2)                    // expect: -4
print(int(-3.7))                   // expect: -3
print(2 + 3 & 6 == 4)              // expect: true
print(9007199254740992)            // expect: 9007199254740992
var h = 7
for c in "abc" { h = (h * 31 + 97) & 0xffff }
print(h)                           // expect: 42714
print(1.5 & 1)                     // error: '&' needs whole numbers, got 1.5 and 1
