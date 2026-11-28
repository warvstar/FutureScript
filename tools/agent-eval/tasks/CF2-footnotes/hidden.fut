test "hidden footnote lines classify" {
  expect classify("  [^Note_2-b]: text", 4).kind == "footnote"
  expect classify("[^1]:", 1).kind == "footnote"
  expect classify("[^a b]: text", 1).kind == "text"
  expect classify("[^]: text", 1).kind == "text"
  expect classify("[x]: text", 1).kind == "text"
}

test "hidden footnote references parse" {
  expect parseInline("a[^n1] **b[^x-y]** [^] [^a b] [^c", 1) == [Node.Text("a"), Node.FootnoteRef("n1"), Node.Text(" "), Node.Strong([Node.Text("b"), Node.FootnoteRef("x-y")]), Node.Text(" [^] [^a b] [^c")]
  expect parseInline("[link](u)[^1]", 1) == [Node.Link("link", "u"), Node.FootnoteRef("1")]
}

test "hidden footnote definitions parse" {
  var d = parse("# T\ntext[^b]\n[^b]:   Bee *note*  \nmore\n[^a]: Ay\n[^b]: again\n```\n[^c]: code\n```")
  expect d.nodes == [Node.Heading(1, "T", 1), Node.Para([Node.Text("text"), Node.FootnoteRef("b")]), Node.Para([Node.Text("more")]), Node.CodeBlock("", "[^c]: code")]
  expect d.notes.len() == 2
  expect isa(d.notes[0], Footnote)
  expect d.notes[0].label == "b"
  expect d.notes[0].parts == [Node.Text("Bee "), Node.Em([Node.Text("note")])]
  expect d.notes[1].label == "a"
  expect d.notes[1].parts == [Node.Text("Ay")]
  expect parse("no notes").notes == []
  expect Doc([]).notes == []
  var f = Footnote("k", [Node.Text("v")])
  expect f.label == "k" && f.parts.len() == 1
}

test "hidden footnotes render" {
  var html = renderDoc(parse("Uses[^two] and[^one] and[^none].\n\n[^one]: First & *best*\n- item[^one]\n[^two]: See [x](https://x.io)[^one]"))
  expect html == "<p>Uses<sup><a href=\"#fn-2\">2</a></sup> and<sup><a href=\"#fn-1\">1</a></sup> and[^none].</p>\n<ul>\n<li>item<sup><a href=\"#fn-1\">1</a></sup></li>\n</ul>\n<ol class=\"footnotes\">\n<li id=\"fn-1\">First &amp; <em>best</em></li>\n<li id=\"fn-2\">See <a href=\"https://x.io\">x</a><sup><a href=\"#fn-1\">1</a></sup></li>\n</ol>"
  expect renderDoc(parse("[^z]: only")) == "<ol class=\"footnotes\">\n<li id=\"fn-1\">only</li>\n</ol>"
  expect renderDoc(parse("a[^q]")) == "<p>a[^q]</p>"
  expect renderInline(parseInline("x[^1] *y*", 1)) == "x[^1] <em>y</em>"
}

test "hidden footnote links checked" {
  var d = parse("[a](http://a.io)[^n]\n[^m]: **[b]()**\n[^n]: [c](http://c.io) [d](https://d.io)\n- [e](my e)")
  expect checkLinks(d) == ["'a': insecure http://a.io", "'e': space in my e", "'b': empty url", "'c': insecure http://c.io"]
}
