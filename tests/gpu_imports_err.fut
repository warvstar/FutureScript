// A gpu fn calls the gpu fns of a file it imports, reads its gpu vars and uses its types; not its plain fns.
import "lib/glow.fut" as glow
type Light = {x: num}
gpu fn a(x: num): num { return glow.plain(x) }        // error: glow.plain is not a gpu fn: a gpu fn calls only gpu fns, of its file and of files it imports
gpu fn b(l: Light, m: glow.Light): num { return l.x }  // error: two files' structs are named Light, and a shader holds one struct of a name: rename one
gpu fn c(x: num): num { var f = glow.plain return x }  // error: glow.plain is not a gpu var
