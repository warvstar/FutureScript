
test "hidden longestParagraph and readingMinutes" {
  expect longestParagraph(parse("one two\n\n*three* four [five](x)\n\n- a b c d e f g\n\nsix seven eight")) == "three four five"
  expect longestParagraph(parse("# A long heading with many words\nshort one")) == "short one"
  expect longestParagraph(parse("# Only a heading\n- item")) == ""
  expect readingMinutes(400, 200) == 2
  expect readingMinutes(401, 200) == 3
  expect readingMinutes(0, 200) == 0
}
