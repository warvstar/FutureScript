# fwebbrowser: a small web browser engine, written in fut

Build the core of a browser as a fut library, with thorough tests: an HTML parser, CSS selectors, a stylesheet cascade, block layout, and a text-mode renderer. There's no networking, no JavaScript and no pixels. It takes a page as a string and gives back a tree, styles, boxes and lines of text. The rules below simplify real browsers on purpose. Where they differ, follow these rules. Whitespace means space, tab, newline, carriage return and form feed.

## Scope (roughly in order)
1. Text helpers the rest needs (fut has few built in): character classes, ASCII upper and lower case, whitespace splitting and collapsing, number parsing via parseNum.
2. HTML tokenizer and tree builder: `parseHtml`, `serialize`, `textContent`.
3. Selector parser and matcher: `querySelectorAll`, `querySelector`, and specificity.
4. CSS parser and cascade: `computedStyle`.
5. Block layout: `layout`.
6. Text rendering: `renderText`.
7. Optional, once those work: a pretty-printer for trees and boxes, and a small CLI (`fut src/main.fut page.html`) that prints `renderText` at 80 columns.

## Public API (fixed: hidden tests are written against it)
`src/browser.fut` exports these functions, through `pub import` or directly:
```
pub fn parseHtml(html: str): map
pub fn serialize(node: map): str
pub fn textContent(node: map): str
pub fn querySelectorAll(root: map, selector: str): list
pub fn querySelector(root: map, selector: str)          // the first match, or nil
pub fn computedStyle(html: str, selector: str, prop: str) // str, or nil
pub fn layout(html: str, width: num): list
pub fn renderText(html: str, cols: num): list
```

### Nodes
- Document: `{type: "document", children: [...]}`.
- Element: `{type: "element", tag: "div", attrs: {name: value}, children: [...]}`. Attributes are kept in source order.
- Text: `{type: "text", text: "..."}`.

Other fields (such as a parent link) may be added. Tests read only these.

### parseHtml
- **Tags.**
  - A start tag is `<` followed by an ASCII letter. An end tag is `</` followed by an ASCII letter. Any other `<` is text.
  - Tag and attribute names are lowercased. The name runs until whitespace, `/` or `>`.
  - A tag left unterminated at the end of the input is dropped, along with everything after it.
- **Attributes.**
  - A value may be in double quotes, in single quotes, unquoted (running until whitespace or `>`, so it may contain `/`), or absent (the value is `""`).
  - When a name repeats, the first value wins.
- **Comments and declarations.** `<!-- ... -->` comments, `<!DOCTYPE ...>`, other `<!...>` and `<?...>` are dropped. Text on both sides of a dropped comment joins into one text node.
- **Entities.** Only `&amp; &lt; &gt; &quot; &#39;` are decoded, in text and attribute values, in a single pass, so `&amp;lt;` gives `&lt;`. Any other `&...` stays as written.
- **Whitespace.** All text is kept as written, whitespace-only text included: `"<ul>\n <li>"` gives the `ul` a text child `"\n "`. Adjacent text nodes never occur.
- **Void elements.** `area base br col embed hr img input link meta source track wbr` never have children or end tags.
- **Self-closing.** `/>` closes any element at once, void or not, so `<span/>` is an empty span.
- **Raw text.** The content of `script` and `style` is one raw text node, with no entity decoding, up to its end tag (`</script` or `</style` in any case, followed by whitespace, `/` or `>`). If the end tag never comes, it runs to the end of the input.
- **Pre.** A newline right after `<pre>` is dropped.
- **Implicit end tags,** applied when a start tag arrives:
  - If the innermost open element is a `p`, it's closed by any of `address article aside blockquote div dl fieldset footer form h1 h2 h3 h4 h5 h6 header hr main nav ol p pre section table ul`.
  - `li` closes the nearest open `li`, unless a `ul` or `ol` is open inside that `li`.
  - `tr` closes the nearest open `tr`, unless a `table`, `tbody`, `thead` or `tfoot` is open inside it.
  - `td` and `th` close the nearest open `td` or `th`, unless a `tr` or `table` is open inside it.

  Closing an element closes everything open inside it.
- **End tags.** An end tag closes the nearest open element with its name and everything inside that element. If no element with that name is open, the end tag is ignored. Elements still open at the end of the input are closed.
- **No implied elements.** No `html`, `head` or `body` is added. The document's children are exactly what the input has.

