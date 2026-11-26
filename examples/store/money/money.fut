// Money as whole cents, so sums are exact. Each function's contract sits between its signature and its body; `fut
// test` also checks each one on random inputs, throwing away inputs that break a requires, so `requires` also tells
// the generator which inputs make sense.

// Dollars to cents, rounding half up.
pub fn cents(dollars: num): num
  ensures result == floor(result)
{
  return floor(dollars * 100 + 0.5)
}

// "$1234.56", "-$0.05".
pub fn format(c: num): str
  requires c == floor(c)
  ensures result.has("$")
  ensures result.has(".")
{
  var sign = ""
  if c < 0 {
    sign = "-"
    c = -c
  }
  var r = c % 100
  var pad = ""
  if r < 10 { pad = "0" }
  return sign + "$" + str((c - r) / 100) + "." + pad + str(r)
}

// p percent of c, rounded half up.
pub fn percent(c: num, p: num): num
  requires c >= 0
  requires c == floor(c)
  requires p >= 0
  requires p <= 100
  ensures result >= 0
  ensures result <= c
  ensures result == floor(result)
{
  return floor(c * p / 100 + 0.5)
}

// total split into `ways` parts that differ by at most a cent and add up to it exactly; the bigger parts first.
pub fn split(total: num, ways: num): list
  requires total >= 0
  requires total == floor(total)
  requires ways >= 1
  requires ways <= 10000000
  requires ways == floor(ways)
  ensures result.len() == ways
  ensures sum(result) == total
  ensures largest(result) - smallest(result) <= 1
{
  var base = floor(total / ways)
  var extra = total - base * ways
  var parts = []
  for i in 0..ways {
    if i < extra { parts.push(base + 1) } else { parts.push(base) }
  }
  return parts
}

// For the clauses above: private to the package, like any function without pub.
fn sum(l) { var s = 0 for x in l { s += x } return s }
fn largest(l) { var m = l[0] for x in l { if x > m { m = x } } return m }
fn smallest(l) { var m = l[0] for x in l { if x < m { m = x } } return m }
