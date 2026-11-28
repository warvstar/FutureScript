// Building a document tree from Markdown source.
import "lexer.fut"

// Block nodes (Heading .. Rule) make up a document; inline nodes (Text .. Link) make up a block's parts.
pub enum Node {
  Heading(level: num, text: str, line: num)
  Para(parts: list)
  ListItem(depth: num, ordered: bool, parts: list)
  CodeBlock(lang: str, text: str)
  Rule
  Text(s: str)
  Em(parts: list)
  Strong(parts: list)
  Code(text: str)
  Link(text: str, url: str)
}

pub class Doc {
  var nodes: list
  var title: str = ""
  fn init(nodes: list) {
    self.nodes = nodes
    for h in self.headings() { if h.level == 1 { self.title = h.text break } }
  }
  // The Heading nodes, in order.
  fn headings(): list {
    var out = []
    for n in self.nodes { match n { Heading(_, _, _) { out.push(n) } else { } } }
    return out
  }
}

// The index of the first token of kind in ts[from:to], or -1.
fn findKind(ts: list, kind: str, from: num, to: num): num {
  for i in from..to { if ts[i].kind == kind { return i } }
  return -1
}

// ts[from:to] as the source text they came from.
fn rawText(ts: list, from: num, to: num): str {
  var s = ""
  for i in from..to {
    var t = ts[i]
    s += t.kind == "code" ? "`" + t.text + "`" : t.text
  }
  return s
}

// Append text to out, joining it to a Text node already at the end.
fn addText(out: list, s: str) {
  if out.len() > 0 {
    match out[out.len() - 1] { Text(prev) { out[out.len() - 1] = Node.Text(prev + s) return } else { } }
  }
  out.push(Node.Text(s))
}

fn inlineRange(ts: list, from: num, to: num): list {
  var out = []
  var i = from
  while i < to {
    var t = ts[i]
    if t.kind == "code" { out.push(Node.Code(t.text)) i += 1 continue }
    if t.kind == "em" || t.kind == "strong" {
      var close = findKind(ts, t.kind, i + 1, to)
      if close > i + 1 {
        var inner = inlineRange(ts, i + 1, close)
        out.push(t.kind == "em" ? Node.Em(inner) : Node.Strong(inner))
        i = close + 1 continue
      }
    }
    if t.kind == "lbrack" {
      var rb = findKind(ts, "rbrack", i + 1, to)
      if rb > 0 && rb + 1 < to && ts[rb + 1].kind == "lparen" {
        var rp = findKind(ts, "rparen", rb + 2, to)
        if rp > 0 {
          out.push(Node.Link(rawText(ts, i + 1, rb), rawText(ts, rb + 2, rp).trim()))
          i = rp + 1 continue
        }
      }
    }
    addText(out, t.text)
    i += 1
  }
  return out
}

// The inline nodes of one piece of text. Unmatched markers stay as plain text.
pub fn parseInline(s: str, line: num): list {
  var ts = lexInline(s, line)
  return inlineRange(ts, 0, ts.len())
}

// A paragraph from consecutive text lines, joined with single spaces.
fn paragraph(lines: list): Node {
  var texts = []
  for l in lines { texts.push(l.text) }
  return Node.Para(parseInline(texts.join(" "), lines[0].lineNo))
}

// Parse a whole document.
pub fn parse(src: str): Doc {
  var lines = lexLines(src)
  var nodes = []
  var para = []
  var i = 0
  while i < lines.len() {
    var l = lines[i]
    i += 1
    if l.kind == "text" { para.push(l) continue }
    if para.len() > 0 { nodes.push(paragraph(para)) para = [] }
    if l.kind == "heading" {
      nodes.push(Node.Heading(l.level, l.text, l.lineNo))
    } else if l.kind == "bullet" || l.kind == "ordered" {
      nodes.push(Node.ListItem(l.level, l.kind == "ordered", parseInline(l.text, l.lineNo)))
    } else if l.kind == "rule" {
      nodes.push(Node.Rule)
    } else if l.kind == "fence" {
      var body = []
      while i < lines.len() && lines[i].kind != "fence" { body.push(lines[i].raw) i += 1 }
      i += 1
      nodes.push(Node.CodeBlock(l.text, body.join("\n")))
    }
  }
  if para.len() > 0 { nodes.push(paragraph(para)) }
  return Doc(nodes)
}

test "parseInline nests emphasis and finds links" {
  var parts = parseInline("see **the [docs](http://x.io/a)** or `fut`", 1)
  expect parts == [Node.Text("see "), Node.Strong([Node.Text("the "), Node.Link("docs", "http://x.io/a")]), Node.Text(" or "), Node.Code("fut")]
}

test "parseInline keeps unmatched markers as text" {
  expect parseInline("2 * 3 [not a link]", 1) == [Node.Text("2 * 3 [not a link]")]
  expect parseInline("**", 1) == [Node.Text("**")]
}

test "parse builds blocks" {
  var d = parse("# Title\n\nsome *text*\nmore\n\n- one\n  - two\n1. first\n\n```fut\nvar x = 1\n```\n---")
  expect d.title == "Title"
  expect d.nodes[0] == Node.Heading(1, "Title", 1)
  expect d.nodes[1] == Node.Para([Node.Text("some "), Node.Em([Node.Text("text")]), Node.Text(" more")])
  expect d.nodes[2:5] == [Node.ListItem(0, false, [Node.Text("one")]), Node.ListItem(1, false, [Node.Text("two")]), Node.ListItem(0, true, [Node.Text("first")])]
  expect d.nodes[5:] == [Node.CodeBlock("fut", "var x = 1"), Node.Rule]
  expect d.headings().len() == 1
}

test "parse keeps a document without a top heading untitled" {
  expect parse("## Only a section").title == ""
  expect parse("").nodes == []
}