### serialize and textContent
- `serialize` writes HTML:
  - an element is `<tag a="v">children</tag>`;
  - a void element is `<tag a="v">`;
  - attributes come in source order, every value is double-quoted, and `& < > "` are escaped as `&amp; &lt; &gt; &quot;`;
  - in text, `& < >` are escaped, except inside `script` and `style`, which are written raw;
  - a document is its children, concatenated.
- `textContent` concatenates every descendant text node in document order, `script` and `style` text included.

### Selectors
- **Grammar.**
  - A selector list separates complex selectors with `,`.
  - A complex selector joins compound selectors with combinators: descendant (whitespace), child `>`, next sibling `+`, and later sibling `~`. Whitespace is allowed around combinators and commas.
  - A compound selector is an optional type or `*`, followed by any number of the following, with at least one part in total: `#id`, `.class`, `[attr]`, `[attr=v]`, `[attr~=v]`, `[attr^=v]`, `[attr$=v]`, `[attr*=v]`, `:first-child`, `:last-child`, `:not(compound)`.
  - Names (types, ids, classes, attributes and unquoted values) are runs of `A-Z a-z 0-9 _ -`. Values may also be quoted with `"` or `'`.
- **Matching.**
  - Types and attribute names are case-insensitive. Ids, classes and values are case-sensitive.
  - A class matches any whitespace-separated word of the `class` attribute, and `~=` matches the same way.
  - `^=`, `$=` and `*=` mean starts with, ends with and contains. An empty value matches nothing.
  - `:first-child` and `:last-child` count only element siblings, and the document counts as a parent.
- **Invalid selectors.** Anything else is invalid, including `""`, `div[`, `a >`, `a,,b` and any other pseudo-class. `querySelectorAll` and `querySelector` throw on an invalid selector.
- **querySelectorAll** returns the root's element descendants that match, in document order, without duplicates. The root itself is never included, but ancestors above the root count for combinators, as in the DOM: under a `p`, `"p span"` finds its spans.
- **Specificity** is (ids, classes + attributes + pseudo-classes, types). `*` counts nothing. `:not(x)` counts x.

### CSS and computedStyle
- **The element.** `computedStyle` parses the HTML and finds the first element `querySelector` would give, or returns nil if there is none. It returns that element's computed value of `prop`, which must be a longhand. Shorthands give nil.
- **Stylesheets.** These apply, in order: the default sheet below, then the text of every `style` element in document order (wherever it is in the tree), then each element's `style` attribute.
- **Parsing.**
  - `/* */` comments are removed.
  - An at-rule (`@...`) is skipped up to its `;` or to the end of its `{...}` block, nested braces included.
  - A rule is `selector list { declarations }`. If the selector list is invalid, the whole rule is dropped.
  - Declarations are separated by `;` and written `name: value`. A declaration with an empty name or value is skipped. Names are lowercased.
  - A value ending in `!important` (case-insensitive, whitespace allowed after `!`) is important.
  - Values are trimmed, and runs of whitespace become one space.
- **Shorthands** expand to longhands at parse time:
  - `margin`, `padding` and `border-width` take 1 to 4 values (top, right, bottom, left, in the usual CSS way) and set `margin-top` etc., `padding-top` etc. and `border-top-width` etc.
  - `border` sets all four `border-*-width` to its first token that is a length (`Npx`, `Nem` or `0`). With no length it is ignored.
- **The cascade.** For each property, the winning declaration is the highest by, in order:
  1. important over normal;
  2. author (style elements and attributes) over the default sheet;
  3. a `style` attribute over style elements;
  4. specificity (for a selector list, its most specific selector that matches the element);
  5. the later declaration.
- **No winner.** Without a winning declaration, an inherited property takes the parent element's computed value, and any other property takes its initial value.
  - The keyword `inherit` takes the parent's value (or the initial value for a top-level element), and `initial` takes the initial value.
  - A property with no initial value and no declaration is nil.
- **Inherited properties:** `color font-size font-weight font-style text-align text-transform visibility line-height`.
- **Initial values:**

  | Value | Properties |
  |---|---|
  | `inline` | `display` |
  | `black` | `color` |
  | `16px` | `font-size` |
  | `normal` | `font-weight`, `font-style`, `line-height` |
  | `left` | `text-align` |
  | `none` | `text-transform` |
  | `visible` | `visibility` |
  | `auto` | `width`, `height` |
  | `transparent` | `background-color` |
  | `0` | `margin-*`, `padding-*`, `border-*-width` |
