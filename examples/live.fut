// Run with:  build/fut -w examples/live.fut
// Then edit this file while it runs: change Ball.step, draw, the glyph, the width...
// Each save is compiled to machine code and patched into the running process; only changed
// functions are recompiled. `ball` keeps its state. Editing main() restarts it.

class Ball {
  fn init(x) { self.x = x self.v = 1 }
  fn step(width) {
    self.x += self.v
    if self.x <= 0 || self.x >= width - 1 { self.v = -self.v }
  }
}

fn draw(ball, width) {
  var line = ""
  for i in 0..width { if i == floor(ball.x) { line += "o" } else { line += "." } }
  print("{line}  x={ball.x}")
}

var ball = Ball(0)   // top-level state survives reloads
var width = 30

fn reload() { print("-- reloaded --") }   // optional hook, runs after every patch

fn main() {
  while true {
    ball.step(width)
    draw(ball, width)
    sleep(0.1)
  }
}
