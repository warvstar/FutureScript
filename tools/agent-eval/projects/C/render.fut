// HTML output, plain text of inline parts, and the table of contents.
import "parser.fut"
import "std/strings"

// s with & < > " written as HTML entities.
pub fn escape(s: str): str ! {
  var out = strings.replace(s, "&", "&amp;")
  out = strings.replace(out, "<", "&lt;")
  out = strings.replace(out, ">", "&gt;")
  return strings.replace(out, "\"", "&quot;")
}

// The text a reader sees in inline parts: markup dropped, a link's text kept, its URL dropped.
pub fn plain(parts: list): str {
  var s = ""
  for p in parts {
    match p {
      Text(t) { s += t }
      Em(inner) { s += plain(inner) }
      Strong(inner) { s += plain(inner) }
      Code(t) { s += t }
      Link(text, _) { s += text }
      else { }
    }
  }
  return s
}

pub fn renderInline(parts: list): str {
  var s = ""
  for p in parts {
    match p {
      Text(t) { s += escape(t) }
      Em(inner) { s += "<em>" + renderInline(inner) + "</em>" }
      Strong(inner) { s += "<strong>" + renderInline(inner) + "</strong>" }
      Code(t) { s += "<code>" + escape(t) + "</code>" }
      Link(text, url) { s += "<a href=\"" + escape(url) + "\">" + escape(text) + "</a>" }
      else { }
    }
  }
  return s
}

// The HTML for a document, one block per line. Consecutive list items share one <ul> or <ol>.
pub fn renderDoc(doc: Doc): str {
  var out = []
  var openList = ""
  for n in doc.nodes {
    var tag = ""
    match n { ListItem(_, ordered, _) { tag = ordered ? "ol" : "ul" } else { } }
    if openList != "" && tag != openList { out.push("</{openList}>") openList = "" }
    if tag != "" && openList == "" { out.push("<{tag}>") openList = tag }
    match n {
      Heading(level, text, _) { out.push("<h{level}>" + escape(text) + "</h{level}>") }
      Para(parts) { out.push("<p>" + renderInline(parts) + "</p>") }
      ListItem(_, _, parts) { out.push("<li>" + renderInline(parts) + "</li>") }
      CodeBlock(lang, text) {
        var cls = lang == "" ? "" : " class=\"language-{lang}\""
        out.push("<pre><code{cls}>" + escape(text) + "</code></pre>")
      }
      Rule { out.push("<hr>") }
      else { }
    }
  }
  if openList != "" { out.push("</{openList}>") }
  return out.join("\n")
}

// A heading's anchor: lower case, letters and digits kept, each run of spaces and hyphens made one "-",
// none at either end; everything else dropped. "Hello, World!" -> "hello-world".
pub fn slug(text: str): str ! {
  var s = ""
  var dash = false
  for c in strings.lower(text) {
    if (c >= "a" && c <= "z") || (c >= "0" && c <= "9") {
      if dash && s != "" { s += "-" }
      s += c
      dash = false
    } else if c == " " || c == "-" {
      dash = true
    }
  }
  return s
}

// base the first time; then base-1, base-2, ... for each repeat. seen remembers how often each base came up.
pub fn uniqueAnchor(base: str, seen: map): str {
  if !seen.has(base) { seen[base] = 0 return base }
  seen[base] += 1
  return "{base}-{seen[base]}"
}

pub class TocEntry {
  var text: str
  var level: num
  var anchor: str
  fn init(text: str, level: num, anchor: str) { self.text = text self.level = level self.anchor = anchor }
}

fn tocEntry(h: Node.Heading, seen: map): TocEntry {
  return TocEntry(h.text, h.level, uniqueAnchor(slug(h.text), seen))
}

// One entry per heading, in order, each with a distinct anchor.
pub fn toc(doc: Doc): list {
  var seen = {}
  var out = []
  for h in doc.headings() { out.push(tocEntry(h, seen)) }
  return out
}

test "escape" {
  expect escape("a < b && \"c\" > d") == "a &lt; b &amp;&amp; &quot;c&quot; &gt; d"
}

test "plain drops markup" {
  expect plain(parseInline("*a* **b** `c` [d](e)", 1)) == "a b c d"
}

test "renderInline" {
  expect renderInline(parseInline("x *y* [z](/q?a=1&b=2)", 1)) == "x <em>y</em> <a href=\"/q?a=1&amp;b=2\">z</a>"
}

test "renderDoc groups list items" {
  var html = renderDoc(parse("# T\n- a\n- b\n1. c\n\npara\n```\n<x>\n```"))
  expect html == "<h1>T</h1>\n<ul>\n<li>a</li>\n<li>b</li>\n</ul>\n<ol>\n<li>c</li>\n</ol>\n<p>para</p>\n<pre><code>&lt;x&gt;</code></pre>"
}

test "slug" {
  expect slug("Hello, World!") == "hello-world"
  expect slug("  Step 2 -- Install  ") == "step-2-install"
  expect slug("???") == ""
}