- **Computed values.**
  - All values are lowercased.
  - `font-size`: `Npx` stays; `Nem` is N times the parent's font size, and `N%` is N/100 times it (16px at the top). Any other font-size is ignored, as if it weren't declared.
  - Lengths (`width height margin-* padding-* border-*-width`): `Npx` stays; `Nem` is N times the element's own font size; `0` is `0px`; anything else (`auto`, `50%`) stays as written.
  - `line-height`: `Npx` and `Nem` become px as above; a bare number (`1.5`) and `normal` stay.
  - Numbers print without a trailing `.0` (`24px`).
- **Default sheet:**
  ```
  html, body, div, p, h1, h2, h3, h4, h5, h6, ul, ol, pre, blockquote, section, article, header, footer, nav, main, form, hr { display: block }
  li { display: list-item }
  head, script, style, title, meta, link { display: none }
  h1 { font-size: 2em; font-weight: bold }
  h2 { font-size: 1.5em; font-weight: bold }
  h3, h4, h5, h6, b, strong, th { font-weight: bold }
  i, em { font-style: italic }
  ```

### Inline content (shared by layout and renderText)
- **Kinds of element.** An element whose computed `display` is `block` or `list-item` is block-level. One whose display is `none` is skipped with everything inside it. Everything else is inline.
- **Runs.** A block's children are processed in order. Consecutive text nodes and inline elements form a run, and each block-level child ends the current run.
- **Flattening a run.**
  - The run is flattened to text. Inline elements contribute their contents, including any block-level elements nested inside them, which are treated as inline.
  - `<br>` is a forced break. `<img>` contributes `[alt]` if it has a non-empty `alt`, and nothing otherwise.
  - Text is transformed by the `text-transform` of its parent element: `uppercase`, `lowercase` or `none`.
- **Lines.**
  - Between forced breaks, the text is split into words at whitespace runs, so whitespace collapses and words join across element boundaries (`un<b>believ</b>able` is one word).
  - Words fill lines greedily, joined by one space. A word that doesn't fit on an empty line gets a line of its own.
  - Lines with no words are dropped, so a run of only whitespace gives no lines, and neither does `<br><br>`.
- **Pre.** Inside a `pre` element, the run's text isn't wrapped. It's split at `\n` (a `<br>` counts as one), a final empty piece is dropped, and every other piece is a line, empty ones included.

### layout(html, width)
- **The result** is one box per block-level element that is laid out, in document order (a parent before its children): `{tag, x, y, width, height}`. The box is the border box, and its numbers are px. The document is the outermost container, at x 0, y 0, with the given width.
- **Blocks.**
  - Margins, borders and padding are px values. A `%` is of the containing block's content width, and `auto` counts as 0.
  - Content width is `width` if that's px (or `%` of the containing width). Otherwise it's the containing content width minus left and right margin, border and padding.
  - x is the container's content x plus margin-left.
  - The first child's y is the container's content top plus its own margin-top. A later child's y is the previous child's border-box bottom, plus that child's margin-bottom, plus its own margin-top. Margins never collapse.
  - Content height is `height` if px. Otherwise it reaches from the content top to the margin-box bottom of whatever comes last: a block child or a run's lines.
- **Lines.**
  - Each run adds (number of lines) x (line height). It uses the font size of the block that holds the run (16px at the document level): every character, space included, is font-size / 2 wide.
  - A line fits when its characters x that width is at most the content width.
  - Line height is `line-height` if px; a bare number times the font size; and `normal` is 1.25 x the font size, so 16px text has 8px characters on 20px lines.

### renderText(html, cols)
- **The result** is a list of lines, like a text-mode browser.
- **Wrapping.** Each run becomes lines as above, wrapped so a line fits in its available width (`cols` minus its indent).
- **Blocks.** A block always starts on a new line. There are no blank lines between blocks, and a block with nothing to show gives no lines.
- **List items.**
  - A `list-item` indents everything inside it by 2.
  - The first line it produces, its own or one from a nested block, has `* ` at the item's indent. With nesting, `<li><ul><li>b` gives `* * b`.
  - If the item produces no lines, it has no marker.
- **Alignment.** The holding block's `text-align` pads each line within the available width: `center` adds floor((available - length) / 2) spaces in front, `right` adds (available - length), and `left` adds nothing.
- **Pre.** Pre lines aren't aligned.
- **hr.** `<hr>` is a line of (cols - indent) `-` characters.

## Layout
- `src/*.fut` holds the modules. `src/browser.fut` is the entry and makes the public API visible.
- `tests/*_test.fut` hold the tests. Each imports what it tests with a relative path (`import "../src/html.fut"`).
- `all.fut` imports every module (`import "src/x.fut"`), so `fut map all.fut` shows the whole project. Keep it current.
