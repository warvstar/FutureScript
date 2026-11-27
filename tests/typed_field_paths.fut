class Game { var pointer: Pointer }
class Pointer { var hover }
fn check(g: Game) {
  g.pointer.hovre   // error: 'hovre' is not a field of Pointer (pointer is Pointer:
  var p = g.pointer
  p.hovre           // error: 'hovre' is not a field of Pointer (p is Pointer:
}
