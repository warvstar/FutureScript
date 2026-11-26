// import "file" as name: the file's top-level names live in their own namespace, reached as name.x
import "../mods/geom.fut" as geom
import "../mods/other.fut" as o
fn square(x) { return "app square" }
class Ring : geom.Circle { override fn area() { return super.area() - 1 } }

test "module functions, and same names in the program and other modules" {
  expect geom.area(1) == 3.14159
  expect geom.square(3) == 9
  expect square(3) == "app square"
  expect o.square(3) == "other square"
}
test "module classes and vars; the module's code uses its own names" {
  var before = geom.calls
  var c = geom.Circle(2)
  expect c.area() == 12.56636
  expect geom.calls == before + 1   // Circle.area -> area -> square, all inside geom
}
test "a module var assigned from outside is seen by the module's code" {
  var pi = geom.PI
  geom.PI = 3
  expect geom.area(1) == 3
  expect Ring(1).area() == 2        // a class may extend a module's class
  geom.PI = pi
}
test "a module's main is not run" {
  // progs/modules_main.fut imports mods/geom.fut, whose main prints "geom main must not run"
  var r = run("progs/modules_main.fut")
  expect r.code == 0
  expect r.out == "9\n"
  expect r.err == ""
}
