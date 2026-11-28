
test "hidden tocMarkdown" {
  expect tocMarkdown(parse(sample)) == "- [Getting started](#getting-started)\n  - [Usage](#usage)\n  - [Links](#links)"
  expect tocMarkdown(parse("### Deep One\n# Top")) == "    - [Deep One](#deep-one)\n- [Top](#top)"
  expect tocMarkdown(parse("text only")) == ""
}
