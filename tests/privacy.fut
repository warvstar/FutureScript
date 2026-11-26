// Top-level names are private to their file unless marked `pub`.
import "lib/limits.fut"
import "mods/shapes.fut" as shapes
fn clamp(x) { return "my own clamp {x}" }   // no clash with limits.fut's private clamp
var calls = "mine"                         // nor with its private calls
print(limit(42))                // expect: 10
print(limit(-3))                // expect: 0
print(limitCalls())             // expect: 2
print(clamp(1))                 // expect: my own clamp 1
print(calls)                    // expect: mine
print(shapes.Square(3).area())  // expect: 9
class Tri : shapes.Shape { override fn area() { return 0.5 } }
print(Tri().area())             // expect: 0.5
print(Tri().name())             // expect: shape
var pub = "pub is still a name"
print(pub)                      // expect: pub is still a name
