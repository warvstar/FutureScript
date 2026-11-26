// A class that declares its fields has exactly those fields; one never assigned reads as its default.
class Player {
  var name
  var hp = 100
  var tag = "new"
  fn init(name) { self.name = name }
  fn hit(n) { self.hp -= n return self }
}
var p = Player("ada")
print(p.hp)                       // expect: 100
print(p.hit(30).hp)               // expect: 70
print(p.tag)                      // expect: new
try { p.hpp = 1 } catch e { print(e) }   // expect: Player has no field 'hpp' (its fields are declared: add 'var hpp' to the class)
try { print(p.mana) } catch e { print(e) } // expect: Player has no field 'mana'
class Mage : Player {             // inherits the declared fields and adds one
  var mana = 50
  fn init(name) { super.init(name) self.mana += 5 }
}
var m = Mage("zed")
print("{m.name} {m.hp} {m.mana}") // expect: zed 100 55
class Loose { fn init() { self.anything = 1 } }   // declares nothing: fields come from assignment, as before
var l = Loose()
l.more = 2
print(l.anything + l.more)        // expect: 3
