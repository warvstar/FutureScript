// A file imported plainly first and as a module after: both importers see its pub names.
import "../../mods/twice_plain.fut"
import "../../mods/twice_as.fut"
fn main() { print(viaAs() + viaPlain()) }
