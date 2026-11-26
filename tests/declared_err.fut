class Player {
  var hp = 100
  fn heal() { self.hpp += 10 }    // error: Player has no field 'hpp' (declare it in the class: var hpp)
}
