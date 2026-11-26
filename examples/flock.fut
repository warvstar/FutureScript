// A simulation that runs the same everywhere: the same seed and inputs give the same boids, bit for bit, natively
// and in the browser, which is what lockstep multiplayer and replays need. Run:  build/fut examples/flock.fut
//   arrows move the target the seekers chase; space makes the "stuck" boids' behaviour loop forever (each one is
//   cut off by its step budget, the same tick everywhere, and the others carry on); escape quits.
// Record a run with --record run.log and play it back with --replay run.log: the same flight, frame for frame.
import "std/math"

var W = 320
var H = 240
var SPEED = 1.6
var SIGHT = 28          // how far a boid sees its neighbours
var BUDGET = 400        // steps (loop iterations and calls) a behaviour may take each tick

class Boid {
  fn init(x, y, a, kind) { self.x = x self.y = y self.a = a self.kind = kind }
}
class World {
  fn init(boids) { self.boids = boids self.tick = 0 self.faults = 0 self.tx = W / 2 self.ty = H / 2 }
}

// A world of n boids from seed s: the same s, the same world.
fn world(n, s) {
  seed(s)
  var boids = []
  for i in 0..n { boids.push(Boid(random() * W, random() * H, random() * 2 * math.pi, i % 4)) }
  return World(boids)
}

// The behaviours: each turns its boid (radians this tick) from what it sees. They are ordinary functions, as a
// player's or a mod's scripts would be, and each runs under a budget.
fn flock(b, w, input) {  // head the way the neighbours head, and toward where they are
  var sx = 0
  var sy = 0
  var cx = 0
  var cy = 0
  var n = 0
  for o in w.boids {
    var dx = o.x - b.x
    var dy = o.y - b.y
    if o != b && dx * dx + dy * dy < SIGHT * SIGHT {
      sx += math.cos(o.a)  sy += math.sin(o.a)  cx += dx  cy += dy  n += 1
    }
  }
  if n == 0 { return 0 }
  return turnToward(b.a, math.atan2(sy + cy * 0.02, sx + cx * 0.02)) * 0.2
}
fn seek(b, w, input) { return turnToward(b.a, math.atan2(w.ty - b.y, w.tx - b.x)) * 0.1 }
fn wander(b, w, input) { return (random() - 0.5) * 0.4 }
fn stuck(b, w, input) {  // a script with a bug: with chaos on, it never returns
  var t = 0
  while input.chaos { t += 1 }
  return flock(b, w, input)
}
var BEHAVIOURS = [flock, seek, wander, stuck]

fn turnToward(a, target) {  // the turn from heading a to target, in (-pi, pi]
  var d = (target - a) % (2 * math.pi)
  if d > math.pi { d -= 2 * math.pi }
  if d <= -math.pi { d += 2 * math.pi }
  return d
}

// One tick. What comes from outside is input ({left, right, up, down, chaos}): no clock, no keys read here.
fn step(w, input) {
  if input.left { w.tx -= 3 }
  if input.right { w.tx += 3 }
  if input.up { w.ty -= 3 }
  if input.down { w.ty += 3 }
  var turns = []
  for b in w.boids {
    var behave = BEHAVIOURS[b.kind]
    var turn = 0
    try { turn = budget(BUDGET, fn() { return behave(b, w, input) }) } catch e { w.faults += 1 }
    turns.push(math.clamp(turn, -0.3, 0.3))
  }
  for i in 0..w.boids.len() {  // move after all have decided: the order boids are listed in doesn't matter
    var b = w.boids[i]
    b.a += turns[i]
    b.x = (b.x + math.cos(b.a) * SPEED + W) % W
    b.y = (b.y + math.sin(b.a) * SPEED + H) % H
  }
  w.tick += 1
}

// Every bit of every boid's state, folded into one number: equal fingerprints, equal worlds.
var P = 1000000007
fn mix(h, x) {
  var a = x
  var e = 0
  if a != 0 { while math.abs(a) >= 2 { a = a / 2 e += 1 } while math.abs(a) < 1 { a = a * 2 e -= 1 } }
  return (h * 31 + (a * 4503599627370496) % P + e + 3000) % P
}
fn fingerprint(w) {
  var h = w.tick
  for b in w.boids { h = mix(mix(mix(h, b.x), b.y), b.a) }
  return h
}

var COLORS = [0x60c0ff, 0xffb000, 0x80e080, 0xff6060]
fn draw(w) {
  clear(0x10121c)
  rect(w.tx - 2, w.ty - 2, 5, 5, 0xffffff)
  for b in w.boids {
    circle(b.x, b.y, 2, COLORS[b.kind])
    rect(b.x + math.cos(b.a) * 4, b.y + math.sin(b.a) * 4, 1, 1, COLORS[b.kind])
  }
}

fn main() {
  var w = world(60, 7)
  window(W, H)
  while present() && !key("escape") {
    step(w, {left: key("left"), right: key("right"), up: key("up"), down: key("down"), chaos: key("space")})
    draw(w)
    if w.tick % 120 == 0 { print("tick {w.tick}: fingerprint {fingerprint(w)}, {w.faults} behaviours cut off") }
  }
}

// fut test examples/flock.fut (both suites run it): headless, the same seed and inputs give the same world, every
// bit, natively and in wasm, and a behaviour stuck in a loop is cut off the same tick everywhere.

fn fly(ticks) {
  var w = world(60, 42)
  for t in 0..ticks {
    step(w, {left: t >= 50 && t < 80, right: false, up: t >= 60 && t < 70, down: false, chaos: t >= 100 && t < 110})
  }
  return w
}

test "a run is the same bits every time and on every platform" {
  var w = fly(300)
  expect [fingerprint(w), w.faults] == [855363764, 150]
  expect fingerprint(fly(300)) == fingerprint(w)
}

test "a stuck behaviour is cut off, and the rest of the flock carries on" {
  var w = fly(110)
  expect w.faults == 150  // 15 stuck boids, 10 ticks
  expect w.boids[0].x != w.boids[0].y
}
