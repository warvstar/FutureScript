// Run with:  build/fut -w examples/paddle.fut
// Live-code a game: while it runs, edit colors, sizes, physics or the rules and save.
// Edit a `var` line (say, gravity) to reset just that value; everything else keeps its state.
// Type into the terminal to poke the running game: `balls.len()`, `score = 100`, `gravity = -0.1`.
// Arrow keys move the paddle, click to drop a ball, escape quits.

var W = 320
var H = 240
var gravity = 0.15
var score = 0
var px = 140
var balls = []

class Ball {
  fn init(x, y) { self.x = x self.y = y self.vx = 2 self.vy = 0 }
  fn step() {
    self.vy += gravity
    self.x += self.vx  self.y += self.vy
    if self.x < 4 || self.x > W - 4 { self.vx = -self.vx }
    if self.y > H - 14 && self.y < H - 6 && self.x > px && self.x < px + 50 && self.vy > 0 {
      self.vy = -6  score += 1
    }
  }
}

fn draw() {
  clear(0x1b1b2f)
  rect(px, H - 10, 50, 6, 0xe0e0e0)
  for b in balls { circle(b.x, b.y, 4, 0xffb000) }
  for i in 0..score { rect(4 + i * 6, 4, 4, 4, 0x40c070) }   // score as pips
}

fn main() {
  window(W, H)
  balls.push(Ball(160, 40))
  while present() && !key("escape") {
    if key("left") { px -= 4 }
    if key("right") { px += 4 }
    if mousedown() && balls.len() < 20 { balls.push(Ball(mousex(), mousey())) }
    for b in balls { b.step() }
    var kept = []
    for b in balls { if b.y < H + 10 { kept.push(b) } }
    balls = kept
    draw()
  }
}
