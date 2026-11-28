test "hidden front matter parses" {
  var d = parse("---\nTitle :  \"Release: 2.0\"  \nAuthor: Ann Lee\njust words\n: no key\ndate: 2026-11-01\nauthor: Bo\n  ---  \n# Heading\n\ntext")
  expect d.meta == {title: "Release: 2.0", author: "Bo", date: "2026-11-01"}
  expect d.meta.keys() == ["title", "author", "date"]
  expect d.title == "Release: 2.0"
  expect d.metaValue("author") == "Bo"
  expect d.metaValue("nope") == ""
  expect d.nodes == [Node.Heading(1, "Heading", 9), Node.Para([Node.Text("text")])]
  expect d.headings()[0].line == 9
}

test "hidden front matter edge cases" {
  var open = parse("---\ntitle: x\n# H")
  expect open.meta == {}
  expect open.title == "H"
  expect open.nodes == [Node.Rule, Node.Para([Node.Text("title: x")]), Node.Heading(1, "H", 3)]
  var empty = parse("---\n---\n## Sub")
  expect empty.meta == {}
  expect empty.title == ""
  expect empty.nodes == [Node.Heading(2, "Sub", 3)]
  var late = parse("text\n---\nk: v\n---")
  expect late.meta == {}
  expect late.nodes.len() == 4
  var q = parse("---\ntitle: \"\"\na: \"\nb: \"x\" y\n---\n# Top")
  expect q.title == "Top"
  expect q.metaValue("title") == ""
  expect q.metaValue("a") == "\""
  expect q.metaValue("b") == "\"x\" y"
  expect Doc([]).meta == {}
  expect parse("plain").meta == {}
}

test "hidden front matter in stats" {
  var d = parse("---\ntitle: Meta Title\nwords: these are not counted\n---\none two\n# Three")
  expect wordCount(d) == 3
  expect summarize(d).title == "Meta Title"
}

test "hidden renderHead and page" {
  var d = parse("---\nauthor: A & \"B\"\ntitle: <T>\nlang: en\n---\ntext")
  expect renderHead(d) == "<title>&lt;T&gt;</title>\n<meta name=\"author\" content=\"A &amp; &quot;B&quot;\">\n<meta name=\"lang\" content=\"en\">"
  expect renderHead(parse("## no title")) == "<title>Untitled</title>"
  expect renderHead(parse("---\nTag: x\n---\n# Head")) == "<title>Head</title>\n<meta name=\"tag\" content=\"x\">"
  expect page("---\ntitle: Notes\n---\n# Big\n- a") == "<title>Notes</title>\n<h1>Big</h1>\n<ul>\n<li>a</li>\n</ul>"
  expect page("# A & B\ntext") == "<title>A &amp; B</title>\n<h1>A &amp; B</h1>\n<p>text</p>"
  expect page("---\ndesc: d\n---\n") == "<title>Untitled</title>\n<meta name=\"desc\" content=\"d\">\n"
}
