test "hidden quote lines classify" {
  var q = classify("   >  said *so*  ", 7)
  expect q.kind == "quote" && q.text == "said *so*" && q.lineNo == 7
  expect classify(">", 1).kind == "quote" && classify(">", 1).text == ""
  expect classify("> ## not a heading", 1).kind == "quote" && classify("> ## not a heading", 1).text == "## not a heading"
  expect classify("a > b", 1).kind == "text"
}

test "hidden quotes parse" {
  var d = parse("# T\nbefore\n> a **b**\n>c\n>\n>\n> [d](x)\nafter")
  expect d.nodes == [Node.Heading(1, "T", 1), Node.Para([Node.Text("before")]), Node.Quote([Node.Para([Node.Text("a "), Node.Strong([Node.Text("b")]), Node.Text(" c")]), Node.Para([Node.Link("d", "x")])]), Node.Para([Node.Text("after")])]
  expect parse(">\n>").nodes == [Node.Quote([])]
  expect parse("> one\n\n> two").nodes == [Node.Quote([Node.Para([Node.Text("one")])]), Node.Quote([Node.Para([Node.Text("two")])])]
  expect parse("> x\n- item\n> y").nodes == [Node.Quote([Node.Para([Node.Text("x")])]), Node.ListItem(0, false, [Node.Text("item")]), Node.Quote([Node.Para([Node.Text("y")])])]
  expect parse("```\n> code\n```").nodes == [Node.CodeBlock("", "> code")]
}

test "hidden quotes render" {
  expect renderDoc(parse("> 1 < 2\n>\n> *yes*")) == "<blockquote>\n<p>1 &lt; 2</p>\n<p><em>yes</em></p>\n</blockquote>"
  expect renderDoc(parse("1. a\n>\npara")) == "<ol>\n<li>a</li>\n</ol>\n<blockquote>\n</blockquote>\n<p>para</p>"
  expect page("> # Q\n# Real").has("<blockquote>\n<p># Q</p>\n</blockquote>\n<h1>Real</h1>")
}

test "hidden quotes in stats" {
  var d = parse("intro [a](https://a.io)\n> quoted words [b](http://b.io)\n>\n> **more** [c]()\n- [d](http://d.io)")
  expect wordCount(d) == 8
  expect checkLinks(d) == ["'b': insecure http://b.io", "'c': empty url", "'d': insecure http://d.io"]
  expect summarize(d).links == 4
}
