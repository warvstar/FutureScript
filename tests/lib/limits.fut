// Imported plainly by privacy.fut: `pub` names become global, the rest stay private to this file.
fn clamp(x, lo, hi) { if x < lo { return lo } if x > hi { return hi } return x }
var calls = 0
pub fn limit(x) { calls += 1 return clamp(x, 0, 10) }
pub fn limitCalls() { return calls }
