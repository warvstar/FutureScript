// mdlite: a small Markdown subset to HTML, with stats.
import "lexer.fut"
import "parser.fut"
import "render.fut"
import "stats.fut"

var sample = "# Getting started

Install **mdlite** with `fut get mdlite`, then read the [guide](https://mdlite.dev/guide).

## Usage

1. Write a file
2. Run `mdlite file.md`

```sh
mdlite notes.md > notes.html
```

## Links

- [home](https://mdlite.dev)
- [old site](http://mdlite.org)
"

// A page: the document's title, then its HTML.
fn page(src: str): str {
  var doc = parse(src)
  var title = doc.title == "" ? "Untitled" : doc.title
  return "<title>" + escape(title) + "</title>\n" + renderDoc(doc)
}

test "sample parses into blocks" {
  var d = parse(sample)
  expect d.title == "Getting started"
  expect d.headings().len() == 3
  expect d.nodes.len() == 9
}

test "sample renders" {
  var html = renderDoc(parse(sample))
  expect html.has("<h2>Usage</h2>")
  expect html.has("<ol>\n<li>Write a file</li>\n<li>Run <code>mdlite file.md</code></li>\n</ol>")
  expect html.has("<pre><code class=\"language-sh\">mdlite notes.md &gt; notes.html</code></pre>")
}

test "sample stats" {
  var d = parse(sample)
  expect wordCount(d) == 23
  expect checkLinks(d) == ["'old site': insecure http://mdlite.org"]
}

test "page" {
  expect page("# A & B\ntext") == "<title>A &amp; B</title>\n<h1>A &amp; B</h1>\n<p>text</p>"
  expect page("just text").has("<title>Untitled</title>")
}
