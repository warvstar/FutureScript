var PI = 3.141592653589793
var SOLAR_MASS = 4 * PI * PI
var DAYS = 365.24
class Body {
  fn init(x, y, z, vx, vy, vz, mass) {
    self.x = x self.y = y self.z = z
    self.vx = vx * DAYS self.vy = vy * DAYS self.vz = vz * DAYS
    self.mass = mass * SOLAR_MASS
  }
}
fn system() {
  var b = [Body(0, 0, 0, 0, 0, 0, 1),
    Body(4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01, 1.66007664274403694e-03, 7.69901118419740425e-03, -6.90460016972063023e-05, 9.54791938424326609e-04),
    Body(8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01, -2.76742510726862411e-03, 4.99852801234917238e-03, 2.30417297573763929e-05, 2.85885980666130812e-04),
    Body(1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01, 2.96460137564761618e-03, 2.37847173959480950e-03, -2.96589568540237556e-05, 4.36624404335156298e-05),
    Body(1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01, 2.68067772490389322e-03, 1.62824170038242295e-03, -9.51592254519715870e-05, 5.15138902046611451e-05)]
  var px = 0.0
  var py = 0.0
  var pz = 0.0
  for o in b { px += o.vx * o.mass py += o.vy * o.mass pz += o.vz * o.mass }
  b[0].vx = 0 - px / SOLAR_MASS
  b[0].vy = 0 - py / SOLAR_MASS
  b[0].vz = 0 - pz / SOLAR_MASS
  return b
}
fn advance(b, dt) {
  var n = len(b)
  for i in 0..n {
    var bi = b[i]
    var j = i + 1
    while j < n {
      var bj = b[j]
      var dx = bi.x - bj.x
      var dy = bi.y - bj.y
      var dz = bi.z - bj.z
      var d2 = dx * dx + dy * dy + dz * dz
      var mag = dt / (d2 * sqrt(d2))
      bi.vx -= dx * bj.mass * mag
      bi.vy -= dy * bj.mass * mag
      bi.vz -= dz * bj.mass * mag
      bj.vx += dx * bi.mass * mag
      bj.vy += dy * bi.mass * mag
      bj.vz += dz * bi.mass * mag
      j += 1
    }
  }
  for o in b { o.x += dt * o.vx o.y += dt * o.vy o.z += dt * o.vz }
}
fn energy(b) {
  var e = 0.0
  var n = len(b)
  for i in 0..n {
    var bi = b[i]
    e += 0.5 * bi.mass * (bi.vx * bi.vx + bi.vy * bi.vy + bi.vz * bi.vz)
    var j = i + 1
    while j < n {
      var bj = b[j]
      var dx = bi.x - bj.x
      var dy = bi.y - bj.y
      var dz = bi.z - bj.z
      e -= bi.mass * bj.mass / sqrt(dx * dx + dy * dy + dz * dz)
      j += 1
    }
  }
  return e
}
fn kernel() {
  var b = system()
  for s in 0..200000 { advance(b, 0.01) }
  return energy(b)
}
