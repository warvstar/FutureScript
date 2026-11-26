// import "file" as name: the file's top-level names live in their own namespace, reached as name.x
import "mods/geom.fut" as geom
import "mods/other.fut" as o
fn square(x) { return "app square" }
print(geom.area(1))                // expect: 3.14159
print(geom.square(3))              // expect: 9
print(square(3))                   // expect: app square
print(o.square(3))                 // expect: other square
var c = geom.Circle(2)
print(c.area())                    // expect: 12.56636
print(geom.calls)                  // expect: 3
geom.PI = 3
print(geom.area(1))                // expect: 3
class Ring : geom.Circle { fn area() { return super.area() - 1 } }
print(Ring(1).area())              // expect: 2
