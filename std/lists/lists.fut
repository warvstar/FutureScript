// Lists: sorting, searching and the usual higher-order helpers. None changes the list it is given.
// Functions that take a function f declare no effects, so f may do whatever their caller may.

// A sorted copy of l, smallest first, by <. Stable.
pub fn sort(l: list): list !
  requires sortable(l)
  ensures result.len() == l.len()
{
  return mergeSort(l, nil)
}

// Can sort() take l: are its items all numbers, or all strings?
pub fn sortable(l: list): bool ! {
  if l.len() == 0 { return true }
  var t = kind(l[0])
  for x in l { if kind(x) != t || t == "" { return false } }
  return true
}

// A copy of l sorted by key(item), smallest first. Stable.
pub fn sortBy(l: list, key: fn): list {
  return mergeSort(l, key)
}

// [f(x) for each x in l].
pub fn map(l: list, f: fn): list {
  var out = []
  for x in l { out.push(f(x)) }
  return out
}

// The items x of l for which f(x) is truthy, in order.
pub fn filter(l: list, f: fn): list {
  var out = []
  for x in l { if f(x) { out.push(x) } }
  return out
}

// f(...f(f(start, l[0]), l[1])..., l[n-1]).
pub fn reduce(l: list, start: any, f: fn): any {
  var acc = start
  for x in l { acc = f(acc, x) }
  return acc
}

// The first x in l for which f(x) is truthy, or nil.
pub fn find(l: list, f: fn): any {
  for x in l { if f(x) { return x } }
  return nil
}

// Is f(x) truthy for some x in l?
pub fn any(l: list, f: fn): bool {
  for x in l { if f(x) { return true } }
  return false
}

// Is f(x) truthy for every x in l?
pub fn all(l: list, f: fn): bool {
  for x in l { if !f(x) { return false } }
  return true
}

// How many x in l have f(x) truthy.
pub fn count(l: list, f: fn): num {
  var n = 0
  for x in l { if f(x) { n += 1 } }
  return n
}

// The first index of v in l (by ==), or -1.
pub fn indexOf(l: list, v: any): num !
  ensures result >= -1 && result < l.len()
{
  for i in 0..l.len() { if l[i] == v { return i } }
  return -1
}

// Is v in l (by ==)?
pub fn contains(l: list, v: any): bool ! {
  return indexOf(l, v) >= 0
}

// A reversed copy of l.
pub fn reverse(l: list): list !
  ensures result.len() == l.len()
{
  var out = []
  var i = l.len() - 1
  while i >= 0 { out.push(l[i])  i -= 1 }
  return out
}

// The items of l without repeats (by ==), first occurrences in order.
pub fn unique(l: list): list !
  ensures result.len() <= l.len()
{
  var seen = {}
  var out = []
  for x in l { if !seen.has(x) { seen[x] = true  out.push(x) } }
  return out
}

// The items of l's items, one level down: [[1, 2], [3]] -> [1, 2, 3].
pub fn flatten(l: list): list ! {
  var out = []
  for x in l { for y in x { out.push(y) } }
  return out
}

// [a, a + 1, ..., b - 1].
pub fn range(a: num, b: num): list !
  requires b - a < 1e7
{
  var out = []
  for i in a..b { out.push(i) }
  return out
}

// The sum of l's numbers (0 when empty).
pub fn sum(l: list): num ! {
  var s = 0
  for x in l { s += x }
  return s
}

// The smallest item of l by <, or nil when empty.
pub fn min(l: list): any ! {
  if l.len() == 0 { return nil }
  var m = l[0]
  for x in l { if x < m { m = x } }
  return m
}

// The largest item of l by <, or nil when empty.
pub fn max(l: list): any ! {
  if l.len() == 0 { return nil }
  var m = l[0]
  for x in l { if m < x { m = x } }
  return m
}

fn kind(x) {  // "num" (not NaN), "str", or "" for anything else
  if x == str(x) { return "str" }
  var num = false
  try { num = x - 0 == x } catch { }
  if num { return "num" }
  return ""
}
fn mergeSort(l, key) {
  var n = l.len()
  var items = []
  for x in l { items.push(x) }
  var keys = items
  if key != nil { keys = [] for x in items { keys.push(key(x)) } }
  var idx = []
  for i in 0..n { idx.push(i) }
  var tmp = []
  for i in 0..n { tmp.push(0) }
  var width = 1
  while width < n {
    var lo = 0
    while lo < n {
      var mid = lo + width
      if mid > n { mid = n }
      var hi = lo + 2 * width
      if hi > n { hi = n }
      var i = lo
      var j = mid
      var k = lo
      while k < hi {
        if i < mid && (j >= hi || !(keys[idx[j]] < keys[idx[i]])) { tmp[k] = idx[i]  i += 1 } else { tmp[k] = idx[j]  j += 1 }
        k += 1
      }
      lo = hi
    }
    var t = idx
    idx = tmp
    tmp = t
    width *= 2
  }
  var out = []
  for i in idx { out.push(items[i]) }
  return out
}
