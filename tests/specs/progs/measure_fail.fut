// Run with `fut test` by measure.fut: each test fails.
fn labels(n) { var l = [] for i in 0..n { l.push("item {i}") } return l }
fn pairs(n) { var s = 0 for i in 0..n { for j in 0..n { s += i ^ j } } return s }
test "allocs" { expect allocs { labels(100) } <= 50 }
test "time" { expect grows { n -> pairs(n) } like "n" }
test "allocations" { expect grows allocs { n -> labels(n) } like "1" }
