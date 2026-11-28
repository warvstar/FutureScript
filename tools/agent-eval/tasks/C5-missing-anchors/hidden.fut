
test "hidden missingAnchors" {
  var d = parse("# Setup\n## Setup\n## Use it\n\n[a](#setup) [b](#setup-1) and [c](#setup-2)\n- [d](#use-it) [e](#nope) *[f](#nope)* [g](https://x.io)")
  expect missingAnchors(d) == ["#setup-2", "#nope"]
  expect missingAnchors(parse(sample)) == []
  expect missingAnchors(parse("# A\n\n**[x](#b)**")) == ["#b"]
}
