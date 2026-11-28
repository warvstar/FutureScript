
test "hidden linkUrls and summarize" {
  expect linkUrls(parse(sample)) == ["https://mdlite.dev/guide", "https://mdlite.dev", "http://mdlite.org"]
  expect linkUrls(parse("[a](u1) *[b](u2)* [c](u1)\n- **[d](u3)**\n- [e](u2)")) == ["u1", "u2", "u3"]
  expect linkUrls(parse("none")) == []
  var s = summarize(parse(sample))
  expect s.title == "Getting started" && s.words == 23 && s.headings == 3 && s.links == 3 && s.minutes == 1
}
