import "../lib/geom.fut"
import "../lib/util.fut"   // already loaded (geom.fut imports it): no-op

test "names from an import and from the file it imports" {
  expect Vec(1, 2).add(Vec(3, 4)).str() == "(4, 6)"
  expect origin.str() == "(0, 0)"
  expect twice(fn(x) { return x * 3 }, 2) == 18
}
