// Text, on top of the str methods (len has find slice split trim) and list.join. Indexes count bytes.

var uppers = {}
var lowers = {}
var az = "abcdefghijklmnopqrstuvwxyz"
var AZ = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
for i in 0..26 { uppers[az[i]] = AZ[i]  lowers[AZ[i]] = az[i] }

// s with every occurrence of part replaced by with.
pub fn replace(s: str, part: str, with: str): str !
  requires part.len() > 0
{
  return s.split(part).join(with)
}

// Does s begin with part?
pub fn startsWith(s: str, part: str): bool !
  ensures !result || s.len() >= part.len()
{
  return s.slice(0, part.len()) == part
}

// Does s end with part?
pub fn endsWith(s: str, part: str): bool !
  ensures !result || s.len() >= part.len()
{
  return part.len() <= s.len() && s.slice(s.len() - part.len(), s.len()) == part
}

// How many times part occurs in s, not overlapping.
pub fn count(s: str, part: str): num !
  requires part.len() > 0
  ensures result >= 0
{
  return s.split(part).len() - 1
}

// s, n times over.
pub fn repeat(s: str, n: num): str !
  requires n >= 0
  ensures result.len() == s.len() * int(n)
{
  var parts = []
  for i in 0..int(n) { parts.push(s) }
  return parts.join("")
}

// s padded on the left with fill (one character) to at least n bytes.
pub fn padStart(s: str, n: num, fill: str): str !
  requires fill.len() == 1
  ensures result.len() >= s.len()
{
  return repeat(fill, max0(n - s.len())) + s
}

// s padded on the right with fill (one character) to at least n bytes.
pub fn padEnd(s: str, n: num, fill: str): str !
  requires fill.len() == 1
  ensures result.len() >= s.len()
{
  return s + repeat(fill, max0(n - s.len()))
}

// s with a-z made A-Z; other bytes unchanged.
pub fn upper(s: str): str !
  ensures result.len() == s.len()
{
  return mapChars(s, uppers)
}

// s with A-Z made a-z; other bytes unchanged.
pub fn lower(s: str): str !
  ensures result.len() == s.len()
{
  return mapChars(s, lowers)
}

// The lines of s, without their "\n" (or "\r\n"). A final newline doesn't start another line.
pub fn lines(s: str): list ! {
  var l = s.split("\n")
  if l.len() > 0 && l[l.len() - 1] == "" { l.pop() }
  for i in 0..l.len() { if endsWith(l[i], "\r") { l[i] = l[i].slice(0, l[i].len() - 1) } }
  return l
}

// The words of s: its runs of characters other than spaces, tabs and newlines.
pub fn words(s: str): list ! {
  var out = []
  var start = -1
  for i in 0..s.len() + 1 {
    var space = i == s.len() || s[i] == " " || s[i] == "\t" || s[i] == "\n" || s[i] == "\r"
    if space && start >= 0 { out.push(s.slice(start, i))  start = -1 }
    if !space && start < 0 { start = i }
  }
  return out
}

// s backwards (by bytes).
pub fn reverse(s: str): str !
  ensures result.len() == s.len()
{
  var parts = []
  var i = s.len() - 1
  while i >= 0 { parts.push(s[i])  i -= 1 }
  return parts.join("")
}

// Is s a whole number: an optional "-" then one or more digits 0-9, and nothing else (no spaces, "+" or ".")?
pub fn isInt(s: str): bool !
  ensures !result || (s.len() > 0 && s[s.len() - 1] != "-")
{
  var start = 0
  if s.len() > 0 && s[0] == "-" { start = 1 }
  if s.len() == start { return false }
  for i in start..s.len() {
    var b = s.byte(i)
    if b < 48 || b > 57 { return false }   // "0".."9"
  }
  return true
}

// The whole number s is ("-12" -> -12), or nil when it isn't one (isInt(s) is false).
pub fn parseInt(s: str): num? !
  ensures (result == nil) == !isInt(s)
{
  if !isInt(s) { return nil }
  return parseNum(s)
}

fn max0(n) { if n > 0 { return int(n) } return 0 }
fn mapChars(s, table) {
  var parts = []
  for c in s { var t = table[c] if t == nil { parts.push(c) } else { parts.push(t) } }
  return parts.join("")
}
