class P { fn init() { self.a = 1 } }
var p = P()
var q = P()
q.b = 2               // grows the class layout; p has no slot for b
print(p.b)
// error: instance has no field 'b'
