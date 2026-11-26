// A shader written in fut: `gpu fn` makes a function run on the GPU too. sea() colors each pixel of an evening sea,
// with a sun that follows the mouse; the same function runs on the CPU, which the test uses to check the picture.
// `python3 build.py gpu` once, then `fut examples/gpu_shader.fut`.
import "std/gpu"
import "std/math" as math

gpu fn mixc(a: vec3f, b: vec3f, t: num): vec3f { return a + (b - a) * math.clamp(t, 0, 1) }

// uv: (0, 0) top left to (1, 1) bottom right; sun: where the sun is, in uv; t: seconds
gpu fn sea(uv: vec2f, sun: vec2f, t: num): vec4f {
  var horizon = 0.6
  var c = mixc(vec3f(0.1, 0.1, 0.3), vec3f(1, 0.5, 0.3), uv.y / horizon)  // sky: dark above, warm near the horizon
  if uv.y > horizon {                                                      // sea: waves, darker further down
    var depth = (uv.y - horizon) / (1 - horizon)
    var wave = math.sin(uv.x * 40 + t * 2 + depth * 30) * 0.5 + 0.5
    c = mixc(vec3f(0.6, 0.3, 0.3), vec3f(0.02, 0.05, 0.15), depth) + vec3f(0.05, 0.05, 0.08) * wave
    var glint = 1 - math.abs(uv.x - sun.x) * 8                              // the sun's path on the water
    if glint > 0 { c = c + vec3f(1, 0.7, 0.4) * glint * wave * 0.5 }
  }
  var d = ((uv - sun) * vec2f(4 / 3, 1)).length()                          // (uv is stretched: the window is 4:3)
  if d < 0.06 && uv.y < horizon { c = vec3f(1, 0.9, 0.6) }                // the sun, above the water
  return vec4f(c.x, c.y, c.z, 1)
}

fn main() {
  window(800, 600)
  var g = gpu.open(nil)
  var start = clock()
  while present() {
    var sun = vec2f(mousex() / 800, math.min(mousey() / 600, 0.55))
    g.frame(0).paint(sea, [sun, clock() - start])
  }
}

test "the GPU paints what sea() says, pixel by pixel" {
  var g = gpu.open({headless: true, width: 64, height: 64})
  var sun = vec2f(0.5, 0.3)
  g.frame(0).paint(sea, [sun, 1])
  var px = g.pixels()
  for p in [[5, 5], [32, 19], [10, 50], [32, 60], [60, 45]] {
    var x = p[0]
    var y = p[1]
    var want = sea(vec2f((x + 0.5) / 64, (y + 0.5) / 64), sun, 1) * 255
    var i = (y * 64 + x) * 4
    for k in 0..3 { expect math.abs(px[i + k] - math.clamp(want[k], 0, 255)) <= 2 }
  }
}
