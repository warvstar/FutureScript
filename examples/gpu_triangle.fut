// A spinning triangle on the GPU (std/gpu, over Dawn). `python3 build.py gpu` once, then `fut -w examples/gpu_triangle.fut`:
// edit the shader or the colors while it runs.
import "std/gpu"

var SHADER = "
struct Frame \{ angle: f32 }
@group(0) @binding(0) var<uniform> frame: Frame;
struct Out \{ @builtin(position) pos: vec4f, @location(0) color: vec3f }

@vertex fn vs(@location(0) pos: vec2f, @location(1) color: vec3f) -> Out \{
  let c = cos(frame.angle);
  let s = sin(frame.angle);
  return Out(vec4f(c * pos.x - s * pos.y, s * pos.x + c * pos.y, 0.0, 1.0), color);
}
@fragment fn fs(in: Out) -> @location(0) vec4f \{ return vec4f(in.color, 1.0); }"

var Vertex = gpu.layout({pos: "vec2f", color: "vec3f"})
var Frame = gpu.layout({angle: "f32"})

fn main() {
  window(640, 480)
  var g = gpu.open(nil)
  var verts = g.vertices(Vertex.pack([
    {pos: [0, 0.6], color: [1, 0.3, 0.2]},
    {pos: [-0.5, -0.4], color: [0.2, 1, 0.3]},
    {pos: [0.5, -0.4], color: [0.2, 0.4, 1]}
  ], false))
  var frame = g.uniform(Frame)
  var tri = g.render({shader: SHADER, layout: Vertex, bind: [frame]})
  var t = 0
  while present() {
    t += 1 / 60
    frame.set({angle: t})
    g.frame(0x101020).draw(tri, verts, 3)
  }
}
