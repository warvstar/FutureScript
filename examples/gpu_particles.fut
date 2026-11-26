// 100,000 particles falling and bouncing, moved by a compute shader and drawn as points from the same buffer
// (std/gpu, over Dawn). `python3 build.py gpu` once, then `fut examples/gpu_particles.fut`.
import "std/gpu"

var Particle = gpu.layout({pos: "vec2f", vel: "vec2f"})
var N = 100000

var STEP = "
struct P \{ pos: vec2f, vel: vec2f }
@group(0) @binding(0) var<storage, read_write> ps: array<P>;
@compute @workgroup_size(64) fn main(@builtin(global_invocation_id) id: vec3u) \{
  if (id.x >= arrayLength(&ps)) \{ return; }
  var p = ps[id.x];
  let dt = 1.0 / 60.0;
  p.vel.y -= 2.0 * dt;
  p.pos += p.vel * dt;
  if (p.pos.y < -1.0) \{ p.pos.y = -1.0; p.vel.y = -p.vel.y * 0.8; }
  if (abs(p.pos.x) > 1.0) \{ p.vel.x = -p.vel.x; }
  ps[id.x] = p;
}"

var DOTS = "
struct Out \{ @builtin(position) pos: vec4f, @location(0) speed: f32 }
@vertex fn vs(@location(0) pos: vec2f, @location(1) vel: vec2f) -> Out \{
  return Out(vec4f(pos, 0.0, 1.0), length(vel));
}
@fragment fn fs(in: Out) -> @location(0) vec4f \{
  return vec4f(0.3 + in.speed * 0.4, 0.6, 1.0 - in.speed * 0.3, 1.0);
}"

// particles start in a band near the top, each with its own velocity
fn spawn(n) {
  var ps = f32s(n * Particle.size)
  for i in 0..n {
    var at = i * Particle.size
    ps[at] = random() * 2 - 1
    ps[at + 1] = random() * 0.5 + 0.4
    ps[at + 2] = random() - 0.5
    ps[at + 3] = random() * 0.5
  }
  return ps
}

fn main() {
  window(800, 600)
  var g = gpu.open(nil)
  var buf = g.storage(spawn(N))
  var step = g.compute({shader: STEP, bind: [buf]})
  var dots = g.render({shader: DOTS, layout: Particle, draw: "points"})
  while present() {
    g.run(step, N)
    g.frame(0x05050a).draw(dots, buf, N)
  }
}

test "particles stay above the floor" {
  var g = gpu.open({headless: true, width: 64, height: 64})
  var buf = g.storage(spawn(1024))
  var step = g.compute({shader: STEP, bind: [buf]})
  for i in 0..300 { g.run(step, 1024) }
  var ps = buf.read(nil)
  for i in 0..1024 { expect ps[i * 4 + 1] >= -1 }
}
