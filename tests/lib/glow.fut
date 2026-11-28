// Imported by specs/gpu_fns.fut: a struct, a gpu var and gpu fns that another file's gpu fns use.
import "std/math" as math

pub type Light = {dir: vec3f, power: num}
pub gpu var LEVELS: u32 = 4

pub gpu fn lit(l: Light, n: vec3f): num { return math.max(n.dot(l.dir), 0) * l.power }
pub fn plain(x: num): num { return x }
