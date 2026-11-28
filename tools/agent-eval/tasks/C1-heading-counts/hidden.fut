
test "hidden headingCounts and linkDensity" {
  expect headingCounts(parse(sample)) == [1, 2, 0, 0, 0, 0]
  expect headingCounts(parse("###### a\n###### b\n# c\ntext")) == [1, 0, 0, 0, 0, 2]
  expect headingCounts(parse("no headings")) == [0, 0, 0, 0, 0, 0]
  expect linkDensity(parse("[a](x) b c d")) == 25
  expect linkDensity(parse("```\ncode\n```")) == 0
}
