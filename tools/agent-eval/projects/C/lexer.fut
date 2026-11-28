// Splitting Markdown source into classified lines and inline tokens.
import "std/strings"
import "std/lists"

// A piece of inline text. kind is "text", "code", "em", "strong", "lbrack", "rbrack", "lparen" or "rparen".
// col counts bytes from 1.
pub class Token {
  var kind: str
  var text: str
  var line: num
  var col: num
  fn init(kind: str, text: str, line: num, col: num) { self.kind = kind self.text = text self.line = line self.col = col }
}

// One source line. kind is "heading", "bullet", "ordered", "fence", "rule", "blank" or "text".
// text is what follows the marker, trimmed; level is the heading level, or the list depth (two spaces of indent per step).
pub class Line {
  var kind: str
  var text: str
  var raw: str
  var level: num
  var lineNo: num
  fn init(kind: str, text: str, raw: str, lineNo: num, level: num) {
    self.kind = kind self.text = text self.raw = raw self.lineNo = lineNo self.level = level
  }
}

var punct = {"[": "lbrack", "]": "rbrack", "(": "lparen", ")": "rparen"}

// The inline tokens of one line of text. A backtick with no closing one is plain text.
pub fn lexInline(s: str, line: num): list {
  var out = []
  var buf = ""
  var start = 1
  var i = 0
  while i < s.len() {
    var c = s[i]
    var kind = nil
    var text = c
    var width = 1
    if punct.has(c) {
      kind = punct[c]
    } else if c == "*" {
      if i + 1 < s.len() && s[i + 1] == "*" { kind = "strong" text = "**" width = 2 } else { kind = "em" }
    } else if c == "`" {
      var close = s[i + 1:].find("`")
      if close >= 0 { kind = "code" text = s.slice(i + 1, i + 1 + close) width = close + 2 }
    }
    if kind == nil {
      if buf == "" { start = i + 1 }
      buf += c i += 1 continue
    }
    if buf != "" { out.push(Token("text", buf, line, start)) buf = "" }
    out.push(Token(kind, text, line, i + 1))
    i += width
  }
  if buf != "" { out.push(Token("text", buf, line, start)) }
  return out
}

// How many times ch repeats at the start of s.
fn leading(s: str, ch: str): num ! {
  var n = 0
  while n < s.len() && s[n] == ch { n += 1 }
  return n
}

// Classify one line of source.
pub fn classify(raw: str, lineNo: num): Line {
  var s = raw.trim()
  if s == "" { return Line("blank", "", raw, lineNo, 0) }
  if strings.startsWith(s, "```") { return Line("fence", s[3:].trim(), raw, lineNo, 0) }
  var hashes = leading(s, "#")
  if hashes >= 1 && hashes <= 6 && s.len() > hashes && s[hashes] == " " { return Line("heading", s[hashes:].trim(), raw, lineNo, hashes) }
  if s.len() >= 3 && (leading(s, "-") == s.len() || leading(s, "*") == s.len()) { return Line("rule", "", raw, lineNo, 0) }
  var depth = int(leading(raw, " ") / 2)
  if strings.startsWith(s, "- ") || strings.startsWith(s, "* ") { return Line("bullet", s[2:].trim(), raw, lineNo, depth) }
  var dot = s.find(". ")
  if dot > 0 && strings.isInt(s.slice(0, dot)) { return Line("ordered", s[dot + 2:].trim(), raw, lineNo, depth) }
  return Line("text", s, raw, lineNo, 0)
}

// Every line of src, classified, numbered from 1.
pub fn lexLines(src: str): list {
  var out = []
  var n = 1
  for raw in src.split("\n") { out.push(classify(raw, n)) n += 1 }
  return out
}

test "lexInline splits markup from text" {
  var ts = lexInline("a *b* `c d` [x](y)", 3)
  expect lists.map(ts, fn(t) { return t.kind }) == ["text", "em", "text", "em", "text", "code", "text", "lbrack", "text", "rbrack", "lparen", "text", "rparen"]
  expect ts[5].text == "c d" && ts[5].col == 7 && ts[5].line == 3
  expect lexInline("**x**", 1)[0].kind == "strong"
}

test "lexInline keeps a stray backtick" {
  expect lexInline("1 ` 3", 1)[0].text == "1 ` 3"
}

test "classify headings, lists and rules" {
  var h = classify("## Setup ", 1)
  expect h.kind == "heading" && h.level == 2 && h.text == "Setup"
  expect classify("#hashtag", 1).kind == "text" && classify("####### seven", 1).kind == "text"
  expect classify("---", 1).kind == "rule"
  var b = classify("    - deep", 4)
  expect b.kind == "bullet" && b.level == 2 && b.text == "deep" && b.lineNo == 4
  expect classify("12. twelve", 1).kind == "ordered"
  expect classify("```py", 1).text == "py"
  expect classify("   ", 1).kind == "blank"
}
