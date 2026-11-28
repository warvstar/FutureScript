// Word counts, reading time and link checks.
import "render.fut"
import "std/strings"
import "std/math"

// Every Link node in parts, nested ones too, in order.
fn collectLinks(parts: list, out: list) {
  for p in parts {
    match p {
      Link(_, _) { out.push(p) }
      Em(inner) { collectLinks(inner, out) }
      Strong(inner) { collectLinks(inner, out) }
      else { }
    }
  }
}

// The Link nodes of a document, in order.
fn linksOf(doc: Doc): list {
  var out = []
  for n in doc.nodes {
    match n {
      Para(parts) { collectLinks(parts, out) }
      ListItem(_, _, parts) { collectLinks(parts, out) }
      else { }
    }
  }
  return out
}

// The prose of a block: a heading's text, a paragraph's or list item's plain text. Code and rules have none.
fn blockText(n: Node): str {
  match n {
    Heading(_, text, _) { return text }
    Para(parts) { return plain(parts) }
    ListItem(_, _, parts) { return plain(parts) }
    else { return "" }
  }
}

pub fn countWords(s: str): num ! {
  return strings.words(s).len()
}

// Words of prose in a document; code blocks don't count.
pub fn wordCount(doc: Doc): num {
  var n = 0
  for b in doc.nodes { n += countWords(blockText(b)) }
  return n
}

// Whole minutes to read words at wpm words a minute, rounded up: 0 words take 0 minutes.
pub fn readingMinutes(words: num, wpm: num): num ! {
  if words == 0 { return 0 }
  return math.ceil(words / wpm)
}

// A message for each link that looks wrong: an empty URL, a URL with a space, or plain http.
pub fn checkLinks(doc: Doc): list {
  var problems = []
  for l in linksOf(doc) {
    if l.url == "" { problems.push("'{l.text}': empty url") }
    else if l.url.has(" ") { problems.push("'{l.text}': space in {l.url}") }
    else if strings.startsWith(l.url, "http://") { problems.push("'{l.text}': insecure {l.url}") }
  }
  return problems
}

// Links per 100 words of prose; 0 when there are no words.
pub fn linkDensity(doc: Doc): num ! {
  var words = wordCount(doc)
  if words == 0 { return 0 }
  var links = linksOf(doc).len()
  return links * 100 / words
}

pub class Summary {
  var title: str
  var words: num = 0
  var headings: num = 0
  var links: num = 0
  var minutes: num = 0
  fn init(title: str) { self.title = title }
}

// The numbers for a document's info panel, at 200 words a minute.
pub fn summarize(doc: Doc): Summary {
  var s = Summary(doc.title)
  s.words = wordCount(doc)
  s.headings = doc.headings().len()
  s.links = linksOf(doc).len()
  s.minutes = readingMinutes(s.words, 200)
  return s
}

// Parse src and count its words, timing the work: {words, ms}.
pub fn profile(src: str): map ! clock {
  var start = clock()
  var words = wordCount(parse(src))
  return {words: words, ms: (clock() - start) * 1000}
}

test "countWords" {
  expect countWords("  one two\tthree\n") == 3
  expect countWords("") == 0
}

test "wordCount skips code" {
  expect wordCount(parse("# Two words\n\nthree *four five*\n- [six](x)\n```\nnot these\n```")) == 6
}

test "checkLinks" {
  var d = parse("[a]() and [b](http://x.io) and *[c](https://ok.io)*\n- [d](my file.md)")
  expect checkLinks(d) == ["'a': empty url", "'b': insecure http://x.io", "'d': space in my file.md"]
}

test "profile times with the clock" {
  var now = {t: 1}
  handle {
    var p = profile("one two")
    expect p.words == 2 && p.ms == 250
  } with clock() { now.t += 0.25 return now.t }
}
