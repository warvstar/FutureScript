// A file imported as a module first and plainly after: both importers see its pub names.
import "../../mods/twice_as.fut"
import "../../mods/twice_plain.fut"
fn main() { print(viaAs() + viaPlain()) }
