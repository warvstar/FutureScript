// A module's main is not run: only the program's own main is.
import "../../mods/geom.fut" as geom
fn main() { print(geom.square(3)) }
